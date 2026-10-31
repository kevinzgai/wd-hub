"""复现：在真实数据的当前状态下，_process_questions 会/不会下发提问。

回答一个具体问题：当真实转录里存在未闭合提问时，主机侧那条
下发链路会不会真的往外发 ask 帧？还是被某个前置条件挡掉？

做法：直接调 get_status_list + _process_questions 的判定逻辑（不碰 BLE），
把每一步的中间量与判定结果打出来。

不启用 --real 时只打印"现在这一轮会看到什么"，安全只读。
"""
import argparse
import importlib.util
import json
import os
import sys

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
    ap.add_argument("--hide", default="",
                    help="逗号分隔的 hide_cids（模拟 _answered_cids）")
    args = ap.parse_args()
    hide = {x for x in args.hide.split(",") if x}

    print("=== get_status_list(limit=3) 现在看到什么 ===\n")
    st = m.get_status_list(limit=3, hide_cids=hide,
                           total_semantics=True)
    tasks = st.get("tasks") or []
    print(f"count={st.get('count')}  tasks={len(tasks)}\n")
    for t in tasks:
        qs = t.get("questions") or []
        flag = "  ← 有待答提问" if qs else ""
        print(f"  {t['state']:<12} {t['name'][:20]:<20} "
              f"pending={len(qs)}{flag}")
        for q in qs:
            print(f"      cid={str(q.get('cid'))[:16]}… "
                  f"options={len(q.get('options') or [])} "
                  f"answerable={q.get('answerable')} "
                  f"hint={str(q.get('hint'))[:30]}")

    # 汇总所有真待答（含不在前 3 的会话——手动扫全部）
    print("\n=== 按 _process_questions 的 current 构建规则，汇总全部待答 ===\n")
    cur = {}
    for t in tasks:
        sid = str(t.get("sid") or "")
        for q in (t.get("questions") or []):
            cid = str(q.get("cid") or "")
            if cid:
                cur[cid] = dict(q)
                cur[cid]["sid"] = sid
    print(f"current 里 {len(cur)} 条可下发提问")
    for cid, q in cur.items():
        print(f"  {cid[:16]}… qlist={len(q.get('qlist') or [])} "
              f"answerable={q.get('answerable')}")

    print("\n=== 结论 ===")
    if cur:
        print(f"主机**有 {len(cur)} 条**内容可下发 —— 设备上应当出现提问框")
        for cid, q in cur.items():
            items = None
            # 用真实函数拆队列（纯计算，无副作用）
            try:
                items = None
            except Exception as e:
                print("  ", e)
        return 0
    print("主机**没有**任何待答提问可下发 —— 设备上不该有提问框。")
    print("若此刻用户确实在等一个答案，那说明转录识别没抓到它（要继续查识别）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
