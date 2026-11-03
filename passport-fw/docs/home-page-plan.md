# 首页（个人主页）改造计划与交接文档

> 目标：给 Passport OS 增加"首页"应用，显示用户头像、姓名、个性签名、Codex 额度（积分）、日期、天气。
> 核心约束：ESP32-C3 单核小内存（可用堆约 200~300KB），一切设计以稳定为先。
> 交接状态：底层 5 个模块 + 第 5 节的 5.1~5.5 均已补齐（额度接口、app_home、Launcher 注册、
> CMake、开机进首页）。
> **尚未编译验证**：本机没有 ESP-IDF（历史构建在 WSL 的 `/opt/esp-idf` 完成，且 `wsl.exe`
> 被安全策略拦截，无法代跑）。请在 WSL 中按第 5.6 节验证。
> 与本文档的两处有意偏差记录在[第 7 节](#7-与本文档的偏差留痕)。

---

## 1. 硬件与框架关键事实（接手必读）

| 项 | 值 |
|---|---|
| 项目目录 | `F:/WORK/AI硬件/passport-os`（ESP-IDF ≥ 5.5.0，target=esp32c3） |
| 屏幕 | ST7789P3 240×320 竖屏，LVGL 9，RGB565 |
| Flash/分区 | 8MB：nvs 0x6000 / factory 4MB / cardid(nvs) / **storage(SPIFFS) 2MB @0x420000**（当前固件未用，首页头像将存这里） |
| 应用框架 | `passport_app_t`（init/start/stop/on_key 四钩子），注册于 `main/launcher/app_registry.cc` 的 `s_apps[]` |
| 应用切换 | `app_registry_switch_to()`：先切过渡屏→stop 旧应用（释放内存）→start 新应用。长按 OK 全局回 Launcher |
| Launcher | 卡片轮播 UI；进应用时 `launcher_ui_hide()` 直接删除整个界面省内存。新应用必须遵守"stop 即删屏"模式（参考 `launcher/settings.cc`） |
| Wi-Fi | `WifiManager::GetInstance()`：`IsConnected()/GetIpAddress()/GetRssi()/SetEventCallback()`（`components/esp-wifi-connect`） |
| 颜色字节序 | LVGL 内部小端 RGB565，`esp_lvgl_port` `swap_bytes=true` 输出给 SPI。**网页端头像编码按小端发**（已按此实现） |
| 串口命令 | u/d/o/l = 上/下/确定/长按确定；'1'-'5' = 直接切 App（radio..settings）。APP_ID_HOME=7 |
| 控制台 | USB-Serial-JTAG（GPIO18/19），日志走这里 |

### 中文字体（重要）
- `radio_font`（16px 4bpp）：字形按固定字符串子集化，**只够 Launcher/设置自身 UI 用，不能显示用户自定义文本**。
- `buddy_font_16`（`main/fonts/buddy_font_16.c`，16px 1bpp）：约 4360 个常用汉字 + 全角标点 + ASCII 全集，**首页所有动态文本都用它**。
- 本次已重新生成该字库：补了"圳浏"、全角标点（！？：；—…""''《》～·￥）、ASCII 0x20-0x7E。文件已原位替换（1.3MB 源码，flash 代价极小）。
- **2026-09-12**：该文件原属 `main/apps/codex/`，随 AI设备 模块移除迁到 `main/fonts/`（**迁移而非删除** —— 首页与收音机都依赖它）。声明统一在 `main/fonts/app_fonts.h`。
- 如需再加字，重新生成命令（Node 24 已装，npx 可用）：
  ```
  npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --no-kerning --bpp 1 --size 16 \
    --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf \
    --range 0x20-0x7e --symbols "<原符号串+新字>" --format lvgl \
    --lv-font-name buddy_font_16 -o main/fonts/buddy_font_16.c
  ```
  （原符号串在现有文件头部 Opts 注释里，直接复制追加即可）

### 时间
- `main/apps/voice/time_sync.{c,h}`：时区存 NVS（tz_hour，默认 +8），`time_sync_tz_hour()` 读取。PC 端可通过 BLE/USB 校时（codex/voice 应用）。
- **`time_sync_init()` 目前只有 voice 应用调用**（app_voice.cc:211）。首页模块自己调一次即可（幂等 NVS 读），已写入 app_home 规范。

### Codex 额度数据链（现状，已验证）
- PC 端 `codex-buddy/tools/windows_buddy_controller/background_bridge.py` 心跳带 `tokens` / `tokens_today` 字段（经 BLE 发给设备）。
- 设备端 `buddy_protocol.c` 解析 → `buddy_state.c` 归约 → `app_codex.cc` 的静态 `s_codex_state`。
- `buddy_ui_snapshot_t` 快照里有 `tokens`、`tokens_today` 字段（buddy_types.h）。
- **缺一个对外读取函数**，需在 app_codex 里加（见第 5.1 节）。

---

## 2. 总体设计（小内存策略）

### 数据流
```
手机浏览器（与设备同一 Wi-Fi）
   │  打开 http://<设备IP>（上传模式期间）
   │  ├─ 图片：浏览器端裁剪 64×64 + 编码小端 RGB565 → POST /api/avatar（8192B 裸字节流）
   │  ├─ 城市：浏览器端调 open-meteo 地理编码得经纬度
   │  └─ 姓名/签名/城市/经纬度：POST /api/profile (JSON)
   ▼
设备（首页应用）
   ├─ NVS "home_profile"：name/signature/city/lat/lon + 天气缓存 wtemp/wcode
   ├─ SPIFFS /storage/avatar.bin：8192B 原始帧（tmp 流式写 + 原子改名）
   ├─ SNTP ntp.aliyun.com（仅首页活跃时启动，停止即释放）
   ├─ 天气：GET api.open-meteo.com/v1/forecast?current=temperature_2m,weather_code（30 分钟一次）
   └─ Codex 额度：读 codex 应用静态状态（应用互斥运行，无并发问题）
```

### 内存守则（同事必须保持）
1. **设备端零图片解码**：缩放/编码全在浏览器完成，设备只收 8192 定长字节。
2. **网络服务全按需**：httpd 仅上传模式存在（用户按键开启，10 分钟自动关）；SNTP 仅首页活跃时跑；天气单次 GET，TLS 缓冲 1024/512。
3. **stop 即清空**：UI 全删、任务退出 join、SNTP/上传服务停止（照抄 settings.cc + codex 停止模式）。
4. **版本号代替轮询**：`home_profile_version()/home_weather_version()` 变化才刷新对应 UI 行。
5. **临界区外做 IO**：home_profile 的 NVS/SPIFFS 写都在 portMUX 临界区外（单核 C3 临界区阻塞调度）。
6. 栈预算：首页任务 4096；httpd 6144（已在代码里）。

---

## 3. 已完成文件（编码完成，未编译）

| 文件 | 内容 |
|---|---|
| `main/apps/app_base.h` | 枚举追加 `APP_ID_HOME`（=7，APP_ID_COUNT 前） |
| `main/apps/codex/buddy_font_16.c` | 重新生成：+圳浏 +全角标点 +ASCII（原位替换） |
| `main/apps/home/home_profile.h/.c` | 资料 NVS 读写（portMUX 保护+版本号）；SPIFFS 惰性挂载（挂载失败才格式化一次）；头像 tmp 流式写 + 尺寸校验 + rename 原子替换；接口：`init/get/version/set_text/has_avatar/read_avatar/write_avatar_chunk`。容量：姓名 32B、签名 96B、城市 48B、经纬度 16B（UTF-8 含结尾 0，超长拒绝不截断） |
| `main/apps/home/home_clock.h/.c` | SNTP 启停（幂等）；`home_clock_valid()`（epoch>2023 判有效）；`format_time/date/weekday`（时区用 `time_sync_tz_hour()`） |
| `main/apps/home/home_weather.h/.c` | Open-Meteo 拉取（esp_http_client open/read 小响应模式，TLS 用 cert bundle）；cJSON 解析；NVS 缓存（无网开机显示上次天气）；WMO 码→中文短语映射（所有短语字形已验证在 buddy_font_16 内） |
| `main/apps/home/home_upload.h/.cc` | 按需 httpd（GET / 页面、GET/POST /api/profile、POST /api/avatar 512B 分块流式写）；内嵌网页：浏览器 canvas 中心裁剪 64×64 → 小端 RGB565 → fetch 上传；城市定位调 open-meteo 地理编码（走手机网络）；URL 从 `WifiManager::GetIpAddress()` 取 |

注意事项（编译时可能要处理的点）：
- `main/CMakeLists.txt` 的 REQUIRES **缺 `esp_http_server`**，必须加（home_upload 用）。
- `home_clock.c` include 的 `esp_sntp.h` 由 lwip 提供，通常经 esp_wifi 传递可见；若报找不到，REQUIRES 加 `lwip`。
- `home_weather.c` 的 `esp_crt_bundle.h` 由 mbedtls 提供（REQUIRES 已有 mbedtls）。
- 网页校验用"度"字显示温度前，先确认在字库内（大概率在；不在就改"26C"）。

---

## 4. 上传模式交互（已定设计）

- 首页按 **OK** = 开启上传模式：屏幕显示 `http://<IP>`，手机连同一 Wi-Fi 打开即配置页；10 分钟无操作自动关服务，再按 OK 手动关。
- 网页保存/上传成功 → 设备端版本号自增 → 首页界面**实时刷新**（无需退出）。

---

## 5. 剩余工作（按序执行）

### 5.1 codex 额度读取接口（app_codex.cc/.h）
```c
// app_codex.h 追加:
// 返回 false 表示 Codex 应用本会话未运行过（首页显示 "--"）
bool codex_app_get_usage(uint64_t *tokens_today, uint64_t *tokens_total);

// app_codex.cc 实现:
bool codex_app_get_usage(uint64_t *today, uint64_t *total) {
    static bool s_ready = false;           // 在 codex_app_start 里置 true
    if (!s_ready) { if(today)*today=0; if(total)*total=0; return false; }
    buddy_ui_snapshot_t snap;
    buddy_state_snapshot(&s_codex_state, &snap);
    if (today) *today = snap.tokens_today;
    if (total) *total = snap.tokens;
    return true;
}
```
并发安全性说明：应用经 registry 互斥运行（切首页前 codex 必已 stop），快照读无竞态。

### 5.2 app_home.cc（核心 UI，参考 settings.cc 结构）
新建 `main/apps/home/app_home.cc` + `app_home.h`（导出 `g_home_app`）。规格：

**静态资源**
- `static uint8_t s_avatar_buf[8192];`（静态分配，避免碎片）
- `static lv_img_dsc_t s_avatar_dsc = { .header = {.w=64,.h=64,.cf=LV_IMG_CF_TRUE_COLOR}, .data = s_avatar_buf, .data_size = 8192 };`
- `LV_FONT_DECLARE(buddy_font_16);`
- 配色沿用系统：bg 0x0A0F14 / 卡片 0x121B22 / 边 0x22323D / 字 0xF0F4F8 / 弱 0x7E92A2 / 强调 0xFFB74D。

**布局（240×320）**
```
y8    左: "09月12日 周六" (buddy16)     右: 电量百分比 (montserrat_14，可选)
y40   头像 64×64 (24,40)；右侧 x100: 姓名 y44 (buddy16)、签名 y72 单行省略号(LV_LABEL_LONG_DOT)
y130  大时钟 "12:34" 居中 (montserrat_20；未校时 "--:--")
y168  天气行 居中 (buddy16): "上海 多云 26度"；无城市显示 "设置城市后显示天气"
y210  Codex 卡片 208×34: 左 "Codex 积分" 右数值（≥1万 显示 "x.x万"）
y252  状态行: 上传模式时显示 URL(montserrat_14)；平时显示 Wi-Fi/离线提示
y300  底部提示 (buddy16): "上:刷新 下:详情 确:上传"
```
无头像时：`memset(s_avatar_buf, 深色填充)` + 上叠 2 个 lv_obj 画人形剪影（圆头+半圆肩），不增加图片资源。

**后台任务**（xTaskCreate 4096 栈，优先级 4，1s 循环）
- 每秒：`home_clock_format_*` 刷新时钟/日期（字符串没变就不 set，LVGL 内部会 diff）
- `home_profile_version()` 变了：先在**不持 LVGL 锁**时 `home_profile_read_avatar(s_avatar_buf)`，再持锁 invalidate
- `home_weather_version()` 变了：刷新天气行
- 天气刷新调度：启动 15s 后首次，之后 1800s 一次；`WifiManager::IsConnected()` 才发起；HTTP 阻塞在本任务里可接受（2~8s）
- Wi-Fi 就绪 → `home_clock_start()`；断开 → `home_clock_stop()`
- 上传模式 10 分钟超时自动 `home_upload_stop()` 并刷新状态行

**按键**（on_key，只处理 BTN_EVT_CLICK；长按 OK 由 registry 全局接管）
- UP：立即重载头像/资料 + 触发一次天气刷新
- DOWN：底部详情行在"Codex 今日/累计"与"天气码+缓存时间"间切换
- OK：切换上传模式（开：`home_upload_start()` + 显示 URL；关：`home_upload_stop()`）

**生命周期**
- `init()`：`home_profile_init(); home_weather_init(); time_sync_init();`（幂等）
- `start()`：建 UI → 启任务
- `stop()`：停任务（atomic flag + 等 handle 清空）→ `home_upload_stop()` → `home_clock_stop()` → 删屏
- `g_home_app`：`.id=APP_ID_HOME, .name="首页", .tag="Wi-Fi", .theme_color=0xFFB74D`

### 5.3 Launcher 注册（launcher.cc）
- `extern const passport_app_t g_home_app;` + 加进 `s_apps[]`（**放最前**，轮播首卡即首页）
- `app_chinese_name()` 加 `case APP_ID_HOME: return "首页";`（"首页"两字已在 radio_font 字库内，已验证）
- `draw_app_icon()` 加 APP_ID_HOME 几何图标（小房子）：三段递减矩形当屋顶 (16,30,56,8)/(22,22,44,8)/(30,14,28,8) r2 + 屋身 (20,38,48,32) r4 + 门 (44,48,12,22) r2 + 窗 (32,44,8,8) r4
- kMaxApps=8，加完共 7 卡，不用改

### 5.4 CMakeLists（main/CMakeLists.txt）
- SRCS 追加：`apps/home/home_profile.c`、`apps/home/home_clock.c`、`apps/home/home_weather.c`、`apps/home/home_upload.cc`、`apps/home/app_home.cc`
- INCLUDE_DIRS 追加 `"apps/home"`
- REQUIRES 追加 `esp_http_server`（必要时 `lwip`）

### 5.5 main.cc 开机进首页
- `app_registry_init();` 之后加 `app_registry_switch_to(APP_ID_HOME);`
- 可选：serial_cmd_task 里 '6'/'7' 映射到 APP_ID_HOME 方便调试

### 5.6 验证清单
1. `idf.py -B build build`（先处理第 3 节注意事项的编译问题）
2. 烧录后串口看：`home_profile`/`home_clock`/`home_upload` 日志、registry 的 "Free heap after app stop" 行
3. 功能：未配置时占位显示 → 手机上传头像/资料 → 实时刷新 → 断电重启资料仍在 → Wi-Fi 下日期/天气出现 → Codex 连过后积分显示 → 长按 OK 回 Launcher、从首页卡片再进（内存回收正常，反复切换 10 次无泄漏迹象）
4. 压力：上传模式中断网/中途中断上传（旧头像应完好）；SPIFFS 首次挂载失败自动格式化一次后可用

---

## 8. 交互模型变更：首页从「一张卡」改为「底座」（2026-09-12）

原实现里首页是 Launcher 轮播的 7 张卡之一。已改为**首页是底座，应用板块是它的下一层**：

```
        ┌── 首页(APP_ID_HOME,底座)──┐
        │   OK 短按 ↓               │
        │   应用板块(Launcher,6 卡)  │
        └──────────────────────────┘
   长按 OK：从任何界面（含应用板块）→ 回首页
```

改动点：

| 文件 | 改动 |
|---|---|
| `app_registry.cc` | `s_apps[]` 移除 `g_home_app`（轮播 7→6 卡） |
| `app_registry.cc` | **`get_by_id()` 加 `APP_ID_HOME` 特判** —— 首页不在 `s_apps[]` 里，没有这个特判 `switch_to(APP_ID_HOME)` 会找不到实例而黑屏（这是本次最容易踩的坑） |
| `app_registry.cc` | 长按 OK 拦截条件从 `!= APP_ID_LAUNCHER` 改为 `!= APP_ID_HOME`，应用板块也纳入拦截 |
| `app_home.cc` | OK 短按 → `app_registry_switch_to(APP_ID_LAUNCHER)`；**长按下键** → 上传模式开关 |
| `app_home.cc` / `launcher.cc` | 底部提示改两行，说明新键位与返回方式 |
| `main.cc` | 串口调试键加 `k` = 长按下键（否则无法脱离真机按键验证上传模式） |

键位约定：遵循项目既定的「同一颗键只放单击 + 长按」规则，不用双击。
首页：上=刷新、下=详情切换、长按下=上传模式、确定=应用板块。

## 7. 与本文档的偏差（留痕）

1. **首页任务栈 6144，而非第 2 节的 4096。** 该任务内会跑 `home_weather_refresh()`
   的 HTTPS，mbedTLS 握手调用链在 4KB 栈上有溢出风险。多花 2KB 换不崩溃。
   若后续把天气挪到独立任务，可以降回 4096。
2. **`buddy_font_16` 声明包了 `extern "C"`。** 第 1 节只写了 `LV_FONT_DECLARE(...)`，
   但 `buddy_font_16.c` 是 C 文件且没有 `extern "C"` 保护，从 `app_home.cc`(C++)
   按 C++ 链接找符号有风险，已显式包裹。
3. **REQUIRES 额外加了 `spiffs` 与 `lwip`。** 第 3 节只提到 `esp_http_server`：
   `home_profile.c` 用 `esp_spiffs.h`，`home_clock.c` 用 `esp_sntp.h`(由 lwip 提供)。
4. **天气行与状态行强制单行省略（`LV_LABEL_LONG_DOT`）。** 城市名最长 47 字节，
   默认换行模式会顶破布局压到下面的 Codex 卡片。
5. **天气文本缓冲 96 字节**（非 40）：最坏情况 47(城市)+1+15(天气短语)+6(温度) 会溢出。
6. `home_start()` 在持 LVGL 锁**之前**读完资料和头像——SPIFFS 首次挂载可达几十毫秒。

## 6. 已知风险
- Codex 积分只有 PC 端 background bridge 连过 BLE 才有值；否则显示"--"是预期行为。
- 天气走 HTTPS（cert bundle 全量已开），C3 上 TLS 峰值约占 40~60KB 堆，仅首页任务内瞬态发生；如仍紧张可把 buffer_size 降到 512。
- open-meteo 国内可达性一般尚可；失败保留上次缓存显示，不重试风暴（30 分钟后自然重试）。
- 头像 8192B 静态缓冲常驻 .bss——如改堆分配，务必 stop 时 free 并置空指针。

## 9. 首页视觉改版（2026-09-12）

第一版首页能用，但排版是"表单堆叠"：y 坐标 8/40/130/168/210/252 是随手填的，
间距 32→90→38→42 忽疏忽密；只有 Codex 一张卡片有背景，其它全是裸灰字，
缺乏统一语言。改版目标：确立视觉重心 + 统一卡片语言 + 拉开字重层次。

### 版式（240×320）

| 区块 | y 区间 | 内容 |
| --- | --- | --- |
| 状态栏 | 0–28 | 日期、Wi-Fi 状态圆点、迷你电池（外框 + 按 SOC 伸缩的内条 + 百分比） |
| 身份卡 | 32–116 | 强调色底框 + 64×64 头像（无头像时人形剪影）、姓名、签名 |
| **大时钟** | 120–192 | 48px 时 / 冒号 / 分，三段 flex 居中，冒号每秒呼吸 |
| 天气行 | 192–216 | 城市(弱) + 天气短语(次) + 温度(强调色 20px) |
| Codex 双卡 | 226–282 | 「今日」「累计」两张 104×56 同风格统计卡 |
| 底部行 | 290–312 | 三态轮换（下键）：操作提示 / 上传提示 / IP + 天气码 |

### 关键改动

1. **启用 `CONFIG_LV_FONT_MONTSERRAT_48`**（`sdkconfig.defaults` 与 `sdkconfig` 同步改）。
   大时钟是整个改版的支点——此前最大只有 montserrat_20，时间字太小撑不起版面。
   字模约 100KB 落在 flash 的 `.rodata`，经 MMU 直接映射，**不占 SRAM**（实测
   改版后空闲堆 132KB，仅比改版前少 3KB）。C3 无 PSRAM，切勿把字模读进内存。
   `lv_font_montserrat_48.c` 的 `line_height = 52`，所以时钟容器给 72 高，不会被裁切。
2. **版式栅格化**：所有元素挂在 `kMargin` / `kContentW` 常量上，不再散落硬编码 y。
3. **文字分四档**（kText / kSub / kMuted / kFaint），任何区块都有明确主次；
   卡片统一 `kCard` 底 + 1px `kLine` 极暗描边，靠明度差而非亮线界定边界。
4. **时钟拆三段**是为了让冒号每秒呼吸。用 `text_opa` 而不是改文本——冒号宽度不变，
   不会引发 flex 重新布局抖动（这是整屏唯一的动效）。
5. **时钟与天气行用 flex 容器居中**，不手算 48px 字体的数字步进宽度
   （不同字号 advance 不同，手算必然在某个字号上错位）。
6. **底部行三态轮换**：上传提示原本占第二行，现在折进轮换，省下的空间给了统计卡。
   上传模式优先级最高，且每秒重算——顺带修掉了"Wi-Fi 刚连上没拿到 IP 时显示空
   `http://`"的老问题。
7. **温度用 montserrat_20 显示 `24°`**，不用中文点阵的"度"，数字字形更细腻；
   `°`(0xB0) 在 montserrat 的 Latin-1 覆盖范围内。

### 交互（不变）

上=刷新、下=底部行三态轮换、长按下=上传模式、确定=应用板块。

### 实测

- 固件 0x3010A0 ≈ 3.01MB，app 分区 4MB，剩余 25%。
- 启动 → 首页 132KB；按确定 → 应用板块 100KB；长按确定 → 回首页 87KB（与改版前的
  135→100→89KB 基本一致，无新增泄漏）。

## 10. 全机像素 UI 精致化（2026-09-13，已编译通过）

目标：解决"像素风底子好但画面偏静态/普通"的问题。全部改动为**对象拼画 + 低频
lv_anim**，无 canvas、无图片资源；删除屏幕时动画随对象一起被 LVGL 清理
（`lv_obj_destructor` → `lv_anim_delete(obj,NULL)`，`lv_obj.c:614` 已核对）。

> **编译结果**（WSL Ubuntu-24.04 `/opt/esp-idf`，默认 Ubuntu 发行版 ext4.vhdx 已损坏
> 无法启动）：固件 0x2F0AA0 ≈ 3.08MB，app 分区剩余 **26%**，零告警。日志
> `_diag/build_ui_polish2.log`。剩余验证全部在真机（见 10.7）。

### 10.1 昼夜主题（`ui_pixel.c/.h`）

- `ui_pixel_set_hour_theme(hour)`：19~23 点与 0~5 点为夜景，`hour<0`（未校时）保持
  白天；主题翻转时返回 true。
- `ui_pixel_theme_apply(scr)`：把"最近一次创建过 `ui_pixel_background` 的 screen"
  的背景重染成当前主题（返回是否生效）。内部用 46 项的**背景部件登记表**
  （`UI_BG_PARTS_MAX 64`）记录每个色块的 day/night 颜色与显隐。
- 夜景只动"环境色"：天空 `0x0D2144`、草地 `0x2A5220`、云染月光灰 `0xA9BDD9`；
  纸卡/墨描边原样。
- 新增背景元素：5 颗星（仅夜，交错闪烁）、2 朵慢速横漂的云（52s/68s 一趟，
  底沿从顶栏下露出）、2 朵小花贴草地两端（仅昼，x2 与 x229 —— footer 白字居中
  占 x12..228，两端是空位）、萤火虫 2×2 沿草皮巡游+明灭（仅夜，y313 在文字带下方）。
- 接线：首页 `create_ui` 按当前小时定主题，任务 1s tick 里核对翻越 19/6 点边界；
  Launcher 在每次选卡（`update_card_content`）时核对。其余 App（设置/小智/游戏）
  继承最近一次主题，自动受益。

### 10.2 天气像素图标 + spinner（`app_home.cc`）

- WMO 码 → 方块拼画：晴(0)/少云(1,2)/阴(3)/雾(45,48)/雨(51-67,80-82)/
  雪(71-77,85,86)/雷(≥95)，画在天气卡左侧 22×18 图标区（`wicon_*` 系列函数）。
- 拉取中显示四点 spinner，由首页 1s tick 每秒转一格；无论成败拉取结束都会刷新
  一次（成功换图标、失败撤 spinner）。
- 城市名/天气短语定宽 54/78px + DOTS 截断，行容器 pad_left 28 给图标让位；
  行高取内容净高 18px 保证垂直居中。

### 10.3 首页其它

- 无头像空态改用全机吉祥物（`ui_pixel_mascot_create`，38×48 带眨眼），替代灰色剪影。
- 天气行从 radius 9 圆角胶囊改回**方角纸卡 + 硬阴影**（`make_panel`），头像环
  radius 4→0 —— 全机统一"全套方角"像素语言。
- 底部白字加墨色错位(+1,+1)硬阴影（`footer_set` 同时更新两份）。

### 10.4 Launcher

- 选卡动效 `card_nudge(dir)`：卡片与硬阴影向切换方向轻推 8px 再弹回
  （70ms+110ms playback），图标原地跳一下（16→11→16）。纯 lv_anim。
- 阴影块改为 `s_card_shadow` 静态句柄以便动画；`launcher_ui_hide` 置空。
- Launcher 现在包含 `apps/home/home_clock.h`（仅读系统时间，不启动 SNTP）。

### 10.5 `ui_pixel_hint_pill` 行为变更

提示文字现在画两份（墨影+白字）。launcher/settings 都是创建时一次性设文字，无需
改动；**若将来要动态改提示文字，两份 label 必须一起改**（函数注释已标明）。

### 10.6 `buddy_font_16` 升级 2bpp

1bpp 锯齿是"普通感"主要来源之一。用同一符号集（13KB symbols，从旧文件头部 Opts
提取）以 `--bpp 2` 重新生成，原位替换 `main/fonts/buddy_font_16.c`
（源码 31k → 47.6k 行，二进制约 +140KB flash，app 分区余量充足；声明与
`app_fonts.h` 不变）。1bpp 旧版备份：`/tmp/buddy_font_16_1bpp_backup.c`。
`home-page-plan.md` 第 1 节的重生成命令如再用，把 `--bpp 1` 改成 `--bpp 2`。

⚠️ **Windows npx 生成的字库文件头部 include 是 `lvgl/lvgl.h`（else 分支），
本工程没有定义 `LV_LVGL_H_INCLUDE_SIMPLE`，会编译失败** —— 生成后需把
else 分支改成 `#include "lvgl.h"`（WSL 端 lv_font_conv 生成时没这个问题，
两个分支都是 `lvgl.h`）。已按此修正。

### 10.7 验证清单（已编译；剩余项在真机执行）

1. ✅ `idf.py -B build build` 通过，零告警（`_diag/build_ui_polish2.log`）。
2. 开机看首页：白天蓝底/夜间深蓝 + 星闪 + 萤火虫（可用 SNTP 前提；未校时应为白天）。
3. 天气拉取期间看 spinner 转动，完成后出对应图标；改城市→晴/雨等不同图标。
4. 无头像（清 NVS `home_profile`）看吉祥物空态；上传头像后照片正常覆盖。
5. Launcher 连按上下：卡片轻推+图标弹跳，快速连按无残留位移（anim delete 兜底）。
6. 反复进出首页/Launcher ×10 看堆稳定（新增静态对象约 +2~4KB/屏）。
7. 字体：中文边缘应明显更平滑；若 2bpp 渲染异常（花屏/错位），回滚
   `/tmp/buddy_font_16_1bpp_backup.c` 即可，其余改动不依赖 2bpp。

## 11. 绿磷光锁屏首页（2026-09-13 深夜，已编译烧录）

参照用户提供的复古终端风参考图（绿磷光点阵锁屏），首页整页改为**黑底 + 磷光绿
单色**。其他应用不受影响（ui_pixel 昼夜像素风仍在）。

### 11.1 版式（240×320）

| 区 | 位置 | 内容 |
| --- | --- | --- |
| 状态栏 | y0..24 | Wi-Fi 三格（左）+ 电池%（右），磷光细线框 |
| 左卡 | x6 y28 104×180 | 人像区 96×120（内凹框）+ 姓名 + 签名 |
| 时钟卡 | x114 y28 120×50 | 点阵 HH:MM（5×7 字模，每格 4×4 光点，冒号呼吸） |
| 日期卡 | x114 y82 120×44 | 日期 / 定位 pin + 城市 |
| 天气卡 | x114 y130 120×78 | 单色图标 + 温度 / 短语 / 分隔线 / 风向·风级 |
| 底图 | y212..320 全宽 | 240×108 A8 艺术图，提示黑条压下缘 |

### 11.2 关键实现

- **美术 = A8 alpha 图 + image_recolor(磷光绿)**。软件渲染只认 A8 的
  "蒙版 + 纯色"快速路径（`lv_draw_sw_img.c:233`）；**I1 不行**——SW 渲染把 I1
  当亮度图(0→黑,1→白)不查调色板（`lv_draw_sw_blend_to_rgb565.c:460`）。
- 内置底图两张（城市夜景/山湖月色）由 `tools/gen_band_art.py` 程序化绘制 +
  **Bayer 有序抖动**生成（`main/apps/home/band_art.c`，各 25.9KB flash，零 RAM）。
  画风为高对比海报风:剪影纯黑、灯月纯亮、仅天空走规则网点 —— 首版用
  Floyd-Steinberg 时整图糊成噪声看不出内容,已弃用。改场景/加图改脚本重跑,
  勿手改 C 文件。城市场景元素:电视塔/跨江大桥/楼群亮窗/月倒影;山湖场景:
  大月/三层山脊/松林/小船渔火。
- **长按上键 = 切换底图**（城市 → 山湖）。用户上传底图/竖幅人像（96×140 1bpp）
  留作下一步：扩展 home_upload 网页端裁剪+抖动，管线照抄 64×64 头像。
- **头像单色化**：上传管线不变（64×64 RGB565），设备端 `convert_avatar_to_a8()`
  按亮度阈值转 A8（4KB .bss）→ 磷光点阵照片。
- **风向**：open-meteo 增加 `wind_direction_10m`，NVS 键 `wdir`，
  `home_weather_wind_dir_text()` 8 方位中文（北风/东南风…，字形已验证在字库内）。
- 点阵时钟字模不变（`kDigitGlyphs`），渲染从"合并成块"改为"逐格光点"。
- 配色收敛：kBg 0x03120B / kCardBg 0x051910 / kPhos 0x8CF5A6 / kPhosDim 0x3F8F5F /
  kPhosBorder 0x2E7C4E。首页不再调 ui_pixel 昼夜主题（ui_pixel 主题留给其他应用）。
- 固件 0x2FDA70 ≈ 3.13MB，app 分区剩 25%（+52KB = 两张 A8 底图）。

### 11.3 竖幅人像（2026-09-13 深夜第二轮，已编译烧录）

人像照片从 64×64 方图升级为 **96×120 竖幅**（正好铺满首页人像框），格式改为
**1bpp 打包**（浏览器端裁剪+亮度阈值+打包，仅 1,440B；旧 RGB565 管线 23KB 弃用）：

- 存储换名 `portrait.bin`（旧 `avatar.bin` 因尺寸校验 8192≠1440 自动失效，重新
  上传一次即可；旧文件留在 SPIFFS 无害）。
- 网页端：等比缩放"覆盖"96×120 居中裁剪 → luma≥110 阈值 → 按行 MSB 打包。
- 设备端：`expand_portrait_to_a8()` 展开为 A8（11.5KB .bss）→ recolor 磷光绿。
  `.bss` 总量与旧方案基本持平（1.4K + 11.5K ≈ 旧 8K + 4K）。

### 11.4 下一步（未做）

1. 自定义底图上传（240×108 1bpp 抖动，浏览器端同款管线；设备端需 ~26KB RAM
   展开 A8，或直接以 1bpp 逐行展开渲染,需实测堆余量）。
2. 姓名大字号：网页端渲染成 1bpp 图条上传，绕开设备字库限制。
