"""TFT-style item icons (docs/icons/T_Item_<id>.png, 128 x 128): one big painted object per item -- bevelled metal, cut gems, wood --, lit from the top left
with a specular highlight and a dark outline, over a glowing background in the item's colour, in a bevelled frame (steel for components, gold for
completed items, the trait's colour for emblems). Replaces the glyph icons of make_icons.py for the items. numpy only; deterministic.

    python3 tools/make_item_icons.py [--only 3,11]

What an item looks like follows its recipe: a completed item takes the SHAPE of its more "object-like" component (a sword beats a bow beats a staff ...)
and the COLOUR of the other one (gems, glow, background); a small variation per item id keeps two sword items apart (blade shape, gem cut).
"""
import json, math, os, re, struct, sys, zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.normpath(os.path.join(HERE, "..", "data", "items.json"))
OUT = os.path.normpath(os.path.join(HERE, "..", "docs", "icons"))
N = 256            # drawn at 256, saved at 128
PX = 2.0 / N

# What each component is, and its colour (display sRGB 0..1)
COMPONENT = {
    1: ("helmet", (0.7, 0.72, 0.82)),    # Omnilium's Helmet: armor + magic resist
    2: ("water", (0.3, 0.6, 1.0)),       # Selini's Water: mana
    3: ("sword", (1.0, 0.5, 0.25)),      # Coregons Sword: attack damage
    4: ("vest", (0.95, 0.75, 0.35)),     # Omnilium Vest: armor
    5: ("bow", (0.55, 0.9, 0.35)),       # Phaisa's Bow: attack speed
    6: ("staff", (0.7, 0.4, 1.0)),       # Phaisa Stick: ability damage
    7: ("glove", (1.0, 0.85, 0.3)),      # Selini's Gloves: crit
    8: ("heart", (1.0, 0.3, 0.35)),      # Omnilium Heart: health
    9: ("seed", (0.3, 0.95, 0.75)),      # Omnilium Seed
    51: ("orb", (0.75, 0.55, 1.0)),      # Omnilium Orb
}
SHAPE_PRIORITY = ["sword", "bow", "staff", "vest", "helmet", "glove", "heart", "water", "orb", "seed"]
TRAIT_COLOURS = {"Helios": (0.95, 0.55, 0.16), "Phaisa": (0.48, 0.25, 0.75), "Hexagon": (0.18, 0.77, 0.78), "Coregons": (0.62, 0.71, 0.6),
                 "Selini": (0.36, 0.5, 0.84), "Najmi": (0.91, 0.42, 0.66), "Omnilium": (0.85, 0.71, 0.29), "Protector": (0.63, 0.65, 0.71),
                 "Assassin": (0.45, 0.42, 0.55), "Nature": (0.35, 0.8, 0.35), "Bastion": (0.75, 0.62, 0.35), "Bruiser": (0.8, 0.35, 0.3),
                 "Sorcerer": (0.55, 0.45, 1.0), "Marksman": (0.4, 0.85, 0.5), "Mystic": (0.4, 0.7, 1.0), "Duelist": (0.95, 0.5, 0.3),
                 "Gunslinger": (0.95, 0.8, 0.35)}

GOLD = (1.0, 0.78, 0.36)
STEEL = (0.78, 0.8, 0.86)
DARKSTEEL = (0.42, 0.44, 0.5)
WOOD = (0.55, 0.36, 0.2)
LEATHER = (0.45, 0.28, 0.18)

ys, xs = np.mgrid[0:N, 0:N].astype(np.float64)
X = (xs + 0.5) / N * 2 - 1
Y = 1 - (ys + 0.5) / N * 2


# ------------------------------------------------------------------------------------------------------------------------------ shapes (signed distance, negative inside)
def circle(cx, cy, r, x=X, y=Y):
    return np.hypot(x - cx, y - cy) - r


def seg(ax, ay, bx, by, r, x=X, y=Y):
    px, py = x - ax, y - ay
    ex, ey = bx - ax, by - ay
    t = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0, 1)
    return np.hypot(px - ex * t, py - ey * t) - r


def poly(pts, x=X, y=Y):
    """A convex polygon, points counter-clockwise."""
    d = np.full(x.shape, -1e9)
    for i in range(len(pts)):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
        ex, ey = bx - ax, by - ay
        l = math.hypot(ex, ey)
        d = np.maximum(d, ((x - ax) * ey - (y - ay) * ex) / l)
    return d


def rbox(cx, cy, hw, hh, r, x=X, y=Y):
    qx = np.abs(x - cx) - hw + r
    qy = np.abs(y - cy) - hh + r
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


def rot(angle, cx=0.0, cy=0.0):
    """Coordinates rotated by -angle around (cx, cy): draw axis-aligned, show rotated."""
    c, s = math.cos(angle), math.sin(angle)
    return (X - cx) * c + (Y - cy) * s + cx, -(X - cx) * s + (Y - cy) * c + cy


def union(*ds):
    out = ds[0]
    for d in ds[1:]: out = np.minimum(out, d)
    return out


# ------------------------------------------------------------------------------------------------------------------------------ painting
def coverage(d, soft=1.0):
    return np.clip(0.5 - d / (PX * soft), 0, 1)


def shade(d, colour, kind="metal", bevel=0.09, glow=0.0):
    """An RGBA layer: the shape as a bevelled, lit solid. kind: metal | gold | gem | wood | leather | cloth."""
    h = np.clip(-d / bevel, 0, 1) ** 0.55
    gy, gx = np.gradient(h)
    k = 6.0
    nx, ny, nz = -gx * N * PX * k, gy * N * PX * k, np.ones_like(h)
    ln = np.sqrt(nx * nx + ny * ny + nz * nz); nx, ny, nz = nx / ln, ny / ln, nz / ln
    L = np.array([-0.55, 0.6, 0.58]); L /= np.linalg.norm(L)
    H = L + np.array([0, 0, 1.0]); H /= np.linalg.norm(H)
    diff = np.clip(nx * L[0] + ny * L[1] + nz * L[2], 0, 1)
    spec = np.clip(nx * H[0] + ny * H[1] + nz * H[2], 0, 1)
    c = np.array(colour)
    top = 0.85 + 0.25 * (Y + 1) / 2                                         # a painted top-down gradient
    specp, speci, amb = {"metal": (40, 0.9, 0.32), "gold": (30, 0.8, 0.35), "gem": (70, 1.2, 0.45), "wood": (8, 0.12, 0.35),
                         "leather": (10, 0.15, 0.33), "cloth": (6, 0.08, 0.38)}[kind]
    rgb = c[None, None, :] * (amb + 0.85 * diff)[..., None] * top[..., None]
    if kind == "gem":   # light inside the gem: brighter in the middle
        rgb = rgb + c[None, None, :] * (0.55 * h)[..., None]
    if kind in ("wood", "leather"):
        grain = 0.9 + 0.1 * np.sin((X * 7 + Y * 23) * 3.0 + np.sin(Y * 9) * 2)
        rgb = rgb * grain[..., None]
    rgb = rgb + (speci * spec ** specp)[..., None] * np.array([1.0, 0.97, 0.9])[None, None, :]
    rgb = rgb + glow * c[None, None, :] * h[..., None]
    edge = np.clip(1 - (-d) / (PX * 3.0), 0, 1)                              # a dark painted outline
    rgb = rgb * (1 - 0.65 * edge)[..., None]
    return np.concatenate([np.clip(rgb, 0, 1), coverage(d)[..., None]], axis=2)


def over(dst, layer):
    a = layer[..., 3:4]
    dst[..., :3] = dst[..., :3] * (1 - a) + layer[..., :3] * a
    return dst


def blur(a, passes=6):
    small = a[::4, ::4].copy()
    for _ in range(passes):
        small = (small + np.roll(small, 1, 0) + np.roll(small, -1, 0) + np.roll(small, 1, 1) + np.roll(small, -1, 1)) / 5
    return np.repeat(np.repeat(small, 4, 0), 4, 1)[:N, :N]


def background(colour, seed):
    rng = np.random.default_rng(seed)
    c = np.array(colour)
    r = np.hypot(X, Y + 0.05)
    img = np.zeros((N, N, 3))
    img += c * 0.16
    img += c * (0.55 * np.exp(-(r / 0.75) ** 2))[..., None]
    ang = np.arctan2(Y, X)
    img += c * (0.12 * (0.5 + 0.5 * np.sin(ang * 7 + rng.uniform(0, 6))) ** 4 * np.exp(-r))[..., None]
    for _ in range(18):                                                      # a few painted motes
        px, py, rr = rng.uniform(-0.9, 0.9), rng.uniform(-0.9, 0.9), rng.uniform(0.01, 0.035)
        img += c * (0.5 * np.exp(-(np.hypot(X - px, Y - py) / rr) ** 2))[..., None]
    img *= (1 - 0.45 * np.clip(r - 0.55, 0, 1))[..., None]
    return np.clip(img, 0, 1)


def frame(img, colour, kind):
    """A bevelled frame round the icon; completed items get gold, components steel, emblems their colour."""
    d_out = rbox(0, 0, 1.0, 1.0, 0.12)
    d_in = rbox(0, 0, 0.9, 0.9, 0.08)
    ring = np.maximum(d_out, -d_in)
    lay = shade(ring, colour, "gold" if kind != "component" else "metal", bevel=0.06)
    img = over(img, lay)
    inner = np.clip(1 - (-d_in) / (PX * 8), 0, 1) * (d_in < 0)              # a shadow just inside the frame
    img[..., :3] *= (1 - 0.5 * inner)[..., None]
    return img


# ------------------------------------------------------------------------------------------------------------------------------ objects
def obj_sword(accent, variant):
    x, y = rot(math.radians(45))
    blade_w = [0.13, 0.17, 0.11, 0.15][variant % 4]
    tip = 0.95
    blade = poly([(-blade_w, -0.28), (blade_w, -0.28), (blade_w * 0.8, tip - 0.25), (0.0, tip), (-blade_w * 0.8, tip - 0.25)], x, y)
    fuller = rbox(0, 0.28, 0.025, 0.42, 0.02, x, y)
    guard = rbox(0, -0.33, 0.33, 0.055, 0.04, x, y)
    grip = rbox(0, -0.55, 0.05, 0.2, 0.03, x, y)
    pommel = circle(0, -0.8, 0.09, x, y)
    gem = circle(0, -0.33, 0.06, x, y)
    return [(blade, STEEL, "metal", 0.08, 0.0), (fuller, accent, "gem", 0.03, 0.4), (guard, GOLD, "gold", 0.05, 0.0), (grip, LEATHER, "leather", 0.04, 0.0),
            (pommel, accent, "gem", 0.07, 0.3), (gem, accent, "gem", 0.05, 0.5)]


def obj_bow(accent, variant):
    x, y = rot(math.radians(-20))
    r = np.hypot(x - 0.45, y)
    arc = np.maximum(np.abs(r - 0.95) - 0.075, x - 0.1)
    arc = np.maximum(arc, np.abs(y) - 0.85)
    string = seg(0.2, 0.83, 0.2, -0.83, 0.012, x, y)
    grip = rbox(-0.5, 0.0, 0.07, 0.16, 0.04, x, y)
    tips = union(circle(0.12, 0.84, 0.07, x, y), circle(0.12, -0.84, 0.07, x, y))
    arrow = union(seg(-0.4, 0.0, 0.85, 0.0, 0.022, x, y), poly([(0.8, 0.09), (0.98, 0.0), (0.8, -0.09)][::-1] if False else [(0.8, -0.09), (0.98, 0.0), (0.8, 0.09)], x, y))
    return [(arc, WOOD, "wood", 0.07, 0.0), (string, (0.9, 0.9, 0.85), "cloth", 0.01, 0.0), (arrow, STEEL, "metal", 0.03, 0.0),
            (grip, LEATHER, "leather", 0.05, 0.0), (tips, accent, "gem", 0.06, 0.5)]


def obj_staff(accent, variant):
    x, y = rot(math.radians(-35))
    rod = rbox(0, -0.25, 0.05, 0.7, 0.04, x, y)
    rings = union(rbox(0, 0.28, 0.09, 0.035, 0.02, x, y), rbox(0, -0.2, 0.08, 0.03, 0.02, x, y))
    if variant % 2 == 0:
        crystal = poly([(0, 0.35), (0.19, 0.62), (0, 0.98), (-0.19, 0.62)], x, y)
    else:
        crystal = circle(0, 0.64, 0.24, x, y)
    claws = union(seg(-0.13, 0.35, -0.22, 0.62, 0.035, x, y), seg(0.13, 0.35, 0.22, 0.62, 0.035, x, y))
    return [(rod, WOOD, "wood", 0.05, 0.0), (rings, GOLD, "gold", 0.03, 0.0), (claws, GOLD, "gold", 0.04, 0.0), (crystal, accent, "gem", 0.14, 0.7)]


def obj_vest(accent, variant):
    body = union(rbox(0, -0.1, 0.5, 0.62, 0.22), circle(-0.45, 0.42, 0.24), circle(0.45, 0.42, 0.24))
    neck = circle(0, 0.62, 0.2)
    plate = np.maximum(body, -neck)
    rim = np.maximum(plate, -(plate + 0.09))
    gem = poly([(0, -0.25), (0.14, 0.02), (0, 0.28), (-0.14, 0.02)])
    return [(plate, (0.78, 0.66, 0.4) if variant % 2 else STEEL, "metal", 0.12, 0.0), (rim, GOLD, "gold", 0.04, 0.0), (gem, accent, "gem", 0.1, 0.6)]


def obj_helmet(accent, variant):
    dome = np.maximum(circle(0, 0.05, 0.66), -(Y + 0.45))
    cheeks = union(rbox(-0.42, -0.4, 0.2, 0.22, 0.08), rbox(0.42, -0.4, 0.2, 0.22, 0.08))
    helm = union(dome, cheeks)
    visor = rbox(0, -0.12, 0.46, 0.07, 0.05)
    crest = rbox(0, 0.55, 0.08, 0.3, 0.06)
    return [(helm, STEEL, "metal", 0.14, 0.0), (visor, (0.08, 0.08, 0.12), "cloth", 0.02, 0.0), (crest, accent, "gem", 0.06, 0.4)]


def obj_glove(accent, variant):
    palm = rbox(0, -0.15, 0.36, 0.34, 0.14)
    fingers = union(*[rbox(-0.27 + i * 0.18, 0.33, 0.075, 0.22, 0.07) for i in range(4)])
    thumb = seg(0.36, -0.1, 0.62, 0.18, 0.09)
    cuff = rbox(0, -0.62, 0.42, 0.16, 0.08)
    knuckles = union(*[circle(-0.27 + i * 0.18, 0.12, 0.06) for i in range(4)])
    return [(cuff, LEATHER, "leather", 0.08, 0.0), (union(palm, fingers, thumb), GOLD if variant % 2 else STEEL, "metal", 0.12, 0.0), (knuckles, accent, "gem", 0.05, 0.5)]


def obj_heart(accent, variant):
    heart = union(circle(-0.3, 0.2, 0.36), circle(0.3, 0.2, 0.36), poly([(0.0, -0.8), (0.64, 0.05), (-0.64, 0.05)][::-1] if False else [(-0.64, 0.05), (0.0, -0.8), (0.64, 0.05)]))
    setting = np.maximum(heart - 0.08, -(heart + 0.0))
    return [(heart - 0.06, GOLD, "gold", 0.08, 0.0), (heart + 0.02, accent, "gem", 0.2, 0.6)]


def obj_water(accent, variant):
    drop = union(circle(0, -0.2, 0.52), poly([(-0.44, 0.05), (0.0, 0.92), (0.44, 0.05)][::-1] if False else [(0.44, 0.05), (0.0, 0.92), (-0.44, 0.05)]))
    band = np.maximum(np.abs(drop + 0.04) - 0.035, -(drop + 0.04))
    return [(drop, accent, "gem", 0.24, 0.6), (np.maximum(drop, -(drop + 0.07)) , GOLD, "gold", 0.04, 0.0)]


def obj_seed(accent, variant):
    seed = circle(0, -0.25, 0.42)
    sprout = seg(0, 0.1, 0.0, 0.55, 0.05)
    leaves = union(circle(-0.25, 0.62, 0.2) , circle(0.25, 0.62, 0.2))
    return [(seed, (0.6, 0.45, 0.3), "wood", 0.15, 0.0), (sprout, (0.35, 0.75, 0.3), "cloth", 0.03, 0.0), (leaves, accent, "gem", 0.1, 0.5)]


def obj_orb(accent, variant):
    orb = circle(0, 0.0, 0.62)
    ring = np.maximum(np.abs(np.hypot(X, Y * 3.0) - 0.8) - 0.05, -(Y + 0.2))
    return [(orb, accent, "gem", 0.3, 0.6), (ring, GOLD, "gold", 0.04, 0.0)]


def obj_emblem(accent, variant):
    hexd = poly([(0.78 * math.cos(math.radians(a)), 0.78 * math.sin(math.radians(a))) for a in range(0, 360, 60)])
    inner = poly([(0.55 * math.cos(math.radians(a)), 0.55 * math.sin(math.radians(a))) for a in range(0, 360, 60)])
    star = []
    for k in range(10):
        a = math.pi / 2 + k * math.pi / 5
        rr = 0.42 if k % 2 == 0 else 0.18
        star.append((rr * math.cos(a), rr * math.sin(a)))
    stard = union(*[poly([star[(2 * k - 1) % 10], star[2 * k], star[(2 * k + 1) % 10]]) for k in range(5)], poly([star[(2 * k + 1) % 10] for k in range(5)]))
    return [(hexd, GOLD, "gold", 0.1, 0.0), (inner, accent, "gem", 0.2, 0.5), (stard, (1.0, 0.98, 0.9), "metal", 0.05, 0.3)]


def obj_remover(accent, variant):
    """The Item Remover: a horseshoe magnet."""
    x, y = rot(math.radians(-30))
    r = np.hypot(x, y - 0.1)
    arch = np.maximum(np.maximum(np.abs(r - 0.48) - 0.16, -(y - 0.1)), 0)
    legs = union(rbox(-0.48, -0.25, 0.16, 0.35, 0.03, x, y), rbox(0.48, -0.25, 0.16, 0.35, 0.03, x, y))
    body = union(np.maximum(np.abs(r - 0.48) - 0.16, -(y - 0.1)), legs)
    tips = union(rbox(-0.48, -0.55, 0.16, 0.1, 0.02, x, y), rbox(0.48, -0.55, 0.16, 0.1, 0.02, x, y))
    return [(body, (0.9, 0.22, 0.2), "metal", 0.12, 0.0), (tips, STEEL, "metal", 0.06, 0.0)]


OBJECTS = {"sword": obj_sword, "bow": obj_bow, "staff": obj_staff, "vest": obj_vest, "helmet": obj_helmet, "glove": obj_glove, "heart": obj_heart,
           "water": obj_water, "seed": obj_seed, "orb": obj_orb}


# ------------------------------------------------------------------------------------------------------------------------------ the items
def load_items():
    text = open(DATA).read()
    text = re.sub(r"//[^\n]*", "", text)
    return json.loads(text)["items"]


def design(item):
    """(object function, accent colour, frame kind, frame colour, background colour)"""
    iid = item["id"]
    comps = item.get("components", [])
    if item.get("consumable"):
        return obj_remover, (1.0, 0.35, 0.3), "component", STEEL, (0.9, 0.3, 0.3)
    if iid in COMPONENT:
        shape, colour = COMPONENT[iid]
        return OBJECTS[shape], colour, "component", STEEL, colour
    if item.get("grantsTraits"):
        colour = TRAIT_COLOURS.get(item["grantsTraits"][0], (0.8, 0.8, 0.8))
        return obj_emblem, colour, "emblem", colour, colour
    if len(comps) == 2 and all(c in COMPONENT for c in comps):
        shapes = [COMPONENT[c][0] for c in comps]
        first = min(range(2), key=lambda i: SHAPE_PRIORITY.index(shapes[i]))
        other = comps[1 - first] if comps[0] != comps[1] else comps[first]
        accent = COMPONENT[other][1]
        return OBJECTS[shapes[first]], accent, "completed", GOLD, accent
    return obj_orb, (0.8, 0.8, 0.8), "completed", GOLD, (0.6, 0.6, 0.6)


def render(item):
    fn, accent, kind, frame_colour, bg = design(item)
    variant = item["id"] * 7 % 11
    img = background(bg, item["id"])
    parts = fn(accent, variant)
    glow_mask = np.zeros((N, N))
    for d, colour, material, bevel, glow in parts:
        if glow > 0: glow_mask = np.maximum(glow_mask, coverage(d) * glow)
    img += np.array(accent)[None, None, :] * (0.9 * blur(glow_mask))[..., None]   # the glow spills onto the background
    silhouette = union(*[p[0] for p in parts])
    shadow = coverage(silhouette + 0.02, soft=12.0)                           # a soft drop shadow down-right
    shadow = np.roll(np.roll(shadow, 6, 0), 6, 1)
    img[..., :3] *= (1 - 0.45 * shadow)[..., None]
    for d, colour, material, bevel, glow in parts:
        img = over(np.concatenate([img[..., :3], np.ones((N, N, 1))], axis=2), shade(d, colour, material, bevel, glow))[..., :3] if img.shape[2] == 3 else over(img, shade(d, colour, material, bevel, glow))
    img = frame(np.concatenate([img, np.ones((N, N, 1))], axis=2) if img.shape[2] == 3 else img, frame_colour, kind)
    rgb = np.clip(img[..., :3], 0, 1)
    small = rgb.reshape(N // 2, 2, N // 2, 2, 3).mean(axis=(1, 3))
    return small


def write_png(path, rgb):
    h, w = rgb.shape[:2]
    data = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    only = None
    if "--only" in sys.argv:
        only = {int(v) for v in sys.argv[sys.argv.index("--only") + 1].split(",")}
    os.makedirs(OUT, exist_ok=True)
    count = 0
    for item in load_items():
        if only and item["id"] not in only:
            continue
        write_png(os.path.join(OUT, "T_Item_%d.png" % item["id"]), render(item))
        count += 1
    print("wrote %d item icons to %s" % (count, OUT))


if __name__ == "__main__":
    main()
