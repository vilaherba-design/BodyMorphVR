#!/usr/bin/env python3
# BodyMorph VR. Copyright (C) 2026 Axel. GPL-3.0-or-later: see LICENSE.
#
# Writes data/Scripts/BodyGen.pex (+ data/Source/Scripts/BodyGen.psc): a Papyrus script that contains nothing but native function declarations
# ("Scriptname BodyGen Native Hidden"); BodyMorphVR.dll binds the implementations at start-up (F4SE papyrus interface). The functions and their
# signatures are the interface that Silhouette's scripts call (papyrus-stubs/BodyGen.psc of Silhouette, GPL-3.0).
#
# The .pex layout follows Caprica's writer (MIT, github.com/Orvid/Caprica, pex/PexFile.cpp, PexObject.cpp, PexState.cpp, PexFunction.cpp): Fallout 4 = game 2,
# format 3.9, little endian. A compiler can replace this file any time (the .psc is the source):  Caprica BodyGen.psc  ->  BodyGen.pex.
#
# Usage: make_bodygen_pex.py [out_dir]     (default: ../data)      make_bodygen_pex.py --check file.pex
import struct
import sys
import time
from pathlib import Path

FUNCS = [
    # (name, return type, [(param name, type)...])
    ("SetMorph", "None", [("akActor", "Actor"), ("isFemale", "Bool"), ("morph", "String"), ("akKeyword", "Keyword"), ("value", "Float")]),
    ("GetMorph", "Float", [("akActor", "Actor"), ("isFemale", "Bool"), ("morph", "String"), ("akKeyword", "Keyword")]),
    ("RemoveMorphsByName", "None", [("akActor", "Actor"), ("isFemale", "Bool"), ("morph", "String")]),
    ("RemoveMorphsByKeyword", "None", [("akActor", "Actor"), ("isFemale", "Bool"), ("akKeyword", "Keyword")]),
    ("RemoveAllMorphs", "None", [("akActor", "Actor"), ("isFemale", "Bool")]),
    ("GetKeywords", "Keyword[]", [("akActor", "Actor"), ("isFemale", "Bool"), ("morph", "String")]),
    ("GetMorphs", "String[]", [("akActor", "Actor"), ("isFemale", "Bool")]),
    ("RegenerateMorphs", "None", [("akActor", "Actor"), ("update", "Bool")]),
    ("UpdateMorphs", "None", [("akActor", "Actor")]),
    ("ClearAll", "None", []),
    ("SetSkinOverride", "Bool", [("akActor", "Actor"), ("id", "String")]),
    ("RemoveSkinOverride", "Bool", [("akActor", "Actor")]),
]

PSC = """Scriptname BodyGen Native Hidden
; BodyMorph VR (GPL-3.0-or-later). Native declarations only: BodyMorphVR.dll implements them (F4SE papyrus interface).
; Same functions and signatures as the BodyGen script Silhouette's scripts call, so they run unchanged.

; Sets the value of a morph (a BodySlide slider name, or any marker name) under a keyword; None = the plain key. 0 removes it.
Function SetMorph(Actor akActor, Bool isFemale, String morph, Keyword akKeyword, Float value) Native Global
; The value stored under exactly that keyword (None = the plain key); 0.0 when there is none.
Float Function GetMorph(Actor akActor, Bool isFemale, String morph, Keyword akKeyword) Native Global
Function RemoveMorphsByName(Actor akActor, Bool isFemale, String morph) Native Global
Function RemoveMorphsByKeyword(Actor akActor, Bool isFemale, Keyword akKeyword) Native Global
Function RemoveAllMorphs(Actor akActor, Bool isFemale) Native Global
Keyword[] Function GetKeywords(Actor akActor, Bool isFemale, String morph) Native Global
String[] Function GetMorphs(Actor akActor, Bool isFemale) Native Global
; Clears the actor's morphs and runs BodyGen again; applies them when update is True.
Function RegenerateMorphs(Actor akActor, Bool update) Native Global
; Applies the actor's stored morphs to its body now.
Function UpdateMorphs(Actor akActor) Native Global
Function ClearAll() Native Global
; Skin overrides are not supported by BodyMorph VR: both return False.
Bool Function SetSkinOverride(Actor akActor, String id) Native Global
Bool Function RemoveSkinOverride(Actor akActor) Native Global
"""


class Strings:
    def __init__(self):
        self.items = []
        self.index = {}

    def idx(self, text):
        key = text  # the table is case sensitive as stored; one entry per exact spelling
        if key not in self.index:
            self.index[key] = len(self.items)
            self.items.append(text)
        return self.index[key]


def build(compile_time):
    st = Strings()
    ident = lambda s: struct.pack("<H", st.idx(s))
    u8 = lambda v: struct.pack("<B", v)
    u16 = lambda v: struct.pack("<H", v)
    u32 = lambda v: struct.pack("<I", v)

    empty = st.idx("")
    hidden = st.idx("hidden")
    obj_name = st.idx("BodyGen")

    # ---- object body (after the size field)
    body = bytearray()
    body += ident("")            # parent class
    body += ident("")            # documentation string
    body += u8(0)                # isConst (Fallout 4)
    body += u32(1 << 0)          # user flags: hidden (bit 0 of the user flag table below)
    body += ident("")            # auto state name
    body += u16(0)               # structs (Fallout 4)
    body += u16(0)               # variables
    body += u16(0)               # properties
    body += u16(1)               # states
    body += ident("")            #   state name ("" = the default state)
    body += u16(len(FUNCS))
    for name, ret, params in FUNCS:
        body += ident(name)
        body += ident(ret)
        body += ident("")        # documentation
        body += u32(0)           # user flags
        body += u8(1 | 2)        # global | native
        body += u16(len(params))
        for pname, ptype in params:
            body += ident(pname)
            body += ident(ptype)
        body += u16(0)           # locals
        body += u16(0)           # instructions

    # string table is final only now: header strings first so their indices exist
    src = b"BodyGen.psc"
    user = b"BodyMorphVR"
    comp = b"build"
    head = bytearray()
    head += struct.pack("<I", 0xFA57C0DE)
    head += u8(3) + u8(9)
    head += u16(2)               # Fallout 4
    head += struct.pack("<Q", compile_time)
    for s in (src, user, comp):
        head += u16(len(s)) + s
    head += u16(len(st.items))
    for s in st.items:
        e = s.encode("ascii")
        head += u16(len(e)) + e
    head += u8(0)                # no debug info
    head += u16(1)               # user flag table: hidden = bit 0
    head += u16(hidden) + u8(0)
    head += u16(1)               # objects
    head += u16(obj_name)
    head += u32(len(body))
    head += body
    return bytes(head)


# ---- reader (mirrors Caprica's PexFile::read for game 2): used by --check and by the unit-test-like self check below
class R:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError("truncated at %d" % self.p)
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def text(self): return self.take(self.u16()).decode("ascii")


def parse(data):
    r = R(data)
    if r.u32() != 0xFA57C0DE:
        raise ValueError("bad magic")
    major, minor, game = r.u8(), r.u8(), r.u16()
    if (major, minor, game) != (3, 9, 2):
        raise ValueError("not a Fallout 4 3.9 file: %r" % ((major, minor, game),))
    r.u64()
    for _ in range(3):
        r.text()
    strings = [r.text() for _ in range(r.u16())]
    s = lambda: strings[r.u16()]
    if r.u8() != 0:
        raise ValueError("debug info not expected")
    flags = [(s(), r.u8()) for _ in range(r.u16())]
    out = {"flags": flags, "objects": []}
    for _ in range(r.u16()):
        name = s()
        size = r.u32()
        start = r.p
        parent, doc, const, uflags, auto = s(), s(), r.u8(), r.u32(), s()
        structs, variables, props = r.u16(), r.u16(), r.u16()
        if structs or variables or props:
            raise ValueError("unexpected members")
        states = []
        for _ in range(r.u16()):
            sname = s()
            funcs = []
            for _ in range(r.u16()):
                fname, ret, fdoc, fuf, fl = s(), s(), s(), r.u32(), r.u8()
                params = [(s(), s()) for _ in range(r.u16())]
                locals_, instrs = r.u16(), r.u16()
                if locals_ or instrs:
                    raise ValueError("native function with code")
                funcs.append((fname, ret, params, fl))
            states.append((sname, funcs))
        if r.p - start != size:
            raise ValueError("object size field %d but %d bytes read" % (size, r.p - start))
        out["objects"].append((name, parent, uflags, auto, states))
    if r.p != len(data):
        raise ValueError("%d trailing bytes" % (len(data) - r.p))
    return out


def check(data):
    info = parse(data)
    (name, parent, uflags, auto, states), = info["objects"]
    assert name == "BodyGen" and parent == "" and uflags == 1 and auto == ""
    (sname, funcs), = states
    assert sname == ""
    assert [(f[0], f[1], f[2]) for f in funcs] == [(n, r_, p) for n, r_, p in FUNCS]
    assert all(f[3] == 3 for f in funcs)
    return len(funcs)


def main(argv):
    if len(argv) >= 3 and argv[1] == "--check":
        print("ok:", check(Path(argv[2]).read_bytes()), "native functions")
        return 0
    out = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent / "data"
    (out / "Scripts").mkdir(parents=True, exist_ok=True)
    (out / "Source" / "Scripts").mkdir(parents=True, exist_ok=True)
    data = build(int(time.time()))
    check(data)
    (out / "Scripts" / "BodyGen.pex").write_bytes(data)
    (out / "Source" / "Scripts" / "BodyGen.psc").write_text(PSC.replace("\n", "\r\n"), encoding="ascii", newline="")
    print("wrote", out / "Scripts" / "BodyGen.pex", len(data), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
