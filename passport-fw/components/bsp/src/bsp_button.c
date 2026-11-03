// components/bsp/src/bsp_button.c
// 移植自 trae_card/components/platform/platform_esp32/src/btn_iot_button.c
#include "bsp_button.h"
#include "bsp_pins.h"
#include "iot_button.h"
#include "button_adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

// 面板上电瞬态抑制窗口:SLPOUT+120ms+DISPON+LVGL 全屏刷新 ~300ms,取 600ms。
#define BSP_BTN_PANEL_GLITCH_US (600 * 1000)

// 面板上电 SPI 活动会耦合进 ADC1_CH0(按键分压),此时间窗内的读数不可信。
// bsp_display_power(true) 在唤醒序列前登记抑制截止时刻,读路径见 read_mv。
// 历史上这是对 managed_component button_adc.c 的补丁,组件升级会覆盖,故收编到此。
static volatile int64_t s_ignore_until_us = 0;

void button_adc_set_ignore_until(int64_t until_us) {
    if (until_us > s_ignore_until_us) s_ignore_until_us = until_us;
}

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t    s_cb;
static void           *s_user;

// ADC1 是 unit 级独占资源:iot_button 与 bsp_button_read_mv() 必须共用同一个 oneshot
// 句柄。谁第二个调 adc_oneshot_new_unit() 谁就拿到 "adc1 is already in use"。
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

// BSP 轮询与 button 组件共用衰减档,校准也必须一致,否则读数会与按键阈值错位。
// managed_components/espressif__button/button_adc.c:26 在 C3 上取 ADC_ATTEN_DB_6+1。
#define BSP_BTN_ATTEN  ADC_ATTEN_DB_12       // 量程约 0~3100mV,覆盖松开态

// 每个按键把"哪个键"随回调带回来。button 组件的回调签名固定,故用 usr_data 传索引。
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press  (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);   }
static void cb_release(void *a, void *u) { on_event(a, u, BSP_BTN_RELEASE); }
static void cb_click  (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);   }
static void cb_double (void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE);  }
static void cb_long   (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);    }
static void cb_long_up(void *a, void *u) { on_event(a, u, BSP_BTN_LONG_UP); }

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    s_cb = cb; s_user = user;

    // 先由 BSP 建 unit,再把句柄交给 button 组件(button_adc.h:adc_handle 非 NULL 即复用),
    // 这样本文件的 bsp_button_read_mv() 也能读同一路 ADC。
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t ae = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit 创建失败 (%s)", esp_err_to_name(ae));
        s_adc = NULL;
        return ae;
    }

    // 主程序用无回调的轮询模式,不会调用 iot_button_new_adc_device。
    // 因此通道必须由 BSP 配置,不能依赖 button 组件的创建流程。
    const adc_oneshot_chan_cfg_t channel_cfg = {
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &channel_cfg);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC 通道配置失败 (%s)", esp_err_to_name(ae));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ae;
    }

    if (cb) {
        for (int i = 0; i < BSP_BTN_COUNT; i++) {
            const button_adc_config_t ac = {
                .adc_handle   = &s_adc,          // 复用上面这一个,别让组件自建
                .unit_id      = BSP_BTN_ADC_UNIT,
                .adc_channel  = BSP_BTN_ADC_CHANNEL,
                .button_index = i,
                .min          = BTN_MV[i][0],
                .max          = BTN_MV[i][1],
            };
            button_config_t bc = { 0 };
            bc.long_press_time = 500;
            esp_err_t e = iot_button_new_adc_device(&bc, &ac, &s_btn[i]);
            if (e != ESP_OK || !s_btn[i]) {
                ESP_LOGE(TAG, "按键 %d 创建失败 (%s) —— 检查 GPIO%d 的 ADC 配置与分压电阻",
                         i, esp_err_to_name(e), BSP_BTN_ADC_CHANNEL);
                return e == ESP_OK ? ESP_FAIL : e;
            }
            void *idx = (void *)(intptr_t)i;
            iot_button_register_cb(s_btn[i], BUTTON_PRESS_DOWN,      NULL, cb_press,   idx);
            iot_button_register_cb(s_btn[i], BUTTON_PRESS_UP,        NULL, cb_release, idx);
            iot_button_register_cb(s_btn[i], BUTTON_SINGLE_CLICK,    NULL, cb_click,   idx);
            iot_button_register_cb(s_btn[i], BUTTON_DOUBLE_CLICK,    NULL, cb_double,  idx);
            iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_START,NULL, cb_long,    idx);
            iot_button_register_cb(s_btn[i], BUTTON_LONG_PRESS_UP,  NULL, cb_long_up,  idx);
        }
    }

    // 通道已配置好,这里创建与其衰减档一致的校准句柄给 bsp_button_read_mv() 用。
    // 轮询模式的按键判定依赖校准电压,失败必须报错;回调模式可用组件自己的校准。
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id  = BSP_BTN_ADC_UNIT,
        .chan     = BSP_BTN_ADC_CHANNEL,
        .atten    = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (ae != ESP_OK) {
        ESP_LOGW(TAG, "ADC 校准创建失败");
        s_cali = NULL;
        if (!cb) {
            adc_oneshot_del_unit(s_adc);
            s_adc = NULL;
            return ae;
        }
    }

    ESP_LOGI(TAG, "按键就绪:ADC1_CH%d 三键分压", BSP_BTN_ADC_CHANNEL);
    return ESP_OK;
}

void bsp_button_suppress_panel_glitch(void) {
    button_adc_set_ignore_until(esp_timer_get_time() + BSP_BTN_PANEL_GLITCH_US);
}

int bsp_button_read_mv(void) {
    // 读的是 bsp_button_init() 建好、并与 iot_button 共用的那一路 ADC。
    // 单次采样与组件的按键轮询互不干扰(oneshot 内部自带锁)。
    if (!s_adc || !s_cali) return -1;
    if (esp_timer_get_time() < s_ignore_until_us) return -1;  // 抑制窗内读数不可信

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}
