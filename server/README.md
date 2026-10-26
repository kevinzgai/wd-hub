# WorkBuddy 看板后端

只读数据 API + 演示页。数据源是 WorkBuddy 自己维护的 `~/.workbuddy`，
不对其做任何写操作。

需求文档：[`docs/plans/2026-10-26-workbuddy-board-api-requirements.md`](../docs/plans/2026-10-26-workbuddy-board-api-requirements.md)

## 启动

```bash
py -m pip install -r server/requirements.txt
py -m server.api_server                    # 默认 27777
py -m server.api_server --port 9000 --refresh 2
```

Windows 下**双击项目根目录的 `start-board.bat`** 即可（自动查依赖、查端口占用）。
macOS / Linux 用 `./start-board.sh`（会自动建 `.venv`）。
关闭那个黑窗口就是停止服务。

> 该 .bat 用 GBK + CRLF 编码保存，才能让 cmd 正确显示中文。
> 如果用编辑器改它，记得保持 GBK，否则双击后中文会变成乱码。

> **Windows 下必须用 `py` 而不是 `python`。** 本机 `python` 指向 WorkBuddy 自带的
> 3.13（`~/.workbuddy/binaries/python/...`），里面没有 fastapi；
> `py` 指向 3.14（`AppData\Local\Programs\Python\Python314`），依赖装在这里。

> **pip 走阿里云镜像。** 直连 PyPI 在国内常被 SSL 中断
> （`SSLError: UNEXPECTED_EOF_WHILE_READING`），装不上 fastapi。
> `pip install -i https://mirrors.aliyun.com/pypi/simple --trusted-host mirrors.aliyun.com`

启动后：

| 地址 | 说明 |
|---|---|
| `http://127.0.0.1:27777/` | 演示页 |
| `http://127.0.0.1:27777/docs` | OpenAPI 文档 |
| `http://<LAN-IP>:27777/status` | ESP32 固件端点 |

## 接口

固件契约（**冻结**，改动会导致已烧录设备解析失败）：

```
GET /status     紧凑 JSON，响应体 < 4096 字节
```

通用接口：

```
GET /api/v1/board        聚合快照（演示页用这个）
GET /api/v1/summary      三格统计
GET /api/v1/sessions     会话列表  ?state= &project= &include_background= &limit=
GET /api/v1/tasks        任务列表  ?status= &since= &limit=
GET /api/v1/automations  自动化 + 最近 5 次运行
GET /api/v1/activity     今日代码改动
GET /api/v1/artifacts    产物列表  ?limit=
GET /api/v1/system       系统指标
GET /api/v1/health       数据源自检
```

## 三格统计口径

| 字段 | 定义 | SQL / 逻辑 |
|---|---|---|
| `working` | 近 30 分钟活跃的交互式会话 | `sessions` 表，排除软删除与后台自动化会话 |
| `today_done` | 今日完成的任务 | 本地账本 `state/task_ledger.json`，按 `completed_at >= 今日0点` |
| `automations` | 已启用自动化数 | `automations` 表 `status='ACTIVE'` |

## 五个必须知道的坑

改代码前先读这几条，都是实测踩出来的：

1. **`automations.deleted_at` 不是删除标记**。5 条记录全 `ACTIVE` 但 `deleted_at`
   全非空。加 `deleted_at IS NULL` 会得出 0，与事实相反。这张表禁止对该字段过滤。
2. **`automations.next_run_at` 全部过期**，不能用来筛"活跃自动化"。`ACTIVE` 只表示
   用户没禁用，不代表正在调度。
3. **`sessions.status` 大小写混用**（`completed`/`Completed`、`working`/`active`），
   查询一律 `LOWER()` 归一，且白名单外的状态不计数。
4. **毫秒时间戳可能是浮点**（实测 `1783644600000.0027`）。Pydantic 对 `int` 字段
   遇到浮点直接抛校验错 → 接口 500。统一走 `models._to_ms` 兜住。
5. **删任务就是删文件**。`tasks/*/*.json` 没有墓碑，AI 收尾清空任务清单后历史
   直接消失。所以完成数不能再从盘上现算，必须走 `sources/ledger.py` 账本
   （2026-10-26 踩过：当天做完 4 个任务，清单一清，屏上 `done` 归零）。
6. **项目名不能从文件路径猜**。changes-index 的**文件名就是会话 id**
   （实测 26/26 都能匹配到 `sessions.cwd`），改文件路径正则只会抓到
   `WorkBuddy/<时间戳>/` 这种沙箱目录（2026-10-26 踩过：屏上"涉及项目"
   显示成 `2026-10-25-23-57-47`）。正确做法是按会话 id 关联 `session.cwd`，
   再用 `models.project_name` 取末级目录；cwd 是沙箱目录则归为"临时工作区"。
7. **越界屏号返回 HTTP 200 + `{"ok":false}`**，不是 4xx。这是故意的：固件按
   `strstr(json, "\"ok\":true")` 判断成败（`firmware/main/main.cpp`），
   不看 HTTP 状态码。写自检脚本时别断言状态码。

## 与固件的契约

| 位置 | 常量 | 值 |
|---|---|---|
| `server/api_server.py` | `DEFAULT_PORT` | 27777 |
| `firmware/main/main.cpp` | `BRIDGE_BASE` 端口 | 27777 |
| `configure.py` | `DEFAULT_PORT` | 27777 |

改端口必须三处同步。

`configure.py` 写进 `config.local.h` 的宏名必须是 **`BRIDGE_BASE`**（只给 base，
路径由固件自己拼 `/status` 或 `/screen/<i>`）。固件不读 `BRIDGE_URL`。
`configure.py` 的探活端点是 **`/api/v1/health`**（没有裸 `/health` 路由，那是 404）。

## 架构

```
server/
├── api_server.py     路由 + 启动入口
├── collector.py      2s 定时采集 → 内存快照 → 降级
├── models.py         Pydantic 模型 + 文本截断工具
├── static/index.html 演示页
└── sources/
    ├── db.py         workbuddy.db 只读访问
    ├── files.py      tasks / changes / artifacts 的 JSON 扫描
    ├── ledger.py     任务账本（历史不随文件删除消失）
    └── system.py     psutil 指标
```

**所有接口只读内存快照，不在请求路径上查库。** 采集协程每 2s 刷新一次。
这样既满足 `/status` 50ms 延迟目标，也避免高频读去抢 WorkBuddy 主进程的库锁。

## 降级行为

任何数据源故障都不会返回 5xx，也不会让屏幕闪：

- 数据库不可用 → 该部分返回上次缓存，`/status` 的 `ok` 变 `false`，`error` 写原因
- `tasks/` 缺失 → 盘上任务读不到，但账本里的历史仍生效，不算故障
- 账本损坏 → 从空账本重建，任务统计退回"只看盘上"
- `psutil` 缺失 → 系统指标全 0，服务正常
- 单个 JSON 损坏 → 跳过该文件

## 验证

```bash
# 一键回归（端点矩阵 + 固件契约 + 数据源自检 + 详情屏 + 越界保护）
python scripts/check_api.py

# 演示页截图，确认真实渲染
python scripts/shot_board.py

# 单项手测
curl -s http://127.0.0.1:27777/status | wc -c          # 应 < 4096
curl -s http://127.0.0.1:27777/status | grep -c '"ok":true'
```

`scripts/shot_board.py` 跨平台：浏览器用 playwright 自带的 chromium，
端口默认 27777，可用 `BOARD_PORT=9000` 覆盖。
