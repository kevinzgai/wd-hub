"""量出真实转录里「提问 → 闭合」之间的数据量。

这个数字决定修复策略：
  · 若提问与闭合之间只隔几行/几 KB → 尾窗 20KB 本来够用，
    "看不到提问"另有原因（要往别处查）。
  · 若间隔动辄几十上百 KB / 上百行 → 尾窗必然漏，
    必须改成"不看窗口、看累积状态"。

同时输出每个提问的绝对时间戳，便于与 daemon 日志（hello / 连接时刻）对齐，
判断"提问发生时设备到底在不在线"。
"""
import glob
import json
import os
import time

PROJECTS = os.path.expanduser("~/.workbuddy/projects")


def ts(ev):
    v = ev.get("timestamp")
    if isinstance(v, (int, float)):
        s = v / 1000.0 if v > 1e11 else v
        return time.strftime("%H:%M:%S", time.localtime(s)), s
    return "?", None


def main():
    for path in sorted(glob.glob(os.path.join(PROJECTS, "**", "*.jsonl"),
                                recursive=True)):
        if "/subagents/" in path:
            continue
        try:
            raw = open(path, "rb").read()
        except OSError:
            continue
        lines = raw.splitlines()
        # 逐行累计字节位置
        offsets = []
        acc = 0
        for ln in lines:
            offsets.append(acc)
            acc += len(ln) + 1
        total = acc

        calls = {}
        rows = []
        for i, ln in enumerate(lines):
            if not ln.strip():
                continue
            try:
                ev = json.loads(ln)
            except ValueError:
                continue
            t = ev.get("type")
            if t == "function_call" and \
                    str(ev.get("name") or "").strip().lower() == "askuserquestion":
                cid = str(ev.get("callId") or ev.get("id") or "")
                calls[cid] = {"line": i, "off": offsets[i], "ev": ev}
            elif t == "function_call_result":
                cid = str(ev.get("callId") or ev.get("id") or "")
                if cid in calls:
                    c = calls.pop(cid)
                    rows.append({
                        "cid": cid, "start": c["ev"], "end": ev,
                        "line_gap": i - c["line"],
                        "byte_gap": offsets[i] - c["off"],
                    })

        if not rows:
            continue
        print(f"\n=== {os.path.relpath(path, PROJECTS)}  "
              f"({len(lines)} 行 / {total/1048576:.1f}MB) ===")
        for r in rows:
            t0, s0 = ts(r["start"])
            t1, s1 = ts(r["end"])
            dur = f"{s1 - s0:.1f}s" if (s0 and s1) else "?"
            over_l = "**超60行**" if r["line_gap"] > 60 else "在60行内"
            over_b = ("**超20KB**" if r["byte_gap"] > 20000 else "在20KB内")
            print(f"  {t0} → {t1}  历时 {dur}")
            print(f"      间隔 {r['line_gap']} 行（{over_l}）/ "
                  f"{r['byte_gap']} 字节（{over_b}）")
        if calls:
            print(f"  仍有 {len(calls)} 个未闭合")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
