#!/usr/bin/env python3
"""Read and write Kingdom Under Fire "RLE Encode File" (.URE) images, and convert them to PNG.

File layout (all little-endian):
  32 bytes   title "RLE Encode File"
  int32      version (2)
  int32      extra value (version >= 2 only)
  int32      frame count
  frames     frame headers (0x6B bytes each when the frame has one part)
  int32      size of block 1 in bytes
  block 1    per frame: int32 word count n, int32 part offset table, n*2 bytes of data (RGB555)
  block 2    the same frames again in RGB565 (the game reads one block, depending on the display)

Frame data, version 2 files, row by row (height rows):
  color section: length byte, then runs. Run byte b: b & 0x80 -> the next 16-bit color repeated
                 (b & 0x7F) times; otherwise b literal 16-bit colors follow.
  alpha section: length byte, then runs over 8-bit alpha values with the same scheme.

Frame data, version 1 files (the format the game's blitters draw), row by row:
  word: row length in words (from the start of this word); then 16-bit ops until the row is full
  or 0xFFFF. Op low byte = alpha a (0 = transparent, 0xF8+ = opaque), high byte c, n = c & 0x7F:
  a == 0 -> skip n pixels (one filler word follows); c & 0x80 -> the next color repeated n times;
  otherwise n literal colors follow.

    py -3 ure.py topng FILE.URE OUTDIR       write OUTDIR/frame_N.png for every frame
"""
import os
import struct
import sys
import zlib

TITLE = b"RLE Encode File"
FRAME_HEADER = 0x6B  # one-part frame header size


def _runs(data, pos, length, item_size, total):
    """Decode one RLE section; returns (values, new_pos)."""
    end = pos + length
    out = []
    while pos < end:
        b = data[pos]
        pos += 1
        n = b & 0x7F
        if b & 0x80:
            v = data[pos:pos + item_size]
            pos += item_size
            out.extend([v] * n)
        else:
            for _ in range(n):
                out.append(data[pos:pos + item_size])
                pos += item_size
    if pos != end or len(out) != total:
        raise ValueError("bad run section: pos %d end %d values %d expected %d" % (pos, end, len(out), total))
    return out, pos


def to_rgb(v, rgb565):
    if rgb565:
        return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
    return ((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31


def decode_frame(data, width, height, rgb565):
    """Version 2 frame data. Returns rows of (r, g, b, a) tuples."""
    rows, pos = [], 0
    for _ in range(height):
        colors, pos = _runs(data, pos + 1, data[pos], 2, width)
        alphas, pos = _runs(data, pos + 1, data[pos], 1, width)
        rows.append([to_rgb(struct.unpack("<H", c)[0], rgb565) + (a[0],) for c, a in zip(colors, alphas)])
    return rows, pos


def decode_frame_v1(data, width, height, rgb565):
    """Version 1 frame data. Returns rows of (r, g, b, a) tuples."""
    rows, start = [], 0
    for _ in range(height):
        length = struct.unpack_from("<H", data, start)[0]
        pos, row = start + 2, []
        while len(row) < width:
            op = struct.unpack_from("<H", data, pos)[0]
            pos += 2
            if op == 0xFFFF:
                break
            a, c = op & 0xFF, op >> 8
            n = c & 0x7F
            if a == 0:
                row.extend([(0, 0, 0, 0)] * n)
                pos += 2
            elif c & 0x80:
                v = struct.unpack_from("<H", data, pos)[0]
                pos += 2
                row.extend([to_rgb(v, rgb565) + (a,)] * n)
            else:
                for _ in range(n):
                    row.append(to_rgb(struct.unpack_from("<H", data, pos)[0], rgb565) + (a,))
                    pos += 2
        row = (row + [(0, 0, 0, 0)] * width)[:width]
        rows.append(row)
        start += length * 2
    return rows, start


class URE:
    def __init__(self, path):
        d = open(path, "rb").read()
        if not d.startswith(TITLE):
            raise ValueError("not a URE file")
        o = 0x20
        self.version = struct.unpack_from("<i", d, o)[0]
        o += 4
        self.extra = None
        if self.version >= 2:
            self.extra = struct.unpack_from("<i", d, o)[0]
            o += 4
        count = struct.unpack_from("<i", d, o)[0]
        o += 4
        self.headers = []
        for _ in range(count):
            parts = struct.unpack_from("<i", d, o + 4)[0]
            if parts != 1:
                raise ValueError("frames with %d parts are not supported yet" % parts)
            self.headers.append(d[o:o + FRAME_HEADER])
            o += FRAME_HEADER
        self.block1_size = struct.unpack_from("<I", d, o)[0]
        o += 4
        self.blocks = []
        for _ in range(2):
            frames = []
            for h in self.headers:
                n = struct.unpack_from("<I", d, o)[0]
                table = struct.unpack_from("<I", d, o + 4)[0]
                frames.append((table, d[o + 8:o + 8 + 2 * n]))
                o += 8 + 2 * n
            self.blocks.append(frames)
        if o != len(d):
            raise ValueError("unexpected file length (%d of %d bytes used)" % (o, len(d)))

    def size(self, i):
        return struct.unpack_from("<HH", self.headers[i], 8)

    def rows(self, i, block=1):
        w, h = self.size(i)
        table, data = self.blocks[block][i]
        decode = decode_frame if self.version >= 2 else decode_frame_v1
        rows, used = decode(data[table * 2:], w, h, rgb565=block == 1)
        return rows


def _encode_runs(values, item_bytes):
    """RLE runs for one row: (0x80|n) + value for repeats, n + n values for literals (n <= 127)."""
    out, i, n = bytearray(), 0, len(values)
    while i < n:
        j = i
        while j < n and j - i < 127 and values[j] == values[i]:
            j += 1
        if j - i >= 2:
            out.append(0x80 | (j - i))
            out += item_bytes(values[i])
            i = j
            continue
        j = i
        while j < n and j - i < 127 and not (j + 1 < n and values[j] == values[j + 1]):
            j += 1
        j = max(j, i + 1)
        out.append(j - i)
        for v in values[i:j]:
            out += item_bytes(v)
        i = j
    return bytes(out)


def pack_color(r, g, b, rgb565):
    if rgb565:
        return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
    return (r >> 3) << 10 | (g >> 3) << 5 | b >> 3


def encode_frame(rows, rgb565):
    """Version 2 frame data from rows of (r, g, b, a)."""
    out = bytearray()
    for row in rows:
        colors = [0 if a < 8 else pack_color(r, g, b, rgb565) for r, g, b, a in row]
        alphas = [(a >> 3) << 3 if a < 248 else 0xF8 for r, g, b, a in row]
        for section in (_encode_runs(colors, lambda v: struct.pack("<H", v)),
                        _encode_runs(alphas, lambda v: bytes([v]))):
            if len(section) > 255:
                raise ValueError("row section too long for one length byte")
            out.append(len(section))
            out += section
    if len(out) % 2:
        out.append(0)
    return bytes(out)


def save(u, path):
    """Write a version 2 URE from u.version/extra/headers/blocks (blocks hold encoded frame data)."""
    out = bytearray(TITLE.ljust(0x20, b"\0"))
    out += struct.pack("<i", u.version)
    if u.version >= 2:
        out += struct.pack("<i", u.extra)
    out += struct.pack("<i", len(u.headers))
    for h in u.headers:
        out += h
    blocks = []
    for frames in u.blocks:
        b = bytearray()
        for table, data in frames:
            b += struct.pack("<II", len(data) // 2, table) + data
        blocks.append(b)
    out += struct.pack("<I", len(blocks[0])) + blocks[0] + blocks[1]
    open(path, "wb").write(out)


def write_png(path, rows):
    h, w = len(rows), len(rows[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def contact_sheet(u, frames, columns=10, scale=2, cell=(56, 48)):
    """All given frames (that fit the cell) side by side, scaled, on a grey background, in order."""
    cw, ch = cell[0] * scale, cell[1] * scale
    rows_n = (len(frames) + columns - 1) // columns
    sheet = [[(60, 60, 60, 255)] * (cw * columns) for _ in range(ch * rows_n)]
    for k, i in enumerate(frames):
        img = u.rows(i)
        ox, oy = (k % columns) * cw, (k // columns) * ch
        for y, row in enumerate(img[:cell[1]]):
            for x, (r, g, b, a) in enumerate(row[:cell[0]]):
                for dy in range(scale):
                    for dx in range(scale):
                        bg = sheet[oy + y * scale + dy][ox + x * scale + dx]
                        sheet[oy + y * scale + dy][ox + x * scale + dx] = tuple(
                            (c * a + bc * (255 - a)) // 255 for c, bc in zip((r, g, b), bg[:3])) + (255,)
    return sheet


if __name__ == "__main__":
    if len(sys.argv) >= 5 and sys.argv[1] == "sheet":
        u = URE(sys.argv[2])
        first, last = int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else len(u.headers) - 1
        frames = [i for i in range(first, last + 1) if u.size(i)[0] <= 56 and u.size(i)[1] <= 48]
        write_png(sys.argv[3], contact_sheet(u, frames))
        print("frames on sheet, row by row (10 per row):", frames)
    elif len(sys.argv) == 4 and sys.argv[1] == "topng":
        u = URE(sys.argv[2])
        os.makedirs(sys.argv[3], exist_ok=True)
        for i in range(len(u.headers)):
            out = os.path.join(sys.argv[3], "frame_%d.png" % i)
            write_png(out, u.rows(i))
            print("frame %d %dx%d -> %s" % ((i,) + u.size(i) + (out,)))
    else:
        raise SystemExit(__doc__)
