# archive/ —— 归档区

这里的东西**不属于当前架构**，保留只为追溯。当前架构请看根目录 [`README.md`](../README.md)。

## 为什么有这一层

2026-10-31 项目从 **WiFi + HTTP 轮询** 切换到 **蓝牙直连**：

```text
旧：  WorkBuddy 数据 → server/ (:27777, FastAPI) → WiFi → 设备轮询 /status
新：  WorkBuddy 数据 → host/wb_app.py (:8080) ──BLE notify──► 设备
                                        ◄──BLE write（审批/作答）──
```

切换理由（详见根 README）：新架构不需要配网、不受电脑休眠/IP 变更影响、
交互延迟从秒级降到百毫秒级。

## 内容清单

| 路径 | 是什么 | 为何保留 |
|---|---|---|
| `README-legacy-http.md` | 旧 WiFi/HTTP 架构的完整文档（8 屏、手机配网、27777 接口矩阵） | 记录了配网流程、验证码机制、`/screen/<i>` 契约的设计依据 |
| `legacy-entrypoints/start-board.sh` | 旧服务启动脚本（macOS/Linux） | 内含建 venv、装依赖、查端口占用的完整逻辑，新启动脚本参考过 |
| `legacy-entrypoints/start-board.bat` | 旧服务启动脚本（Windows，GBK+CRLF） | 同上；GBK 编码坑的解法值得留 |
| `legacy-entrypoints/configure.py` | 写 WiFi 凭据与 `BRIDGE_BASE` 到 `config.local.h` | 记录了"固件不烧默认密码"这条决策 |
| `retired-tools/check_api.py` | 旧 HTTP 端点矩阵自检 | 端点清单本身有参考价值 |
| `retired-tools/shot_board.py` | 旧网页演示页截图 | playwright 用法可复用 |
| `retired-tools/voice_proto_check.py` | 语音上行组包交叉验证 | 语音功能已下线（`WB_CAP_VOICE_UP` 不再声明） |
| `retired-tools/voice_hw_capture.py` | 真机语音上行抓包 | 同上。若恢复语音功能会用到 |
| `server-state-backup/task_ledger.json` | 旧 server 的任务账本数据 | 纯数据备份，防止有历史信息 |

## 旧 server 源码在哪

`server/` 目录（FastAPI app + `sources/` 下 db/files/ledger/system/tokens 五个数据源，
共 15 个文件）已从工作区删除，**完整保留在 git 历史**：

```bash
git log --oneline -- server/          # 看提交历史
git show <commit>:server/api_server.py # 取出某个版本的文件
git checkout <commit> -- server/      # 整体恢复到工作区
```

## 什么该放这里

- 架构变更后不再参与构建/运行的东西
- 被删但设计依据仍有价值的东西
- 纯历史数据备份

**不该放这里**：还能用的工具（那些应该归到 `tools/` 的合适子目录）、
临时调试产物（那些直接删）。
