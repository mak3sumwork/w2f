"""The UI glyphs of the Unreal client (docs/icons/T_UI_*.png): white shapes on transparent, tinted in Slate. Drawn from signed distance fields with numpy,
4x supersampled; saved with Blender's image writer (no PIL in the bpy venv).

    ~/w2f_bpy/venv/bin/python tools/make_ui_icons.py

T_UI_Hex / HexRim (trait badges), Circle / Ring (avatars, odds dots), Tri / Diamond / Pent (odds glyphs of the higher tiers), Chevron (buy XP), Reroll, Clock, Swords (a PvP round),
Monster (a PvE round), Leaf (a Mother Nature round), Flame (win / loss streak), Grad (a vertical fade for the shop cards), Glow (a soft radial light).
"""
import math, os
import numpy as np
import bpy

OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "icons"))
S = 128          # output size
SS = 4           # supersampling


def grid(n=S * SS):
    c = (np.arange(n, dtype=np.float32) + 0.5) / n * 2.0 - 1.0          # -1 .. 1, y up
    return np.meshgrid(c, -c)


def fill(d, soft=0.0):
    """Coverage of a signed distance (negative inside); `soft` widens the edge."""
    px = 2.0 / (S * SS)
    return np.clip(0.5 - d / (px * (1.0 + soft * 40)), 0.0, 1.0)


def down(a):
    n = a.shape[0] // SS
    return a.reshape(n, SS, n, SS, *a.shape[2:]).mean(axis=(1, 3))


def poly(x, y, pts):
    """Signed distance to a convex polygon (counter-clockwise points)."""
    d = np.full(x.shape, -1e9, np.float32)
    n = len(pts)
    for i in range(n):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        l = math.hypot(ex, ey); nx, ny = ey / l, -ex / l                   # outward normal of a CCW edge
        d = np.maximum(d, (x - ax) * nx + (y - ay) * ny)
    return d


def seg(x, y, a, b, r):
    ax, ay = a; bx, by = b
    px, py = x - ax, y - ay; ex, ey = bx - ax, by - ay
    t = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0, 1)
    return np.hypot(px - ex * t, py - ey * t) - r


def ngon(n, r, rot=0.0):
    return [(r * math.cos(rot + 2 * math.pi * k / n), r * math.sin(rot + 2 * math.pi * k / n)) for k in range(n)]


def save(name, alpha, shade=None):
    a = down(alpha)
    rgb = np.ones(a.shape + (3,), np.float32) if shade is None else np.repeat(down(shade)[..., None], 3, axis=2)
    h, w = a.shape
    img = bpy.data.images.new(name, w, h, alpha=True)
    rgba = np.concatenate([rgb, a[..., None]], axis=2)[::-1]
    img.pixels.foreach_set(rgba.astype(np.float32).reshape(-1))
    img.filepath_raw = os.path.join(OUT, name + ".png"); img.file_format = "PNG"; img.save()
    bpy.data.images.remove(img)
    print("wrote", name)


def main():
    os.makedirs(OUT, exist_ok=True)
    x, y = grid()
    hexp = ngon(6, 0.96, 0.0)                                               # flat top: a vertex on the right
    d = poly(x, y, hexp)
    shade = np.clip(0.86 + 0.14 * y, 0, 1) * np.where(d > -0.1, 0.72, 1.0)  # lit from above, a darker rim
    save("T_UI_Hex", fill(d), shade)
    save("T_UI_HexRim", fill(np.maximum(d, -(d + 0.12))))
    r = np.hypot(x, y)
    save("T_UI_Circle", fill(r - 0.96))
    save("T_UI_Ring", fill(np.maximum(r - 0.96, -(r - 0.8))))
    save("T_UI_Tri", fill(poly(x, y, ngon(3, 0.95, math.pi / 2)) + 0.0))
    save("T_UI_Diamond", fill(np.abs(x) * 1.0 + np.abs(y) * 0.8 - 0.8))
    save("T_UI_Pent", fill(poly(x, y, ngon(5, 0.95, math.pi / 2))))
    ch = np.minimum(np.minimum(seg(x, y, (-0.6, -0.15), (0.0, 0.45), 0.12), seg(x, y, (0.0, 0.45), (0.6, -0.15), 0.12)),
                    np.minimum(seg(x, y, (-0.6, -0.65), (0.0, -0.05), 0.12), seg(x, y, (0.0, -0.05), (0.6, -0.65), 0.12)))
    save("T_UI_Chevron", fill(ch))
    ang = np.arctan2(y, x)
    arc = np.where((ang > -2.4) & (ang < 2.6), np.abs(r - 0.62) - 0.13, 9.0)
    head = poly(x, y, [(-0.53, -0.12), (-0.18, 0.34), (-0.88, 0.34)])        # the arrowhead at the arc's upper-left end, pointing on down the circle
    save("T_UI_Reroll", fill(np.minimum(arc, head)))
    clock = np.maximum(r - 0.95, -(r - 0.78))
    clock = np.minimum(clock, np.minimum(seg(x, y, (0, 0), (0, 0.55), 0.09), seg(x, y, (0, 0), (0.4, -0.1), 0.09)))
    save("T_UI_Clock", fill(clock))
    sw = 9.0
    for s in (-1, 1):
        a, b = (-0.72 * s, -0.72), (0.62 * s, 0.62)
        sw = np.minimum(sw, seg(x, y, a, b, 0.09))
        g0 = (-0.45 * s - 0.2, -0.45 + 0.2 * s); g1 = (-0.45 * s + 0.2, -0.45 - 0.2 * s)
        sw = np.minimum(sw, seg(x, y, (-0.5 * s - 0.22 * s, -0.5 + 0.22), (-0.5 * s + 0.22 * s, -0.5 - 0.22), 0.07))
    save("T_UI_Swords", fill(sw))
    face = r - 0.62
    horns = np.minimum(seg(x, y, (-0.45, 0.35), (-0.8, 0.9), 0.11), seg(x, y, (0.45, 0.35), (0.8, 0.9), 0.11))
    eyes = np.minimum(np.hypot(x + 0.25, y - 0.05) - 0.13, np.hypot(x - 0.25, y - 0.05) - 0.13)
    mouth = poly(x, y, [(-0.3, -0.25), (0.3, -0.25), (0.0, -0.45)][::-1])
    save("T_UI_Monster", fill(np.maximum(np.minimum(face, horns), -np.minimum(eyes, mouth))))
    leaf = np.maximum(np.hypot(x - 0.45, y + 0.45) - 1.05, np.hypot(x + 0.45, y - 0.45) - 1.05)
    vein = seg(x, y, (-0.55, -0.55), (0.5, 0.5), 0.05)
    save("T_UI_Leaf", fill(np.maximum(leaf, -vein)))
    fl = np.minimum(np.hypot(x, y + 0.3) - 0.55, poly(x, y, [(0.05, 0.98), (-0.5, -0.15), (0.5, -0.15)]))
    save("T_UI_Flame", fill(fl))
    # rank chevron (pointing left) and a five-point star, for the unit plates
    chev = np.minimum(seg(x, y, (0.35, 0.75), (-0.35, 0.0), 0.2), seg(x, y, (-0.35, 0.0), (0.35, -0.75), 0.2))
    save("T_UI_ChevLeft", fill(chev))
    star = []
    for k in range(10):
        a_ = math.pi / 2 + k * math.pi / 5
        rr = 0.95 if k % 2 == 0 else 0.4
        star.append((rr * math.cos(a_), rr * math.sin(a_)))
    tri_d = np.full(x.shape, 9.0, np.float32)
    for k in range(5):   # a star = the union of five triangles (outer point + the two inner points beside it) and the inner pentagon
        o = star[2 * k]; l = star[(2 * k - 1) % 10]; r_ = star[(2 * k + 1) % 10]
        tri_d = np.minimum(tri_d, poly(x, y, [l, o, r_]))
    tri_d = np.minimum(tri_d, poly(x, y, [star[(2 * k + 1) % 10] for k in range(5)]))
    save("T_UI_Star", fill(tri_d))
    # stat icons for the unit panel: attack damage (sword), ability power (swirl star), armor (shield), magic resist (shield with a rune),
    # attack speed (double arrow), crit (burst), range (bow arrow), mana (drop)
    sword = np.minimum(np.minimum(seg(x, y, (-0.6, -0.6), (0.65, 0.65), 0.1), seg(x, y, (-0.55, -0.15), (-0.15, -0.55), 0.09)), seg(x, y, (-0.8, -0.8), (-0.62, -0.62), 0.12))
    save("T_UI_StatAD", fill(sword))
    ap = np.maximum(np.abs(r - 0.55) - 0.12, -(np.abs(np.arctan2(y, x) * 0.0) - 1)) 
    swirl = np.full(x.shape, 9.0, np.float32)
    for k in range(3):
        a0 = k * 2 * math.pi / 3
        pts = [(0.15 * math.cos(a0 + t) * (1 + t), 0.15 * math.sin(a0 + t) * (1 + t)) for t in np.linspace(0, 2.4, 10)]
        for i_ in range(len(pts) - 1): swirl = np.minimum(swirl, seg(x, y, pts[i_], pts[i_ + 1], 0.08))
    save("T_UI_StatAP", fill(swirl))
    shield = np.maximum(np.maximum(np.abs(x) - 0.7, y - 0.75), np.hypot(x, np.minimum(y, 0) * 0.9 + 0.0) - 0.8)
    shield = np.maximum(np.maximum(np.abs(x) - 0.7, y - 0.75), np.where(y < 0.1, np.hypot(x / 0.7, (y - 0.1) / 0.95) - 1.0, -1.0))
    save("T_UI_StatArmor", fill(shield))
    rune = np.maximum(np.hypot(x, y - 0.12) - 0.3, -(np.hypot(x, y - 0.12) - 0.16))
    save("T_UI_StatMR", fill(np.maximum(shield, -rune)))
    arrows = np.minimum(np.minimum(seg(x, y, (-0.7, 0.3), (0.2, 0.3), 0.09), seg(x, y, (-0.7, -0.3), (0.2, -0.3), 0.09)),
                        np.minimum(poly(x, y, [(0.8, 0.3), (0.2, 0.62), (0.2, -0.02)]), poly(x, y, [(0.8, -0.3), (0.2, 0.02), (0.2, -0.62)])))
    save("T_UI_StatAS", fill(arrows))
    burst = []
    for k in range(16):
        a_ = k * math.pi / 8
        rr = 0.95 if k % 2 == 0 else 0.45
        burst.append((rr * math.cos(a_), rr * math.sin(a_)))
    bd = np.full(x.shape, 9.0, np.float32)
    for k in range(8):
        bd = np.minimum(bd, poly(x, y, [burst[(2 * k - 1) % 16], burst[2 * k], burst[(2 * k + 1) % 16]]))
    bd = np.minimum(bd, r - 0.47)
    save("T_UI_StatCrit", fill(bd))
    rng_ = np.minimum(seg(x, y, (-0.7, -0.7), (0.5, 0.5), 0.08), poly(x, y, [(0.85, 0.85), (0.25, 0.65), (0.65, 0.25)]))
    rng_ = np.minimum(rng_, np.minimum(seg(x, y, (-0.7, -0.7), (-0.85, -0.4), 0.07), seg(x, y, (-0.7, -0.7), (-0.4, -0.85), 0.07)))
    save("T_UI_StatRange", fill(rng_))
    drop = np.minimum(np.hypot(x, y + 0.25) - 0.55, poly(x, y, [(0.0, 0.95), (-0.48, 0.0), (0.48, 0.0)]))
    save("T_UI_StatMana", fill(drop))
    heart = np.minimum(np.minimum(np.hypot(x + 0.3, y - 0.25) - 0.38, np.hypot(x - 0.3, y - 0.25) - 0.38), poly(x, y, [(0.0, -0.85), (0.66, 0.1), (-0.66, 0.1)]))
    save("T_UI_StatHP", fill(heart))
    xs, ys = grid(S * SS)
    save("T_UI_Grad", np.clip((1.0 - ys) / 2.0, 0, 1) ** 1.4 * np.ones_like(xs))
    save("T_UI_Glow", np.clip(1.0 - r, 0, 1) ** 2)


main()
