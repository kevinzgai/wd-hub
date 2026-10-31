"""主机侧 ask 作答解码验证：用真实 _decode_ask_picks 处理设备发来的帧。

配合 test_ask.c 里打出的真实上行帧：
  {"type":"ask_answer","id":9101,"index":2}
  {"type":"ask_answer","id":9202,"mask":5}
验证主机能否把这些字节正确还原成选项下标。
"""
import importlib.util, datetime, json, sys

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

# 拿真实的解码函数
fn = getattr(m.UnifiedDaemon, "_decode_ask_picks", None)
print("签名 OK（staticmethod）")
if fn is None:
    print("找不到 _decode_ask_picks")
    sys.exit(2)

# 它是实例方法但只用参数，用 None 作 self
import types
def call(_unused, mask, index, n_opts, is_multi):
    # _decode_ask_picks 是 staticmethod，签名为 (mask, index, n_opts, is_multi)
    return fn(mask, index, n_opts, is_multi)

print("=== 单选 index=2（4 选项）===")
r = call(None, None, 2, 4, False)
print("   picks =", r)
ck(r == [2], "index=2 → 第 3 个选项")

print("=== 单选 index=0 ===")
ck(call(None, None, 0, 4, False) == [0], "index=0 → 第 1 个选项")

print("=== 单选越界 index=9 ===")
ck(call(None, None, 9, 4, False) == [], "越界 index → 空（拒绝无效答案）")

print("=== 单选 index=None ===")
ck(call(None, None, None, 4, False) == [], "缺 index → 空")

print("=== 多选 mask=5（二进制 0101 → 第1、3 项）===")
r = call(None, 5, None, 4, True)
print("   picks =", r)
ck(r == [0, 2], "mask=5 → [0, 2]")

print("=== 多选 mask=0 ===")
ck(call(None, 0, None, 4, True) == [], "mask=0 → 空（一项没勾）")

print("=== 多选 mask 高位越界 mask=0xF0 ===")
r = call(None, 0xF0, None, 4, True)
print("   picks =", r)
ck(all(i < 4 for i in r), "越界位被过滤掉")

print("=== 多选 mask=1 ===")
ck(call(None, 1, None, 4, True) == [0], "mask=1 → [0]")

# 再验证：把设备真实上行帧串成 JSON 后能否解析出字段
print("\n=== 设备上行帧字段名对拍 ===")
dev_ans = json.loads('{"type":"ask_answer","id":9101,"index":2}')
ck(dev_ans["type"] == "ask_answer", "type 字段")
ck(dev_ans["id"] == 9101, "id 字段（主机靠它认领当前题）")
ck(dev_ans["index"] == 2, "index 字段（主机 _handle_ask_answer 读的键）")
dev_mask = json.loads('{"type":"ask_answer","id":9202,"mask":5}')
ck(dev_mask["mask"] == 5, "mask 字段（多选路径的键）")

print("\n" + ("全部通过" if not fails else f"{fails} 项失败"))
sys.exit(0 if not fails else 1)
