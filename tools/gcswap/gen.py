"""Generate src/SB/Core/pc/iGCSwapTable.inc from spec.py and the DWARF layouts.

    python tools/gcswap/gen.py

The output is a swap function per struct -- one gcsSwap16/gcsSwap32 per scalar
at its DWARF offset -- and one per asset type that walks the type's steps, and
iGCSwapGenerated, which dispatches on the packer's type code. iGCSwap.cpp
includes it.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import dwarf
import spec

OUT = os.path.join(dwarf.ROOT, "src", "SB", "Core", "pc", "iGCSwapTable.inc")
DYNA_BASE = 0x10   # sizeof(xDynAsset)
SWAP = {2: "gcsSwap16", 4: "gcsSwap32", 8: "gcsSwap64"}


def xstrhash(s):
    h = 0
    for ch in s.encode("ascii"):
        h = (((ch - (ch & (ch >> 1) & 0x20)) & 0xFF) + h * 0x83) & 0xFFFFFFFF
    return h


def fourcc(t):
    return "0x%08X" % int.from_bytes(t.encode("ascii"), "big")


def ident(t):
    return t.strip().replace(" ", "_")


class Gen:
    def __init__(self):
        self.L = dwarf.Layouts()
        self.structs = {}   # (name, skip_below) -> emitted C++
        self.fields = {}    # raw field lists -> emitted C++

    def swap_lines(self, leaves, skip_below=0, indent="    ", base="p"):
        out = []
        for off, width, what in leaves:
            if off < skip_below or width == 1:
                continue
            if what in spec.EITHER:
                kind = spec.EITHER[what]
                out.append("%sgcsSwap32Either%s(%s + 0x%X); // %s: see spec.EITHER" % (
                    indent, kind.capitalize(), base, off, what))
                continue
            if what.endswith(":bits"):
                raise SystemExit("%s is a bitfield and needs a rule" % what)
            out.append("%s%s(%s + 0x%X);" % (indent, SWAP[width], base, off))
        return out

    def struct_fn(self, name, skip_below=0, bounded=False):
        """The name of the C++ function that swaps struct `name` at p. The
        bounded form takes the bytes present, n, and swaps only the fields
        that fit: the tail of an asset older and shorter than its struct."""
        key = (name, skip_below, bounded)
        fn = "gcs_%s%s%s" % (name, "_from%X" % skip_below if skip_below else "",
                             "_upto" if bounded else "")
        if key in self.structs:
            return fn
        self.structs[key] = None    # reserve, for recursion
        size, leaves = self.L.layout(name)
        body = []
        for leaf in leaves:
            if leaf[2].endswith(":union"):
                lines = self.union_lines(name, leaf)
                if bounded:
                    lines = ["    if (n >= 0x%X)" % (leaf[0] + leaf[1]), "    {"] + \
                            ["    " + l for l in lines] + ["    }"]
                body += lines
            else:
                lines = self.swap_lines([leaf], skip_below)
                if bounded and lines:
                    lines = ["    if (n >= 0x%X)" % (leaf[0] + leaf[1]), "    " + lines[0]]
                body += lines
        sig = "U8* p, U32 n" if bounded else "U8* p"
        unused = "    (void)p;\n" if not body else ""
        if bounded and not any("n >=" in l for l in body):
            unused += "    (void)n;\n"
        self.structs[key] = "static void %s(%s) // %s, 0x%X bytes\n{\n%s%s%s}\n" % (
            fn, sig, name, size, "\n".join(body), "\n" if body else "", unused)
        return fn

    def union_lines(self, name, leaf):
        off, size, what = leaf
        rule = spec.UNIONS.get(name)
        if rule is None or rule["at"] != off:
            raise SystemExit("%s: union at 0x%X (%s) needs a rule in spec.UNIONS" % (name, off, what))
        sel_off, sel_width = self.L.field(name, rule["selector"])
        read = {1: "gcsRead8", 2: "gcsRead16", 4: "gcsRead32"}[sel_width]
        out = ["    switch (%s(p + 0x%X)) // %s.%s" % (read, sel_off, name, rule["selector"]), "    {"]
        for value, member in sorted(rule["cases"].items()):
            out.append("    case %d:" % value)
            if member == "u32":
                out.append("        for (U32 w = 0; w < 0x%X; w += 4)" % size)
                out.append("            gcsSwap32(p + 0x%X + w);" % off)
            elif member is not None:
                out.append("        %s(p + 0x%X);" % (self.struct_fn(member), off))
            out.append("        break;")
        out.append("    }")
        return out

    def fields_fn(self, name, size, leaves, skip_below=0):
        fn = "gcs_fields_%s" % name
        if fn not in self.fields:
            body = self.swap_lines([(o, w, "%s+%X" % (name, o)) for o, w in leaves], skip_below)
            self.fields[fn] = "static void %s(U8* p) // 0x%X bytes, from spec.py\n{\n%s\n}\n" % (
                fn, size, "\n".join(body))
        return fn

    def steps(self, steps, lines, fixed, skip_below=0):
        """Emit C++ for a step list. `fixed` maps struct name -> offset for
        structs placed at a known offset, for count fields."""
        at = 0      # where the next struct goes, while that is still known
        known = True
        for step in steps:
            if isinstance(step, str) or step[0] == "fields":
                if isinstance(step, str):
                    size = self.L.size(step)
                    fn = self.struct_fn(step, skip_below)
                    name = step
                else:
                    _, name, size, leaves = step
                    fn = self.fields_fn(name, size, leaves, skip_below)
                if isinstance(step, str):
                    lines.append("    if (c + 0x%X > end) // an older, shorter asset: what is there" % size)
                    lines.append("    {")
                    lines.append("        if (c < end) %s(p + c, end - c);" % self.struct_fn(step, skip_below, True))
                    lines.append("        return 1;")
                    lines.append("    }")
                else:
                    lines.append("    if (c + 0x%X > end) return 0;" % size)
                lines.append("    %s(p + c);" % fn)
                lines.append("    c += 0x%X;" % size)
                if known:
                    fixed[name] = at
                    at += size
                skip_below = 0
            elif step[0] == "array":
                _, elem, count = step
                esize = {"u16": 2, "u32": 4}.get(elem) or self.L.size(elem)
                if count == "rest":
                    lines.append("    for (; c + 0x%X <= end; c += 0x%X)" % (esize, esize))
                    lines.append("        %s;" % self.elem_call(elem))
                else:
                    if isinstance(count, int):
                        cexpr = str(count)
                    else:
                        sname, mname = count.split(".")
                        moff, msize = self.L.field(sname, mname)
                        if sname not in fixed:
                            raise SystemExit("%s is not at a fixed offset" % sname)
                        cexpr = "gcsRead%d(p + 0x%X)" % (msize * 8, fixed[sname] + moff)
                    # A count past the end still gets the elements that are
                    # there: mods ship move points whose numPoints is one
                    # more than their ids, and the game reads the rest from
                    # the next asset.
                    lines.append("    {")
                    lines.append("        U32 n = %s;" % cexpr)
                    lines.append("        U32 fit = (end - c) / 0x%X;" % esize)
                    lines.append("        for (U32 k = 0; k < n && k < fit; k++, c += 0x%X)" % esize)
                    lines.append("            %s;" % self.elem_call(elem))
                    lines.append("        if (n > fit) return 0;")
                    lines.append("    }")
                known = False
            elif step[0] == "tagged":
                # A list whose entries each start with a u32 type, big-endian
                # until it is swapped, that picks the entry's struct -- or,
                # for a number, an xParCmdAsset-style 8-byte header of a u32
                # and four bytes, then words to that size.
                _, count, cases = step
                if isinstance(cases, str):
                    cases = getattr(spec, cases)
                sname, mname = count.split(".")
                moff, msize = self.L.field(sname, mname)
                if sname not in fixed:
                    raise SystemExit("%s is not at a fixed offset" % sname)
                lines.append("    {")
                lines.append("        U32 n = gcsRead%d(p + 0x%X);" % (msize * 8, fixed[sname] + moff))
                lines.append("        for (U32 k = 0; k < n; k++)")
                lines.append("        {")
                lines.append("            if (c + 4 > end) return 0;")
                lines.append("            switch (gcsReadBE32(p + c))")
                lines.append("            {")
                for tag, case in sorted(cases.items()):
                    if isinstance(case, int):
                        lines.append("            case %d: // header and words" % tag)
                        lines.append("                if (c + 0x%X > end) return 0;" % case)
                        lines.append("                gcsSwap32(p + c);")
                        lines.append("                for (U32 w = 8; w < 0x%X; w += 4)" % case)
                        lines.append("                    gcsSwap32(p + c + w);")
                        lines.append("                c += 0x%X;" % case)
                    else:
                        size = self.L.size(case)
                        lines.append("            case %d: // %s" % (tag, case))
                        lines.append("                if (c + 0x%X > end) return 0;" % size)
                        lines.append("                %s(p + c);" % self.struct_fn(case))
                        lines.append("                c += 0x%X;" % size)
                    lines.append("                break;")
                lines.append("            default:")
                lines.append("                return 0;")
                lines.append("            }")
                lines.append("        }")
                lines.append("    }")
                known = False
            elif step[0] == "words":
                if skip_below:
                    lines.append("    if (c < 0x%X) c = 0x%X;" % (skip_below, skip_below))
                lines.append("    for (; c + 4 <= end; c += 4)")
                lines.append("        gcsSwap32(p + c);")
                known = False
            elif step[0] == "bytes":
                lines.append("    c = end; // bytes, nothing to swap")
                known = False
            elif step[0] == "custom":
                lines.append("    return %s(p, size);" % step[1])
                return
            else:
                raise SystemExit("unknown step %r" % (step,))
        lines.append("    return 1;")

    def elem_call(self, elem):
        if elem == "u16":
            return "gcsSwap16(p + c)"
        if elem == "u32":
            return "gcsSwap32(p + c)"
        return "%s(p + c)" % self.struct_fn(elem)

    def type_fn(self, t, steps):
        lines = ["static S32 gcs_type_%s(U8* p, U32 size)" % ident(t), "{"]
        if len(steps) == 1 and not isinstance(steps[0], str) and steps[0][0] == "custom":
            lines.append("    return %s(p, size);" % steps[0][1])
            lines.append("}")
            return "\n".join(lines) + "\n"
        if t in spec.NOBASE:
            lines.append("    U32 end = size;")
        else:
            lines.append("    U32 end;")
            lines.append("    if (!gcsSwapLinks(p, size, &end)) return 0;")
        lines.append("    U32 c = 0;")
        self.steps(steps, lines, {})
        lines.append("}")
        return "\n".join(lines) + "\n"

    def dyna_fn(self):
        lines = ["static S32 gcs_type_DYNA(U8* p, U32 size)", "{",
                 "    U32 end;",
                 "    if (!gcsSwapLinks(p, size, &end)) return 0;",
                 "    if (end < 0x%X) return 0;" % DYNA_BASE,
                 "    %s(p);" % self.struct_fn("xDynAsset"),
                 "    U32 c = 0;",
                 "    switch (gcsRead32(p + 0x8))",
                 "    {"]
        for name, steps in spec.DYNA.items():
            h = name if isinstance(name, int) else xstrhash(name)
            label = spec.DYNA_NAMES.get(h, "?") if isinstance(name, int) else name
            lines.append("    case 0x%08XU: // %s" % (h, label))
            lines.append("    {")
            body = []
            self.steps(steps, body, {}, DYNA_BASE)
            lines += ["    " + l for l in body]
            lines.append("    }")
        lines.append("    }")
        lines.append("    return 0;")
        lines.append("}")
        return "\n".join(lines) + "\n"

    def run(self):
        self.struct_fn("xLinkAsset")   # gcsSwapLinks, for every base type
        for name in spec.EXTRA_STRUCTS:
            self.struct_fn(name)
        types = []
        for t, steps in spec.TYPES.items():
            types.append((t, self.type_fn(t, steps)))
        types.append(("DYNA", self.dyna_fn()))

        out = ["// Generated by tools/gcswap/gen.py from tools/gcswap/spec.py and",
               "// dwarf/dwarf_data.cpp. Do not edit; change the spec and regenerate.",
               ""]
        emitted = sorted(self.structs.items(), key=lambda kv: kv[0])
        # Declared first: one struct's function can call another's.
        out += [text.splitlines()[0].split(" //")[0] + ";" for _, text in emitted]
        out.append("")
        out += [text for _, text in emitted]
        out += [self.fields[k] for k in sorted(self.fields)]
        out += [fn for _, fn in types]
        out.append("static S32 iGCSwapGenerated(U32 type, U8* p, U32 size)")
        out.append("{")
        out.append("    switch (type)")
        out.append("    {")
        for t, _ in sorted(types):
            out.append("    case %s: return gcs_type_%s(p, size); // '%s'" % (fourcc(t), ident(t), t))
        out.append("    }")
        out.append("    return -1;")
        out.append("}")
        out.append("")
        out.append("static bool iGCSwapGeneratedKnows(U32 type)")
        out.append("{")
        out.append("    switch (type)")
        out.append("    {")
        for t, _ in sorted(types):
            out.append("    case %s:" % fourcc(t))
        out.append("        return true;")
        out.append("    }")
        out.append("    return false;")
        out.append("}")
        text = "\n".join(out) + "\n"
        with open(OUT, "w", newline="\n") as f:
            f.write(text)
        print("%s: %d structs, %d types" % (os.path.relpath(OUT, dwarf.ROOT), len(self.structs), len(types)))


if __name__ == "__main__":
    Gen().run()
