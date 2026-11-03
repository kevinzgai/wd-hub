# Passport OS 开源整理审计报告

审计日期：2026-10-25  
审计对象：当前工作区与刚刚完整烧录到 COM3 的固件 0.2.0-rc.55  
审计目标：确定开源包应该保留什么、排除什么，以及社区用户如何下载、烧录和首次配置。

本报告先给整理结果，不删除工作区内容，也不改变当前构建默认值。历史候选、诊断证据和临时构建目录仍保留，等维护者确认清单后再做清理。

## 1. 当前固件身份

| 项目 | 当前值 |
| --- | --- |
| 芯片 | ESP32-C3 |
| Flash | 8 MB |
| 固件版本 | `0.2.0-rc.55` |
| 当前应用镜像 | `passport-os-status-fixed.bin` |
| 应用镜像 SHA256 | `112eb7d1cce27e2c47d60b5913f6da954120de3f549e34e02034c74679016466` |
| 当前完整镜像 | `FoloToy-AI-Passport-full.bin` |
| 完整镜像 SHA256 | `985ea2f3afa4cbead16933dad2624a5942c8b86c82719a1d2b789aed3231a0f9` |
| ESP-IDF | 5.5.3 |
| 分区 | factory `0x10000 / 0x400000`，NVS `0x9000 / 0x6000`，storage `0x420000 / 0x200000` |
| 当前构建开关 | `PASSPORT_PUBLIC_RELEASE=ON`，`PASSPORT_RADIO_HLS_TEST=OFF`，`PASSPORT_MUSE_USE_JOLLYBOT=ON` |

完整擦除、8 MB 擦除读回、完整镜像写入和读回校验均已通过。首次启动日志确认没有旧 Wi-Fi 自动连接，开放配网热点正常启动，设备停在首次设置页。

## 2. 当前固件实际包含的功能

这些功能在 `main/CMakeLists.txt` 中被编译，属于当前产品范围，应保留对应源代码、资源和测试：

- 首页：个人资料、头像/名片、天气、空气质量、城市目录、时钟、开放手机配网。
- 启动器和设置：四套主题、亮度、自动熄屏、音量、按键音、无线网络、网络与时间、计时恢复。
- 收音机：城市目录、推荐电台、MP3 播放、音量、睡眠计时和网络状态。
- 随身语音：电脑端语音输入/快捷键桥接协议、UDP 音频、时间同步和本地设置。
- 小智：WebSocket 语音对话、Opus 音频、OTA 配置、对话状态和 UI。
- Muse：BLE 配对、Noise 协议、TLS/HTTP 代理路由、聊天链路和设备状态。
- 游戏：狗游戏、俄罗斯方块、声音跳一跳；后者当前不绘制预测轨迹和落点辅助线。
- 秒表和番茄钟：计时、后台恢复和提示音。
- 板级支持：ESP32-C3 引脚、ST7789 显示、LVGL、ES8311 音频、ADC 三键、电池、功耗管理和 USB Serial/JTAG。

## 3. 建议的开源包结构

### 必须保留

```text
.
├── CMakeLists.txt / build.sh / dependencies.lock / partitions.csv
├── sdkconfig.defaults / LICENSE / README.md / README.zh_CN.md
├── .github/                         # CI、贡献、安全和支持模板
├── bootloader_components/bl_early/  # 当前引导阶段代码
├── components/bsp/                  # 当前板级驱动
├── components/esp-wifi-connect/     # 配网、DNS、站点连接和保存的 Wi-Fi
├── components/noise_core/           # Muse Noise 协议，含 Apache-2.0 说明
├── components/passport_muse/        # Muse 适配层，含上游许可证和 NOTICE
├── main/                            # 当前固件的全部编译源、字体和内置资源
├── third_party/china-cities/        # 城市目录生成所需 MIT 数据和许可证
├── tests/                            # 主机测试、模拟器和测试桩
├── tools/                            # 构建、验证、生成和打包脚本
└── docs/                             # 精简后的架构、功能、发布和贡献说明
```

`main/apps/home/cities.json.gz`、`main/apps/radio/radio_cities.inc`、`main/fonts/city_symbols.txt` 和 `main/ui_sfx_pcm.inc` 是生成后参与编译的资源；生成脚本和源数据也必须保留。

### 不应进入开源源代码包

- `build/`、`build-*`、`managed_components/`、`.cache/`、`sdkconfig`、`.elf`、`.map`、`.bin`、`.log`：机器生成或本地配置。组件由 `dependencies.lock` 和 ESP-IDF Component Manager 重新获取。
- `release/` 下的 rc.12–rc.55 历史压缩包、旧候选目录和旧验证文件：它们不是当前源代码，容易让用户刷错版本。
- `_diag/`、`output/`、临时预览、截图、串口私有日志和设备验证目录：保留在维护者归档，不放公开源码包。
- `all_chars.txt`、`ui_chars.txt`、`.impeccable/`、设计过程草稿和没有被构建或测试引用的实验资源。
- 旧 HLS 专项候选、旧版本 release notes、历史视觉审计和重复的功能说明；合并成当前文档或放入历史归档。
- `web/phone-location.html` 当前没有被固件 CMake 或手机配网页嵌入；除非作为独立网页发布，否则排除。

### 必须单独处理的内容

1. **Muse 头像资产**：刚刚烧录的固件用了 `main/apps/muse/muse_avatar_frames.c` 中的 Jollybot 帧；仓库另有 `muse_avatar_public_frames.c`，说明标为 MIT 的公开几何头像。Jollybot 不是 Apache-2.0 或 MIT 资产，不能因为软件源码使用 MIT 就自动重新授权。建议公开源码默认切换到 `PASSPORT_MUSE_USE_JOLLYBOT=OFF`，发布 MIT 几何头像，并把 Jollybot 仅作为得到单独授权后的维护者构建选项。
2. **Muse 上游代码**：`components/passport_muse/LICENSE`、`NOTICE.md` 和 `components/noise_core/NOTICE.md` 必须随源码保留；Apache-2.0 代码、Meta 上游来源、本地适配改动和个人配置默认值要分开说明。
3. **字体与图片**：字体是编译进固件的派生资源，开源前需要保留来源、生成参数和许可证说明。建议新增统一的 `docs/THIRD-PARTY-NOTICES.md`。
4. **网络服务**：天气、IP 城市查询、公共电台、小智服务和 Muse 代理不是本项目托管的服务，说明中应写清楚网络可用性、第三方条款和用户自行承担的账号/代理配置。

## 4. 发布前需要修正的问题

- `PASSPORT_MUSE_USE_JOLLYBOT` 默认值和公开构建路径目前都是 `ON`，与公开头像说明存在边界冲突。
- `dependencies.lock` 被 `.gitignore` 忽略，但它是可复现构建的重要输入；正式仓库应取消忽略并提交它。
- 当前工作区包含大量未提交改动和未跟踪文件，不能直接以当前 Git HEAD 作为 release 来源。
- 版本号仍为 `0.2.0-rc.55`。正式社区开源版建议单独递增版本号，并让源码、固件、发布说明和 SHA256 清单一致。
- `tools/package_firmware.py` 已有源码快照和逐文件哈希能力，但必须在清理完成后再打包，不能复用历史 release ZIP。

## 5. 推荐的社区安装路径

普通用户不需要安装 AI，也不需要先安装 ESP-IDF：下载匹配硬件的 `*-full.bin`，验证 SHA256，写入 `0x0`，首次启动连接 `Passport-WiFi-xxxx` 开放热点，在 `http://192.168.4.1` 完成配网。普通升级使用同一分区表确认过的 `*-app.bin` 写入 `0x10000`。

源码开发者再安装 ESP-IDF 5.5.3，执行 `bash build.sh release` 或 `./tools/validate.sh --firmware`。构建脚本会重新生成 `managed_components`，并验证分区、应用大小、SRAM 和公开默认配置。

## 6. 审计结论

当前项目具备开源所需的源代码、构建脚本、测试和许可证基础；问题集中在历史内容太多，以及 Muse 头像发布边界。建议先生成干净公开分支，默认使用 MIT 几何头像，保留完整功能源代码和必要测试，再确认是否发布 Jollybot 资产。

本报告没有执行删除、改默认值或重新发布操作。
