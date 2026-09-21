#!/usr/bin/env python3
"""W2F placeholder icons: a 64x64 icon for every item (T_Item_<id>.png), a bust portrait for every champion (T_Portrait_<id>.png), and a few UI glyphs
(T_Coin, T_LockOn, T_LockOff), written to docs/icons/ for the Unreal viewer. Standard library only; deterministic.

    python3 tools/make_icons.py [--data data] [--blockouts docs/blockouts] [--out docs/icons]

Items are drawn from primitives at 3x and downsampled (anti-aliased): the 8 base components have a glyph each (helmet, drop, sword, vest, bow, staff, glove, heart), the Omnilium Seed a
sprouting seed, every finished item the glyphs of its two ingredients on a gold frame, every emblem the seed on its trait's colour, the Item Remover a red cross. Portraits are rendered
from the blockout models (run make_blockouts.py first) on a background in the champion's cost colour. Replace them with real art later: the viewer only loads PNGs by name.
"""
import argparse, math, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_blockouts as mb   # noqa: E402  (load_json, TRAIT_COLOURS, read_glb, render_cell, write_png)

S = 3   # supersampling


class Canvas:
    def __init__(self, size):
        self.n = size * S
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(self.n * self.n)]

    def _blend(self, i, c, a):
        d = self.px[i]
        oa = a + d[3] * (1 - a)
        if oa <= 0: return
        for k in range(3): d[k] = (c[k] * a + d[k] * d[3] * (1 - a)) / oa
        d[3] = oa

    def poly(self, pts, colour, alpha=1.0):
        P = [(x * self.n, y * self.n) for x, y in pts]
        miny, maxy = max(0, int(min(p[1] for p in P))), min(self.n - 1, int(max(p[1] for p in P)) + 1)
        for y in range(miny, maxy + 1):
            yc = y + 0.5
            xs = []
            for i in range(len(P)):
                (x0, y0), (x1, y1) = P[i], P[(i + 1) % len(P)]
                if (y0 <= yc < y1) or (y1 <= yc < y0): xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for j in range(0, len(xs) - 1, 2):
                for x in range(max(0, int(xs[j] + 0.5)), min(self.n, int(xs[j + 1] + 0.5))): self._blend(y * self.n + x, colour, alpha)

    def circle(self, cx, cy, r, colour, alpha=1.0, ry=None):
        ry = r if ry is None else ry
        pts = [(cx + r * math.cos(2 * math.pi * i / 28), cy + ry * math.sin(2 * math.pi * i / 28)) for i in range(28)]
        self.poly(pts, colour, alpha)

    def rect(self, x0, y0, x1, y1, colour, alpha=1.0): self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], colour, alpha)

    def line(self, x0, y0, x1, y1, w, colour, alpha=1.0):
        dx, dy = x1 - x0, y1 - y0
        l = math.hypot(dx, dy) or 1
        nx, ny = -dy / l * w / 2, dx / l * w / 2
        self.poly([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], colour, alpha)

    def rrect(self, x0, y0, x1, y1, r, colour, alpha=1.0):
        pts = []
        for (cx, cy, a0) in ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180)):
            for k in range(7):
                a = math.radians(a0 + 90 * k / 6)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        self.poly(pts, colour, alpha)

    def to_rgba(self, size):
        out = bytearray()
        for y in range(size):
            for x in range(size):
                acc = [0.0, 0.0, 0.0, 0.0]
                for sy in range(S):
                    for sx in range(S):
                        d = self.px[(y * S + sy) * self.n + x * S + sx]
                        acc[3] += d[3]
                        for k in range(3): acc[k] += d[k] * d[3]
                a = acc[3] / (S * S)
                for k in range(3): out.append(int(min(1.0, (acc[k] / acc[3]) if acc[3] > 0 else 0.0) * 255 + 0.5))
                out.append(int(min(1.0, a) * 255 + 0.5))
        return bytes(out)


def write_png_rgba(path, w, h, data):
    raw = bytearray()
    for y in range(h):
        raw.append(0); raw += data[y * w * 4:(y + 1) * w * 4]
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def col(h): return mb.srgb(h)
def dark(c, t=0.45): return tuple(v * (1 - t) for v in c)
def light(c, t=0.4): return tuple(v + (1 - v) * t for v in c)

# ---- glyphs: each draws into the unit square placed at (ox, oy) with size k (canvas fractions)
def T(ox, oy, k): return lambda x, y: (ox + x * k, oy + y * k)

def K(t): return t(1, 0)[0] - t(0, 0)[0]

def ln(c, t, p0, p1, w, colour): c.line(*t(*p0), *t(*p1), w * K(t), colour)

def g_helmet(c, t, base):
    c.poly([t(0.15, 0.62)] + [t(0.5 + 0.35 * math.cos(math.radians(a)), 0.55 - 0.42 * math.sin(math.radians(a))) for a in range(0, 181, 15)][::-1] + [t(0.85, 0.62)], base)
    c.rect(*t(0.15, 0.55), *t(0.85, 0.82), dark(base, 0.15)); c.rect(*t(0.3, 0.6), *t(0.7, 0.68), (0.08, 0.08, 0.1))
    c.poly([t(0.47, 0.1), t(0.53, 0.1), t(0.56, 0.4), t(0.44, 0.4)], light(base, 0.5))

def g_water(c, t, base):
    k = K(t)
    c.poly([t(0.5, 0.1), t(0.26, 0.55), t(0.74, 0.55)], base); c.circle(t(0.5, 0.62)[0], t(0.5, 0.62)[1], 0.24 * k, base)
    c.circle(t(0.42, 0.6)[0], t(0.42, 0.6)[1], 0.06 * k, (1, 1, 1), 0.7)

def g_sword(c, t, base):
    c.poly([t(0.46, 0.08), t(0.54, 0.08), t(0.57, 0.66), t(0.43, 0.66)], base); c.poly([t(0.46, 0.08), t(0.5, 0.0), t(0.54, 0.08)], light(base, 0.3))
    c.rect(*t(0.27, 0.66), *t(0.73, 0.73), (0.85, 0.7, 0.3)); c.rect(*t(0.46, 0.73), *t(0.54, 0.9), (0.45, 0.28, 0.15))
    c.circle(t(0.5, 0.93)[0], t(0.5, 0.93)[1], 0.05 * (t(1, 0)[0] - t(0, 0)[0]), (0.85, 0.7, 0.3))

def g_vest(c, t, base):
    c.poly([t(0.28, 0.15), t(0.42, 0.15), t(0.5, 0.27), t(0.58, 0.15), t(0.72, 0.15), t(0.9, 0.36), t(0.74, 0.47), t(0.74, 0.86), t(0.26, 0.86), t(0.26, 0.47), t(0.1, 0.36)], base)
    ln(c, t, (0.5, 0.3), (0.5, 0.86), 0.02, dark(base, 0.4)); c.rect(*t(0.26, 0.7), *t(0.74, 0.76), dark(base, 0.3))

def g_bow(c, t, base):
    pts = [t(0.62 + 0.28 * math.sin(math.radians(a)) * 0.9, 0.5 - 0.42 * math.cos(math.radians(a))) for a in range(-90, 91, 15)]
    for i in range(len(pts) - 1): c.line(*pts[i], *pts[i + 1], 0.06 * K(t), base)
    ln(c, t, (0.62, 0.08), (0.62, 0.92), 0.012, (0.9, 0.9, 0.9)); ln(c, t, (0.2, 0.5), (0.7, 0.5), 0.025, (0.8, 0.8, 0.85)); c.poly([t(0.14, 0.5), t(0.24, 0.45), t(0.24, 0.55)], (0.85, 0.85, 0.9))

def g_stick(c, t, base):
    ln(c, t, (0.3, 0.9), (0.66, 0.26), 0.07, (0.5, 0.33, 0.2))
    r = 0.15 * K(t)
    c.circle(t(0.7, 0.2)[0], t(0.7, 0.2)[1], r, base); c.circle(t(0.66, 0.16)[0], t(0.66, 0.16)[1], r * 0.35, (1, 1, 1), 0.75)

def g_glove(c, t, base):
    k = t(1, 0)[0] - t(0, 0)[0]
    c.rrect(*t(0.3, 0.42), *t(0.72, 0.84), 0.05 * k, base)
    for i in range(4): c.rrect(*t(0.3 + i * 0.105, 0.2 + (0.03 if i in (0, 3) else 0)), *t(0.39 + i * 0.105, 0.5), 0.03 * k, light(base, 0.1))
    c.rrect(*t(0.16, 0.5), *t(0.34, 0.62), 0.04 * k, light(base, 0.1)); c.rect(*t(0.3, 0.8), *t(0.72, 0.9), dark(base, 0.3))

def g_heart(c, t, base):
    k = t(1, 0)[0] - t(0, 0)[0]
    c.circle(t(0.37, 0.38)[0], t(0.37, 0.38)[1], 0.2 * k, base); c.circle(t(0.63, 0.38)[0], t(0.63, 0.38)[1], 0.2 * k, base)
    c.poly([t(0.19, 0.46), t(0.81, 0.46), t(0.5, 0.86)], base); c.circle(t(0.32, 0.32)[0], t(0.32, 0.32)[1], 0.05 * k, (1, 1, 1), 0.6)

def g_seed(c, t, base):
    k = t(1, 0)[0] - t(0, 0)[0]
    c.circle(t(0.5, 0.6)[0], t(0.5, 0.6)[1], 0.2 * k, base, ry=0.27 * k); c.circle(t(0.44, 0.52)[0], t(0.44, 0.52)[1], 0.05 * k, (1, 1, 0.9), 0.7)
    c.poly([t(0.5, 0.36), t(0.72, 0.2), t(0.66, 0.4)], (0.35, 0.75, 0.3)); c.poly([t(0.5, 0.36), t(0.3, 0.16), t(0.36, 0.38)], (0.3, 0.65, 0.28))

def g_remove(c, t, base):
    ln(c, t, (0.22, 0.22), (0.78, 0.78), 0.13, base); ln(c, t, (0.78, 0.22), (0.22, 0.78), 0.13, base)

GLYPHS = {1: (g_helmet, "8FA6BF"), 2: (g_water, "49A3F2"), 3: (g_sword, "D4D9E2"), 4: (g_vest, "AEB8C9"), 5: (g_bow, "A06A35"), 6: (g_stick, "A56BE0"),
          7: (g_glove, "E4A25C"), 8: (g_heart, "E5566A"), 9: (g_seed, "CDB544")}


def frame(c, border, top, bottom):
    c.rrect(0.02, 0.02, 0.98, 0.98, 0.14, border)
    steps = 12
    for i in range(steps):
        y0 = 0.07 + 0.86 * i / steps; y1 = 0.07 + 0.86 * (i + 1) / steps
        f = i / (steps - 1); cc = tuple(top[k] * (1 - f) + bottom[k] * f for k in range(3))
        if i == 0: c.rrect(0.07, y0, 0.93, y1 + 0.1, 0.1, cc)
        elif i == steps - 1: c.rrect(0.07, y0 - 0.1, 0.93, y1, 0.1, cc)
        else: c.rect(0.07, y0, 0.93, y1 + 0.002, cc)


def item_icon(item, items_by_id):
    c = Canvas(64)
    iid = item["id"]; comps = item.get("components")
    if item.get("consumable"):
        frame(c, col("C0392B"), col("5A1E1A"), col("2B0F0D")); g_remove(c, T(0.05, 0.05, 0.9), col("FF6B5B"))
    elif iid in GLYPHS:
        g, colour = GLYPHS[iid]
        frame(c, col("8899AA"), col("39445A"), col("1B2130"))
        g(c, T(0.1, 0.1, 0.8), col(colour))
    elif 40 <= iid <= 47:
        trait = (item.get("grantsTraits") or [""])[0]
        tc = col(mb.TRAIT_COLOURS.get(trait, "8A8F99"))
        frame(c, light(tc, 0.2), dark(tc, 0.2), dark(tc, 0.6))
        c.circle(0.5, 0.5, 0.33, dark(tc, 0.3)); c.circle(0.5, 0.5, 0.27, tc, 0.9)
        g_seed(c, T(0.2, 0.2, 0.6), col("F3E7A0"))
    else:   # a finished item: its two ingredients on a gold frame
        frame(c, col("E2B94A"), col("3B3222"), col("1C1710"))
        a, b = (comps or [1, 2])[0], (comps or [1, 2])[1]
        ga, ca = GLYPHS.get(a, GLYPHS[1]); gb, cb = GLYPHS.get(b, GLYPHS[2])
        ga(c, T(0.06, 0.08, 0.58), col(ca)); gb(c, T(0.36, 0.34, 0.58), col(cb))
        c.circle(0.84, 0.16, 0.06, col("FFD86B")); c.circle(0.84, 0.16, 0.03, col("FFF3B8"))
    return c.to_rgba(64)


def ui_icons(out):
    c = Canvas(48)
    c.circle(0.5, 0.5, 0.46, col("B8860B")); c.circle(0.5, 0.5, 0.4, col("FFD54A")); c.circle(0.5, 0.5, 0.27, col("F0B400"))
    c.rect(0.46, 0.3, 0.54, 0.7, col("FFF0A0"))
    write_png_rgba(os.path.join(out, "T_Coin.png"), 48, 48, c.to_rgba(48))
    for name, locked in (("T_LockOn", True), ("T_LockOff", False)):
        c = Canvas(48)
        body = col("FFD54A") if locked else col("9AA5B4")
        c.rrect(0.2, 0.44, 0.8, 0.9, 0.08, body)
        arc = [(0.5 + 0.2 * math.cos(math.radians(a)), (0.44 if locked else 0.3) - 0.2 * math.sin(math.radians(a))) for a in range(0, 181, 15)]
        if not locked: arc = [(x + 0.12, y) for x, y in arc]
        for i in range(len(arc) - 1): c.line(*arc[i], *arc[i + 1], 0.09, body)
        c.line(arc[0][0], arc[0][1], arc[0][0], 0.46, 0.09, body)
        if locked: c.line(arc[-1][0], arc[-1][1], arc[-1][0], 0.46, 0.09, body)
        c.circle(0.5, 0.64, 0.07, (0.15, 0.15, 0.2)); c.rect(0.48, 0.64, 0.52, 0.78, (0.15, 0.15, 0.2))
        write_png_rgba(os.path.join(out, name + ".png"), 48, 48, c.to_rgba(48))


COST_COLOURS = {1: ("7C8794", "3A414B"), 2: ("57B26E", "1E4A2D"), 3: ("4C86E0", "1B3A73"), 4: ("A560D6", "45206A"), 5: ("F0B93C", "7A5510")}


def portraits(out, champs, blockouts):
    import json
    manifest = json.load(open(os.path.join(blockouts, "manifest.json")))
    heads = {m["id"]: m["sockets_m"]["head_top"]["up"] for m in manifest["models"] if "sockets_m" in m}
    idx = {}
    for f in os.listdir(blockouts):
        if f.startswith("SM_Champion_") and f.endswith(".glb"): idx[int(f.split("_")[2])] = os.path.join(blockouts, f)
    made = 0
    for c in champs:
        if c["id"] not in idx or c.get("summon"): continue
        n, lo, hi, pos, nor, colr = mb.read_glb(idx[c["id"]])
        W, H = 480, 480
        px = mb.render_cell((pos, nor, colr), W, H, yaw=22.0, pitch=6.0)
        # the renderer centres the model and fits it to the canvas: recompute its scale to crop a bust (the head and shoulders, about 1.1 m of the model) out of the top
        height, diag = hi[1] - lo[1], math.hypot(hi[0] - lo[0], hi[2] - lo[2])
        sc = min((H - 30) / max(height + 0.2, 1.0), (W - 20) / max(diag, 1.0))
        head_top = heads.get(c["id"], 1.75)
        head_y = H / 2 - 8 - (0.84 * head_top - (lo[1] + hi[1]) / 2) * sc        # the head's centre on screen (the renderer centres the model vertically)
        side = 1.05 * sc
        cx0, cy0 = W / 2 - side / 2, head_y - side * 0.42
        top, bottom = COST_COLOURS.get(c["cost"], COST_COLOURS[1])
        tc, bc = mb.srgb(top), mb.srgb(bottom)
        bg = (0.93, 0.94, 0.96)
        size = 160
        rows = bytearray()
        for y in range(size):
            f = y / (size - 1)
            for x in range(size):
                sx = min(W - 1, max(0, int(cx0 + (x + 0.5) * side / size))); sy = min(H - 1, max(0, int(cy0 + (y + 0.5) * side / size)))
                p = px[sy * W + sx]
                if abs(p[0] - bg[0]) < 1e-6 and abs(p[1] - bg[1]) < 1e-6 and abs(p[2] - bg[2]) < 1e-6:
                    v = tuple(tc[k] * (1 - f) * 0.9 + bc[k] * f for k in range(3))
                    v = tuple(v[k] * (1 - 0.35 * (math.hypot(x - size / 2, y - size / 2) / (size * 0.75))) for k in range(3))
                else: v = p
                rows += bytes((int(min(1, v[0]) * 255 + 0.5), int(min(1, v[1]) * 255 + 0.5), int(min(1, v[2]) * 255 + 0.5), 255))
        write_png_rgba(os.path.join(out, "T_Portrait_%d.png" % c["id"]), size, size, bytes(rows))
        made += 1
    return made


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(HERE, "..", "data")); ap.add_argument("--blockouts", default=os.path.join(HERE, "..", "docs", "blockouts"))
    ap.add_argument("--out", default=os.path.join(HERE, "..", "docs", "icons"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for f in os.listdir(a.out):
        if f.endswith(".png"): os.remove(os.path.join(a.out, f))
    items = mb.load_json(os.path.join(a.data, "items.json"))["items"]
    by_id = {i["id"]: i for i in items}
    for it in items: write_png_rgba(os.path.join(a.out, "T_Item_%d.png" % it["id"]), 64, 64, item_icon(it, by_id))
    ui_icons(a.out)
    champs = mb.load_json(os.path.join(a.data, "champions.json"))["champions"]
    n = portraits(a.out, champs, a.blockouts)
    print("wrote %d item icons, %d portraits, 3 UI glyphs to %s" % (len(items), n, a.out))


if __name__ == "__main__":
    main()
