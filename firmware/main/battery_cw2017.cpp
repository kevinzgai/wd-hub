// 电池电量读取 —— ESP32-C3（上千 AI Passport）专用实现：CW2017 电量计（I2C）。
//
// 与 battery.cpp 的关系：对外接口完全相同（battery_init / battery_get /
// battery_raw_to_percent），由 CMake 按芯片型号二选一编译。
//
// 为什么不能复用 battery.cpp：
//   · 它读 S3 的 ADC2_CH6 = GPIO17，而 C3 上 GPIO17 根本不存在（QFN32
//     引脚最多到 21）；
//   · 它读 GPIO38 做充电检测，C3 没有这个脚，这块板也没有充电检测电路。
//
// 这块板用 CW2017 电量计（I2C 0x63，与 ES8311 codec 共用 SDA10/SCL7）。
// 寄存器与上电时序参考 passport-os 仓库 components/bsp/src/bsp_battery.c
// (MIT License, HEAD b4d9555)：
//   VERSION 0x00 / VCELL 0x02(14bit, 312.5µV/LSB) / SOC 0x04(高字节整数百分比)
//   CONFIG  0x08(0xF0 睡眠 / 0x30 复位态 / 0x00 正常)
//   SOC_ALERT 0x0B(bit7 = profile 已写入标志)
//   PROFILE 0x10(80 字节厂家电芯参数，优特利 520mAh)
// 驱动按上述寄存器规格独立实现，没有整文件拷贝。

#include "battery.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "board_config.h"

static const char *TAG = "battery";

#define CW_REG_VERSION   0x00
#define CW_REG_VCELL_H   0x02
#define CW_REG_SOC_H     0x04
#define CW_REG_CONFIG    0x08
#define CW_REG_SOC_ALERT 0x0B
#define CW_REG_PROFILE   0x10

#define CW_CONFIG_ACTIVE   0x00
#define CW_CONFIG_RESTART  0x30
#define CW_CONFIG_SLEEP    0xF0
#define CW_UPDATE_FLAG     0x80
#define CW_PROFILE_SIZE    80

#define BAT_POLL_MS        30000   // 电量是慢变量，30 秒一次足够

// 厂家为"优特利 520mAh"电芯生成的 80 字节 profile。
// ⚠ 换电芯必须同步替换，否则 SOC 会按错误的曲线计算。
static const uint8_t s_profile[CW_PROFILE_SIZE] = {
    0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAD, 0xC7, 0xC8, 0xCA, 0xBD, 0xB1, 0xC1, 0x94,
    0x88, 0xD1, 0xBD, 0x97, 0x88, 0x66, 0x56, 0x4A,
    0x3F, 0x33, 0x26, 0x5C, 0x37, 0xD1, 0x27, 0xD8,
    0xCC, 0xB7, 0xCF, 0xB3, 0xB2, 0xAE, 0xA6, 0x9E,
    0x99, 0x97, 0x9B, 0x86, 0x47, 0x1E, 0x17, 0x26,
    0x49, 0x96, 0xD9, 0xE1, 0xDD, 0xDC, 0xD4, 0x59,
    0x00, 0x00, 0x90, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5C,
};
static_assert(sizeof(s_profile) == CW_PROFILE_SIZE, "CW2017 profile 必须是 80 字节");

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t        s_lock;

static battery_state_t s_state = {
    .raw = -1, .mv = -1, .percent = -1, .volt_pct = -1,
    .charging = false, .percent_held = false, .valid = false,
};

// ---------------------------------------------------------------------------
// I2C 基础读写
// ---------------------------------------------------------------------------
static int cw_read(uint8_t reg, uint8_t *buf, size_t n)
{
    if (!s_dev) return -1;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100) == ESP_OK ? 0 : -1;
}

static int cw_write(uint8_t reg, uint8_t val)
{
    if (!s_dev) return -1;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100) == ESP_OK ? 0 : -1;
}

// CONFIG 低 4bit 是保留位，切换状态时要原样带回。
static int cw_set_config(uint8_t mode)
{
    uint8_t cfg = 0;
    if (cw_read(CW_REG_CONFIG, &cfg, 1) != 0) return -1;
    const uint8_t reserved = cfg & 0x0F;
    if (cw_write(CW_REG_CONFIG, reserved | CW_CONFIG_RESTART) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (cw_write(CW_REG_CONFIG, reserved | mode) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

// 同时核对 UPDATE_FLAG 与 80 字节内容，避免只凭标志误用旧电芯参数。
static bool cw_profile_matches(void)
{
    uint8_t val = 0;
    if (cw_read(CW_REG_SOC_ALERT, &val, 1) != 0) return false;
    if ((val & CW_UPDATE_FLAG) == 0) return false;

    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1) != 0) return false;
        if (val != s_profile[i]) return false;
    }
    return true;
}

// profile 必须在睡眠态逐字节写入，读回校验后置 UPDATE_FLAG，再回正常态。
static int cw_update_profile(void)
{
    if (cw_set_config(CW_CONFIG_SLEEP) != 0) return -1;
    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_write((uint8_t)(CW_REG_PROFILE + i), s_profile[i]) != 0) {
            ESP_LOGE(TAG, "写 profile 失败 index=%u", (unsigned)i);
            return -1;
        }
    }
    uint8_t val = 0;
    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1) != 0) return -1;
        if (val != s_profile[i]) {
            ESP_LOGE(TAG, "profile 校验失败 index=%u", (unsigned)i);
            return -1;
        }
    }
    if (cw_read(CW_REG_SOC_ALERT, &val, 1) != 0) return -1;
    if (cw_write(CW_REG_SOC_ALERT, val | CW_UPDATE_FLAG) != 0) return -1;
    return cw_set_config(CW_CONFIG_ACTIVE);
}

// 首次计算期间 SOC 可能暂时 >100 或保持上电默认 0。
// 电压明显高于终止电压时的 0% 不是有效结果，继续等，最长约 5 秒。
static void cw_wait_soc_ready(void)
{
    for (int retry = 0; retry < 50; retry++) {
        uint8_t soc = 0;
        uint8_t vcell[2] = { 0 };
        vTaskDelay(pdMS_TO_TICKS(100));
        if (cw_read(CW_REG_SOC_H, &soc, 1) != 0 || soc > 100) continue;
        if (soc != 0) return;
        if (cw_read(CW_REG_VCELL_H, vcell, 2) != 0) continue;
        const int mv = (int)(((((uint32_t)vcell[0] << 8) | vcell[1]) & 0x3FFF)
                             * 3125 / 10000);
        if (mv <= 3400) return;
    }
    ESP_LOGW(TAG, "SOC 尚未就绪，后续周期采样会继续等它稳定");
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------
int battery_raw_to_percent(int raw)
{
    // C3 走电量计，不需要 raw→percent 换算。这里保留一个按电压的粗略映射，
    // 只给"确认电芯曲线是否离谱"时做参照：3.3V→0%，4.2V→100%。
    int mv = raw;
    if (mv <= 3300) return 0;
    if (mv >= 4200) return 100;
    return (mv - 3300) * 100 / (4200 - 3300);
}

static void bat_poll(void)
{
    uint8_t soc[2] = { 0 }, vcell[2] = { 0 }, cfg = 0;

    battery_state_t s;
    bool ok = true;
    if (cw_read(CW_REG_SOC_H, soc, 2) != 0 ||
        cw_read(CW_REG_VCELL_H, vcell, 2) != 0 ||
        cw_read(CW_REG_CONFIG, &cfg, 1) != 0) {
        ok = false;
    }

    const int mv = (int)(((((uint32_t)vcell[0] << 8) | vcell[1]) & 0x3FFF)
                         * 3125 / 10000);
    const int pct = (soc[0] <= 100) ? (int)soc[0] : -1;

    // 电量计睡着了（CONFIG 高 4bit 不是 0）就把它叫醒，下一轮再读。
    if (ok && (cfg & 0xF0) != CW_CONFIG_ACTIVE) {
        ESP_LOGW(TAG, "CW2017 处于睡眠/复位态，恢复 active");
        cw_set_config(CW_CONFIG_ACTIVE);
        ok = false;
    }
    if (ok && mv >= 2500 && mv <= 4500 && pct >= 0) {
        s.raw          = mv;      // 这块板没有 raw ADC，用电压占位（仅供日志）
        s.mv           = mv;
        s.percent      = pct;
        s.volt_pct     = battery_raw_to_percent(mv);
        s.charging     = false;   // 板上无充电检测
        s.percent_held = false;
        s.valid        = true;
    } else {
        // 保留上一次有效值；一次读失败不该让电量角标瞬间消失。
        s = s_state;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = s;
    xSemaphoreGive(s_lock);
}

static void bat_task(void *arg)
{
    (void)arg;
    int last_pct = -999;

    for (;;) {
        bat_poll();

        int pct;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        pct = s_state.percent;
        xSemaphoreGive(s_lock);

        if (pct != last_pct) {
            last_pct = pct;
            ESP_LOGI(TAG, "电量 %d%% (CW2017, %dmV)", pct, s_state.mv);
        }
        vTaskDelay(pdMS_TO_TICKS(BAT_POLL_MS));
    }
}

void battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock);

    i2c_master_bus_config_t bcfg = {};
    bcfg.i2c_port                    = BOARD_I2C_PORT;
    bcfg.sda_io_num                  = BOARD_I2C_SDA;
    bcfg.scl_io_num                  = BOARD_I2C_SCL;
    bcfg.clk_source                  = I2C_CLK_SRC_DEFAULT;
    bcfg.glitch_ignore_cnt           = 7;
    bcfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bcfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线创建失败: %s（电量不可用）", esp_err_to_name(err));
        s_bus = NULL;
        return;
    }

    i2c_device_config_t dcfg = {};
    dcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dcfg.device_address  = BOARD_I2C_CW2017_ADDR;
    dcfg.scl_speed_hz    = 100000;
    err = i2c_master_bus_add_device(s_bus, &dcfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CW2017 挂载失败: %s", esp_err_to_name(err));
        s_dev = NULL;
        return;
    }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != 0) {
        ESP_LOGW(TAG, "CW2017 未应答（0x%02X）—— 电量角标显示 --", BOARD_I2C_CW2017_ADDR);
        return;
    }
    ESP_LOGI(TAG, "CW2017 VERSION=0x%02X（SDA=GPIO%d SCL=GPIO%d）",
             ver, BOARD_I2C_SDA, BOARD_I2C_SCL);

    if (cw_profile_matches()) {
        ESP_LOGI(TAG, "电池 profile 已匹配（优特利 520mAh）");
        cw_set_config(CW_CONFIG_ACTIVE);
    } else {
        ESP_LOGI(TAG, "写入优特利 520mAh 电池 profile");
        if (cw_update_profile() != 0) {
            ESP_LOGE(TAG, "profile 写入失败 —— SOC 可能不准");
        }
    }
    cw_wait_soc_ready();

    // 先采一次再起任务，首帧就能带出真实百分比（而不是先显示 "--"）。
    bat_poll();

    if (xTaskCreate(bat_task, "bat", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "电量采样任务创建失败 —— 电量不会更新");
    }
}

void battery_get(battery_state_t *out)
{
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->raw = out->mv = out->percent = out->volt_pct = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}
