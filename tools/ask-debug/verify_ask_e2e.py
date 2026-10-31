"""端到端验证：用**真实转录里的一次真实提问**跑修复后的扫描器。

背景（2026-10-31 实机排查「设备只有声音、没有提问框」）：
  read_transcript 原来只看「文件末尾 20KB + 最后 60 行」找待答提问。
  真实会话转录已达 22.9MB，提问发生后助手继续输出，很快把它推出窗口
  → pending 恒空 → 主机什么也不发 → 设备上永远没有框。

本脚本不做任何合成：直接把某个真实会话转录**复制**到临时文件，然后
把那次真实提问的 function_call_result 从副本里删掉（模拟"用户还没答"），
再交给实际生产代码 _ask_scan_incremental 扫描。

这样验证的是：
  · 真实 payload 能被 _question_info 正确解析出题干与选项
  · 即使该行距文件末尾几百行 / 十几 MB，修复后仍能识别
  · 同一份副本喂给旧判据（尾窗 20KB + 60 行）则识别不到（对照）

用法：
    python3 tools/verify_ask_e2e.py
    python3 tools/verify_ask_e2e.py --session <sid前缀>
"""
import argparse
import glob
import importlib.util
import json
import os
import shutil
import sys
import tempfile

# 定位项目根：脚本住在 tools/ 的子目录里，逐级数层数会随整理失效，
# 统一走 tools/_root.py 的"向上查骨架目录"（见该文件说明）。
_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)
from _root import project_root
ROOT = str(project_root())
HOST = os.path.join(ROOT, "host", "wb_app.py")
spec = importlib.util.spec_from_file_location("wbapp", HOST)
m = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(m)
except SystemExit:
    pass

TRANS_TAIL_BYTES = m.TRANS_TAIL_BYTES
LINE_WIN = 60


def old_logic(path: str) -> list:
    """复刻**旧实现**：只读末尾 20KB、只看最后 60 行。用来做对照。"""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(max(0, size - TRANS_TAIL_BYTES))
        data = f.read().decode("utf-8", "replace")
    out = {}
    for ln in [x for x in data.splitlines() if x.strip()][-LINE_WIN:]:
        try:
            ev = json.loads(ln)
        except ValueError:
            continue
        t = ev.get("type")
        if t == "function_call" and \
                str(ev.get("name") or "").strip().lower() == "askuserquestion":
            cid = str(ev.get("callId") or ev.get("id") or "")
            out[cid] = ev
        elif t == "function_call_result":
            out.pop(str(ev.get("callId") or ev.get("id") or ""), None)
    return list(out.values())


def find_asks(lines):
    """找出所有 AskUserQuestion 的 function_call 行号与其 callId。"""
    res = []
    for i, ln in enumerate(lines):
        try:
            ev = json.loads(ln)
        except ValueError:
            continue
        if ev.get("type") == "function_call" and \
                str(ev.get("name") or "").strip().lower() == "askuserquestion":
            res.append((i, str(ev.get("callId") or ev.get("id") or ""), ev))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", default=None)
    args = ap.parse_args()

    # 挑一个最大的转录（最能体现"被推出窗口"的场景）
    cands = []
    for p in glob.glob(os.path.join(m.PROJECTS, "**", "*.jsonl"), recursive=True):
        if "/subagents/" in p:
            continue
        try:
            size = os.path.getsize(p)
        except OSError:
            continue
        if args.session and args.session not in p:
            continue
        cands.append((size, p))
    if not cands:
        print("没找到任何转录")
        return 1
    cands.sort(reverse=True)
    size, src = cands[0]
    print(f"样本转录：{os.path.relpath(src, m.PROJECTS)}")
    print(f"          {size / 1048576:.1f}MB\n")

    lines = open(src, encoding="utf-8").read().splitlines()
    asks = find_asks(lines)
    if not asks:
        print("该转录里没有 AskUserQuestion，换一个样本（--session）")
        return 1
    idx, cid, ev = asks[-1]          # 取最后一次真实提问
    print(f"选取真实提问：行 {idx} / 共 {len(lines)} 行"
          f"（距末尾 {len(lines) - idx} 行）")
    info = m._question_info(ev.get("arguments"))
    print(f"  解析结果：题干「{str(info.get('hint'))[:50]}」"
          f" 选项 {len(info.get('options') or [])} 项 {info.get('options')}")
    print(f"  cid = {cid[:20]}…\n")

    # 副本：删掉这次提问的 function_call_result → 模拟"用户还没答"
    tmpdir = tempfile.mkdtemp(prefix="wb_e2e_")
    dst = os.path.join(tmpdir, "sess.jsonl")
    with open(dst, "w", encoding="utf-8") as f:
        for ln in lines:
            try:
                o = json.loads(ln)
            except ValueError:
                f.write(ln + "\n")
                continue
            if o.get("type") == "function_call_result" and \
                    str(o.get("callId") or o.get("id") or "") == cid:
                continue      # 丢掉这条 result，让它变成未闭合
            f.write(ln + "\n")
    dsize = os.path.getsize(dst)

    print("对照组（旧实现：末尾 20KB + 最后 60 行）：")
    old = old_logic(dst)
    print(f"  识别到 {len(old)} 个待答提问"
          f"{'（同一次提问也看不到 → 这就是设备没框的原因）' if not old else ''}")

    print("\n实验组（修复后：增量扫描）：")
    got = m._ask_scan_incremental(dst, dsize)
    st = m._TRANS_CURSOR.get(dst) or {}
    print(f"  首扫起点 {st.get('first_off')} / {dsize}"
          f"（回退 {dsize - st.get('first_off', 0)} 字节）")
    print(f"  识别到 {len(got)} 个待答提问")
    ok = False
    for g in got:
        if g["cid"] == cid:
            ok = True
            print(f"    ✓ 命中真实提问 {cid[:16]}…")
            print(f"      题干「{str(g.get('hint'))[:50]}」"
                  f" 选项 {len(g.get('options') or [])} 项：{g.get('options')}")
    if not ok and got:
        print(f"    （识别到的是别的提问：{[g['cid'][:12] for g in got]}）")

    shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    if ok:
        print("结论：修复后**能够识别**这次真实提问，主机可以把它下发到设备。")
        print("      旧实现识别不到 —— 与设备上「只有声音没有框」的现场表现一致。")
        return 0
    print("结论：未命中，需要检查。")
    return 2


if __name__ == "__main__":
    sys.exit(main())
