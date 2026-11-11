#!/usr/bin/env python3
"""Multiplies the morph values of the FEMALE templates of a BodyGen templates.ini by a factor.

Used to make the "Silhouette, more visible" variant shipped with BodyMorph VR (Silhouette's own values are
very subtle). Male templates (any BT* slider), the markers (names starting with "Silhouette_"), Silhouette_Unshaped
and the player templates are left alone. Usage: scale_templates.py <in> <out> <factor>
"""
import re
import sys


def fmt(v):
    s = ("%.4f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def scale_token(tok, k):
    m = re.fullmatch(r"(?P<name>[^@]+)@(?P<a>-?[\d.]+)(?::(?P<b>-?[\d.]+))?", tok)
    if not m or m.group("name").startswith("Silhouette_"):
        return tok
    out = "%s@%s" % (m.group("name"), fmt(float(m.group("a")) * k))
    if m.group("b") is not None:
        out += ":" + fmt(float(m.group("b")) * k)
    return out


def main():
    src, dst, k = sys.argv[1], sys.argv[2], float(sys.argv[3])
    data = open(src, "rb").read().decode("utf-8")
    lines = data.split("\r\n")
    changed = 0
    for i, line in enumerate(lines):
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, body = line.split("=", 1)
        if name in ("Silhouette_Unshaped",) or name.startswith("Silhouette_Player"):
            continue
        toks = [t.strip() for t in body.split(",")]
        if any(t.startswith("BT") for t in toks):
            continue  # male
        lines[i] = name + "=" + ", ".join(scale_token(t, k) for t in toks)
        changed += 1
    open(dst, "wb").write("\r\n".join(lines).encode("utf-8"))
    print("templates scaled:", changed)


main()
