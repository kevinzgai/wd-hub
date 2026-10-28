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
import threading
import time
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Optional

from pydantic import BaseModel, Field

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
        TokenDay, TokenHour, TokenModel, TokenSession, TokenSummary,
        human_ago, project_name, session_title, utf8_trunc,
    )
    from server.sources import db as db_src
    from server.sources import files as files_src
    from server.sources import system as sys_src
    from server.sources import tokens as tokens_src
else:
    from .collector import STALE_MS, Collector, iso_now
    from .models import (
        ActivityStat, ArtifactListResponse, AutomationItem, AutomationListResponse,
        BoardResponse, HealthResponse, LiveRunItem, LiveState, LiveWaitItem,
        SessionItem, SessionListResponse,
        SourceHealth, Summary, SystemStat, TaskItem, TaskListResponse, TaskStat,
        TokenDay, TokenHour, TokenModel, TokenSession, TokenSummary,
        human_ago, project_name, session_title, utf8_trunc,
    )
    from .sources import db as db_src
    from .sources import files as files_src
    from .sources import system as sys_src
    from .sources import tokens as tokens_src

VERSION = "1.0.0"
log = logging.getLogger("board.api")

# ---------------------------------------------------------------- 访问令牌
#
# ⚠ 为什么需要：服务默认监听 0.0.0.0，也就是**整个局域网**都能访问。
# 而返回的内容包含会话标题、项目名、任务标题、改动的文件路径 ——
# 足以还原你一整天在干什么。
#
# 默认不加 token 是为了不破坏现有用法（固件不会带 token）。
# 但只要加了 --token，除了固件专用的几个路径，其余接口都要带
# `?t=<token>` 或 `X-Board-Token` 头。
#
# 固件路径豁免的原因：固件的 URL 是写死在 BRIDGE_BASE 里的，
# 加 query 会让它每 5 秒拉一个带 token 的长 URL；而且固件本来就跑在
# 局域网内、屏幕上的数据也是给人看的。
#
# ⚠ 固件路径仍然会暴露会话标题 —— 真正的防护应该是"别在公共网络上跑"。
# 见 README 的"已知限制"。 */
API_TOKEN: Optional[str] = None

# 豁免鉴权的路径。
#
# ⚠ 这里不能用 startswith 前缀匹配整个列表 —— 列表里有 "/"，
# 而 startswith("/") 对**任何**路径都为真，等于全部豁免。
# 所以分成两类：精确匹配的路径，和前缀匹配的路径。
TOKEN_EXEMPT_EXACT = ("/", "/docs", "/redoc", "/openapi.json",
                       "/ticker", "/api/ticker")
# /device/report 也必须豁免：设备的 URL 是编译期写死的，带不了 ?t=xxx
TOKEN_EXEMPT_PREFIX = ("/status", "/screen/", "/device/report")


def _token_ok(request) -> bool:
    """校验请求带的令牌。没配 token 就一律放行。"""
    if not API_TOKEN:
        return True
    path = request.url.path
    if path in TOKEN_EXEMPT_EXACT:
        return True
    if any(path.startswith(p) for p in TOKEN_EXEMPT_PREFIX):
        return True

    # 两种带法：query（?t=xxx，方便浏览器调试）和请求头（干净一点）
    supplied = request.query_params.get("t") or request.headers.get("X-Board-Token")
    if not supplied:
        return False
    # 常量时间比较，避免时序侧信道
    if len(supplied) != len(API_TOKEN):
        return False
    diff = 0
    for a, b in zip(supplied, API_TOKEN):
        diff |= ord(a) ^ ord(b)
    return diff == 0


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
#
# 现有分配：1 任务 / 2 自动化 / 3 今日活动 / 4 系统（图表）/ 5 Token 用量 / 6 实时
SCREEN_COUNT = 7

# 哪一屏用图表渲染（服务端只发数据，具体画法在固件）。
# 改这个数字 = 换一屏图表化，固件不用动 —— 它按 kind 字段判断，不认屏号。
CHART_SCREEN = 4

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


@app.middleware("http")
async def _auth_middleware(request, call_next):
    """令牌校验中间件。

    只在 --token 指定了令牌时才生效（API_TOKEN 为 None 时一律放行）。
    固件契约路径（/status、/screen/*）豁免 —— 固件 URL 是写死的，
    加 query 会变成每 5 秒拉一个长 URL。"""
    if not _token_ok(request):
        log.warning("未授权访问: %s %s", request.method, request.url.path)
        return JSONResponse(
            status_code=401,
            content={"ok": False, "error": "unauthorized",
                     "detail": "缺少或错误的访问令牌（?t=xxx 或 X-Board-Token 头）",
                     "source": None},
        )
    return await call_next(request)


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
        # Token 总览。数据源不可用时给 None 而不是空对象 ——
        # 0 和"没数据"是两回事，前端要能区分"真的没用过"和"读不到"。
        "tokens": snap.tokens or None,
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


def _live_by_session(snap) -> dict:
    """把实时状态按 session_id 归拢，供固件逐行下发。

    返回 {session_id: {"st": 1|2, "act": "中文动作"}}：
      st=1 运行中（绿）  st=2 等你确认（黄）

    ⚠ 口径：同一会话同时命中"运行中"和"等你确认"时，**等确认优先**——
    用户更需要知道"我得出手了"，而不是"它还在忙"。
    """
    out: dict[str, dict] = {}
    lv = getattr(snap, "live", None) or {}
    for r in lv.get("running") or []:
        sid = str(r.get("session_id") or "")
        if not sid:
            continue
        out[sid] = {"st": 1, "act": str(r.get("action") or "")}
    for w in lv.get("waiting") or []:
        sid = str(w.get("session_id") or "")
        if not sid:
            continue
        prev = out.get(sid) or {}
        if prev.get("st") == 2:
            continue
        out[sid] = {"st": 2, "act": str(w.get("action") or "")}
    return out


def _firmware_session(row: dict, pending_convs: set[str],
                     live_map: Optional[dict] = None) -> dict:
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
    # ---- 实时状态（可选键）----
    # st: 0空闲 1运行中 2等你确认；act: 当前动作的中文名
    #
    # ⚠ 这两个键是**可选的**：老固件不认会直接忽略，新固件读老服务端
    # 也会因为缺省值退化成"空闲"。所以只在真的有实时信息时才发，
    # 没数据就不占字节（固件响应体上限 4095）。
    lv = (live_map or {}).get(str(row.get("id") or ""))
    if lv and lv.get("st"):
        out["st"] = int(lv["st"])
        if lv.get("act"):
            out["act"] = utf8_trunc(str(lv["act"]), 12)
    return out


def _pct(v) -> int:
    """百分比取整并夹到 0..100。psutil 缺失时 system_stat() 给 0.0，原样下发。"""
    try:
        return max(0, min(100, int(round(float(v or 0)))))
    except (TypeError, ValueError):
        return 0


def _snap_age_sec(snap) -> int:
    """快照距现在多少秒（数据有多新），取不到给 -1。

    页脚要显示"更新 3s前"而不是"更新 09:44"：
    后者是**服务端当前时刻**，每轮轮询都会变，跟"数据是否新鲜"没关系；
    真正的信息来源是 generated_at_ms（collector 每 2s 采一次写一次）。
    """
    gen = int(getattr(snap, "generated_at_ms", 0) or 0)
    if gen <= 0:
        return -1
    age = int(time.time() - gen / 1000.0)
    return age if age >= 0 else 0


def _chip_temp() -> int:
    """设备芯片结温（°C 取整），-1 = 设备没上报过或上报已过期。

    注意这是**结温**不是环境温度：芯片自发热让读数比室温高 5~15°C，
    满载时更高。看趋势可以，别拿它当室温用。
    """
    t = _device_snapshot().get("chip_temp")
    if t is None:
        return -1
    try:
        return int(round(float(t)))
    except (TypeError, ValueError):
        return -1


def _firmware_payload(snap) -> dict:
    now = datetime.now()
    sysd = getattr(snap, "sys", None) or {}
    # 扫盘有 3s TTL + mtime 签名缓存，放大到 5s 轮询频率上开销可忽略
    try:
        pending_convs = files_src.tasks_pending_by_conversation()
    except Exception:      # 任务目录不可读不该拖垮整个 /status
        pending_convs = set()
    # 实时状态映射（运行中 / 等确认）
    try:
        live_map = _live_by_session(snap)
    except Exception:
        live_map = {}
    return {
        "ok": not snap.degraded,
        # 固件 error 字段 char[64]，按字节截断（纯 ASCII 原因码，不含中文）
        "error": ("; ".join(snap.errors))[:63] if snap.degraded else "",
        "clock": now.strftime("%H:%M"),
        "date": now.strftime("%m-%d"),          # 必须纯 ASCII：固件 buffer 只有 8 字节
        # 数据有多新（秒）。固件页脚显示"更新 3s前"，本地每秒递增刷新。
        # -1 = 拿不到采集时间，固件显示 "--"。
        "age": _snap_age_sec(snap),
        "working": snap.working,
        "today_done": snap.today_done,
        "automations": snap.automations,
        "sessions": [
            _firmware_session(r, pending_convs, live_map)
            for r in snap.sessions[:MAX_SESSION_ROWS]
        ],
        # 设备（本机）指标：页脚状态条。全部取整下发——屏幕只有 12px 字体，
        # 小数位纯属浪费字节。旧固件不认这些键，直接忽略，向后兼容。
        "cpu":  _pct(sysd.get("cpu")),
        "mem":  _pct(sysd.get("mem")),
        "disk": _pct(sysd.get("disk")),
        "temp": int(sysd.get("temp") or 0),   # °C，取不到为 0，固件据此隐藏
        # 芯片结温（°C），来自设备自己上报的 /device/report，不是 PC 的传感器。
        # 取一位小数没必要 —— 12px 屏上显示整数，且传感器本身 ±1°C 精度。
        # 没有上报过就是 -1，固件显示 "--"，不显示假温度。
        "chip_temp": _chip_temp(),
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


def _fmt_tok(n) -> str:
    """token 数 → 1.2M / 345K / 987。

    屏幕上右值只有约 60px（12px 字体），10 位数的原始 token
    （累计 29 亿）根本放不下，必须缩写。
    """
    try:
        v = float(n or 0)
    except (TypeError, ValueError):
        return "-"
    if v >= 1e9:
        return f"{v / 1e9:.2f}B"
    if v >= 1e6:
        return f"{v / 1e6:.1f}M"
    if v >= 1e3:
        return f"{v / 1e3:.0f}K"
    return str(int(v))


def _fmt_money(v) -> str:
    """金额 → ¥1,234 / ¥12.3 / ¥0.87。

    累计到千元级时小数位没意义，屏幕上"¥2,137"比"¥2136.93"好读。
    """
    try:
        x = float(v or 0)
    except (TypeError, ValueError):
        return "-"
    if x >= 1000:
        return f"¥{x:,.0f}"
    if x >= 10:
        return f"¥{x:.1f}"
    return f"¥{x:.2f}"


def _charts_for(snap, idx: int) -> Optional[list[dict]]:
    """系统屏的图表数据。None 表示这一屏不是图表屏。

    **与 rows 并存下发**，不替换：旧固件认不出 `charts` 键会直接忽略，
    继续渲染 rows 文本，不会白屏。新固件优先画图表。

    字段含义（三字节量级，固件用 atoi 解析）：
      k  指标键，固件据此选默认色，也给未来加图标留位置
      v  0..100 的整数
      c  颜色码 1 蓝 / 2 绿 / 3 黄 / 5 红（超阈值）。与 rows 的 c 同一套映射，
         额外扩了 5=红，因为图表需要"正常色 + 危险色"两档，而 4 是灰。
    """
    if idx != CHART_SCREEN:
        return None
    sy = getattr(snap, "sys", None) or {}
    cpu  = _pct(sy.get("cpu"))
    mem  = _pct(sy.get("mem"))
    disk = _pct(sy.get("disk"))
    return [
        {"k": "cpu",  "n": "CPU",  "v": cpu,  "c": 5 if cpu  >= 85 else 1},
        {"k": "mem",  "n": "内存", "v": mem,  "c": 5 if mem  >= 90 else 2},
        {"k": "disk", "n": "磁盘", "v": disk, "c": 5 if disk >= 90 else 3},
    ]


def _screen_rows(snap, idx: int) -> tuple[str, list[dict]]:
    """详情屏内容。**全部基于内存快照**，不在请求路径查库（见 README 架构说明）。

    这里就是设备的全部"第二屏"：想改显示什么、加减屏，只改这个函数即可。
    """
    if idx == 1:                                    # 任务
        t = snap.tasks or {}
        ip  = int(t.get("in_progress") or 0)
        rows = [
            _row("总计",     t.get("total")),
            _row("已完成",   t.get("completed"), 2),
            # snap.today_done 与 tasks["today_completed"] 现在同源（collector 直接
            # 转发），保留 today_done 这个入口是因为概览屏用的也是它 —— 两处取
            # 同一个来源，将来口径再调不会出现"一屏新一屏旧"。
            _row("今日完成", snap.today_done, 2),
            _row("进行中",   ip, 1 if ip else 4),
        ]
        # 「有哪些任务」——只列**正在做**的任务标题。
        # 屏上只有 6 行，前 4 行给了统计，剩 2 行放最靠前的两个进行中任务。
        # 标题用 16px 主字体，中文一行大概 10 个字，超了会被截。
        try:
            _, ip_tasks = files_src.tasks_list(status="in_progress", limit=2)
            if not ip_tasks:
                _, ip_tasks = files_src.tasks_list(status="pending", limit=2)
        except Exception:
            ip_tasks = []
        for tk in ip_tasks:
            subj = str(tk.get("subject") or "").strip()
            if subj:
                rows.append(_row(utf8_trunc(subj, 8), None, 1))
        return "任务", rows

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

    if idx == 5:                                    # Token 用量
        t  = getattr(snap, "tokens", None) or {}
        td = t.get("today") or {}
        if not t or not t.get("requests"):
            # 数据源没起来（刚启动还没扫完，或日志目录不可读）
            return "Token", [
                _row("状态", "扫描中" if not t.get("ready") else "无数据", 4),
            ]
        rate = float(t.get("cache_rate") or 0.0)
        # 缓存命中率是省钱的关键指标，低了对成本影响很大，给黄灯提示
        return "Token", [
            _row("今日",  _fmt_tok(td.get("total")),  1),
            _row("今日费", _fmt_money(td.get("cost_est")), 1),
            _row("累计",  _fmt_tok(t.get("total")),   2),
            _row("累计费", _fmt_money(t.get("cost_est")), 2),
            _row("缓存率", f"{rate:.0f}%", 3 if rate < 50 else 4),
            _row("请求",  f"{int(t.get('requests') or 0):,}", 4),
        ]

    if idx == 6:                                    # 实时（消耗了多少资源）
        lv = getattr(snap, "live", None) or {}
        run  = lv.get("running") or []
        wait = lv.get("waiting") or []
        totals = lv.get("tool_totals") or {}
        if not run and not wait and not totals:
            return "实时", [_row("状态", "无活动", 4)]
        rows = [
            _row("运行中", len(run), 2 if run else 4),
            _row("等你",   len(wait), 3 if wait else 4),
        ]
        # 资源消耗：按累计耗时排前 3 的工具。
        # ⚠ 这些是**墙钟时间**（模型在想、Bash 在跑），不是 CPU 时间 ——
        # 屏上要说清楚，别让人以为是在烧 CPU。
        for name, sec in sorted(totals.items(), key=lambda kv: -kv[1])[:3]:
            rows.append(_row(utf8_trunc(str(name), 10), f"{int(sec)}s", 4))
        return "实时", rows

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
    charts = _charts_for(snap, idx)
    now = datetime.now()
    payload = {
        "ok":    not snap.degraded,
        "error": ("; ".join(snap.errors))[:63] if snap.degraded else "",
        "clock": now.strftime("%H:%M"),
        "date":  now.strftime("%m-%d"),
        # 与 /status 同口径：数据有多新（秒），供页脚"更新 Ns前"用
        "age":   _snap_age_sec(snap),
        "i":     idx,
        "t":     title,
        "rows":  rows[:MAX_SCREEN_ROWS],
        # 页脚要显示的设备指标，和 /status 同源同口径
        "cpu":   _pct(sysd.get("cpu")),
        "mem":   _pct(sysd.get("mem")),
        "disk":  _pct(sysd.get("disk")),
        # 图表屏页脚要显示芯片结温；和 /status 同源同口径
        "chip_temp": _chip_temp(),
        "nscr":  SCREEN_COUNT,
    }
    if charts:
        # kind 是固件选渲染器的唯一开关：没有这个键 = 老式文本屏
        payload["kind"] = "chart"
        payload["charts"] = charts
        # 图表屏底部两行。不复用 rows 的后两行，是因为那要求固件知道
        # "第 4 行是进程、第 5 行是运行时长" —— 服务端一调顺序固件就错位。
        # 用两个平铺字符串而不是数组：固件的 json_nth_object 只认对象数组，
        # 给它加字符串数组解析不值当。
        payload["f1"] = f"进程 {int(sysd.get('proc') or 0)}"
        payload["f2"] = f"运行 {_fmt_uptime(sysd.get('uptime'))}"
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    return Response(content=body.encode("utf-8"),
                    media_type="application/json; charset=utf-8",
                    headers={"Cache-Control": "no-store"})


# ===========================================================================
# 设备上报：芯片温度等板载指标
# ===========================================================================
# 为什么是内存字典而不是落盘：
#   这是"设备现在有多热"的实时读数，过期就没有意义。重启服务后 30 秒内
#   设备会再报一次，不需要持久化。
#
# 为什么单独开一个上行端点而不是塞进 /status 的 query：
#   /status 是冻结契约且被设备高频轮询（5s），把上报混进去会让"读"和"写"
#   耦合 —— 设备哪天只读不写，服务端就永远拿不到温度。分开更干净。
_device_report: Dict[str, dict] = {}
_device_report_lock = threading.Lock()
DEVICE_REPORT_TTL_SEC = 180   # 超过 3 分钟没上报就当离线，不再透传


class DeviceReport(BaseModel):
    """设备主动上报的板载指标。全部可选，缺字段就不覆盖旧值。"""
    chip_temp: Optional[float] = Field(None, description="片上温度传感器读数，°C")
    battery:   Optional[int]   = Field(None, description="电量 %，-1 = 未知")
    charging:  Optional[bool]  = Field(None, description="是否在充电")
    uptime_s:  Optional[int]   = Field(None, description="设备运行秒数")


def _device_snapshot() -> dict:
    """取设备最近一次上报，过期则返回空。线程安全。"""
    with _device_report_lock:
        rep = dict(_device_report)
    if not rep:
        return {}
    ts = rep.pop("_ts", 0.0)
    if time.time() - ts > DEVICE_REPORT_TTL_SEC:
        return {}
    rep.pop("_dev", None)
    return rep


@app.post(
    "/device/report",
    summary="设备上报板载指标（温度/电量）",
    description="ESP32 主动上行。数据存内存，供 /status 的 `chip_temp` 等字段透传。",
    tags=["firmware"],
)
async def device_report(req: DeviceReport):
    dev = (req.model_dump(exclude_none=True))
    with _device_report_lock:
        _device_report.update(dev)
        _device_report["_ts"] = time.time()
    return {"ok": True}


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

    if len(body.encode("utf-8")) >= FIRMWARE_MAX_BYTES:
        # 真超限时优先裁会话列表，保住三格数字（那才是屏幕主体）
        log.warning("响应体超限，裁剪会话列表")
        payload["sessions"] = payload["sessions"][:1]
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))

        # ⚠ 裁完必须**再判一次**。固件 s_http_buf[4096] 只能装 4095 字节 + NUL，
        # 4096 整会被截掉最后一个字节。原来只判一次且用 ">"，
        # 恰好 4096 字节时照发（固件丢掉末字节），裁成 1 条后也没复检 ——
        # 一次裁剪没解决就放弃了。
        if len(body.encode("utf-8")) >= FIRMWARE_MAX_BYTES:
            # 还是超：丢掉 sessions 和 activity.projects 这种长尾字段，
            # 只保三格数字 + 页头页脚。
            payload.pop("sessions", None)
            payload.pop("projects", None)
            body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
            if len(body.encode("utf-8")) >= FIRMWARE_MAX_BYTES:
                # 兜底：连上面的都保不住，就只留最核心的几个键
                payload = {k: payload[k] for k in
                           ("ok", "clock", "date", "working", "today_done",
                            "automations", "cpu", "mem", "disk", "temp", "nscr")
                           if k in payload}
                body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
                if len(body.encode("utf-8")) >= FIRMWARE_MAX_BYTES:
                    log.error("响应体裁到最小仍超限，固件将拿到残缺 JSON")

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
        # ⚠ total 必须是**过滤后的总条数**，不是当前返回的条数。
        # sessions_active() 带 LIMIT，len(rows) 最多等于 limit ——
        # 前端拿它算分页会永远只看到一页。
        total = await asyncio.to_thread(db_src.sessions_active_total, include_background)
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


@app.get("/api/v1/live", response_model=LiveState, tags=["v1"],
         summary="实时工作状态")
async def v1_live() -> LiveState:
    """此刻在干什么 / 卡在哪等你确认 / 烧了多少时间。

    数据来自本地 traces + 会话日志的增量扫描，不查云端。
    详见 sources/live.py 的口径说明。
    """
    snap = await collector.snapshot()
    lv = getattr(snap, "live", None) or {}
    running = []
    for r in (lv.get("running") or [])[:8]:
        running.append(LiveRunItem(
            session_id=r.get("session_id") or "",
            pid=str(r.get("pid") or ""),
            project=project_name(r.get("cwd")),
            action=r.get("action") or "",
            raw_action=r.get("raw_action") or "",
            elapsed_s=int(r.get("elapsed_s") or 0),
        ))
    waiting = []
    for w in (lv.get("waiting") or [])[:8]:
        waiting.append(LiveWaitItem(
            session_id=w.get("session_id") or "",
            project=project_name(w.get("cwd")),
            tool=w.get("tool") or "",
            action=w.get("action") or "",
            wait_s=int(w.get("wait_s") or 0),
            is_ask=bool(w.get("is_ask")),
        ))
    return LiveState(
        running=running,
        waiting=waiting,
        durations=lv.get("durations") or {},
        tool_totals=lv.get("tool_totals") or {},
        scanned=lv.get("scanned") or {},
    )


@app.get("/api/v1/system", response_model=SystemStat, tags=["v1"],
         summary="系统指标")
async def v1_system() -> SystemStat:
    snap = await collector.snapshot()
    return SystemStat(**snap.sys)


# ---------------------------------------------------------------- Token 用量
#
# 数据源：~/.workbuddy/projects/**/*.jsonl 里的 providerData.rawUsage。
# 与 token-dashboard skill 同源，口径一致（字段兜底顺序、模型价表都对齐）。
#
# ⚠ 这几个端点查的是 sources/tokens.py 的内存聚合，**不读盘**。
# 盘上的扫描由 collector 每 2s 增量推进，请求路径只做取数。
# 但注意 tokens 模块内部有锁，且 by_day/by_model 会遍历全表 ——
# 所以仍走 to_thread，别让快照区间被持锁卡住。

# tokens.summary() 拿不到时的兜底（服务刚起、首轮还没跑完）。
# 字段必须与 TokenSummary 对齐，漏一个就是 500。
_EMPTY_TOKEN_SUMMARY = {
    "requests": 0, "in": 0, "out": 0, "total": 0,
    "cache_hit": 0, "cache_write": 0, "cache_rate": 0.0,
    "cost_est": 0.0, "days": 0, "avg_per_day": 0,
    "today": {"requests": 0, "in": 0, "out": 0, "total": 0,
              "cache_rate": 0.0, "cost_est": 0.0},
    "ready": False, "scanned_at": 0,
}


@app.get("/api/v1/tokens", response_model=TokenSummary, tags=["v1"],
         summary="Token 用量总览")
async def v1_tokens() -> TokenSummary:
    # 快照里的 tokens 就是 tokens.summary() 的原样转发，字段名一一对应，
    # 不用重新加工 —— 加工会让"接口值"和"屏幕值"有机会跑偏。
    snap = await collector.snapshot()
    return TokenSummary(**(snap.tokens or _EMPTY_TOKEN_SUMMARY))


@app.get("/api/v1/tokens/daily", response_model=list[TokenDay], tags=["v1"],
         summary="逐日 Token 用量")
async def v1_tokens_daily(
    days: int = Query(30, ge=1, le=365, description="回溯天数"),
) -> list[TokenDay]:
    rows = await asyncio.to_thread(tokens_src.by_day, days)
    return [TokenDay(**r) for r in rows]


@app.get("/api/v1/tokens/models", response_model=list[TokenModel], tags=["v1"],
         summary="按模型汇总 Token 用量")
async def v1_tokens_models(
    days: int = Query(30, ge=1, le=365, description="回溯天数"),
) -> list[TokenModel]:
    rows = await asyncio.to_thread(tokens_src.by_model, days)
    return [TokenModel(**r) for r in rows]


@app.get("/api/v1/tokens/sessions", response_model=list[TokenSession], tags=["v1"],
         summary="消耗最多的会话")
async def v1_tokens_sessions(
    limit: int = Query(10, ge=1, le=100),
    days: int = Query(30, ge=1, le=365),
) -> list[TokenSession]:
    rows = await asyncio.to_thread(tokens_src.top_sessions, limit, days)
    return [TokenSession(**r) for r in rows]


@app.get("/api/v1/tokens/hours", response_model=list[TokenHour], tags=["v1"],
         summary="0-23 时用量分布")
async def v1_tokens_hours(
    days: int = Query(7, ge=1, le=40, description="回溯天数，上限 40 天（内存窗口）"),
) -> list[TokenHour]:
    rows = await asyncio.to_thread(tokens_src.by_hour, days)
    return [TokenHour(**r) for r in rows]


@app.post("/api/v1/tokens/rescan", tags=["v1"],
          summary="强制全量重扫 Token 日志",
          description="清空增量偏移与聚合，从头重读全部日志（约 1.5s）。"
                      "日志目录被清理、或数字明显对不上时用它。")
async def v1_tokens_rescan() -> dict:
    r = await asyncio.to_thread(tokens_src.scan, True)
    return {"ok": True, **r}


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
        html = page.read_text(encoding="utf-8")
        # ⚠ 把 token 注入页面。
        # 页面自己会去 fetch /api/v1/board 和 /api/v1/health，这两个路径
        # 在设了 --token 时是受保护的。没有注入的话，页面打开就是一片
        # "连接失败"，而服务端日志里 401 一片 —— 看起来像 bug，其实是
        # 鉴权生效了但页面没带上凭据。
        #
        # 注入方式是把 token 写进一段 JS 全局变量，页面读取后
        # 拼到 fetch 的 query 上。
        #
        # ⚠ 安全性说明：token 会被送到浏览器，任何能打开这个页面的人
        # 都能从开发者工具里看到它。所以这是"防误用/防同网段乱扫"级别的
        # 防护，不是"防定向攻击"。真要更严，得改成登录态 + 服务端 session，
        # 而不是把 token 明文塞进 HTML。
        token_js = json.dumps(API_TOKEN or "")
        inject = "<script>window.__WB_TOKEN__ = %s;</script>" % token_js
        # ⚠ 注入时**不能**吃掉原来那个 <script> 的开标签。
        # 之前写成 html[:idx] + inject + html[idx+len("<script>"):]，
        # 结果 inject 自带一个 </script>，原开标签又被删掉 ——
        # 于是原来 <script> 里的 JS 变成了普通文本，直接显示在页面上。
        # 正确做法：在原 <script> **之前**插入一段完整闭合的 script，
        # 原标签原样保留。
        head_end = html.lower().find("</head>")
        if head_end >= 0:
            html = html[:head_end] + inject + "\n" + html[head_end:]
        else:
            idx = html.find("<script>")
            if idx >= 0:
                html = html[:idx] + inject + "\n" + html[idx:]
            else:
                html = inject + "\n" + html
        return HTMLResponse(html)
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
    global API_TOKEN

    ap = argparse.ArgumentParser(description="WorkBuddy 看板数据服务")
    # 27777 是刻意挑的：8080 太常见，容易和别的开发服务撞车。
    # 改这里必须同步改固件 BRIDGE_BASE、start-board.bat、configure.py。
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--refresh", type=float, default=2.0, help="采集周期（秒）")
    ap.add_argument("--token", default="",
                    help="访问令牌。指定后 /api/v1/* 需要带 ?t=<token>；"
                         "固件契约路径（/status、/screen/*）豁免。"
                         "默认不加 —— 服务监听 0.0.0.0，局域网内都能访问。")
    args = ap.parse_args()

    collector.refresh_sec = args.refresh
    API_TOKEN = args.token.strip() or None

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
    if API_TOKEN:
        print(f"  访问令牌   : 已启用（/api/v1/* 需带 ?t=...）")
    else:
        print("  访问令牌   : 未启用（局域网内任何人可读全部数据）")
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
