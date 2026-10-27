"""Token 消耗 —— 从 WorkBuddy 会话日志里抽请求级真实 usage。

数据源与 `token-dashboard` skill 同源：`~/.workbuddy/projects/**/*.jsonl`
里每条记录的 `providerData.rawUsage`。这是**真实计费口径**，不是按字数估算。

## 为什么要增量扫

全量 287 个文件、380MB，实测 2.1 秒。采集周期是 2 秒 —— 直接全扫会
让采集协程永远追不上，CPU 也白烧。

所以做两层优化：

  1. **按文件记住读了多长**（`_OFFSETS`）。jsonl 只会往后追加，新内容从
     上次的字节偏移续读，历史部分不再碰。
  2. **聚合而不是留存**。每条 usage 只累加进「日期 × 模型」的桶里，
     原始记录读完就丢。内存占用与日志总量无关，只与天数和模型数有关。

第一次启动仍要全扫一遍（2.1s），但只发生在冷启动。之后每轮只读新增的那几 KB。

## 口径

所有时间戳按**本地时区**归日。日志里是毫秒 epoch，用
`datetime.fromtimestamp()` 而不是 `utcfromtimestamp()` —— CST-8 下后者会让
每天 08:00 之前的用量算到前一天。
"""

from __future__ import annotations

import datetime
import glob
import json
import logging
import os
import threading
from typing import Any, Optional

log = logging.getLogger("board.tokens")

PROJECTS_DIR = os.path.join(os.path.expanduser("~"), ".workbuddy", "projects")

# 只统计最近 N 天。日志有 287 个文件、跨好几个月，全留着没有意义，
# 而且首页只展示 30 天。留 40 天是给"上月同期"留余量。
KEEP_DAYS = 40


# ---------------------------------------------------------------- 价格表
#
# 单位：元 / 百万 token，格式 (输入, 输出, 缓存命中)。
# 与 token-dashboard skill 的 PRICE 表保持一致，改价两边一起改。
#
# ⚠ 这**只是牌价估算**。用户套餐折扣、赠送额度、阶梯价都不在里面，
# 所以金额字段一律标 "est" 字样，别当成账单。
PRICE: tuple[tuple[str, tuple[float, float, float]], ...] = (
    ("deepseek-v4-flash", (1.0, 4.0, 0.2)),
    ("deepseek-v4-pro",   (2.0, 8.0, 0.5)),
    ("deepseek",          (2.0, 8.0, 0.5)),
    ("glm-5.3",           (2.0, 8.0, 0.4)),
    ("glm-5.2-x",         (1.2, 6.0, 0.24)),
    ("glm-5.2",           (0.8, 3.2, 0.16)),
    ("glm",               (1.0, 4.0, 0.2)),
    ("kimi",              (1.5, 6.0, 0.3)),
    ("claude-opus-5",     (150.0, 750.0, 15.0)),
    ("claude-opus-4-8",   (110.0, 550.0, 11.0)),
    ("claude",            (110.0, 550.0, 11.0)),
    ("qwen",              (2.4, 9.6, 0.48)),
    ("hy3",               (4.0, 16.0, 0.8)),
    ("hy",                (4.0, 16.0, 0.8)),
    ("minimax",           (1.0, 5.0, 0.2)),
    ("m3",                (1.0, 5.0, 0.2)),
)

# 查不到模型时用它。取中位档，宁可在屏幕上显示个粗略值，
# 也不要因为出现新模型就把金额算成 0。
_PRICE_FALLBACK = (2.0, 8.0, 0.4)


def price_of(model: str) -> tuple[float, float, float]:
    """按前缀匹配取价。模型名带日期后缀（-20260101）也能命中。"""
    m = (model or "").lower()
    for prefix, p in PRICE:
        if m.startswith(prefix):
            return p
    return _PRICE_FALLBACK


# ---------------------------------------------------------------- 状态
#
# 增量扫描的全部可变状态都在锁里。采集走线程池（asyncio.to_thread），
# 而 /api/v1/tokens 可能在另一个线程读 —— 必须加锁。

_lock = threading.Lock()

# 文件绝对路径 -> 已消费的字节偏移。只增不减，文件被截断时归零重扫。
_OFFSETS: dict[str, int] = {}

# 归日聚合：{ "YYYY-MM-DD": { model: [in, out, cacheHit, cacheWrite, req] } }
_DAYS: dict[str, dict[str, list[int]]] = {}

# 会话级聚合：{ sid: {"d": 首条日期, "t": 标题, "w": 项目名,
#                     "in": .., "out": .., "hit": .., "req": .., "m": 主模型} }
_SESS: dict[str, dict[str, Any]] = {}

# 全量统计（不受 KEEP_DAYS 裁剪影响，但要跟 _DAYS 同步维护）
_TOTAL = {"in": 0, "out": 0, "hit": 0, "write": 0, "req": 0}

# 逐小时用量：{ "YYYY-MM-DD": [24 个 token 数] }。
# 必须在解析时顺手记 —— 事后无法从「天 × 模型」的桶里还原到小时。
_HOURS: dict[str, list[int]] = {}

_ready = False          # 首轮全扫是否已完成
_last_scan_ms = 0


# ---------------------------------------------------------------- 解析


def _grab(o: dict) -> Optional[dict]:
    """从一条日志里抽 usage。抽不到返回 None。

    字段兼容三种风格（各家模型厂商写法不同）：
      DeepSeek   rawUsage.prompt_tokens / prompt_cache_hit_tokens
      OpenAI    usage.inputTokens / prompt_tokens_details.cached_tokens
      Claude    prompt_tokens_details.cached_creation_tokens（缓存写入）

    与 token-dashboard 的 grab() 保持同一套兜底顺序 —— 两处口径必须一致，
    否则看板和网页的数字对不上。
    """
    pd = o.get("providerData")
    if not isinstance(pd, dict):
        return None
    ru = pd.get("rawUsage") or {}
    u = pd.get("usage") or {}

    tt = ru.get("total_tokens") or u.get("totalTokens")
    if not tt:
        return None

    ptd = ru.get("prompt_tokens_details") or {}
    hit = ru.get("prompt_cache_hit_tokens")
    if hit is None:
        hit = ptd.get("cached_tokens") or 0
    cw = (ptd.get("cached_creation_tokens")
          or ptd.get("cache_write_tokens") or 0)

    model = (pd.get("model") or pd.get("requestModelId")
             or pd.get("requestModelName") or "unknown")

    return {
        "in":  int(ru.get("prompt_tokens") or u.get("inputTokens") or 0),
        "out": int(ru.get("completion_tokens") or u.get("outputTokens") or 0),
        "hit": int(hit or 0),
        "write": int(cw or 0),
        "req": 1,
        "hour": 0,          # 由调用方按 timestamp 补，_grab 不解析时间
        "model": str(model),
    }


def _session_title(o: dict) -> Optional[str]:
    """首条真实用户输入的前 42 字。

    ⚠ 与 skill 的 session_title() 有一处**故意的差异**：
    skill 遇到 `<` 开头的段直接 `return None`，会把整条消息放弃。
    但实测 WorkBuddy 把用户真正的输入排在 system-reminder 之后：

        content = [ {input_text: "<system-reminder ...>"},
                    {input_text: "<craft_mode>..."},
                    {input_text: "换掉吧"} ]

    按 skill 的写法，这种消息处理到第一段就返回了，标题永远为空 ——
    实测 8000+ 个会话全是"无标题会话"。这里改成**跳过内部标签继续往后找**，
    找不到再返回 None。
    """
    if o.get("role") != "user":
        return None
    c = o.get("content")

    def pick(raw: str) -> Optional[str]:
        t = " ".join(raw.split())
        if not t or t.startswith("<"):      # 内部注入块，跳过
            return None
        return t[:42]

    if isinstance(c, str):
        return pick(c)
    if not isinstance(c, list):
        return None
    for seg in c:
        if not isinstance(seg, dict):
            continue
        if seg.get("type") in ("input_text", "text"):
            got = pick(seg.get("text") or "")
            if got:
                return got
    return None


def _day_of(ts_ms: float) -> Optional[str]:
    """毫秒 epoch → 本地日期 "YYYY-MM-DD"。

    ⚠ 必须用 fromtimestamp（本地时区）。utcfromtimestamp 在 CST-8 下会把
    每天 00:00–08:00 的用量算到前一天，热力图整体错位一格。
    """
    try:
        return datetime.datetime.fromtimestamp(ts_ms / 1000.0).strftime("%Y-%m-%d")
    except (ValueError, OSError, OverflowError):
        return None


def _ts_ms(v) -> Optional[float]:
    """时间戳归一成毫秒。日志里实测有 int、float、ISO 字符串三种。"""
    if isinstance(v, (int, float)):
        if v > 1e12:
            return float(v)          # 已经是毫秒
        if v > 1e9:
            return float(v) * 1000   # 秒
        return None
    if isinstance(v, str):
        s = v.strip()
        for fmt in ("%Y-%m-%dT%H:%M:%S.%fZ", "%Y-%m-%dT%H:%M:%SZ",
                    "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S.%f"):
            try:
                import calendar
                dt = datetime.datetime.strptime(s, fmt)
                return calendar.timegm(dt.timetuple()) * 1000.0
            except ValueError:
                pass
        try:
            return float(s)
        except ValueError:
            return None
    return None


def _short_ws(folder: str) -> str:
    """工作空间目录名是路径 slug，取 WorkBuddy 之后那段作为短名。"""
    i = folder.rfind("-WorkBuddy-")
    if i >= 0 and folder[i + 11:]:
        return folder[i + 11:]
    return folder


# ---------------------------------------------------------------- 标题
#
# ⚠ 不要从 jsonl 里猜标题。
#
# 实测：主会话的 user 消息几乎全是 system-reminder / user-context /
# task-notification 这类注入块，真实输入被夹在中间，而且**会话被压缩过之后
# 早期输入就没了**（被摘要替换）。从日志里提取，实测 59% 的会话拿不到标题，
# 拿到的还常常是 "Please continue with the convers..." 这种摘要注入。
#
# WorkBuddy 自己在 sessions 表里存了 title / custom_title —— 就是侧边栏
# 显示的那个名字，直接读它，准确率 100%。jsonl 提取只作为 DB 读不到时的兜底。

_TITLES: dict[str, str] = {}
_titles_loaded = False
_titles_at = 0.0
_TITLES_TTL = 30.0        # 标题很少变，30 秒刷一次足够


def _load_titles(force: bool = False) -> None:
    """从 workbuddy.db 批量读会话标题。失败就静默保持空表，走 jsonl 兜底。

    一次性把所有标题读进内存（实测 8000+ 行、约 1MB），之后查标题
    不再碰数据库 —— 采集周期 2s，逐会话查库会撞锁。
    """
    global _titles_loaded, _titles_at
    import time as _t
    now = _t.time()
    if _titles_loaded and not force and now - _titles_at < _TITLES_TTL:
        return
    _titles_at = now
    try:
        # 循环导入：db.py 不 import tokens，安全。
        # 放函数内是为了让 tokens 模块在 db 不可用时仍能 import。
        from .db import session_title_map
        rows = session_title_map()
    except Exception as e:              # noqa: BLE001
        log.warning("读会话标题失败，回退到日志提取: %s", e)
        _titles_loaded = True
        return

    from ..models import session_title as _mk_title
    for sid, r in rows.items():
        _TITLES[sid] = _mk_title(r["title"], r["custom_title"], r["cwd"], sid)
    _titles_loaded = True
    log.info("已加载 %d 个会话标题", len(_TITLES))


# ---------------------------------------------------------------- 累加


def _add(day: str, model: str, d: dict) -> None:
    """把一条 usage 累加进归日桶、小时桶和总量。调用方持锁。"""
    bucket = _DAYS.setdefault(day, {})
    cell = bucket.setdefault(model, [0, 0, 0, 0, 0])
    cell[0] += d["in"]
    cell[1] += d["out"]
    cell[2] += d["hit"]
    cell[3] += d["write"]
    cell[4] += d["req"]

    hours = _HOURS.get(day)
    if hours is None:
        hours = _HOURS[day] = [0] * 24
    hours[d["hour"]] += d["in"] + d["out"]

    _TOTAL["in"] += d["in"]
    _TOTAL["out"] += d["out"]
    _TOTAL["hit"] += d["hit"]
    _TOTAL["write"] += d["write"]
    _TOTAL["req"] += d["req"]


def _add_session(sid: str, day: str, d: dict, ws: str, title: Optional[str]) -> None:
    """累加到会话桶。调用方持锁。

    标题优先级：数据库（sessions.title/custom_title）> 本次传入的 jsonl 提取值。
    DB 里的就是侧边栏显示的名字，比日志猜的准。

    `day` 允许为空串：记标题时会走到这里，那时候还不知道日期。
    所以日期相关的字段（d/last/m）只在 day 非空时更新。
    """
    e = _SESS.get(sid)
    if e is None:
        e = _SESS[sid] = {"d": day, "t": "", "w": ws,
                          "in": 0, "out": 0, "hit": 0, "req": 0,
                          "m": d["model"], "last": day}

    # 每轮都尝试补标题：DB 可能是在会话建好后才有 title 的，
    # 只在建桶时取一次会永久停在空标题上
    if not e["t"]:
        e["t"] = _TITLES.get(sid) or title or ""

    e["in"] += d["in"]
    e["out"] += d["out"]
    e["hit"] += d["hit"]
    e["req"] += d["req"]
    if day:
        if not e["d"]:
            e["d"] = day
        if day > e["last"]:
            e["last"] = day
            e["m"] = d["model"]       # 主模型取最近一天的，不是首个


# ---------------------------------------------------------------- 扫描


def _scan_file(fp: str, folder: str, sid_hint: str, force: bool = False) -> int:
    """读一个 jsonl 的新增部分。返回**本轮消费的字节数**（0 = 没新内容）。

    ⚠ 返回值是"消费了多少"而不是"当前偏移"。原来返回偏移，调用方用
    `if got` 判断有没有变化 —— 只要文件非空就恒为真，统计里的 touched
    永远等于文件总数，等于没统计。

    用二进制 seek 而不是逐行迭代，是为了精确知道"读到哪了"。文本模式下
    多字节字符会让 tell() 与字节数对不上。
    """
    try:
        size = os.path.getsize(fp)
    except OSError:
        return 0

    start = 0 if force else _OFFSETS.get(fp, 0)
    if size < start:            # 文件被截断/重写 → 从头来
        start = 0
    if size == start:
        return 0                # 没有新内容，最常见的情况

    ws = _short_ws(folder)
    pos = start
    try:
        with open(fp, "rb") as f:
            f.seek(start)
            for raw in f:
                pos += len(raw)
                # 最后一行可能正在写入（无换行结尾），跳过它等下一轮。
                # 不跳过的话会读到半截 JSON，解析失败丢弃 —— 而偏移量已经
                # 前进了，那一行就永远丢了。
                if not raw.endswith(b"\n"):
                    pos -= len(raw)
                    break

                # ⚠ 预筛条件必须同时放行两类行，缺一不可：
                #   rawUsage / usage  → 用量行（要计费）
                #   "role":"user"     → 用户消息行（取标题）
                # 只留前者会让标题全部丢失（实测 8000+ 会话全是"无标题会话"）。
                # 两个字节检查都是 O(n) 的 memmem，比 json.loads 便宜两个数量级。
                has_usage = b"rawUsage" in raw or b'"usage"' in raw
                has_user = b'"role":"user"' in raw
                if not has_usage and not has_user:
                    # 99% 的行是助手回复/工具结果，体积又大。
                    # 先做字节级预筛，380MB 全解析要 2.1s，预筛后 1.1s。
                    continue
                try:
                    o = json.loads(raw)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
                if not isinstance(o, dict):
                    continue

                sid = str(o.get("sessionId") or sid_hint)
                d = _grab(o)
                if d is None:
                    # 不是 usage 行，但可能是带标题的用户消息
                    if has_user:
                        t = _session_title(o)
                        if t:
                            _add_session(sid, "", {"in": 0, "out": 0, "hit": 0,
                                                   "write": 0, "req": 0,
                                                   "hour": 0, "model": "unknown"},
                                         ws, t)
                    continue

                ts = _ts_ms(o.get("timestamp"))
                if ts is None:
                    continue
                day = _day_of(ts)
                if day is None:
                    continue
                # 小时按本地时区。和 _day_of 同一套换算，不会出现
                # "日期算今天、小时算明天"的错位
                try:
                    d["hour"] = datetime.datetime.fromtimestamp(ts / 1000.0).hour
                except (ValueError, OSError, OverflowError):
                    d["hour"] = 0

                model = d["model"]
                _add(day, model, d)
                _add_session(sid, day, d, ws, _session_title(o))
    except OSError as e:
        log.warning("读日志失败 %s: %s", os.path.basename(fp), e)
        return pos - start

    _OFFSETS[fp] = pos
    return pos - start


def _prune() -> None:
    """丢掉太旧的日期桶，控制内存。调用方持锁。

    三个表要一起裁：只裁 _DAYS 会让 _HOURS 无限涨，
    而 _SESS 靠 last 日期判断，不裁也能自然过滤，所以只清掉太旧的会话。
    """
    if not _DAYS and not _HOURS and not _SESS:
        return
    cutoff = (datetime.date.today()
              - datetime.timedelta(days=KEEP_DAYS)).strftime("%Y-%m-%d")
    for day in [d for d in _DAYS if d and d < cutoff]:
        _DAYS.pop(day, None)
    for day in [d for d in _HOURS if d and d < cutoff]:
        _HOURS.pop(day, None)
    for sid in [s for s, e in _SESS.items() if e["last"] and e["last"] < cutoff]:
        _SESS.pop(sid, None)


def scan(force: bool = False) -> dict[str, Any]:
    """扫一轮。返回统计摘要，供日志和健康检查用。

    首次调用会全量扫（约 2s），之后每次只读新增字节（毫秒级）。
    force=True 从头重建（文件被删/截断后要这么干）。
    """
    global _ready, _last_scan_ms

    with _lock:
        if force:
            _OFFSETS.clear()
            _DAYS.clear()
            _HOURS.clear()
            _SESS.clear()
            for k in _TOTAL:
                _TOTAL[k] = 0

        # cold 必须在任何 _ready 赋值**之前**取。
        # 原来写在"目录不存在"那个 return 之后，而那一支已经把
        # _ready 置成 True，于是永远返回 cold=False。
        cold = not _ready

        if not os.path.isdir(PROJECTS_DIR):
            _ready = True
            _last_scan_ms = int(datetime.datetime.now().timestamp() * 1000)
            return {"files": 0, "days": 0, "reqs": 0, "cold": cold}

        files = glob.glob(os.path.join(PROJECTS_DIR, "**", "*.jsonl"),
                          recursive=True)

        # 标题来自 workbuddy.db，30s 缓存。放这里而不是模块导入时，
        # 是为了让 db 不可用时不拖垮整个数据源。
        _load_titles(force=cold)

        seen = 0
        for fp in files:
            # rel 第一段是工作空间 slug，第二段是**父会话 id**。
            # 两种层级：
            #   工作空间/<会话id>.jsonl                      → 2 段，就是会话本身
            #   工作空间/<父会话id>/subagents/agent-x.jsonl   → 4 段，归父会话
            # 子代理的 token 算在父会话头上，否则一次 Explore 会多出
            # 一个消耗很大的"幽灵会话"。
            rel = os.path.relpath(fp, PROJECTS_DIR)
            parts = rel.split(os.sep)
            folder = parts[0] if parts else ""
            if len(parts) == 2:
                sid_hint = os.path.splitext(parts[1])[0]
            elif len(parts) >= 3:
                sid_hint = parts[1]          # 父会话 id
            else:
                sid_hint = folder
            try:
                got = _scan_file(fp, folder, sid_hint, force=force)
                if got:
                    seen += 1
            except Exception as e:          # noqa: BLE001
                # 单个坏文件不该拖垮整轮扫描
                log.warning("扫描异常 %s: %s", os.path.basename(fp), e)

        _prune()
        _ready = True
        _last_scan_ms = int(datetime.datetime.now().timestamp() * 1000)

        # 已消失的文件（会话被删）从偏移表里清掉，避免无限增长
        alive = set(files)
        for fp in [p for p in _OFFSETS if p not in alive]:
            _OFFSETS.pop(fp, None)

        return {"files": len(files), "touched": seen,
                "days": len(_DAYS), "reqs": _TOTAL["req"], "cold": cold}


# ---------------------------------------------------------------- 查询


def _cost(model: str, tin: int, tout: int, hit: int, write: int) -> float:
    """按牌价估算金额（元）。

    缓存命中走缓存价；缓存写入（Claude 系）按 1.25× 输入价；
    其余输入走输入价。未命中的部分 = 输入 - 命中 - 写入，不能重复扣。
    """
    pin, pout, phit = price_of(model)
    miss = max(0, tin - hit - write)
    return (miss * pin + hit * phit + write * pin * 1.25 + tout * pout) / 1_000_000


def summary() -> dict[str, Any]:
    """总量 KPI。网页和固件屏都从这里取，保证口径唯一。"""
    with _lock:
        tin = _TOTAL["in"]
        tout = _TOTAL["out"]
        hit = _TOTAL["hit"]
        write = _TOTAL["write"]
        req = _TOTAL["req"]

        # 金额按 天 × 模型 逐格算 —— 不同模型单价差几十倍，
        # 不能先合并再计价
        cost = 0.0
        for day, bucket in _DAYS.items():
            for model, c in bucket.items():
                cost += _cost(model, c[0], c[1], c[2], c[3])

        days = sorted(_DAYS.keys())
        n_days = len(days)
        today = datetime.date.today().strftime("%Y-%m-%d")
        today_cell = _DAYS.get(today, {})
        t_in = sum(c[0] for c in today_cell.values())
        t_out = sum(c[1] for c in today_cell.values())
        t_hit = sum(c[2] for c in today_cell.values())
        t_write = sum(c[3] for c in today_cell.values())
        t_req = sum(c[4] for c in today_cell.values())
        t_cost = sum(_cost(m, c[0], c[1], c[2], c[3])
                     for m, c in today_cell.items())
        t_cache = (t_hit / t_in * 100.0) if t_in else 0.0

        return {
            "requests": req,
            "in": tin,
            "out": tout,
            "total": tin + tout,
            "cache_hit": hit,
            "cache_write": write,
            "cache_rate": round(hit / tin * 100.0, 1) if tin else 0.0,
            "cost_est": round(cost, 2),
            "days": n_days,
            "avg_per_day": int((tin + tout) / n_days) if n_days else 0,
            "today": {
                "requests": t_req,
                "in": t_in,
                "out": t_out,
                "total": t_in + t_out,
                "cache_rate": round(t_cache, 1),
                "cost_est": round(t_cost, 2),
            },
            "ready": _ready,
            "scanned_at": _last_scan_ms,
        }


def by_day(days: int = 30) -> list[dict[str, Any]]:
    """逐日用量，按日期升序。缺的日子补 0 —— 前端画柱状图不能有断档。"""
    n = max(1, min(365, int(days or 30)))
    today = datetime.date.today()
    with _lock:
        out = []
        for i in range(n - 1, -1, -1):
            d = (today - datetime.timedelta(days=i)).strftime("%Y-%m-%d")
            bucket = _DAYS.get(d, {})
            tin = sum(c[0] for c in bucket.values())
            tout = sum(c[1] for c in bucket.values())
            hit = sum(c[2] for c in bucket.values())
            req = sum(c[4] for c in bucket.values())
            cost = sum(_cost(m, c[0], c[1], c[2], c[3])
                       for m, c in bucket.items())
            out.append({
                "date": d,
                "requests": req,
                "in": tin,
                "out": tout,
                "total": tin + tout,
                "cache_rate": round(hit / tin * 100.0, 1) if tin else 0.0,
                "cost_est": round(cost, 2),
            })
        return out


def by_model(days: int = 30) -> list[dict[str, Any]]:
    """按模型汇总，用量降序。"""
    n = max(1, min(365, int(days or 30)))
    cutoff = (datetime.date.today()
              - datetime.timedelta(days=n - 1)).strftime("%Y-%m-%d")
    agg: dict[str, list] = {}
    with _lock:
        for day, bucket in _DAYS.items():
            if day < cutoff:
                continue
            for model, c in bucket.items():
                cell = agg.setdefault(model, [0, 0, 0, 0, 0])
                for i in range(5):
                    cell[i] += c[i]

        out = []
        for model, c in agg.items():
            out.append({
                "model": model,
                "requests": c[4],
                "in": c[0],
                "out": c[1],
                "total": c[0] + c[1],
                "cache_rate": round(c[2] / c[0] * 100.0, 1) if c[0] else 0.0,
                "cost_est": round(_cost(model, c[0], c[1], c[2], c[3]), 2),
            })
        out.sort(key=lambda x: x["total"], reverse=True)
        return out


def top_sessions(limit: int = 10, days: int = 30) -> list[dict[str, Any]]:
    """消耗最多的会话。按总量降序。

    ⚠ 会话 id 截断要保证**唯一**。原先统一取 `sid[:8]`，但实测存在
    `01a143cb-530e-...` 和 `01a143cb-52f9-...` 这种前 8 位相同的不同会话，
    截断后列表里出现"两条一样的会话"，前端下钻会认错。
    这里冲突时自动加长到 12 位，仍冲突就退化成完整 id。

    ⚠ 会话级只留了总量，没有逐日拆分（会占 30 倍内存），
    所以 `days` 在这里只用于过滤"最近还有活动的会话"，不是精确窗口。
    """
    n = max(1, min(365, int(days or 30)))
    cutoff = (datetime.date.today()
              - datetime.timedelta(days=n - 1)).strftime("%Y-%m-%d")
    with _lock:
        picked = [(sid, e) for sid, e in _SESS.items()
                  if e["last"] >= cutoff and e["req"] > 0]

        # 先按 8 位分组，冲突的升级到 12 位，还冲突就用全 id
        short: dict[str, int] = {}
        for sid, _ in picked:
            short[sid[:8]] = short.get(sid[:8], 0) + 1
        label: dict[str, str] = {}
        for sid, _ in picked:
            if short[sid[:8]] == 1:
                label[sid] = sid[:8]
            elif len(sid) > 12 and sum(
                    1 for s2, _ in picked if s2[:12] == sid[:12]) == 1:
                label[sid] = sid[:12]
            else:
                label[sid] = sid

        rows = [
            {
                "session": label[sid],
                "workspace": e["w"],
                "title": e["t"] or "(无标题会话)",
                "model": e["m"],
                "date": e["d"],
                "requests": e["req"],
                "total": e["in"] + e["out"],
                "cost_est": round(_cost(e["m"], e["in"], e["out"], e["hit"], 0), 2),
            }
            for sid, e in picked
        ]
    rows.sort(key=lambda x: x["total"], reverse=True)
    return rows[:max(1, min(100, int(limit or 10)))]


def by_hour(days: int = 7) -> list[dict[str, Any]]:
    """0-23 时的用量分布，近 N 天累加。

    逐小时必须在解析时就记，事后无法从「天 × 模型」的桶里还原 ——
    所以 `_HOURS` 单独存一份 {日期: [24 个 token 数]}。
    内存有界：最多 KEEP_DAYS × 24 个整数。
    """
    n = max(1, min(KEEP_DAYS, int(days or 7)))
    cutoff = (datetime.date.today()
              - datetime.timedelta(days=n - 1)).strftime("%Y-%m-%d")
    agg = [0] * 24
    with _lock:
        for day, hours in _HOURS.items():
            if day < cutoff:
                continue
            for h in range(24):
                agg[h] += hours[h]
    return [{"hour": h, "total": agg[h]} for h in range(24)]
