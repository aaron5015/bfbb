"""Draw the keyboard and mouse button-prompt set.

usage: python tools/keyglyphs.py [outdir]

outdir defaults to src/SB/Core/pc/res/buttons/keyboard.

Writes one 32x32 PNG per input a [keyboard] binding can name, named after the
binding token -- `e.png`, `space.png`, `mouse1.png` -- because that is what
iPadGlyph.cpp looks a glyph up by. Also `stick_left.png` and `stick_right.png`,
the pictures for "move" and "turn the camera", which no binding names.

Drawn to sit beside the disc's own glyphs: glossy, lit from the upper left,
outlined in black, with heavy lettering. A key is a charcoal cap like the Xbox
pad's black buttons, lettered white the way BACK and START are. A mouse button
is the mouse with that button lit gold, the colour of the Xbox Y.

Everything is drawn at 8x and box-filtered down, which is what keeps a 32-pixel
glyph from going jagged at the curves.

Needs Pillow.
"""

import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

SIZE = 32
SS = 8
W = SIZE * SS

FONT_HEAVY = "C:/Windows/Fonts/ariblk.ttf"
FONT_BOLD = "C:/Windows/Fonts/arialbd.ttf"

INK = (14, 14, 18, 255)
LETTER = (250, 250, 250, 255)

# The printed label where it is not the token in capitals. Kept to four or five
# characters: the prompt draws the glyph in a square the height of a line.
LABELS = {
    "space": "SPACE",
    "enter": "ENTER",
    "tab": "TAB",
    "escape": "ESC",
    "backspace": "BKSP",
    "shift": "SHIFT",
    "lshift": "SHIFT",
    "rshift": "SHIFT",
    "ctrl": "CTRL",
    "lctrl": "CTRL",
    "rctrl": "CTRL",
    "alt": "ALT",
    "lalt": "ALT",
    "ralt": "ALT",
    "insert": "INS",
    "delete": "DEL",
    "home": "HOME",
    "end": "END",
    "pageup": "PGUP",
    "pagedown": "PGDN",
    "capslock": "CAPS",
    "comma": ",",
    "period": ".",
    "minus": "-",
    "equals": "=",
    "semicolon": ";",
    "slash": "/",
    "tilde": "~",
    "lbracket": "[",
    "backslash": "\\",
    "rbracket": "]",
    "quote": "'",
}
for i in range(1, 13):
    LABELS["f%d" % i] = "F%d" % i
for i in range(26):
    LABELS[chr(ord("a") + i)] = chr(ord("A") + i)
for i in range(10):
    LABELS[str(i)] = str(i)

# Numpad keys carry a small NUM above the symbol, which is how a keypad key is
# told from its twin on the main board.
NUMPAD = {"numpad%d" % i: str(i) for i in range(10)}
NUMPAD.update({"numpadplus": "+", "numpadminus": "-", "numpadstar": "*",
               "numpadslash": "/", "numpaddot": "."})

ARROWS = {"up": 0, "right": 90, "down": 180, "left": 270}

MOUSE = ["mouse1", "mouse2", "mouse3", "mouse4", "mouse5", "wheelup", "wheeldown"]


def px(v):
    return int(round(v * W))


def rect(box):
    return [px(box[0]), px(box[1]), px(box[2]), px(box[3])]


def vgradient(size, top, bottom):
    """An RGBA image `size` filled top to bottom from `top` to `bottom`."""
    w, h = size
    col = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        col.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return col.resize((w, h))


def fill_mask(img, mask, top, bottom):
    """Paints a vertical gradient into `img` through `mask`, running from the
    top of the shape to its bottom."""
    bbox = mask.getbbox()
    if bbox is None:
        return
    grad = Image.new("RGBA", img.size, (0, 0, 0, 0))
    grad.paste(vgradient((img.size[0], bbox[3] - bbox[1]), top, bottom), (0, bbox[1]))
    img.alpha_composite(Image.composite(grad, Image.new("RGBA", img.size, (0, 0, 0, 0)), mask))


def rounded_mask(box, radius):
    m = Image.new("L", (W, W), 0)
    ImageDraw.Draw(m).rounded_rectangle(rect(box), radius=px(radius), fill=255)
    return m


def ellipse_mask(box):
    m = Image.new("L", (W, W), 0)
    ImageDraw.Draw(m).ellipse(rect(box), fill=255)
    return m


def gloss(img, box, strength=120, clip=None):
    """The highlight in the upper left that every disc glyph carries: a soft
    spot, brightest a third of the way into `box`, kept inside `clip`."""
    g = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(g)
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    cx, cy = x0 + w * 0.36, y0 + h * 0.30
    steps = 32
    for i in range(steps):
        t = i / steps  # 0 at the rim of the spot, 1 at its centre
        a = int(strength * (t ** 1.4))
        rx, ry = w * 0.62 * (1 - t), h * 0.50 * (1 - t)
        d.ellipse(rect((cx - rx, cy - ry, cx + rx, cy + ry)), fill=a)
    if clip is not None:
        g = ImageChops.multiply(g, clip)
    white = Image.new("RGBA", (W, W), (255, 255, 255, 255))
    img.alpha_composite(Image.composite(white, Image.new("RGBA", (W, W), (0, 0, 0, 0)), g))


def text_layer(text, font_path, stroke):
    """`text` in white with a black outline, cropped to its ink."""
    size = 400
    font = ImageFont.truetype(font_path, size)
    sw = int(size * stroke)
    probe = ImageDraw.Draw(Image.new("L", (1, 1)))
    l, t, r, b = probe.textbbox((0, 0), text, font=font, stroke_width=sw)
    layer = Image.new("RGBA", (r - l + 4, b - t + 4), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((2 - l, 2 - t), text, font=font, fill=LETTER,
                               stroke_width=sw, stroke_fill=INK)
    return layer.crop(layer.getbbox())


def place_text(img, text, box, font_path=FONT_HEAVY, squeeze=0.68, stroke=0.11):
    """Fits `text` inside `box`, filling its height where it can.

    A long word is narrowed before it is shrunk, down to `squeeze` of its own
    width, the way the disc letters START and SELECT.
    """
    layer = text_layer(text, font_path, stroke)
    bw, bh = px(box[2] - box[0]), px(box[3] - box[1])
    w, h = layer.size
    sy = bh / h
    sx = sy
    if w * sx > bw:
        sx = bw / w
        sy = min(sy, sx / squeeze)
    tw, th = max(1, int(w * sx)), max(1, int(h * sy))
    layer = layer.resize((tw, th), Image.LANCZOS)
    cx = px((box[0] + box[2]) / 2) - tw // 2
    cy = px((box[1] + box[3]) / 2) - th // 2
    img.alpha_composite(layer, (cx, cy))


def arrow(img, box, angle, fill=LETTER):
    """A solid arrow pointing `angle` degrees clockwise from up, outlined."""
    a = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(a)
    shape = [(0.50, 0.08), (0.92, 0.52), (0.66, 0.52), (0.66, 0.92), (0.34, 0.92), (0.34, 0.52),
             (0.08, 0.52)]
    pts = [(px(x), px(y)) for x, y in shape]
    d.polygon(pts, fill=fill, outline=INK, width=px(0.07))
    a = a.rotate(-angle, resample=Image.BICUBIC, center=(W / 2, W / 2))
    bw, bh = px(box[2] - box[0]), px(box[3] - box[1])
    a = a.resize((bw, bh), Image.LANCZOS)
    img.alpha_composite(a, (px(box[0]), px(box[1])))


def keycap(img, box=(0.0, 0.0, 1.0, 1.0)):
    """A key seen a little from above: rim, skirt, face. Returns the face box."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0

    def sub(a, b, c, d):
        return (x0 + w * a, y0 + h * b, x0 + w * c, y0 + h * d)

    r = min(w, h)
    outline = sub(0.02, 0.04, 0.98, 0.98)
    skirt = sub(0.075, 0.09, 0.925, 0.925)
    face = sub(0.13, 0.10, 0.87, 0.78)

    fill_mask(img, rounded_mask(outline, 0.22 * r), INK, INK)
    skirt_mask = rounded_mask(skirt, 0.17 * r)
    fill_mask(img, skirt_mask, (70, 70, 78, 255), (6, 6, 8, 255))
    # The face, with a lighter lip along its top edge.
    fill_mask(img, rounded_mask(sub(0.12, 0.085, 0.88, 0.795), 0.14 * r),
              (150, 150, 160, 255), (20, 20, 24, 255))
    face_mask = rounded_mask(face, 0.13 * r)
    fill_mask(img, face_mask, (78, 78, 88, 255), (10, 10, 12, 255))
    gloss(img, sub(0.08, 0.06, 0.92, 0.80), strength=150, clip=skirt_mask)
    return face


def draw_key(token):
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    fx0, fy0, fx1, fy1 = keycap(img)
    fw, fh = fx1 - fx0, fy1 - fy0

    if token in ARROWS:
        s = 0.80 * min(fw, fh)
        cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
        arrow(img, (cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2), ARROWS[token])
    elif token in NUMPAD:
        place_text(img, "NUM", (fx0 + fw * 0.12, fy0 + fh * 0.04, fx1 - fw * 0.12,
                                fy0 + fh * 0.30), FONT_BOLD, squeeze=0.8, stroke=0.14)
        place_text(img, NUMPAD[token], (fx0 + fw * 0.10, fy0 + fh * 0.34, fx1 - fw * 0.10,
                                        fy1 - fh * 0.02))
    else:
        label = LABELS[token]
        if len(label) == 1:
            # Punctuation is drawn at the height of the font's capitals rather
            # than blown up to fill the face, so a comma stays a comma.
            if label.isalnum():
                box = (fx0 + fw * 0.06, fy0 + fh * 0.03, fx1 - fw * 0.06, fy1 - fh * 0.01)
                place_text(img, label, box)
            else:
                place_punct(img, label, (fx0, fy0, fx1, fy1))
        else:
            n = len(label)
            hgt = {2: 0.74, 3: 0.60, 4: 0.50}.get(n, 0.44)
            cy = (fy0 + fy1) / 2
            box = (fx0 - fw * 0.02, cy - fh * hgt / 2, fx1 + fw * 0.02, cy + fh * hgt / 2)
            place_text(img, label, box, squeeze=0.62)
    return img


def place_punct(img, ch, face):
    """A punctuation key, scaled as the font sets it beside a capital."""
    fx0, fy0, fx1, fy1 = face
    size = 400
    font = ImageFont.truetype(FONT_HEAVY, size)
    sw = int(size * 0.11)
    probe = ImageDraw.Draw(Image.new("L", (1, 1)))
    cap = probe.textbbox((0, 0), "H", font=font, stroke_width=sw)
    l, t, r, b = probe.textbbox((0, 0), ch, font=font, stroke_width=sw)
    capw, caph = cap[2] - cap[0], cap[3] - cap[1]
    scale = px((fy1 - fy0) * 0.84) / caph

    layer = Image.new("RGBA", (max(capw, r - l) + 8, max(caph, b - cap[1], b - t) + 8 + size // 2),
                      (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((4 - l, 4 - cap[1]), ch, font=font, fill=LETTER, stroke_width=sw,
                               stroke_fill=INK)
    bbox = layer.getbbox()
    layer = layer.crop(bbox)
    w, h = layer.size
    layer = layer.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
    # Placed by the capital's box, so a comma sits low and a quote sits high.
    top = px((fy0 + fy1) / 2) - int(caph * scale) // 2 + int((bbox[1] - 4) * scale)
    left = px((fx0 + fx1) / 2) - layer.size[0] // 2
    img.alpha_composite(layer, (left, top))


GOLD_TOP = (255, 246, 170, 255)
GOLD_BOTTOM = (236, 146, 0, 255)
BODY_TOP = (170, 170, 180, 255)
BODY_BOTTOM = (36, 36, 42, 255)


def mouse_body(img, box):
    """The mouse, seen from above. Returns (body box, button line y)."""
    x0, y0, x1, y1 = box
    fill_mask(img, ellipse_mask(box), INK, INK)
    inner = (x0 + 0.045, y0 + 0.035, x1 - 0.045, y1 - 0.04)
    fill_mask(img, ellipse_mask(inner), BODY_TOP, BODY_BOTTOM)
    split = y0 + (y1 - y0) * 0.47
    return inner, split


def lit(img, mask, inner):
    fill_mask(img, ImageChops.multiply(mask, ellipse_mask(inner)), GOLD_TOP, GOLD_BOTTOM)


def draw_mouse(token):
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    box = (0.12, 0.01, 0.88, 0.99)
    inner, split = mouse_body(img, box)
    ix0, iy0, ix1, iy1 = inner
    cx = (ix0 + ix1) / 2

    def region(b):
        m = Image.new("L", (W, W), 0)
        ImageDraw.Draw(m).rectangle(rect(b), fill=255)
        return m

    if token == "mouse1":
        lit(img, region((0, 0, cx, split)), inner)
    elif token == "mouse2":
        lit(img, region((cx, 0, 1, split)), inner)

    # The seams: between the two buttons, and under them.
    d = ImageDraw.Draw(img)
    seam = px(0.035)
    d.line([(px(cx), px(iy0)), (px(cx), px(split))], fill=INK, width=seam)
    d.line([(px(ix0), px(split)), (px(ix1), px(split))], fill=INK, width=seam)

    # The wheel, lit for the middle button and the wheel itself.
    wheel = (cx - 0.065, iy0 + 0.09, cx + 0.065, split - 0.05)
    wheel_lit = token in ("mouse3", "wheelup", "wheeldown")
    d.rounded_rectangle(rect(wheel), radius=px(0.06), fill=INK)
    wm = rounded_mask((wheel[0] + 0.025, wheel[1] + 0.025, wheel[2] - 0.025, wheel[3] - 0.025),
                      0.04)
    if wheel_lit:
        fill_mask(img, wm, GOLD_TOP, GOLD_BOTTOM)
    else:
        fill_mask(img, wm, (110, 110, 118, 255), (50, 50, 56, 255))

    gloss(img, (ix0, iy0, ix1, iy0 + (iy1 - iy0) * 0.8), strength=170, clip=ellipse_mask(inner))

    lower = (ix0 + 0.06, split + 0.05, ix1 - 0.06, iy1 - 0.05)
    lw, lh = lower[2] - lower[0], lower[3] - lower[1]
    s = min(lw, lh) * 0.92
    lcx, lcy = (lower[0] + lower[2]) / 2, (lower[1] + lower[3]) / 2
    if token in ("wheelup", "wheeldown"):
        arrow(img, (lcx - s / 2, lcy - s / 2, lcx + s / 2, lcy + s / 2),
              0 if token == "wheelup" else 180, fill=GOLD_TOP)
    elif token in ("mouse4", "mouse5"):
        # The side buttons: lit on the thumb's flank, and numbered, since the
        # two sit together and which is which differs by make.
        flank = (ix0 - 0.03, split + (0.02 if token == "mouse5" else 0.17),
                 ix0 + 0.07, split + (0.15 if token == "mouse5" else 0.30))
        d.rounded_rectangle(rect((flank[0] - 0.025, flank[1] - 0.025, flank[2] + 0.025,
                                  flank[3] + 0.025)), radius=px(0.05), fill=INK)
        fill_mask(img, rounded_mask(flank, 0.04), GOLD_TOP, GOLD_BOTTOM)
        place_text(img, token[-1], (lcx - s * 0.30 + 0.04, lcy - s * 0.42, lcx + s * 0.30 + 0.04,
                                    lcy + s * 0.42))
    return img


def draw_wasd():
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    k = 0.345
    top = 0.03
    row = 0.50
    for letter, (x, y) in (("W", (0.5 - k / 2, top)), ("A", (0.0, row)),
                           ("S", (0.5 - k / 2, row)), ("D", (1.0 - k, row))):
        face = keycap(img, (x, y, x + k, y + 0.47))
        fx0, fy0, fx1, fy1 = face
        fw, fh = fx1 - fx0, fy1 - fy0
        place_text(img, letter, (fx0 + fw * 0.04, fy0 + fh * 0.02, fx1 - fw * 0.04,
                                 fy1 - fh * 0.0), stroke=0.14)
    return img


def draw_mouse_move():
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    inner, split = mouse_body(img, (0.27, 0.10, 0.73, 0.90))
    ix0, iy0, ix1, iy1 = inner
    cx = (ix0 + ix1) / 2
    d = ImageDraw.Draw(img)
    seam = px(0.03)
    d.line([(px(cx), px(iy0)), (px(cx), px(split))], fill=INK, width=seam)
    d.line([(px(ix0), px(split)), (px(ix1), px(split))], fill=INK, width=seam)
    gloss(img, (ix0, iy0, ix1, iy0 + (iy1 - iy0) * 0.8), strength=170, clip=ellipse_mask(inner))
    s = 0.27
    arrow(img, (0.0, 0.5 - s / 2, s, 0.5 + s / 2), 270, fill=GOLD_TOP)
    arrow(img, (1.0 - s, 0.5 - s / 2, 1.0, 0.5 + s / 2), 90, fill=GOLD_TOP)
    return img


def save(img, outdir, name):
    small = img.reduce(SS)
    small.save(os.path.join(outdir, name + ".png"), optimize=True)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        here, "..", "src", "SB", "Core", "pc", "res", "buttons", "keyboard")
    os.makedirs(outdir, exist_ok=True)

    count = 0
    for token in sorted(set(LABELS) | set(NUMPAD) | set(ARROWS)):
        save(draw_key(token), outdir, token)
        count += 1
    for token in MOUSE:
        save(draw_mouse(token), outdir, token)
        count += 1
    save(draw_wasd(), outdir, "stick_left")
    save(draw_mouse_move(), outdir, "stick_right")
    count += 2
    print("%d glyphs -> %s" % (count, os.path.normpath(outdir)))


if __name__ == "__main__":
    main()
