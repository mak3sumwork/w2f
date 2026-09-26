"""The mouse cursors of the Unreal client (demo 1.4, FEEDBACK V4 "a cursor like the old League cursor"): a golden gauntlet arrow and a closed gauntlet (carrying a unit
or an item). Written straight into the Unreal project (<project>/Content/W2F/Cursor/*.png): the game loads them from disk as hardware cursors (AW2FArena::BeginPlay).

    ~/w2f_bpy/venv/bin/python tools/make_cursor.py

Original art: signed distance fields, 8x supersampled, a bronze-to-gold gradient lit from the top left, a dark outline and a bright bevel edge.
"""
import math, os
import numpy as np
import bpy

OUT = os.path.expanduser("~/Desktop/work2fightgame/work2fightgame/Content/W2F/Cursor")
N = 48          # pixels (the hardware cursor size)
SS = 8


def grid():
    n = N * SS
    c = (np.arange(n, dtype=np.float32) + 0.5) / n
    return np.meshgrid(c, c)          # x right, y DOWN (screen space), 0..1


def poly(x, y, pts):
    d = np.full(x.shape, -1e9, np.float32)
    n = len(pts)
    area = sum(pts[i][0] * pts[(i + 1) % n][1] - pts[(i + 1) % n][0] * pts[i][1] for i in range(n))
    if area < 0: pts = pts[::-1]
    for i in range(n):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        l = math.hypot(ex, ey); nx, ny = ey / l, -ex / l
        d = np.maximum(d, (x - ax) * nx + (y - ay) * ny)
    return d    # negative inside


def seg(x, y, a, b, r):
    ax, ay = a; bx, by = b
    px, py = x - ax, y - ay; ex, ey = bx - ax, by - ay
    t = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0, 1)
    return np.hypot(px - ex * t, py - ey * t) - r


def cov(d, soft=1.0):
    px = 1.0 / (N * SS)
    return np.clip(0.5 - d / (px * soft * SS * 0.5), 0.0, 1.0)


def down(a):
    return a.reshape(N, SS, N, SS, *a.shape[2:]).mean(axis=(1, 3))


def shade(x, y, d, inner):
    """Gold metal: a gradient lit from the top left, darker toward the lower right, a bright bevel just inside the edge, a dark outline."""
    t = np.clip(1.0 - (x * 0.55 + y * 0.75), 0, 1)
    dark = np.array([0.42, 0.26, 0.08]); mid = np.array([0.86, 0.63, 0.24]); hi = np.array([1.0, 0.92, 0.62])
    col = dark + (mid - dark) * np.clip(t * 1.6, 0, 1)[..., None]
    col = col + (hi - mid) * np.clip(t * 1.6 - 0.8, 0, 1)[..., None]
    bevel = np.exp(-((d + 0.035) ** 2) / 0.00012)          # a highlight line along the inside of the edge
    col = np.clip(col + bevel[..., None] * 0.35, 0, 1)
    col = np.where((inner < 0)[..., None], col * 0.72, col)   # the inner plate a shade darker
    body = cov(d + 0.028)                                    # the gold
    outline = cov(d)                                         # plus a dark rim
    rgb = col * body[..., None] + np.array([0.08, 0.05, 0.02]) * (outline - body)[..., None]
    return np.concatenate([rgb, outline[..., None]], -1)


def save(name, rgba):
    a = down(rgba)
    img = bpy.data.images.new(name, N, N, alpha=True)
    img.pixels.foreach_set(np.clip(a[::-1], 0, 1).astype(np.float32).reshape(-1))
    img.filepath_raw = os.path.join(OUT, name + ".png"); img.file_format = "PNG"; img.save()
    bpy.data.images.remove(img)
    print("wrote", os.path.join(OUT, name + ".png"))


def main():
    os.makedirs(OUT, exist_ok=True)
    x, y = grid()
    # the pointer: a gauntlet arrow -- the tip at the top left, a flared head, a notched armoured tail
    head = [(0.05, 0.03), (0.07, 0.74), (0.63, 0.53)]                          # tip, lower-left wing, right wing
    tail = [(0.22, 0.56), (0.39, 0.93), (0.53, 0.86), (0.37, 0.5)]              # the armoured tail, overlapping the head
    d = np.minimum(poly(x, y, head), poly(x, y, tail))
    inner = poly(x, y, [(0.13, 0.19), (0.14, 0.59), (0.47, 0.5)])               # the raised plate inside the head
    rgba = shade(x, y, d, inner)
    gem = np.hypot(x - 0.22, y - 0.42) - 0.045                  # a small blue gem in the plate
    g = cov(gem)
    rgba[..., :3] = rgba[..., :3] * (1 - g[..., None]) + np.array([0.35, 0.8, 1.0]) * g[..., None] * (0.7 + 0.3 * np.clip(1 - (x + y), 0, 1))[..., None]
    save("Pointer", rgba)
    # the grab cursor: a closed gauntlet -- a rounded fist with four knuckles and a thumb
    fist = np.minimum(np.maximum(np.abs(x - 0.5) - 0.26, np.abs(y - 0.55) - 0.2) - 0.06, 9.0)
    for k in range(4):
        fist = np.minimum(fist, np.hypot(x - (0.3 + k * 0.135), y - 0.33) - 0.085)
    fist = np.minimum(fist, seg(x, y, (0.2, 0.62), (0.4, 0.5), 0.07))
    knuckles = np.full(x.shape, 9.0, np.float32)
    for k in range(3):
        knuckles = np.minimum(knuckles, seg(x, y, (0.365 + k * 0.135, 0.28), (0.365 + k * 0.135, 0.45), 0.008))
    rgba = shade(x, y, fist, np.full(x.shape, 9.0, np.float32))
    lines = cov(knuckles)
    rgba[..., :3] = rgba[..., :3] * (1 - 0.6 * lines[..., None])
    save("Grab", rgba)


main()
