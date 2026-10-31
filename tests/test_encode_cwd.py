"""encode_cwd 路径编码回归测试（2026-10-31 修的「提问框永不出现」根因）。

被修的缺陷：encode_cwd 只按 Windows 盘符路径写，把 POSIX 路径开头的 "/"
当成盘符，还连带吃掉了 "U"：

    /Users/kevin/WebstormProjects/xiaozhi-workbuddy-board
      → "/-sers-kevin-WebstormProjects-xiaozhi-workbuddy-board"   ✗
    真实目录名：
      → "Users-kevin-WebstormProjects-xiaozhi-workbuddy-board"    ✓

后果：read_transcript 拼出的路径不存在 → 返回 None（**不报错**）→
pending 恒空 → 会话永不进 NEED_ANSWER → 设备永远不弹提问框。
状态/积分/消耗那些帧不走这条路径，全部正常，所以表面看"一切正常"。

本测试用**真实存在的转录路径**做断言：不只是字符串比对，
还要求编码结果指向一个确实存在的目录，否则测试无法证明修复有效。
"""
import importlib.util
import os
import sys

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


enc = m.encode_cwd

print("=== 1. POSIX 路径（macOS/Linux）—— 本次修的根因 ===")
ck(enc("/Users/kevin/WebstormProjects/xiaozhi-workbuddy-board")
   == "Users-kevin-WebstormProjects-xiaozhi-workbuddy-board",
   "macOS 工作目录编码正确（去掉前导 /，其余换成 -）")
ck(enc("/Users/kevin/WorkBuddy/PassportForWorkBuddy")
   == "Users-kevin-WorkBuddy-PassportForWorkBuddy",
   "含大写段目录编码正确")
ck(enc("/Users/kevin/Desktop/glass-ui")
   == "Users-kevin-Desktop-glass-ui",
   "短路径编码正确")

print("\n=== 2. 关键：不再吃掉首字母 U（老实现把 / 当盘符，跳过了 U）===")
got = enc("/Users/kevin/Desktop/glass-ui")
ck(got.startswith("Users-kevin"),
   f"编码结果以 'Users-kevin' 开头（实际: {got}）")
# 注意不能用 "sers-kevin" 做否定断言："Users" 本身含 "sers"，
# 老实现的坏产物是**以 '/-' 开头**的那串，判据要卡前缀而不是子串。
ck(not got.startswith("/-"),
   f"**不以 '/-' 开头** —— 老实现是 '/-sers-kevin…'（实际: {got}）")
ck(not got.startswith("/"),
   "**不以 '/' 开头** —— 老实现会带上这个假盘符")
ck("U" in got.split("-")[0],
   f"首段保留了首字母 U（首段 = {got.split('-')[0]}）")

print("\n=== 3. Windows 路径行为必须保持不变（不能修 macOS 时改坏 Windows）===")
ck(enc("C:\\Users\\foo\\bar") == "c-Users-foo-bar",
   "反斜杠 Windows 路径 → c-Users-foo-bar（盘符小写保留）")
ck(enc("C:/Users/foo/bar") == "c-Users-foo-bar",
   "正斜杠 Windows 路径结果一致")
ck(enc("D:\\work\\proj") == "d-work-proj", "其它盘符同样正确")

print("\n=== 4. 边界：空路径 / 根路径 ===")
ck(enc("") == "", "空路径返回空串")
ck(enc("/") == "", "根路径返回空串（不产生 '-'）")

print("\n=== 5. 真实目录：编码结果必须指向 projects/ 下确实存在的目录 ===")
projects = m.PROJECTS
if os.path.isdir(projects):
    real = sorted(os.listdir(projects))
    checked = 0
    for cwd in [
        "/Users/kevin/WebstormProjects/xiaozhi-workbuddy-board",
        "/Users/kevin/WebstormProjects/mengxin",
        "/Users/kevin/WorkBuddy/PassportForWorkBuddy",
    ]:
        e = enc(cwd)
        if e in real:
            checked += 1
            ck(True, f"编码结果命中真实目录: {e}")
        else:
            ck(False, f"编码结果找不到真实目录: {e}")
    ck(checked > 0, f"至少验证通过 1 个真实目录（共检查 {checked} 个命中）")

    # 反向断言：老实现的结果**不可能**是真实目录
    old = "/" + "-" + "sers-kevin-WebstormProjects-xiaozhi-workbuddy-board".lstrip("-")
    old_wrong = "/-sers-kevin-WebstormProjects-xiaozhi-workbuddy-board"
    ck(old_wrong not in real,
       f"老实现的产物 {old_wrong!r} 确实不在真实目录里 —— 证明修复是必要的")

print("\n=== 6. read_transcript：修复后必须能读到真实会话 ===")
# 用真实数据库里的 (cwd, sid) 去读——这是生产代码实际走的那条路，
# 比自己反推目录名可靠得多（反推会因为目录名里的 "-" 与路径分隔符
# 同形而拆错段，这里踩过一次）。
found = False
try:
    con = m.sqlite3_connect_ro(m.DB)
    rows = con.execute(
        "SELECT id, cwd FROM sessions WHERE deleted_at IS NULL "
        "ORDER BY last_activity_at DESC LIMIT 5").fetchall()
    con.close()
    for r in rows:
        cwd, sid = r["cwd"] or "", r["id"] or ""
        tr = m.read_transcript(cwd, sid)
        if tr is not None:
            found = True
            ck(True, f"read_transcript 读到 {sid[:12]}… "
                     f"（pending={len(tr['pending'])} "
                     f"active_tools={tr['active_tools']}）")
            break
    if not any(True for _ in [1] if found):
        ck(False, "前 5 个真实会话都读不到转录 —— 修复未生效")
except Exception as e:
    ck(False, f"读数据库失败: {e}")

# 反向断言：老编码结果下，同一个会话必然读不到
try:
    con = m.sqlite3_connect_ro(m.DB)
    row = con.execute(
        "SELECT id, cwd FROM sessions WHERE deleted_at IS NULL "
        "ORDER BY last_activity_at DESC LIMIT 1").fetchone()
    con.close()
    if row and (row["cwd"] or "").startswith("/"):
        old_enc = "/-" + (row["cwd"].lstrip("/").replace("/", "-"))[1:]
        old_path = os.path.join(m.PROJECTS, old_enc,
                                (row["id"] or "") + ".jsonl")
        ck(not os.path.exists(old_path),
           f"老编码路径不存在（{old_enc[:40]}…）—— 证明修复确实必要")
except Exception as e:
    ck(False, f"反向断言失败: {e}")

print("\n" + ("全部通过" if not fails else f"{fails} 项失败"))
sys.exit(0 if not fails else 1)
