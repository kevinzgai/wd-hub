# WorkBuddy 看板 API 服务 —— 需求文档

| 项 | 内容 |
|---|---|
| 版本 | v1.0 |
| 日期 | 2026-10-26 |
| 状态 | 待评审 |
| 定位 | 只读 API 层，不对前端负责 |
| 技术栈 | FastAPI + Uvicorn，不落库 |
| 现有实现 | `server/api_server.py`（标准库 demo，基准参照物） |

---

## 1. 背景

已有一条跑通的链路：ESP32-S3（星智 1.54" ST7789，240×240）WiFi 联网 → 每 5s 轮询 `http://<PC>:8080/status` → 解析紧凑 JSON → LVGL 渲染。固件与屏幕侧全部验证通过。

但服务端 `server/api_server.py` 只是 demo，存在三个硬伤：

1. **口径不对**。屏幕三格统计里"工作数"是数 `python.exe`/`node.exe` 进程个数，"完成数"是数今天被改过的文件数（包含我们自己的 build 产物），"自动化数"是猜的一个不存在的目录。三个数字都不反映真实工作状态。
2. **数据源浅**。WorkBuddy 自己维护着 `~/.workbuddy/workbuddy.db`（SQLite，含 `sessions` / `automations` / `automation_runs` 三张真表）和 `tasks/*/*.json`（330 个任务文件），demo 一个都没用。
3. **不可扩展**。标准库 `http.server` 手写路由，加第二个接口就要复制粘贴分支判断，无参数校验、无自动文档、无并发能力。

本需求目标：把它升级成一个**正式但克制的只读 API 服务**，接真实数据、口径可解释、接口可扩展，同时**不破坏已烧录固件的既有契约**。

---

## 2. 目标与非目标

### 2.1 目标

| # | 目标 | 判定方式 |
|---|---|---|
| G1 | 屏幕三格统计反映真实工作状态 | 每个数字都能查到原始记录 |
| G2 | 不重烧固件即可上线 | `/status` 字节级兼容（见 §5.1） |
| G3 | 提供可被任意前端消费的 REST API | 有 OpenAPI 文档，纯 GET |
| G4 | 数据源不可用时优雅降级 | 断源不 500、不闪屏清零 |
| G5 | 单文件级部署复杂度 | `pip install` + 一条命令启动 |

### 2.2 非目标（明确不做）

- **不做前端页面**。除保留一个设备预览调试页外，不写任何 UI。
- **不落库**。不建自己的 SQLite，不做历史趋势、不做曲线。聚合结果只存内存。
- **不做写操作**。所有端点 pure GET，不接受任何状态变更。
- **不做多用户 / 权限体系**。单机单用户场景。
- **不改固件**。固件侧任何改动都不在本需求范围内。

---

## 3. 数据源与实测约束

### 3.1 数据源清单（已实测）

| 源 | 路径 | 实测规模 | 用途 |
|---|---|---|---|
| 会话库 | `~/.workbuddy/workbuddy.db` → `sessions` | 235 行 | 工作数、会话列表 |
| 自动化 | 同库 → `automations` | 5 行（全 ACTIVE） | 自动化数 |
| 自动化运行 | 同库 → `automation_runs` | 72 行（PENDING_REVIEW 43 / ACCEPTED 29） | 自动化最近状态 |
| 用量 | 同库 → `session_usage` | 94 行 | 上下文占用 |
| 工作区 | 同库 → `workspaces` | 54 行 | 项目名映射 |
| 任务 | `~/.workbuddy/tasks/<convId>/<n>.json` | 330 个文件，284 已完成 | 完成数、任务列表 |
| 产物 | `~/.workbuddy/artifact-index/*.json` | 167 个 | 产物统计 |
| 变更 | `~/.workbuddy/changes-index/*.json` | 110 个 | 代码改动行数 |
| 变更详情 | `~/.workbuddy/changes-detail/<convId>/` | 110 个目录 | diff 明细（暂不用） |
| 系统指标 | `psutil` | 运行时 | CPU / 内存 / 磁盘 / 温度 |

**关键结论：优先读 SQLite，不要解析 JSONL。** `projects/*/*.jsonl` 是完整对话流水（单会话 1270 行），体积大、结构随模型版本漂移，只用于兜底。

### 3.2 必须遵守的硬约束

这些是实测踩到的，写错就是 bug：

| # | 约束 | 原因 / 现状 |
|---|---|---|
| C1 | **响应总长 < 4096 字节** | 固件 `HTTP_BUF_SIZE 4096`，超出部分被静默截断 → JSON 不完整 → 解析出脏数据 |
| C2 | **`"ok":true` 必须无空格** | 固件用 `strstr(json, "\"ok\":true")` 判断成败，带空格直接判定失败 |
| C3 | `clock` ≤ 7 字节、`date` ≤ 7 字节且**纯 ASCII** | 固件 buffer 是 `char[8]`，中文一字符 3 字节会溢出 |
| C4 | `title`≤40B / `project`≤20B / `elapsed`≤12B | 固件结构体固定长度 |
| C5 | 按字节截断，**不能切碎 UTF-8** | 半个中文字符会被 LVGL 渲染成乱码 |
| C6 | **`sessions.status` 大小写混用** | 实测同时存在 `completed`(158) 与 `Completed`(2)；`working`(4) 与 `active`(1)。查询必须 `LOWER()` 归一 |
| C7 | **`sessions` 必须过滤软删除** | 103 条 `deleted_at IS NOT NULL`（其中 completed 98 / working 2 / error 3），不过滤会把废弃会话算进统计 |
| C8 | **必须排除后台自动化会话** | `is_background_automation=1` 有 59 条，这些是自动化跑出来的会话，不是"人在干活" |
| C9 | **SQLite 是 WAL 模式 + 可能被占用** | 必须用只读连接 `file:...?mode=ro`，并设 `busy_timeout`；打开失败要降级，不能崩 |
| C10 | 所有时间戳是**毫秒 epoch** | "今日"必须按本地时区（CST-8）算，不能用 UTC 截断 |
| C11 | **`automations.deleted_at` 不是删除标记** | 实测 5 条记录 `status` 全为 `ACTIVE`、`deleted_at` 却**全部非空**。在此表上套用常规软删除过滤（`deleted_at IS NULL`）会得到 0，与事实相反。**禁止对该字段做过滤** |
| C12 | **`automations.status` 不可信，需按活性判断** | 5 条虽标 `ACTIVE`，但 `next_run_at` 分别是 2026-05-29 / 05-29 / 07-10 / 09-20 / 09-21，相对当前（10-26）**全部过期**；`last_run_at` 最新一条是 09-20。说明"ACTIVE"只表示未被用户禁用，不代表正在调度 |
| C11 | 字库只烘焙了固定字符集 | 服务端下发的中文可能不在字库 → 固件已有 `utf8_sanitize()` 降级为 `?`，服务端不必额外处理，但**不要下发 emoji**（彩色 emoji 渲染为黑块） |

---

## 4. 统计口径（核心需求）

屏幕三格数字必须可解释、可复现。定义如下：

### 4.1 `working` — 工作数

> **近 30 分钟内有过活动的交互式会话数**

```sql
SELECT COUNT(*) FROM sessions
WHERE LOWER(status) IN ('working', 'active')
  AND deleted_at IS NULL
  AND (is_background_automation IS NULL OR is_background_automation = 0)
  AND last_activity_at >= :now_ms - 30*60*1000
```

**为什么加 30 分钟窗口**：`status='working'` 会残留（崩了不改状态）。实测不过滤窗口时能查到 3 条（含 1 条 `active` 状态、`last_activity_at` 是 2026-10-14 的陈年残留），30 分钟窗口能把它自动排除。已实测：加窗口后返回 **2** 条，正好是本机在跑的两个会话。

**已验证**：该 SQL 在真实库上返回 2 条，标题分别为「是否需要安装 ESP-IDF 开发工具」与「规划 workbuddy 看板 API 服务需求」，符合预期。

### 4.2 `today_done` — 完成数

> **今日被标记为 completed 的任务数**

```python
今日0点毫秒 = 本地时区今日 00:00:00 的 epoch_ms
遍历 ~/.workbuddy/tasks/*/*.json
  条件: status == 'completed' and updatedAt >= 今日0点毫秒
  计数
```

**注意**：实测当前该值为 **0**。这是真实情况（今天的任务都还没到 completed 状态），不是 bug。文档写清楚，免得联调时误判。

**为什么不用 `changes-index` 的改动文件数**：会把 `build/` 里 700 个 `.obj` 也算进去，数字虚高且毫无意义。

### 4.3 `automations` — 自动化数

> **已启用的自动化任务数**

```sql
SELECT COUNT(*) FROM automations WHERE status = 'ACTIVE'
```

**注意：绝对不要加 `deleted_at IS NULL`。** 实测该表 5 条记录 `status` 全为 `ACTIVE` 但 `deleted_at` 全非空，加上过滤会变成 0（错误结果）。该字段在本表是同步占位，不具备删除语义（见 C11）。

**不要用 `next_run_at` 反推"活跃"**：5 条 `next_run_at` 全部早于当前时间（最早 2026-05-29，最晚 2026-09-21），如果按"未来还会跑"筛，结果同样是 0。`next_run_at` 只是上次调度时算出的时间点，任务停用后不再更新（见 C12）。

**结论：屏幕上的"自动化数"= `status='ACTIVE'` 的条数 = 5。** 这是"配置了几个自动化"的中性事实，不承诺它们当下在跑。

附带的运行态统计单独放进 v1 接口，不与屏幕数字混淆：

```sql
-- 今日已跑次数 / 失败数（automation_runs.status: ACCEPTED / PENDING_REVIEW）
SELECT COUNT(*) FROM automation_runs WHERE created_at >= :今日0点毫秒
```

实测今日运行数 = 0（最近一次运行是 2026-09-20），`failed_today` 同样为 0。这是真实情况，不是 bug。

### 4.4 口径实测基线（2026-10-26 11:25 采样）

实现完成后必须能复现下表。数字对不上就是口径写错了，不要归因于"数据在变"。

| 指标 | 实测值 | 备注 |
|---|---|---|
| `working` | **2** | 近 30 分钟活跃的交互会话 |
| `today_done` | **0** | 今日无任务转为 completed |
| `automations` | **5** | `status='ACTIVE'` 全部条数 |
| `sessions` 总行数 | 235 | 含 103 条软删除 |
| `sessions` 有效行（未删除） | 132 | archived 67 / completed 62 / working 2 / active 1 |
| `tasks` 文件总数 | 330 | 累计 completed 284 |
| `automation_runs` 总行数 | 72 | PENDING_REVIEW 43 / ACCEPTED 29 |
| 今日自动化运行 | 0 | 最近一次 2026-09-20 |

---

## 5. 接口设计

Base URL：`http://<host>:8080`。所有端点 `GET`，无鉴权（见 §6.4）。

### 5.1 `GET /status` —— 固件契约（**冻结，禁止改动**）

响应体（紧凑 JSON，无任何空格）：

```json
{"ok":true,"error":"","clock":"11:22","date":"10-26","working":2,"today_done":0,"automations":5,"sessions":[{"title":"开发看板固件","project":"firmware","elapsed":"5m"}]}
```

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| `ok` | bool | 序列化为 `"ok":true` | 成败标志，必须无空格 |
| `error` | string | ≤63B | 失败原因，成功时为空串 |
| `clock` | string | 5B，`HH:MM` | 24 小时制 |
| `date` | string | 5B，`MM-DD`，纯 ASCII | **不得出现中文** |
| `working` | int | ≥0 | §4.1 |
| `today_done` | int | ≥0 | §4.2 |
| `automations` | int | ≥0 | §4.3 |
| `sessions` | array | 最多 3 项 | 最近活动的会话 |
| `sessions[].title` | string | ≤40B | 会话标题 |
| `sessions[].project` | string | ≤20B | 项目名（取 cwd 末级目录） |
| `sessions[].elapsed` | string | ≤12B | `5m` / `2h` / `3d` 形式 |

**附加硬性要求**：整包 ≤ 4096 字节（C1）。**实测最坏情况 410 字节**（3 条会话，标题/项目名全部顶满字节上限，三格数字取三位数），余量约 10 倍，不存在溢出风险。实现时仍须断言长度，防止未来加字段时突破。

**契约冻结的含义**：字段名、顺序、类型、紧凑序列化方式全部保持。新增字段一律进 `/api/v1/*`，不得往这里加。

### 5.2 `GET /api/v1/board` —— 看板聚合快照

`/status` 的超集，给 Web / H5 用。同样走缓存，字段完整可读。

```json
{
  "generated_at": "2026-10-26T11:22:03+08:00",
  "cache_age_ms": 820,
  "summary": { "working": 2, "today_done": 0, "automations": 5 },
  "sessions": [
    { "id": "...", "title": "规划看板 API 服务", "project": "xiaozhiLed",
      "status": "working", "model": "deepseek-v4.1-flash",
      "last_activity_at": 1792984559065, "elapsed": "5m" }
  ],
  "tasks": { "total": 330, "completed": 284, "today_completed": 0 },
  "automations": { "active": 5, "runs_today": 0, "failed_today": 0 },
  "system": { "cpu": 12, "mem": 47, "disk": 61, "temp": 55 }
}
```

### 5.3 明细接口

| 方法 | 路径 | 查询参数 | 返回 | 消费端 |
|---|---|---|---|---|
| GET | `/api/v1/sessions` | `state`、`project`、`limit`(默认20，≤100)、`offset` | 会话分页列表 | Web |
| GET | `/api/v1/tasks` | `status`、`since`、`limit` | 任务列表 | Web |
| GET | `/api/v1/automations` | — | 自动化列表 + 最近 5 次运行 | Web |
| GET | `/api/v1/activity` | `date`(默认今日) | 改动文件数 / 增删行数 / 涉及项目 | Web |
| GET | `/api/v1/artifacts` | `limit` | 产物列表（路径、类型、大小、时间） | Web |
| GET | `/api/v1/system` | — | CPU / 内存 / 磁盘 / 温度 / 进程数 | 监控 |
| GET | `/api/v1/health` | — | 各数据源可达性 + 缓存年龄 + 版本 | 监控 |
| GET | `/` | — | 设备预览调试页（240×240 模拟） | 人工 |
| GET | `/docs` | — | OpenAPI（FastAPI 自带） | 开发 |

**参数校验统一走 FastAPI 的 `Query(...)`**：越界返回 422，不静默截断。

### 5.4 保留但降级的旧端点

| 路径 | 处理 |
|---|---|
| `/api/status` | 保留，标记 deprecated。demo 时期的系统指标接口，前端若有用可继续用 |
| `/api/ticker` | 保留，返回纯文本滚动条内容。后续可支持从配置读取 |

### 5.5 错误响应格式

```json
{ "ok": false, "error": "code", "detail": "人话描述", "source": "workbuddy.db" }
```

| HTTP | code | 场景 |
|---|---|---|
| 200 | — | 正常 |
| 200 | `degraded` | 部分数据源不可用，返回上次缓存（**不降级为 5xx**，理由见 §6.3） |
| 422 | `invalid_param` | 参数越界 |
| 404 | `not_found` | 路径不存在 |
| 503 | `no_data` | 服务刚起、缓存尚未建立 |

---

## 6. 非功能需求

### 6.1 性能

| 指标 | 要求 | 理由 |
|---|---|---|
| `/status` P95 延迟 | < 50ms | 固件超时 5s，但要留余量 |
| `/api/v1/*` P95 延迟 | < 200ms | Web 体验 |
| 数据采集周期 | 2s | 屏幕 5s 刷新，2s 采集足够且不会过度开销 |
| 内存常驻 | < 80MB | 常驻后台服务 |
| CPU 均值 | < 3% | 用户明确要求 CPU 占用低 |

**实现约束**：**所有接口只读内存缓存，不在请求路径上查库**。后台协程每 2s 刷新一次快照，请求直接返回最新快照。这是满足上述延迟与 CPU 指标的唯一简单方案。

### 6.2 缓存

- 单份 `Snapshot` 对象，`asyncio.Lock` 保护读写。
- 每次刷新记录 `generated_at`，接口返回 `cache_age_ms`。
- `cache_age_ms > 15000`（约 7 个采集周期没成功）→ `/api/v1/health` 报 `stale`，`/status` 保留旧数据只把 `ok` 置 `false`。

### 6.3 容错（关键）

**原则：任何数据源故障都不能让屏幕显示错误，也不能 500。**

| 故障 | 行为 |
|---|---|
| `workbuddy.db` 打不开 | 该部分返回上次缓存值；`/status` 的 `ok` 变 `false`，`error` 写原因（截断 63B） |
| 数据库被锁（`database is locked`） | 重试 1 次（`busy_timeout=200ms`），仍失败则跳过本轮刷新 |
| `tasks/` 目录不存在 | `today_done` 取 0，不报错 |
| `psutil` 未安装 | 系统指标全 0，服务正常运行 |
| 单个 JSON 文件损坏 | 跳过该文件，继续扫，不中断 |
| 服务刚启动 | 启动时**同步**建立首份快照后再监听端口，避免首屏显示空数据 |

**为什么降级不报 5xx**：固件逻辑是"HTTP 非 200 或解析失败 → 沿用旧数据、状态点变红"。返回 5xx 只会让屏幕进入离线态，而降级返回旧数据 + `ok:false` 能同时保住画面和状态提示。

### 6.4 安全

- 仅监听内网，**不加鉴权**（本需求范围外）。启动时打印所有 LAN IP 便于配置固件。
- 配置项（`--port`、`--host`、`--db`、`--refresh`）通过命令行参数与环境变量注入，不硬编码路径。
- **只读数据库**：`mode=ro`，物理上禁止写入（避免 WAL 模式下误锁 WorkBuddy 主进程）。
- 日志只打 API 请求行与错误，不打数据内容。

### 6.5 可运维性

- 一条命令启动：`python -m server.api_server`（或 `uvicorn server.api_server:app`）。
- 启动时打印：监听地址、LAN IP、检测到的数据源及条数、字库/固件契约版本。
- 优雅关闭：SIGINT/SIGTERM 停采集协程、关连接。
- 结构化日志，单行一条，含时间戳与级别。

---

## 7. 技术方案

### 7.1 目录结构

```
server/
├── api_server.py          # FastAPI app + 路由 + 启动入口
├── collector.py           # 数据采集（各数据源 → Snapshot）
├── models.py              # Pydantic 响应模型 + 截断工具
└── sources/
    ├── db.py              # workbuddy.db 只读访问
    ├── tasks.py           # tasks/*/*.json 扫描
    ├── changes.py         # changes-index 聚合
    └── system.py          # psutil 指标
```

单文件也能写完，但拆开更好测。**上限 5 个模块，不再细拆** —— 这是内网小服务，不是产品。

### 7.2 关键实现点

1. **采集器**：`asyncio.create_task` 起一个 `while True: refresh(); await sleep(2)` 的协程。DB 查询用 `asyncio.to_thread` 包一层，避免阻塞事件循环。
2. **截断工具**：`utf8_trunc(s, n)` 复用现有实现（按字节切、`errors="ignore"` 收尾），必须单元测试中文边界。
3. **项目名提取**：`cwd` 末级目录名，Windows 路径要处理 `\` 和 `/` 两种分隔符。
4. **elapsed 计算**：`<60s→"Ns"`、`<1h→"Nm"`、`<1d→"Nh"`、否则 `"Nd"`，保持现有格式。
5. **今日边界**：`datetime.now().astimezone().replace(hour=0,...)` 本地时区，不要用 `utcfromtimestamp`。

### 7.3 依赖

```
fastapi
uvicorn[standard]
psutil        # 可选，缺失时降级
```

三项。不引入 ORM、不引入 SQLAlchemy —— 直接 `sqlite3` 足够。

### 7.4 兼容策略

部署后 `curl http://127.0.0.1:8080/status` 的输出与当前 demo 输出**逐字节可 diff**（除时间/数据本身外，格式完全一致）。这是上线的唯一硬门槛。

---

## 8. 实施计划

### P0 — 数据层与契约冻结（核心，不可省）

1. `sources/db.py`：只读连接、`LOWER()` 归一、软删除过滤、后台会话排除
2. `sources/tasks.py`：任务扫描 + 今日完成计数
3. `collector.py`：Snapshot 聚合 + 2s 定时刷新 + 缓存锁
4. `api_server.py`：`/status` 严格按 §5.1 输出
5. **验收**：`curl` 输出与 demo 格式一致；断网/锁库/删目录三种故障不崩

### P1 — v1 只读接口

6. `models.py`：Pydantic 模型，全部字段加 `description`（吃 OpenAPI 的红利）
7. `/api/v1/board` 聚合接口
8. `/api/v1/sessions` / `tasks` / `automations` / `system` 明细接口
9. `/api/v1/health` 数据源自检
10. 调试预览页迁移到 FastAPI 模板

### P2 — 打磨

11. `/api/v1/activity` + `/api/v1/artifacts`（读 110/167 个 JSON，注意加 mtime 缓存）
12. 结构化日志、优雅关闭、启动横幅
13. 单元测试：截断、口径 SQL、降级路径

### 交付判定

P0 完成即可替换现有 demo 上线（固件无需改动）。P1/P2 可分期落地，不影响设备。

---

## 9. 验收标准

| # | 场景 | 期望 |
|---|---|---|
| A1 | 设备正常联网 | 屏幕显示真实工作数/完成数/自动化数，5s 刷新，无方框 |
| A2 | `curl /status` | 紧凑 JSON，`"ok":true` 无空格，无中文出现在 `date` |
| A3 | 响应体积 | < 4096 字节（用 `wc -c` 验证） |
| A4 | 停掉 WorkBuddy | 屏幕保留最后数据，右上角状态点变红，服务不崩 |
| A5 | 数据库文件被删 | 服务正常响应，`/api/v1/health` 明确报 `db: unreachable` |
| A6 | 会话数为 0 | 屏幕显示"空闲中"，不显示 `0` 也不空白 |
| A7 | 中文超长标题 | 截断后无乱码、无半个汉字、无崩溃 |
| A8 | `today_done` | 与实际 `tasks` 目录完成数一致（可脚本比对） |
| A9 | `/docs` | 可访问且所有接口有参数说明 |
| A10 | 常驻 1 小时 | CPU 均值 < 3%，内存 < 80MB |

---

## 10. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| WorkBuddy 内部 DB schema 升级 | 查询报错 | 所有查询包 `try/except`，schema 变化只影响对应字段，`/status` 继续用缓存 |
| `sessions.status` 出现新状态值 | 统计偏差 | 用白名单 + `LOWER()`，未知状态不计数（宁可少算不可乱算） |
| 4096 字节被突破 | 屏幕解析脏数据 | 序列化后强制断言长度，超限则裁掉会话列表（保三格数字） |
| 用户期望"完成数"是代码行数 | 认知偏差 | 本文档 §4 明确定义，界面用 `✓ 完成` + 任务数语义 |
| 内网端口被外网访问 | 数据泄露 | 不加鉴权是已知取舍；仅监听内网 + 防火墙限制 |

---

## 11. 待决问题

1. `/api/v1/activity` 的"今日改动"是否要排除 `build/`、`node_modules/`？**建议排除**，否则数字全部被编译产物污染。
2. 会话标题为空时（`title` 和 `custom_title` 都 null）回退显示什么？**建议**用 `cwd` 末级目录 + 会话 ID 前 8 位。
3. 是否需要 `/api/v1/usage` 暴露 `session_usage`（上下文占用）？当前 94 行数据，可做但屏幕用不上。**建议 P2 再说**。
