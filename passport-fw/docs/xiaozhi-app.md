# 小智 AI 对话应用（passport-os/main/apps/xiaozhi）改造记录

本文记录把 FoloToy `folo-ai-passport-xiaozhi` 的对话能力并进 Passport OS 之后，
针对「服务跑不通（对话没有返回）」和「对话后不稳定、闪退」两个问题的定位与修复。

硬件：ESP32-C3（400KB SRAM / 8MB Flash），应用启动后可用堆约 **94KB**。
桌面端日志基线：`Initial Free Heap: 95 KB`；进入小智 App 时 `94 KB`。

---

## 一、结论速览

| # | 症状 | 根因 | 修复 |
| --- | --- | --- | --- |
| 1 | 连不上小智后台 / 对话没有返回 | 完全跳过了官方后台的 **OTA 取配置**流程，硬编码 `wss://api.tenclass.net/xiaozhi/v1/` + 字面量 `test-token`，拿不到服务端下发的 `websocket{url,token,version}` | 新增 `xiaozhi_ota.cc`：按官方流程 `POST /xiaozhi/ota/`，解析 `websocket` / `activation`，并把结果落 NVS |
| 2 | 对话没有返回 | `listen`/`abort` 报文**不带 `session_id`**，服务端无法把这一轮挂到会话上 | 解析服务端 `hello.session_id` 并在所有出站报文里回传 |
| 3 | 对话没有返回 | 握手请求缺少 `Protocol-Version` 头；`Client-Id` 用 MAC 冒充（官方要求持久化 UUIDv4） | 补 `Protocol-Version`；首次生成 UUIDv4 并写入 NVS |
| 4 | 对话没有返回（偶发） | 客户端 **DTX 静音抑制**：判定静音就一个包都不发，服务端收不到音频干脆不回应 | `enable_dtx = false`（按下说话场景下 DTX 只有坏处） |
| 5 | 屏幕上看不到回复 | 回复正文被绑到了 `llm` 报文上，而官方 `llm` 只带 `emotion`，正文在 **`tts/sentence_start`** | 正文改绑 `tts/sentence_start`（兼容部分服务端的 `llm.text`） |
| 6 | **闪退（必现）** | 「AI 说话时按 OK 打断」：websocket 任务在 `StartPlayback()`、输入任务在 `StopPlayback()`，两者无锁并发操作同一 codec 句柄与缓冲 → use-after-free → panic 重启 | 音频生命周期只允许控制任务调用；websocket 回调只投事件/写环形缓冲 |
| 7 | 闪退（栈溢出） | Opus 编解码器临时缓冲开在栈上，**编码约 19KB、解码 >6KB**；原先录音任务栈 24KB 尚可，但拆到多个任务后每处都不够 | 录音/播放共用**一条常驻工作线程**（20KB 栈），半双工天然互斥 |
| 8 | 闪退（栈溢出） | OTA 的 mbedTLS 握手 + NVS/SPI-Flash 读取调用链很深，控制任务 6KB 栈溢出 | OTA 拆到用完即删的 `xzg_ota` 临时任务（12KB 栈） |
| 9 | 卡死 | `StopRecording()` 无界自旋等任务退出；录音任务一旦卡在 `portMAX_DELAY` 的 I2S 读里，`esp_websocket_client_stop()` 的 `portMAX_DELAY` 等待也永不返回 | 所有任务等待改为**有界超时**；超时只记日志并继续收尾 |
| 10 | App 卡在「思考中」按 OK 没反应 | 状态机没有超时：服务端不回 `tts` 就永久停在 Thinking | 加思考/回复超时兜底，超时自动 abort 回 Idle |

---

## 二、关键实测数据（真机 COM3 串口日志）

修复过程中值得保留的几个数字，后续改代码时不要踩回去：

| 项目 | 实测值 | 说明 |
| --- | --- | --- |
| Opus **编码**栈需求 | ~19KB | `clt_mdct_forward_c` / `silk_NSQ_c`；8KB 栈溢出错 6040 字节 |
| Opus **解码**栈需求 | >6KB | `quant_all_bands`；4KB 栈溢出错 1320 字节 |
| Opus 编码器**堆**占用 | ~23KB | 每轮开/关，把它还给堆 |
| 服务端下发采样率 | **24000 Hz** | 解码器与 codec 必须按 24kHz 打开，否则音调/语速全错 |
| 服务端回声 `frame_duration` | 跟随客户端 | 客户端报多少，服务端 `hello` 就回多少 |
| 说话时可用堆 | ~14KB | 编码器 + TLS 同时在线，这是最紧的时刻 |
| 回复时可用堆 | ~20KB | 编码器已释放，解码器上线 |

**帧长选择**：客户端按 **60ms**、编码侧固定 **16kHz** 单声道上报。
试过 20ms 与 SILK(VOIP) 编码，服务端完全不回应（已回退），
且帧长对编码栈需求几乎没有影响（CELT 的临时缓冲大小与帧长关系不大）。

**已知的良性日志**：

- `tts state=sentence_end (ignored)` —— 新版服务端会发这个状态，只表示一句念完，
  不影响音频流与状态机，忽略是正确行为。
- `Playback ring full (...), frame dropped` —— 服务端突发下发超过缓冲余量。
  正常路径下不应出现；若频繁出现，可调大 `kPlayRingCap`（代价是等量堆）。
- `WebSocket error, type=0/1` 后自动 `letting it auto-reconnect` —— 网络抖动。
  已在真机验证：断线→录音自动收尾→客户端自动重连→重新握手→继续对话，
  全程不重启、不需要用户干预。


---

## 三、架构：单所有者控制任务

```text
按键任务(3072栈) ──post──┐
                        ├─► xzg_ctl (控制任务, 6KB栈)
websocket 任务(5KB栈) ──┐│     * 唯一的 Protocol::Start/Stop/Trigger* 调用者
   └ 回调只做两件事：    ││     * 唯一的 AudioService Start*/Stop* 调用者
       · 事件写队列 ─────┘│     * 唯一的 ui_* 调用者
       · Opus 塞环形缓冲 ─┘     * 录音帧的发送者（drain_audio_tx）
                                    │
xzg_ota (临时, 12KB栈) ── OTA 的 HTTPS 握手，用完即删
xzg_audio (常驻, 20KB栈) ── 录音编码 / 播放解码（半双工，共用一份栈）
```

要点：

- **websocket 回调里绝不做阻塞工作**（不解码、不写 I2S、不加锁等待）。
  旧实现把 `PlayAudioFrame()`（解码 + 阻塞式 I2S 写）直接放在回调里，
  等于让网络任务替音频干活。
- **`Protocol` 是函数内静态对象，永不析构**，从根上消除「回调访问已销毁对象」。
- **`xzg_audio` 在应用启动时创建**：那时堆新鲜连续（最大块 50KB+），20KB 连续栈
  一定分配得到；等用户按下说话再申请，堆已被 TLS/OTA 打散（最大块只剩 7KB），
  必然失败。
- **切走时 `Deinit()` 把常驻线程和缓冲全部还给堆**：退出小智后
  `free heap` 回到 80KB，不侵占收音机 / Codex 等其它应用。
- **Opus 编码器常驻（2026-09-14 内存修复）**：编码器需要 ~25KB 连续块（实测
  largest=22528/23552 时 open 失败 ret:-7），而会话中后段 largest 只有 4~9KB ——
  用时开/用完关在碎堆下必然失败。现在 `Init()` 时 `EnsureEncoder()` 开好常驻、
  `StopRecording()` 不再关；播放时若解码器开不出来，临时放掉编码器
  （`enc_dropped_for_playback_`），播完 `StopPlayback()/AbortPlayback()` 立即补回。
  解码器收尾未确认 idle 时保留句柄（旧代码置空 = 永久泄漏 ~13KB）。
- **配套的全局内存腾挪**（否则常驻编码器会把 TLS 握手饿死
  `Dynamic Impl: alloc(3778) failed`）：首页 `s_portrait_a8`（14.4KB A8 人像缓冲）
  与开机铃 `s_note`（3.2KB）从 .bss 改为用时分配、用完归还；小机会话内
  free 由 ~30KB 升至 ~48KB。真机验证：3 轮对话录音/播放全通，退出后
  free=99.5KB/largest=51.2KB 无泄漏（`_diag/xz_memfix4.log`）。

---

## 四、文件职责

| 文件 | 职责 |
| --- | --- |
| `app_xiaozhi.cc` | 单所有者控制任务、状态机、超时兜底、按键事件、录音帧发送 |
| `xiaozhi_protocol.{h,cc}` | WebSocket v3：请求头、`hello` 握手、`session_id`、分片重组、报文解析 |
| `xiaozhi_audio.{h,cc}` | ES8311 半双工音频：常驻工作线程、环形缓冲、有界等待、内存闸门 |
| `xiaozhi_ota.{h,cc}` | 官方后台 OTA/激活：换 `websocket{url,token,version}`、显示激活码 |
| `xiaozhi_store.{h,cc}` | NVS 配置读写（命名空间 `xiaozhi`，兼容官方 `websocket`） |
| `xiaozhi_ui.{h,cc}` | LVGL 界面；所有入口在锁内二次校验控件指针 |

---

## 五、配置项

NVS 命名空间 `xiaozhi`（可直接用 `nvs_flash` 工具或后续加串口命令写入）：

| key | 默认 | 说明 |
| --- | --- | --- |
| `ws_url` | 空 | 自建服务端时填 `ws://192.168.x.x:8000/xiaozhi/v1/` |
| `token` | 空 | 自建服务端一般留空；官方后台由 OTA 自动写入 |
| `version` | 3 | 协议版本 |
| `ota_url` | `https://api.tenclass.net/xiaozhi/ota/` | 自建服务端改这里 |
| `client_id` | 首次自动生成 | 持久化 UUIDv4，**不要手改**（换了等于换设备身份） |

优先级：**OTA 下发 > `xiaozhi` 命名空间 > 官方同名 `websocket` 命名空间 > 内置默认**。
缓存 10 分钟；连续 3 次连接失败会重新拉取。

### 官方后台（xiaozhi.me）

设备首次连接会 POST OTA 接口：

- 若返回 `activation{code,message}` → 屏幕显示「待激活 绑定码 XXXXXX」，
  去 xiaozhi.me 后台添加本设备的 MAC 即可，绑定后每 30 秒自动重试，**不需要重启**。
- 若返回 `websocket{url,token,version}` → 已绑定，自动连上开始对话。

设备 MAC 可以在小智 App 界面上直接看到（顶栏下方第一行「设备 MAC: xx:xx:...」）。

### 自建服务端（xiaozhi-esp32-server）

把 NVS 里的 `ota_url` 指向自建服务端的 OTA 地址，或直接写 `ws_url` + `version`。

---

## 六、按键

| 按键 | 状态 | 动作 |
| --- | --- | --- |
| OK 单击 | 未连接 | 立即重试连接 |
| OK 单击 | 待机 | 开始说话 |
| OK 单击 | 聆听中 | 结束说话，上传并等待回复 |
| OK 单击 | 回复中 | 打断（先掐声音再通知服务端） |
| OK 单击 | 思考中 | 忽略（有 25 秒超时兜底） |
| 音量+ / 音量− | 任意 | 音量 ±10% |
| 长按 OK | 任意 | 返回启动器（由全局处理器执行） |
