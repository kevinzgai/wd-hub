#pragma once

#include "radio_player.h"

#include <cstddef>
#include <cstdint>

struct WifiNetworkView {
    char ssid[33];
    int8_t rssi;
    bool secure;
};

#include "launcher/wifi_keyboard.h"

enum class RadioSettingsPage {
    Menu,
    Volume,
    City,
    SleepTimer,
    Display,
    WifiMenu,
    SavedWifi,
    WifiAction,
    WifiDeleteConfirm,
};

// 城市电台「目录层」的状态。播放态仍由 RadioPlaybackState 表达，两者分开，
// 这样「正在搜台」和「搜到了但正在缓冲」不会混成同一个状态。
enum class RadioListState {
    Loading,   // 正在按城市查询目录
    Ready,     // 目录可用
    Empty,     // 该城市没有可用电台
    Error,     // 查询或播放失败
};

bool radio_ui_init();
void radio_ui_show_main();
void radio_ui_hide();
void radio_ui_deinit();
void radio_ui_show_config(const char *ssid, const char *url);
void radio_ui_show_wifi_list(const WifiNetworkView *networks, std::size_t count,
                             std::size_t selected, bool scanning,
                             const char *setup_ap_ssid, bool can_cancel = false);
void radio_ui_show_wifi_keyboard(const char *ssid, std::size_t password_length,
                                 WifiKeyboardPage page, std::size_t selected,
                                 const char *status = nullptr);
void radio_ui_show_wifi_connecting(const char *ssid, const char *detail);
void radio_ui_show_wifi_success(const char *ssid);
const char *radio_ui_wifi_key(WifiKeyboardPage page, std::size_t index);
std::size_t radio_ui_wifi_key_count(WifiKeyboardPage page);
void radio_ui_set_network(bool connected, const char *detail);
void radio_ui_set_location(const char *location);
void radio_ui_set_station(std::size_t index, std::size_t count,
                          const RadioStation &station);
void radio_ui_set_playback(RadioPlaybackState state, const char *detail = nullptr);
void radio_ui_set_audio_levels(const uint8_t *levels, std::size_t count);
void radio_ui_show_settings(RadioSettingsPage page, std::size_t selected,
                            uint8_t volume);

// 城市电台列表页：每行显示电台名称、频率与当前播放状态。列表末尾由 UI 自动
// 追加「切换城市」「返回主屏」两行，因此 selected 的合法范围是 [0, count+2)。
// state 非 Ready 时只画状态说明和两个操作行，不画电台行。
void radio_ui_show_station_list(const char *city, RadioListState state,
                                const RadioStation *stations, std::size_t count,
                                std::size_t selected, std::size_t playing_index,
                                const char *detail);

// 城市选择器复用 RadioSettingsPage::City。业务层通过下面两个函数取同一份
// 城市表，避免再硬编码一份名字导致两边对不上。索引 0 是「自动定位」。
std::size_t radio_ui_city_count();
const char *radio_ui_city_name(std::size_t index);

void radio_ui_show_saved_wifi(const char *const *ssids, std::size_t count,
                              std::size_t selected);
void radio_ui_show_wifi_action(const char *ssid, std::size_t selected);
void radio_ui_show_wifi_delete_confirm(const char *ssid, std::size_t selected);
void radio_ui_show_easter_egg(const char *message);
bool radio_ui_show_easter_egg_frame(const uint8_t *jpeg, std::size_t size,
                                    uint16_t width, uint16_t height);
void radio_ui_finish_easter_egg();
void radio_ui_set_volume(uint8_t volume);
