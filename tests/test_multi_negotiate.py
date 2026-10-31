"""主机侧多选能力协商测试。

验证 _push_ask_item 按 CAP_ASK_MULTI 降级：
· 设备声明了 CAP_ASK_MULTI → 原样下发 multi=1
· 设备没声明（含 caps=None 未握手）→ 降级 multi=0，并打降级日志

背景（2026-10-31 实测）：设备只有一颗键，固件不实现多选勾选
（wb_view.c 不画 [ ]/[x]），主机若硬发 multi=1，用户会卡在提问页——
短按只移光标，mask 恒 0，提交被拒，怎么按都没反应。
"""
import asyncio, importlib.util, json, sys, types

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


def make_daemon(caps):
    d = m.UnifiedDaemon.__new__(m.UnifiedDaemon)
    # caps 实际存在 AppState 上，daemon 通过 self._st 访问
    d._st = types.SimpleNamespace(device_caps=caps)
    d._device_answer_enabled = True
    d._next_id = 1
    d._ask_shown = None
    d._ask_shown_id = -1
    d._ask_shown_cid = None
    d._ask_shown_at = 0.0
    d._ask_shown_payload = None
    d._ask_queue = []
    d._ask_queue_cid = None
    d.st = {"written": [], "logs": []}
    return d


def run_push(caps, item):
    """驱动一次 _push_ask_item，返回下发出去的 payload 与日志。"""
    d = make_daemon(caps)
    written = []

    async def _write(payload):
        written.append(payload)
        return True
    d._write = _write
    d._log = lambda s: d.st["logs"].append(s)

    async def _go():
        await d._push_ask_item(item, {}, queued=False)
    asyncio.run(_go())
    return written[0] if written else None, d.st["logs"]


print("=== 1. 设备声明 CAP_ASK_MULTI → 原样下发 multi=1 ===")
payload, logs = run_push(
    m.CAP_ASK_MULTI,
    {"cid": "c1", "sid": "s1", "qid": "q_0", "hint": "要跑哪几种测试？",
     "options": ["A", "B", "C"], "multi": True, "answerable": True})
ck(payload is not None, "下发了一帧")
ck(payload and payload["multi"] == 1, "multi=1（设备支持，原样下发）")
ck(payload and payload["ans"] == 1, "可作答")
ck(not any("降级" in s for s in logs), "没有降级日志")

print("\n=== 2. 设备未声明 CAP_ASK_MULTI → 降级 multi=0 ===")
payload, logs = run_push(
    m.CAP_APPROVAL_PAGE | m.CAP_ASK_PAGE,     # caps=0x3，本机实际值
    {"cid": "c1", "sid": "s1", "qid": "q_0", "hint": "要跑哪几种测试？",
     "options": ["A", "B", "C"], "multi": True, "answerable": True})
ck(payload is not None, "下发了一帧")
ck(payload and payload["multi"] == 0,
   "**multi 已降级为 0**（否则设备卡死在提问页）")
ck(payload and payload["ans"] == 1, "仍然可作答（用户能选一项）")
ck(any("降级" in s for s in logs), "打了降级日志")
ck(payload and len(payload["opts"]) == 3, "选项原样保留")

print("\n=== 3. caps=None（未握手/老固件）→ 最保守，降级 multi=0 ===")
payload, logs = run_push(
    None,
    {"cid": "c1", "sid": "s1", "qid": "q_0", "hint": "多选",
     "options": ["A", "B"], "multi": True, "answerable": True})
ck(payload and payload["multi"] == 0, "caps 缺失 → multi=0（不假设新特性）")
ck(any("降级" in s for s in logs), "打了降级日志")

print("\n=== 4. 单选题不受协商影响 ===")
payload, logs = run_push(
    m.CAP_ASK_PAGE,
    {"cid": "c1", "sid": "s1", "qid": "q_0", "hint": "单选",
     "options": ["A", "B"], "multi": False, "answerable": True})
ck(payload and payload["multi"] == 0, "multi=0")
ck(not any("降级" in s for s in logs), "单选不触发降级日志")

print("\n=== 5. 登记的显示态与下发帧一致（multi 字段）===")
# 关键：_ask_shown["multi"] 必须等于实际下发的 multi，
# 否则 _handle_ask_answer 会用错的语义解码回传的答案。
d = make_daemon(m.CAP_ASK_PAGE)
written = []
async def _write(p):
    written.append(p); return True
d._write = _write
d._log = lambda s: d.st["logs"].append(s)
async def _go():
    await d._push_ask_item(
        {"cid": "c1", "sid": "s1", "qid": "q_0", "hint": "多选",
         "options": ["A", "B"], "multi": True, "answerable": True}, {}, False)
asyncio.run(_go())
ck(d._ask_shown["multi"] == 0, "_ask_shown.multi 与下发帧一致（都是 0）")
ck(d._ask_shown["multi"] == written[0]["multi"], "两者严格相等")

print("\n" + ("全部通过" if not fails else f"{fails} 项失败"))
sys.exit(0 if not fails else 1)
