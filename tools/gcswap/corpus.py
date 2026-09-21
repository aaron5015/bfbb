"""Pair every GameCube asset with the Xbox asset of the same id and type, and
write the pairs to one file for the checker.

    python tools/gcswap/corpus.py <gamecube files/> <xbox assets/> <out.bin>

The Xbox copy is the answer key: a GameCube asset swapped correctly is
byte-for-byte its Xbox twin, wherever the two builds shipped the same content.

Record: fourcc, id, GameCube size, Xbox size (u32 LE each), then both payloads.
Types whose payload is a RenderWare stream or audio are left out; they are not
byte-swapped.
"""
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import hipfile

SKIP = {"MODL", "RWTX", "JSP ", "BSP ", "SND ", "SNDS", "CSN ", "RAW "}


def load(root):
    out = {}
    for path in sorted(glob.glob(os.path.join(root, "**", "*"), recursive=True)):
        if path.upper().endswith((".HIP", ".HOP")) and os.path.isfile(path):
            for a in hipfile.Package.load(path).assets:
                out.setdefault(a.aid, a)
    return out


def main():
    gcroot, xbroot, dest = sys.argv[1:4]
    gc, xb = load(gcroot), load(xbroot)
    n = 0
    with open(dest, "wb") as f:
        for aid, a in sorted(gc.items()):
            b = xb.get(aid)
            if a.atype in SKIP or b is None or b.atype != a.atype:
                continue
            f.write(struct.pack("<4sIII", a.atype.encode("ascii"), aid, len(a.data), len(b.data)))
            f.write(a.data)
            f.write(b.data)
            n += 1
    print("%d pairs -> %s" % (n, dest))


if __name__ == "__main__":
    main()
