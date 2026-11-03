<div align="center">

# Passport OS

**把一块小硬件，折腾成每天会用的小玩意。**

FoloToy AI Passport 社区固件 · ESP32-C3 · 8 MB Flash · 7 款功能与应用

**简体中文** · [English](docs/README.en.md)

[看看能玩什么](#七款应用装进口袋) · [安装固件](#先用起来) · [自己改一版](#把它改成你的版本) · [关联项目](#老朋友)

</div>

---

## 从自己想用开始

首先，感谢大家对这个项目的喜欢和支持。收到这么多反馈，让这件原本只为自己做的小事，多了很多继续折腾的理由。

制作 Passport OS 的初衷很简单：把这块硬件变成一个我自己会经常使用的“小玩意”。经过一轮轮尝试和筛选，我留下了日常用得上的 7 款功能与应用：能聊两句、听听电台，也能计个时、练练电键。

大家提出了不少有意思的个性化需求。每个人的使用习惯不同，与其把所有想法都塞进同一个固件，不如把项目开放出来，让大家自己动手：增加喜欢的功能，删掉用不上的部分，做一个更适合自己的版本。

> 希望它既能成为你日常用得上的工具，也能成为值得折腾的小玩具。大家玩得开心！

## 七款应用，装进口袋

| 应用 | 可以做什么 | 使用条件 |
| --- | --- | --- |
| 首页 | 时钟、个人资料、头像与名片、天气和空气质量 | 天气与空气质量需要联网 |
| 收音机 | 选择城市和电台，听广播，设置睡眠计时 | Wi-Fi 与可用的网络电台 |
| 小智 AI | 通过麦克风和扬声器进行语音对话 | 网络与小智服务配置 |
| Muse | 语音发送给 Muse，在屏幕上阅读回复 | 手机配对、自己的账号与可用网络 |
| 秒表 | 计时、分段、暂停与继续 | 可离线使用 |
| 番茄钟 | 专注、休息和到时提醒 | 可离线使用 |
| 摩斯电键 | 按住电键发报，解码上屏，调节发报速度 | 可离线使用 |

应用之外，还包含音量、亮度、自动熄屏、短按唤醒、Wi-Fi 配网、电池显示与功耗管理等整机功能。

## 老朋友

此前单独开源的 Muse，也有独立的源码、教程和配套内容：

| 项目 | 地址 | 内容 |
| --- | --- | --- |
| Muse | [ai-passport-muse](https://github.com/manchunx7-bit/ai-passport-muse) | Muse 应用模块源码与使用教程 |

## 先用起来

**只想使用现有功能，不需要安装 AI 工具或编译环境。** 有对应版本的发布包时，从 [GitHub Releases](https://github.com/manchunx7-bit/passport-os/releases) 下载固件即可。想修改功能，再看下方的源码构建流程。

适用硬件：**相同硬件版本的 FoloToy AI Passport，ESP32-C3、8 MB Flash**。外观相似的其他开发板不能直接套用。

| 你的情况 | 选择镜像 | 烧录地址 |
| --- | --- | --- |
| 首次安装，或重新体验完整配网流程 | `*-full.bin` 完整镜像 | `0x0` |
| 已安装 Passport OS，且确认分区布局一致 | `*-app.bin` 应用镜像 | `0x10000` |

> **先分清完整安装和普通升级。** 完整镜像会覆盖写入范围内的配置；彻底重置还需要擦除 Flash。想保留配网、资料和图片时，先备份并确认分区布局一致，再使用应用镜像，不能先擦除整片 Flash。

详细的文件选择、Windows 烧录命令与常见问题，见 [社区安装说明](docs/open-source-guide-draft-20261025.md)。

<details>
<summary><strong>首次开机，怎么连接手机？</strong></summary>

1. 手机连接设备发出的 `Passport-WiFi-xxxx` 热点，无需密码。
2. 在浏览器打开 `http://192.168.4.1`。
3. 按页面提示选择 Wi-Fi、设置城市和个人资料；头像与名片按需添加。
4. 完成设置后返回首页，开始使用。

小智和 Muse 需要使用者自己的服务配置；Muse 使用代理时，填写代理主机的局域网 IP 和 HTTP 端口。

</details>

## 把它改成你的版本

**这个仓库包含整机固件源码。** 你可以从现有功能出发，改界面、改交互、增删应用，再编译并烧录到匹配的硬件上。

**下载源码 → 修改功能 → 编译固件 → 烧录设备 → 真机试一试。**

### 从哪里动手

| 想改什么 | 先看这里 |
| --- | --- |
| 某个应用的界面或逻辑 | [`main/apps/`](main/apps/) |
| 应用入口、切换、设置与系统交互 | [`main/launcher/`](main/launcher/) |
| 通用界面与字体资源 | [`main/ui/`](main/ui/) · [`main/fonts/`](main/fonts/) |
| 按键、显示、音频、电池等板级驱动 | [`components/bsp/`](components/bsp/) |
| Wi-Fi 配网与连接管理 | [`components/esp-wifi-connect/`](components/esp-wifi-connect/) |
| Muse 接入与协议适配 | [`components/passport_muse/`](components/passport_muse/) |
| 主机测试与构建工具 | [`tests/`](tests/) · [`tools/`](tools/) |

### 编译第一版

准备 **Git、ESP-IDF 5.5.3** 及其配套工具链。Windows 用户可以在 WSL 中构建，再从 Windows 串口烧录。

```bash
git clone https://github.com/manchunx7-bit/passport-os.git
cd passport-os
bash build.sh release
```

构建脚本默认从 `/opt/esp-idf` 加载环境。如果安装在其他目录，通过 `IDF_DIR` 指定路径，例如：

```bash
IDF_DIR="$HOME/esp/esp-idf" bash build.sh release
```

第三方组件由 ESP-IDF Component Manager 根据依赖文件获取。构建后得到：

| 文件 | 用途 |
| --- | --- |
| `build-public/FoloToy-AI-Passport-full.bin` | 完整镜像，写入 `0x0` |
| `build-public/passport-os.bin` | 应用镜像；相同分区布局的升级写入 `0x10000` |

### 改完，先检查

在已激活 ESP-IDF 5.5.3 的环境中执行：

```bash
./tools/validate.sh --static
./tools/validate.sh --firmware
```

第二条命令会重新构建并校验完整镜像，输出到 `build/FoloToy-AI-Passport-full.bin`。编译通过之后，仍需在设备上检查按键、显示、音频、配网及改动涉及的功能。

<details>
<summary><strong>准备分享自己的固件包？</strong></summary>

本地打包工具可以附带源码快照、manifest 和 SHA256 校验清单：

```bash
python3 tools/package_firmware.py --date YYYY-MM-DD --verification /path/to/verification-public.json
```

日期和验收文件路径需要替换为实际值。验收记录必须对应当前固件版本，且不包含私人数据。打包工具不会自动发布 GitHub Release；源码变更后要重新构建、验证，不能沿用旧版本的验证结果。

</details>

## 分享代码，保留自己的钥匙

发布构建会启用 `PASSPORT_PUBLIC_RELEASE=ON`，清空默认代理配置。每位使用者在自己的设备上填写 Wi-Fi、账号和服务凭据。

提交代码、Issue 或自己的改版时，请勿附上密码、API Token、设备配置导出、私人日志或个人图片。提交问题时，写明固件版本、硬件型号、复现步骤和是否保留了旧数据，会更容易定位。

## 许可与致谢

项目主体采用 [MIT 许可证](LICENSE)。Muse、Noise 协议和其他第三方内容遵循各自目录中的 `LICENSE`、`NOTICE` 及资源说明；Jollybot 形象的使用边界见 [单独声明](main/apps/muse/JOLLYBOT-NOTICE.md)。

感谢 FoloToy 的硬件与上游项目，也感谢所有分享想法、反馈问题和参与改进的朋友。

**从一个自己想用的小玩意开始，接下来，欢迎把它改成你的样子。**
