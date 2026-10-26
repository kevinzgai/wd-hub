"""截图看板演示页，确认真实渲染。窗口模式，肉眼可看。"""
import asyncio, pathlib
from playwright.async_api import async_playwright

OUT = pathlib.Path(__file__).parent / "screenshots"
OUT.mkdir(exist_ok=True)

async def main():
    async with async_playwright() as p:
        # 复用本机已装的 chromium-1200，不下载新版本
        exe = r"C://Users//Administrator//AppData//Local//ms-playwright//chromium-1200//chrome-win64//chrome.exe"
        b = await p.chromium.launch(headless=False, executable_path=exe,
                                    args=["--no-proxy-server"])
        pg = await b.new_page(viewport={"width": 1280, "height": 1000})
        errs = []
        pg.on("console", lambda m: errs.append(f"{m.type}: {m.text}") if m.type == "error" else None)
        pg.on("pageerror", lambda e: errs.append(f"pageerror: {e}"))

        await pg.goto("http://127.0.0.1:8080/", wait_until="networkidle")
        await pg.wait_for_timeout(2500)

        # 关键文本是否渲染出来
        for sel, name in [("#sWorking", "工作中"), ("#sDone", "今日完成"),
                          ("#sAuto", "自动化"), ("#ver", "版本号"),
                          ("#pulseTxt", "状态")]:
            txt = await pg.inner_text(sel)
            print(f"  {name:8} = {txt!r}")

        sess = await pg.inner_text("#sessList")
        print(f"  会话列表 = {sess[:80]!r}")

        await pg.screenshot(path=str(OUT / "board_demo.png"), full_page=True)
        print("截图:", OUT / "board_demo.png")
        print("控制台错误:", errs if errs else "无")
        await b.close()

asyncio.run(main())
