#!/usr/bin/env python3
"""List or extract files from Kingdom Under Fire's .res archives ("EFB KUF Resource File").

Directory entries hold: int name length, name, int folder length, folder, int flag,
int offset, int size. Files are stored uncompressed. Read-only: archives are never changed.

    py -3 res.py list ARCHIVE.res [filter]
    py -3 res.py extract ARCHIVE.res NAME OUTFILE
"""
import re
import struct
import sys

TITLE = b"EFB KUF Resource File"


def entries(data):
    """Yields (folder, name, offset, size) for directory entries found in the archive."""
    for m in re.finditer(rb"[\x01-\x7f]\x00\x00\x00", data):
        o = m.start()
        n = data[o]
        name = data[o + 4:o + 4 + n]
        if not re.fullmatch(rb"[\w .\-]+\.\w{2,4}", name):
            continue
        p = o + 4 + n
        if p + 4 > len(data):
            continue
        dlen = struct.unpack_from("<I", data, p)[0]
        if not 0 < dlen < 128:
            continue
        folder = data[p + 4:p + 4 + dlen]
        if not re.fullmatch(rb"[\w .\\\-]+", folder):
            continue
        q = p + 4 + dlen
        if q + 12 > len(data):
            continue
        flag, offset, size = struct.unpack_from("<III", data, q)
        if flag in (0, 1) and 0 < offset < len(data) and 0 < size <= len(data) - offset:
            yield folder.decode(), name.decode(), offset, size


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("list", "extract"):
        raise SystemExit(__doc__)
    data = open(sys.argv[2], "rb").read()
    if not data.startswith(TITLE):
        raise SystemExit("not a KUF resource archive")
    if sys.argv[1] == "list":
        pattern = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for folder, name, offset, size in entries(data):
            if pattern in (folder + "\\" + name).lower():
                print("%-40s %10d  %s\\%s" % (name, size, folder, name))
        return
    want = sys.argv[3].lower()
    for folder, name, offset, size in entries(data):
        if name.lower() == want:
            open(sys.argv[4], "wb").write(data[offset:offset + size])
            print("extracted %s\\%s (%d bytes) to %s" % (folder, name, size, sys.argv[4]))
            return
    raise SystemExit("no file named %s" % sys.argv[3])


if __name__ == "__main__":
    main()
