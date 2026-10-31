"""转录增量扫描回归测试（2026-10-31 修的「提问框不出现」缺陷）。

被修的缺陷：read_transcript 原来只用「文件末尾 20KB + 最后 60 行」找
待答提问。会话转录持续增长（实测 23.6MB），提问发出后助手继续输出，
几秒内就把那条 function_call 推出窗口 —— pending 恒为空，主机什么都不发，
设备上永远不弹提问框（但有别的事件提示音，于是看起来"只有声音"）。

本测试模拟「文件持续变大、提问被越推越远」，验证：
  · 增量扫描仍能持续看到未闭合提问（窗口外也可见）
  · 提问闭合（function_call_result）后立刻消失
  · 文件被截断/重建时从头重扫，不残留幽灵提问
  · 大量无关行涌入不会把提问挤出状态（这正是原实现的死因）
"""
import importlib.util
import json
import os
import sys
import tempfile

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


def write_lines(fh, events):
    """把事件追加进转录文件（每条一行 JSON + 换行）。"""
    for ev in events:
        fh.write(json.dumps(ev, ensure_ascii=False) + "\n")
    fh.flush()


def filler(n, tag="noise"):
    """造 n 条无关行，用来把提问"推远"（模拟助手持续输出）。"""
    return [{"type": "message", "role": "assistant", "tag": tag,
             "text": "填充 " + "x" * 60} for _ in range(n)]


def ask_call(cid, question="部署到哪个环境？", opts=("staging", "prod", "docker")):
    return {"type": "function_call", "name": "AskUserQuestion", "callId": cid,
            "arguments": json.dumps({"questions": [
                {"question": question, "options": [{"label": o} for o in opts]}]},
                ensure_ascii=False)}


def ask_result(cid):
    return {"type": "function_call_result", "callId": cid,
            "output": "answered"}


def reset_cursor(path):
    m._TRANS_CURSOR.pop(path, None)


# --------------------------------------------------------------------------
tmpdir = tempfile.mkdtemp(prefix="wb_trans_")
path = os.path.join(tmpdir, "sess.jsonl")


def read_pending():
    r = m.read_transcript(cwd="/tmp/fake", session_id="sess")
    return (r or {}).get("pending") or []


# read_transcript 按 PROJECTS/encode_cwd(cwd) 拼路径，这里直接调内部函数
def read_pending_direct(size):
    return m._ask_scan_incremental(path, size)


print("=== 1. 基础：提问发出即被看到 ===")
reset_cursor(path)
with open(path, "w", encoding="utf-8") as f:
    write_lines(f, [ask_call("c1")])
got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 1, "未闭合提问被识别（1 个）")
ck(got and got[0]["cid"] == "c1", "cid 正确")
ck(got and len(got[0]["options"]) == 3, f"选项解析出 3 项: {got[0]['options'] if got else None}")

print("\n=== 2. 关键回归：提问被大量后续行'推出'尾部窗口后仍然可见 ===")
reset_cursor(path)
with open(path, "w", encoding="utf-8") as f:
    write_lines(f, filler(1000, "before"))     # 提问发生前的正常对话
    write_lines(f, [ask_call("c2")])           # ← 提问在此刻发生（在文件尾）
size_ask = os.path.getsize(path)
# 关键：此刻 daemon 那一轮推流就应当看到它
got_at_birth = read_pending_direct(size_ask)
ck(len(got_at_birth) == 1 and got_at_birth[0]["cid"] == "c2",
   "提问发生当下即被捕获")

# 然后助手继续输出，把提问远远推出 20KB / 60 行窗口
with open(path, "a", encoding="utf-8") as f:
    write_lines(f, filler(100000, "after-ask"))
size = os.path.getsize(path)
got = read_pending_direct(size)
ck(size > m.TRANS_TAIL_BYTES, f"文件已远超尾扫窗口（{size} 字节 > {m.TRANS_TAIL_BYTES}）")
ck(len(got) == 1 and got[0]["cid"] == "c2",
   "**提问仍在 pending 里** —— 这就是原实现失败的地方（旧代码此处为空）")

# 反向验证：用旧的"只看最后 60 行"逻辑扫同一份文件，应当扫不到
tail_lines = list(open(path, encoding="utf-8"))[-60:]
old_hits = [ln for ln in tail_lines
            if "askuserquestion" in ln.lower()]
ck(not old_hits, "旧逻辑（最后 60 行）确实扫不到 —— 证明本测试能抓住 bug")

print("\n=== 3. 增量不重复：连续多轮扫描结果稳定 ===")
for i in range(5):
    got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 1, "多轮扫描仍是 1 个（增量游标未重复消费/漏读）")

print("\n=== 4. 闭合后立刻消失 ===")
with open(path, "a", encoding="utf-8") as f:
    write_lines(f, [ask_result("c2")])
got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 0, "function_call_result 到达后 pending 清空")

print("\n=== 5. 新提问在同一长期对话里仍能被看到 ===")
with open(path, "a", encoding="utf-8") as f:
    write_lines(f, [ask_call("c3", "用哪个 DB？", ("sqlite", "postgres"))])
got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 1 and got[0]["cid"] == "c3", "新提问 c3 被识别（老实现必然漏掉）")
ck(got and got[0]["options"] == ["sqlite", "postgres"], f"选项正确: {got[0]['options'] if got else None}")

print("\n=== 6. 文件被截断/重建 → 从头重扫，不留幽灵 ===")
# 先记住当时 pending 有 c3
read_pending_direct(os.path.getsize(path))
with open(path, "w", encoding="utf-8") as f:
    write_lines(f, [{"type": "message", "role": "user", "text": "重启后新会话"}])
size = os.path.getsize(path)      # 比旧 size 小
got = read_pending_direct(size)
ck(len(got) == 0, "截断后没有残留幽灵提问（旧状态已清）")

print("\n=== 7. 多个未闭合提问并存 ===")
with open(path, "a", encoding="utf-8") as f:
    write_lines(f, [ask_call("c4"), ask_call("c5")])
got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 2, f"两个未闭合提问都可见（实际 {len(got)}）")

print("\n=== 8. 只闭合其中一个 → 另一个保留 ===")
with open(path, "a", encoding="utf-8") as f:
    write_lines(f, [ask_result("c4")])
got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 1 and got[0]["cid"] == "c5", "c4 闭合后只剩 c5")

print("\n=== 9. 半行 JSON（正在写入）不被误判为提问消失 ===")
reset_cursor(path)
with open(path, "w", encoding="utf-8") as f:
    write_lines(f, [ask_call("c6")])
    # 写一个不完整行（有内容但没有换行结尾）
    f.write('{"type":"function_call","name":"AskUserQuest')
    f.flush()
got = read_pending_direct(os.path.getsize(path))
ck(len(got) == 1 and got[0]["cid"] == "c6", "半行不影响已识别的 c6")

print("\n=== 10. 大文件：首扫只回退 2MB，且一轮追平 ===")
reset_cursor(path)
big = os.path.join(tmpdir, "big.jsonl")
# 真实时序：先有大量陈年历史 → 提问在尾部发生 → daemon 当轮捕获。
# 首扫只回退 TRANS_FIRST_WINDOW（2MB），不从字节 0 起 ——
# 既避免"重启后把陈年残题弹出来"，也避免慢吞吞追几十轮。
with open(big, "w", encoding="utf-8") as f:
    write_lines(f, filler(60000, "ancient"))     # ~4MB 陈年历史
    write_lines(f, [ask_call("c7")])             # ← 提问在尾部
size0 = os.path.getsize(big)
got0 = m._ask_scan_incremental(big, size0)
off0 = m._TRANS_CURSOR[big]["off"]
ck(size0 > 2 * m.TRANS_FIRST_WINDOW,
   f"文件大于首扫窗口（{size0} > {2 * m.TRANS_FIRST_WINDOW}）")
ck(len(got0) == 1 and got0[0]["cid"] == "c7",
   "尾部提问被找到（首扫窗口覆盖到它）")
ck(off0 == size0, f"首扫一轮就追平到末尾（off={off0} == size={size0}）")
ck(off0 > 0, "首扫起点不在字节 0，没有整读文件历史")

# 追加新数据，验证第二轮只消费新增部分
with open(big, "a", encoding="utf-8") as f:
    write_lines(f, [{"type": "message", "text": "one-more"}])
size1 = os.path.getsize(big)
m._ask_scan_incremental(big, size1)
off1 = m._TRANS_CURSOR[big]["off"]
consumed = off1 - off0
ck(size1 - size0 < 4096, "新增数据很小")
ck(consumed == size1 - size0,
   f"第二轮只消费新增的 {consumed} 字节（不是整读 {size1} 字节）")
ck(len(m._TRANS_CURSOR[big]["asks"]) == 1, "提问仍被正确保留")

print("\n=== 11. 首扫不回退到 0：陈年残题不会复活 ===")
reset_cursor(path)
old = os.path.join(tmpdir, "old.jsonl")
with open(old, "w", encoding="utf-8") as f:
    write_lines(f, [ask_call("ancient-1")])          # 很远的历史残留提问
    write_lines(f, filler(300000, "much-later"))     # 远超 2MB
size = os.path.getsize(old)
got = m._ask_scan_incremental(old, size)
ck(size > m.TRANS_FIRST_WINDOW,
   f"文件大于首扫窗口（{size} > {m.TRANS_FIRST_WINDOW}）")
ck(len(got) == 0,
   "窗口外的陈年残题**没有**被当成待答（daemon 重启不会弹幽灵页）")

print("\n" + ("全部通过" if not fails else f"{fails} 项失败"))
sys.exit(0 if not fails else 1)
