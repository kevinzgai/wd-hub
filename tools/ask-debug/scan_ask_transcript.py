"""扫描 WorkBuddy 转录，统计 AskUserQuestion 的「发起」与「闭合」。

用途：排查「设备上没弹提问框」这类问题时，第一件要确认的事是
**主机侧到底有没有识别到待答提问**。如果转录里根本没有未闭合的
function_call(name=AskUserQuestion)，那设备端无论怎么改都不会有框
——问题在识别环节，不在渲染环节。

判据与 host/wb_app.py 的 read_transcript 完全一致：
  · function_call name == AskUserQuestion   → 记为未闭合调用
  · function_call_result 且 callId 命中     → 从集合里移除
只有最后还留在集合里的，才是设备该显示的那些「待答提问」。

用法：
    python3 tools/scan_ask_transcript.py                # 扫最近 3 小时改动的转录
    python3 tools/scan_ask_transcript.py --hours 24     # 放宽时间窗
    python3 tools/scan_ask_transcript.py --tail 200000  # 加大尾扫窗口
"""
import argparse
import glob
import json
import os
import time

PROJECTS = os.path.expanduser("~/.workbuddy/projects")
TAIL_DEFAULT = 20000          # 与 wb_app.py 的 TRANS_TAIL_BYTES 一致


def scan_file(path: str, tail: int):
    """返回 (未闭合的 ask 调用 dict, 全部 ask 调用数, 已闭合数, 是否读到了文件)。"""
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            size = f.tell()
            f.seek(max(0, size - tail))
            data = f.read().decode("utf-8", "replace")
    except OSError:
        return None, 0, 0, False

    open_calls = {}
    n_call = 0
    n_closed = 0
    for ln in data.splitlines():
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
                cid = str(ev.get("callId") or ev.get("id") or ev.get("timestamp") or ln)
                n_call += 1
                open_calls[cid] = {"cid": cid, "arguments": ev.get("arguments")}
        elif t == "function_call_result":
            cid = str(ev.get("callId") or ev.get("id") or "")
            if cid and cid in open_calls:
                open_calls.pop(cid, None)
                n_closed += 1
    return open_calls, n_call, n_closed, True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=3.0)
    ap.add_argument("--tail", type=int, default=TAIL_DEFAULT)
    args = ap.parse_args()

    cutoff = time.time() - args.hours * 3600
    files = 0
    total_open = []
    for path in glob.glob(os.path.join(PROJECTS, "**", "*.jsonl"), recursive=True):
        try:
            mtime = os.stat(path).st_mtime
        except OSError:
            continue
        if mtime < cutoff:
            continue
        files += 1
        opens, n_call, n_closed, ok = scan_file(path, args.tail)
        if not ok:
            continue
        rel = os.path.relpath(path, PROJECTS)
        stamp = time.strftime("%H:%M:%S", time.localtime(mtime))
        if opens or n_call:
            print(f"{stamp}  {rel}")
            print(f"          发起 {n_call} 次，闭合 {n_closed} 次，"
                  f"未闭合 {len(opens)} 个")
        for cid, info in opens.items():
            args_raw = info.get("arguments")
            if isinstance(args_raw, str):
                try:
                    args_raw = json.loads(args_raw)
                except ValueError:
                    pass
            summary = ""
            if isinstance(args_raw, dict):
                qs = args_raw.get("questions") or []
                if isinstance(qs, list):
                    parts = []
                    for q in qs[:4]:
                        if not isinstance(q, dict):
                            continue
                        hint = str(q.get("question") or q.get("hint") or "")[:40]
                        nopt = len(q.get("options") or [])
                        parts.append(f"{hint}({nopt}项)")
                    summary = " / ".join(parts)
            print(f"          └ 未闭合 {cid[:14]}…  {summary}")
            total_open.append((rel, cid, summary))

    print()
    print(f"扫描 {files} 个转录（最近 {args.hours} 小时，尾扫 {args.tail} 字节）")
    if total_open:
        print(f"**发现 {len(total_open)} 个未闭合提问** —— 主机应当下发到设备")
    else:
        print("**没有未闭合提问** —— 主机没什么可发，设备上不该出现提问框")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
