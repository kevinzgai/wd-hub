"""生成 WORKBUDDY 看板图标：深色底 + 青色信号环，跟看板页面同一套色值。"""

from pathlib import Path

from PIL import Image, ImageDraw

RES = Path(__file__).resolve().parent.parent / "android/app/src/main/res"

BG = (11, 14, 19, 255)
ACCENT = (34, 211, 238)
ACCENT2 = (103, 232, 249)

DENSITIES = {
    "mdpi": 1.0,
    "hdpi": 1.5,
    "xhdpi": 2.0,
    "xxhdpi": 3.0,
    "xxxhdpi": 4.0,
}

SS = 4  # 超采样倍数，边缘抗锯齿


def draw_mark(size: int, with_bg: bool, round_mask: bool = False) -> Image.Image:
    s = size * SS
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    if with_bg:
        if round_mask:
            d.ellipse([0, 0, s - 1, s - 1], fill=BG)
        else:
            d.rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.22), fill=BG)

    cx = cy = s / 2

    # 外圈：两层同心环，透明度递减，做出信号扩散的观感
    for radius_ratio, width_ratio, alpha in ((0.335, 0.016, 70), (0.245, 0.020, 130)):
        r = s * radius_ratio
        w = max(1, int(s * width_ratio))
        d.ellipse(
            [cx - r, cy - r, cx + r, cy + r],
            outline=ACCENT2 + (alpha,),
            width=w,
        )

    # 中心光点：先铺几层低透明度的大圆当光晕，再压一个实心点
    for glow_ratio, alpha in ((0.16, 26), (0.13, 40), (0.105, 62)):
        r = s * glow_ratio
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=ACCENT + (alpha,))

    r = s * 0.072
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=ACCENT + (255,))

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    for name, scale in DENSITIES.items():
        folder = RES / f"mipmap-{name}"
        folder.mkdir(parents=True, exist_ok=True)

        legacy = int(48 * scale)
        draw_mark(legacy, with_bg=True).save(folder / "ic_launcher.png")
        draw_mark(legacy, with_bg=True, round_mask=True).save(folder / "ic_launcher_round.png")

        fg = int(108 * scale)
        draw_mark(fg, with_bg=False).save(folder / "ic_launcher_foreground.png")

    print("图标已生成：", ", ".join(DENSITIES))


if __name__ == "__main__":
    main()
