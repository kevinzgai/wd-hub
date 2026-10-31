"""列出转录里每次 AskUserQuestion 的精确时间线与行位置。

排查「设备没弹提问框」时，只有时间戳能回答三个关键问题：
  1. 提问发生时设备在线吗？（对照 daemon 日志的 hello 时间）
  2. 提问的 function_call 距离文件末尾多少字节？
     若超过 TRANS_TAIL_BYTES(20000)，read_transcript 的尾扫窗口**根本读不到**
     —— 主机自然什么都不发。
  3. 提问距文件末尾多少行？read_transcript 只解析 `lines[-60:]`，
     超出 60 行同样读不到。
"""
import argparse
import glob
import json
import os
import time

PROJECTS = os.path.expanduser("~/.workbuddy/projects")
TAIL = 20000        # wb_app.py: TRANS_TAIL_BYTES
LINE_WIN = 60       # read_transcript: lines[-60:]


def fmt_ts(ev):
    ts = ev.get("timestamp")
    if isinstance(ts, (int, float)):
        v = ts / 1000.0 if ts > 1e11 else ts
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(v))
    if isinstance(ts, str):
        return ts[:19]
    return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=48)
    args = ap.parse_args()
    cutoff = time.time() - args.hours * 3600

    for path in sorted(glob.glob(os.path.join(PROJECTS, "**", "*.jsonl"),
                                 recursive=True)):
        try:
            mtime = os.stat(path).st_mtime
        except OSError:
            continue
        if mtime < cutoff:
            continue
        rel = os.path.relpath(path, PROJECTS)
        try:
            with open(path, "rb") as f:
                raw = f.read()
        except OSError:
            continue
        size = len(raw)
        text = raw.decode("utf-8", "replace")
        lines = text.splitlines()
        total = len(lines)

        calls = []
        closed = set()
        for idx, ln in enumerate(lines):
            if not ln.strip():
                continue
            try:
                ev = json.loads(ln)
            except ValueError:
                continue
            t = ev.get("type")
            if t == "function_call":
                name = str(ev.get("name") or "")
                if name.strip().lower() == "askuserquestion":
                    cid = str(ev.get("callId") or ev.get("id") or "")
                    tail_gap = size - len("\n".join(lines[idx:]).encode("utf-8"))
                    calls.append({
                        "idx": idx, "cid": cid, "ts": fmt_ts(ev),
                        "from_end_lines": total - idx,
                        "from_end_bytes": tail_gap,
                    })
            elif t == "function_call_result":
                cid = str(ev.get("callId") or ev.get("id") or "")
                if cid:
                    closed.add(cid)

        if not calls:
            continue
        print(f"\n=== {rel}  ({total} 行 / {size} 字节) ===")
        for c in calls:
            is_closed = c["cid"] in closed
            in_tail = c["from_end_bytes"] <= TAIL
            in_win = c["from_end_lines"] <= LINE_WIN
            print(f"  {c['ts']}  cid={c['cid'][:14]}…  "
                  f"{'已闭合' if is_closed else '**未闭合**'}")
            print(f"      距末尾 {c['from_end_lines']} 行 / {c['from_end_bytes']} 字节"
                  f"  → 尾扫窗口{'命中' if in_tail else '**漏掉**(>20000B)'}"
                  f" / 60 行窗口{'命中' if in_win else '**漏掉**(>60行)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
