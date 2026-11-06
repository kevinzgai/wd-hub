#include "apps/radio/app_radio.h"
#include "apps/radio/radio_ui.h"
#include "apps/radio/radio_player.h"
#include "apps/radio/radio_catalog.h"
#include "apps/home/home_profile.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>

static const char *TAG = "app_radio";

namespace {

std::atomic<bool> s_radio_active{false};
std::atomic<bool> s_wifi_connected{false};
std::atomic<bool> s_ui_alive{false};
std::atomic<bool> s_player_ready{false};
// Reserve before task creation so concurrent Wi-Fi/start paths own one worker.
std::atomic<bool> s_catalog_running{false};
std::atomic<bool> s_catalog_cancel{false};
// One successful city search per Radio session. Outdoor Wi-Fi drops and
// returns often; re-running the search on every reconnect stopped playback
// for several seconds each time.
std::atomic<bool> s_catalog_installed{false};
SemaphoreHandle_t s_catalog_commit = nullptr;

// ---- 城市电台列表：app 层只持有「当前城市 + 目录加载态 + 选中项」。
// RadioStation 本身已有 name / frequency_decihz，RadioPlaybackState 已有六态，
// 不需要给数据结构加城市或状态字段；城市是整份目录的上下文。
enum class RadioPage { Main, StationList, City };
constexpr std::size_t kStationCapacity = 8;
RadioPage s_radio_page = RadioPage::Main;
char s_current_city[48] = "自动定位";
char s_city_override[48] = {};
char s_list_detail[64] = {};
RadioListState s_list_state = RadioListState::Loading;
std::size_t s_list_selected;
std::size_t s_city_selected = 1;
RadioStation s_station_rows[kStationCapacity] = {};

// catalog_worker 与「城市电台列表」之间的三个边界函数，定义放在
// start_catalog() 之后。tests/test_radio_lifecycle.py 会按字符串切片把
// catalog_cancelled()..start_catalog() 抽出来单独编译，切片里只应出现
// 数据层已有的符号；列表页的状态变量不能渗进这段生产代码。
void radio_catalog_fill_preference(const home_profile_t &profile,
                                   RadioLocation *preferred);
void radio_catalog_apply_result(const char *city, std::size_t found);
void radio_catalog_refresh_list();

std::size_t station_total() {
    const std::size_t count = std::min(radio_player_station_count(), kStationCapacity);
    return (s_list_state == RadioListState::Ready ? count : 0) + 2;
}

void refresh_station_list() {
    const std::size_t count = std::min(radio_player_station_count(), kStationCapacity);
    for (std::size_t i = 0; i < count; ++i) {
        s_station_rows[i] = radio_player_station(i);
    }
    const std::size_t shown = s_list_state == RadioListState::Ready ? count : 0;
    radio_ui_show_station_list(s_current_city, s_list_state, s_station_rows, shown,
                               s_list_selected, radio_player_station_index(),
                               s_list_detail);
}

void adjust_volume(int delta) {
    const uint8_t cur = radio_player_volume();
    int next = static_cast<int>(cur) + delta;
    if (next < 0) next = 0;
    if (next > 100) next = 100;
    radio_player_set_volume(static_cast<uint8_t>(next));
    ESP_LOGI(TAG, "Radio volume: %u -> %d", cur, next);
}

bool catalog_cancelled() { return s_catalog_cancel.load() || !s_radio_active.load(); }

void catalog_worker(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Starting shared city station catalog search...");
    constexpr std::size_t kDiscoveryCapacity = 8;
    // Release the live decoder/TLS working set before directory HTTP/cJSON.
    radio_player_set_playing(false);
    if (!radio_player_wait_idle(2000) || catalog_cancelled()) {
        // A timed-out decoder/TLS worker still owns its memory. Do not overlap
        // it with another HTTP connection, and leave normal playback retryable.
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGW(TAG, "Catalog deferred: player has not released stream resources");
        vTaskDelete(nullptr);
        return;
    }
    auto *stations = static_cast<RadioStation *>(
        std::calloc(kDiscoveryCapacity, sizeof(RadioStation)));
    if (!stations) {
        xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
        if (!catalog_cancelled()) radio_player_set_playing(true);
        s_catalog_running.store(false);
        xSemaphoreGive(s_catalog_commit);
        ESP_LOGE(TAG, "Unable to allocate station catalog");
        vTaskDelete(nullptr);
        return;
    }
    home_profile_t profile;
    home_profile_get(&profile);
    RadioLocation preferred = {}, loc = {};
    radio_catalog_fill_preference(profile, &preferred);
    std::size_t found = radio_catalog_discover_location(preferred, stations, kDiscoveryCapacity,
                                                       &loc, catalog_cancelled);
    // Serialize installation/start with stop; no late playback in the next app.
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    const bool alive = !catalog_cancelled() && s_ui_alive.load();
    if (alive && found > 0) {
        ESP_LOGI(TAG, "Location detected: %s (%s), found %u stations", loc.city, loc.region, (unsigned)found);
        radio_ui_set_location(loc.city);
        radio_player_replace_stations_resuming(stations, found);
        s_catalog_installed.store(true);
        radio_catalog_apply_result(loc.city, found);
    } else if (alive) {
        // 目录一条都没查到：不安装、保留可播放的预设台，列表明确显示空态，
        // 不伪造分项，也不把「加载中」一直挂在那里。
        radio_catalog_apply_result(nullptr, 0);
    }
    std::free(stations);
    if (alive) {
        // Start the MP3 decoder only after directory HTTP/cJSON is released.
        radio_player_set_playing(true);
        radio_catalog_refresh_list();
    }
    xSemaphoreGive(s_catalog_commit);
    s_catalog_running.store(false);
    vTaskDelete(nullptr);
}

void start_catalog() {
#ifdef PASSPORT_RADIO_HLS_TEST
    // Keep the known test matrix visible; city discovery would replace it.
    radio_ui_set_location("全球电台测试");
    radio_player_set_playing(true);
    return;
#endif
    if (!s_catalog_commit) return;
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    bool idle = false;
    // Reserve before task creation; a fast worker cannot leave a stale handle.
    if (!s_radio_active.load() || !s_player_ready.load() || s_catalog_installed.load() ||
        !s_catalog_running.compare_exchange_strong(idle, true)) {
        xSemaphoreGive(s_catalog_commit);
        return;
    }
    s_catalog_cancel.store(false);
    if (xTaskCreate(catalog_worker, "radio_cat", 6144, nullptr, 5, nullptr) != pdPASS) {
        s_catalog_running.store(false);
        ESP_LOGE(TAG, "Failed to create catalog task");
        radio_player_set_playing(true);
    }
    xSemaphoreGive(s_catalog_commit);
}

void radio_catalog_fill_preference(const home_profile_t &profile,
                                   RadioLocation *preferred) {
    if (!preferred) return;
    *preferred = {};
    if (s_city_override[0]) {
        // 设备端选了城市：city 与 search_city 都填，走 discover_location
        // 的「preferred.city 非空」分支，跳过 IP 定位且保留取消回调。
        // 不用 radio_catalog_discover_city()：它没有 cancelled 参数，
        // 快速连切城市时旧查询无法中断。
        std::snprintf(preferred->city, sizeof(preferred->city), "%s", s_city_override);
        std::snprintf(preferred->search_city, sizeof(preferred->search_city), "%s",
                      s_city_override);
        std::snprintf(preferred->country_code, sizeof(preferred->country_code), "%s",
                      profile.country_code[0] ? profile.country_code : "CN");
    } else {
        std::snprintf(preferred->city, sizeof(preferred->city), "%s", profile.city);
        std::snprintf(preferred->region, sizeof(preferred->region), "%s", profile.region);
        std::snprintf(preferred->search_city, sizeof(preferred->search_city), "%s",
                      profile.radio_city);
        std::snprintf(preferred->country_code, sizeof(preferred->country_code), "%s",
                      profile.country_code);
    }
}

void radio_catalog_apply_result(const char *city, std::size_t found) {
    if (found > 0) {
        if (city && city[0]) {
            std::snprintf(s_current_city, sizeof(s_current_city), "%s", city);
        }
        s_list_state = RadioListState::Ready;
        s_list_detail[0] = '\0';
    } else {
        s_list_state = RadioListState::Empty;
        std::snprintf(s_list_detail, sizeof(s_list_detail), "%s",
                      "未查到电台，可切换城市重试");
    }
}

void radio_catalog_refresh_list() {
    // 切换城市后重建列表：名称/频率/播放状态与选中态一起刷新。
    if (s_radio_page != RadioPage::Main) refresh_station_list();
}

void restart_catalog() {
    if (!s_catalog_commit) return;
    xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    s_catalog_installed.store(false);
    xSemaphoreGive(s_catalog_commit);

    // 旧 worker 可能还卡在 HTTP 上。等它自己退出再起新的：两个 HTTP 连接
    // 同时占堆会拖垮 C3，而且旧城市的结果不能覆盖新城市的列表。
    for (int i = 0; i < 60 && s_catalog_running.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_catalog_running.load()) {
        ESP_LOGW(TAG, "City switch deferred: catalog still exiting");
        s_list_state = RadioListState::Error;
        std::snprintf(s_list_detail, sizeof(s_list_detail), "%s",
                      "目录仍在退出，请稍后重试");
        refresh_station_list();
        return;
    }
    start_catalog();
}

void switch_city(std::size_t index) {
    const char *city = radio_ui_city_name(index);
    if (!city) return;
    std::snprintf(s_current_city, sizeof(s_current_city), "%s", city);
    if (index == 0) {
        // 「自动定位」= 清掉设备端覆盖，回到 profile / IP 定位这条既有路径。
        s_city_override[0] = '\0';
    } else {
        std::snprintf(s_city_override, sizeof(s_city_override), "%s", city);
    }
    s_list_state = RadioListState::Loading;
    s_list_selected = 0;
    s_radio_page = RadioPage::StationList;
    refresh_station_list();   // 先把「正在搜索」画出来，再真正发起查询
    ESP_LOGI(TAG, "City switched to %s", city);
    restart_catalog();
}

void on_wifi_event(WifiEvent event, const std::string &data) {
    if (!s_radio_active.load()) return;

    if (event == WifiEvent::Connected) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s", data.c_str());
        s_wifi_connected.store(true);
        radio_ui_set_network(true, ("已连接: " + data).c_str());
        if (!s_player_ready.load()) return;
        radio_player_set_network(true);

        start_catalog();
    } else if (event == WifiEvent::Disconnected) {
        ESP_LOGI(TAG, "Wi-Fi disconnected: %s", data.c_str());
        s_wifi_connected.store(false);
        radio_ui_set_network(false, "Wi-Fi已断开");
        if (s_player_ready.load()) radio_player_set_network(false);
    }
}

} // namespace

void radio_app_init(void) {
    ESP_LOGI(TAG, "Initializing Radio App subsystems...");
    radio_ui_init();
    static StaticSemaphore_t catalog_commit_storage;
    s_catalog_commit = xSemaphoreCreateMutexStatic(&catalog_commit_storage);
}

void radio_app_start(void) {
    ESP_LOGI(TAG, "Starting Radio App...");
    if (s_radio_active.load()) {
        ESP_LOGW(TAG, "Radio App is already active");
        return;
    }
    // 每次进入 Radio 都从主屏开始；上次在设备端选过的城市沿用（override 不清），
    // 目录状态回到「加载中」，等 catalog_worker 给出结果后再翻成 Ready/Empty/Error。
    s_radio_page = RadioPage::Main;
    s_list_state = RadioListState::Loading;
    s_list_selected = 0;
    s_list_detail[0] = '\0';
    // 1. 显示 Radio UI 界面
    radio_ui_show_main();
    radio_ui_set_volume(radio_player_volume());
    if (!s_catalog_commit || s_catalog_running.load()) {
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "目录仍在退出，请稍后重试");
        return;
    }

    // A previous bounded stop may have retained a slow stream worker to avoid
    // freeing decoder/I2S resources underneath it.  Finish that cleanup before
    // creating the next session; never allow two player owners to coexist.
    if (s_player_ready.load()) {
        if (!radio_player_deinit(2000)) {
            s_radio_active.store(false);
            s_ui_alive.store(true);
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "上次音频仍在退出，请稍后重试");
            ESP_LOGE(TAG, "Previous player worker is still alive; refusing a second owner");
            return;
        }
        s_player_ready.store(false);
    }

    if (!radio_player_init()) {
        s_player_ready.store(false);
        s_radio_active.store(false);
        s_ui_alive.store(true);
        radio_ui_set_playback(RadioPlaybackState::Error, "内存不足，音频启动失败");
        return;
    }
    s_player_ready.store(true);
    s_catalog_installed.store(false); // stations were reset to presets above
    s_radio_active.store(true);
    s_ui_alive.store(true);
    radio_player_set_playing(false);

    // 2. 挂接全局 Wi-Fi 事件回调
    WifiManager::GetInstance().SetEventCallback(on_wifi_event);
    if (!WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "Wi-Fi not connected yet, checking station...");
        if (!SsidManager::GetInstance().GetSsidList().empty()) {
            radio_ui_set_network(false, "正在连接网络...");
            WifiManager::GetInstance().StartStation();
        } else {
            radio_ui_set_playback(RadioPlaybackState::Error, "请在手机设置中配网");
        }
    } else {
        radio_ui_set_network(true, "Wi-Fi已连接");
        radio_player_set_network(true);
        start_catalog();
    }

    // 3. 目录查询释放 HTTP/cJSON 内存后再启动播放。
}

bool radio_app_prepare_stop(void) {
    // Match catalog installation's lock: it must not resume playback after
    // this cancellation has stopped the player.
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    if (s_radio_active.exchange(false) && s_player_ready.load())
        radio_player_set_playing(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);
    if (s_catalog_running.load()) return false;
    if (s_player_ready.load()) {
        if (!radio_player_deinit(0)) return false;
        s_player_ready.store(false);
    }
    return true;
}

void radio_app_stop(void) {
    ESP_LOGI(TAG, "Stopping Radio App (keeping global Wi-Fi active)...");
    s_radio_page = RadioPage::Main;
    if (s_catalog_commit) xSemaphoreTake(s_catalog_commit, portMAX_DELAY);
    s_catalog_cancel.store(true);
    s_radio_active.store(false);
    if (s_catalog_commit) xSemaphoreGive(s_catalog_commit);

    // 1. 停止播放并等待拉流线程退出
    if (s_player_ready.load()) {
        radio_player_set_playing(false);
        radio_player_wait_idle(3500);
    }

    // 2. 2026-09-13 修复(C2):目录任务**必须 join**。原来只置标志就返回,
    //    任务还占着 6144B 栈 + 24KB 响应缓冲(单次 HTTP 最长 5s),紧接着进
    //    小智/语音时极易分配失败。这里等 5s 上界,超时也不 vTaskDelete
    //    (它可能正持堆锁),只记日志由它自行退出。
    s_ui_alive.store(false);
    int waited_ms = 0;
    while (s_catalog_running.load() && waited_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited_ms += 50;
    }
    if (s_catalog_running.load()) {
        ESP_LOGW(TAG, "catalog task still running after 5s; left to exit by itself");
    }

    // 3. 移除事件回调，保留全局 Wi-Fi 连接
    WifiManager::GetInstance().SetEventCallback(nullptr);

    // 4. Radio is a session, not a boot-lifetime service. Release the 7KB
    // stream task, decoder registry, codec objects and I2S DMA before the next
    // app (especially Xiaozhi) tries to allocate a large contiguous block.
    if (s_player_ready.load()) {
        if (radio_player_deinit(5000)) s_player_ready.store(false);
    }

    // 5. 隐藏 Radio UI 并暂停高频定时器
    radio_ui_hide();

    ESP_LOGI(TAG, "Radio App stopped. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void radio_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_player_ready.load()) return;

    if (event == BTN_EVT_CLICK) {
        if (s_radio_page == RadioPage::Main) {
            // 主屏交互保持原样：上下换台、OK 播放/暂停。
            if (btn == BSP_BTN_UP) radio_player_select_relative(1);
            else if (btn == BSP_BTN_DOWN) radio_player_select_relative(-1);
            else if (btn == BSP_BTN_OK) radio_player_toggle();
            return;
        }

        if (s_radio_page == RadioPage::StationList) {
            const std::size_t total = station_total();
            const std::size_t shown = total - 2;   // 末尾两项是操作行
            if (btn == BSP_BTN_UP) {
                s_list_selected = total ? (s_list_selected + total - 1) % total : 0;
                refresh_station_list();
            } else if (btn == BSP_BTN_DOWN) {
                s_list_selected = total ? (s_list_selected + 1) % total : 0;
                refresh_station_list();
            } else if (btn == BSP_BTN_OK) {
                if (s_list_selected < shown) {
                    // 选中别的台 = 切换电台并播放；再按当前台 = 暂停/恢复。
                    if (s_list_selected == radio_player_station_index()) {
                        radio_player_toggle();
                    } else {
                        radio_player_set_station(s_list_selected);
                        radio_player_set_playing(true);
                    }
                    refresh_station_list();
                } else if (s_list_selected == shown) {
                    s_radio_page = RadioPage::City;
                    radio_ui_show_settings(RadioSettingsPage::City, s_city_selected,
                                           radio_player_volume());
                } else {
                    s_radio_page = RadioPage::Main;
                    radio_ui_show_main();
                }
            }
            return;
        }

        if (s_radio_page == RadioPage::City) {
            const std::size_t count = radio_ui_city_count();
            if (btn == BSP_BTN_UP) {
                s_city_selected = count ? (s_city_selected + count - 1) % count : 0;
            } else if (btn == BSP_BTN_DOWN) {
                s_city_selected = count ? (s_city_selected + 1) % count : 0;
            } else if (btn == BSP_BTN_OK) {
                switch_city(s_city_selected);   // 回列表页并显示「正在搜索」
                return;
            }
            radio_ui_show_settings(RadioSettingsPage::City, s_city_selected,
                                   radio_player_volume());
        }
        return;
    }

    if (event == BTN_EVT_LONG) {
        if (btn == BSP_BTN_UP) {
            if (s_radio_page == RadioPage::Main) {
                // 主屏长按上：进入城市电台列表。音量 + 仍在列表/城市页可用。
                s_radio_page = RadioPage::StationList;
                s_list_selected = std::min(radio_player_station_index(),
                                           kStationCapacity - 1);
                refresh_station_list();
            } else {
                adjust_volume(10);
            }
        } else if (btn == BSP_BTN_DOWN) {
            if (s_radio_page == RadioPage::Main) {
                adjust_volume(-10);
            } else if (s_radio_page == RadioPage::StationList) {
                s_radio_page = RadioPage::Main;
                radio_ui_show_main();
            } else {
                s_radio_page = RadioPage::StationList;
                refresh_station_list();
            }
        }
        // 长按 OK 一律不处理：整机统一的「返回」继续由 app_registry 拦截，
        // 不设 hold_ok，其它应用的按键语义完全不受影响。
    }
}

const passport_app_t g_radio_app = {
    .id = APP_ID_RADIO,
    .name = "城市收音机",
    .en_name = "RADIO",
    .desc = "收听城市网络电台\n上下换台，确定播放",
    .tag = "直连",
    .theme_color = 0xFFB74D,
    .init = radio_app_init,
    .start = radio_app_start,
    .stop = radio_app_stop,
    .on_key = radio_app_on_key,
    .prepare_stop = radio_app_prepare_stop,
};
