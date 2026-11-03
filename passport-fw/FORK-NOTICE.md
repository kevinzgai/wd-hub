# passport-fw：FoloToy AI Passport 的 WorkBuddy 伴生固件

## 底座出处

本目录的主体代码 fork 自 **passport-os**：

- 仓库：<https://gitee.com/alex_kan/passport-os.git>
- 本地参考快照：`C:\Users\Administrator\WorkBuddy\_ref\passport-os-gitee`
- 提交：`b4d9555`（"多功能掌上伴侣：首页+8款应用……"）
- 许可证：项目主体 MIT（见本目录 `LICENSE`）

`components/passport_muse/` 来自 Muse 项目，遵循 Apache-2.0，原始
`LICENSE` 与 `NOTICE.md` 一并保留在 `components/passport_muse/` 内，未作修改。
`components/pace_core/`、`components/esp-wifi-connect/` 等第三方组件的
许可与声明同样保留在原目录。

> 本目录是**独立新工程**：自带 `CMakeLists.txt`、`sdkconfig.defaults`、
> `partitions.csv` 与构建入口，不引用、不合并仓库根目录 `firmware/`
> （ESP32-S3 小智盒子）的任何源码。协议层仅按只读方式参考其 GATT UUID
> 与帧语义，在本工程内重新实现。

## 本工程的改动方向

把底座从"7 款本地应用"改造为 **WorkBuddy 主机（`host/wb_app.py`）的
BLE 伴生屏**：会话总览、审批、提问、待机钟面、省电与三键交互。
底座中与本任务无关的应用暂不删除，先保证可编译、可启动，再逐轮瘦身。

Muse 的 Jollybot 素材保持原样，未替换为本项目品牌形象。
