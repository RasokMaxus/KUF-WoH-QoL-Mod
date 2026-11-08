#!/usr/bin/env python3
"""Draw the two 52x43 "Build" hammer icons used by kufmod.

Human: forged steel head with a brass band on a smooth wooden handle.
Dark Legion: worked stone head with rope straps and wraps, on a dark handle with a leather grip.
Everything is drawn at 4x and averaged down for smooth edges, with a soft shadow, on a transparent
background (the game draws its grid tile underneath every icon).

    py -3 draw_hammers.py MenuIcon8.URE OUTDIR
writes OUTDIR/hammer_human.png, hammer_devil.png and preview.png (preview uses the game's tile from
your own MenuIcon8.URE, which you can extract from res6.res with tools/res.py).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ure  # noqa: E402

TILE = 13  # the game's empty grid tile in MenuIcon8.URE (only used for previews)
W, H, SS = 52, 43, 4  # icon size, supersampling


def over(dst, src, ox, oy, opacity=1.0, tint=None):
    """Alpha-composite src onto dst at (ox, oy)."""
    for y, row in enumerate(src):
        for x, (r, g, b, a) in enumerate(row):
            ty, tx = oy + y, ox + x
            if not (0 <= ty < len(dst) and 0 <= tx < len(dst[0])) or a == 0:
                continue
            if tint is not None:
                r, g, b = tint
            sa = a * opacity / 255
            dr, dg, db, da = dst[ty][tx]
            da = da / 255
            oa = sa + da * (1 - sa)
            if oa <= 0:
                continue
            dst[ty][tx] = (int((r * sa + dr * da * (1 - sa)) / oa), int((g * sa + dg * da * (1 - sa)) / oa),
                           int((b * sa + db * da * (1 - sa)) / oa), int(oa * 255))


def upscale(img, k):
    return [[px for px in row for _ in range(k)] for row in img for _ in range(k)]


def blank():
    return [[(0, 0, 0, 0)] * (W * SS) for _ in range(H * SS)]


def put(img, x, y, color):
    if 0 <= y < len(img) and 0 <= x < len(img[0]):
        img[y][x] = color


def shape(img, cx, cy, angle, inside, shade):
    """Fill pixels whose local coords (u along the handle, v across) satisfy inside(u, v);
    colour from shade(u, v). Local coords are in icon pixels, rotated by angle about (cx, cy)."""
    ca, sa = math.cos(angle), math.sin(angle)
    for py in range(H * SS):
        for px in range(W * SS):
            x, y = px / SS - cx, py / SS - cy
            u, v = x * ca + y * sa, -x * sa + y * ca
            if inside(u, v):
                put(img, px, py, shade(u, v) + (255,))


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def down(img):
    """Average SSxSS blocks (premultiplied) into a WxH image."""
    out = []
    for y in range(H):
        row = []
        for x in range(W):
            r = g = b = a = 0
            for dy in range(SS):
                for dx in range(SS):
                    pr, pg, pb, pa = img[y * SS + dy][x * SS + dx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            row.append((0, 0, 0, 0) if a == 0 else (r // a, g // a, b // a, a // (SS * SS)))
        out.append(row)
    return out


ANGLE = math.radians(-45)  # handle runs from bottom-left up to the head at top-right
CX, CY = 27.5, 21.0         # icon centre


def human():
    img = blank()
    wood_dark, wood_light = (92, 56, 28), (176, 120, 66)

    def handle_in(u, v):
        return -20 <= u <= 6 and abs(v) <= 2.1

    def handle_shade(u, v):
        t = (v + 2.1) / 4.2  # light on the upper side
        base = mix(wood_light, wood_dark, t)
        grain = 10 * math.sin(u * 1.7 + v * 0.6)
        return tuple(max(0, min(255, int(c + grain))) for c in base)

    shape(img, CX, CY, ANGLE, handle_in, handle_shade)
    # pommel
    shape(img, CX, CY, ANGLE, lambda u, v: -22 <= u <= -19 and abs(v) <= 2.6,
          lambda u, v: mix((200, 160, 70), (110, 80, 30), (v + 2.6) / 5.2))
    steel_hi, steel_mid, steel_lo = (236, 238, 242), (158, 164, 174), (70, 74, 84)

    def head_in(u, v):
        return 4.5 <= u <= 13.5 and -11 <= v <= 8

    def head_shade(u, v):
        t = (u - 4.5) / 9.0
        base = mix(steel_hi, steel_mid, t) if t < 0.5 else mix(steel_mid, steel_lo, (t - 0.5) * 2)
        if abs(v - 8) < 1.0 or abs(v + 11) < 1.0:  # striking faces: bright rims
            base = mix(base, steel_hi, 0.6)
        return base

    shape(img, CX, CY, ANGLE, head_in, head_shade)
    # brass band where the head meets the handle
    shape(img, CX, CY, ANGLE, lambda u, v: 3.2 <= u <= 5.2 and abs(v) <= 3.0,
          lambda u, v: mix((236, 196, 92), (128, 92, 30), (v + 3) / 6))
    return down(img)


def devil():
    """Worked stone hammer: a dressed block of dark stone with chipped corners, an iron band with
    rivets, and a straight dark handle with a leather-wrapped grip."""
    img = blank()

    def handle_in(u, v):
        return -20 <= u <= 6 and abs(v) <= 2.2

    def handle_shade(u, v):
        base = mix((104, 72, 46), (34, 22, 14), (v + 2.2) / 4.4)
        if -20 <= u <= -11:  # leather grip: diagonal wraps
            wrap = (u * 1.1 + v * 0.8) % 2.2
            base = mix((96, 58, 34), (40, 22, 12), (v + 2.2) / 4.4)
            if wrap < 0.5:
                base = mix(base, (20, 12, 6), 0.6)
        return tuple(max(0, min(255, int(c + 8 * math.sin(u * 2.3)))) for c in base)

    shape(img, CX, CY, ANGLE, handle_in, handle_shade)

    def head_in(u, v):
        if not (4.5 <= u <= 14 and -11.5 <= v <= 8.5):
            return False
        # chipped corners and a few nicks along the edges
        corner = min(abs(u - 4.5), abs(u - 14)) + min(abs(v + 11.5), abs(v - 8.5))
        if corner < 1.6:
            return False
        nick = 0.7 * max(0, math.sin(v * 1.9 + 1.0)) if u > 13.2 else 0
        return u <= 14 - nick

    def head_shade(u, v):
        t = (u - 4.5) / 9.5
        base = mix((150, 142, 132), (88, 82, 76), t) if t < 0.45 else mix((88, 82, 76), (38, 34, 32), (t - 0.45) / 0.55)
        if abs(v - 8.5) < 1.1 or abs(v + 11.5) < 1.1:  # striking faces catch the light
            base = mix(base, (176, 168, 156), 0.45)
        grain = 16 * math.sin(u * 6.1 + v * 2.3) * math.sin(v * 5.7 - u * 1.1)
        return tuple(max(0, min(255, int(c + grain))) for c in base)

    shape(img, CX, CY, ANGLE, head_in, head_shade)

    # rope: tan with dark twist lines
    def rope(u, v, across):
        twist = (u * 1.6 + v * 1.6 if across else u * 1.6 - v * 1.6) % 1.6
        base = (196, 164, 110) if twist > 0.45 else (92, 66, 36)
        return base

    # straps around the head, on both sides of where the handle passes through
    strap_v = (-3.2, 1.6)
    shape(img, CX, CY, ANGLE,
          lambda u, v: 4.0 <= u <= 14.4 and any(abs(v - s) <= 0.95 for s in strap_v),
          lambda u, v: mix(rope(u, v, False), (40, 28, 16), 0.35 * min(abs(v - s) for s in strap_v) / 0.95))
    # wraps around the top of the handle, just under the head
    shape(img, CX, CY, ANGLE, lambda u, v: 0.6 <= u <= 4.6 and abs(v) <= 2.9,
          lambda u, v: mix(rope(u, v, True), (40, 28, 16), 0.45 * abs(v) / 2.9))
    return down(img)


def with_shadow(art):
    """The hammer and its soft shadow on a transparent 52x43 icon (the game draws the grid tile
    underneath every icon itself)."""
    img = [[(0, 0, 0, 0)] * W for _ in range(H)]
    over(img, art, 2, 2, opacity=0.65, tint=(0, 0, 0))
    over(img, art, 0, 0)
    return img


def on_tile(u, icon):
    tile = [list(row) for row in u.rows(TILE)]
    over(tile, icon, 0, 0)
    return tile


def icons():
    return {"hammer_human": with_shadow(human()), "hammer_devil": with_shadow(devil())}


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    u = ure.URE(sys.argv[1])
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    art = icons()
    for name, img in art.items():
        ure.write_png(os.path.join(outdir, name + ".png"), img)  # transparent, our art only
    # preview: the hammers on the game's tile, then the game's own tool icons (17, 52), at 4x
    samples = [on_tile(u, art["hammer_human"]), on_tile(u, art["hammer_devil"]), u.rows(17), u.rows(52)]
    gap = [(30, 30, 30, 255)] * 6
    rows = []
    for y in range(H):
        line = []
        for k, img in enumerate(samples):
            line += [px if px[3] else (30, 30, 30, 255) for px in img[y]] + (gap * 3 if k == 1 else gap)
        rows.append(line)
    ure.write_png(os.path.join(outdir, "preview.png"), upscale(rows, 4))
    print("wrote", outdir)
