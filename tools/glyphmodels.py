#!/usr/bin/env python3
"""Extract the GameCube's talk prompt model from its boot.HIP.

The model that floats over an NPC you can talk to is fx_glyph_talk, a 3D
picture of the button that talks. Each disc models its own: the Xbox and PS2
discs a black trigger lettered R (Btn_xb_r), the GameCube its grey R button
(Btn_gc_r). The port runs the Xbox assets, so the GameCube's has to be carried
beside the executable, the way the button glyph sets are.

It comes out as `gamecube/rt.dff` -- named for the input it pictures, the same
as the glyph files beside it -- and `gamecube/Btn_gc_r.png`, the texture its
material names. src/SB/Core/pc/iPadGlyph.cpp loads a model's textures from
PNGs in its own folder by that name.

The geometry is RenderWare's generic format, not GameCube native data, so the
MODL asset is a DFF as it stands and is written out unchanged.

Usage:
    python tools/glyphmodels.py <GameCube boot.HIP> [buttons directory]

The directory defaults to src/SB/Core/pc/res/buttons.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from padglyphs import hip_assets, native_struct, write_png

MODEL = "fx_glyph_talk"
TEXTURE = "Btn_gc_r"


def rgb565(c):
    return [((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31, 255]


def decode_cmpr(blob):
    """A GameCube CMPR texture: DXT1 blocks, four to an 8x8 tile, big-endian,
    with each block's row of indices read from the high bits down."""
    o, _ = native_struct(blob)
    w, h = struct.unpack_from(">HH", blob, o + 92)
    depth, levels, fmt = struct.unpack_from(">BBB", blob, o + 96)
    if fmt != 14:
        raise ValueError("expected CMPR, got fmt=%d" % fmt)
    raw = blob[o + 108 : o + 108 + w * h // 2]
    out = bytearray(w * h * 4)
    i = 0
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for sy in (0, 4):
                for sx in (0, 4):
                    c0, c1, bits = struct.unpack_from(">HHI", raw, i)
                    i += 8
                    a, b = rgb565(c0), rgb565(c1)
                    if c0 > c1:
                        third = [(2 * a[k] + b[k]) // 3 for k in range(3)] + [255]
                        twothirds = [(a[k] + 2 * b[k]) // 3 for k in range(3)] + [255]
                        pal = [a, b, third, twothirds]
                    else:
                        half = [(a[k] + b[k]) // 2 for k in range(3)] + [255]
                        pal = [a, b, half, [0, 0, 0, 0]]
                    for p in range(16):
                        x = tx + sx + p % 4
                        y = ty + sy + p // 4
                        d = (y * w + x) * 4
                        out[d : d + 4] = bytes(pal[(bits >> (30 - 2 * p)) & 3])
    return w, h, bytes(out)


def main(argv):
    if len(argv) not in (2, 3):
        raise SystemExit(__doc__)

    here = os.path.dirname(os.path.abspath(__file__))
    outdir = argv[2] if len(argv) == 3 else os.path.join(here, "..", "src", "SB", "Core", "pc", "res", "buttons")
    outdir = os.path.join(outdir, "gamecube")
    os.makedirs(outdir, exist_ok=True)

    data, assets = hip_assets(argv[1])
    by_name = {name.split(".")[0]: (off, size, typ) for _, off, size, name, typ in assets}

    for want, typ in ((MODEL, "MODL"), (TEXTURE, "RWTX")):
        if want not in by_name or by_name[want][2] != typ:
            raise SystemExit("%s holds no %s %s; is it the GameCube boot.HIP?" % (argv[1], typ, want))

    off, size, _ = by_name[MODEL]
    with open(os.path.join(outdir, "rt.dff"), "wb") as f:
        f.write(data[off : off + size])

    off, size, _ = by_name[TEXTURE]
    w, h, rgba = decode_cmpr(data[off : off + size])
    write_png(os.path.join(outdir, TEXTURE + ".png"), w, h, rgba)

    print("wrote %s/rt.dff and %s.png" % (outdir, TEXTURE))


if __name__ == "__main__":
    main(sys.argv)
