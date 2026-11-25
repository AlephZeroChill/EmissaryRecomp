"""EmissaryRecomp's program icon: the title card's Emissary mask (tools/make_emissary_title.py's shapes) on a tile of the
card's night sky, with its crimson glow and the eyes' light, drawn from the shapes at every size so small icons stay
sharp.
   python tools/make_emissary_icon.py
Writes, in wiicompiled/runtime/assets/icon/:
  emissary.ico   Windows: the program's icon (16-256 px), linked into WiiCompiled.exe
  emissary.icns  macOS: Emissary.app's icon (16-1024 px, the rounded tile of macOS's icon grid)
  emissary.png   the window's icon (256 px), which the game sets on its window (the taskbar's on Windows and Linux)
Needs Pillow."""
import math
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_emissary_title as T  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "wiicompiled", "runtime", "assets", "icon")
SS = 4  # supersampling before the final downscale

SKY_TOP = (12, 14, 30)  # the title card's night sky, top to bottom (settings_overlay.cpp)
SKY_BOTTOM = (52, 26, 40)
GLOW = (160, 0, 63)  # the card's crimson glow round the mask
EYE_LIGHT = (255, 201, 122)

# The mask's extent in logo units (make_emissary_title.mask_image's canvas), and its middle.
MX0, MY0, MX1, MY1 = 162.0, 96.5, 2 * T.AXIS - 162.0, 275.5


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


def icon(size, mac=False):
    """One size of the icon, RGBA."""
    s = size * SS
    small = size <= 32
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

    # The mask, as wide as most of the tile (wider at small sizes, where detail is lost first), a little above the
    # middle like the card's.
    inner = s * (1 - 2 * inset)
    width = inner * (0.94 if small else 0.8)
    scale = width / (MX1 - MX0)
    ox = s / 2 - (MX0 + MX1) / 2 * scale
    oy = s / 2 - (MY0 + MY1) / 2 * scale + inner * 0.01

    def pt(x, y):
        return (ox + x * scale, oy + y * scale)

    def layer(shapes):
        lay = Image.new("L", (s, s), 0)
        for shp in shapes:
            ImageDraw.Draw(lay).polygon([pt(x, y) for x, y in shp], fill=255)
        return lay

    wings = layer([T.WING, T.mirror(T.WING)])
    spikes = layer([T.OUTER_SPIKE, T.mirror(T.OUTER_SPIKE), T.INNER_SPIKE, T.mirror(T.INNER_SPIKE)])
    visor = layer([T.VISOR])
    cx, cy, w, h, tilt = T.EYE
    eyes = ImageChops.multiply(layer([T.ellipse(cx, cy, w, h, -tilt), T.ellipse(2 * T.AXIS - cx, cy, w, h, tilt)]),
                               visor)
    black = ImageChops.lighter(spikes, visor)
    whole = ImageChops.lighter(black, wings)

    def paint(mask, colour, alpha=1.0):
        a = mask.point(lambda v: round(v * alpha))
        img.alpha_composite(Image.merge("RGBA", tuple(Image.new("L", (s, s), c) for c in colour) + (a,)))

    def clip():
        # Nothing outside the tile.
        r, g, b, a = img.split()
        return Image.merge("RGBA", (r, g, b, ImageChops.multiply(a, ImageChops.lighter(shape, shade or shape))))

    # The crimson glow behind the mask, and the eyes' light.
    paint(whole.filter(ImageFilter.GaussianBlur(inner * 0.06)), GLOW, 0.95)
    paint(whole.filter(ImageFilter.GaussianBlur(inner * 0.02)), GLOW, 0.6)
    # A tight crimson edge, so the black spikes and visor stand out from the dark tile at any size.
    paint(black.filter(ImageFilter.GaussianBlur(max(SS * 0.8, inner * 0.008))), GLOW, 0.9)
    paint(wings, T.CRIMSON[:3])
    paint(black, (0, 0, 0))
    paint(eyes.filter(ImageFilter.GaussianBlur(inner * 0.025)), EYE_LIGHT, 0.85)
    paint(eyes, T.AMBER[:3])
    img = clip()
    return img.resize((size, size), Image.LANCZOS)


def main():
    os.makedirs(OUT, exist_ok=True)
    win_sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    frames = [icon(n) for n in win_sizes]
    frames[-1].save(os.path.join(OUT, "emissary.ico"), sizes=[(n, n) for n in win_sizes],
                    append_images=frames[:-1])
    icon(256).save(os.path.join(OUT, "emissary.png"), optimize=True)
    # macOS: Pillow writes every ICNS size from the largest image; each is drawn at its own size here instead.
    mac = {n: icon(n, mac=True) for n in (16, 32, 64, 128, 256, 512, 1024)}
    mac[1024].save(os.path.join(OUT, "emissary.icns"), append_images=[mac[n] for n in (16, 32, 64, 128, 256, 512)])
    preview = Image.new("RGBA", (1024 + 256 + 128 + 64 + 48 + 32 + 16 + 7 * 16, 1024), (40, 40, 40, 255))
    x = 0
    for img in (mac[1024], icon(256), icon(128), icon(64), icon(48), icon(32), icon(16)):
        preview.alpha_composite(img, (x, 0))
        x += img.width + 16
    preview.save(os.path.join(ROOT, "build", "emissary_icon_preview.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
