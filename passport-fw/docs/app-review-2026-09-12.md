# 应用评估：AI 语音 与 AI 设备（2026-09-12）

评估对象（Launcher 中文名 → 代码位置）：

| 板块名 | App ID | 代码 | 规模 |
| --- | --- | --- | --- |
| AI语音 | `APP_ID_VOICE`「随身 AI 语音」 | `main/apps/voice/` | 28 文件 ≈ 4.4k 行 |
| AI设备 | `APP_ID_CODEX`「Codex 陪伴精灵」 | `main/apps/codex/` | 38 文件 ≈ 38.5k 行（其中 `buddy_font_16.c` 占 31k 行，被首页复用） |

---

## 一、AI 语音（apps/voice）

### 1.1 它现在到底是什么

**纯「哑终端 + 外挂电脑」方案**，设备端没有任何 ASR。

```
按住 UP → 播 440Hz 滴声 → 滴声结束才真正开始采集
   → bsp_audio_read 每 100ms 取 3200B(16kHz/16bit/mono)
   → IMA ADPCM 4:1 压成 804B → BLE notify(2M PHY, DLE 251, 分 5 片)
   → 电脑端 companion 做 ASR/LLM/注入
   → 结果下行：transcript / agent.status / agent.approval_request
```

关键证据：
- `audio_streamer.c:405` 编码 ADPCM → `ble_audio_notify_audio`，804B/100ms = 8KB/s
- `app_voice.cc:80-89` 上行 `{"event":"voice.start","audio":"ima_adpcm"}` / `voice.end` / `status{drop}`
- `app_protocol.c:133-136` 下行只回 ≤128B 文本；**文本注入已退役到 PC**（`app_types.h:96`）

### 1.2 必须知道的三个硬伤

**① 离线 100% 不可用，而且 companion 不在仓库里**
`app_state.c:176` 未连接时直接 toast `OFFLINE - PTT blocked`。companion 是
`companion/relay.py` + `companion/adpcm.py`（见 `audio_streamer.c:16`、`adpcm.c:5` 注释），
**本仓库没有**。连系统时钟都要电脑下发 `time.set`。

**② `APP_EV_TICK` 有处理无生产者 → 所有超时失效（功能性 bug）**
`app_state.c:725` 是全仓唯一出现处。后果：
- 20s 息屏不触发
- **30s 转写超时不触发**
- **60s PTT 兜底不触发** —— 按住不放会永远录下去

这条无论选哪个方案都要先修。

**③ 与 xiaozhi 两套音频栈重复造轮子**
两者都 `bsp_audio_read` 16kHz/16bit 单声道、都按键触发无唤醒词；但 voice 走
ADPCM+BLE+电脑 ASR，xiaozhi 走 Opus 60ms + WebSocket + 云端 STT。
codec 生命周期、ring、状态机、按键语义**各写一套，无共享音频服务层**。

**④ 为了用 BLE，进 voice 会停掉 Wi-Fi（直接损害其他应用）**

`app_voice.cc:222` 原文：
```c
// Voice transport is BLE/USB; release Wi-Fi runtime buffers while active.
suspend_wifi_driver();
```
与 codex（`app_codex.cc:135`）是同一个模式。后果：进语音 → Wi-Fi 停 → 天气/时间同步/
上传全断；退出 → Wi-Fi 重连（耗时、耗电、可能失败）。**首页的天气与时钟都依赖 Wi-Fi**，
所以每次用完语音回首页，天气都要等重新连上才能更新。

这是「BLE 选型」的连带伤害，不是 voice 单独的问题。

**⑤ BLE 让整机踩在内存悬崖上（稳定运行的最大威胁）**

`ble_audio.c:186-189` 原文注释：
> `nimble_port_init(controller)` 吃掉 **~44.7KB** heap 后，剩余 largest 仅 **~7KB**，
> 动态栈分配与 host 任务(4096B) 争抢，实测导致 host 任务创建失败 → 不广播、连不上。
> **5120 使 heap 少了 1KB → `ble_hs_start` 分配失败重启循环**

`audio_streamer.c:83-84` 亦记：编码器 + NimBLE controller 后 **heap 只剩 ~3KB**，
所以 worker 栈必须静态化到 `.bss`（动态分配会失败）。同处还留着一句历史结论：
> 「Opus 时代一路加到 16384 仍崩，根因是**堆耗尽**而非栈」

系统对 1KB 量级的内存变动都敏感 → 这是「稳定运行」的头号风险源。

附带：`audio_streamer_peak()` 算了峰值却无人调用 → **录音界面没有波形**；
USB 通道是半成品（`mode.c:13-19` `mode_channel_up(USB)` 恒 false，全仓无 `usb_link`）。
另有「射频腐蚀导致幽灵按键」，`app_state.c:10-72` 为此堆了 5 组 mV 门禁——
根因同样是 BLE 射频，去掉 BLE 这个坑一并消失。

### 1.3 定位更正：它是「无线麦克风」，不是语音助手

> 2026-09-12 用户澄清：诉求是**设备当麦克风 → 电脑端 ASR → 文本粘贴**。
> 追求**极致轻量化**，但要求**各个应用能正常稳定运行**。

这个定位推翻了下面「转投小智 MCP」的初稿结论。定位变了，答案就变了。

### 1.4 先排除两条「看起来更优雅」的路

**① 设备直接被电脑认成 USB 麦克风（零 companion）→ C3 硬件不支持**

乐鑫官方 FAQ 与 esp-iot-solution 外设支持矩阵：

- ESP32-C3 只有 **USB-Serial-JTAG** 外设：「**无法自定义描述符**」、
  「不支持二次开发修改 USB 功能或描述符」
- USB-OTG（能做 USB Audio / HID / MSC）只有 S2 / S3 / P4 有；C3 那一栏是 X

→ **C3 做不了 UAC 麦克风，companion 必须存在。这是硬件定死的。**
（长期若换 ESP32-S3，这条路能让 companion 彻底消失。）

**② 设备端本地 ASR → 不可能**

ESP-SR 在无 PSRAM 的 C3 上只支持 WakeNet9s 唤醒词，**不支持 MultiNet 命令词**，
自由说的连续识别更无从谈起。

**结论**：「设备采音 → 传给电脑 → 电脑 ASR + 粘贴」**本身就是这块硬件上的最优解**。
问题不在「做什么」，而在「**用什么通道传**」。

### 1.5 真正的优化点：把通道从 BLE 换成 Wi-Fi

| 维度 | BLE（现状） | **Wi-Fi UDP（建议）** |
| --- | --- | --- |
| 协议栈内存 | NimBLE controller **~44.7KB**，剩余最大连续块仅 ~7KB | **0** —— lwIP 已在跑（天气/时间/上传都在用） |
| 对其他应用 | **必须 `suspend_wifi_driver()`**（`app_voice.cc:222`），天气/时间/上传全停 | 无需停，各应用共存 |
| 稳定性 | 栈 4096→5120（多 1KB）即 `ble_hs_start` 失败**重启循环** | 无此风险 |
| 带宽 | 8KB/s（2M PHY + DLE 251，mbuf 池背压是真瓶颈） | 局域网 Mbps 级，极富余 |
| 端到端延迟 | 100ms/包节奏 | 更低，但**瓶颈在 ASR 而非传输**，感知改善有限 |
| 射频干扰 | 有（幽灵按键，`app_state.c` 为此堆了 5 组 mV 门禁） | 无 |
| 代码量 | `ble_audio.c` 747 行 + 一整套 BLE 封装 | 一个 UDP socket，几十行 |
| 前提 | 配对即可，不要求同网 | **设备与电脑需在同一局域网** |

**关键洞察：这条通道其实早就建好了。** 首页的头像上传就跑在 Wi-Fi 上
（httpd，真机已验证 `http://192.168.5.4`）。设备 ↔ 电脑的 IP 通路已经存在，
语音只是再加一个 UDP 端口——几乎零新增基础设施。

**代价与应对**：
- 需同局域网：你的场景（人在电脑前说话）天然成立
- 需知道对端 IP：用 UDP 广播 + companion 应答做自动发现；或沿用上传页那种
  「设备开服务、电脑来连」的既有模式
- companion 改接收端：BLE notify → UDP socket，工作量不大

### 1.6 建议：保留什么、改什么

**保留（这些设计是对的，别动）**

- **ADPCM**：`adpcm.h:25-28` 块头 4B + 800B，**单块独立可解** → 丢包只影响一块，
  天然抗 UDP 丢包。比 Opus 更契合「极致轻量化」——历史也验证过：
  `audio_streamer.c:90` 留着原话「Opus 时代一路加到 16384 仍崩，根因是堆耗尽而非栈」
- 按键即按即说的交互
- companion 负责 ASR + 粘贴（硬件决定必须如此）

**改（按性价比排序）**

1. **通道 BLE → Wi-Fi UDP** —— 一次换掉 44.7KB 常驻 + 被迫停 Wi-Fi +
   重启循环风险 + 幽灵按键，**同时命中「轻量化」与「稳定运行」两个目标**
2. 顺带删掉 `suspend_wifi_driver()` 调用
3. 删 `ble_audio.c`(747) + USB 半成品 `mode.c` + 各类死代码
   → 估算 4.4k 行可降到 ~1.5k 行
4. **补 `APP_EV_TICK`**（P0，与换通道互相独立，无论如何都要修）
5. 接上 `audio_streamer_peak()`，录音界面出波形

**关于「物理审批手刹」**：它和「无线麦克风」是两件事
（`app_state.c:585-612`，OK=批准 / UP=拒绝 / 无超时 / 常亮不熄屏）。
你不用就一起砍掉进一步减重；要用就保留——纯状态机，不占额外内存。

> 注：若 `suspend_wifi_driver()` / `resume_wifi_driver()` 不成对
> （异常路径提前 return），Wi-Fi 会永久停摆。换 Wi-Fi 通道后这个隐患自动消失。

---

## 二、AI 设备（apps/codex）

### 2.1 决定性发现：数据链是断的

这不是「功能弱」，是**收上来的数据根本没进状态机**。

```
buddy_ble.c:369   发出 BUDDY_BLE_EVENT_RX_LINE
        ↓
app_codex.cc:112  on_codex_ble_event() 只处理 CONNECTED / DISCONNECTED
        ↓  ✗ 没有 else 分支处理 RX_LINE
buddy_orchestrator_process_rx()   ← 全 main/ 下零调用方
                                    （只有 buddy_orchestrator.c:42 定义
                                      和 .h:29 声明，Grep 已确认）
        ↓  ✗
buddy_protocol_parse() 永不执行 → s_codex_state 在 init 后再无写入
```

**已验证**（`app_codex.cc:112-119`，原文）：
```c
static void on_codex_ble_event(const buddy_ble_event_t *event, void *context) {
    (void)context;
    if (event->type == BUDDY_BLE_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Codex BLE connected");
    } else if (event->type == BUDDY_BLE_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "Codex BLE disconnected");
    }
}
```

**连带后果**：
- `tokens` / `tokens_today` **恒为 0**，与 BLE 是否连接无关
- 任务监听 / 审批弹窗 / 庆祝动效 **永不触发**
- `buddy_app_logic.c`（544 行）纯死代码；`buddy_screenshot.c` 未进 CMakeLists
- `app_codex.cc:190-210` —— **OK / UP / DOWN 三个键全部只播音效**，
  UI 上写的「上键切页 / 下键拒绝 / 确定允许」（`buddy_ui.c:379`）未实现
- Home 提示语恒显示「请在电脑连接 Codex Buddy」（`buddy_ui.c:322-323`）

**对首页的直接影响（比想象中糟）**：
`s_usage_ready` 在 `codex_app_start()` 后置位（`app_codex.cc:164`），之后**不再复位**。
所以用户只要进过一次 codex app，首页那两张统计卡就从 `--` 变成**永久显示 `0`**——
看着像「有数据但是 0」，比 `--` 更具误导性。

### 2.2 内嵌到首页的风险

| 风险 | 说明 | 级别 |
| --- | --- | --- |
| **Wi-Fi 与 BLE 架构冲突** | `app_codex.cc:135` `suspend_wifi_driver()`——codex 启动就停 Wi-Fi 给 38KB canvas 腾地方。内嵌首页 = **要么永久断 Wi-Fi**（天气/时间/上传全废），**要么 38KB + Wi-Fi buffer 共存大概率 OOM** | 🔴 架构级死结 |
| **常驻 42KB+** | 38KB I4 canvas（`buddy_ui.c:18,505`）+ 6KB 任务栈 + 4KB NimBLE host 栈。从「按需占用」变「永久占用」 | 🔴 |
| **CPU 常驻** | 5fps 全屏 38KB memset + 逐像素重绘（`buddy_ui.c:485,558`），与首页刷新抢锁、抢单核 | 🟠 |
| **收益为零** | 搬过去的只是「永远 0 的积分 + 一只 BSOD 精灵」，得不到任何新能力 | 🔴 |

**结论：不要内嵌。** 首页已经通过 `codex_app_get_usage()` 拿到了 codex 唯一真实可售的
价值位（两张统计卡），再往下搬就是把「空的」搬上去，还要付出 Wi-Fi 常断的代价。

### 2.3 建议（分两步）

**第一步（立刻，1~2 小时）：修数据链，让首页那两张卡有意义**

在 `on_codex_ble_event()` 补 `RX_LINE` 分支，转调 `buddy_orchestrator_process_rx()`。
然后**顺手修 `s_usage_ready`**：目前它一旦置位永不复位，会导致首页显示误导性的 `0`。
建议改成「BLE 未连接时让 `codex_app_get_usage()` 返回 false」，首页自然回落 `--`。

这是**最小改动、最大收益**的一步。

**第二步（产品决策，二选一）**

- **A. 保留板块**：如果你想留那只像素精灵当桌搭玩具，就留着，但别内嵌
- **B. 删掉板块**：保留 `codex_app_get_usage()` 作为数据源，删掉
  `buddy_ble` / `state` / `ui` / `orchestrator` / `app_logic` / `screenshot`。
  Launcher 从 6 卡变 5 卡

> ⚠️ **删之前必须确认**：`buddy_font_16.c`（31,044 行）是**首页的中文字库**，
> 首页所有中文（姓名/签名/天气/提示）全靠它。删 codex 目录时**绝不能一起删**。

**顺带修**：`codex_app_stop()` 不释放 `buddy_sound` 任务（4096B 栈，无 deinit，
stop 后永久常驻）+ `buddy_settings.c:318` NVS handle 从不 `nvs_close`
→ **反复进出 codex 会泄漏**。

---

## 三、行动优先级

| 优先级 | 动作 | 预估 |
| --- | --- | --- |
| P0 | 补 `APP_EV_TICK`（voice 所有超时失效，按住不放会永远录） | 小 |
| P0 | 补 `on_codex_ble_event` 的 `RX_LINE` 分发 + 修 `s_usage_ready` 语义 | 1~2h |
| **P1** | **voice 通道 BLE → Wi-Fi UDP**（省 44.7KB、不再停 Wi-Fi、消除重启循环与幽灵按键） | 中 |
| P1 | 修 codex 的 sound 任务 / NVS handle 泄漏 | 小 |
| P2 | 录音界面接 `audio_streamer_peak()` 出波形 | 小 |
| P2 | 删 BLE 栈 / USB 半成品 / 死代码（voice 4.4k → ~1.5k 行） | 中 |
| P2 | codex 板块去留决策（删 or 留） | ✅ 已决策：**移除**，见第五节 |
| — | 审批手刹去留（与「无线麦克风」无关，可砍） | 需决策 |

---

## 四、附：本次评估的方法与证据来源

- voice：通读 `main/apps/voice/` 全部 28 文件，重点 `app_state.c` /
  `audio_streamer.c` / `ble_audio.c` / `app_protocol.c` / `app_voice.cc`
- codex：通读 `main/apps/codex/`，关键结论均经 Grep 复核
  （`buddy_orchestrator_process_rx` 全仓零调用方已二次确认）
- 外部方案（2026-09-12 联网核实）：
  - **ESP32-C3 只有 USB-Serial-JTAG，无法自定义描述符，做不了 USB UAC 麦克风**
    （乐鑫官方 FAQ + esp-iot-solution 外设矩阵；USB-OTG 仅 S2/S3/P4 有）
    → 「设备被电脑直认成麦克风、零 companion」这条路在 C3 上硬件不通
  - **ESP-SR 在 C3(无 PSRAM) 只支持 WakeNet9s 唤醒词，不支持 MultiNet 命令词**
    → 本地 ASR 路线不成立

---

## 五、AI设备 黑屏定位与移除（2026-09-12 执行）

用户报「打开 AI设备 后是黑屏页面」。定位后确认该模块在本项目里**从未在真机上跑通过**，
且其价值需要一台 PC 端 Codex 桥，当前不需要 → 按用户授权**整体移除**。

### 5.1 排查过程与证据

| 步骤 | 手段 | 结果 |
| --- | --- | --- |
| 1 | Grep 全部历史日志找 codex 痕迹 | **零命中** —— 该模块从建仓起没被真机走过一次 |
| 2 | 加诊断日志（canvas 分配 / 堆 / LVGL 锁）→ 编译烧录 → 串口 `'4'` 进应用 | `canvas 38468 bytes OK`、`ui_init done; screen loaded`，**无 panic、无重启** |
| 3 | 加画布像素直方图（每 2s 统计 16 个调色板索引的像素数） | `nonzero=4595`（ink=608 / dim=1583 / line=428 / orange=111），`active=1` |
| 4 | 读 LVGL 9 源码对账 | **根因**：见下 |

结论：**画布内容是对的、codex 屏幕也确实是当前活动屏，但物理屏只有底色 `0x080A0C`（近黑）**
—— 所以用户看到的就是一页黑屏。

### 5.2 根因

`main/apps/codex/buddy_ui.c` 用 `lv_canvas` + `LV_COLOR_FORMAT_I4`（4bpp 索引色）当整屏
帧缓冲（为了省 RAM：I4 只要 38KB，RGB565 要 150KB，而进入 codex 时只剩 ~124KB 空闲）。

但 **LVGL 9 的软件渲染器不支持把 I4 源图混到 RGB565 目标**：

- `src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c:444-451` 的 image blend 分支里，
  索引格式**只有 `case LV_COLOR_FORMAT_I1`**（受 `LV_DRAW_SW_SUPPORT_I1` 控制），
  I2/I4/I8 全部落到 `default: LV_LOG_WARN("Not supported source color format")` → 一个像素都不画。
- 其它目标格式（`lv_draw_sw_blend_to_rgb888/argb8888/l8/a8/al88`）同样只有 I1 分支。
- 本项目 `sdkconfig` 里 `CONFIG_LV_DRAW_SW_SUPPORT_I1=y`，而 LVGL 的 Kconfig
  **根本没有 I2/I4/I8 的开关** —— 也就是说这不是配置漏开，是 LVGL 这个版本的能力边界。

顺带解释了为什么日志里一条警告都没有：`LV_LOG_LEVEL` 没开到 WARN。

### 5.3 为什么不"修"而是移除

修 I4 渲染没有便宜的解法：

- 换 RGB565 整屏 canvas → 需 150KB，**内存不够**（进 codex 时只剩 ~124KB）。
- 绕开 LVGL、用行缓冲直接推 `esp_lcd_panel_draw_bitmap()` → 相当于把 codex 做成
  LVGL 框架外的特例应用，要接管面板与过渡屏机制，是重构不是修复。
- 即使修好显示，该应用**仍然不可用**：它的数据/命令链只接通了一半
  （`buddy_orchestrator_process_rx` 至今零调用方，三键只播音效），
  真正的功能依赖 PC 端 Codex 桥，而用户明确当前不需要。

代价对照：它占 38KB canvas + ~45KB NimBLE controller，并且**启动就 `suspend_wifi_driver()`**
把 Wi-Fi 关掉（天气/时间/上传全断）。

### 5.4 实际执行

- **保命**：`buddy_font_16.c`（4360 字中文字库，首页+收音机共用）**迁移**到
  `main/fonts/buddy_font_16.c`，声明统一进 `main/fonts/app_fonts.h`。
- **备份**：整个 codex 目录（38 文件）备份到
  `F:\WORK\AI硬件\_diag\removed-codex-backup-2026-09-12\codex\`（同时 git 历史里也有）。
- **构建**：`main/CMakeLists.txt` 去掉 17 个 codex 源文件、`apps/codex` INCLUDE、
  以及 codex 专用的 `-Wl,--wrap=ble_att_svr_register` 链接选项（wrapper 定义在 `buddy_ble.c`）。
- **注册**：`app_registry.cc` 去掉 `g_codex_app` 与其 extern；**新增未知 app id 守卫**
  （目标 app 不存在时直接拒绝切换 —— 否则会卡死在过渡屏，就是黑屏）。
- **枚举**：`app_base.h` 删 `APP_ID_CODEX` 并重排（WIFI=4 / SETTINGS=5 / HOME=6）；
  `main.cc` 串口键位 `'1'-'7'` → `'1'-'6'`。
- **Launcher**：去掉「AI设备」卡名与图标两个 case。
- **首页**：`codex_app_get_usage()` 调用摘掉，两张统计卡（今日/累计）恒显 `--`
  （视觉与移除前一致 —— 之前没连过桥也是 `--`）；`update_codex_labels` 更名 `update_stat_labels`。
- **删除**：`main/apps/codex/` 整个目录。

### 5.5 真机验证（`_diag/after_remove.log`）

| 项 | 移除前 | 移除后 |
| --- | --- | --- |
| Launcher 卡片 | 6 | **5** |
| 开机落首页 | `Switching app: 0 -> 7` | `0 -> 6`（枚举重排） |
| 开屏后空余堆 | 132 KB | **141 KB**（+.bss 回收 ≈ +9KB） |
| 退出首页后空余堆 | 146 KB | **155 KB** |
| 串口 `'4'` | → Codex（黑屏） | → 无线网络（正确） |
| codex/BLE 日志 | 一屏 BLE_INIT/NimBLE | **零命中** |
| panic / 重启 | 无 | 无 |

固件体积 3,161,360 → **3,108,096 字节（-52KB）**，app 分区剩余 26%。

### 5.6 顺带发现并修复：语音退出必崩（预存 bug，与 codex 无关）

移除 codex 后跑全应用回归时发现：**进入「AI语音」再退出，100% panic 重启**
（`Load access fault`）。历史日志里 `Switching app: 2 -> 3`、`Stopping app: 随身 AI 语音`
从未出现 —— 这条路径以前从没被测过。

崩溃地址经 `riscv32-esp-elf-addr2line` 反查，调用链为：

```
app_registry_switch_to (app_registry.cc:141)
 → voice_app_stop (app_voice.cc:292)
  → app_ui_deinit (app_ui.c:412) → app_ui_hide (app_ui.c:407)
   → lv_screen_load → lv_obj_add_flag → lv_obj_invalidate
    → lv_obj_tree_walk → lv_obj_get_display → lv_obj_get_parent   ← 崩在这
```

**根因**：`app_ui.c` 的 `app_ui_show()` 把 `lv_screen_active()`（此刻是 app_registry 的
**过渡屏**）缓存进 `s_previous_screen`，而 `release_transition_screen()` 在切换完成后
就把它删了 → 离开语音时 `app_ui_hide()` 用悬空指针 `lv_screen_load()` → 解引用已释放内存。
而"恢复上一屏"本来就多余：`switch_to()` 在 `stop()` 之前已加载过渡屏，之后又会启动下一个
App / 回 Launcher，屏幕归属由 registry 统一管。

**修法**：`app_ui.c` 删掉 `s_previous_screen` 与其全部恢复逻辑，`app_ui_hide()` 只隐藏自己的
chrome。全仓只有 voice 有这种写法。

**验证**：`_diag/voice_exit_test.log`（修前）→ 必 panic；`_diag/voice_fix_verify.log`（修后）
→ 连进连出语音两次均 `Voice App cleanly stopped. Free heap: 105 KB`，零 panic、零重启、
堆稳定；`_diag/full_sweep.log` → 五个应用顺序进出 + 长按 OK 回首页全通。

### 5.7 如果将来要恢复

1. 从 `_diag/removed-codex-backup-2026-09-12/codex/` 或 git 历史取回源码（注意把
   `buddy_font_16.c` 留在 `main/fonts/`，不要再放回 codex 目录）。
2. **先解决 I4 渲染**：把 `buddy_ui.c` 的 canvas 换成 LVGL 能混的格式，或改成
   行缓冲直推面板。`buddy_i4.c` / `buddy_sprite.c` 是 I4 索引绘制层，可保留复用，只换最后一步输出。
3. 再补齐 `buddy_orchestrator_process_rx()` 的接线，否则依然只是个会动的黑屏宠物。

  - 小智 AI v2 能力矩阵（ESP-SR 唤醒 / Opus / 流式 ASR+LLM+TTS / 声纹 /
    设备端 MCP / 云端 MCP 含 PC 桌面操作 / 支持 ESP32-C3）
    —— 注：这适用于「语音助手」定位；本次用户澄清为「无线麦克风」定位后，
    小智方案不再是 voice 的替代方向（已改写入 1.3~1.6）

---

## 六、AI语音 通道改造：BLE → Wi-Fi UDP（2026-09-12 执行）

方向由用户拍板：**通道重写为 Wi-Fi UDP**，且 **BLE 完全替换（不留回退）**。
完整协议契约见 [`voice-udp-channel.md`](./voice-udp-channel.md)；此处只记「做了什么 + 实测 + 边界」。

### 6.1 一句话

进语音不再杀 Wi-Fi；上行音频/事件从 NimBLE notify 换成 UDP 单播；NimBLE 整层退出链接。

### 6.2 协议（设备端与 companion 共同遵守，改一侧必须同步另一侧）

| type | 方向 | payload | 总长（含 1B type） |
| --- | --- | --- | --- |
| `0x01` AUDIO | 设备→电脑 | `[seq][0x80]` + 804B IMA ADPCM block | 807B |
| `0x02` EVENT | 双向 | UTF-8 JSON 行（**含结尾 `\n`**） | ≤ 513B |
| `0x03` BEACON | 电脑→设备 | `{"v":1,"role":"pc","port":33333}`，每 1s 广播 | ~40B |
| `0x04` PING | 设备→电脑 | 空，每 2s 一发，**只判活**（不派发到 relay） | 1B |

- 端口 **33333**；companion = 服务端（bind + 广播 beacon），设备 = 客户端（bind + 从 beacon 学电脑 IP）。
- `link_up` = 已学到对端 IP **且** 距最近一次收到对端报文 < 3000ms。
- **AUDIO 保留 BLE 时代的 2B 分片帧头**（UDP 下退化为 `[seq][0x80]`）—— 让 companion 的
  `reassemble_adpcm()` 一行都不用改；EVENT 同样带 `\n`，`reassemble_event()` 也零改动。

### 6.3 改了什么

**设备端（`passport-os`）**

| 文件 | 改动 |
| --- | --- |
| `main/apps/voice/udp_audio.c` / `.h` | **新增**（348 行）：socket + RX 任务 + beacon 学习 + PING 判活 |
| `main/apps/voice/mode.c` | 重写为单通道转发（原 BLE/USB 双通道 → 只剩 UDP；USB 半成品分支删除） |
| `main/apps/voice/app_voice.cc` | 删 `suspend_wifi_driver()` / `resume_wifi_driver()`；`ble_audio_*` → `udp_audio_*` |
| `main/apps/voice/app_state.c` / `app_ui.c` | 显示名 "BLE" → "WiFi" |
| `main/CMakeLists.txt` | `ble_audio.c` → `udp_audio.c`；REQUIRES 去掉 `bt` |
| `main/apps/voice/ble_audio.c` / `.h` | **删除**（747 行；备份见 §6.7） |

**companion（`folo-ai-passport-voice/companion/`）**

| 文件 | 改动 |
| --- | --- |
| `udp_transport.py` | **新增**：`UdpTransport`，实现既有 5 方法契约（scan/connect/write/notify/disconnect）+ beacon 循环 + 读线程 |
| `relay.py` | `_build_transport()` 增加 `channel == "wifi"` 分支 |
| `fre_state.py` | `VALID_CHANNELS` 加 `"wifi"`；删掉「WiFi 通道已移除」的旧报错文案 |
| `tests/test_udp_transport.py` | **新增** 13 条单测（含与 `reassemble_adpcm` 的跨模块契约、真 socket 回环） |

`relay.py` 核心与 `adpcm.py` / `asr_client.py` / `inject.py` **一行未改** —— 5 方法契约吸收了一切差异。

### 6.4 实测结果

| 项 | 改造前 | 改造后 |
| --- | --- | --- |
| 固件体积 | 3,108,096 B | **2,870,416 B（−232 KB）** |
| app 分区剩余 | 26% | **32%** |
| ELF 里的 NimBLE 符号 | 存在 | **零**（`nm` 查不到 `ble_audio*` / `nimble_port*` / `ble_gap_*` / `ble_gatts*`） |
| 进语音时的 Wi-Fi | `StopStation()` + `esp_wifi_deinit()` | **完全不动** |
| 传输层代码 | `ble_audio.c` 747 行 | `udp_audio.c` 348 行 |

编译 `BUILD_EXIT=0`；真机 `_diag/udp_voice.log`：进/出语音干净 —— `UDP RX task started (port 33333)`
→ `UDP 通道就绪` → `UDP 通道已关闭`，Wi-Fi 全程在（无 `esp_wifi_deinit`）、无 NimBLE 日志，
退出后 `Free heap: 131 KB`，无 panic。companion 单测 `13 passed`（`_diag/pytest_udp.log`）。

### 6.5 验证边界（未跑通的部分，需知悉）

**设备→电脑的实时音频 e2e 未在本机跑通**，原因是网络拓扑而非代码：设备已连 `ZTE-2302-5G`、
拿到 `sta ip: 192.168.5.4`，而本机唯一可路由地址是 `192.168.1.3`（以太网），Wi-Fi 网卡均为
「已断开连接」；`Test-Connection 192.168.5.4` 返回 False。

→ 复现 e2e 只需：在一台**与设备同网段**的电脑上，把 companion 的 `channel` 设为 `wifi`
（`udp_port` 留空即默认 33333）后跑 `relay.py`；首次运行需在防火墙弹窗放行入站 UDP 33333。

### 6.6 遗留风险（沿用 `voice-udp-channel.md` §8）

1. 需同子网；路由器若开 **AP 隔离（client isolation）** 则 beacon 到不了设备 → 永远 OFFLINE。
2. **无加密无鉴权**（本次唯一安全回退）：同网段任意主机可伪造 beacon / 下行 JSON。
3. 单设备假设（端口固定 33333，同网关两套设备会冲突）。
4. Wi-Fi 常驻 + LVGL + 采集并存的堆余量已实测（退出后 131 KB），但**连续长时间录音**未压测。

### 6.7 补充清理（本次复核发现）

设计文档 §7 计划移除 `ble_audio.c/.h`，但复核时发现这两个文件**仍在工作区**（git 显示 `M`，
即被改过但未删）。确认它们**未进 `main/CMakeLists.txt` 的 SRCS、也无任何 `#include "ble_audio.h"`**
（全仓仅剩注释提及）→ 属孤立死代码，已删除。

- 备份：`_diag/removed-ble-backup-2026-09-12/`（`ble_audio.c` 747 行 + `ble_audio.h` 128 行，完整）。
- git 历史同样可恢复（`git show HEAD:main/apps/voice/ble_audio.c`）。
- 删除后重新编译校验（`_diag/build_udp2.log`）→ **构建不受影响**（这两个文件本就没参与编译）。
