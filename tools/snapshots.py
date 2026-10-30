#!/usr/bin/env python3
"""Run the PC port's snapshot tests: tour scripts whose shots are compared
against committed PNGs.

Each case in tests/snapshots/ is a pair:

  <case>.ini   the config.ini the game runs with; unset keys take defaults
  <case>.tour  the tour script (src/SB/Core/pc/iTour.h); each `shot NAME`
               is compared against tests/snapshots/NAME.png

The game runs hidden with a scratch config, an empty save folder and no audio
device. The assets come from BFBB_ASSETS, or from `[assets] path` in the
config.ini beside the executable.

A shot passes when its size matches the reference and its mean per-channel
difference is at most --tolerance (0-255 scale). Failing shots and their diff
images are left in the scratch folder, which is printed.

Usage:
  snapshots.py                     run every case
  snapshots.py <case> [<case>...]  run only these; a case may be a glob, 'reg_*'
  snapshots.py --update            write shots that are new or differ
  snapshots.py --exe build/bfbb.exe
"""
import argparse
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, "tests", "snapshots")


def assets_path(exe):
    env = os.environ.get("BFBB_ASSETS")
    if env:
        return env
    ini = os.path.join(os.path.dirname(exe), "config.ini")
    if not os.path.exists(ini):
        return None
    # The game appends sections, so they repeat; read the first [assets] path.
    section = None
    with open(ini, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.split(";", 1)[0].strip()
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1].strip().lower()
            elif section == "assets" and "=" in line:
                key, value = line.split("=", 1)
                if key.strip().lower() == "path" and value.strip():
                    return value.strip()
    return None


def run_case(case, exe, assets, scratch, timeout):
    work = os.path.join(scratch, case)
    out = os.path.join(work, "out")
    saves = os.path.join(work, "saves")
    os.makedirs(out)
    os.makedirs(saves)

    config = os.path.join(work, "config.ini")
    shutil.copyfile(os.path.join(CASES, case + ".ini"), config)

    env = dict(os.environ)
    env.update({
        "BFBB_CONFIG": config,
        "BFBB_TOUR": os.path.join(CASES, case + ".tour"),
        "BFBB_TOUR_OUT": out,
        "BFBB_SAVE_DIR": saves,
        "BFBB_ASSETS": assets,
        "SDL_AUDIO_DRIVER": "dummy",
    })

    log = os.path.join(work, "log.txt")
    with open(log, "w") as f:
        try:
            r = subprocess.run([exe], cwd=os.path.dirname(exe), env=env, stdout=f,
                               stderr=subprocess.STDOUT, timeout=timeout)
            code = r.returncode
        except subprocess.TimeoutExpired:
            code = "timeout"

    shots = sorted(n[:-4] for n in os.listdir(out) if n.endswith(".png"))
    return code, log, out, shots


def compare(shot, ref):
    a = np.asarray(Image.open(shot).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(ref).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return None, None
    diff = np.abs(a - b)
    return float(diff.mean()), diff.max(axis=2).astype(np.uint8)


def expected_shots(case):
    names = []
    with open(os.path.join(CASES, case + ".tour")) as f:
        for line in f:
            words = line.split("#", 1)[0].split()
            if len(words) >= 2 and words[0] == "shot":
                names.append(words[1])
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cases", nargs="*")
    ap.add_argument("--update", action="store_true",
                    help="write shots that are missing or differ")
    ap.add_argument("--exe", default=os.path.join(ROOT, "bin", "bfbb.exe"))
    ap.add_argument("--tolerance", type=float, default=2.0)
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        sys.exit("no executable at %s; build it or pass --exe" % exe)
    assets = assets_path(exe)
    if not assets:
        sys.exit("no assets: set BFBB_ASSETS or [assets] path in the config.ini "
                 "beside the executable")

    all_cases = sorted(n[:-5] for n in os.listdir(CASES) if n.endswith(".tour"))
    cases = []
    for pattern in args.cases or ["*"]:
        matched = fnmatch.filter(all_cases, pattern)
        if not matched:
            sys.exit("no case %s; have %s" % (pattern, ", ".join(all_cases)))
        cases += [c for c in matched if c not in cases]

    scratch = tempfile.mkdtemp(prefix="bfbb_snapshots_")
    failed = 0
    for case in cases:
        code, log, out, shots = run_case(case, exe, assets, scratch, args.timeout)
        if code != 0:
            print("%s: exit %s, log in %s" % (case, code, log))
            failed += 1
        for name in expected_shots(case):
            if name not in shots:
                print("%s/%s: not written, log in %s" % (case, name, log))
                failed += 1
                continue

            shot = os.path.join(out, name + ".png")
            ref = os.path.join(CASES, name + ".png")
            if os.path.exists(ref):
                mean, diff = compare(shot, ref)
            else:
                mean, diff = None, None

            if mean is not None and mean <= args.tolerance:
                print("%s/%s: ok (%.2f)" % (case, name, mean))
                continue

            what = ("missing" if not os.path.exists(ref) else
                    "size differs" if mean is None else "differs (%.2f)" % mean)
            if args.update:
                Image.open(shot).convert("RGB").save(ref, optimize=True)
                print("%s/%s: %s, updated" % (case, name, what))
                continue

            if diff is not None:
                Image.fromarray(diff).save(os.path.join(out, name + ".diff.png"))
            print("%s/%s: %s, see %s" % (case, name, what, out))
            failed += 1

    if failed == 0:
        shutil.rmtree(scratch, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
