"""用【实际修复后的】read_transcript 扫真实转录，确认修复在生产数据上生效。

与 tools/scan_ask_timeline.py 的区别：
  · scan_ask_timeline 是**排查用**，复刻旧判据（尾窗 20KB / 60 行），
    用来证明"提问确实掉出窗口"；
  · 本脚本直接 import host/wb_app.py 的 _ask_scan_incremental，
    即设备实际运行的那套代码，用来证明"修复后能看见"。

用法：
    python3 tools/verify_ask_scan.py                  # 默认扫最近 48h 改动的转录
    python3 tools/verify_ask_scan.py --hours 6
"""
import argparse
import glob
import importlib.util
import os
import sys
import time

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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=48.0)
    args = ap.parse_args()

    projects = m.PROJECTS
    cutoff = time.time() - args.hours * 3600
    n = 0
    total = 0
    print(f"用【修复后】的扫描器（TRANS_FIRST_WINDOW="
          f"{m.TRANS_FIRST_WINDOW} 字节）扫真实转录：\n")
    for path in sorted(glob.glob(os.path.join(projects, "**", "*.jsonl"),
                                recursive=True)):
        try:
            mtime = os.stat(path).st_mtime
            size = os.path.getsize(path)
        except OSError:
            continue
        if mtime < cutoff:
            continue
        n += 1
        got = m._ask_scan_incremental(path, size)
        st = m._TRANS_CURSOR.get(path) or {}
        rel = os.path.relpath(path, projects)
        # first_off 是**首扫起点**（记在 state 里，'off' 扫完就追平到末尾了，
        # 直接拿 off 会显示成 0，看着像"整读历史"，其实不是）
        first_off = st.get("first_off", st.get("off", 0))
        print(f"{rel}")
        print(f"  {size / 1048576:.1f}MB，首扫起点 {first_off} "
              f"（从末尾回退 {size - first_off} 字节，不整读历史），"
              f"未闭合提问 {len(got)} 个")
        for g in got:
            total += 1
            hint = str(g.get("hint") or "")[:36]
            print(f"    └ {str(g.get('cid'))[:18]}… "
                  f"选项 {len(g.get('options') or [])} 项  {hint}")
    print()
    print(f"扫描 {n} 个转录，识别到 {total} 个未闭合提问")
    if total:
        print("→ 这些提问**现在会被下发到设备**（修复前会因掉出尾窗而丢失）")
    else:
        print("→ 当前没有待答提问；设备上不该出现提问框（属正常）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
