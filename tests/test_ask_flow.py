"""_handle_ask_answer 分支回归测试。

覆盖 2026-10-31 修复的缺陷：原来 `if cid is None or not picks:` 把
"答案无效"和"无归属会话"混为一谈，导致设备已提交的有效答案被丢弃
（设备已乐观退出提问页，用户白答且无法重答）。

用真实类的方法，但把外部依赖（BLE 写帧、ACP 通道）替换成打桩，
只测状态机与分支走向。

注：UnifiedDaemon 有 __slots__，不能挂动态属性，所以打桩状态放在
外部 dict 里，由闭包引用。
"""
import importlib.util, sys

import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "host", "wb_app.py")
spec = importlib.util.spec_from_file_location("wbapp", HOST)
m = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(m)
except SystemExit:
    pass

fails = 0
def ck(cond, msg):
    global fails
    if cond:
        print(f"  ✓ {msg}")
    else:
        print(f"  ✗ {msg}")
        fails += 1

Daemon = m.UnifiedDaemon


def make_daemon(shown, shown_id, cid, queue=None, accum=None, targets=None):
    """造一个只填了 _handle_ask_answer 用得到的字段的 daemon 实例。"""
    d = Daemon.__new__(Daemon)
    d._ask_shown = shown
    d._ask_shown_id = shown_id
    d._ask_shown_cid = cid
    d._ask_shown_at = 0.0
    d._ask_shown_payload = None
    d._ask_queue = list(queue or [])
    d._ask_queue_cid = "cid-1"
    d._ask_accum = dict(accum or {})
    d._acp_questions = {}
    d._ans_watch = {}
    st = {"marked": [], "logs": [], "posted": []}

    def _log(s):
        st["logs"].append(s)
    def _mark(c):
        st["marked"].append(c)
    def _targets(sid):
        return targets if targets is not None else []

    d._log = _log
    d._mark_answered = _mark
    d._answer_targets = _targets
    d.st = st
    return d


class Conn:
    """记录 post_request 的假连接。"""
    def __init__(self, st):
        self.st = st
    def post_request(self, method, params, tag=None):
        self.st["posted"].append((method, params, tag))


print("=== 1. 无效答案（index 越界）→ 保留当前提问页，不动状态 ===")
d = make_daemon(
    shown={"cid": "cid-1", "sid": "sid-1", "qid": "q_0",
           "options": ["A", "B", "C", "D"], "multi": False},
    shown_id=100, cid="cid-1")
d._handle_ask_answer({"id": 100, "index": 9})
ck(d._ask_shown_id == 100, "越界答案后 _ask_shown_id 不变（页还在）")
ck(d._ask_shown is not None, "越界答案后 _ask_shown 保留（可重答）")
ck(len(d._ask_queue) == 0 and d._ask_queue_cid == "cid-1", "队列未被误弹")
ck(any("未给出有效选项" in s for s in d.st["logs"]), "打了『未给出有效选项』日志")
ck(not d.st["marked"], "未标记已答")

print("\n=== 2. cid=None 的有效答案 → 有效但未投递，不当无效丢弃 ===")
d = make_daemon(
    shown={"cid": None, "sid": "", "qid": "q_0",
           "options": ["A", "B", "C", "D"], "multi": False},
    shown_id=100, cid=None)
d._handle_ask_answer({"id": 100, "index": 3})
ck(d._ask_shown is None, "作答后 _ask_shown 已清（页已收）")
ck(any("有效但无归属会话" in s for s in d.st["logs"]),
   "打了『有效但无归属会话』日志（而不是『未给出有效选项』）")
ck(not any("未给出有效选项" in s for s in d.st["logs"]),
   "**没有**误报『未给出有效选项』——这就是被修掉的 bug")
ck(not d.st["marked"], "未标记已答（没有真实会话）")

print("\n=== 3. cid=None 且答案本身无效 → 仍判无效，保留页面 ===")
d = make_daemon(
    shown={"cid": None, "sid": "", "qid": "q_0",
           "options": ["A", "B"], "multi": False},
    shown_id=100, cid=None)
d._handle_ask_answer({"id": 100, "index": 7})
ck(d._ask_shown is not None, "无效答案后页面保留")
ck(any("未给出有效选项" in s for s in d.st["logs"]), "打了『未给出有效选项』日志")

print("\n=== 4. cid=None 的有效答案不误弹真实队列 ===")
d = make_daemon(
    shown={"cid": None, "sid": "", "qid": "q_0",
           "options": ["A", "B"], "multi": False},
    shown_id=100, cid=None,
    queue=[{"cid": "cid-1", "qid": "q_1", "options": ["X", "Y"]}])
d._handle_ask_answer({"id": 100, "index": 0})
ck(len(d._ask_queue) == 1, "真实队列未被误弹（仍剩 1 题）")
ck(d._ask_queue_cid == "cid-1", "队列归属未变")

print("\n=== 5. id 不匹配 → 完全忽略 ===")
d = make_daemon(
    shown={"cid": "cid-1", "sid": "sid-1", "qid": "q_0",
           "options": ["A", "B"], "multi": False},
    shown_id=100, cid="cid-1")
d._handle_ask_answer({"id": 999, "index": 1})
ck(d._ask_shown is not None, "id 不匹配时页面保留")
ck(not d.st["logs"], "id 不匹配时不打任何日志（静默忽略）")

print("\n=== 6. 真实会话（cid/sid 齐全）→ 走投递路径 ===")
d = make_daemon(
    shown={"cid": "cid-1", "sid": "sid-1", "qid": "q_0",
           "options": ["A", "B", "C"], "multi": False},
    shown_id=100, cid="cid-1")
d._answer_targets = lambda sid: [Conn(d.st)]
d._handle_ask_answer({"id": 100, "index": 1})
ck(len(d.st["posted"]) == 1, "发出 1 条 resolveInterruption")
ck(d.st["posted"] and d.st["posted"][0][0] == "_codebuddy.ai/resolveInterruption",
   "方法名正确")
ck(d.st["posted"] and d.st["posted"][0][1]["answers"] == {"q_0": "B"},
   "答案映射到选项文本 B")
ck(d.st["posted"] and d.st["posted"][0][1]["decision"] == "allow", "decision=allow")
ck(d.st["marked"] == ["cid-1"], "标记已答")

print("\n=== 7. 多选题回 mask → 多个选项文本 ===")
d = make_daemon(
    shown={"cid": "cid-1", "sid": "sid-1", "qid": "q_0",
           "options": ["A", "B", "C", "D"], "multi": True},
    shown_id=100, cid="cid-1")
d._answer_targets = lambda sid: [Conn(d.st)]
d._handle_ask_answer({"id": 100, "mask": 5})   # 0101 → 第 1、3 项
ck(len(d.st["posted"]) == 1, "发出 1 条 resolveInterruption")
ck(d.st["posted"] and d.st["posted"][0][1]["answers"] == {"q_0": "A,C"},
   "mask=5 → 答案 A,C")

print("\n=== 8. 多题序列：中间题只累积，不投递 ===")
# 队列**含当前题**（队首就是正在显示的那题，见 _push_ask_item(self._ask_queue[0])）
d = make_daemon(
    shown={"cid": "cid-1", "sid": "sid-1", "qid": "q_0",
           "options": ["A", "B"], "multi": False},
    shown_id=100, cid="cid-1",
    queue=[{"cid": "cid-1", "qid": "q_0", "options": ["A", "B"]},
           {"cid": "cid-1", "qid": "q_1", "options": ["X", "Y"]}])
d._answer_targets = lambda sid: [Conn(d.st)]
d._handle_ask_answer({"id": 100, "index": 0})
ck(not d.st["posted"], "中间题不投递")
ck(d._ask_accum.get("cid-1", {}).get("q_0") == ["A"], "中间题答案已累积")
ck(len(d._ask_queue) == 1, "队列剩下一题（q_1）")

print("\n=== 9. 多题序列：最后一题合并累积后一次投递 ===")
d = make_daemon(
    shown={"cid": "cid-1", "sid": "sid-1", "qid": "q_1",
           "options": ["X", "Y"], "multi": False},
    shown_id=100, cid="cid-1",
    queue=[{"cid": "cid-1", "qid": "q_1", "options": ["X", "Y"]}],
    accum={"cid-1": {"q_0": ["A"]}})
d._answer_targets = lambda sid: [Conn(d.st)]
d._handle_ask_answer({"id": 100, "index": 1})
ck(len(d.st["posted"]) == 1, "最后一题发出 1 条投递")
ans = d.st["posted"][0][1]["answers"] if d.st["posted"] else {}
ck(ans.get("q_0") == "A" and ans.get("q_1") == "Y", f"两题答案合并: {ans}")
ck("cid-1" not in d._ask_accum, "累积已清空")

print("\n" + ("全部通过" if not fails else f"{fails} 项失败"))
sys.exit(0 if not fails else 1)
