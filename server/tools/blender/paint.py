"""Hand-painted texture toolkit (numpy only): value noise / fBm, cellular (Voronoi) stones, painted bevel light, soft masks.

Images are float32 arrays shaped (H, W) or (H, W, 3), authored in display (sRGB) values 0..1, row 0 = the TOP of the picture. `to_image()` turns one into a packed Blender image
(Blender stores rows bottom-up). Everything is seeded, so a rebuild paints the same picture.
"""
import numpy as np


def rng(seed):
    return np.random.default_rng(seed)


def smooth(t):
    return t * t * (3.0 - 2.0 * t)


def smoothstep(a, b, x):
    return smooth(np.clip((x - a) / (b - a), 0.0, 1.0))


def value_noise(w, h, cells_x, cells_y, r, wrap=True):
    """Smooth value noise on a (h, w) grid with cells_x x cells_y lattice cells; `wrap` makes it tile."""
    g = r.random((cells_y + 1, cells_x + 1)).astype(np.float32)
    if wrap:
        g[:, -1] = g[:, 0]; g[-1, :] = g[0, :]
    xs = np.linspace(0, cells_x, w, endpoint=False, dtype=np.float32); ys = np.linspace(0, cells_y, h, endpoint=False, dtype=np.float32)
    x0 = np.floor(xs).astype(np.int32); y0 = np.floor(ys).astype(np.int32)
    fx = smooth(xs - x0); fy = smooth(ys - y0)
    a = g[y0][:, x0]; b = g[y0][:, x0 + 1]; c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
    top = a + (b - a) * fx[None, :]; bot = c + (d - c) * fx[None, :]
    return top + (bot - top) * fy[:, None]


def fbm(w, h, cells, octaves, r, gain=0.5, aspect=1.0, wrap=True):
    """Fractal noise 0..1; `cells` = lattice cells across the width of the first octave; `aspect` stretches the lattice (h cells = cells * h / w * aspect)."""
    out = np.zeros((h, w), np.float32); amp, total = 1.0, 0.0
    for o in range(octaves):
        cx = int(cells * 2 ** o); cy = max(1, int(round(cx * h / w * aspect)))
        out += amp * value_noise(w, h, cx, cy, r, wrap); total += amp; amp *= gain
    return out / total


def cells(w, h, nx, ny, r, jitter=0.8, square=0.0):
    """Cellular pattern: nx x ny jittered feature points (tiling). Returns (f1, f2, cell_id) with distances in cell units. `square` 0..1 blends Euclid towards
    Chebyshev distance, which makes squarer, flagstone-like cells."""
    px = (np.arange(nx)[None, :] + 0.5 + (r.random((ny, nx)) - 0.5) * jitter).astype(np.float32)
    py = (np.arange(ny)[:, None] + 0.5 + (r.random((ny, nx)) - 0.5) * jitter).astype(np.float32)
    xs = (np.arange(w, dtype=np.float32) + 0.5) / w * nx; ys = (np.arange(h, dtype=np.float32) + 0.5) / h * ny
    ix = np.floor(xs).astype(np.int32); iy = np.floor(ys).astype(np.int32)
    f1 = np.full((h, w), 9.0, np.float32); f2 = np.full((h, w), 9.0, np.float32); cid = np.zeros((h, w), np.int32)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cx = ix + dx; cy = iy + dy
            wx = np.mod(cx, nx); wy = np.mod(cy, ny)
            fpx = px[wy[:, None], wx[None, :]] + (cx - wx)[None, :]
            fpy = py[wy[:, None], wx[None, :]] + (cy - wy)[:, None]
            ddx = np.abs(xs[None, :] - fpx); ddy = np.abs(ys[:, None] - fpy)
            d = np.sqrt(ddx * ddx + ddy * ddy) * (1.0 - square) + np.maximum(ddx, ddy) * square
            ids = (wy[:, None] * nx + wx[None, :]).astype(np.int32)
            closer = d < f1
            f2 = np.where(closer, f1, np.minimum(f2, d)); cid = np.where(closer, ids, cid); f1 = np.where(closer, d, f1)
    return f1, f2, cid


def bevel_light(height, strength=1.0, light=(-0.6, -0.7)):
    """Painted light from a height field: lit where the surface faces the light (top-left by default), dark where it faces away. Returns -1..1."""
    gy, gx = np.gradient(height)
    lx, ly = light
    n = np.hypot(lx, ly)
    return np.clip(-(gx * lx + gy * ly) / n * strength, -1.0, 1.0)


def blend(a, b, t):
    t = t[..., None] if a.ndim == 3 and t.ndim == 2 else t
    return a * (1.0 - t) + b * t


def col(rgb, h, w):
    return np.broadcast_to(np.array(rgb, np.float32), (h, w, 3)).copy()


def palette(values, stops):
    """Maps values 0..1 onto a list of (position, rgb) colour stops."""
    pos = np.array([p for p, _ in stops], np.float32); cs = np.array([c for _, c in stops], np.float32)
    out = np.empty(values.shape + (3,), np.float32)
    for k in range(3):
        out[..., k] = np.interp(values, pos, cs[:, k])
    return out


def to_image(name, img, pack=True):
    """A packed Blender image from an (H, W, 3) display-value array."""
    import bpy
    h, w = img.shape[:2]
    im = bpy.data.images.new(name, w, h, alpha=False)
    rgba = np.concatenate([np.clip(img[::-1], 0, 1), np.ones((h, w, 1), np.float32)], axis=2)
    im.pixels.foreach_set(rgba.astype(np.float32).reshape(-1))
    if pack:
        im.pack()
    return im


def save_png(path, img):
    """Writes an (H, W, 3) array as a PNG with Blender (no PIL in the bpy venv)."""
    import bpy
    im = to_image("tmp_save", img, pack=False)
    im.filepath_raw = path; im.file_format = "PNG"; im.save()
    bpy.data.images.remove(im)
