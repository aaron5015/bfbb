"""Decode every GameCube sound and compare it with its Xbox twin.

    python tools/gcswap/sndcheck.py <gamecube files/> <xbox assets/>

Pairs packages by their path under each root. For each GameCube package with
an SNDI, every sound it names is read from that level's HIP or HOP on both
discs. sndcheck.cpp swaps the table with iGCSwap.cpp, decodes the GameCube
sound with iSndDsp.cpp, and compares rate, length, loop flag and waveform.
Nothing is written to disk but the checker itself.
"""
import glob
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dwarf
import hipfile

ROOT = dwarf.ROOT
PC = os.path.join(ROOT, "src", "SB", "Core", "pc")


def fourcc(s):
    return struct.pack("<I", int.from_bytes(s.encode("ascii"), "big"))


def blob(b):
    return struct.pack("<I", len(b)) + b


def packages(root):
    out = {}
    for path in glob.glob(os.path.join(root, "**", "*"), recursive=True):
        if path.upper().endswith((".HIP", ".HOP")) and os.path.isfile(path):
            out[os.path.relpath(path, root).replace("\\", "/").lower()] = path
    return out


def level_assets(pkgs, rel, pkg):
    """Every asset of `pkg`, at `rel`, and of the HIP or HOP beside it, by id."""
    out = {a.aid: a for a in pkg.assets}
    other = rel[:-4] + (".hop" if rel.endswith(".hip") else ".hip")
    if other in pkgs:
        for a in hipfile.Package.load(pkgs[other]).assets:
            out.setdefault(a.aid, a)
    return out


def main():
    gcroot, xbroot = sys.argv[1:3]
    exe = os.path.join(HERE, "build", "gcswap_sndcheck.exe" if os.name == "nt" else "gcswap_sndcheck")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.check_call(["clang++", "-std=c++17", "-O2", "-I", PC, "-idirafter", os.path.join(ROOT, "include"),
                           "-DPLATFORM_PC", "-D_CRT_SECURE_NO_WARNINGS", "-Wno-multichar",
                           os.path.join(HERE, "sndcheck.cpp"), os.path.join(PC, "iGCSwap.cpp"),
                           os.path.join(PC, "iSndDsp.cpp"), "-o", exe])

    gcp, xbp = packages(gcroot), packages(xbroot)
    proc = subprocess.Popen([exe], stdin=subprocess.PIPE)
    seen = set()
    for rel in sorted(gcp):
        if rel not in xbp:
            continue
        g = hipfile.Package.load(gcp[rel])
        x = hipfile.Package.load(xbp[rel])
        gt, xt = g.by_type("SNDI"), x.by_type("SNDI")
        if not gt:
            continue
        gtab = gt[0].data
        xtab = xt[0].data if xt else b""
        proc.stdin.write(fourcc("TBL ") + blob(rel.encode()) + blob(gtab) + blob(xtab))

        n = sum(struct.unpack(">I", gtab[o:o + 4])[0] for o in (0, 8, 12)) if len(gtab) >= 16 else 0
        ids = [struct.unpack(">I", gtab[16 + i * 0x64 + 0x60:16 + i * 0x64 + 0x64])[0] for i in range(n)]
        ga = level_assets(gcp, rel, g)
        xa = level_assets(xbp, rel, x)
        for aid in ids:
            if aid in seen:
                continue
            seen.add(aid)
            gd = ga[aid].data if aid in ga else b""
            xd = xa[aid].data if aid in xa else b""
            proc.stdin.write(fourcc("SND ") + struct.pack("<I", aid) + blob(gd) + blob(xd))
    proc.stdin.write(fourcc("END "))
    proc.stdin.close()
    sys.exit(proc.wait())


if __name__ == "__main__":
    main()
