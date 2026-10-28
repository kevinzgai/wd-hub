"""实时工作状态 —— 「此刻在干什么」「卡在哪」「烧了多少」。

这是看板里最有价值但最难拿的一块数据。三个需求分别对应三个本地源：

  1. **有哪些任务**      → `tasks/<会话id>/*.json`（files.py 已经有一份，
     这里不重复实现，只做跨会话聚合）
  2. **消耗了多少资源**  → `traces/<pid>/trace_*.jsonl` 里 span 的 duration
  3. **卡住等确认**      → 会话日志里 `function_call` 有、但 `function_call_result`
     没有配对 —— 那就是 AI 停下来等你了

## 为什么第 3 条不能看状态字段

`sessions` 表只有会话级 status（working / completed / active），没有
「正在等用户输入」这一维。WorkBuddy 内部也不落盘任何 pending-confirm 状态。

唯一的可靠信号是**调用配对**：AI 调了 `AskUserQuestion` 或者弹了权限审批，
调用记录写下去了，结果还没回来 —— 这就是"卡住等确认"。

实测历史数据：AskUserQuestion 的等待时长从 387 秒到 11000 秒不等，
中间人根本不在屏幕前，全靠这个信号才能发现。

## 为什么第 2 条要读 traces 而不是会话日志

会话日志（`projects/*.jsonl`）里的 `providerData.rawUsage` 有 token 用量，
但没有**时间**用量。traces 里每个 span 有 `duration`（毫秒），能算出
"这次任务 61 秒在等模型、20 秒在跑 Bash"。

## 扫描范围：只看活跃窗口

全量 traces 是 571 个文件 / 370MB，全量会话日志 239 个 / 480MB。
每轮全扫要 5.4 秒 —— 采集周期才 2 秒，必然追不上。

实测活跃窗口的规模：
  最近 30 分钟内修改的 trace  = 14 个  /   4 MB
  最近 30 分钟内修改的日志   =  1 个  / 1.4 MB

所以**只扫 30 分钟窗口内动过的文件**。代价是「历史总耗时」只统计窗口内，
但对"当前烧了多少"这个需求来说恰好是对的 —— 昨天的耗时跟今天的决策无关。

冷启动实测降到 ~180ms，之后每轮增量读新增字节（0.098ms）。

## 增量策略

  1. **文件级**：只扫 mtime 在窗口内的文件。
  2. **偏移级**：文件读到的字节偏移跨轮保留，只续读新增部分。
  3. **配对状态跨轮保留**：调用和结果可能隔几轮才配对（用户在思考），
     所以未配对的调用必须**跨轮累积**，不能每轮重算。

## 状态机

每个会话三种状态，屏上用不同颜色：

  running  绿  正在执行
  waiting  黄  停住了，等你确认
  idle     灰  存在但没动静

## ⚠「正在执行」为什么不看 trace 的未闭合 span

**踩过的坑**：最早的实现用 `traces/<pid>/trace_*.jsonl` 里"有 span_start
没有对应 span_end"来判定正在执行，实测**永远返回空**。

原因：trace 文件是**整个回合结束时才一次性落盘**的。实测一个跑了 5.5 分钟
的回合，trace 文件里的 span_start / span_end 是 739 / 739 —— **全部成对闭合**，
运行期间文件根本不存在（`os.path.getsize` 拿不到，mtime 也停在回合结束那一刻）。

所以 trace 只能用来做**事后耗时统计**（span 的 duration），不能做实时状态。

实时状态改用**会话日志**（`projects/<slug>/<sessionId>.jsonl`）：
  - 文件名就是 sessionId，不用猜
  - 内容是**增量写入**的，每条 function_call / function_call_result 立刻落盘
  - 判定规则：
      有未配对调用 + 工具是 AskUserQuestion → 等你确认（黄）
      有未配对调用 + 工具是别的             → 正在跑那个工具（绿）
      没有未配对调用 + 文件刚被写过          → 正在等模型（绿）

实测：在跑的这个会话，mtime age = 0s，未配对调用 = 1（Bash），
running 判定 1ms 内命中。
"""

from __future__ import annotations

import json
import logging
import os
import time
from typing import Any, Optional

from .db import SourceError

log = logging.getLogger("board.live")

WB_HOME = os.path.expanduser("~/.workbuddy")
TRACES_DIR = os.path.join(WB_HOME, "traces")
SESSIONS_DIR = os.path.join(WB_HOME, "sessions")
PROJECTS_DIR = os.path.join(WB_HOME, "projects")

# traces 扫描窗口：只看这段时间内被修改过的文件。
# trace 的语义是"正在跑"，30 秒没动静就不算 running，30 分钟窗口足够。
# 实测 30 分钟窗口 = 14 个 trace / 4MB。
SCAN_WINDOW_MS = 30 * 60 * 1000

# 会话日志扫描窗口：**比 traces 长得多**。
#
# 原因：AI 问完问题后，日志文件就不再被写入，mtime 停在提问那一刻。
# 如果窗口只有 30 分钟，"两小时前问我、我没看到"这种真正该提示的情况
# 会被漏掉 —— 而这恰恰是这个功能存在的意义。
#
# 实测代价（冷启动一次，之后靠 cursor 增量）：
#   0.5 小时窗口 =  1 个文件 / 1.5 MB
#  24 小时窗口   =  7 个文件 / 47 MB / 全量 JSON 解析 211ms
# 接受 211ms 的一次性开销，换"卡了半天你才发现"不会漏报。
LOG_WINDOW_MS = 24 * 60 * 60 * 1000

# 超过这个时间没动静就算"停住了"，不再算 running。
# 工具调用正常也就几秒到几十秒，30s 是安全线。
STALE_RUNNING_MS = 30_000

# 挂起超过这个时间就算"僵尸"，不再提示。
# 陈旧会话里残留的未配对调用（实测有 20 万秒前的）不该永远挂着。
# 24 小时足够覆盖"AI 问了个问题然后你去睡觉了"。
WAIT_ZOMBIE_MS = 24 * 60 * 60 * 1000

# 工具名 → 屏上显示的中文。
# 没列出的工具直接用原名（英文也短，屏上放得下）。
ACTION_LABELS = {
    "Bash": "跑命令",
    "PowerShell": "跑命令",
    "Read": "读文件",
    "Write": "写文件",
    "Edit": "改文件",
    "Grep": "搜索",
    "Glob": "搜索",
    "WebFetch": "抓网页",
    "WebSearch": "搜资料",
    "ViewImage": "看图",
    "Task": "子任务",
    "Agent": "子代理",
    "TaskCreate": "建任务",
    "TaskUpdate": "改任务",
    "TaskList": "看任务",
    "TaskOutput": "读任务",
    "TaskStop": "停任务",
    "Skill": "调技能",
    "show_widget": "画图",
    "widget_guidelines": "画图",
    "present_files": "交付文件",
    "SendMessage": "发消息",
    "AskUserQuestion": "等你回答",
    "mcp_tools": "调外部服务",
    "generation": "等模型",
}

# 明确在"问人"的工具。只有这些才触发黄灯，
# 普通工具执行慢不算 —— 否则满屏黄灯，没人看。
ASK_TOOLS = ("AskUserQuestion",)

# 「正在执行」的判定门槛：会话日志在多久内被写过就算活跃。
#
# 实测：一个回合里日志每 1~5 秒就会被追加一条（模型输出、工具调用、
# 工具结果），所以 90 秒的窗口足够覆盖正常节奏，又不会把
# "5 分钟前停下等你"的会话误判成在跑。
ACTIVE_WINDOW_MS = 90 * 1000


# ---------------------------------------------------------------- 工具函数


def _read_json(path: str) -> Optional[dict[str, Any]]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None


def _mtime_ms(path: str) -> int:
    try:
        return int(os.path.getmtime(path) * 1000)
    except OSError:
        return 0


def _pid_from_path(path: str) -> str:
    """从 traces/<pid>/trace_xxx.jsonl 里抠出 pid。

    目录名就是 worker pid，比 trace 元数据里的 workerPid 可靠
    （实测两者一致，但目录名不会为空）。
    """
    parts = os.path.normpath(path).replace("\\", "/").split("/")
    if len(parts) >= 2:
        return parts[-2]
    return ""


# ---------------------------------------------------------------- 跨轮状态

# 跨轮累积的状态。不能每轮重算 —— 调用和结果可能隔很多轮才配对。
#
# _pending_calls: callId → {name, ts, session_id}
#   未配对的工具调用。收到对应 result 时才移除。
#
# _span_state:   trace文件路径 → {"started": {spanId: span}, "ended": set(), "trace": {}}
#   span 的开闭状态。未闭合的 span = 正在执行。
#
# _file_cursor:  文件路径 → 读到的字节偏移
#   增量读位置。
#
# _durations:    pid → {工具名: 累计毫秒}
#   耗时聚合。键用 pid 而不是 traceId —— pid 直接对应会话。
#
# _user_ts:      sessionId → 最后一条用户消息的时间戳
#   用来区分「这一轮刚开始（还没调工具）」和「卡在半路」。
#
# _trace_session: traceId → sessionId
#   trace 文件里没有 sessionId，但会话日志的 providerData.traceId 能对上。
#   这是把耗时统计归到正确会话的唯一可靠办法。
_PENDING_CALLS: dict[str, dict[str, Any]] = {}
# 已经拿到结果的 callId。日志里同一个 callId 可能「先 result 后 call」
# 重复出现（会话恢复时重放），见 _consume_log 里的说明。
_DONE_CALLS: set[str] = set()
_DONE_CALLS_MAX = 4000
_SPAN_STATE: dict[str, dict[str, Any]] = {}
_FILE_CURSOR: dict[str, int] = {}
_DURATIONS: dict[str, dict[str, int]] = {}
_USER_TS: dict[str, int] = {}


# ---------------------------------------------------------------- traces


def _recent_files(root: str, suffix: str, window_ms: int) -> list[str]:
    """root 下 mtime 在窗口内的、suffix 结尾的文件。

    实测 571 个 trace 文件全量 stat 排序 22.7ms，窗口过滤后只 stat
    命中的那几个。返回按 mtime 倒序（最新的先扫）。
    """
    now_ms = int(time.time() * 1000)
    out: list[tuple[int, str]] = []
    try:
        entries = os.listdir(root)
    except OSError:
        return []
    for name in entries:
        if not name.endswith(suffix):
            continue
        p = os.path.join(root, name)
        mt = _mtime_ms(p)
        if mt and now_ms - mt <= window_ms:
            out.append((mt, p))
    out.sort(reverse=True)
    return [p for _, p in out]


def _recent_trace_files() -> list[str]:
    """窗口内被修改过的 trace 文件。"""
    now_ms = int(time.time() * 1000)
    out: list[tuple[int, str]] = []
    try:
        pids = os.listdir(TRACES_DIR)
    except OSError:
        return []
    for pid in pids:
        d = os.path.join(TRACES_DIR, pid)
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for fn in names:
            if not fn.startswith("trace_") or not fn.endswith(".jsonl"):
                continue
            p = os.path.join(d, fn)
            mt = _mtime_ms(p)
            if mt and now_ms - mt <= SCAN_WINDOW_MS:
                out.append((mt, p))
    out.sort(reverse=True)
    return [p for _, p in out]


def _scan_trace_file(path: str) -> None:
    """增量读一个 trace 文件，更新 span 开闭状态和耗时聚合。

    只读上次偏移之后的新增部分。文件被截断（不可能，但防御）就从头读。
    """
    st = _SPAN_STATE.setdefault(path, {"started": {}, "ended": set(), "trace": {}})
    try:
        size = os.path.getsize(path)
    except OSError:
        return
    off = _FILE_CURSOR.get(path, 0)
    if size < off:
        # 文件被截断或替换，重置状态
        st["started"].clear()
        st["ended"].clear()
        st["trace"] = {}
        off = 0
    if size == off:
        return

    try:
        with open(path, "rb") as f:
            f.seek(off)
            chunk = f.read()
    except OSError:
        return
    if not chunk:
        return

    # 只处理完整行：最后一行可能还没写完，留到下一轮
    text = chunk.decode("utf-8", errors="replace")
    nl = text.rfind("\n")
    if nl < 0:
        return
    body = text[:nl + 1]
    _FILE_CURSOR[path] = off + len(body.encode("utf-8"))

    pid = _pid_from_path(path)
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        rtype = rec.get("type")
        if rtype == "trace_start":
            st["trace"] = rec.get("trace") or {}
        elif rtype == "span_start":
            sp = rec.get("span") or {}
            sid = sp.get("spanId")
            if sid:
                st["started"][sid] = sp
        elif rtype == "span_end":
            sp = rec.get("span") or {}
            sid = sp.get("spanId")
            if sid:
                # span 只能结算一次。文件重放 / 读到重复行时
                # 不拦就会把同一个 span 的耗时累加两遍。
                if sid in st["ended"]:
                    continue
                st["ended"].add(sid)
                dur = sp.get("duration")
                name = sp.get("name")
                if dur and name:
                    # 用 pid 做键。trace 文件名里的 traceId 和会话日志的
                    # providerData.traceId **完全不是同一套 id**
                    # （实测 571 个 trace 与日志 traceId 零重叠），
                    # 时间邻近匹配也不可靠，所以只能靠 pid。
                    #
                    # ⚠ 代价：sessions/<pid>.json 只有活着的 worker，
                    # 所以 pid→sessionId 只对**当前活跃**的会话有效；
                    # 历史会话的耗时会挂在 pid 下而认不出是谁。
                    # 屏上要展示「消耗了多少资源」用 tool_totals（全局聚合），
                    # 那份数据不受这个问题影响。
                    slot = _DURATIONS.setdefault(_pid_from_path(path), {})
                    slot[name] = slot.get(name, 0) + int(dur)


# ---------------------------------------------------------------- pid 映射


def _pid_session_map() -> dict[str, dict[str, Any]]:
    """pid → {session_id, cwd, kind, started_at}。

    `sessions/<pid>.json` 是 WorkBuddy 写的心跳文件，只有 4 条（活着的
    worker 才有）。零成本，全量读。
    """
    out: dict[str, dict[str, Any]] = {}
    try:
        names = os.listdir(SESSIONS_DIR)
    except OSError:
        return out
    for fn in names:
        if not fn.endswith(".json"):
            continue
        d = _read_json(os.path.join(SESSIONS_DIR, fn))
        if not isinstance(d, dict):
            continue
        pid = str(d.get("pid") or "")
        if not pid:
            continue
        out[pid] = {
            "session_id": d.get("sessionId") or "",
            "cwd": d.get("cwd") or "",
            "kind": d.get("kind") or "",
            "started_at": int(d.get("startedAt") or 0),
            "url": d.get("url") or "",
        }
    return out


# ---------------------------------------------------------------- 会话日志


def _recent_session_logs() -> list[str]:
    """窗口内被修改过的会话日志（不含 subagents 子目录）。

    窗口比 traces 长（LOG_WINDOW_MS = 24 小时），见常量处的说明。
    """
    now_ms = int(time.time() * 1000)
    out: list[tuple[int, str]] = []
    try:
        dirs = os.listdir(PROJECTS_DIR)
    except OSError:
        return []
    for d in dirs:
        pdir = os.path.join(PROJECTS_DIR, d)
        try:
            names = os.listdir(pdir)
        except OSError:
            continue
        for fn in names:
            if not fn.endswith(".jsonl"):
                continue
            p = os.path.join(pdir, fn)
            mt = _mtime_ms(p)
            if mt and now_ms - mt <= LOG_WINDOW_MS:
                out.append((mt, p))
    out.sort(reverse=True)
    return [p for _, p in out]


def _session_id_from_log(path: str) -> str:
    """从会话日志路径里取 sessionId。

    `projects/<项目slug>/<sessionId>.jsonl` —— **文件名就是 sessionId**，
    比翻内容可靠（内容里每条记录的 sessionId 字段偶尔缺失）。
    """
    fn = os.path.basename(path)
    if not fn.endswith(".jsonl"):
        return ""
    sid = fn[:-len(".jsonl")]
    # sessionId 是 UUID（36 字符带横杠）或者是 prewarm-xxx 这类内部 id
    if len(sid) < 8:
        return ""
    return sid


def _cwd_from_log(path: str) -> str:
    """从会话日志里取 cwd（取最新一条记录的 cwd 字段）。"""
    try:
        size = os.path.getsize(path)
    except OSError:
        return ""
    # 只读文件尾部最后 64KB —— cwd 字段在每条记录里都有，
    # 没必要全量解析（实测单文件可能 7MB）。
    try:
        with open(path, "rb") as f:
            f.seek(max(0, size - 65536))
            chunk = f.read()
    except OSError:
        return ""
    lines = chunk.decode("utf-8", errors="replace").splitlines()
    for line in reversed(lines):
        line = line.strip()
        if not line or '"cwd"' not in line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        cwd = rec.get("cwd")
        if cwd:
            return str(cwd)
    return ""


def _scan_session_log(path: str) -> None:
    """增量读会话日志，更新未配对调用集合。

    匹配规则：`function_call` 用 `callId` 标识，`function_call_result` 也带
    同一个 `callId`。实测 93 调用 / 92 result 能对上 92 个。

    ⚠ 不能用 `id` 或 `providerData.messageId` 配对 —— 实测这两个对不上，
    会把 65 个已完成的调用全误判成"挂起"。

    同时记录每个会话最后一次「用户消息」的时间，用来判断这轮是不是
    刚开始（刚开始时还没调工具，属于"等模型"而不是"卡住"）。
    """
    try:
        size = os.path.getsize(path)
    except OSError:
        return
    off = _FILE_CURSOR.get(path, 0)
    if size < off:
        off = 0
    if size == off:
        return

    try:
        with open(path, "rb") as f:
            f.seek(off)
            chunk = f.read()
    except OSError:
        return
    if not chunk:
        return

    text = chunk.decode("utf-8", errors="replace")
    nl = text.rfind("\n")
    if nl < 0:
        return
    body = text[:nl + 1]
    _FILE_CURSOR[path] = off + len(body.encode("utf-8"))

    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        rtype = rec.get("type")
        cid = rec.get("callId")
        if rtype == "function_call":
            # ⚠ 日志里**同一个 callId 可能出现两次**：先有 result、
            # 后有函数调用（实测串口复位/会话恢复时会重放）。
            # 顺序处理时那次 result 来得太早、pop 了个空气，随后这条
            # function_call 就永远等不到结果 —— 于是「等你确认」永久误报。
            # 实测这个 bug 让一个 64 分钟前的 Bash 一直占着黄灯。
            # 所以记下已经完成的 callId，重复出现时直接跳过。
            if cid and cid in _DONE_CALLS:
                continue
            _PENDING_CALLS[cid] = {
                "name": rec.get("name") or "?",
                "ts": int(rec.get("timestamp") or 0),
                "session_id": rec.get("sessionId") or "",
                "cwd": rec.get("cwd") or "",
            }
            # ⚠ traceId → sessionId 映射走不通：trace 文件名里的 traceId
            # 和会话日志的 providerData.traceId **是两套完全不同的 id**
            # （实测 571 个 trace 与日志 traceId 零重叠）。
            # 时间邻近匹配也不可靠（历史会话日志已过 24h 窗口）。
            # 所以 trace 的耗时只能靠 pid 归属，见第 6 步的说明。
            continue
        if rtype == "function_call_result":
            if cid:
                _PENDING_CALLS.pop(cid, None)
                _DONE_CALLS.add(cid)
                # 集合别无限涨：超出就丢掉最早的一半。
                # 只保留最近的很够用 —— 重放的都是刚写入的相邻行。
                if len(_DONE_CALLS) > _DONE_CALLS_MAX:
                    keep = list(_DONE_CALLS)[-_DONE_CALLS_MAX // 2:]
                    _DONE_CALLS.clear()
                    _DONE_CALLS.update(keep)
            continue
        # 用户消息：标记「这一轮刚开始」
        if rtype == "message" and rec.get("role") == "user":
            sid = rec.get("sessionId") or ""
            if sid:
                _USER_TS[sid] = int(rec.get("timestamp") or 0)


# ---------------------------------------------------------------- 公开接口


def _action_label(name: str) -> str:
    return ACTION_LABELS.get(name, name)


def live_snapshot() -> dict[str, Any]:
    """实时工作状态快照。

    返回结构：
      {
        "running":   [ {session_id, title, project, action, elapsed_s} ],
        "waiting":   [ {session_id, title, project, tool, action, wait_s} ],
        "durations": { session_id: {工具名: 累计秒} },
        "tool_totals": { 工具名: 累计秒 },
        "scanned":   {traces: N, logs: N},
      }

    这个函数**不抛异常** —— 采集器里它已经被 try/except 包住了，
    但内部自己也兜住所有文件错误，避免一个坏文件毁掉整份快照。
    """
    now_ms = int(time.time() * 1000)

    # ---- 1. 扫 traces ----
    trace_files = _recent_trace_files()
    for path in trace_files:
        try:
            _scan_trace_file(path)
        except Exception as e:                       # noqa: BLE001
            log.debug("trace 扫描失败 %s: %s", path, e)

    # ---- 2. 扫会话日志（找挂起的调用）----
    log_files = _recent_session_logs()
    for path in log_files:
        try:
            _scan_session_log(path)
        except Exception as e:                       # noqa: BLE001
            log.debug("会话日志扫描失败 %s: %s", path, e)

    # ---- 3. pid → session 映射 ----
    pmap = _pid_session_map()

    # ---- 4. 汇总「正在执行」----
    #
    # ⚠ 不用 trace 的未闭合 span（实测 trace 回合结束才落盘，运行中永远是 0）。
    # 改用会话日志：文件名就是 sessionId，内容增量写入。
    #
    # 判定优先级（同一个会话）：
    #   有未配对调用 且 工具是 AskUserQuestion → 等你确认（黄）
    #   有未配对调用 且 工具是别的             → 正在跑那个工具（绿）
    #   没有未配对调用 但日志刚写过            → 正在等模型（绿）
    running: list[dict[str, Any]] = []
    seen_running: set[str] = set()

    # 4a. 有未配对调用 → 正在跑那个工具
    for cid, call in list(_PENDING_CALLS.items()):
        name = call.get("name") or "?"
        sid = str(call.get("session_id") or "")
        if not sid or sid in seen_running:
            continue
        ts = int(call.get("ts") or 0)
        if not ts or now_ms - ts > ACTIVE_WINDOW_MS:
            # 调用挂太久还没结果 → 归到 waiting 区，不算 running
            continue
        seen_running.add(sid)
        running.append({
            "session_id": sid,
            "pid": "",
            "cwd": call.get("cwd") or "",
            "action": _action_label(name),
            "raw_action": name,
            "elapsed_s": max(0, (now_ms - ts) // 1000),
        })

    # 4b. 没有未配对调用，但日志刚写过 → 正在等模型
    for path in log_files:
        mt = _mtime_ms(path)
        if not mt or now_ms - mt > ACTIVE_WINDOW_MS:
            continue
        sid = _session_id_from_log(path)
        if not sid or sid in seen_running:
            continue
        seen_running.add(sid)
        running.append({
            "session_id": sid,
            "pid": "",
            "cwd": _cwd_from_log(path),
            "action": "等模型",
            "raw_action": "generation",
            "elapsed_s": max(0, (now_ms - mt) // 1000),
        })

    # ---- 5. 汇总挂起调用 = 等你确认 ----
    #
    # 与 running 的分界：**同一个门槛 ACTIVE_WINDOW_MS（90 秒）**。
    #   挂起 < 90s  → 还在正常执行（running 区）
    #   挂起 > 90s  → 停住了，在等你（waiting 区）
    #
    # ⚠ 必须 list() 拷贝后再遍历 —— 循环里会 pop 掉僵尸项，
    # 直接遍历 dict 会抛 "dictionary changed size during iteration"。
    sid_to_log = {}
    for p in log_files:
        s = _session_id_from_log(p)
        if s:
            sid_to_log.setdefault(s, p)

    waiting: list[dict[str, Any]] = []
    seen_wait: set[str] = set()
    for cid, call in list(_PENDING_CALLS.items()):
        ts = int(call.get("ts") or 0)
        if not ts:
            continue
        waited_ms = now_ms - ts
        if waited_ms < ACTIVE_WINDOW_MS:
            # 还在正常执行窗口内，归 running 区
            continue
        if waited_ms > WAIT_ZOMBIE_MS:
            # 僵尸：24 小时前的残留调用，永远等不到答案了
            _PENDING_CALLS.pop(cid, None)
            continue
        name = call.get("name") or "?"
        sid = str(call.get("session_id") or "")
        if not sid or sid in seen_wait:
            continue
        # 同一会话不能既"运行中"又"等你确认" —— 它此刻正在动，
        # 那条挂起记录必然是被重放/漏配对的老数据（见 _consume_log）。
        # 不拦的话屏上会出现「运行中 1 / 等你 1」却只列 1 行会话。
        if sid in seen_running:
            continue
        seen_wait.add(sid)
        cwd = call.get("cwd") or ""
        if not cwd and sid in sid_to_log:
            cwd = _cwd_from_log(sid_to_log[sid])
        waiting.append({
            "session_id": sid,
            "tool": name,
            "action": _action_label(name),
            "wait_s": int(waited_ms // 1000),
            "cwd": cwd,
            "is_ask": name in ASK_TOOLS,
        })

    # ---- 6. 耗时聚合（pid → session_id）----
    #
    # ⚠ 只对**活跃**会话可靠：sessions/<pid>.json 只有活着的 worker 才有。
    # 历史会话的耗时会挂在 pid key 下认不出是谁 —— 这是本地数据的硬限制，
    # 不是 bug。屏上要展示「消耗了多少资源」请用 tool_totals（全局聚合）。
    durations: dict[str, dict[str, int]] = {}
    tool_totals: dict[str, int] = {}
    for pid, tools in _DURATIONS.items():
        info = pmap.get(pid) or {}
        sid = info.get("session_id") or pid
        slot = durations.setdefault(sid, {})
        for name, ms in tools.items():
            slot[name] = slot.get(name, 0) + ms
            tool_totals[name] = tool_totals.get(name, 0) + ms

    # 毫秒 → 秒（整数，屏上放得下）
    durations_s = {
        sid: {n: max(1, ms // 1000) for n, ms in tools.items()}
        for sid, tools in durations.items()
    }
    totals_s = {n: max(1, ms // 1000) for n, ms in tool_totals.items()}

    running.sort(key=lambda x: x["elapsed_s"])
    waiting.sort(key=lambda x: -x["wait_s"])

    return {
        "running": running[:8],
        "waiting": waiting[:8],
        "durations": durations_s,
        "tool_totals": totals_s,
        "scanned": {"traces": len(trace_files), "logs": len(log_files)},
    }


def waiting_conversations() -> set[str]:
    """正在等用户确认的会话 id 集合。

    固件"黄灯闪"用。注意只认**明确在问问题**的工具（ASK_TOOLS），
    普通工具执行慢不算 —— 那会满屏黄灯。

    ⚠ 直接复用 live_snapshot 的 waiting 结果，不要自己再遍历
    _PENDING_CALLS —— 那边有多重过滤（已完成的 callId、同一会话
    不能同时 running/waiting）。自己重算一遍必然跟看板对不上，
    就会出现"看板说在等、固件灯不闪"。
    """
    snap = live_snapshot()
    out: set[str] = set()
    for w in snap.get("waiting") or []:
        if w.get("is_ask"):
            sid = str(w.get("session_id") or "")
            if sid:
                out.add(sid)
    return out


def reset_cache() -> None:
    """清空跨轮状态。仅测试用。"""
    _PENDING_CALLS.clear()
    _DONE_CALLS.clear()
    _SPAN_STATE.clear()
    _FILE_CURSOR.clear()
    _DURATIONS.clear()
    _USER_TS.clear()
