# AI Passport（ESP32-C3）独立固件 · 阶段一交付报告

> 生成日期：2026-11-03 · 分支 `dev` · 底座 `passport-os` Gitee `b4d9555`

## 一、结论

- **工程已建成**：新增独立顶层目录 `passport-fw/`，自带 `CMakeLists.txt`、`sdkconfig`、`sdkconfig.defaults`、`partitions.csv` 与 Windows 构建入口 `build.ps1`，不引用、不合并 `firmware/`（S3 小智盒子）的任何源码。
- **能编译**：ESP-IDF **v5.5.3**、target `esp32c3` 全量构建通过。产物 `build/wbpasport.bin` 为 **0x3b51e0 = 3,887,584 字节（3.71 MiB）**，占 `factory` 分区 `0x10000/0x7f0000` 的 **46.7%**，剩余 **53%**。
- **已上板**：先用 `esptool chip_id` 确认 COM6 是 ESP32-C3（USB MAC `98:C3:77:F4:49:00`，与任务给定一致），随后烧录并冒烟。**没有碰 S3 小智盒子的板子**。
- **主链路已实测**（串口 + 主机侧联合取证）：开机 -> BLE 广播 `Workbuddy BOX` -> 主机 `host/wb_app.py` 自动连接 -> `hello` 握手成功（主机日志 `fw=1.3.0-passport proto=2 caps=0x3`）-> `cfg` 的 `OK_CFG` 回执被主机解析 -> 状态/积分帧无 `ERR_JSON` -> 空闲进入待机（背光 50%、BLE 连接间隔 247ms、LVGL 刷新 33→66ms）-> 待机中下发审批后**自动唤醒**（背光 100%、刷新恢复 33ms、连接间隔升回约 45ms）。
- **未实测**：物理三键、审批/提问的实际作答回传、最终视觉稿、功耗电流。详见“已验证 / 未验证”。

## 二、目录与关键文件

```
passport-fw/
├─ CMakeLists.txt            # 工程入口，project(wbpasport)
├─ partitions.csv            # 8MB 原厂布局：nvs/phy_init/factory(0x10000,0x7f0000)
├─ sdkconfig / sdkconfig.defaults
├─ build.ps1 / build.sh      # Windows / Linux 构建入口
├─ FORK-NOTICE.md            # fork 出处、提交、许可证说明
├─ LICENSE                   # 底座 MIT
├─ main/
│  ├─ main.cc                # 启动流程：纯 BLE、跳过 Wi-Fi 配网与开机音，直接进 WB
│  ├─ CMakeLists.txt         # 加入 wb/ 与 apps/wb/，REQUIRES 增加 bt
│  ├─ apps/
│  │  ├─ app_base.h          # APP_ID_WB（唯一改动：枚举插入 APP_ID_COUNT 之前）
│  │  └─ wb/
│  │     ├─ app_wb.h/.cc     # 应用壳：1s tick、按键路由、握手/电量上行
│  ├─ launcher/
│  │  └─ app_registry.cc     # 只加一行按 id 寻址（WB 不进 Launcher 轮播）
│  └─ wb/
│     ├─ wb_proto.h/.c       # WB JSON 协议：解码 + 编码 + ACK（独立实现）
│     ├─ wb_link.h/.c        # NimBLE GATT 外设、分帧、切片发送、连接间隔档位
│     ├─ wb_biz.h/.c         # 业务状态机：会话/审批/提问/页面/唤醒位（纯数据，不碰 LVGL）
│     ├─ wb_power.h/.c       # 180s 待机、背光 50%、BLE 省电档、帧率减半
│     └─ wb_ui.h/.cc         # 占位视觉层：全部坐标/字号/颜色集中在此文件
├─ components/ …             # 底座组件原样保留（bsp、音频、Muse 等）
└─ main/apps/ …              # 底座其余 8 款应用原样保留（先保证可编可跑）
```

分层约定：

- `wb_proto` / `wb_link` / `wb_biz` **不包含任何 LVGL 头文件**，只产出只读快照 `wb_view_t`；
- `wb_ui.cc` 是唯一允许出现坐标、字号、颜色的文件，设计稿定稿后只替换它；
- `wb_power` 负责时间与电源策略，LVGL 相关动作统一收敛到 1s tick（LVGL 上下文）。

## 三、与底座的差异

| 项 | 底座 passport-os | 本工程 |
|---|---|---|
| 启动 | Wi-Fi Manager + 首次配网 + 首页/Launcher | 不初始化 Wi-Fi，不开配网，直接 `APP_ID_WB` |
| 开机音 | `boot_chime_start()` 占用 codec | 跳过（无音频使用需求） |
| home_profile | 后台挂载 SPIFFS（依赖 storage 分区） | 跳过（本机分区表无 storage，头像/名片降级不可用） |
| 熄屏 | 60s 熄屏（app_registry） | `set_screen_timeout(0)`，改由 WB 自己 180s 进待机（只降亮不熄屏） |
| 分区 | 仓库自带 4MB 布局 | 原厂 8MB：`factory 0x10000/0x7f0000` |
| 工程名 | `passport-os` | `wbpasport`（产物 `wbpasport.bin`） |
| 新增依赖 | — | `bt`（NimBLE），其余沿用底座 |
| 其余应用 | 8 款 | **一个未删**，仅业务上不再可达（Launcher 不存在），后续按需瘦身 |

## 四、协议复用方式

在 `wb_link.c` / `wb_proto.c` 内**重新实现**，不共享 `firmware/` 头文件；只读参考了 S3 的 UUID 与帧语义。

- GATT：Service `4fafc201-1fb5-459e-8fcc-c5c9c331914b`，Characteristic `beb5483e-36e1-4688-b7f5-ea07361b26a8`，设备名 `Workbuddy BOX`。
- 下行：`status_list`（会话数/名称/状态/模型/消息 + `t` 墙钟秒）、`approval`（id/tool/hint）、`ask`（id/ans/multi/text/opts）、`ask_end`、`cfg`、`msg`、无 `type` 的 `voice`/`usage`/`remain`。
- 上行：`key`（`approval_id` + `ok`/`down`/`skip`）、`ask_answer`（`id` + `index`）、`battery`（`soc`）、`hello`（`fw`/`proto`/`caps`）。
- ACK：`OK_STATUS`、`OK_APPROVAL`、`OK_ASK`、`OK_ASK_END`、`OK_WB`、`OK_MSG`、`OK_CFG:<vol>`；解析失败 `ERR_JSON`。**所有 ACK 都带 `\n`**——主机按行切分，裸文本不带换行会和下一帧粘连（本次实现已按此修正）。
- `caps = 3`（审批页 + 提问页）；未声明语音上行（bit5），与 S3 看板一致。
- 分帧：按花括号配平 + 换行双判据，兼容同一次 notify 里多帧；发送按 MTU 切片并加互斥，帧内不会被其它帧插入。

## 五、分区与体积

| 分区 | 类型 | 偏移 | 长度 |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x6000 |
| phy_init | data/phy | 0xf000 | 0x1000 |
| factory | app/factory | 0x10000 | **0x7f0000（8,323,072 B）** |

- `wbpasport.bin`：**0x3b51e0 = 3,887,584 B（3.71 MiB）**
- 分区占用：**46.7%**，剩余 **0x43ae20 = 4,431,392 B（53%）**
- bootloader：0x5290（21,136 B），保留 35%
- 静态内存：DRAM 170,504 / 321,296 B（53%），运行期 BLE 初始化后主机侧日志显示应用可用堆约 **90 KB**。

## 六、编译与烧录

依赖：ESP-IDF **v5.5.3**，已安装 `esp32c3` 工具链（riscv32-esp-elf `esp-14.2.0_20251107`）与 5.5 Python 环境；`build.ps1` 自行拼 PATH，不依赖 `export.ps1`（后者会检查未安装的其它 target）。

```powershell
cd passport-fw
powershell -ExecutionPolicy Bypass -File .\build.ps1 build
powershell -ExecutionPolicy Bypass -File .\build.ps1 size
powershell -ExecutionPolicy Bypass -File .\build.ps1 flash      # 默认 COM6，可用 -Port 覆盖
```

等价的 IDF 命令：`idf.py set-target esp32c3 && idf.py build`（需先使能 5.5 环境）。

烧录前确认芯片：

```powershell
python -m esptool --port COM6 chip_id   # 期望 ESP32-C3，MAC 98:C3:77:F4:49:00
```

## 七、已验证 / 未验证

### 已验证（有现场证据）

1. **编译**：ESP-IDF 5.5.3 全量构建通过，产物与体积见上。
2. **芯片确认 + 烧录**：COM6 为 ESP32-C3，`write_flash` 成功；启动日志显示分区表为 `nvs/phy_init/factory(0x10000,0x7f0000)`，应用名为 `wbpasport`。
3. **启动链路**：I2C、ST7789 240×320、LVGL、CW2017（520mAh profile）、ADC 三键、app registry 均初始化成功；应用 `App=11`（`APP_ID_WB`）启动，BLE 初始化后可用堆 ≈90 KB。
4. **BLE 广播/连接**：广播名 `Workbuddy BOX`；主机 `host/wb_app.py` 自动连接 `98:C3:77:F4:49:02`。
5. **协议握手**：主机日志 `设备握手：fw=1.3.0-passport proto=2 caps=0x3（审批页、提问页）`，证明 `hello` 上行与 `caps` 解析一致。
6. **ACK 链路**：主机日志 `设备已确认提示音音量 100`，证明 `OK_CFG:<vol>\n` 被正确按行解析；整段会话**没有** `ERR_JSON` 日志，说明 `status_list`、积分（`remain`）等帧解析成功。
7. **电量上行**：主机状态接口显示 `battery: 52`，来自本机 `battery` 帧。
8. **待机进入**：首帧状态到达后 idle 重新计时，串口 `t=188.8s` 出现 `进入待机：背光 50%，BLE 省电档`，随后 `连接参数更新 rc=0 itvl=198（约 247ms）`、`渲染周期 33ms → 66ms`；待机期间仍在接收/回 ACK，说明“有状态帧但不唤醒”符合预期。
9. **待机自动唤醒**：`t=226.0s` 主机下发 `test-approval`，串口同一时刻出现 `退出待机：背光 100%，BLE 正常档`、`连接参数 24-40 rc=0`、`渲染周期恢复 33ms`；Windows 主机先两次拒绝升速（rc=2，仍为 198），第三次重试成功 `itvl=36（约 45ms）`。这同时验证了审批帧解析、唤醒位、背光/帧率恢复与连接参数重试。

### 未验证（不要当成已通过）

1. **物理三键**：本次未按按键；短按 UP/DOWN 选择、OK 确认、长按 DOWN 返回/跳过/唤醒、长按 OK 关机（底座 2s/5s 全局逻辑）均未实按验证。
2. **审批/提问的作答回传**：未实际按 OK 发送 `key` / `ask_answer`；仅验证了帧能被设备接收（主机侧 `test-approval` 发送成功、设备未报错）。
3. **视觉**：只保证占位界面能构建/加载，未拍照核对布局；设计稿未定稿，本就不作为验收项。
4. **断线重连**：未做拔远/关闭主机再连的循环测试；仅验证了 Windows 拒绝升速时的参数重试（见已验证第 9 条）。
5. **功耗电流**：未测量。IDF 启动日志提示“蓝牙开启时 light sleep 不生效”，待机电流主要由 BLE 连接间隔决定。
6. **音频**：完全未初始化/未验证（本阶段不需要）。

## 八、取舍说明（需要用户决策的点）

1. **长按 DOWN 的语义冲突**：底座默认“长按 DOWN = 返回上一级”。本机按场景拆分：
   - 待机：唤醒并吞掉本次按键（避免唤醒后立刻误操作）；
   - 审批页：返回总览，**不发送任何帧**（审批保持待处理，可再次进入）；
   - 提问页：发送 `key(skip)` 跳过，并回总览（与主机约定一致）。
   
   取舍理由：审批是“不可撤销的授权”，误触返回不应等价于拒绝/同意；提问允许跳过更符合主机流程。若设计稿要求“长按 DOWN 一律返回”，只需改 `wb_on_key` 一处。

2. **审批改为“选择 + OK 确认”**，而不是 S3 的“OK=允许 / DOWN=拒绝”。这样三键语义统一（UP/DOWN 只做选择），代价是多一次按键；如果要求一键决策，可在 `wb_biz_confirm` 外再加一个 `wb_biz_quick(allow)`。

3. **长按 OK**：保持底座全局“2s 提示 / 5s 关机”，应用内不占用；`hold_ok=true` 只阻止 registry 把长 OK 当成“回桌面”。因此审批/提问页长 OK 不会误返回。

4. **180s 待机 vs 底座 60s 熄屏**：禁用底座熄屏，改为“常亮 + 50% 背光 + 帧率减半 + BLE 省电档”的待机钟面；不熄屏是为了让时间/在线状态始终可读。若希望进一步省电，可在此策略上叠加“更深一级熄屏”（需另立决策）。

5. **主亮度固定 100%**：本机没有设置页，若不固定，NVS 里的旧亮度（默认 80%）会让“待机 50%”实际只有 40%。启动时强制 master=100，待机 50 才是字面 50%。

6. **共用蓝牙名 `Workbuddy BOX`**：主机按名字扫描，改名会导致现有主机连不上；两台设备同时上电时主机会二选一。若必须区分，可改为名字后缀 + 主机白名单，属于跨端改动，本阶段未动。

7. **无 storage 分区**：沿用原厂 8MB 布局，home_profile 的头像/名片功能自然降级；换来的 7.94 MiB 应用区足够容纳现有字库（`buddy_font_16` 源码 2.21 MB）。

8. **字库**：未裁剪。`buddy_font_16`（4360 汉字）已覆盖本阶段全部文案，先保证可跑；下一轮可按实际文案裁剪，预期能显著缩小 `.rodata`。

## 九、遗留风险与下一步

**风险**

- **会话匹配按数组下标**：`status_list` 若重排，名称/状态可能错位。主机当前按稳定顺序推送，短期可用；下一轮应改用任务 id/会话 key 对齐。
- **堆余量**：BLE 初始化后约 90 KB 可用；占位 UI 很轻，但高保真界面（图片/多屏）可能吃紧，且 C3 无 PSRAM，碎片风险高。需要在设计稿定稿前做一次堆预算。
- **待机功耗**：未实测；BLE light sleep 未开启，247ms 连接间隔是当前唯一手段。
- **同名设备**：见取舍 6。
- **版本号**：`app_init` 仍显示底座版本 `0.2.0-rc.56`；对外协议版本是 `1.3.0-passport`。发布前应统一 `version.txt` / `PROJECT_VER`。

**下一步（建议顺序）**

1. 实按三键回归：短按选择、OK 确认、长按 DOWN 返回/跳过/唤醒、长按 OK 关机；同时验证审批/提问的上行回传（主机侧 `test-approval` / `test-ask`）。
2. 抓一次“待机中收到审批”的串口，确认自动唤醒（背光 100%、帧率恢复、连接间隔升速）。
3. 断线/重连循环（关主机、走远、重启主机），确认广播重启与 Windows 连接参数重试。
4. UI 设计稿定稿后，仅替换 `wb_ui.cc`；在此之前不改业务层接口。
5. 按最终文案裁剪字库、评估删除不可达应用，缩小 `.bin` 与 `.rodata`。
6. 测量待机/激活电流，再决定是否开启 BT modem sleep。

---

### 附：本次使用的关键命令与产物

```text
构建：powershell -ExecutionPolicy Bypass -File .\build.ps1 build
体积：wbpasport.bin 0x3b51e0 / 0x7f0000（53% free）
烧录：powershell -ExecutionPolicy Bypass -File .\build.ps1 flash（COM6）
备份：.tmp/ai-passport-dump/full_8mb.bin（未改动，回退用）
```
