#!/usr/bin/env python3
"""Install GC/2.0p2, the BFBB build of the decompiled CodeWarrior.

doldecomp/mweppc rebuilds mwcceppc.exe from its decompiled source. Its 2.0p2
is GC/2.0 with rules that model the compiler retail BFBB was built with; see
"Battle for Bikini Bottom (2.0p2)" in that repository's README. This fetches
the release archive, checks it against the pinned hashes, and writes 2.0p2's
mwcceppc.exe and lmgr326b.dll next to the given output path.

Usage:
  mweppc.py build/compilers/GC/2.0p2/mwcceppc.exe
"""

import argparse
import hashlib
import io
import sys
import urllib.request
import zipfile
from pathlib import Path

TAG = "initial"
URL = f"https://github.com/doldecomp/mweppc/releases/download/{TAG}/mweppc-release.zip"
ARCHIVE_SHA256 = "2dddf8e5a9475c20d23068a69263143ebd8e6f78623dfffd1e404575924a8af3"
MEMBERS = {
    "2.0p2/mwcceppc.exe": "6e065762da584cb3ed282d3000228f3b4987d8fe",
    "2.0p2/lmgr326b.dll": "207f766c11a68d03305408a3500fa0d64b0c2984",
}


def installed(out_dir: Path) -> bool:
    for member, sha1 in MEMBERS.items():
        path = out_dir / Path(member).name
        if not path.exists() or hashlib.sha1(path.read_bytes()).hexdigest() != sha1:
            return False
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("output", type=Path, help="path of mwcceppc.exe to write")
    args = parser.parse_args()
    out_dir = args.output.parent

    # Leave hash-valid files alone so their timestamps, and every object built
    # with them, stay up to date (the rule is restat).
    if installed(out_dir):
        return

    print(f"Downloading {URL}")
    req = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != ARCHIVE_SHA256:
        sys.exit(f"{URL}: SHA-256 mismatch")

    out_dir.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for member, sha1 in MEMBERS.items():
            content = archive.read(member)
            if hashlib.sha1(content).hexdigest() != sha1:
                sys.exit(f"{member}: SHA-1 mismatch")
            (out_dir / Path(member).name).write_bytes(content)


if __name__ == "__main__":
    main()
