#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""extract-assets.py: fill in the UI art headers that this repository does not ship.

A few of azmod's headers hold small images that were made from the deck's own
firmware (button faces, captions, Beat FX labels, title masks), and one holds the key
the deck's library database is encrypted with. Those bytes are not ours to publish, so this repository ships them as TEMPLATES (assets/*.h.in): the
code, the array names and sizes, and a SHA-256 for every array, but not the bytes.

This script takes an official azmod.so (from an AZ-MODS release, or the one on your
own deck) and copies each array out of it by symbol name, checks it against the
SHA-256 in the template, and writes the finished header into src/azmod/.

  python3 tools/extract-assets.py path/to/official/azmod.so

Exit 0 = every header written and every array verified. Exit 1 = an array is
missing or does not match (the .so is from a different release than this source).
Python 3.6+, no extra packages.
"""
import hashlib, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEMPLATES = os.path.join(ROOT, "assets")
OUT = os.path.join(ROOT, "src", "azmod")
SLOT = re.compile(r"/\*@(asset|string) (\w+) (\d+) ([0-9a-f]{64}) (present|absent)@\*/")


def elf_symbols(path):
    """{name: [(file_offset, size), ...]} for every sized object symbol in .symtab."""
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
        sys.exit("%s: not a 64-bit little-endian ELF file" % path)
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", b, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in secs:
        if s[1] != 2:          # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for k in range(s[5] // 24):
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", b, s[4] + k * 24)
            if size == 0 or shndx == 0 or shndx >= len(secs) or (info & 0xF) != 1:   # STT_OBJECT
                continue
            sec = secs[shndx]
            if sec[1] == 8:    # SHT_NOBITS
                continue
            o = strtab[4] + name
            nm = b[o:b.index(b"\0", o)].decode()
            off = sec[4] + (value - sec[3])
            syms.setdefault(nm, []).append((off, size))
    return b, syms


def fmt(data):
    rows = []
    for i in range(0, len(data), 16):
        rows.append("  " + ",".join("0x%02x" % x for x in data[i:i + 16]) + ",")
    return "\n" + "\n".join(rows) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    blob, syms = elf_symbols(sys.argv[1])
    bad = 0
    for t in sorted(os.listdir(TEMPLATES)):
        if not t.endswith(".h.in"):
            continue
        text = open(os.path.join(TEMPLATES, t)).read()

        def fill(m):
            kind, name, n, want, where = m.group(1), m.group(2), int(m.group(3)), m.group(4), m.group(5)
            if kind == "string":
                # a C string literal: the symbol holds it with its NUL, the slot without
                for off, size in syms.get(name, []):
                    data = blob[off:off + n]
                    if size >= n and hashlib.sha256(data).hexdigest() == want:
                        return '"%s"' % data.decode("ascii")
                raise LookupError(name)
            if where == "absent":
                # the official build drops this array (unused), so its bytes cannot
                # reach the .so: zeros of the right length build the same binary
                return fmt(bytes(n))
            for off, size in syms.get(name, []):
                data = blob[off:off + size]
                if size == n and hashlib.sha256(data).hexdigest() == want:
                    return fmt(data)
            raise LookupError(name)

        try:
            out = SLOT.sub(fill, text)
        except LookupError as e:
            print("MISSING or DIFFERENT: %s (%s). Use the azmod.so from the release that matches this source." % (e, t))
            bad = 1
            continue
        dst = os.path.join(OUT, t[:-3])
        open(dst, "w").write(out)
        print("wrote %s" % os.path.relpath(dst, ROOT))
    sys.exit(bad)


if __name__ == "__main__":
    main()
