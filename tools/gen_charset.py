# 从 bridge 实际数据中提取所有可能出现在屏幕上的中文字符，
# 生成最小字符集，避免打包整个 GB2312 浪费 Flash。
import json, sys, pathlib, urllib.request

chars = set()

# 1) UI 固定文案
chars |= set("工作中已完成自动化会话空闲连接离线更新于暂无数据")
chars |= set("正在加载等待网络错误重试成功失败")

# 2) 从 bridge 拉当前真实数据
try:
    raw = urllib.request.urlopen("http://127.0.0.1:8080/status", timeout=5).read().decode("utf-8")
    d = json.loads(raw)
    for s in d.get("sessions", []):
        for k in ("title", "project", "model"):
            chars |= set(s.get(k) or "")
except Exception as e:
    print(f"[warn] bridge 不可达，只用固定字符集: {e}", file=sys.stderr)

# 3) 从数据库里取所有历史会话标题，覆盖将来可能出现的中文
import sqlite3, os
db = pathlib.Path(os.path.expanduser("~")) / ".workbuddy" / "workbuddy.db"
if db.exists():
    try:
        c = sqlite3.connect(f"file:{db.as_posix()}?mode=ro", uri=True)
        for (t,) in c.execute("SELECT title FROM sessions WHERE title IS NOT NULL LIMIT 500"):
            chars |= set(t)
        for (t,) in c.execute("SELECT custom_title FROM sessions WHERE custom_title IS NOT NULL LIMIT 500"):
            chars |= set(t)
        for (t,) in c.execute("SELECT name FROM automations WHERE name IS NOT NULL"):
            chars |= set(t)
        c.close()
    except Exception as e:
        print(f"[warn] 读库失败: {e}", file=sys.stderr)

# 4) ASCII 全量（数字、字母、标点）
for i in range(0x20, 0x7F):
    chars.add(chr(i))

# 去掉控制字符与不可打印
chars = {c for c in chars if c.isprintable() and c not in "\r\n\t"}

out = "".join(sorted(chars))
pathlib.Path("charset.txt").write_text(out, encoding="utf-8")
print(f"字符集大小: {len(chars)}")
print(f"其中中文: {sum(1 for c in chars if ord(c) > 0x4E00)}")
