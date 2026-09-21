"""Struct layouts out of dwarf/dwarf_data.cpp, flattened to the scalars a byte
swap has to touch.

The dump is the PS2 build's debug info printed as C++: one class per block,
every member with its offset and size. The asset structs are the same on every
platform, so their layouts are the GameCube's too.

layout(name) returns (size, leaves): each leaf is (offset, width, what), width
1, 2, 4 or 8, `what` the member path ("xTimerAsset.randomRange"). A union whose
members disagree on where the scalars are comes back as one leaf named
"...:union"; one whose members agree is flattened like a struct. A bitfield
comes back named "...:bits".
"""
import os
import re

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
DUMP = os.path.join(ROOT, "dwarf", "dwarf_data.cpp")

SCALARS = {
    "char": 1, "signed char": 1, "unsigned char": 1, "bool": 1,
    "short": 2, "signed short": 2, "unsigned short": 2, "wchar_t": 2,
    "int": 4, "signed int": 4, "unsigned int": 4, "long": 4, "signed long": 4,
    "unsigned long": 4, "float": 4,
    "long long": 8, "signed long long": 8, "unsigned long long": 8, "double": 8,
}

HEAD = re.compile(r"^(class|struct|union) (\w+)(?: : public (\w+))? \{")
SIZE = re.compile(r"// total size: 0x([0-9A-Fa-f]+)")
MEMBER = re.compile(
    r"^\s+(?P<type>.+?)\s*(?P<ptr>\*+)?\s*(?P<name>\w+)(?P<dims>(\[\d+\])*)"
    r"(?:\s*:\s*(?P<bits>\d+))?; // offset 0x(?P<off>[0-9A-Fa-f]+), size 0x(?P<size>[0-9A-Fa-f]+)")
ANON_END = re.compile(
    r"^\s*\}\s*(?P<name>\w+)?(?P<dims>(\[\d+\])*); // offset 0x(?P<off>[0-9A-Fa-f]+), "
    r"size 0x(?P<size>[0-9A-Fa-f]+)")


class Struct:
    def __init__(self, name, base, size, kind, members):
        self.name = name
        self.base = base
        self.size = size
        self.kind = kind
        self.members = members


def parse_block(lines, i):
    """Members up to the matching close brace: (members, index of the close,
    the closing line)."""
    members = []
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("}"):
            return members, i, line
        if re.search(r"\{\s*(//.*)?$", stripped):
            inner, i, close = parse_block(lines, i + 1)
            if stripped.startswith("union"):
                # An anonymous union's members carry offsets in the parent.
                members.append(("union", inner))
            else:
                # An anonymous class's members are relative to it, and its
                # closing line says where it sits.
                m = ANON_END.match(close)
                if m:
                    members.append(("anon", inner, int(m.group("off"), 16),
                                    int(m.group("size"), 16)))
            i += 1
            continue
        mm = MEMBER.match(line)
        if mm:
            members.append(("member", mm.group("type").strip(), bool(mm.group("ptr")),
                            mm.group("name"), mm.group("dims"), mm.group("bits"),
                            int(mm.group("off"), 16), int(mm.group("size"), 16)))
        i += 1
    return members, i, ""


def parse(path=DUMP):
    structs = {}
    lines = open(path, encoding="latin-1").read().splitlines()
    i = 0
    while i < len(lines):
        m = HEAD.match(lines[i])
        if not m:
            i += 1
            continue
        kind, name, base = m.groups()
        size = 0
        if i + 1 < len(lines):
            sm = SIZE.search(lines[i + 1])
            if sm:
                size = int(sm.group(1), 16)
        members, i, _ = parse_block(lines, i + 1)
        # Later compile units repeat the same types. Keep the first complete
        # one: a forward reference prints as an empty block of size 0.
        old = structs.get(name)
        if old is None or (old.size == 0 and size != 0):
            structs[name] = Struct(name, base, size, kind, members)
        i += 1
    return structs


def extent(m):
    if m[0] == "member":
        return m[6], m[6] + m[7]
    if m[0] == "anon":
        return m[2], m[2] + m[3]
    spans = [extent(sub) for sub in m[1]]
    return min(a for a, b in spans), max(b for a, b in spans)


class Layouts:
    def __init__(self, path=DUMP):
        self.structs = parse(path)
        self.cache = {}

    def size(self, name):
        return self.structs[name].size

    def layout(self, name):
        if name not in self.cache:
            s = self.structs[name]
            leaves = []
            if s.base:
                leaves += self.layout(s.base)[1]
            for m in s.members:
                leaves += self.member(m, 0, name)
            if s.kind == "union":
                leaves = self.merge_union(leaves, 0, s.size, name)
            unique = {}
            for off, width, what in leaves:
                unique.setdefault((off, width), what)
            self.cache[name] = (s.size, sorted(((o, w, n) for (o, w), n in unique.items()),
                                                key=lambda l: (l[0], -l[1])))
        return self.cache[name]

    def field(self, name, member):
        """(offset, size) of a named member, searching the base classes."""
        s = self.structs[name]
        for m in s.members:
            if m[0] == "member" and m[3] == member:
                return m[6], m[7]
        if s.base:
            return self.field(s.base, member)
        raise KeyError("%s has no member %s" % (name, member))

    def member(self, m, base, path):
        if m[0] == "anon":
            _, inner, off, size = m
            leaves = []
            for sub in inner:
                leaves += self.member(sub, base + off, path)
            return leaves
        if m[0] == "union":
            leaves = []
            for sub in m[1]:
                leaves += self.member(sub, base, path)
            start, end = extent(m)
            return self.merge_union(leaves, base + start, end - start, path)
        _, typ, ptr, name, dims, bits, off, size = m
        off += base
        what = path + "." + name
        if bits:
            return [(off, size, what + ":bits")]
        if ptr or typ.startswith("enum "):
            return [(off + k * 4, 4, what) for k in range(size // 4)]
        typ = re.sub(r"^(class|struct|union) ", "", typ)
        typ = re.sub(r"^/\* @class \*/\s*", "", typ)
        if typ in SCALARS:
            # Bytes come back too, width 1: nothing swaps them, but a union
            # whose members put a byte where another puts a float has to be
            # told apart from one whose members all agree.
            width = SCALARS[typ]
            return [(off + k * width, width, what) for k in range(size // width)]
        if typ in self.structs:
            esize, eleaves = self.layout(typ)
            if esize == 0:
                return []
            out = []
            for k in range(size // esize):
                out += [(off + k * esize + lo, lw, what + lwhat[len(typ):])
                        for lo, lw, lwhat in eleaves]
            return out
        raise KeyError("unknown type %r in %s" % (typ, what))

    @staticmethod
    def merge_union(leaves, start, size, path):
        # The members agree if no two scalars overlap with different extents.
        spans = {}
        for off, width, what in leaves:
            for b in range(off, off + width):
                if spans.setdefault(b, (off, width)) != (off, width):
                    return [(start, size, path + ":union")]
        return leaves
