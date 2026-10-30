"""Regenerate the swap table, build check.cpp with iGCSwap.cpp, and run it.

    python tools/gcswap/check.py <corpus.bin> [TYPE]

The corpus comes from corpus.py. With TYPE (a prefix, e.g. DYNA or SIMP) only
those rows are printed, with more of their differing offsets.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dwarf
import gen

ROOT = dwarf.ROOT
PC = os.path.join(ROOT, "src", "SB", "Core", "pc")


def main():
    gen.Gen().run()
    exe = os.path.join(HERE, "build", "gcswap_check.exe" if os.name == "nt" else "gcswap_check")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    subprocess.check_call(["clang++", "-std=c++17", "-O2", "-I", PC, "-idirafter", os.path.join(ROOT, "include"),
                           "-DPLATFORM_PC", "-D_CRT_SECURE_NO_WARNINGS", os.path.join(HERE, "check.cpp"),
                           os.path.join(PC, "iGCSwap.cpp"), os.path.join(PC, "iSndDsp.cpp"), "-o", exe])
    subprocess.check_call([exe] + sys.argv[1:])


if __name__ == "__main__":
    main()
