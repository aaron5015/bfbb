"""Write src/SB/Core/pc/iGCMemTable.inc: where each scalar of a GameCube global
lives in the PC build.

GameCube mods write bytes at absolute addresses through eEventGivePowerUp,
whose index is not bounds-checked (see iGCMem.h). The table maps those
addresses onto the PC's copy of the same field.

The GameCube layouts come from the decomp's own headers: a probe TU that
includes each symbol's header is compiled with mwcceppc -sym on and its DWARF
dumped with dtk. The addresses come from config/GQPE78/symbols.txt. The PC side
is an address expression per field, which the PC compiler resolves.

Run from anywhere after `ninja` has fetched the compilers. The argument names
another checkout's build/ folder, for a worktree that has none:

    python tools/gcmem/gen.py [build dir]
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "gcswap"))
import dwarf  # noqa: E402

OUT = os.path.join(ROOT, "src", "SB", "Core", "pc", "iGCMemTable.inc")
SYMBOLS_TXT = os.path.join(ROOT, "config", "GQPE78", "symbols.txt")
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "build")
MWCC = os.path.join(BUILD, "compilers", "GC", "2.0p1g", "mwcceppc.exe")
DTK = os.path.join(BUILD, "tools", "dtk.exe")

# The globals the table covers: (symbol, type, header).
GLOBALS = [
    ("globals", "zGlobals", "zGlobals.h"),
]

# Members the PC build declares differently; they stay unmapped.
SKIP = set()

CFLAGS = [
    "-nodefaults", "-proc", "gekko", "-align", "powerpc", "-enum", "int", "-fp", "hardware",
    "-Cpp_exceptions", "off", "-O4,p", "-inline", "off", "-maxerrors", "1", "-nosyspath",
    "-RTTI", "off", "-fp_contract", "on", "-multibyte", "-lang=c++", "-common", "on",
    "-char", "unsigned", "-str", "reuse,pool,readonly", "-use_lmw_stmw", "on",
    "-pragma", "cpp_extensions on", "-gccinc", "-sym", "on",
    "-DBUILD_VERSION=0", "-DVERSION_GQPE78", "-DNDEBUG=1", "-DGAMECUBE",
]
INCLUDES = [
    "include", "src/PowerPC_EABI_Support/include", "src/dolphin/include", "src/dolphin/src",
    "src/bink/include", "src/bink/src", "src", os.path.join(BUILD, "GQPE78", "include"), "include/inline",
    "include/rwsdk", "src/SB/Core/gc", "src/SB/Core/x", "src/SB/Game",
]


def gc_layouts(tmp):
    probe = os.path.join(tmp, "probe.cpp")
    with open(probe, "w") as f:
        for _, _, header in GLOBALS:
            f.write('#include "%s"\n' % header)
        for i, (sym, typ, _) in enumerate(GLOBALS):
            f.write("%s* gcmem_probe%d() { return &%s; }\n" % (typ, i, sym))
    obj = os.path.join(tmp, "probe.o")
    dump = os.path.join(tmp, "probe_dwarf.cpp")
    args = [MWCC] + CFLAGS
    for inc in INCLUDES:
        args += ["-i", inc]
    subprocess.check_call(args + ["-c", probe, "-o", obj], cwd=ROOT)
    subprocess.check_call([DTK, "dwarf", "dump", obj, "-o", dump], cwd=ROOT)
    return dwarf.parse(dump)


def gc_addresses():
    out = {}
    pat = re.compile(r"^(\w+) = \.\w+:0x([0-9A-Fa-f]+); // type:object size:0x([0-9A-Fa-f]+)")
    for line in open(SYMBOLS_TXT):
        m = pat.match(line)
        if m:
            out[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return out


def clean(typ):
    typ = re.sub(r"^/\* @class \*/\s*", "", typ)
    return re.sub(r"^(class|struct|union|enum) ", "", typ)


class Walker:
    def __init__(self, structs):
        self.structs = structs
        self.fields = []    # (gc offset, width, count, pc expression)
        self.dropped = []   # (gc offset, size, path, why)

    def struct(self, name, off, path):
        s = self.structs[name]
        if s.base:
            self.struct(s.base, off, path)
        members = s.members
        if s.kind == "union":
            members = members[:1]
        for m in members:
            self.member(m, off, path)

    def member(self, m, off, path):
        if m[0] == "anon":
            _, inner, aoff, size, name, dims = m
            if dims:
                self.dropped.append((off + aoff, size, path + "." + name, "array of anonymous"))
                return
            if name:
                path += "." + name
            for sub in inner:
                self.member(sub, off + aoff, path)
            return
        if m[0] == "union":
            # The first member stands for the union, as an initializer would.
            if m[1]:
                self.member(m[1][0], off, path)
            return
        _, typ, ptr, name, dims, bits, moff, size = m
        where = path + "." + name
        moff += off
        if where in SKIP:
            self.dropped.append((moff, size, where, "skipped"))
            return
        if bits:
            self.dropped.append((moff, size, where, "bitfield"))
            return
        if ptr:
            self.dropped.append((moff, size, where, "pointer"))
            return
        dims = [int(d) for d in re.findall(r"\[(\d+)\]", dims)]
        count = 1
        for d in dims:
            count *= d
        first = where + "".join("[0]" for _ in dims)
        typ = clean(typ)
        if typ in dwarf.SCALARS or m[1].startswith("enum "):
            width = dwarf.SCALARS.get(typ, 4)
            if width * count != size:
                self.dropped.append((moff, size, where, "size %d of %d x %d" % (size, count, width)))
                return
            self.fields.append((moff, width, count, first))
            return
        if typ not in self.structs:
            self.dropped.append((moff, size, where, "unknown type " + typ))
            return
        esize = self.structs[typ].size
        if esize == 0 or esize * count != size:
            self.dropped.append((moff, size, where, "size"))
            return
        for k in range(count):
            index = ""
            rest = k
            for d in reversed(dims):
                index = "[%d]" % (rest % d) + index
                rest //= d
            self.struct(typ, moff + k * esize, where + index)


def main():
    addresses = gc_addresses()
    with tempfile.TemporaryDirectory() as tmp:
        structs = gc_layouts(tmp)

    rows = []
    dropped = []
    for sym, typ, _ in GLOBALS:
        base, size = addresses[sym]
        if structs[typ].size != size:
            sys.exit("%s is 0x%X bytes in the probe and 0x%X in symbols.txt" % (sym, structs[typ].size, size))
        w = Walker(structs)
        w.struct(typ, 0, sym)
        rows += [(base + o, wd, n, e) for o, wd, n, e in w.fields]
        dropped += [(base + o, sz, p, why) for o, sz, p, why in w.dropped]
    rows.sort()

    powerup = addresses["globals"][0]
    w = Walker(structs)
    w.struct("zGlobals", 0, "globals")
    powerup += next(o for o, _, _, e in w.fields if e == "globals.player.g.PowerUp[0]")

    lines = [
        "// Generated by tools/gcmem/gen.py from the decomp's headers and",
        "// config/GQPE78/symbols.txt. Do not edit.",
        "",
        "// &globals.player.g.PowerUp[0] on the GameCube.",
        "#define IGCMEM_POWERUP 0x%08Xu" % powerup,
        "",
        "static const iGCMemField sGCMemFields[] = {",
    ]
    for addr, width, count, expr in rows:
        lines.append("    { 0x%08Xu, %d, %d, &%s }," % (addr, width, count, expr))
    lines.append("};")
    lines.append("")
    lines.append("// Not mapped:")
    for addr, size, path, why in sorted(dropped):
        lines.append("//   0x%08X +0x%X %s (%s)" % (addr, size, path, why))
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%d fields, %d unmapped -> %s" % (len(rows), len(dropped), os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
