# xiaozhiLed — WorkBuddy 状态看板

把 WorkBuddy 的工作状态显示在一块 1.54" 的 ESP32-S3 小屏上。

```
WorkBuddy 数据库 ──► 本机服务 :27777 ──WiFi──► ESP32-S3 ──SPI──► ST7789 240×240
   (只读)              每5s轮询               (LVGL)
```

## 快速开始

### 1. 启动服务

**Windows**：双击根目录的 `start-board.bat`，出现黑窗口即已启动。

**macOS / Linux**：

```bash
./start-board.sh
```

首次运行会自动建 `.venv` 并装依赖（走阿里云镜像，PyPI 直连常常 SSL 断流）。

关闭窗口 / `Ctrl+C` = 停止服务。

启动脚本会自动检查依赖、端口占用，有问题会在窗口里说清楚。

## 换 WiFi（手机配网，不用重烧固件）

WiFi 凭据存在板子的 NVS 里，换网络不需要重新编译。

### 什么时候会提示配网

开机连不上（重试 4 次约 16 秒）后，屏幕上出现：

```
        WiFi 未连接
        小米路由器_5G
      长按 BOOT 3 秒
      进入配网模式
     用手机配置 WiFi
```

一次都没配过（NVS 空且没有编译期默认值）的设备，**开机会直接进配网**。

### 配网步骤

1. **长按板上 BOOT 键 3 秒**
   只有**未联网**时才生效。已联网时 BOOT 长按（700ms）仍是调亮度，
   按住多久都不会进配网 —— 配网会中断数据轮询，联网状态下不该被误触。
2. 屏幕切到配网页，显示热点名和配置地址：

   ```
          配网模式
        WorkBuddy-A1B2      ← 热点名（带 MAC 后两字节）
         192.168.4.1        ← 配置页地址
      手机连上面这个热点
        （不需要密码）
   ```

   **热点是开放网络，不设密码** —— 省掉在小屏上念一串密码的麻烦。
   手机连上之后屏幕会立刻变：

   ```
          配网模式
        WorkBuddy-A1B2
         192.168.4.1        ← 变绿，更显眼
         手机已连接
    点开浏览器访问上面的地址
   ```

3. 手机 WiFi 列表里连 `WorkBuddy-XXXX`（无需密码）
4. 浏览器打开 `http://192.168.4.1/`，点扫描 → 选你家 WiFi → 输密码 → 保存
5. 板子自动重启，连上新 WiFi

> 配网页空闲 3 分钟会退出配网 —— 防止误触发把设备卡住。
> 但**只有已经存过凭据**时才重启；一次都没配过的设备会一直等（否则会变成
> "开机→配网→3 分钟→重启→配网"的死循环）。

### 凭据从哪来

```
NVS 里存过  →  用 NVS 的
NVS 没有    →  开机直接进配网
```

**固件里不烧任何默认 WiFi。** 2026-10-27 起取消了编译期兜底：
`config.local.h` 只剩 `BRIDGE_BASE`（服务地址），不再有 `WIFI_SSID` /
`WIFI_PASSWORD`。理由是一套默认密码既换环境就要重烧、又把密码带进了构建产物。

所以新烧的板子第一次开机一定进配网，配过一次之后就记住。

### 实现位置

| 文件 | 内容 |
|---|---|
| `firmware/main/wifi_mgr.h` | 状态机定义与对外接口 |
| `firmware/main/wifi_mgr.cpp` | NVS 凭据、STA 重连、SoftAP、配网 HTTP 服务（含内嵌页面） |

屏幕只认 `wifi_mgr_state()` 的四个状态，判断口径集中在一处，
改连接逻辑不用动 UI。

## 换服务地址（电脑 IP 变了不用重烧）

服务地址（`BRIDGE_BASE`）存在 NVS 里，**换电脑 IP 不用改代码、不用重烧**。

### 什么时候会提示

WiFi 是通的，但连续 5 次（约 30 秒）拉不到服务，屏幕自动显示：

```
       服务连不上
    192.168.0.89:27777      ← 现在用的地址（旧 IP）
      192.168.1.23          ← 配置页地址（绿，设备自己的 IP）
   用浏览器打开上面地址
     进去改服务地址
```

同时设备在**已连上的 WiFi** 上开一个配置页 —— 不用切热点、不用重启、
不中断看板，手机或电脑连同一个 WiFi 就能直接访问。

### 改地址步骤

1. 手机/电脑（和看板同一个 WiFi）浏览器打开屏幕上那个绿色地址
2. 在「服务地址」里填新的 `电脑IP:27777`（不写 `http://` 也认）
3. 点保存 —— **几秒内生效**，屏幕上那块提示会自动消失

拉通一次之后配置页自动关闭；还是连不上就一直开着，随时能改。

忘了地址也没关系：**长按 BOOT 3 秒**可以重新打开配置页（只在"服务连不上"
时生效；联网正常时 BOOT 长按仍是调亮度）。

配网热点那个页面里也有同一栏「服务地址」，第一次配 WiFi 时可以顺手填好。

### 地址从哪来

```
NVS 里存过  →  用 NVS 的
NVS 没有    →  用编译期 BRIDGE_BASE（config.local.h 的兜底值）
```

`configure.py` 现在只负责写这个兜底值 —— 板子配过一次之后就以 NVS 为准了。

### 实现位置

和 WiFi 凭据共用一套 NVS 与同一个 httpd（见 `wifi_mgr.cpp`）：
地址每轮轮询现读，所以改完下一轮就生效；连续失败计数也在这里，
屏幕只认 `wifi_mgr_svr_down()`。

## 跨平台

服务和固件在 Windows / macOS / Linux 上都能跑，差异只在启动方式、串口名和工具链位置：

| | Windows | macOS | Linux |
|---|---|---|---|
| 启动服务 | `start-board.bat` | `./start-board.sh` | `./start-board.sh` |
| Python | `py`（依赖装在这） | `.venv`（脚本自动建） | `.venv`（脚本自动建） |
| 串口 | `COM5` 等 | `/dev/cu.usbmodem*` | `/dev/ttyUSB0` 等 |
| ESP-IDF | `C:\esp\v6.1\esp-idf` | `~/.espressif/v6.1/esp-idf` | 自行安装 |

验证脚本也通用，命令行里传串口名即可：

```bash
python scripts/check_api.py                    # 纯标准库，三平台一致
python scripts/shot_board.py                   # 需 playwright
python scripts/serial_read.py <串口> 30         # 需 pyserial
```

开发工具依赖单独放 `server/requirements-dev.txt`（playwright + pyserial），
服务本身只需要 `server/requirements.txt`。

> 路径处理已做成与平台无关：`server/sources/files.py` 的噪音过滤会把
> `\` 和 `/` 都归一后比对，同一份路径在哪个平台判定结果都一样。

### 仍是单平台的工具（不影响正常使用）

| 文件 | 限制 |
|---|---|
| `tools/idfwrap.py` | Windows 专用（绕 Win 上 IDF 路径长度/环境问题的包装器） |
| `tools/watch_serial.py` | 默认 `COM5`，其他平台传参 |

### 2. 设备上电

插上 USB 线（或接电源）后设备自动连 WiFi、连服务、显示数据，约 5 秒。

按键 **BOOT**（板子上的小按钮）可以循环切换 5 个屏幕：

| 屏 | 内容 |
|---|---|
| 0 | 概览：工作中 / 今日完成 / 自动化 + 会话列表 |
| 1 | 任务：总计 / 已完成 / 今日 / 进行中 |
| 2 | 自动化：已启用 / 今日运行 / 失败 / 待审阅 |
| 3 | 今日：改动文件 / 增删行数 / 涉及项目 |
| 4 | 系统：三环图表（CPU / 内存 / 磁盘）+ 进程 / 运行时长 |

屏数由服务端下发，**改屏数不用重烧固件**。

BOOT 的三种语义（按当时的网络状态自动分流）：

| 操作 | 联网正常 | WiFi 连不上 | 服务连不上 |
|---|---|---|---|
| 短按 | 下一屏 | 下一屏 | 下一屏 |
| 长按 3 秒 | 调亮度（700ms 起循环） | 进配网 | 重开服务地址配置页 |

## 常用地址

| 地址 | 说明 |
|---|---|
| `http://127.0.0.1:27777/` | 网页演示页 |
| `http://127.0.0.1:27777/docs` | OpenAPI 文档 |
| `http://127.0.0.1:27777/status` | 固件端点 |

## 字库（屏幕出现方格时看这里）

板子用三套 LVGL 点阵字库，由 `tools/gen_fonts.py` 生成：

| 字号 | 变量 | 用在哪 | 字符集 |
|---|---|---|---|
| 12px | `font_cn_12` | 元信息、列表值、页脚、配网提示 | 3828 字 |
| 16px | `font_cn_16` | **会话标题**、行标题、配网提示 | 3828 字 |
| 28px | `font_cn_28` | 时钟、大数字、配网标题 | 232 字 |

**28px 故意只给 232 字**：它只显示时钟和两三句提示，喂全量字符集会让它从
约 90KB 涨到 214KB —— 它是三个字库里最贵的，纯浪费 Flash。

12/16px 的 3828 字 = **GB2312 一级常用汉字 3755 个**（覆盖日常用字 99.7%）
＋ 固件源码里的全部中文字符串 ＋ 服务端真实下发内容 ＋ ASCII ＋ 界面符号。

### 屏上出现方格怎么办

方格 = 字库不含那个字。按顺序做：

```bash
# 1. 重新收集字符（会扫源码 + 拉服务端全部接口 + 读数据库历史标题）
python tools/gen_charset.py

# 2. 重新生成字库
python tools/gen_fonts.py

# 3. 重新编译烧录
cd firmware && idf.py -DIDF_TARGET=esp32s3 build && idf.py -p <串口> flash
```

`gen_charset.py` 会把结果写成两份：`tools/charset.txt`（12/16px 用）和
`tools/charset_ui.txt`（28px 用）。

如果还是缺字，说明那个字既不在 GB2312 一级区、也不在任何已扫到数据里。
把它加进 `tools/gen_charset.py` 的 `EXTRA_PUNCT`（标点符号）或直接追加到
`charset.txt` 末尾，再跑第 2 步。

### 字体依赖

`gen_fonts.py` 会自动找系统字体，也可用环境变量指定：

| 平台 | 中文 | 符号 |
|---|---|---|
| Windows | `C:/Windows/Fonts/simhei.ttf` | `seguisym.ttf` |
| macOS | `/System/Library/Fonts/Supplemental/Arial Unicode.ttf` | `Apple Symbols.ttf` |

```bash
LV_FONT_CJK=/path/to/font.ttf LV_FONT_SYM=/path/to/sym.ttf python tools/gen_fonts.py
```

> ⚠ macOS 的 `Hei.ttf`（SIL-Hei，形似黑体）**不能用** —— 缺 OS/2 表，
> `lv_font_conv` 直接报 `os/2 table not found`。用 Arial Unicode。

> ⚠ 彩色 emoji 字体（Apple Color Emoji / Segoe UI Emoji）不支持 ——
> `lv_font_conv` 只能处理单色字形，彩色字体会被渲染成实心黑块。

首次生成前要装 `lv_font_conv`：

```bash
cd tools && npm install --registry=https://registry.npmmirror.com
```

## 自检

服务跑起来后，可以一键回归：

```bash
python scripts/check_api.py     # 端点矩阵 + 固件契约 + 数据源自检 + 详情屏
python scripts/shot_board.py     # 截图演示页，确认真实渲染
```

## 目录

| 目录 | 内容 |
|---|---|
| `server/` | 数据服务（FastAPI，只读 WorkBuddy 数据库） |
| `firmware/` | 看板固件（ESP-IDF + LVGL） |
| `demo-screen/` | 纯屏幕验证（不联网，用来排查硬件） |
| `toolchain/` | 编译烧录脚本（Windows，不进版本库） |
| `docs/plans/` | 需求文档 |
| `tools/` | 字库 / Logo 生成 / submodule 补齐 |
| `scripts/` | 自检与截图 |

## 改固件（一般用不到）

### macOS（已跑通，推荐）

ESP-IDF v6.1 装在 `~/.espressif/v6.1/esp-idf`（EIM 装的，submodule 齐全）。

```bash
export IDF_PATH=~/.espressif/v6.1/esp-idf
source $IDF_PATH/export.sh

cd firmware
idf.py -DIDF_TARGET=esp32s3 build            # 编译
idf.py -p /dev/cu.usbmodem21201 flash        # 烧录
idf.py -p /dev/cu.usbmodem21201 monitor      # 看日志，Ctrl+] 退出
```

串口是 ESP32-S3 原生 USB-Serial/JTAG，设备名形如
`/dev/cu.usbmodem*`（用 `ls /dev/cu.*` 查）。

只想看日志不想烧录：

```bash
python scripts/serial_read.py /dev/cu.usbmodem21201 30
```

### Windows

```bash
toolchain/build-firmware.sh                  # 编译
toolchain/flash-board.sh COM5 460800         # 烧录（设备插在 COM5）
```

### 改 WiFi / 服务地址

```bash
python configure.py -b 192.168.x.x:27777
```

`configure.py` 只负责一件事：把 `BRIDGE_BASE`（服务地址）写进
`firmware/main/config.local.h`（不进版本库），并校验服务是否真的可达。

**WiFi 凭据不再写进固件。** 2026-10-27 起取消了编译期默认值：
板子只认 NVS 里配过的网络，没有就直接进配网。所以换 WiFi 一律走
「手机配网」，不必重烧。

**`BRIDGE_BASE` 只是出厂兜底值。** 同样是 2026-10-27 起，服务地址也存进了
NVS：板子配过一次就以 NVS 为准，换服务端 IP 在配置页改一下即可
（见「换服务地址」一节），不用改这里、也不用重烧。

> ⚠ `configure.py` 写的是 `BRIDGE_BASE` 宏（只给 base，路径由固件自己拼
> `/status` 或 `/screen/<i>`）。固件读的就是 `BRIDGE_BASE` —— 别改成别的名字。

> **编译时别用管道接输出**（`... | tail`）。Git Bash 下会因回压把 ninja 挂死，
> 表现为编译到一半不动、CPU 也不涨。一律重定向到文件：
> `idf.py build > build.log 2>&1`。

## 已知限制

- **温度显示不了**（`--`）：psutil 取不到传感器，属正常。
- **依赖本机开着服务**：服务一停，设备 5s 后显示"离线"，数据保留不清零。
- **服务连不上时配置页一直开着**：连续 5 次拉不到服务后，配置页（HTTP 80）
  会在局域网里一直开到拉通为止，改地址不用按键、不用重启。
  这是刻意的取舍 —— 它和"误触发配网 3 分钟自动退出"不同，地址不对的情况下
  这个页面正是唯一的自救入口。在意的话可以给 `h_index` 加个简单口令。
- **USB 串口工具会干扰 BOOT 按键**：GPIO0 是 ESP32-S3 的启动 strapping 脚，
  板上自动复位电路在 DTR 拉低时会把它带下去，所以开着串口终端时可能
  误判成按键。正常插电（不接串口终端）不受影响；真误进了配网，
  3 分钟没人访问会自动重启退出。
- **配网热点是开放网络（无密码）**：只在配网窗口期（最长 3 分钟）存在，
  不承载数据。若在意这 3 分钟内被旁人连入，可给 `wifi_mgr.cpp` 的
  `ap.ap.authmode` 改回 `WIFI_AUTH_WPA2_PSK` 并设 `ap.ap.password`。
- **字库不含的字会显示成方格**：12/16px 覆盖 GB2312 一级 3755 常用字 +
  源码文案 + 服务端真实数据。真遇到生僻字，按「字库」一节的步骤重跑
  `gen_charset.py` → `gen_fonts.py` 再烧录。
- **国内装 ESP-IDF 要用镜像**：`github.com` 常连不上（git / https 都超时），
  只有 `codeload.github.com`、`api.github.com` 和乐鑫 CDN 能连。
  工具链下载加环境变量 `IDF_GITHUB_ASSETS=dl.espressif.com/github_assets`
  即可走乐鑫 CDN（实测 3MB/s）。
  若只有 GitHub tarball（不含 submodule）而没有 EIM 完整包，
  可用 `tools/fetch_idf_submodules.py` 按 SHA 逐个补 submodule。

