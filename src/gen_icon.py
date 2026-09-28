# Generates src/icon.ico (multi-size) for mdzy. Requires Pillow.
# Design: rounded indigo square, white page with folded corner, Markdown "M" + down arrow.
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024  # draw large, downsample for crisp edges


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # background: rounded square, vertical gradient
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    top, bottom = (88, 101, 242), (49, 46, 129)
    for y in range(S):
        gd.line([(0, y), (S, y)], fill=lerp(top, bottom, y / (S - 1)) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=220, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)

    # page with folded corner
    L, T, R, B = 212, 150, 812, 874
    fold = 150
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).polygon([(L + 18, T + 26), (R - fold + 18, T + 26), (R + 18, T + fold + 26),
                                    (R + 18, B + 26), (L + 18, B + 26)], fill=(0, 0, 0, 70))
    img.alpha_composite(shadow)
    d.polygon([(L, T), (R - fold, T), (R, T + fold), (R, B), (L, B)], fill=(250, 250, 255, 255))
    d.polygon([(R - fold, T), (R - fold, T + fold), (R, T + fold)], fill=(199, 203, 240, 255))

    # Markdown mark: "M" and a down arrow
    ink = (67, 56, 202, 255)
    my0, my1 = 470, 740
    d.polygon([(290, my1), (290, my0), (350, my0), (425, 590), (500, my0), (560, my0), (560, my1),
               (500, my1), (500, 590), (425, 690), (350, 590), (350, my1)], fill=ink)
    ax = 668
    d.rectangle([ax - 34, my0, ax + 34, 630], fill=ink)
    d.polygon([(ax - 100, 610), (ax + 100, 610), (ax, my1)], fill=ink)

    # text lines hint at the top of the page
    line = (205, 208, 235, 255)
    d.rounded_rectangle([290, 280, 560, 318], radius=19, fill=line)
    d.rounded_rectangle([290, 360, 700, 398], radius=19, fill=line)
    return img


def main():
    big = render()
    sizes = [256, 128, 64, 48, 32, 24, 16]
    frames = [big.resize((s, s), Image.LANCZOS) for s in sizes]
    frames[0].save(os.path.join(HERE, "icon.ico"), sizes=[(s, s) for s in sizes], append_images=frames[1:])
    frames[0].save(os.path.join(HERE, "..", "docs", "icon.png"))
    print("icon.ico written")


if __name__ == "__main__":
    main()
