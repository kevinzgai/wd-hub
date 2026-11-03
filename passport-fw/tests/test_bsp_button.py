"""Exercise the production BSP's polling ADC setup, calibration and failure cleanup."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = re.sub(r'^#include .*$', '', (ROOT / 'components/bsp/src/bsp_button.c').read_text(), flags=re.M)
pins = (ROOT / 'components/bsp/include/bsp_pins.h').read_text()
pin_defines = '\n'.join(re.search(r'^#define[ \t]+' + name + r'[ \t]+[^\n]+', pins, flags=re.M).group(0)
                        for name in ('BSP_BTN_COUNT', 'BSP_BTN_MV_TABLE')) + '\n'
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define BSP_BTN_ADC_UNIT 1
#define BSP_BTN_ADC_CHANNEL 0
#define ADC_ATTEN_DB_12 3
#define ADC_BITWIDTH_DEFAULT 0
typedef int esp_err_t;
typedef int bsp_btn_t;
typedef int bsp_btn_ev_t;
enum { BSP_BTN_PRESS, BSP_BTN_RELEASE, BSP_BTN_CLICK, BSP_BTN_DOUBLE, BSP_BTN_LONG, BSP_BTN_LONG_UP };
enum { BUTTON_PRESS_DOWN, BUTTON_PRESS_UP, BUTTON_SINGLE_CLICK, BUTTON_DOUBLE_CLICK, BUTTON_LONG_PRESS_START, BUTTON_LONG_PRESS_UP };
typedef void (*bsp_btn_cb_t)(bsp_btn_t, bsp_btn_ev_t, void *);
typedef void *button_handle_t;
typedef struct { bool configured; int atten, bitwidth; } adc_mock_t;
typedef adc_mock_t *adc_oneshot_unit_handle_t;
typedef int *adc_cali_handle_t;
typedef struct { int unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { int atten, bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { adc_oneshot_unit_handle_t *adc_handle; int unit_id, adc_channel, button_index, min, max; } button_adc_config_t;
typedef struct { int long_press_time; } button_config_t;
typedef struct { int unit_id, chan, atten, bitwidth; } adc_cali_curve_fitting_config_t;
static adc_mock_t mock_adc;
static int calibration, new_calls, config_calls, delete_calls, cal_calls, callback_devices, read_calls;
static int config_error, new_error, cal_error, input_mv;
static int64_t now;
static int64_t esp_timer_get_time(void) { return now; }
static esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *out) {
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT); ++new_calls;
    if (new_error) return new_error;
    mock_adc = (adc_mock_t){0}; *out = &mock_adc; return ESP_OK;
}
static esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t adc, int channel, const adc_oneshot_chan_cfg_t *cfg) {
    assert(adc == &mock_adc && channel == BSP_BTN_ADC_CHANNEL); ++config_calls;
    assert(!read_calls && !callback_devices && !cal_calls);
    assert(cfg->atten == ADC_ATTEN_DB_12 && cfg->bitwidth == ADC_BITWIDTH_DEFAULT);
    if (config_error) return config_error;
    adc->configured = true; adc->atten = cfg->atten; adc->bitwidth = cfg->bitwidth; return ESP_OK;
}
static esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t adc) {
    assert(adc == &mock_adc); ++delete_calls; adc->configured = false; return ESP_OK;
}
static esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *out) {
    ++cal_calls; assert(mock_adc.configured);
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && cfg->chan == BSP_BTN_ADC_CHANNEL);
    assert(cfg->atten == mock_adc.atten && cfg->bitwidth == mock_adc.bitwidth);
    if (cal_error) return cal_error;
    calibration = cfg->atten; *out = &calibration; return ESP_OK;
}
static esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t adc, int channel, int *raw) {
    assert(adc == &mock_adc && adc->configured && channel == BSP_BTN_ADC_CHANNEL);
    ++read_calls; *raw = input_mv; return ESP_OK;
}
static esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *mv) {
    assert(handle == &calibration && calibration == mock_adc.atten); *mv = raw; return ESP_OK;
}
static esp_err_t iot_button_new_adc_device(const button_config_t *bc, const button_adc_config_t *ac, button_handle_t *out) {
    assert(bc->long_press_time == 500 && *ac->adc_handle == &mock_adc && mock_adc.configured);
    const int measured_mv[] = {0, 300, 600};
    assert(ac->button_index >= 0 && ac->button_index < BSP_BTN_COUNT);
    assert(measured_mv[ac->button_index] >= ac->min && measured_mv[ac->button_index] < ac->max);
    ++callback_devices; *out = &mock_adc; return ESP_OK;
}
static esp_err_t iot_button_register_cb(button_handle_t handle, int event, void *args, void (*cb)(void *,void *), void *user) {
    (void)event; (void)args; (void)user; assert(handle && cb); return ESP_OK;
}
'''
checks = r'''
static void reset(void) {
    s_adc = NULL; s_cali = NULL; s_cb = NULL; s_ignore_until_us = 0;
    mock_adc = (adc_mock_t){0}; new_calls = config_calls = delete_calls = cal_calls = 0;
    callback_devices = read_calls = config_error = new_error = cal_error = 0; now = 1;
}
static void callback(bsp_btn_t btn, bsp_btn_ev_t event, void *user) { (void)btn; (void)event; (void)user; }
int main(void) {
    reset();
    // The actual application passes nullptr: configure before any read without
    // relying on iot_button's optional callback-device creation.
    assert(bsp_button_init(NULL, NULL) == ESP_OK);
    assert(new_calls == 1 && config_calls == 1 && cal_calls == 1 && callback_devices == 0);
    for (int i = 0; i < 4; ++i) {
        const int samples[] = {0, 300, 600, 2900}; input_mv = samples[i];
        assert(bsp_button_read_mv() == samples[i]);
        if (i < BSP_BTN_COUNT) assert(samples[i] >= BTN_MV[i][0] && samples[i] < BTN_MV[i][1]);
        else for (int button = 0; button < BSP_BTN_COUNT; ++button) assert(samples[i] >= BTN_MV[button][1]);
    }
    assert(read_calls == 4);
    // The existing panel-glitch window must still reject ADC reads briefly.
    bsp_button_suppress_panel_glitch(); assert(bsp_button_read_mv() == -1 && read_calls == 4);
    now += 600001; assert(bsp_button_read_mv() == input_mv && read_calls == 5);
    reset(); config_error = ESP_FAIL;
    assert(bsp_button_init(NULL, NULL) == ESP_FAIL);
    assert(config_calls == 1 && delete_calls == 1 && !cal_calls && !callback_devices && s_adc == NULL);
    assert(bsp_button_read_mv() == -1 && !read_calls);
    // A failed configuration releases the exclusive ADC unit so retry works.
    config_error = 0; assert(bsp_button_init(NULL, NULL) == ESP_OK);
    input_mv = 300; assert(bsp_button_read_mv() == 300);
    reset(); new_error = ESP_ERR_NO_MEM;
    assert(bsp_button_init(NULL, NULL) == ESP_ERR_NO_MEM);
    assert(!config_calls && !delete_calls && !cal_calls && s_adc == NULL);
    reset();
    assert(bsp_button_init(callback, NULL) == ESP_OK);
    assert(config_calls == 1 && callback_devices == 3 && cal_calls == 1);
    reset(); cal_error = ESP_FAIL;
    assert(bsp_button_init(NULL, NULL) == ESP_FAIL);
    assert(delete_calls == 1 && s_adc == NULL && s_cali == NULL && bsp_button_read_mv() == -1 && !read_calls);
    reset(); cal_error = ESP_FAIL;
    assert(bsp_button_init(callback, NULL) == ESP_OK);
    assert(callback_devices == 3 && !delete_calls && mock_adc.configured);
    assert(s_cali == NULL && bsp_button_read_mv() == -1 && !read_calls);
    puts("BSP button: polling ADC setup, shared calibration, callback mode, failure cleanup/retry and glitch window PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='passport-bsp-button-') as directory:
    directory = Path(directory)
    test = directory / 'test.c'
    test.write_text(pin_defines + prelude + source + checks)
    exe = directory / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', '-g', str(test), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
