"""WorkBuddy 看板后端 —— FastAPI 实现。

接口分两层：

  1. `/status`  —— 固件契约，**冻结**。字段名、顺序、紧凑序列化方式都不能动。
     固件用 strstr("\"ok\":true") 判断成败，且缓冲只有 4096 字节。

  2. `/api/v1/*` —— 给 Web / H5 / 监控用的完整 REST 接口。

所有接口只读内存快照，不在请求路径上查库。设计说明见 collector.py。

启动：
    py -m server.api_server
    py -m server.api_server --port 27777 --refresh 2
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import socket
import sys
import time
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from typing import Any, Optional

from fastapi import FastAPI, Query
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, Response

# 兼容 `py server/api_server.py` 直接执行（不经 -m）
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from server.collector import STALE_MS, Collector, iso_now
    from server.models import (
        ActivityStat, ArtifactListResponse, AutomationItem, AutomationListResponse,
        BoardResponse, HealthResponse, SessionItem, SessionListResponse,
        SourceHealth, Summary, SystemStat, TaskItem, TaskListResponse, TaskStat,
        human_ago, project_name, session_title, utf8_trunc,
    )
    from server.sources import db as db_src
    from server.sources import files as files_src
    from server.sources import system as sys_src
else:
    from .collector import STALE_MS, Collector, iso_now
    from .models import (
        ActivityStat, ArtifactListResponse, AutomationItem, AutomationListResponse,
        BoardResponse, HealthResponse, SessionItem, SessionListResponse,
        SourceHealth, Summary, SystemStat, TaskItem, TaskListResponse, TaskStat,
        human_ago, project_name, session_title, utf8_trunc,
    )
    from .sources import db as db_src
    from .sources import files as files_src
    from .sources import system as sys_src

VERSION = "1.0.0"
log = logging.getLogger("board.api")

# 默认端口。刻意不用 8080（开发机上太容易撞车）。
# 改这个值必须同步改三处：
#   firmware/main/main.cpp 的 BRIDGE_BASE
#   start-board.bat         里的 netstat 检查与 --port
#   configure.py            里的默认端口
DEFAULT_PORT = 27777

# 固件 HTTP_BUF_SIZE = 4096，超了会被静默截断 → JSON 不完整 → 解析出脏数据
FIRMWARE_MAX_BYTES = 4096
MAX_SESSION_ROWS = 3

# 屏幕页数：0 = 概览（/status），1..N-1 = 详情屏（/screen/<i>）。
# 改这里就改了设备上能按出几屏，**不用重烧固件** —— 固件从 /status 的 nscr 读。
SCREEN_COUNT = 5

TICKER_TEXT = "WorkBuddy 看板 · 数据每 2 秒采集 · 屏幕每 5 秒刷新"

collector = Collector()


@asynccontextmanager
async def lifespan(app: FastAPI):
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-7s %(name)s | %(message)s",
        datefmt="%H:%M:%S",
    )
    sys_src.prime()
    log.info("启动采集器（周期 %.1fs）", collector.refresh_sec)
    await collector.start()
    yield
    log.info("停止采集器")
    await collector.stop()


app = FastAPI(
    title="WorkBuddy 看板 API",
    version=VERSION,
    description="只读看板数据服务。`/status` 为 ESP32 固件契约（冻结），"
                "`/api/v1/*` 为通用接口。全部端点均为 GET，无写操作。",
    lifespan=lifespan,
)


@app.exception_handler(Exception)
async def _unhandled(request, exc: Exception):
    """兜底：任何未预料的异常都返回结构化 JSON，别让前端拿到裸 500 文本。"""
    log.exception("未处理异常 %s %s", request.method, request.url.path)
    return JSONResponse(
        status_code=500,
        content={"ok": False, "error": "internal",
                 "detail": f"{type(exc).__name__}: {exc}"[:200],
                 "source": None},
    )


# ---------------------------------------------------------------- 工具


def _session_item(row: dict) -> SessionItem:
    last = int(row.get("last_activity_at") or 0)
    now_ms = int(time.time() * 1000)
    return SessionItem(
        id=row["id"],
        title=session_title(row.get("title"), row.get("custom_title"),
                            row.get("cwd"), row["id"]),
        project=project_name(row.get("cwd")),
        status=str(row.get("status") or "unknown").lower(),
        model=row.get("model"),
        elapsed=human_ago((now_ms - last) / 1000 if last else 0),
        last_activity_at=last,
        is_background=bool(row.get("is_background_automation")),
    )


def _board_payload(snap) -> dict:
    now_ms = int(time.time() * 1000)
    age = now_ms - snap.generated_at_ms if snap.generated_at_ms else -1
    return {
        "generated_at": iso_now(snap.generated_at_ms or None),
        "cache_age_ms": age,
        "degraded": snap.degraded,
        "summary": {
            "working": snap.working,
            "today_done": snap.today_done,
            "automations": snap.automations,
        },
        "sessions": [_session_item(r) for r in snap.sessions],
        "tasks": snap.tasks,
        "automations": {"active": snap.automations, **snap.automation_runs},
        "system": snap.sys,
        "activity": snap.activity,
    }


WAIT_IDLE_MS = 3 * 60 * 1000     # 停止活动多久才算"在等人"


def _waiting_flag(row: dict, pending_convs: set[str]) -> bool:
    """判定单个会话是否「等待操作」—— 固件据此闪黄灯。

    口径（用户 2026-10-26 确认）：**该会话里还有 pending 任务，并且已经停止
    活动超过 3 分钟** → 活没干完却停住了，明显是在等人推进。

    ⚠ 为什么不能写"会话不在 working 就算等待"：`snap.sessions` 只装
    working/active 的近 30 分钟会话，那个条件永远为假，灯永远不会亮。

    ⚠ 口径只在这里定义，固件只认 `w` 字段。以后换判断条件（改成出错才算、
    或加上 automation_runs.status='PENDING_REVIEW' 等）改这个函数就行，
    **不用重烧固件**。
    """
    sid = str(row.get("id") or "")
    if sid not in pending_convs:
        return False
    last = int(row.get("last_activity_at") or 0)
    if last <= 0:
        return False
    return int(time.time() * 1000) - last >= WAIT_IDLE_MS


def _firmware_session(row: dict, pending_convs: set[str]) -> dict:
    """单行会话的固件载荷。

    `e` = 距最近活动的**原始秒数**（2026-10-27 加）。只发 `elapsed` 那种成品
    字符串的话，板子只能跟着 5 秒轮询一起跳字，看着像卡住；发了原始秒数，
    固件用本地单调时钟自己累加，秒针就能一秒一跳。

    ⚠ `e` 必须和 `elapsed` 并存下发：旧固件只认 `elapsed`，删掉它那批设备
    会显示空白。固件侧同样保留字符串作为 `e` 缺失时的回退。

    ⚠ 这里不能再报 `last_activity_at` 的毫秒原值：固件 `json_int` 走 atoi，
    32 位装不下 1.7e12 的 epoch 毫秒，会解析成垃圾。传"差了多少秒"既小又够用。
    """
    item = _session_item(row)       # 一次算完，原先是 title/project/elapsed 各调一次
    last = item.last_activity_at
    age_s = int((time.time() * 1000 - last) / 1000) if last else 0
    out = {
        "title": utf8_trunc(item.title, 40),
        "project": utf8_trunc(project_name(row.get("cwd")), 20),
        "elapsed": utf8_trunc(item.elapsed, 12),
        "e": max(0, age_s),
    }
    # w=1 表示等待操作，固件闪黄灯。不等待时**不发这个键**，省字节
    if _waiting_flag(row, pending_convs):
        out["w"] = 1
    return out


def _pct(v) -> int:
    """百分比取整并夹到 0..100。psutil 缺失时 system_stat() 给 0.0，原样下发。"""
    try:
        return max(0, min(100, int(round(float(v or 0)))))
    except (TypeError, ValueError):
        return 0


def _firmware_payload(snap) -> dict:
    now = datetime.now()
    sysd = getattr(snap, "sys", None) or {}
    # 扫盘有 3s TTL + mtime 签名缓存，放大到 5s 轮询频率上开销可忽略
    try:
        pending_convs = files_src.tasks_pending_by_conversation()
    except Exception:      # 任务目录不可读不该拖垮整个 /status
        pending_convs = set()
    return {
        "ok": not snap.degraded,
        # 固件 error 字段 char[64]，按字节截断（纯 ASCII 原因码，不含中文）
        "error": ("; ".join(snap.errors))[:63] if snap.degraded else "",
        "clock": now.strftime("%H:%M"),
        "date": now.strftime("%m-%d"),          # 必须纯 ASCII：固件 buffer 只有 8 字节
        "working": snap.working,
        "today_done": snap.today_done,
        "automations": snap.automations,
        "sessions": [
            _firmware_session(r, pending_convs)
            for r in snap.sessions[:MAX_SESSION_ROWS]
        ],
        # 设备（本机）指标：页脚状态条。全部取整下发——屏幕只有 12px 字体，
        # 小数位纯属浪费字节。旧固件不认这些键，直接忽略，向后兼容。
        "cpu":  _pct(sysd.get("cpu")),
        "mem":  _pct(sysd.get("mem")),
        "disk": _pct(sysd.get("disk")),
        "temp": int(sysd.get("temp") or 0),   # °C，取不到为 0，固件据此隐藏
        # 设备上能按出几屏。固件据此循环，改屏数不用重烧固件
        "nscr": SCREEN_COUNT,
    }


# ---------------------------------------------------------------- 固件契约


MAX_SCREEN_ROWS = 6      # 与固件 MAX_ROWS 一致，超出的行设备也放不下


def _fmt_uptime(sec) -> str:
    """秒 → 2h07m / 12m30s。屏幕上只有十来个字符，不要 00:00:00 那种。"""
    try:
        sec = max(0, int(sec or 0))
    except (TypeError, ValueError):
        return "-"
    if sec >= 3600:
        return f"{sec // 3600}h{(sec % 3600) // 60:02d}m"
    if sec >= 60:
        return f"{sec // 60}m{sec % 60:02d}s"
    return f"{sec}s"


def _row(label, value=None, color: int = 0) -> dict:
    """详情屏的一行。label 走 16px 主字体，value 走 12px 右对齐，color 交给固件映射。

    color: 0 普通 / 1 强调蓝 / 2 成功绿 / 3 警告黄 / 4 暗灰。
    空值不写 v，固件据此把这一行渲染成纯文本（没有右侧数字）。
    """
    r: dict[str, Any] = {"t": str(label)}
    if value is not None and str(value) != "":
        r["v"] = str(value)
    if color:
        r["c"] = color
    return r


def _screen_rows(snap, idx: int) -> tuple[str, list[dict]]:
    """详情屏内容。**全部基于内存快照**，不在请求路径查库（见 README 架构说明）。

    这里就是设备的全部"第二屏"：想改显示什么、加减屏，只改这个函数即可。
    """
    if idx == 1:                                    # 任务
        t = snap.tasks or {}
        ip  = int(t.get("in_progress") or 0)
        return "任务", [
            _row("总计",     t.get("total")),
            _row("已完成",   t.get("completed"), 2),
            # 用 snap.today_done 而不是 tasks.today_completed：
            # 后者口径不同（实测恒 0），会和概览屏的"完成"数字打架
            _row("今日完成", snap.today_done, 2),
            _row("进行中",   ip, 1 if ip else 4),
        ]

    if idx == 2:                                    # 自动化
        a = snap.automation_runs or {}
        fail = int(a.get("failed_today") or 0)
        pend = int(a.get("pending_review") or 0)
        rows = [
            _row("已启用",   snap.automations, 1),
            _row("今日运行", a.get("runs_today"), 2),
            _row("失败",     fail, 3 if fail else 4),
            _row("待审阅",   pend, 3 if pend else 4),
        ]
        last = a.get("last_run_at")
        if last:
            # human_ago() 吃的是**秒**，这里必须先算差值，
            # 直接丢毫秒戳进去会得出 20716000d 这种鬼数字
            age = (int(time.time() * 1000) - int(last)) / 1000.0
            rows.append(_row("最近", human_ago(age), 4))
        return "自动化", rows

    if idx == 3:                                    # 今日活动
        act = snap.activity or {}
        files = int(act.get("files") or 0)
        rows = [
            _row("改动文件", files, 1 if files else 4),
            _row("新增行",   act.get("additions"), 2),
            _row("删除行",   act.get("deletions"), 3),
        ]
        # 20 字符上限：16px 字体下 19 个 ASCII 大约 152px，塞得进 170px 的行宽
        for p in (act.get("projects") or [])[:2]:
            rows.append(_row(utf8_trunc(str(p), 20), None, 4))
        # 标题只留"月-日"，完整 2026-10-26 会顶到右边界
        day = str(act.get("date") or "")
        return f"今日 {day[5:]}" if len(day) >= 10 else "今日", rows

    if idx == 4:                                    # 系统
        sy   = getattr(snap, "sys", None) or {}
        cpu  = _pct(sy.get("cpu"))
        mem  = _pct(sy.get("mem"))
        disk = _pct(sy.get("disk"))
        return "系统", [
            _row("CPU",  f"{cpu}%",  3 if cpu  >= 85 else 1),
            _row("内存", f"{mem}%",  3 if mem  >= 90 else 1),
            _row("磁盘", f"{disk}%", 3 if disk >= 90 else 1),
            _row("进程", sy.get("proc")),
            _row("运行", _fmt_uptime(sy.get("uptime")), 4),
        ]

    return "", []


@app.get(
    "/screen/{idx}",
    summary="固件详情屏（第 1..N-1 屏）",
    description="设备按键切屏时按需拉取。字段与 /status 的公共部分一致"
                "（ok/error/clock/date/cpu/mem/disk），额外给 t（页标题）和 rows（最多 6 行）。"
                "第 0 屏（概览）仍走 /status。",
    tags=["firmware"],
)
async def screen_endpoint(idx: int):
    snap = await collector.snapshot()
    if idx < 1 or idx >= SCREEN_COUNT:
        body = json.dumps({"ok": False, "error": f"bad screen {idx}"},
                          separators=(",", ":"))
        return Response(content=body.encode("utf-8"),
                        media_type="application/json; charset=utf-8",
                        headers={"Cache-Control": "no-store"})

    sysd = getattr(snap, "sys", None) or {}
    title, rows = _screen_rows(snap, idx)
    now = datetime.now()
    payload = {
        "ok":    not snap.degraded,
        "error": ("; ".join(snap.errors))[:63] if snap.degraded else "",
        "clock": now.strftime("%H:%M"),
        "date":  now.strftime("%m-%d"),
        "i":     idx,
        "t":     title,
        "rows":  rows[:MAX_SCREEN_ROWS],
        # 页脚要显示的设备指标，和 /status 同源同口径
        "cpu":   _pct(sysd.get("cpu")),
        "mem":   _pct(sysd.get("mem")),
        "disk":  _pct(sysd.get("disk")),
        "nscr":  SCREEN_COUNT,
    }
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    return Response(content=body.encode("utf-8"),
                    media_type="application/json; charset=utf-8",
                    headers={"Cache-Control": "no-store"})


@app.get(
    "/status",
    summary="固件契约（冻结）",
    description="ESP32 固件专用。紧凑 JSON，`\"ok\":true` 不能带空格。"
                "改动此接口会导致已烧录设备解析失败，新字段一律进 /api/v1。",
    tags=["firmware"],
)
async def status_endpoint():
    snap = await collector.snapshot()
    payload = _firmware_payload(snap)

    # 直接产出 bytes，绕开框架的序列化 —— 保证字段顺序和紧凑格式完全可控
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))

    if len(body.encode("utf-8")) > FIRMWARE_MAX_BYTES:
        # 真超限时优先裁会话列表，保住三格数字（那才是屏幕主体）
        log.warning("响应体超限，裁剪会话列表")
        payload["sessions"] = payload["sessions"][:1]
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))

    return Response(
        content=body.encode("utf-8"),
        media_type="application/json; charset=utf-8",
        headers={"Cache-Control": "no-store"},
    )


@app.get("/api/ticker", response_class=PlainTextResponse, tags=["firmware"],
         summary="滚动条纯文本")
async def ticker() -> str:
    return TICKER_TEXT


# ---------------------------------------------------------------- v1 聚合


@app.get("/api/v1/board", response_model=BoardResponse, tags=["v1"],
         summary="看板聚合快照")
async def v1_board() -> BoardResponse:
    snap = await collector.snapshot()
    return BoardResponse(**_board_payload(snap))


@app.get("/api/v1/summary", response_model=Summary, tags=["v1"],
         summary="三格统计（同屏幕）")
async def v1_summary() -> Summary:
    snap = await collector.snapshot()
    return Summary(working=snap.working, today_done=snap.today_done,
                   automations=snap.automations)


# ---------------------------------------------------------------- v1 明细


@app.get("/api/v1/sessions", response_model=SessionListResponse, tags=["v1"],
         summary="会话列表")
async def v1_sessions(
    state: Optional[str] = Query(None, description="按状态过滤，如 working / completed"),
    project: Optional[str] = Query(None, description="按项目名过滤"),
    include_background: bool = Query(False, description="是否包含自动化后台会话"),
    limit: int = Query(20, ge=1, le=100, description="返回条数上限"),
) -> SessionListResponse:
    if state or project:
        total, rows = await asyncio.to_thread(
            db_src.sessions_recent, limit, state, project)
    else:
        rows = await asyncio.to_thread(db_src.sessions_active, limit, include_background)
        total = len(rows)
    return SessionListResponse(total=total, items=[_session_item(r) for r in rows[:limit]])


@app.get("/api/v1/tasks", response_model=TaskListResponse, tags=["v1"],
         summary="任务列表")
async def v1_tasks(
    status: Optional[str] = Query(None, description="completed / in_progress 等"),
    since: Optional[str] = Query(None, description="起始日期 YYYY-MM-DD"),
    limit: int = Query(50, ge=1, le=500),
):
    since_ms = None
    if since:
        try:
            since_ms = int(datetime.strptime(since, "%Y-%m-%d").timestamp() * 1000)
        except ValueError:
            return JSONResponse(
                status_code=422,
                content={"ok": False, "error": "invalid_param",
                         "detail": "since 需为 YYYY-MM-DD，例如 2026-10-01",
                         "source": None},
            )
    total, items = await asyncio.to_thread(files_src.tasks_list, status, since_ms, limit)
    return TaskListResponse(total=total, items=[TaskItem(**i) for i in items])


@app.get("/api/v1/automations", response_model=AutomationListResponse, tags=["v1"],
         summary="自动化列表（含最近 5 次运行）")
async def v1_automations() -> AutomationListResponse:
    rows = await asyncio.to_thread(db_src.automations_list)
    return AutomationListResponse(total=len(rows),
                                  items=[AutomationItem(**r) for r in rows])


@app.get("/api/v1/activity", response_model=ActivityStat, tags=["v1"],
         summary="今日代码改动（已排除 build/node_modules）")
async def v1_activity() -> ActivityStat:
    snap = await collector.snapshot()
    return ActivityStat(**snap.activity)


@app.get("/api/v1/artifacts", response_model=ArtifactListResponse, tags=["v1"],
         summary="产物列表")
async def v1_artifacts(limit: int = Query(20, ge=1, le=200)) -> ArtifactListResponse:
    total, items = await asyncio.to_thread(files_src.artifacts_list, limit)
    return ArtifactListResponse(total=total, items=items)


@app.get("/api/v1/system", response_model=SystemStat, tags=["v1"],
         summary="系统指标")
async def v1_system() -> SystemStat:
    snap = await collector.snapshot()
    return SystemStat(**snap.sys)


@app.get("/api/v1/health", response_model=HealthResponse, tags=["v1"],
         summary="数据源自检")
async def v1_health() -> HealthResponse:
    snap = await collector.snapshot()
    age = await collector.cache_age_ms()
    return HealthResponse(
        ok=not snap.degraded,
        version=VERSION,
        generated_at=iso_now(snap.generated_at_ms or None),
        cache_age_ms=age,
        stale=age < 0 or age > STALE_MS,
        sources={k: SourceHealth(**v) for k, v in snap.health.items()},
    )


# ---------------------------------------------------------------- 兼容旧端点


@app.get("/api/status", deprecated=True, tags=["legacy"],
         summary="系统指标（旧版，已废弃）")
async def legacy_status() -> dict:
    snap = await collector.snapshot()
    now = datetime.now()
    return {
        "t": now.strftime("%H:%M:%S"),
        "d": now.strftime("%Y-%m-%d"),
        "wd": now.strftime("%a"),
        "wd_cn": "一二三四五六日"[now.weekday()],
        "cpu": snap.sys["cpu"], "mem": snap.sys["mem"],
        "temp": snap.sys["temp"],
        "host": socket.gethostname()[:16],
        "msg": "WorkBuddy Board", "msg_cn": "WorkBuddy 看板",
        "up": snap.sys["uptime"],
    }


# ---------------------------------------------------------------- 调试页

STATIC_DIR = Path(__file__).resolve().parent / "static"


@app.get("/", response_class=HTMLResponse, include_in_schema=False)
async def index() -> HTMLResponse:
    page = STATIC_DIR / "index.html"
    if page.is_file():
        return HTMLResponse(page.read_text(encoding="utf-8"))
    return HTMLResponse(
        "<h1>WorkBuddy 看板 API</h1>"
        "<p>调试页未找到。可访问 <a href='/docs'>/docs</a> 查看接口。</p>"
    )


# ---------------------------------------------------------------- 启动入口


def lan_ips() -> list[str]:
    out: list[str] = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127.") and ip not in out:
                out.append(ip)
    except OSError:
        pass
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        if ip not in out:
            out.insert(0, ip)
    except OSError:
        pass
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="WorkBuddy 看板数据服务")
    # 27777 是刻意挑的：8080 太常见，容易和别的开发服务撞车。
    # 改这里必须同步改固件 BRIDGE_BASE、start-board.bat、configure.py。
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--refresh", type=float, default=2.0, help="采集周期（秒）")
    args = ap.parse_args()

    collector.refresh_sec = args.refresh

    print("=" * 64)
    print("  WorkBuddy 看板数据服务  v" + VERSION)
    print("=" * 64)
    print(f"  调试页     : http://127.0.0.1:{args.port}/")
    print(f"  接口文档   : http://127.0.0.1:{args.port}/docs")
    print(f"  聚合接口   : http://127.0.0.1:{args.port}/api/v1/board")
    for ip in lan_ips():
        print(f"  固件地址   : http://{ip}:{args.port}/status")
    print(f"  采集周期   : {args.refresh}s")
    print(f"  数据源     : {db_src.DB_PATH}")
    print("=" * 64)
    print("  Ctrl+C 停止\n")

    import uvicorn
    # timeout_keep_alive 默认只有 5s，而固件正好每 5s 轮询一次 —— 设备复用连接
    # 时几乎每次都撞在服务端主动关闭的瞬间，表现为 ESP_ERR_HTTP_FETCH_HEADER /
    # CONNECTION_CLOSED 交替出现，屏幕 2/3 的刷新会失败。拉到 30s 留足余量。
    uvicorn.run(app, host=args.host, port=args.port,
                log_level="warning", access_log=False,
                timeout_keep_alive=30)


if __name__ == "__main__":
    main()
