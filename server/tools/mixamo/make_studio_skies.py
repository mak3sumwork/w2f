"""Painted backdrops for the splash studio (demo 1.6): one sky per origin, 2048 x 1152, behind the posed hero in UE (W2FStudio.cpp puts it on a card far behind the
hero; depth of field softens it into a painterly blur).

    ~/w2f_bpy/venv/bin/python tools/mixamo/make_studio_skies.py [--out DIR]

Output: docs/icons/studio/T_StudioSky_<Origin>.png (Helios, Selini, Phaisa, Coregons, Hexagon, Najmi, Nature, Omnilium). tools/unreal/import_studio.py imports them.
Pure numpy: gradients, fractal noise clouds, glows, stars and layered silhouettes (mountains, spires, trees) -- deterministic (fixed seeds).
"""
import os
import sys
import zlib
import struct

import numpy as np

W, H = 2048, 1152
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "icons", "studio"))


def write_png(path, rgb):
    a = (np.clip(rgb, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)
    h, w, _ = a.shape
    raw = b"".join(b"\x00" + a[y].tobytes() for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def lin(c):
    """sRGB hex -> linear rgb"""
    c = np.array([int(c[i:i + 2], 16) / 255.0 for i in (1, 3, 5)])
    return c ** 2.2


def value_noise(rng, w, h, cells):
    g = rng.random((cells[1] + 3, cells[0] + 3))
    ys = np.linspace(0, cells[1], h, endpoint=False)
    xs = np.linspace(0, cells[0], w, endpoint=False)
    y0, x0 = ys.astype(int), xs.astype(int)
    fy, fx = ys - y0, xs - x0
    fy = fy * fy * (3 - 2 * fy)
    fx = fx * fx * (3 - 2 * fx)
    a = g[y0][:, x0]; b = g[y0][:, x0 + 1]; c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
    top = a + (b - a) * fx[None, :]
    bot = c + (d - c) * fx[None, :]
    return top + (bot - top) * fy[:, None]


def fbm(rng, w, h, base=(6, 4), octaves=6, gain=0.5):
    out = np.zeros((h, w)); amp = 1.0; tot = 0.0
    for o in range(octaves):
        out += amp * value_noise(rng, w, h, (base[0] * 2 ** o, base[1] * 2 ** o))
        tot += amp; amp *= gain
    return out / tot


def vgrad(stops):
    """vertical gradient from [(t, colour), ...], t = 0 top .. 1 bottom"""
    t = np.linspace(0, 1, H)[:, None]
    img = np.zeros((H, 1, 3))
    for (t0, c0), (t1, c1) in zip(stops[:-1], stops[1:]):
        m = ((t >= t0) & (t <= t1))
        k = np.clip((t - t0) / max(1e-6, t1 - t0), 0, 1)
        k = k * k * (3 - 2 * k)
        img = np.where(m[..., None], c0 + (c1 - c0) * k[..., None], img)
    return np.repeat(img, W, axis=1)


def glow(img, cx, cy, radius, colour, power=2.0, strength=1.0):
    ys, xs = np.mgrid[0:H, 0:W]
    d = np.sqrt((xs - cx) ** 2 + (ys - cy) ** 2) / radius
    img += colour * (strength * np.exp(-d ** power))[..., None]


def disc(img, cx, cy, r, colour, soft=3.0):
    ys, xs = np.mgrid[0:H, 0:W]
    d = np.sqrt((xs - cx) ** 2 + (ys - cy) ** 2)
    a = np.clip((r - d) / soft, 0, 1)
    img[:] = img * (1 - a[..., None]) + colour * a[..., None]


def clouds(img, rng, colour, lo, hi, y0, y1, strength=0.8, base=(5, 3), stretch=3.0):
    n = fbm(rng, W, H, base=(base[0], max(1, int(base[1] * stretch))), octaves=6)
    n = np.clip((n - lo) / (hi - lo), 0, 1)
    t = np.linspace(0, 1, H)[:, None]
    band = np.clip((t - y0) / 0.08, 0, 1) * np.clip((y1 - t) / 0.08, 0, 1)
    a = n * band * strength
    img[:] = img * (1 - a[..., None]) + colour * a[..., None]


def stars(img, rng, count, y1=0.7, colour=(1, 1, 1), big=0.04):
    for _ in range(count):
        x, y = rng.integers(0, W), int(rng.random() ** 1.3 * H * y1)
        b = rng.random() ** 3
        r = 1 if rng.random() > big else 2
        img[max(0, y - r):y + r, max(0, x - r):x + r] += np.array(colour) * (0.4 + 2.5 * b)
        if r == 2:
            glow(img, x, y, 10, np.array(colour) * 0.25, 2, 1)


def ridge(rng, base_y, amp, rough, smooth=6):
    """a silhouette line: base_y (0..1 of H) + fractal wobble; returns the y (pixels) of the edge for each column"""
    n = fbm(rng, W, 2, base=(smooth, 1), octaves=7, gain=rough)[0]
    return (base_y + (n - 0.5) * amp) * H


def silhouette(img, edge, colour, haze_colour=None, haze=0.0):
    ys = np.arange(H)[:, None]
    a = np.clip((ys - edge[None, :]) / 2.0, 0, 1)
    c = colour if haze_colour is None else colour * (1 - haze) + haze_colour * haze
    img[:] = img * (1 - a[..., None]) + c * a[..., None]


def spires(rng, count, base_y, hmin, hmax, wmin, wmax):
    edge = np.full(W, base_y * H)
    for _ in range(count):
        x = rng.integers(0, W); h = rng.uniform(hmin, hmax) * H; w = rng.uniform(wmin, wmax) * W
        xs = np.arange(W)
        prof = np.clip(1 - np.abs(xs - x) / w, 0, 1) ** 1.6
        edge = np.minimum(edge, base_y * H - h * prof)
    return edge


def trees(rng, count, base_y, hmin, hmax):
    edge = np.full(W, base_y * H)
    xs = np.arange(W)
    for _ in range(count):
        x = rng.integers(0, W); h = rng.uniform(hmin, hmax) * H; w = h * rng.uniform(0.18, 0.3)
        dx = np.abs(xs - x) / w
        prof = np.clip(1 - dx, 0, 1)
        tiered = np.clip(prof - 0.09 * ((prof * 5) % 1), 0, 1) * (prof > 0)   # a conifer: stacked tiers of branches, a pointed top
        edge = np.minimum(edge, base_y * H - h * tiered)
    return edge


def rays(img, cx, cy, colour, count=14, spread=1.2, strength=0.25, rng=None):
    ys, xs = np.mgrid[0:H, 0:W]
    ang = np.arctan2(ys - cy, xs - cx)
    d = np.sqrt((xs - cx) ** 2 + (ys - cy) ** 2) / W
    pattern = np.zeros_like(ang)
    for i in range(count):
        a0 = rng.uniform(-np.pi, np.pi)
        pattern += np.exp(-((np.angle(np.exp(1j * (ang - a0)))) / rng.uniform(0.02, 0.06)) ** 2)
    img += colour * (np.clip(pattern, 0, 1) * np.exp(-d * spread) * strength)[..., None]


def sky_helios(rng):
    img = vgrad([(0, lin("#2a1740")), (0.35, lin("#b8452a")), (0.62, lin("#ffb357")), (1, lin("#ffe0a0"))])
    glow(img, W * 0.68, H * 0.6, W * 0.35, lin("#ffcf7a"), 1.4, 1.2)
    rays(img, W * 0.68, H * 0.6, lin("#ffd89a"), 18, 2.2, 0.35, rng)
    glow(img, W * 0.68, H * 0.6, H * 0.16, lin("#ffe2a0"), 2.0, 0.9)          # the sun: a soft blazing core, no hard edge (a flat disc reads as a grey plate)
    glow(img, W * 0.68, H * 0.6, H * 0.07, lin("#fffaf0"), 2.5, 1.2)
    clouds(img, rng, lin("#6a2536"), 0.5, 0.75, 0.15, 0.55, 0.85)
    clouds(img, rng, lin("#ff9a5a"), 0.55, 0.8, 0.3, 0.6, 0.5)
    silhouette(img, ridge(rng, 0.74, 0.18, 0.55), lin("#6b2f35"), lin("#ffb357"), 0.25)
    silhouette(img, ridge(rng, 0.84, 0.12, 0.6), lin("#2b1320"))
    return img


def sky_selini(rng):
    img = vgrad([(0, lin("#050818")), (0.45, lin("#0e2346")), (0.72, lin("#1e5870")), (1, lin("#0a1a2a"))])
    stars(img, rng, 1400, 0.7, (0.85, 0.9, 1.0))
    glow(img, W * 0.3, H * 0.28, W * 0.28, lin("#6fb4ff"), 1.3, 0.6)
    disc(img, W * 0.3, H * 0.28, H * 0.11, lin("#e8f1ff") * 1.5, 4)
    clouds(img, rng, lin("#1c3558"), 0.55, 0.8, 0.2, 0.6, 0.6)
    clouds(img, rng, lin("#86b6e8"), 0.62, 0.85, 0.3, 0.55, 0.3)
    silhouette(img, ridge(rng, 0.76, 0.1, 0.5), lin("#0b2238"), lin("#1e5870"), 0.3)
    glow(img, W * 0.5, H * 0.8, W * 0.6, lin("#3aa0c8"), 2.0, 0.35)
    silhouette(img, ridge(rng, 0.86, 0.04, 0.4, 3), lin("#071320"))
    return img


def sky_phaisa(rng):
    img = vgrad([(0, lin("#07020e")), (0.5, lin("#1d0730")), (1, lin("#0b0314"))])
    n = fbm(rng, W, H, (4, 3), 7, 0.55)
    img += (np.clip(n - 0.45, 0, 1) ** 1.5 * 3.0)[..., None] * lin("#b02ad8")
    n2 = fbm(rng, W, H, (6, 4), 6, 0.55)
    img += (np.clip(n2 - 0.55, 0, 1) ** 1.5 * 3.0)[..., None] * lin("#ff3fa0")
    stars(img, rng, 500, 0.9, (1.0, 0.8, 1.0))
    # the rift: a jagged vertical crack of light
    xs = W * 0.62 + np.cumsum(rng.normal(0, 3.0, H))
    ys = np.arange(H)
    for y in range(0, H, 2):
        glow_strength = np.exp(-((y - H * 0.45) / (H * 0.3)) ** 2)
        img[y:y + 2, int(max(0, xs[y] - 3)):int(xs[y] + 3)] += lin("#ffd0ff") * 3 * glow_strength
    glow(img, W * 0.62, H * 0.45, W * 0.12, lin("#e060ff"), 1.5, 1.0)
    silhouette(img, spires(rng, 22, 0.86, 0.05, 0.3, 0.004, 0.02), lin("#12051c"), lin("#6a1a88"), 0.2)
    silhouette(img, ridge(rng, 0.9, 0.05, 0.5), lin("#050108"))
    return img


def sky_coregons(rng):
    img = vgrad([(0, lin("#030b0a")), (0.5, lin("#0c2a26")), (0.8, lin("#2a6a5a")), (1, lin("#0a1a18"))])
    glow(img, W * 0.55, H * 0.35, W * 0.3, lin("#8fffe0"), 1.4, 0.6)
    glow(img, W * 0.55, H * 0.35, H * 0.12, lin("#b8ffe8"), 2.0, 0.8)         # a cold ghost light, soft-edged
    glow(img, W * 0.55, H * 0.35, H * 0.05, lin("#f0fffa"), 2.5, 1.0)
    clouds(img, rng, lin("#123a34"), 0.45, 0.75, 0.1, 0.65, 0.8)
    clouds(img, rng, lin("#5ad8b0"), 0.62, 0.82, 0.55, 0.85, 0.35, stretch=5)
    silhouette(img, trees(rng, 26, 0.84, 0.08, 0.3), lin("#07150f"), lin("#2a6a5a"), 0.35)
    silhouette(img, spires(rng, 12, 0.9, 0.03, 0.12, 0.002, 0.006), lin("#030806"))
    return img


def sky_hexagon(rng):
    img = vgrad([(0, lin("#030a18")), (0.55, lin("#0a2a4a")), (0.85, lin("#1a6a8a")), (1, lin("#050d18"))])
    ys, xs = np.mgrid[0:H, 0:W].astype(float)
    s = 64.0
    q = (xs * np.sqrt(3) / 3 - ys / 3) / s
    r = ys * 2 / 3 / s
    cz = -q - r
    rq, rr, rz = np.round(q), np.round(r), np.round(cz)
    edge = np.maximum.reduce([np.abs(q - rq), np.abs(r - rr), np.abs(cz - rz)])
    hexes = np.clip((edge - 0.42) / 0.08, 0, 1)
    fade = np.exp(-((ys / H - 0.35) / 0.3) ** 2)
    img += lin("#39e8ff") * (hexes * fade * 0.35)[..., None]
    glow(img, W * 0.5, H * 0.8, W * 0.5, lin("#39e8ff"), 2, 0.5)
    for _ in range(9):
        x = rng.integers(0, W)
        img[:int(H * 0.85), max(0, x - 2):x + 2] += lin("#7ff4ff") * 0.6 * np.linspace(0, 1, int(H * 0.85))[:, None, None]
    silhouette(img, spires(rng, 40, 0.88, 0.05, 0.4, 0.004, 0.012), lin("#06121e"), lin("#1a6a8a"), 0.25)
    silhouette(img, ridge(rng, 0.93, 0.02, 0.4, 2), lin("#02060c"))
    return img


def sky_najmi(rng):
    img = vgrad([(0, lin("#02020a")), (0.6, lin("#0b0a28")), (1, lin("#05040f"))])
    ys, xs = np.mgrid[0:H, 0:W].astype(float)
    band = np.exp(-(((ys - H * 0.5) - (xs - W * 0.5) * 0.35) / (H * 0.16)) ** 2)
    n = fbm(rng, W, H, (8, 5), 7, 0.55)
    img += band[..., None] * (np.clip(n, 0, 1) ** 2)[..., None] * (lin("#6a4cff") * 1.6)
    img += band[..., None] * (np.clip(n - 0.5, 0, 1) * 2)[..., None] * lin("#ffcf6a")
    stars(img, rng, 2600, 1.0, (1.0, 0.95, 0.85), big=0.08)
    glow(img, W * 0.72, H * 0.3, W * 0.1, lin("#ffe7a8"), 1.2, 0.8)
    for _ in range(4):   # comets
        x, y = rng.uniform(0.1, 0.9) * W, rng.uniform(0.05, 0.5) * H
        for k in range(120):
            glow_a = (1 - k / 120) * 0.6
            yy, xx = int(y - k * 0.8), int(x - k * 2.2)
            if 0 <= yy < H and 0 <= xx < W:
                img[yy:yy + 2, xx:xx + 2] += lin("#bfd4ff") * glow_a
    return img


def sky_nature(rng):
    img = vgrad([(0, lin("#0c2a1a")), (0.45, lin("#4a8a3a")), (0.7, lin("#d8e890")), (1, lin("#2a4a1a"))])
    glow(img, W * 0.35, H * 0.25, W * 0.35, lin("#fff2b0"), 1.2, 0.9)
    rays(img, W * 0.35, H * 0.05, lin("#fff0b0"), 16, 1.5, 0.45, rng)
    for depth, (y, col, haze, n, hmin, hmax) in enumerate([(0.7, "#4a7a3a", 0.45, 60, 0.08, 0.2), (0.8, "#24481c", 0.2, 34, 0.14, 0.32), (0.93, "#0a1a08", 0.0, 14, 0.3, 0.6)]):
        silhouette(img, trees(rng, n, y, hmin, hmax), lin(col), lin("#b8d880"), haze)
        if depth < 2:
            clouds(img, rng, lin("#dcecb0"), 0.58, 0.85, y - 0.12, y + 0.02, 0.35, stretch=8)
    return img


def sky_omnilium(rng):
    img = vgrad([(0, lin("#0a1a2a")), (0.4, lin("#2a6a7a")), (0.7, lin("#e8d8a0")), (1, lin("#3a3a4a"))])
    glow(img, W * 0.6, H * 0.62, W * 0.3, lin("#ffe8b0"), 1.4, 0.9)
    clouds(img, rng, lin("#8ac8d0"), 0.55, 0.8, 0.15, 0.55, 0.5)
    edge = spires(rng, 18, 0.82, 0.08, 0.45, 0.006, 0.02)
    silhouette(img, edge, lin("#1a4a5a"), lin("#e8d8a0"), 0.3)
    silhouette(img, ridge(rng, 0.88, 0.06, 0.5), lin("#0a1a22"))
    return img


SKIES = {"Helios": sky_helios, "Selini": sky_selini, "Phaisa": sky_phaisa, "Coregons": sky_coregons, "Hexagon": sky_hexagon,
         "Najmi": sky_najmi, "Nature": sky_nature, "Omnilium": sky_omnilium}


def main():
    argv = sys.argv[1:]
    out = argv[argv.index("--out") + 1] if "--out" in argv else OUT
    os.makedirs(out, exist_ok=True)
    for i, (name, fn) in enumerate(SKIES.items()):
        img = fn(np.random.default_rng(1000 + i))
        write_png(os.path.join(out, "T_StudioSky_%s.png" % name), img)
        print("wrote T_StudioSky_%s.png" % name, flush=True)


if __name__ == "__main__":
    main()
