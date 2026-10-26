"""把 WorkBuddy 的 logo 转成 LVGL 可直接用的 RGB565 C 数组。

为什么要预合成背景：LVGL 的 ARGB8888 通道顺序（BGRA / ARGB）在不同版本
和不同软件渲染路径上不好静态确认，一旦搞错就是红绿互换。而屏幕页头是
固定纯色 COL_BG(0x0B0E14)，把透明背景直接合到这个色上再存 RGB565，
就完全绕开了 alpha —— 1.1KB、无混合开销、颜色不会错。
代价：页头背景色如果改了，这个文件要重新生成（一行命令的事）。

用法：
    py tools/gen_logo.py
"""

from __future__ import annotations

import io
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# 源图放在项目里而不是剪贴板目录 —— 剪贴板文件会被系统清掉，脚本就再也跑不了
SRC = os.path.join(HERE, "logo_new.png")
OUT = os.path.join(HERE, "..", "firmware", "main", "logo_wb.c")

# 源图本身就是 104x104 的完整图标（四角透明，圆角方块铺满画布），不需要裁剪。
# 24px 是页头时钟（28px 字号 / 30px 行高）左侧能放下的最大清晰尺寸；
# 再小眼睛就糊了，再大就要压缩时钟字号。
SIZE = 24
BG = (0x0B, 0x0E, 0x14)        # 必须和 main.cpp 的 COL_BG 一致


def rgb565_le(r: int, g: int, b: int) -> bytes:
    """LVGL 的 RGB565 在内存里是小端：低字节先。"""
    v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return bytes((v & 0xFF, (v >> 8) & 0xFF))


def main() -> None:
    im = Image.open(SRC).convert("RGBA")
    icon = im.resize((SIZE, SIZE), Image.LANCZOS)

    # 预合成到纯色背景
    flat = Image.new("RGB", (SIZE, SIZE), BG)
    flat.paste(icon, (0, 0), icon)

    raw = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            raw += rgb565_le(*flat.getpixel((x, y)))

    lines = []
    for i in range(0, len(raw), 12):
        chunk = raw[i:i + 12]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")

    src = f"""/*******************************************************************************
 * WorkBuddy logo {SIZE}x{SIZE}，RGB565 小端，透明背景已预合成到 0x0B0E14
 *
 * 由 tools/gen_logo.py 生成，不要手改。
 * 页头背景色一旦调整，必须重新跑脚本，否则图标边缘会出现一圈色差。
 ******************************************************************************/

#include "lvgl.h"

static const uint8_t logo_wb_map[] = {{
{chr(10).join(lines)}
}};

const lv_image_dsc_t logo_wb = {{
    .header.magic  = LV_IMAGE_HEADER_MAGIC,
    .header.cf     = LV_COLOR_FORMAT_RGB565,
    .header.flags  = 0,
    .header.w      = {SIZE},
    .header.h      = {SIZE},
    .header.stride = {SIZE * 2},
    .data_size     = {SIZE * SIZE * 2},
    .data          = logo_wb_map,
}};
"""
    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(src)

    print(f"wrote {os.path.abspath(OUT)}  {len(raw)} bytes ({SIZE}x{SIZE} RGB565)")


if __name__ == "__main__":
    main()
