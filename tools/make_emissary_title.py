"""The Emissary logo for Brawl Online's title card, drawn from shapes so its edges are clean at any size.
   python tools/make_emissary_title.py
Writes wiicompiled/runtime/assets/brawl_online/emissary_mask.png (the mask with its spikes, wings and eyes) and
emissary_wordmark.png ("emissary" over its swoosh), which a build with Brawl Online's core puts in Sys/Resources for its
title card. The shapes were traced from Sarah's Emissary logo (brawlonline.net board, card 764): the
mask made symmetric, the swoosh as curves through its visible edges, and the word set in Impact (the logo's font, at the
logo's letter positions) with its white outline. Needs Windows' Impact (C:/Windows/Fonts/impact.ttf) and Pillow.
Coordinates are the traced logo's pixels; SS is the supersampling used before the final downscale."""
import os

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SS = 4

BLACK = (0, 0, 0, 255)
CRIMSON = (158, 0, 67, 255)
AMBER = (255, 209, 129, 255)
WHITE = (255, 255, 255, 255)

# The mask, symmetric about x = AXIS. Left-hand shapes; the right-hand ones are their mirror images.
AXIS = 304.5
OUTER_SPIKE = [(216.85, 119.55), (222.15, 147.4), (244.15, 163.9), (239.2, 134.85)]
INNER_SPIKE = [(248.95, 100.5), (249.15, 128.9), (267.55, 148.4), (268.35, 119.5)]
WING = [(166.1, 148.8), (171.15, 196.9), (196.8, 213.7), (196.35, 166.5)]
# The visor: top corners, the notch between the eyes, the lower corners and the chin.
VISOR = [(217.5, 179.0), (AXIS, 219.0), (2 * AXIS - 217.5, 179.0), (2 * AXIS - 224.05, 222.4), (AXIS, 271.5),
         (224.05, 222.4)]
# The left eye: centre, width, height, and the tilt of its long axis (degrees, top leaning outwards); the visor's top
# edge cuts it.
EYE = (259.45, 209.26, 17.85, 24.98, 12.0)

# The swoosh: curves through its top edge (left tip to right tip), its lower right edge (right tip to the bottom point)
# and its left edge (bottom point to left tip).
SWOOSH_TOP = [(12, 385), (40, 390), (70, 397), (120, 412), (165, 419.5), (215, 419), (265, 416), (310, 414), (365, 411),
              (435, 406), (520, 401.5), (580, 397.5), (630, 394.5), (700, 389.5), (745, 387), (790, 383.5), (851, 378)]
SWOOSH_BOTTOM = [(851, 378), (800, 390.5), (750, 398.5), (700, 412.5), (630, 427.5), (580, 438.5), (535, 448.5),
                 (495, 457.5), (445, 465.5), (370, 479.5), (340, 485.5), (300, 492.5), (250, 509.5), (200, 537.5),
                 (170, 557.5), (149, 569)]
SWOOSH_LEFT = [(149, 569), (140, 553.5), (135, 541.5), (129, 529.5), (122, 517.5), (116, 505.5),
               (76, 458.5), (66, 446.5), (56, 434.5), (46, 422.5), (35, 410.5), (24, 398.5), (12, 385)]
# Impact at FONT_SIZE, each letter's top-left as in the logo, and the white outline's width.
FONT_SIZE = 184
LETTERS = [("e", 70.5, 295.0), ("m", 166.5, 295.0), ("i", 307.5, 295.0), ("s", 360.5, 295.0), ("s", 447.5, 295.0),
           ("a", 535.5, 295.0), ("r", 628.5, 295.0), ("y", 698.5, 296.0)]
OUTLINE = 2.5


def catmull_rom(points, steps=12):
    """A smooth curve through `points` (ends included), as a list of points, then lightly averaged so uneven spacing
    between the traced points leaves no ripple."""
    p = [points[0]] + list(points) + [points[-1]]
    out = []
    for i in range(1, len(p) - 2):
        (x0, y0), (x1, y1), (x2, y2), (x3, y3) = p[i - 1], p[i], p[i + 1], p[i + 2]
        for s in range(steps):
            t = s / steps
            out.append(tuple(0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t * t +
                                    (-a + 3 * b - 3 * c + d) * t ** 3)
                             for a, b, c, d in ((x0, x1, x2, x3), (y0, y1, y2, y3))))
    out.append(points[-1])
    smooth = [out[0]]
    for i in range(1, len(out) - 1):
        lo, hi = max(0, i - 3), min(len(out), i + 4)
        smooth.append((sum(q[0] for q in out[lo:hi]) / (hi - lo), sum(q[1] for q in out[lo:hi]) / (hi - lo)))
    smooth.append(out[-1])
    return smooth


def mirror(shape):
    return [(2 * AXIS - x, y) for x, y in reversed(shape)]


def ellipse(cx, cy, w, h, tilt_deg, n=96):
    import math
    a = math.radians(tilt_deg)
    return [(cx + (w / 2) * math.cos(t) * math.cos(a) - (h / 2) * math.sin(t) * math.sin(a),
             cy + (w / 2) * math.cos(t) * math.sin(a) + (h / 2) * math.sin(t) * math.cos(a))
            for t in (2 * math.pi * k / n for k in range(n))]


class Canvas:
    """Drawing in logo pixels onto a supersampled RGBA image whose top-left is (x0, y0) and `scale` pixels per unit."""

    def __init__(self, x0, y0, x1, y1, width):
        self.x0, self.y0 = x0, y0
        self.scale = width / (x1 - x0)
        self.size = (width * SS, round((y1 - y0) * self.scale) * SS)
        self.img = Image.new("RGBA", self.size, (0, 0, 0, 0))

    def pt(self, x, y):
        return ((x - self.x0) * self.scale * SS, (y - self.y0) * self.scale * SS)

    def layer(self):
        return Image.new("L", self.size, 0)

    def polygon(self, layer, shape):
        ImageDraw.Draw(layer).polygon([self.pt(x, y) for x, y in shape], fill=255)

    def fill(self, layer, colour):
        self.img.alpha_composite(Image.composite(Image.new("RGBA", self.size, colour),
                                                 Image.new("RGBA", self.size, (0, 0, 0, 0)), layer))

    def result(self):
        return self.img.resize((self.size[0] // SS, self.size[1] // SS), Image.LANCZOS)


def mask_image(width=1600):
    c = Canvas(162.0, 96.5, 2 * AXIS - 162.0, 275.5, width)
    wings, spikes, visor, eyes = c.layer(), c.layer(), c.layer(), c.layer()
    for shape in (WING, mirror(WING)):
        c.polygon(wings, shape)
    for shape in (OUTER_SPIKE, mirror(OUTER_SPIKE), INNER_SPIKE, mirror(INNER_SPIKE)):
        c.polygon(spikes, shape)
    c.polygon(visor, VISOR)
    cx, cy, w, h, tilt = EYE
    c.polygon(eyes, ellipse(cx, cy, w, h, -tilt))
    c.polygon(eyes, ellipse(2 * AXIS - cx, cy, w, h, tilt))
    c.fill(wings, CRIMSON)
    c.fill(spikes, BLACK)
    c.fill(visor, BLACK)
    c.fill(ImageChops.multiply(eyes, visor), AMBER)
    return c.result()


def wordmark_image(width=2112):
    c = Canvas(8.0, 326.0, 856.0, 574.0, width)
    swoosh = c.layer()
    c.polygon(swoosh, catmull_rom(SWOOSH_TOP)[:-1] + catmull_rom(SWOOSH_BOTTOM)[:-1] + catmull_rom(SWOOSH_LEFT)[:-1])
    c.fill(swoosh, CRIMSON)
    font = ImageFont.truetype("C:/Windows/Fonts/impact.ttf", round(FONT_SIZE * c.scale * SS))
    outline, letters = c.layer(), c.layer()
    stroke = round(OUTLINE * c.scale * SS)
    for ch, x, y in LETTERS:
        ImageDraw.Draw(outline).text(c.pt(x, y), ch, font=font, fill=255, stroke_width=stroke, stroke_fill=255)
        ImageDraw.Draw(letters).text(c.pt(x, y), ch, font=font, fill=255)
    c.fill(outline, WHITE)
    c.fill(letters, BLACK)
    return c.result()


def main():
    out = os.path.join(ROOT, "wiicompiled", "runtime", "assets", "brawl_online")
    os.makedirs(out, exist_ok=True)
    mask_image().save(os.path.join(out, "emissary_mask.png"), optimize=True)
    wordmark_image().save(os.path.join(out, "emissary_wordmark.png"), optimize=True)
    print("emissary_mask.png, emissary_wordmark.png written")


if __name__ == "__main__":
    main()
