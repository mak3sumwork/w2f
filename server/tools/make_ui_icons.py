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
    xs, ys = grid(S * SS)
    save("T_UI_Grad", np.clip((1.0 - ys) / 2.0, 0, 1) ** 1.4 * np.ones_like(xs))
    save("T_UI_Glow", np.clip(1.0 - r, 0, 1) ** 2)


main()
