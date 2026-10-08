#!/usr/bin/env python3
"""Writes the largest icon of a Windows executable as a single-image .ico file.

Usage: peicon.py <game.exe> <out.ico>      exit status 1 when the exe has no icon

tool/neutron turns it into the icon of the game's app bundle (sips -> .icns), so the
Dock, Cmd+Tab and window switchers show the game's own icon.
"""
import struct
import sys

RT_ICON, RT_GROUP_ICON = 3, 14


def main(exe, out):
    data = open(exe, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return 1
    nsect, optsize = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    datadir = opt + (112 if magic == 0x20b else 96)
    res_rva = struct.unpack_from('<I', data, datadir + 2 * 8)[0]
    if not res_rva:
        return 1
    sections = []
    for i in range(nsect):
        s = opt + optsize + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, s + 8)
        sections.append((va, max(vsize, rawsize), raw))

    def off(rva):
        for va, size, raw in sections:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError('rva outside sections')

    root = off(res_rva)

    def entries(d):
        named, ids = struct.unpack_from('<HH', data, d + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from('<II', data, d + 16 + 8 * i)
            yield name, target

    def leaf(target):
        # descend to the first language entry of a name/id directory
        while target & 0x80000000:
            target = next(entries(root + (target & 0x7fffffff)))[1]
        rva, size = struct.unpack_from('<II', data, root + target)
        return data[off(rva):off(rva) + size]

    types = {name: target for name, target in entries(root) if not name & 0x80000000}
    if RT_GROUP_ICON not in types or RT_ICON not in types:
        return 1
    icons = {name: target for name, target in entries(root + (types[RT_ICON] & 0x7fffffff))}
    group = leaf(next(entries(root + (types[RT_GROUP_ICON] & 0x7fffffff)))[1])

    best = None
    count = struct.unpack_from('<H', group, 4)[0]
    for i in range(count):
        w, h, colors, _, planes, bits, size, icon_id = struct.unpack_from('<BBBBHHIH', group, 6 + 14 * i)
        if icon_id not in icons:
            continue
        key = ((w or 256) * (h or 256), bits)
        if best is None or key > best[0]:
            best = (key, struct.pack('<BBBBHHI', w, h, colors, 0, planes, bits, size), leaf(icons[icon_id]))
    if not best:
        return 1
    _, entry, image = best
    with open(out, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, 1) + entry[:12] + struct.pack('<I', 22) + image)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1], sys.argv[2]))
    except (OSError, ValueError, struct.error, StopIteration):
        sys.exit(1)
