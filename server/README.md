# WorkBuddy 看板后端

只读数据 API + 演示页。数据源是 WorkBuddy 自己维护的 `~/.workbuddy`，
不对其做任何写操作。

需求文档：[`docs/plans/2026-10-26-workbuddy-board-api-requirements.md`](../docs/plans/2026-10-26-workbuddy-board-api-requirements.md)

## 启动

```bash
py -m pip install -r server/requirements.txt
py -m server.api_server                    # 默认 8080
py -m server.api_server --port 9000 --refresh 2
```

Windows 下也可直接双击 `server/start.bat`（带依赖自检）。

> **必须用 `py` 而不是 `python`。** 本机 `python` 指向 WorkBuddy 自带的
> 3.13（`~/.workbuddy/binaries/python/...`），里面没有 fastapi；
> `py` 指向 3.14（`AppData\Local\Programs\Python\Python314`），依赖装在这里。

启动后：

| 地址 | 说明 |
|---|---|
| `http://127.0.0.1:8080/` | 演示页 |
| `http://127.0.0.1:8080/docs` | OpenAPI 文档 |
| `http://<LAN-IP>:8080/status` | ESP32 固件端点 |

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
| `today_done` | 今日转 completed 的任务 | 扫 `tasks/*/*.json`，`updatedAt >= 今日0点` |
| `automations` | 已启用自动化数 | `automations` 表 `status='ACTIVE'` |

## 四个必须知道的坑

改代码前先读这几条，都是实测踩出来的：

1. **`automations.deleted_at` 不是删除标记**。5 条记录全 `ACTIVE` 但 `deleted_at`
   全非空。加 `deleted_at IS NULL` 会得出 0，与事实相反。这张表禁止对该字段过滤。
2. **`automations.next_run_at` 全部过期**，不能用来筛"活跃自动化"。`ACTIVE` 只表示
   用户没禁用，不代表正在调度。
3. **`sessions.status` 大小写混用**（`completed`/`Completed`、`working`/`active`），
   查询一律 `LOWER()` 归一，且白名单外的状态不计数。
4. **毫秒时间戳可能是浮点**（实测 `1783644600000.0027`）。Pydantic 对 `int` 字段
   遇到浮点直接抛校验错 → 接口 500。统一走 `models._to_ms` 兜住。

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
    └── system.py     psutil 指标
```

**所有接口只读内存快照，不在请求路径上查库。** 采集协程每 2s 刷新一次。
这样既满足 `/status` 50ms 延迟目标，也避免高频读去抢 WorkBuddy 主进程的库锁。

## 降级行为

任何数据源故障都不会返回 5xx，也不会让屏幕闪：

- 数据库不可用 → 该部分返回上次缓存，`/status` 的 `ok` 变 `false`，`error` 写原因
- `tasks/` 缺失 → `today_done` 取 0，不算故障
- `psutil` 缺失 → 系统指标全 0，服务正常
- 单个 JSON 损坏 → 跳过该文件

## 验证

```bash
# 固件契约
curl -s http://127.0.0.1:8080/status | wc -c          # 应 < 4096
curl -s http://127.0.0.1:8080/status | grep -c '"ok":true'

# 接口矩阵
for ep in /status /api/v1/board /api/v1/health /api/v1/system; do
  curl -s -o /dev/null -w "$ep %{http_code} %{time_total}s\n" "http://127.0.0.1:8080$ep"
done

# 演示页截图
py scripts/shot_board.py
```
