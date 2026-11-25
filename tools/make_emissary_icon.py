"""EmissaryRecomp's program icon: Dakota's Emissary emblem (the helmet with its bat wings, tools/emissary_emblem.png) on
a tile of the title card's night sky with its crimson glow. At 32 px and under only the helmet is drawn: the wings blur
into the tile there.
   python tools/make_emissary_icon.py
Writes, in wiicompiled/runtime/assets/icon/:
  emissary.ico   Windows: the program's icon (16-256 px), linked into WiiCompiled.exe; the window's and the taskbar's
  emissary.icns  macOS: Emissary.app's icon (16-1024 px, the rounded tile of macOS's icon grid)
  emissary.png   the window's icon on Linux (256 px), which the game sets on its window (the taskbar's)
tools/emissary_emblem.png is the emblem cut from Dakota's logo (Brawl Online board, card 816) onto transparency; the
helmet's ring is HELMET (its centre and outer radius, in the emblem's pixels). Needs Pillow."""
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "wiicompiled", "runtime", "assets", "icon")
EMBLEM_PATH = os.path.join(ROOT, "tools", "emissary_emblem.png")
SS = 4  # supersampling before the final downscale

SKY_TOP = (12, 14, 30)  # the title card's night sky, top to bottom (settings_overlay.cpp)
SKY_BOTTOM = (52, 26, 40)
GLOW = (160, 0, 63)  # the card's crimson glow round the emblem
HELMET = (530.0, 361.0, 266.0)
SMALL = 32  # this size and under: the helmet alone


def emblem():
    return Image.open(EMBLEM_PATH).convert("RGBA")


def helmet(art):
    """The emblem without its wings: the helmet's ring and what is inside it."""
    cx, cy, r = HELMET
    ring = Image.new("L", art.size, 0)
    ImageDraw.Draw(ring).ellipse((cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2), fill=255)
    out = art.copy()
    out.putalpha(ImageChops.multiply(art.getchannel("A"), ring))
    return out.crop(out.getbbox())


def tile(size, inset, radius, shadow):
    """The tile's shape (L) on a size x size canvas: a rounded square `inset` from each edge, corners `radius` (both
    shares of size); with `shadow`, the soft shadow under it (macOS's icon grid)."""
    s = size * SS
    shape = Image.new("L", (s, s), 0)
    x0, x1 = inset * s, s - inset * s
    ImageDraw.Draw(shape).rounded_rectangle((x0, x0, x1, x1), radius=radius * s, fill=255)
    shade = None
    if shadow:
        shade = Image.new("L", (s, s), 0)
        ImageDraw.Draw(shade).rounded_rectangle((x0, x0 + 0.012 * s, x1, x1 + 0.012 * s), radius=radius * s, fill=90)
        shade = shade.filter(ImageFilter.GaussianBlur(0.02 * s))
    return shape, shade


def icon(size, art, mac=False):
    """One size of the icon, RGBA. `art` is (the emblem, the helmet alone)."""
    s = size * SS
    small = size <= SMALL
    # Windows / Linux: a tile filling the icon (slightly inset so it reads as one); macOS: Apple's grid (an 824 px
    # rounded square in 1024 with a shadow).
    if mac:
        inset, radius = 100 / 1024, 185 / 1024
    else:
        inset, radius = (0.0 if small else 0.03), (0.16 if small else 0.2)
    shape, shade = tile(size, inset, radius, mac and size >= 64)
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    if shade is not None:
        img.alpha_composite(Image.merge("RGBA", (shade.point(lambda _: 0),) * 3 + (shade,)))
    # The sky: top to bottom.
    sky = Image.new("RGBA", (1, s))
    for y in range(s):
        t = y / max(1, s - 1)
        sky.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_BOTTOM)) + (255,))
    sky = sky.resize((s, s))
    img.alpha_composite(Image.composite(sky, Image.new("RGBA", (s, s), (0, 0, 0, 0)), shape))

    # The emblem across most of the tile (the helmet nearly all of it), centred, no taller than 90% of it.
    picture = art[1] if small else art[0]
    inner = s * (1 - 2 * inset)
    w = inner * (0.98 if small else 0.88)
    h = w * picture.height / picture.width
    if h > inner * 0.9:
        h = inner * 0.9
        w = h * picture.width / picture.height
    picture = picture.resize((round(w), round(h)), Image.LANCZOS)
    layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    layer.alpha_composite(picture, (round((s - picture.width) / 2), round((s - picture.height) / 2)))
    alpha = layer.getchannel("A")

    def paint(mask, colour, k=1.0):
        img.alpha_composite(Image.merge("RGBA", tuple(Image.new("L", (s, s), c) for c in colour) +
                                        (mask.point(lambda v: round(v * k)),)))

    # The crimson glow behind it, then the emblem.
    paint(alpha.filter(ImageFilter.GaussianBlur(inner * 0.06)), GLOW, 0.95)
    paint(alpha.filter(ImageFilter.GaussianBlur(inner * 0.02)), GLOW, 0.6)
    img.alpha_composite(layer)
    # Nothing outside the tile.
    r, g, b, a = img.split()
    img = Image.merge("RGBA", (r, g, b, ImageChops.multiply(a, ImageChops.lighter(shape, shade or shape))))
    return img.resize((size, size), Image.LANCZOS)


def main():
    full = emblem()
    art = (full, helmet(full))
    os.makedirs(OUT, exist_ok=True)
    win_sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [icon(n, art) for n in win_sizes]
    frames[-1].save(os.path.join(OUT, "emissary.ico"), sizes=[(n, n) for n in win_sizes],
                    append_images=frames[:-1])
    icon(256, art).save(os.path.join(OUT, "emissary.png"), optimize=True)
    # macOS: Pillow writes every ICNS size from the largest image; each is drawn at its own size here instead.
    mac = {n: icon(n, art, mac=True) for n in (16, 32, 64, 128, 256, 512, 1024)}
    mac[1024].save(os.path.join(OUT, "emissary.icns"), append_images=[mac[n] for n in (16, 32, 64, 128, 256, 512)])
    preview = Image.new("RGBA", (1024 + 256 + 128 + 64 + 48 + 32 + 16 + 7 * 16, 1024), (40, 40, 40, 255))
    x = 0
    for img in (mac[1024], icon(256, art), icon(128, art), icon(64, art), icon(48, art), icon(32, art),
                icon(16, art)):
        preview.alpha_composite(img, (x, 0))
        x += img.width + 16
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    preview.save(os.path.join(ROOT, "build", "emissary_icon_preview.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
