"""The sprite textures of the fight VFX (demo 1.3): <UE project>/SourceArt/FX/T_FX_*.png, imported by tools/unreal/import_fx.py.

    ~/w2f_bpy/venv/bin/python tools/mixamo/make_fx_textures.py [--out DIR]

Every texture is 512 x 512, linear:
  R = the shape (what the sprite looks like),
  G = an erosion field (0.2 .. 1, fbm noise shaped to the sprite): the sprite material dissolves pixels whose G is below the sprite's `erode` value, so smoke,
      fire and shock rings break up into wisps as they age instead of just fading,
  B = a "heat" mask (the brightest core): the material pushes it toward white by the sprite's `hot` value.
The names and order of the first eight match the old set (build_fx_meshes.py), so UW2FFx's texture indices stay valid; the rest are new.
"""
import math, os, sys
import numpy as np
import bpy

N = 512
OUT = os.path.expanduser("~/Desktop/work2fightgame/work2fightgame/SourceArt/FX")
rng = np.random.default_rng(1303)
y, x = np.mgrid[1:-1:N * 1j, -1:1:N * 1j]          # y up
r = np.hypot(x, y)
ang = np.arctan2(y, x)


def smooth(t):
    return t * t * (3 - 2 * t)


def value_noise(cells, seed_rng, tile=True):
    """Smooth value noise on a `cells` x `cells` lattice, tileable."""
    g = seed_rng.random((cells + 1, cells + 1))
    if tile:
        g[-1, :] = g[0, :]; g[:, -1] = g[:, 0]
    u = (x + 1) / 2 * cells; v = (y + 1) / 2 * cells
    i = np.clip(np.floor(u).astype(int), 0, cells - 1); j = np.clip(np.floor(v).astype(int), 0, cells - 1)
    fu = smooth(u - i); fv = smooth(v - j)
    a = g[j, i]; b = g[j, i + 1]; c = g[j + 1, i]; d = g[j + 1, i + 1]
    return (a * (1 - fu) + b * fu) * (1 - fv) + (c * (1 - fu) + d * fu) * fv


def fbm(octaves=5, base=4, seed=0):
    rs = np.random.default_rng(seed)
    acc = np.zeros_like(x); amp = 0.5; tot = 0
    for o in range(octaves):
        acc += amp * value_noise(base * 2 ** o, rs); tot += amp; amp *= 0.5
    acc /= tot
    return (acc - acc.min()) / (acc.max() - acc.min())


def polar_noise(freq=8, seed=0):
    """Noise that wraps around the centre (for rings and bursts): sampled on (angle, radius)."""
    rs = np.random.default_rng(seed)
    g = rs.random((freq + 1, 9))
    g[-1, :] = g[0, :]
    u = (ang / (2 * math.pi) + 0.5) * freq; v = np.clip(r, 0, 1) * 8
    i = np.clip(np.floor(u).astype(int), 0, freq - 1); j = np.clip(np.floor(v).astype(int), 0, 7)
    fu = smooth(u - i); fv = smooth(v - j)
    a = g[i, j]; b = g[i + 1, j]; c = g[i, j + 1]; d = g[i + 1, j + 1]
    return (a * (1 - fu) + b * fu) * (1 - fv) + (c * (1 - fu) + d * fu) * fv


def erosion(field):
    f = (field - field.min()) / max(1e-6, field.max() - field.min())
    return 0.2 + 0.8 * f


def seg(ax, ay, bx, by, w):
    px, py = x - ax, y - ay; ex, ey = bx - ax, by - ay
    t = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0, 1)
    return np.hypot(px - ex * t, py - ey * t) - w


def line(d, width):
    return np.exp(-(d / width) ** 2)


def textures():
    T = {}
    F1, F2, F3 = fbm(5, 4, 1), fbm(5, 3, 2), fbm(4, 6, 3)
    P1 = polar_noise(10, 4)
    # 0 Glow: a soft round light; erodes into a noisy fringe
    glow = np.exp(-(r ** 2) * 4.5) * (r < 1)
    T["T_FX_Glow"] = (glow, erosion(0.6 * F1 + 0.4 * (1 - r)), np.exp(-(r ** 2) * 18))
    # 1 Spark: a star point with a cross
    spark = np.clip(np.exp(-(r ** 2) * 60) + 0.8 * np.exp(-(x ** 2) * 900 - (y ** 2) * 4) + 0.8 * np.exp(-(y ** 2) * 900 - (x ** 2) * 4), 0, 1)
    T["T_FX_Spark"] = (spark, np.ones_like(r), np.exp(-(r ** 2) * 80))
    # 2 Ring: a soft band with a faint fill, its edge wavering
    rr = r + 0.03 * (P1 - 0.5)
    ring = np.clip(np.exp(-((rr - 0.78) ** 2) * 160) + 0.14 * np.exp(-(r ** 2) * 2) * (r < 0.8), 0, 1)
    T["T_FX_Ring"] = (ring, erosion(0.55 * polar_noise(16, 5) + 0.45 * F2), np.exp(-((rr - 0.78) ** 2) * 900))
    # 3 Shock: a shock front -- a sharp bright outer edge over a gradient that fades toward the middle, torn by noise
    rr = r + 0.04 * (polar_noise(14, 6) - 0.5)
    shock = np.clip(np.where(rr < 0.9, (np.clip(rr, 0, 1) / 0.9) ** 3.5, np.exp(-((rr - 0.9) ** 2) * 700)), 0, 1) * (r < 1)
    T["T_FX_Shock"] = (shock, erosion(0.5 * polar_noise(24, 7) + 0.5 * F3), np.exp(-((rr - 0.9) ** 2) * 1500))
    # 4 Smoke: a domain-warped cloud, soft all round
    wx = x + 0.35 * (F1 - 0.5); wy = y + 0.35 * (F2 - 0.5)
    cloud = np.clip(1.15 - np.hypot(wx, wy) * 1.25, 0, 1) ** 1.3
    body = np.clip(cloud * (0.45 + 0.75 * F3), 0, 1)
    T["T_FX_Smoke"] = (body, erosion(0.7 * F3 + 0.3 * F1), body ** 3)
    # 5 Streak: a stretched soft dash (sparks drawn along their velocity)
    streak = np.exp(-(y ** 2) * 70 - (x ** 2) * 1.6)
    T["T_FX_Streak"] = (streak, np.ones_like(r), np.exp(-(y ** 2) * 300 - (x ** 2) * 4))
    # 6 Flare: a hot core with six thin rays and a halo
    rays = sum(np.exp(-((x * math.sin(a) - y * math.cos(a)) ** 2) * 1400) * np.exp(-(r ** 2) * 2.2) for a in np.arange(6) * math.pi / 6)
    flare = np.clip(np.exp(-(r ** 2) * 16) + 0.55 * rays + 0.22 * np.exp(-(r ** 2) * 3), 0, 1)
    T["T_FX_Flare"] = (flare, np.ones_like(r), np.exp(-(r ** 2) * 40))
    # 7 Rune: a magic circle -- two rings, a band of tick marks, a hexagram, an inner ring of dots
    rune = line(r - 0.93, 0.012) + line(r - 0.84, 0.008) + line(r - 0.55, 0.01)
    ticks = (np.abs(((ang / (2 * math.pi)) * 48) % 1 - 0.5) < 0.12) * (r > 0.85) * (r < 0.92)
    rune += ticks * 0.9
    for k in range(2):
        pts = [(0.84 * math.cos(math.pi / 2 + k * math.pi / 3 + i * 2 * math.pi / 3), 0.84 * math.sin(math.pi / 2 + k * math.pi / 3 + i * 2 * math.pi / 3)) for i in range(3)]
        for i in range(3):
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % 3]
            rune += line(seg(x0, y0, x1, y1, 0), 0.01)
    for k in range(12):
        a = k * math.pi / 6
        rune += np.exp(-(np.hypot(x - 0.7 * math.cos(a), y - 0.7 * math.sin(a)) / 0.03) ** 2)
    rune = np.clip(rune, 0, 1) * (r < 0.97)
    T["T_FX_Rune"] = (rune, erosion(0.5 * polar_noise(20, 8) + 0.5 * F1), rune ** 2)
    # 8 RingThin: a crisp thin circle (unit bases, selections, telegraphs)
    thin = np.clip(line(r - 0.9, 0.022) + 0.5 * line(r - 0.9, 0.06), 0, 1)
    T["T_FX_RingThin"] = (thin, np.ones_like(r), line(r - 0.9, 0.012))
    # 9 Flame: a tongue of fire, pointing up, its edge licked by noise
    fy = (y + 0.9) / 1.8                                    # 0 at the base .. 1 at the tip
    width = 0.68 * np.sqrt(np.clip(1 - fy, 0, 1)) * np.clip(fy * 4, 0, 1) ** 0.5
    wob = x + 0.18 * (F1 - 0.5) * fy
    flame = np.clip(1 - np.abs(wob) / np.maximum(width, 1e-3), 0, 1) ** 0.8 * (fy > 0) * (fy < 1)
    flame = np.clip(flame * (0.6 + 0.6 * F2), 0, 1)
    T["T_FX_Flame"] = (flame, erosion(0.6 * F3 + 0.4 * (1 - fy)), np.clip(flame * (1 - fy) * 1.6, 0, 1) ** 2)
    # 10 Swirl: three spiral arms around a bright eye (vortices, portals, pulls)
    arm = np.cos(3 * (ang - 4.2 * r))
    swirl = np.clip((arm * 0.5 + 0.5) ** 3 * np.clip(1 - r, 0, 1) ** 0.7 + np.exp(-(r ** 2) * 30), 0, 1) * (r < 1)
    T["T_FX_Swirl"] = (swirl, erosion(0.5 * F1 + 0.5 * r), np.exp(-(r ** 2) * 25))
    # 11 Slash: a crescent -- a thin bright blade edge with a softer trail inside, tapering to both ends (quad faces +X, the arc opens to -Y)
    ca = np.clip(1 - np.abs(ang - math.pi / 2) / 1.35, 0, 1)             # 0 at the ends, 1 in the middle of the arc (the top)
    edge = line(r - 0.82, 0.02 + 0.03 * ca) * ca ** 0.6
    trail = np.clip((r - 0.45) / 0.37, 0, 1) ** 2 * (r < 0.82) * ca ** 1.5 * 0.55
    slash = np.clip(edge + trail * (0.6 + 0.4 * F3), 0, 1) * (y > -0.2)
    T["T_FX_Slash"] = (slash, erosion(0.6 * F2 + 0.4 * (1 - ca)), edge)
    # 12 Star: a four-point glint with long thin rays
    star = np.clip(np.exp(-(r ** 2) * 50) + 0.9 * np.exp(-(x ** 2) * 2500 - (y ** 2) * 2.5) + 0.9 * np.exp(-(y ** 2) * 2500 - (x ** 2) * 2.5)
                   + 0.35 * np.exp(-((x - y) ** 2) * 1500 - ((x + y) ** 2) * 8) + 0.35 * np.exp(-((x + y) ** 2) * 1500 - ((x - y) ** 2) * 8), 0, 1)
    T["T_FX_Star"] = (star, np.ones_like(r), np.exp(-(r ** 2) * 120))
    # 13 Hex: a hexagon outline with a faint glow inside (tile highlights)
    hx = np.maximum(np.abs(x) * math.cos(math.pi / 6) + np.abs(y) * 0.5, np.abs(y))   # pointy-top hexagon "radius"
    hexo = np.clip(line(hx - 0.86, 0.02) + 0.25 * np.clip(1 - hx / 0.86, 0, 1) * (hx < 0.86) * 0.6, 0, 1)
    T["T_FX_Hex"] = (hexo, np.ones_like(r), line(hx - 0.86, 0.01))
    # 14 Burst: a fireball puff -- a noisy blob, hottest in the middle
    wr = np.hypot(x + 0.25 * (F2 - 0.5), y + 0.25 * (F1 - 0.5))
    burst = np.clip(1.1 - wr * 1.2, 0, 1) ** 0.8 * (0.55 + 0.6 * F3)
    burst = np.clip(burst, 0, 1)
    T["T_FX_Burst"] = (burst, erosion(0.65 * F3 + 0.35 * (1 - wr)), np.clip(1 - wr * 1.8, 0, 1) ** 2)
    # 15 Shadow: a contact shadow -- flat in the middle, soft at the edge
    shadow = np.clip((1 - r) / 0.45, 0, 1) ** 1.5
    T["T_FX_Shadow"] = (shadow, np.ones_like(r), np.zeros_like(r))
    # 16 Noise: three tileable fbm fields (R, G, B) that the effect MESH material pans over the surface in world space
    T["T_FX_Noise"] = (fbm(5, 4, 11), fbm(5, 6, 12), fbm(4, 8, 13))
    return T


def main():
    out = OUT
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out, exist_ok=True)
    for name, (rch, gch, bch) in textures().items():
        rgba = np.stack([np.clip(rch, 0, 1), np.clip(gch, 0, 1), np.clip(bch, 0, 1), np.ones_like(rch)], -1).astype(np.float32)
        img = bpy.data.images.new(name, N, N, alpha=True, float_buffer=False)
        img.colorspace_settings.name = "Non-Color"
        img.pixels.foreach_set(rgba[::-1].reshape(-1))
        img.filepath_raw = os.path.join(out, name + ".png"); img.file_format = "PNG"; img.save()
        bpy.data.images.remove(img)
        print("[fx] %s" % name)


main()
