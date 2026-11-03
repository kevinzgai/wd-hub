# AI Passport（ESP32-C3）阶段二报告：V2「大字流」界面 + 提示音

> 分支 `dev`；硬件 ESP32-C3（8MB XMC，rev v1.1，MAC `98:C3:77:F4:49:00`，板载 BLE 地址 `98:C3:77:F4:49:02`）。
> 本报告只覆盖 `passport-fw/` 内的改动，**未修改 `firmware/`**。

## 一、结论（先看这里）

1. **V2 六屏全部实现并在真机跑通**：总览（单条 + 6 段进度轨）、审批、提问、待机、离线、低电量；每屏底部都有三键键位栏。截图为 240×320 真机实拍（串口 `p` 命令回读 LVGL 缓冲），见 `docs/evidence/`。
2. **提示音接通**：S3 的 5 段 24kHz 音色被重采样为 16kHz 并打包进本工程 rodata（69,408B），播放缓冲 25,600B（400ms，内部 RAM，最大连续块 61,440B 时分配成功）。日志可见真实播放 `CONNECTED 260ms`、`ATTENTION 700ms`、`KEY`。**但“响度/音色好不好”只有人耳能判定，我在远程听不到，这部分只能算“链路已执行”**。
3. **最终固件启动干净**：`heap failures=0`、`largest=114688`（boot 时）、无 Guru/panic，60s 连续运行正常，主机 `connected=true`，2.4s 一次 notify 稳定。
4. **本轮抓到并修掉两个真实缺陷**（不是 UI 问题，但会直接影响可用性，详见第六节）：
   - `wb_task` 4KB 栈在“帧解析失败”的 `printf` 路径里越界 → **Stack protection fault 崩溃重启**（已改 8KB）。
   - 业务帧在待机中会把挂起的低电提示放出来 → **待机时真的响**（已改为跟随真实电源状态）。
5. 板子确实会进**深度关机**（长按 OK 5s、或电量耗尽保护）。**开机必须按住 UP 约 1 秒**，松太快会重新睡回去（第九节）。

## 二、体积与分区

| 项目 | 数值 |
| --- | --- |
| `wbpasport.bin` | `0x3FEB90` = 4,189,072 B |
| app 分区 | `0x10000 / 0x7F0000`（沿用 factory） |
| 占用 | **50.33%**，剩余 `0x3F1470`（约 50%） |
| `bootloader.bin` | `0x5290` = 21,136 B（0x8000 内剩 35%） |
| 提示音 rodata | 69,408 B（5 段，全部在 flash，不占 RAM） |
| 播放缓冲 | 25,600 B，内部 RAM（**未用 PSRAM**） |
| `wb_task` 栈 | 8,192 B（原 4,096 B） |

分区表未改，仍是 `0x10000` 起、8MB 内的 factory 布局。

## 三、字库

- 本轮**没有新增字库文件**（沿用上一轮生成的）：
  - `main/fonts/wb_font_12.c`：5,816 字形（12px，列表/正文）；
  - `main/fonts/wb_font_28.c`：126 字形（28px，状态主字/待机时间）；
  - 16px 沿用底座 `buddy_font_16.c`。
- 六屏上出现的中文（“已完成 / 电量低 · 请尽快充电 / 屏幕已休息 / 按任意键唤醒 / 无人打扰 · 在线 / 长按 OK 关机”等）在截图中**全部正常显示**，未发现方块或掉字。缺字时选的是“补字库”，不是换词。

## 四、六屏逐屏证据

截图在 `passport-fw/docs/evidence/`（真机 240×320，直接从 LVGL 帧缓冲回读，非设计稿截图）。

| 屏 | 证据 | 结论 |
| --- | --- | --- |
| 离线 | 串口 `screen -> offline`（每次上电 3.0s） | 通过 |
| 总览 | [v2-overview.png](file:///c:/Users/Administrator/WorkBuddy/xiaozhiLed/passport-fw/docs/evidence/v2-overview.png) + 日志 `总览 1/3`、UP/DOWN 翻页到 `3/3` | 通过，进度轨同步，无列表残影 |
| 审批 | 上一轮日志 `screen -> approval` | 通过（本轮未重拍图） |
| 提问 | 上一轮日志 `screen -> ask` | 通过（本轮未重拍图） |
| 待机 | [v2-standby.png](file:///c:/Users/Administrator/WorkBuddy/xiaozhiLed/passport-fw/docs/evidence/v2-standby.png) + `screen -> standby` | 通过：28px 时间、日期星期、“屏幕已休息 / 按任意键唤醒”、三键“唤醒” |
| 低电量 | [v2-low-battery.png](file:///c:/Users/Administrator/WorkBuddy/xiaozhiLed/passport-fw/docs/evidence/v2-low-battery.png) + `screen -> low_battery` | 通过：12%、红电池、键位栏“屏蔽 / 返回总览 / 长按 OK 关机” |

> 说明：待机/低电两张截图是在**取证构建**（待机门槛临时 5s、60s 后注入 soc=12）上拍的；最终固件 UI 代码完全相同，只是门槛 180s、电量取真实值。取证后已全部还原并重烧最终版（见第七节）。

## 五、音频验证边界（哪些真响、哪些只到日志）

已执行到“音频任务真的把 PCM 写进 codec”的：

| 触发 | 日志证据 | 时长 |
| --- | --- | --- |
| 新任务/连接 | `播放 CONNECTED: 4160 帧(260ms), 累计 played=1` | 260ms |
| 低电提示 | `播放 ATTENTION: 11201 帧(700ms), 累计 played=2` | 700ms |
| 按键短反馈 | 上一轮串口注入按键时 `播放 KEY ...` | 短音 |

只到日志、未由人耳确认的：

- 音量 20%（与 S3 一致）在实际喇叭上的响度、音色是否合适；
- 按键短音在**干净串口**下没再触发（本轮没人按真键，串口注入会污染证据，所以没做）；
- “待机中不响”是**抑制逻辑的证据**（`电量 12%：低电提示挂起（待机中不播放）`，之后 5 秒内 `played` 不涨），不是“我听过了没响”。

另外：素材是 S3 24kHz 表重采样到 16kHz（低通 + 线性插值），**没有重新合成**，所以音高/时长与 S3 一致，只是采样率减半、频带略窄。ATTENTION 实际 700ms，比需求里说的“最长约 233ms”长；播放是按 400ms 分块流式写，所以更长也能播完。

## 六、本轮修掉的两个真实缺陷（重要）

### 1) `wb_task` 栈溢出导致崩溃重启（已修）

取证时板子每 8–16 秒重启一次，日志：

```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault)
Detected in task "wb_task", SP 低于栈下界约 388 字节
MEPC 落在 _vfprintf_r（由 esp_log 调用）
```

根因：`wb_biz.c` 的 `handle_frame` 里局部变量（`wb_ev_t`）+ newlib `vfprintf`，在“帧解析失败”的 `ESP_LOGW(TAG, "帧解析失败（%uB）：%.80s", ...)` 这条日志上把 4KB 栈用穿。
修复：`WB_TASK_STACK` 4096 → **8192**（其它不变）。修后同场景 85 秒无崩溃。

### 2) 待机中会真的响（已修）

取证日志显示：待机中状态帧到达时，`on_biz_change` 直接调 `wb_audio_set_standby(false)`，把挂起的低电提示放了出来（65.27s 播放，而屏幕到 66.1s 才醒）。
修复：改为 `wb_audio_set_standby(wb_power_is_standby())`，只有真正退出待机才放开音频。修复后日志顺序变为：

```
61.07 电量 12%：低电提示挂起（待机中不播放）   ← 待机中不响
66.08 [SerialCmd] UP press → 退出待机
67.07 唤醒后补报低电量提示
68.60 播放 ATTENTION: 11201 帧(700ms)
```

且 66.08–67.18 之间**没有** `播放 KEY`，即按键唤醒不误响。

## 七、取证构建与还原

临时改动（只为采证，已全部还原并 clean 重建）：

| 文件 | 临时值 | 还原为 |
| --- | --- | --- |
| `main/wb/wb_power.h` | `WB_STANDBY_IDLE_SEC 5` | `180` |
| `main/apps/wb/app_wb.cc` | 60s 后把显示电量压到 12% | 已删除（用真实 CW2017 读数） |
| `main/wb/wb_biz.c` | — | 栈 8192（**保留**，缺陷修复） |
| `main/apps/wb/app_wb.cc` | — | 音频待机时序（**保留**，缺陷修复） |

最终版已全量 clean 编译 + 烧录（`Hash of data verified`），60 秒启动/运行日志干净：`wb_audio 提示音就绪`、`codec 就绪 16000Hz/20%`、`heap failures=0`、无 panic。

## 八、妥协与已知限制

1. **长按 OK 的 2s/5s 关机是底座行为**，V2 键位栏只标注“长按 OK 关机”；2s 时会先弹“继续按住关机 / 松开取消”，5s 才真关。
2. **两套低电阈值并存**：覆盖页 15%（本工程 UI），底座“调暗+提示”10%，关机 3%（连续两次、电压 <3600mV）。即 12% 时用户看到低电页但屏幕不会被底座调暗。
3. **待机门槛 180s** 是设计值；本轮截图用的是 5s 取证构建，所以“180s 自然进入待机”只验证了逻辑、没做长测。
4. **提示音只在“新任务/审批/提问/完成/低电/按键”触发**；待机中一律丢弃（除按键音），这与 S3“待机不放”对齐。
5. **待机时业务帧仍会更新数据**（只是不响、不亮屏），醒来即可看到最新状态。
6. 截图依赖串口 `p` 命令 + 本地解码脚本（在 `.tmp/`，不进仓库），属于测试手段，不是产品功能。
7. 审批/提问两屏本轮没有重拍图，只有上一轮的 `screen -> approval / ask` 日志证据。

## 九、关机 / 开机（可用性，必须知道）

会关机的情况：

| 操作 | 表现 | 日志/文案 |
| --- | --- | --- |
| 长按 **OK ≥ 2s** | 覆盖层提示“继续按住关机 / 松开取消” | `power_hold(1)` |
| 长按 **OK ≥ 5s** | 真关机，屏显“已关机 / 按住上键 1 秒开机” | `Power off` + deep sleep |
| 电量 ≤3% 且电压 <3600mV，连续 2 次（10s 一次）采样 | “电量耗尽，已关机 / 充电后按住上键开机” | `Battery empty` |
| 电量 ≤10%（还没到关机） | 屏幕调暗 + 4s 提示，**不关机** | `Low battery` |

开机：

1. **按住 UP（GPIO0）约 1 秒**；
2. 开机代码会再等约 0.8 秒确认按键仍被按住，**松太早会立刻再睡回去**（日志 `Wake key released early; back to sleep`）；
3. 开机后屏幕会先出离线/总览，等主机 BLE 连上（约 4s）后进入总览。

> 之前那次“板子失联”就是进了这里的深度关机：串口不输出、BLE 不广播、esptool 报 `No serial data received`。这不是死机，是睡死了，按 UP 1 秒即可恢复。

## 十、下次上手要注意

1. 待机是 **180 秒无操作**才进，别按上一轮的 5s 去等。
2. 采日志时**不要往 COM6 写字符**：console 在 USB-Serial/JTAG，任何输入字符都会被 `serial_cmd_task` 当成按键（`u/d/o/[/]` 等），会让提示音乱响、页面乱跳。
3. `esptool` 与串口采集会抢同一个口，抓日志前先复位、DTR/RTS 保持 false。
4. `app_desc` 里的 “Compile time” 不随小改动更新，判断“烧的是不是新版”要看 `wbpasport.bin` 的对象时间戳或日志内容，别只看那行时间。
5. 真实低电（≤3%）会关机，测试电量保护请用电源而不是把电池放空。
6. 提示音音量固定 20%，主机 `beep_vol=0` 时只保留按键音；要静音请从主机侧传 0。

## 十一、复现命令

```powershell
# 编译（干净）
powershell -ExecutionPolicy Bypass -File .\passport-fw\build.ps1 clean
# 烧 COM6
powershell -ExecutionPolicy Bypass -File .\passport-fw\build.ps1 -p COM6 flash
# 复位并抓 N 秒日志（只读，不写串口）
python .tmp\serial_cap.py --out .tmp\run.log --seconds 60 --reset
# 截图（在日志里发 'p'，再用 .tmp\decode_shot.py 还原 PNG）
```

## 十二、仍未做 / 建议下一步

1. 人耳确认 20% 音量的响度与音色（尤其 700ms 的 ATTENTION 会不会太长/太吵）。
2. 真键验证：长按 UP 唤醒不响、审批页长按 OK 返回不发帧、提问页长按 OK 发 skip。
3. 180s 待机的长时间功耗实测（本轮只验证了进入逻辑）。
4. 真实低电（10%~15% 区间）走一遍覆盖页与充电恢复。
5. 主机断连/重连若干轮，确认 `wb_task` 8KB 栈与 notify 队列稳定。
