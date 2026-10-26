"""截图看板演示页，确认真实渲染。

跨平台：不再写死 Windows 的 chromium 路径，也不再写死 8080 端口。
- 浏览器：优先复用本机已装的 chromium，否则用 playwright 自带的。
- 端口：默认 27777（与服务端 DEFAULT_PORT 一致），可用 BOARD_PORT 覆盖。

用法：
    python scripts/shot_board.py                    # 默认 127.0.0.1:27777
    BOARD_PORT=9000 python scripts/shot_board.py    # 指定端口
"""

import asyncio
import os
import pathlib
import sys

from playwright.async_api import async_playwright

OUT = pathlib.Path(__file__).parent / "screenshots"
OUT.mkdir(exist_ok=True)

HOST = os.environ.get("BOARD_HOST", "127.0.0.1")
PORT = os.environ.get("BOARD_PORT", "27777")
URL = f"http://{HOST}:{PORT}/"

# Windows 上历史遗留的手工 chromium 路径，存在就优先复用，别重复下载
WIN_CHROMIUM = r"C:\Users\Administrator\AppData\Local\ms-playwright\chromium-1200\chrome-win64\chrome.exe"


def _launch_kwargs() -> dict:
    """能复用本地 chromium 就复用，否则用 playwright 自带的。"""
    if os.name == "nt" and os.path.isfile(WIN_CHROMIUM):
        return {"executable_path": WIN_CHROMIUM, "args": ["--no-proxy-server"]}
    return {}


async def main() -> int:
    async with async_playwright() as p:
        try:
            browser = await p.chromium.launch(headless=True, **_launch_kwargs())
        except Exception as e:
            print(f"[ERROR] 启动浏览器失败: {e}")
            print("        装依赖：pip install -r server/requirements-dev.txt")
            print("        下浏览器：python -m playwright install chromium")
            return 1

        page = await browser.new_page(viewport={"width": 1280, "height": 1000})
        errs: list[str] = []
        page.on("console", lambda m: errs.append(f"{m.type}: {m.text}")
                if m.type == "error" else None)
        page.on("pageerror", lambda e: errs.append(f"pageerror: {e}"))

        try:
            await page.goto(URL, wait_until="networkidle", timeout=15000)
        except Exception as e:
            print(f"[ERROR] 打开 {URL} 失败: {e}")
            print("        服务没起？先跑 `python -m server.api_server`")
            await browser.close()
            return 1

        await page.wait_for_timeout(2500)

        # 关键文本是否渲染出来（都是页面里的真实 id）
        for sel, name in [("#sWorking", "工作中"), ("#sDone", "今日完成"),
                          ("#sAuto", "自动化"), ("#ver", "版本号"),
                          ("#pulseTxt", "状态")]:
            try:
                txt = await page.inner_text(sel)
            except Exception as e:
                txt = f"<读取失败: {e}>"
            print(f"  {name:8} = {txt!r}")

        for sel, name in [("#tTotal", "任务总数"), ("#tDone", "累计完成"),
                          ("#tToday", "今日完成(任务)"), ("#aFiles", "改动文件"),
                          ("#numCpu", "CPU"), ("#numMem", "内存"), ("#numDisk", "磁盘")]:
            try:
                txt = await page.inner_text(sel)
            except Exception as e:
                txt = f"<读取失败: {e}>"
            print(f"  {name:12} = {txt!r}")

        try:
            sess = await page.inner_text("#sessList")
            print(f"  会话列表 = {sess[:120]!r}")
        except Exception as e:
            print(f"  会话列表 = <读取失败: {e}>")

        out = OUT / "board_demo.png"
        await page.screenshot(path=str(out), full_page=True)
        print(f"截图: {out}")
        print("控制台错误:", errs if errs else "无")
        await browser.close()
        return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
