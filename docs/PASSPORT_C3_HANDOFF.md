# 上千 AI Passport（ESP32-C3）固件 · 交接文档

> 更新时间：2026-11-02 20:10　｜　方案 B 第一步（板级层 + 三键 + 可编译产物）
> 本文只记录"接手就能开工"的事实，不含过程流水。

---

## 1. 一句话现状

**C3 变体已经能完整编译出固件**：`firmware/build-c3/wb-board.bin` = **2,462,272 B (0x259240)**，
bootloader 0x52f0，app 分区 0x7f0000（7.94MB）**剩余 5,865,408 B（70%）**；编译进程已正常结束（rc=0），
**没有遗留进程、没有半成品文件**，工作区处于"可直接重跑编译"的状态。
但 **C3 只验证到"编译通过"，从未上过板**；**S3 代码路径本轮改动后尚未重跑回归**（最后一次 S3 产物是今天 16:41 的 `firmware/build/wb-board.bin`，2,299,408 B）。

---

## 2. 已完成

### 2.1 新增文件

| 路径 | 职责 |
| --- | --- |
| `firmware/main/buttons_adc.cpp` | C3 三键 ADC1_CH0 轮询驱动；与 `buttons.cpp` 同接口 `buttons_start(hooks)`；阈值判定、800ms 长按、上电 600ms 毛刺抑制、事件映射到现有语义（[on_short](file:///c:/Users/Administrator/WorkBuddy/xiaozhiLed/firmware/main/buttons_adc.cpp#L156-L188) / [on_long](file:///c:/Users/Administrator/WorkBuddy/xiaozhiLed/firmware/main/buttons_adc.cpp#L190-L238)） |
| `firmware/main/battery_cw2017.cpp` | C3 电量计：I2C0 SDA10/SCL7，CW2017@0x63，读 VCELL/SOC + 80B 电池 profile，30s 轮询；`charging` 恒 false |
| `firmware/partitions_c3.csv` | 沿用板上原厂分区：nvs 0x9000/0x6000、phy_init 0xf000/0x1000、**factory 0x10000/0x7f0000**、无 otadata/OTA |
| `firmware/sdkconfig.defaults.c3` | C3 默认配置：8MB/DIO-80M、C3 目标、单核 160MHz、无 SPIRAM、USB-Serial-JTAG 控制台、BLE NimBLE 单连接、LVGL 配置 + `CONFIG_LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=y` |
| `toolchain/build-firmware-c3.sh` | C3 构建入口（riscv32-esp-elf **esp-15.2.0_20251204** + ninja，`-B build-c3`，`IDF_TARGET=esp32c3`） |

> 说明：`firmware/sdkconfig.c3` 是构建生成物（首次 configure 产出），不要手改后当配置源；配置源是 `sdkconfig.defaults.c3`。

### 2.2 修改文件

| 路径 | 改动 |
| --- | --- |
| `firmware/main/board_config.h` | 重写为 S3/C3 双分支：`#if CONFIG_IDF_TARGET_ESP32C3` 给 C3 全套引脚，`#else` 原样保留 S3；新增 `BOARD_LCD_SPI_HOST/SPI_MODE`、`BOARD_BL_LEDC_TIMER/CHANNEL/FREQ_HZ`（C3=0/0/20k，S3=1/1/5k） |
| `firmware/main/board_display.cpp` | 参数化 SPI host/mode/LEDC；C3 单缓冲（`LCD_BUF_LINES 12`，约 5.6KB）、S3 仍双缓冲 40 行；invert=1 / mirror=false / swap_xy=false / RGB565_SWAPPED 均保留 |
| `firmware/main/buttons.h` | `buttons_hooks_t` **末尾**追加 `prev_page` / `home` 两个钩子（放末尾避免 S3 既有字段错位） |
| `firmware/main/ui_board.h` | 新增 `ui_board_prev_page()` / `ui_board_home()`；全部声明包进 `extern "C"`（`wb_console.c` 是 C，否则链接不到 C++ 符号） |
| `firmware/main/ui_board.cpp` | 实现 `prev_page` / `home`（只写切屏请求标志，不碰 LVGL） |
| `firmware/main/main.cpp` | 钩子表补两个新函数；`btn_provision()` 在 `WB_BLE_ONLY` 下只打日志（不碰未初始化的 WiFi）；C3 的 LVGL 任务 6144 栈 + `MALLOC_CAP_INTERNAL` |
| `firmware/main/wb_console.c` | `handle_key()` 拆成 `#ifndef WB_KEYS3`（S3 单键逻辑一字未改）/ `#else`（C3 三键：[wb_console.c:415](file:///c:/Users/Administrator/WorkBuddy/xiaozhiLed/firmware/main/wb_console.c#L415-L493)） |
| `firmware/main/audio.cpp` | MCLK 用 `BOARD_SPK_MCLK`（C3=GPIO6），S3 仍是 `I2S_GPIO_UNUSED` |
| `firmware/main/CMakeLists.txt` | 按 target 选源（`buttons_adc.cpp`/`battery_cw2017.cpp` vs `buttons.cpp`/`battery.cpp`）；REQUIRES 加 `esp_driver_i2c`；C3 追加 `WB_KEYS3` 编译宏 |
| `toolchain/idf_build.py` | PATH 同时 glob `xtensa-esp-elf` 与 `riscv32-esp-elf`；补 `ESP_ROM_ELF_DIR`（缺失会让 bootloader 构建静默挂死） |
| `firmware/dependencies.lock` | 被 idf 依赖管理器在 C3 configure 时改动，**S3 回归前需复核此 diff** |

### 2.3 板级参数（照抄 gitee 仓库 `components/bsp/include/bsp_pins.h`，MIT）

| 项 | 值 |
| --- | --- |
| 屏 | ST7789P3，240×320 |
| LCD SPI | SPI2_HOST，40MHz，mode 0 |
| LCD 引脚 | MOSI 9 / SCLK 8 / CS 1 / DC 20 / **RST 未接（−1，走 SWRESET 软复位）** / BL 21 |
| 背光 | LEDC TIMER_0 / CHANNEL_0，10bit，20kHz |
| 按键 | ADC1_CH0 (GPIO0)，12dB 衰减，三键分压：UP 0Ω→0mV〔0–150〕、DOWN 1k→~300mV〔150–450〕、OK 2.2k→~595mV〔450–1000〕，松开 ≥2500mV（1000–2500 为死区） |
| 长按 | 800ms |
| I2C | I2C0，SDA 10 / SCL 7，ES8311@**0x18**（本轮未初始化）、CW2017@**0x63** |
| I2S | MCLK 6 / BCLK 5 / WS 3 / DOUT 2 / DIN 4（full-duplex），PA_CTRL 未接 |
| 缺失硬件 | 无 PSRAM、无充电检测 GPIO、无 OTA/ota 分区、无独立电源键 |
| Flash | 8MB，factory 0x10000 + 0x7f0000 |

### 2.4 三键语义（已实现，未实测）

| 事件 | 行为 |
| --- | --- |
| 短按 UP / DOWN | 上一屏 / 下一屏 |
| 短按 OK | 回总览（待机时任意键都唤醒，OK 为主） |
| 长按 DOWN | 返回首页 / 退出当前浮层 |
| 长按 OK | 亮度调节（沿用现有多档循环语义） |
| 长按 UP | 配网 / 配置页自救入口（BLE-only 固件只打日志） |
| 待机态 | 任意键 → 唤醒回总览 + 恢复 100% 背光 |
| 控制台占屏时 | 键事件改投 `console_key(name, long)`；审批页保留"短 OK=允许 / 长 OK=拒绝"，提问页保留"短 OK=移选项 / 长 OK=提交" |

**取舍说明**：仓库的关机长按（2s/5s）与双击语义**未采纳**，因为它会与已验收的"长按 OK=亮度、长按 DOWN=回首页"冲突；本轮以既有行为为准。

---

## 3. 未完成 / 待办（按优先级）

- [ ] **P0 分割/占用复核**：用 `idf.py size`（或读 `build-c3/*.map`）导出字库与 LVGL 精确占用，把"12/16/28px 合计 ≈5.9MB"改为实测值（注意 5.9MB 是 **.c 源文本体积**，整包仅 2.35MB，Flash 实际压力远小于原估值）
- [ ] **P0 S3 回归**：重跑 `toolchain/build-firmware.sh`，确认 `wb-board.bin` 仍生成且行为不变；顺带检查 `dependencies.lock` 的改动是否影响 S3
- [ ] **P1 240×320 布局**：现 UI 几何常量写死 240×240（图表环 x=16/89/162、页脚 y=185 等），C3 上会出现下 80px 空白/元素偏上；需要按 `BOARD_LCD_V_RES` 做一版布局分支
- [ ] **P1 三键真机实测**：UP/DOWN/OK 阈值与长按 800ms 在实物上校准（尤其 12dB 衰减下 OK 档 ~595mV 与 1000mV 上限的余量）
- [ ] **P1 烧录与实机验证**：`esptool` 写 bootloader/partition-table/app 到 0x0/0x8000/0x10000，验证 SWRESET 软复位能让屏正常点亮、invert/swap 正确、背光 20kHz 无啸叫
- [ ] **P2 ES8311 初始化**：I2C 配置 codec + I2S 启停，音频通路打通（本轮 `audio.cpp` 只接了 MCLK 引脚）
- [ ] **P2 BLE 配网/连接**：`WB_BLE_ONLY` 下的真实配网流程与小智协议联调
- [ ] **P2 待机/休眠策略**：C3 无 PSRAM，需复核 LVGL 与后台任务的 RAM 峰值（当前 SRAM≈400KB）

---

## 4. 关键约束与已知风险

- **无 PSRAM**：LVGL 单缓冲 12 行 ≈5.6KB，`lvgl` 任务栈 6144 且只能用 `MALLOC_CAP_INTERNAL`；任何 `MALLOC_CAP_SPIRAM` 分配在 C3 上都会失败。
- **LVGL 版本差异**：本工程锁定 9.x（managed_components 内当前为 lvgl 9.x），仓库用 9.5/9.6 系列；移植 API 时以本工程版本为准（编译已通过即说明无阻断性差异，但 deprecated 警告较多）。
- **字库体积**：12px 源 2.11MB、16px 3.24MB、28px 0.55MB（源文本）；**按决策不裁字库**，分区放得下（实测整包 2.35MB < 7.94MB）。
- **License**：仓库根为 **MIT**（板级常量与思路可移植，已在本项目注释注明来源与 HEAD）；`components/passport_muse/` 为 **Apache-2.0**，**本轮未涉及、也未拷贝**。
- **上游定位**：gitee `https://gitee.com/alex_kan/passport-os`，HEAD **b4d9555**（`b4d955565759399e110cd45c84c6928ec1f27849`）；只读参考，不整仓拷入本项目。
- **左 1 键不参与逻辑**（用户决策），板上另一颗物理键留空。
- **`firmware/build-c3/` 未被 .gitignore 覆盖**（git status 可见为 untracked）；正式提交前应加忽略项或确认不误提交。

---

## 5. 恢复工作的命令

**第一条（确认环境与产物仍在）**

```bash
cd /c/Users/Administrator/WorkBuddy/xiaozhiLed && ls -l firmware/build-c3/wb-board.bin
```

**C3 构建**（Git Bash；不可用 `bash` 裸命令，必须用 Git Bash 绝对路径）

```bash
& "C:\Program Files\Git\bin\bash.exe" -lc "/c/Users/Administrator/WorkBuddy/xiaozhiLed/toolchain/build-firmware-c3.sh"
```

- 首次/配置改动后需 reconfigure（脚本已内置：`build-c3/build.ninja` 不存在时自动 `-B build-c3 -DIDF_TARGET=esp32c3 -DSDKCONFIG=sdkconfig.c3 -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.c3 reconfigure`）
- RISC-V 工具链必须 `esp-15.2.0_20251204`（14.2 会报 `unknown cpu 'esp-base'`）；若换过工具链，**先删 `firmware/build-c3`** 再 configure（CMake 缓存编译器绝对路径）

**S3 构建**（回归）

```bash
& "C:\Program Files\Git\bin\bash.exe" -lc "/c/Users/Administrator/WorkBuddy/xiaozhiLed/toolchain/build-firmware.sh"
```

**回退**

- 只回退 C3：删除 `firmware/build-c3/`、`firmware/sdkconfig.c3`、`firmware/sdkconfig.defaults.c3`、`firmware/partitions_c3.csv`、`firmware/main/buttons_adc.cpp`、`firmware/main/battery_cw2017.cpp`，并把 `main/CMakeLists.txt` 的 target 分支还原为固定 `buttons.cpp`/`battery.cpp`。
- 全量回退：`git checkout -- firmware/main firmware/partitions.csv firmware/dependencies.lock`（**注意会丢弃本轮全部改动**，先自行备份）。
- 也可用 `toolchain/backup/` 下的历史备份做对照（历史上曾用它覆盖恢复过整份 `firmware/main`）。

---

## 6. 参考物位置

| 路径 | 用途 |
| --- | --- |
| `C:\Users\Administrator\WorkBuddy\_ref\passport-os-gitee` | 上游仓库只读副本（HEAD b4d9555，MIT）；板级常量、按键、CW2017、LVGL 配置的第一手依据 |
| `.tmp/ai-passport-dump/` | 整片 flash dump（恢复/对照用） |
| `toolchain/backup/` | 历史备份（S3 固件与配置快照） |
| `firmware/build-c3/` | C3 构建产物（`wb-board.bin` / `wb-board.elf` / `partition-table.bin` / `bootloader/bootloader.bin`） |
| `firmware/build/` | S3 构建产物（当前为 16:41 那次，**未包含本轮改动**） |

---

## 7. 既有成果（**S3 已验证，C3 未验证**）

- 待机 50% 背光、任意键唤醒恢复 100%
- 新任务到来时的唤醒逻辑
- WiFi modem sleep
- 待机钟面/眼睛动画（`wb_eyes`）
- ACP（审批）相关修复

以上均属 S3 板上的已验收行为；C3 上**语义相同但零实测**，尤其"待机唤醒 + 背光恢复"在 C3 上走的是 `buttons_adc.cpp` 的 `asleep → wake()` 分支，必须上板确认。

---

## 8. 本轮构建记录（C3，20:07）

```
bootloader.bin  0x52f0  bytes   剩余 0x2d10 (35%)
wb-board.bin    0x259240 bytes  分区 0x7f0000 → 剩余 0x596dc0 (70%)
```

`wb-board.elf` 链接成功，`esptool v5.6.0` 生成 ESP32-C3 image 成功。编译期仅有 LVGL deprecated API 警告（`lv_obj_add_flag`/`lv_obj_clear_flag`，作用于 `ui_board.cpp`，S3 同样存在，不影响功能）。
