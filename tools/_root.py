"""统一的项目根目录定位。

为什么需要它：
  tools/ 下的脚本原来是平铺的，都用 `parent.parent` 或
  `dirname(dirname(__file__))` 表示项目根 —— 脚本一移进子目录
  （tools/tones/、tools/ask-debug/…）这些写法就全部指错一层。

  与其每移一次就改一遍相对层数（移两层又错、再加一层还得改），
  不如让所有脚本用同一个"向上查特征目录"的方式定位。

判据是 `firmware/` + `host/` 同时存在 —— 这两个目录是项目骨架，
不会因为整理 tools/ 而消失。

用法：
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from _root import project_root
    ROOT = project_root()
"""
from __future__ import annotations

import os
from pathlib import Path

# 骨架目录：任意一个存在即可确认"这就是项目根"
SKELETON = ("firmware", "host")


def project_root(start: str | os.PathLike | None = None) -> Path:
    """从 start（默认本文件所在目录）逐级向上，找到同时具备骨架目录的根。

    找不到时抛 RuntimeError 而不是猜一个：脚本被放到项目外时，
    悄悄返回某个错误目录会让它去改错地方的固件头文件 —— 那种破坏
    远比直接报错严重。
    """
    here = Path(start).resolve() if start else Path(__file__).resolve().parent
    for p in [here, *here.parents]:
        if all((p / name).is_dir() for name in SKELETON):
            return p
    raise RuntimeError(
        f"找不到项目根目录（从 {here} 向上都没有 "
        f"{'/'.join(SKELETON)}）。本脚本必须在项目目录内运行。")
