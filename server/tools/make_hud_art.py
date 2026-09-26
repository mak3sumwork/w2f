"""The TFT-style HUD art of the Unreal client (demo 1.3), next to the glyphs of make_ui_icons.py: docs/icons/T_UI_*.png.

    ~/w2f_bpy/venv/bin/python tools/make_hud_art.py

* T_UI_Trait_<Name>: one white emblem per trait (data/traits.json), tinted in Slate: black on a lit badge, grey on an inactive one.
* T_UI_Badge<Tier>: the hexagonal trait badge in its metal -- None (dark), Bronze, Silver, Gold, Prismatic (iridescent) -- lit from above with a bevelled rim.
* T_UI_Planner (a clipboard: the Team Planner), T_UI_Shine (a diagonal highlight for hovered cards), T_UI_Vignette (a soft edge fade for panels),
  T_UI_Frame (a thin ornate gold frame, 9-sliced in Slate), T_UI_Orb (a glassy item orb).
Everything is drawn from signed distance fields with numpy, 4x supersampled, and saved with Blender's image writer (bpy venv: no PIL).
"""
import math, os
import numpy as np
import bpy

OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "icons"))
S = 128
SS = 4


def grid(n=S * SS):
    c = (np.arange(n, dtype=np.float32) + 0.5) / n * 2.0 - 1.0
    return np.meshgrid(c, -c)


def fill(d, soft=0.0):
    px = 2.0 / (S * SS)
    return np.clip(0.5 - d / (px * (1.0 + soft * 40)), 0.0, 1.0)


def down(a):
    n = a.shape[0] // SS
    return a.reshape(n, SS, n, SS, *a.shape[2:]).mean(axis=(1, 3))


def poly(x, y, pts):
    d = np.full(x.shape, -1e9, np.float32)
    n = len(pts)
    area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n))
    if area < 0:
        pts = pts[::-1]
    for i in range(n):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        l = math.hypot(ex, ey); nx, ny = ey / l, -ex / l
        d = np.maximum(d, (x - ax) * nx + (y - ay) * ny)
    return d


def seg(x, y, a, b, r):
    ax, ay = a; bx, by = b
    px, py = x - ax, y - ay; ex, ey = bx - ax, by - ay
    t = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0, 1)
    return np.hypot(px - ex * t, py - ey * t) - r


def circ(x, y, cx, cy, r):
    return np.hypot(x - cx, y - cy) - r


def ngon(n, r, rot=0.0, cx=0.0, cy=0.0):
    return [(cx + r * math.cos(rot + 2 * math.pi * k / n), cy + r * math.sin(rot + 2 * math.pi * k / n)) for k in range(n)]


def star(n, r0, r1, rot=math.pi / 2, cx=0.0, cy=0.0, x=None, y=None):
    pts = []
    for k in range(2 * n):
        a = rot + k * math.pi / n
        rr = r0 if k % 2 == 0 else r1
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d = np.full(x.shape, 9.0, np.float32)
    for k in range(n):
        d = np.minimum(d, poly(x, y, [pts[(2 * k - 1) % (2 * n)], pts[2 * k], pts[(2 * k + 1) % (2 * n)]]))
    return np.minimum(d, poly(x, y, [pts[(2 * k + 1) % (2 * n)] for k in range(n)]))


def U(*ds):
    out = ds[0]
    for d in ds[1:]:
        out = np.minimum(out, d)
    return out


def cut(a, b):
    return np.maximum(a, -b)


def save(name, rgba, size=None):
    a = down(rgba) if rgba.shape[0] > S * 1.5 else rgba
    h, w = a.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=True)
    img.pixels.foreach_set(np.clip(a[::-1], 0, 1).astype(np.float32).reshape(-1))
    img.filepath_raw = os.path.join(OUT, name + ".png"); img.file_format = "PNG"; img.save()
    bpy.data.images.remove(img)
    print("wrote", name)


def mask(name, d):
    a = fill(d)
    save(name, np.stack([np.ones_like(a), np.ones_like(a), np.ones_like(a), a], -1))


# ---------------------------------------------------------------------------------------------------------------- trait emblems

def emblems(x, y):
    r = np.hypot(x, y)
    ang = np.arctan2(y, x)
    E = {}
    # Helios: a sun -- a disc and eight rays
    rays = np.full(x.shape, 9.0, np.float32)
    for k in range(8):
        a = k * math.pi / 4
        ca, sa = math.cos(a), math.sin(a)
        rays = np.minimum(rays, poly(x, y, [(0.5 * ca - 0.13 * sa, 0.5 * sa + 0.13 * ca), (0.92 * ca, 0.92 * sa), (0.5 * ca + 0.13 * sa, 0.5 * sa - 0.13 * ca)]))
    E["Helios"] = U(circ(x, y, 0, 0, 0.4), rays)
    # Phaisa: the Void's claw -- three curved slashes
    cl = np.full(x.shape, 9.0, np.float32)
    for k, off in enumerate((-0.42, 0.0, 0.42)):
        c = circ(x, y, off + 0.9, -0.2, 1.05)
        ring = np.abs(c) - (0.1 - 0.02 * abs(k - 1))
        cl = np.minimum(cl, np.maximum(ring, np.maximum(np.abs(y) - 0.78, x - (off + 0.35))))
    E["Phaisa"] = cl
    # Hexagon: a gear around a hexagonal hole
    teeth = np.full(x.shape, 9.0, np.float32)
    for k in range(10):
        a = k * 2 * math.pi / 10
        ca, sa = math.cos(a), math.sin(a)
        teeth = np.minimum(teeth, poly(x, y, [(0.55 * ca - 0.14 * sa, 0.55 * sa + 0.14 * ca), (0.9 * ca - 0.1 * sa, 0.9 * sa + 0.1 * ca), (0.9 * ca + 0.1 * sa, 0.9 * sa - 0.1 * ca), (0.55 * ca + 0.14 * sa, 0.55 * sa - 0.14 * ca)]))
    E["Hexagon"] = cut(U(circ(x, y, 0, 0, 0.66), teeth), poly(x, y, ngon(6, 0.34, math.pi / 6)))
    # Selini: a crescent moon and a small star
    E["Selini"] = U(cut(circ(x, y, -0.05, 0, 0.78), circ(x, y, 0.3, 0.18, 0.66)), star(4, 0.26, 0.07, cx=0.45, cy=-0.35, x=x, y=y))
    # Najmi: a four-point star with a smaller one
    E["Najmi"] = U(star(4, 0.9, 0.2, cx=-0.1, cy=0.05, x=x, y=y), star(4, 0.35, 0.09, cx=0.58, cy=0.55, x=x, y=y))
    # Nature: a leaf with its vein
    leaf = np.maximum(circ(x, y, 0.45, -0.45, 1.05), circ(x, y, -0.45, 0.45, 1.05))
    E["Nature"] = U(cut(leaf, seg(x, y, (-0.55, -0.55), (0.45, 0.45), 0.05)), seg(x, y, (-0.9, -0.9), (-0.55, -0.55), 0.07))
    # Assassin: a dagger, point down
    blade = poly(x, y, [(0.0, -0.95), (0.16, -0.25), (0.13, 0.3), (-0.13, 0.3), (-0.16, -0.25)])
    guard = seg(x, y, (-0.42, 0.36), (0.42, 0.36), 0.08)
    grip = U(seg(x, y, (0, 0.42), (0, 0.78), 0.08), circ(x, y, 0, 0.86, 0.1))
    E["Assassin"] = U(cut(blade, seg(x, y, (0, -0.7), (0, 0.25), 0.025)), guard, grip)
    # Omnilium: an orb in a tilted orbit
    ex_, ey_ = x * math.cos(0.5) + y * math.sin(0.5), -x * math.sin(0.5) + y * math.cos(0.5)
    orbit = np.abs(np.hypot(ex_ / 0.95, ey_ / 0.36) - 1.0) * 0.36 - 0.05
    E["Omnilium"] = U(circ(x, y, 0, 0, 0.42), cut(orbit, circ(x, y, 0, 0, 0.5)), circ(x, y, 0.68, 0.46, 0.1))
    # Coregons: a faceted crystal
    gem = poly(x, y, [(0, 0.95), (0.42, 0.45), (0.42, -0.35), (0, -0.95), (-0.42, -0.35), (-0.42, 0.45)])
    facets = U(seg(x, y, (0, 0.95), (0, -0.95), 0.03), seg(x, y, (-0.42, 0.45), (0, 0.2), 0.03), seg(x, y, (0.42, 0.45), (0, 0.2), 0.03))
    E["Coregons"] = cut(gem, facets)
    # Protector: a kite shield with a cross cut in it
    shield = np.maximum(np.maximum(np.abs(x) - 0.72, y - 0.8), np.where(y < 0.15, np.hypot(x / 0.72, (y - 0.15) / 1.0) - 1.0, -1.0))
    E["Protector"] = cut(shield, U(seg(x, y, (0, 0.5), (0, -0.5), 0.09), seg(x, y, (-0.36, 0.12), (0.36, 0.12), 0.09)))
    # Bastion: a tower with battlements and a gate
    tower = np.maximum(np.abs(x) - 0.55, np.abs(y + 0.1) - 0.75)
    notches = U(*[np.maximum(np.abs(x - cx) - 0.1, np.abs(y - 0.72) - 0.12) for cx in (-0.28, 0.0, 0.28)])
    gate = U(np.maximum(np.abs(x) - 0.18, np.abs(y + 0.62) - 0.24), circ(x, y, 0, -0.38, 0.18))
    E["Bastion"] = cut(cut(tower, notches), gate)
    # Bruiser: a war hammer
    head = poly(x, y, [(-0.62, 0.34), (0.62, 0.34), (0.55, 0.82), (-0.55, 0.82)])
    E["Bruiser"] = U(head, seg(x, y, (0, 0.34), (0, -0.92), 0.1), circ(x, y, 0, -0.92, 0.12))
    # Sorcerer: a staff with a glowing orb and sparks
    E["Sorcerer"] = U(seg(x, y, (-0.7, -0.9), (0.3, 0.3), 0.08), cut(circ(x, y, 0.45, 0.48, 0.34), circ(x, y, 0.45, 0.48, 0.16)), star(4, 0.22, 0.05, cx=-0.45, cy=0.55, x=x, y=y), star(4, 0.16, 0.04, cx=0.72, cy=-0.25, x=x, y=y))
    # Marksman: a crosshair
    E["Marksman"] = U(np.abs(r - 0.62) - 0.07, seg(x, y, (0, 0.95), (0, 0.4), 0.07), seg(x, y, (0, -0.95), (0, -0.4), 0.07), seg(x, y, (0.95, 0), (0.4, 0), 0.07), seg(x, y, (-0.95, 0), (-0.4, 0), 0.07), circ(x, y, 0, 0, 0.12))
    # Mystic: an eye
    lens = np.maximum(circ(x, y, 0, -0.62, 0.98), circ(x, y, 0, 0.62, 0.98))
    E["Mystic"] = U(cut(lens, circ(x, y, 0, 0, 0.36)), circ(x, y, 0, 0, 0.2))
    # Duelist: crossed rapiers
    sw = np.full(x.shape, 9.0, np.float32)
    for s in (-1, 1):
        sw = U(sw, seg(x, y, (-0.55 * s, -0.55), (0.82 * s, 0.82), 0.06), seg(x, y, (-0.62 * s - 0.2, -0.62 + 0.2 * s), (-0.62 * s + 0.2, -0.62 - 0.2 * s), 0.06), circ(x, y, -0.8 * s, -0.8, 0.1))
    E["Duelist"] = sw
    # Gunslinger: a revolver
    gun = U(np.maximum(np.abs(x - 0.18) - 0.62, np.abs(y - 0.35) - 0.11), circ(x, y, -0.2, 0.25, 0.24),
            poly(x, y, [(-0.5, 0.3), (-0.2, 0.2), (-0.35, -0.75), (-0.72, -0.62)]), np.maximum(np.abs(x - 0.72) - 0.05, np.abs(y - 0.5) - 0.07))
    E["Gunslinger"] = cut(gun, U(circ(x, y, -0.2, 0.25, 0.08), np.maximum(np.abs(x + 0.08) - 0.12, np.abs(y + 0.02) - 0.1)))
    # Hexa: a hex outline with a lightning bolt
    bolt = U(poly(x, y, [(0.2, 0.66), (-0.3, -0.06), (0.06, -0.06)]), poly(x, y, [(-0.06, 0.08), (0.3, 0.08), (-0.2, -0.66)]))   # two triangles (poly() is convex only)
    E["Hexa"] = U(np.abs(poly(x, y, ngon(6, 0.9, math.pi / 2))) - 0.07, bolt)
    # Plant: a sprout
    l1 = np.maximum(circ(x, y, -0.05, 0.05, 0.55), circ(x, y, -0.55, 0.55, 0.55))
    l2 = np.maximum(circ(x, y, 0.05, 0.25, 0.5), circ(x, y, 0.5, -0.15, 0.5))
    E["Plant"] = U(seg(x, y, (0, -0.9), (0, 0.3), 0.07), l1, l2, np.maximum(np.abs(x) - 0.5, np.abs(y + 0.88) - 0.07))
    return E


# ---------------------------------------------------------------------------------------------------------------- badges

METALS = {
    "None": ((0.06, 0.07, 0.08), (0.17, 0.18, 0.2), (0.3, 0.32, 0.34)),
    "Bronze": ((0.36, 0.2, 0.1), (0.8, 0.5, 0.3), (1.0, 0.8, 0.6)),
    "Silver": ((0.38, 0.43, 0.48), (0.78, 0.83, 0.88), (1.0, 1.0, 1.0)),
    "Gold": ((0.5, 0.33, 0.06), (0.98, 0.78, 0.3), (1.0, 0.97, 0.75)),
}


def badge(x, y, name):
    d = poly(x, y, ngon(6, 0.97, math.pi / 2))                     # pointy top, like TFT's
    rim = 0.13
    inside = d < -rim
    t = np.clip((y + 1) / 2, 0, 1)                                  # 0 bottom .. 1 top
    if name == "Prismatic":
        hue = (np.arctan2(y - 0.3, x) / (2 * math.pi) + 0.5 + 0.35 * t) % 1.0
        k = np.stack([np.abs(((hue * 6 + o) % 6) - 3) - 1 for o in (0, 4, 2)], -1)
        col = np.clip(k, 0, 1) * 0.45 + 0.55                         # pastel iridescence
        face = col * (0.72 + 0.28 * t)[..., None]
        edge = np.clip(col * 0.5 + 0.55, 0, 1) * (0.6 + 0.4 * t)[..., None]
    else:
        dark, mid, hi = (np.array(c, np.float32) for c in METALS[name])
        face = dark + (mid - dark) * (t ** 0.8)[..., None]
        edge = mid + (hi - mid) * (t ** 1.5)[..., None]
        edge = np.where((t < 0.45)[..., None], dark * 0.9 + (mid - dark) * 0.6, edge)   # the lower rim in shadow
    col = np.where(inside[..., None], face, edge)
    ridge = np.exp(-((d + rim) ** 2) / 0.0006)                      # a bright line where rim meets face
    col = np.clip(col + ridge[..., None] * (0.25 if name != "None" else 0.1), 0, 1)
    sheen = np.clip(1 - np.abs(x + y * 0.4 - 0.1) * 1.6, 0, 1) ** 3 * (inside * 0.12)   # a diagonal gloss
    col = np.clip(col + sheen[..., None], 0, 1)
    outline = fill(d)                                              # a dark hairline around everything
    body = fill(d + 0.035)
    rgb = col * body[..., None]
    return np.concatenate([rgb, outline[..., None]], -1)


def main():
    os.makedirs(OUT, exist_ok=True)
    x, y = grid()
    for name, d in emblems(x, y).items():
        mask("T_UI_Trait_" + name, d)
    for name in ("None", "Bronze", "Silver", "Gold", "Prismatic"):
        save("T_UI_Badge" + name, badge(x, y, name))
    # the Team Planner: a clipboard
    board = np.abs(np.maximum(np.abs(x) - 0.62, np.abs(y + 0.08) - 0.8)) - 0.07
    clip = np.maximum(np.abs(x) - 0.3, np.abs(y - 0.76) - 0.14)
    lines = U(*[seg(x, y, (-0.36, yy), (0.36, yy), 0.055) for yy in (0.28, -0.05, -0.38)])
    mask("T_UI_Planner", U(cut(board, np.maximum(np.abs(x) - 0.36, np.abs(y - 0.72) - 0.2)), clip, lines))
    # a diagonal highlight (hovered cards), a soft vignette (panel shading), a glassy orb (item orbs), a 9-slice gold frame
    n = S * SS
    xs, ys = grid(n)
    band = np.exp(-((xs * 0.8 + ys * 0.6) ** 2) / 0.05)
    save("T_UI_Shine", np.stack([np.ones_like(band)] * 3 + [band * 0.9], -1))
    vig = np.clip(1.0 - np.maximum(np.abs(xs), np.abs(ys)) ** 6, 0, 1)
    save("T_UI_Vignette", np.stack([np.zeros_like(vig)] * 3 + [1 - vig], -1))
    r = np.hypot(xs, ys)
    orb_a = fill(r - 0.95)
    glass = np.clip(0.25 + 0.75 * np.exp(-(np.hypot(xs + 0.3, ys - 0.35) ** 2) / 0.06), 0, 1)
    rim = np.exp(-((r - 0.9) ** 2) / 0.004)
    shade = np.clip(0.08 + 0.35 * (1 - r) + glass * 0.9 + rim * 0.6, 0, 1)
    save("T_UI_Orb", np.stack([shade * 0.95, shade * 0.9, shade * 0.8, orb_a * np.clip(0.35 + glass * 0.65 + rim, 0, 1)], -1))
    fr_d = np.maximum(np.abs(xs), np.abs(ys)) - 0.97
    frame_line = fill(np.abs(fr_d + 0.03) - 0.018)
    inner_line = fill(np.abs(fr_d + 0.1) - 0.008) * 0.6
    corners = U(*[circ(xs, ys, sx * 0.87, sy * 0.87, 0.07) for sx in (-1, 1) for sy in (-1, 1)])
    fa = np.clip(frame_line + inner_line + fill(corners), 0, 1)
    gold = np.stack([np.full_like(fa, 1.0), np.full_like(fa, 0.84), np.full_like(fa, 0.5)], -1) * (0.7 + 0.3 * np.clip((ys + 1) / 2, 0, 1))[..., None]
    save("T_UI_Frame", np.concatenate([gold, fa[..., None]], -1))


main()
