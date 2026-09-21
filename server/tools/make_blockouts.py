#!/usr/bin/env python3
"""W2F blockout mesh generator: low-poly placeholder models for every champion, summon and PvE monster, as binary glTF (.glb).

    python3 tools/make_blockouts.py [--data data] [--out docs/blockouts] [--no-preview]

Reads data/champions.json and data/pve.json (roster, roles, ranges, traits), builds one model per entry from primitives (boxes, cylinders, spheres), colours it by
its traits, and writes:
  SM_<Kind>_<id>_<Name>.glb   one static mesh each: metres, +Y up, feet at the origin, facing +Z (the glTF convention; UE's importer turns it into Z-up, facing +X),
                              flat-shaded, colour in the COLOR_0 vertex colours (one material "M_Blockout" whose base colour is the model's main colour as a fallback)
  SM_HexTile.glb, SM_HexTile_Home/_Away.glb, SM_BenchSlot.glb, SM_ArenaBase.glb, SM_Backdrop.glb, SM_ProjectileOrb.glb, SM_AreaDisc.glb
                              board tiles (pointy-top, 1 m across the flats; outlined like an auto-battler board), a bench slab, the island with pillars and pines, the sky
                              plane, a projectile, a unit-radius disc for spell areas
  manifest.json               per model: id, name, file, height, colours, and sockets (positions on the centre line, in metres: forward = +Z, up = +Y)
  contact_sheet.png           a picture of everything (software-rendered, so it needs no viewer)
Everything is a pure function of the data: run it again after adding a champion and the new one gets a model automatically (a hand-made recipe in RECIPES is
optional, without one the model is chosen from role and range). Standard library only. See docs/blockouts/README.md for the Unreal import steps.
"""
import argparse, json, math, os, struct, sys, zlib

# ------------------------------------------------------------------------------------------------------------------------------ data files (JSON with comments)

def load_json(path):
    s = open(path, encoding="utf-8").read()
    out, i, n, instr = [], 0, len(s), False
    while i < n:
        c = s[i]
        if instr:
            out.append(c)
            if c == "\\":
                out.append(s[i + 1]); i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True; out.append(c)
        elif s.startswith("//", i):
            while i < n and s[i] != "\n": i += 1
            continue
        elif s.startswith("/*", i):
            i = s.index("*/", i) + 2; continue
        else:
            out.append(c)
        i += 1
    return json.loads("".join(out))

# ------------------------------------------------------------------------------------------------------------------------------ small vector helpers

def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def norm(a):
    l = math.sqrt(dot(a, a))
    return (0.0, 1.0, 0.0) if l < 1e-12 else (a[0] / l, a[1] / l, a[2] / l)

def rot_pt(p, ax, deg, pivot):
    """Rotate point p around the X (ax=0), Y (1) or Z (2) axis through `pivot` by deg degrees (right-handed)."""
    a = math.radians(deg); c, s = math.cos(a), math.sin(a)
    x, y, z = p[0] - pivot[0], p[1] - pivot[1], p[2] - pivot[2]
    if ax == 0: y, z = y * c - z * s, y * s + z * c
    elif ax == 1: x, z = x * c + z * s, -x * s + z * c
    else: x, y = x * c - y * s, x * s + y * c
    return (x + pivot[0], y + pivot[1], z + pivot[2])

# ------------------------------------------------------------------------------------------------------------------------------ primitives: lists of triangles
# A triangle is (a, b, c) with counter-clockwise winding seen from OUTSIDE (glTF front faces). Each primitive is convex, so a triangle is flipped when its
# normal points towards the shape's centre.

def _tri(a, b, c, centre):
    n = cross(sub(b, a), sub(c, a))
    if dot(n, n) < 1e-14: return []
    mid = ((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3, (a[2] + b[2] + c[2]) / 3)
    return [(a, b, c)] if dot(n, sub(mid, centre)) >= 0 else [(a, c, b)]

def box(cx, cy, cz, sx, sy, sz):
    hx, hy, hz = sx / 2, sy / 2, sz / 2
    v = [(cx + dx * hx, cy + dy * hy, cz + dz * hz) for dx in (-1, 1) for dy in (-1, 1) for dz in (-1, 1)]
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    t = []
    for a, b, c, d in quads:
        t += _tri(v[a], v[b], v[c], (cx, cy, cz)) + _tri(v[a], v[c], v[d], (cx, cy, cz))
    return t

def cyl(cx, y0, cz, r0, r1, h, seg=8):
    """A (tapered) cylinder along +Y from y0 to y0+h; r1 = 0 makes a cone."""
    c = (cx, y0 + h / 2, cz)
    ring0 = [(cx + r0 * math.cos(2 * math.pi * i / seg), y0, cz + r0 * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
    ring1 = [(cx + r1 * math.cos(2 * math.pi * i / seg), y0 + h, cz + r1 * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
    t = []
    for i in range(seg):
        j = (i + 1) % seg
        t += _tri(ring0[i], ring0[j], ring1[j], c) + _tri(ring0[i], ring1[j], ring1[i], c)
        t += _tri((cx, y0, cz), ring0[j], ring0[i], c) + _tri((cx, y0 + h, cz), ring1[i], ring1[j], c)
    return t

def sphere(cx, cy, cz, rx, ry=None, rz=None, lat=4, lon=8):
    ry = rx if ry is None else ry; rz = rx if rz is None else rz
    c = (cx, cy, cz)
    def pt(i, j):
        a = math.pi * i / lat; b = 2 * math.pi * j / lon
        return (cx + rx * math.sin(a) * math.cos(b), cy + ry * math.cos(a), cz + rz * math.sin(a) * math.sin(b))
    t = []
    for i in range(lat):
        for j in range(lon):
            k = (j + 1) % lon
            t += _tri(pt(i, j), pt(i + 1, j), pt(i + 1, k), c) + _tri(pt(i, j), pt(i + 1, k), pt(i, k), c)
    return t

def rotate(tris, ax, deg, pivot=(0, 0, 0)):
    return [tuple(rot_pt(p, ax, deg, pivot) for p in tri) for tri in tris]

def move(tris, dx=0.0, dy=0.0, dz=0.0):
    return [tuple((p[0] + dx, p[1] + dy, p[2] + dz) for p in tri) for tri in tris]

def scale(tris, s):
    return [tuple((p[0] * s, p[1] * s, p[2] * s) for p in tri) for tri in tris]

def ring(cx, cy, cz, radius, thick, n=10, tilt=0.0):
    """A ring made of n small boxes in the XZ plane, then tilted about X."""
    t = []
    for i in range(n):
        a = 2 * math.pi * i / n
        t += rotate(box(cx + radius * math.cos(a), cy, cz + radius * math.sin(a), thick * 1.6, thick, thick), 1, -math.degrees(a) - 90, (cx + radius * math.cos(a), cy, cz + radius * math.sin(a)))
    return rotate(t, 0, tilt, (cx, cy, cz)) if tilt else t

# ------------------------------------------------------------------------------------------------------------------------------ palette

TRAIT_COLOURS = {   # sRGB hex; one look per synergy so a board reads at a glance
    "Helios": "F28C28", "Phaisa": "7B3FBF", "Hexagon": "2EC4C6", "Coregons": "9DB59A", "Selini": "5B7FD6",
    "Najmi": "E86AA8", "Omnilium": "D9B44A", "Protector": "A0A7B4", "Assassin": "34343F",
}
DARK, SKIN, BONE, METAL = "2B2B33", "E8C9A6", "DDD8C2", "B9BDC7"

def srgb(h): return tuple(int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))
def lin(c): return tuple((v / 12.92) if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in c)
def mix(a, b, t): return tuple(a[i] * (1 - t) + b[i] * t for i in range(3))

def palette(traits, undead=False):
    cols = [srgb(TRAIT_COLOURS[t]) for t in traits if t in TRAIT_COLOURS] or [srgb("8A8F99")]
    P = cols[0]
    S = cols[1] if len(cols) > 1 else mix(P, srgb(DARK), 0.45)
    return {"P": P, "S": S, "D": srgb(DARK), "K": srgb(BONE if undead else SKIN), "M": srgb(METAL), "G": mix(P, (1, 1, 1), 0.55)}

# ------------------------------------------------------------------------------------------------------------------------------ the builder

class Model:
    def __init__(self, pal):
        self.pal, self.tris, self.sockets = pal, [], {}
    def add(self, tris, colour):
        c = self.pal[colour] if isinstance(colour, str) else colour
        self.tris += [(t, c) for t in tris]

# ------------------------------------------------------------------------------------------------------------------------------ the humanoid body and its parts (nominal size 1.7 m, scaled at the end)

def arm(m, side, ang, bw, colour="P"):
    """side -1 = the model's right (glTF +X is its left). ang = forward swing in degrees about the shoulder (negative raises the arm forwards). Returns the hand."""
    sx = side * (0.23 * bw + 0.08); pivot = (sx, 1.25, 0.0)
    m.add(rotate(box(sx, 0.975, 0, 0.12 * bw, 0.55, 0.12), 0, ang, pivot), colour)
    m.add(rotate(sphere(sx, 0.70, 0, 0.065 * bw), 0, ang, pivot), "K")
    return rot_pt((sx, 0.70, 0.0), 0, ang, pivot)

def weapon(m, kind, hand, side=-1):
    x, y, z = hand
    if kind == "sword":
        m.add(rotate(box(x, y + 0.42, z + 0.05, 0.05, 0.84, 0.015), 0, -20, hand), "M"); m.add(rotate(box(x, y + 0.02, z + 0.05, 0.22, 0.04, 0.05), 0, -20, hand), "D")
    elif kind == "greatsword":
        m.add(rotate(box(x, y + 0.65, z + 0.08, 0.13, 1.3, 0.03), 0, -25, hand), "M"); m.add(rotate(box(x, y + 0.04, z + 0.08, 0.32, 0.07, 0.07), 0, -25, hand), "D")
    elif kind == "axe":
        m.add(rotate(cyl(x, y - 0.25, z + 0.05, 0.03, 0.03, 1.1), 0, -15, hand), "D"); m.add(rotate(box(x, y + 0.72, z + 0.16, 0.05, 0.36, 0.34), 0, -15, hand), "M")
    elif kind == "club":
        m.add(rotate(cyl(x, y - 0.15, z + 0.05, 0.05, 0.13, 0.85), 0, -15, hand), "K")
    elif kind == "staff":
        m.add(cyl(x, y - 0.6, z + 0.05, 0.03, 0.03, 1.75), "D"); m.add(sphere(x, y + 1.24, z + 0.05, 0.11), "G")
    elif kind == "wand":
        m.add(rotate(cyl(x, y - 0.05, z + 0.05, 0.025, 0.025, 0.45), 0, -20, hand), "D"); m.add(sphere(x, y + 0.42, z + 0.16, 0.08), "G")
    elif kind == "bow":
        c = (x, y, z + 0.15)
        m.add(rotate(box(c[0], c[1] + 0.25, c[2], 0.04, 0.55, 0.04), 0, -18, c), "D")
        m.add(rotate(box(c[0], c[1] - 0.25, c[2], 0.04, 0.55, 0.04), 0, 18, c), "D")
        m.add(box(c[0], c[1], c[2] - 0.1, 0.01, 1.0, 0.01), "M")
    elif kind == "rifle":
        m.add(box(x, y + 0.02, z + 0.35, 0.07, 0.09, 1.0), "D"); m.add(cyl(x, y - 0.03, z + 0.98, 0.03, 0.03, 0.02), "M")
        m.add(rotate(cyl(x, y, z + 0.9, 0.03, 0.03, 0.3), 0, 90, (x, y, z + 0.9)), "M")
    elif kind == "daggers":
        m.add(rotate(box(x, y + 0.02, z + 0.22, 0.03, 0.05, 0.45), 0, 0, hand), "M")
    elif kind == "shield":
        m.add(box(x - side * 0.12, y + 0.18, z + 0.18, 0.06, 0.62, 0.46), "M"); m.add(sphere(x - side * 0.16, y + 0.18, z + 0.18, 0.09, 0.09, 0.05), "G")
    elif kind == "fist":
        m.add(sphere(x, y - 0.06, z + 0.05, 0.17), "K")

def build_humanoid(m, sp):
    bw = sp.get("bw", 1.0)
    P, S = "P", "S"
    # legs / robe
    if sp.get("robe"):
        m.add(cyl(0, 0.0, 0, 0.30 * bw, 0.20 * bw, 0.80, 8), S)
    else:
        for sx in (-1, 1):
            m.add(box(sx * 0.10 * bw, 0.375, 0, 0.15 * bw, 0.75, 0.17), "D")
    # torso
    m.add(box(0, 1.02, 0, 0.46 * bw, 0.55, 0.26 * (0.8 + 0.2 * bw)), P)
    m.add(box(0, 0.78, 0, 0.48 * bw, 0.07, 0.28 * (0.8 + 0.2 * bw)), "D")   # belt
    # head
    m.add(sphere(0, 1.46, 0.0, 0.16), "K")
    for sx in (-1, 1): m.add(box(sx * 0.055, 1.48, 0.145, 0.045, 0.04, 0.02), "D")   # eyes
    # arms
    weapons = sp.get("weapons", [])
    ranged = any(w in weapons for w in ("bow", "rifle", "staff", "wand"))
    rhand = arm(m, -1, -70 if ranged else -8, bw, P)
    lhand = arm(m, 1, -70 if "bow" in weapons or "rifle" in weapons else 4, bw, P)
    for w in weapons:
        if w in ("shield",): weapon(m, w, lhand, +1)
        elif w == "daggers":
            weapon(m, w, rhand, -1); weapon(m, w, lhand, +1)
        else: weapon(m, w, rhand, -1)
    for e in sp.get("extras", []): EXTRAS[e](m, sp)
    # sockets on the centre line
    m.sockets = {"feet": (0, 0, 0), "chest": (0, 1.1, 0.14), "cast_origin": (0, 1.15, 0.45), "muzzle": (0, 1.2, 0.7), "head_top": (0, 1.75, 0), "overhead": (0, 2.1, 0)}

# ---- extras (each takes the model and the recipe)
def x_hood(m, sp): m.add(cyl(0, 1.44, -0.02, 0.21, 0.0, 0.36), "D"); m.add(cyl(0, 1.3, -0.06, 0.2, 0.2, 0.16), "D")
def x_hat(m, sp): m.add(cyl(0, 1.58, 0, 0.30, 0.30, 0.03), "S"); m.add(cyl(0, 1.6, 0, 0.17, 0.0, 0.45), "S")
def x_helm(m, sp): m.add(sphere(0, 1.5, 0, 0.19, 0.16, 0.19), "M")
def x_crown(m, sp):
    for i in range(5):
        a = 2 * math.pi * i / 5; m.add(cyl(0.15 * math.cos(a), 1.62, 0.15 * math.sin(a), 0.035, 0.0, 0.14, 4), "G")
def x_horns(m, sp):
    for sx in (-1, 1): m.add(rotate(cyl(sx * 0.13, 1.56, 0, 0.045, 0.0, 0.3, 5), 2, -sx * 35, (sx * 0.13, 1.56, 0)), "K")
def x_crest(m, sp):
    for k, (dz, h) in enumerate(((-0.06, 0.36), (0.02, 0.44), (0.1, 0.3))): m.add(cyl(0, 1.6, dz, 0.07, 0.0, h, 5), "G" if k == 1 else "S")
def x_halo(m, sp): m.add(ring(0, 1.85, 0, 0.2, 0.04, 10), "G")
def x_cape(m, sp): m.add(box(0, 1.0, -0.17, 0.55 * sp.get("bw", 1.0), 0.95, 0.03), "S")
def x_shoulders(m, sp):
    bw = sp.get("bw", 1.0)
    for sx in (-1, 1): m.add(sphere(sx * (0.27 * bw + 0.05), 1.3, 0, 0.17 * (0.8 + 0.2 * bw), 0.12, 0.17 * (0.8 + 0.2 * bw)), "M")
def x_spikes(m, sp):
    for i, y in enumerate((1.25, 1.05, 0.85)): m.add(rotate(cyl(0, y, -0.14, 0.06, 0.0, 0.3, 5), 0, -100, (0, y, -0.14)), "K")
def x_wings(m, sp):
    for sx in (-1, 1): m.add(rotate(box(sx * 0.42, 1.2, -0.2, 0.62, 0.05, 0.28), 2, sx * 25, (sx * 0.1, 1.2, -0.2)), "G")
def x_rocket(m, sp): m.add(rotate(cyl(0.28, 1.28, -0.05, 0.075, 0.075, 0.6), 0, -90, (0.28, 1.28, -0.05)), "M")
def x_wall(m, sp): m.add(box(0, 0.85, 0.42, 1.0, 1.3, 0.14), "M"); m.add(box(0, 0.85, 0.5, 0.5, 0.6, 0.05), "G")
def x_moon(m, sp): m.add(ring(0, 1.35, -0.5, 0.55, 0.07, 14, 90), "G")
def x_orbs(m, sp):
    for i in range(3):
        a = 2 * math.pi * i / 3 + 0.5; m.add(sphere(0.6 * math.cos(a), 1.5 + 0.12 * i, 0.6 * math.sin(a), 0.1), "G")
def x_bigorb(m, sp): m.add(sphere(0, 2.15, 0, 0.16), "G")
def x_skulls(m, sp):
    for sx in (-1, 1): m.add(sphere(sx * 0.3, 0.8, 0.1, 0.07), "K")
def x_bigarm(m, sp): m.add(box(0.5, 1.0, 0.1, 0.24, 0.8, 0.24), "M"); m.add(sphere(0.5, 0.52, 0.1, 0.19), "M")
def x_pipes(m, sp): m.add(box(0, 1.1, -0.2, 0.3, 0.45, 0.14), "M")
def x_gem(m, sp): m.add(sphere(0, 1.15, 0.15, 0.07), "G")
def x_orb_hover(m, sp): m.add(sphere(0.55, 1.0, 0.25, 0.14), "D"); m.add(ring(0.55, 1.0, 0.25, 0.22, 0.03, 8, 70), "G")

EXTRAS = {"hood": x_hood, "hat": x_hat, "helm": x_helm, "crown": x_crown, "horns": x_horns, "crest": x_crest, "halo": x_halo, "cape": x_cape, "shoulders": x_shoulders,
          "spikes": x_spikes, "wings": x_wings, "rocket": x_rocket, "wall": x_wall, "moon": x_moon, "orbs": x_orbs, "bigorb": x_bigorb, "skulls": x_skulls, "bigarm": x_bigarm,
          "pipes": x_pipes, "gem": x_gem, "orb_hover": x_orb_hover}

# ------------------------------------------------------------------------------------------------------------------------------ recipes: an optional look per champion id
# hs = height scale, bw = body width, robe = long robe instead of legs, weapons = what the hands hold, extras = head / back / body details, undead = bone skin.
RECIPES = {
    9001: dict(hs=1.05, bw=1.4, weapons=["sword", "shield"], extras=["shoulders", "crest"]),                     # Alesk, Helios tank
    9002: dict(hs=1.0, bw=0.85, weapons=["rifle"], extras=["cape", "hood"]),                                     # Baira, Purple Sniper
    9003: dict(hs=0.95, bw=0.9, weapons=["rifle"], extras=["rocket", "helm"]),                                   # Cyla
    9005: dict(hs=1.0, bw=0.9, weapons=["wand"], extras=["bigarm", "pipes"]),                                    # Faire, Arm of the Future
    9006: dict(hs=1.05, bw=1.6, weapons=["fist"], extras=["wall", "helm"]),                                      # Les, Wall of Omnilium
    9007: dict(hs=1.05, bw=1.45, weapons=["fist"], extras=["shoulders", "horns", "crown"]),                      # Lum, Guardian of Omnilium
    9008: dict(hs=0.9, bw=0.8, weapons=["wand"], extras=["halo", "gem"]),                                        # Astra
    9009: dict(hs=0.95, bw=0.8, weapons=["daggers"], extras=["hood", "cape"]),                                   # Lunis, crescent swipe
    9010: dict(hs=0.95, bw=1.25, weapons=["shield", "sword"], extras=["shoulders", "skulls"], undead=True),      # Soul, The Lost Vessel
    9011: dict(hs=1.0, bw=0.85, robe=True, weapons=["staff"], extras=["moon", "hood"]),                          # Myna, Tidecaller
    9012: dict(hs=1.05, bw=0.85, robe=True, weapons=["wand"], extras=["orbs", "bigorb", "hat"]),                 # Vega, Supernova
    9013: dict(hs=0.95, bw=1.3, weapons=["sword", "shield"], extras=["crest", "shoulders"]),                     # Ignis
    9014: dict(hs=0.95, bw=0.85, weapons=["bow"], extras=["cape"]),                                              # Pyra
    9015: dict(hs=0.92, bw=0.8, weapons=["daggers"], extras=["hood"]),                                           # Vex
    9016: dict(hs=1.0, bw=1.35, weapons=["fist"], extras=["orb_hover", "shoulders"]),                            # Null
    9017: dict(hs=1.0, bw=1.3, weapons=["club"], extras=["skulls", "horns"], undead=True),                       # Bone
    9018: dict(hs=0.95, bw=0.85, robe=True, weapons=["staff"], extras=["skulls", "hood"], undead=True),          # Rot
    9019: dict(hs=1.0, bw=1.2, weapons=["sword"], extras=["helm", "pipes"]),                                     # Bit
    9020: dict(hs=1.0, bw=1.3, weapons=["greatsword"], extras=["crest", "shoulders"]),                           # Solis
    9021: dict(hs=0.95, bw=0.85, weapons=["wand"], extras=["hat", "orb_hover"]),                                 # Xul
    9022: dict(hs=0.95, bw=0.95, robe=True, weapons=["staff"], extras=["skulls", "spikes"], undead=True),        # Grave
    9023: dict(hs=0.95, bw=0.95, weapons=["shield", "wand"], extras=["pipes", "helm"]),                          # Byte
    9024: dict(hs=1.0, bw=0.85, weapons=["bow"], extras=["cape", "wings"]),                                      # Orion
    9025: dict(hs=1.0, bw=1.3, weapons=["daggers"], extras=["horns", "shoulders"]),                              # Nyx
    9026: dict(hs=1.0, bw=0.9, robe=True, weapons=["staff"], extras=["crest"]),                                  # Flare
    9027: dict(hs=1.05, bw=0.9, robe=True, weapons=["staff"], extras=["crown", "skulls"], undead=True),          # Lich
    9028: dict(hs=1.0, bw=0.9, weapons=["daggers"], extras=["hood", "wings"]),                                   # Raa
    9029: dict(hs=1.05, bw=1.4, weapons=["axe"], extras=["spikes", "horns"]),                                    # Kryx
    9030: dict(hs=1.0, bw=0.9, weapons=["rifle"], extras=["cape", "hood", "skulls"], undead=True),               # Mortis
    9031: dict(hs=1.05, bw=1.4, weapons=["fist"], extras=["moon", "shoulders"]),                                 # Umbra
}

def fallback_recipe(role, rng):
    """A model for a champion nobody has drawn a recipe for yet: chosen from role and range."""
    if role == "Tank" or role == "tank": return dict(hs=1.0, bw=1.35, weapons=["sword", "shield"], extras=["shoulders"])
    if rng >= 4: return dict(hs=0.95, bw=0.85, weapons=["bow"], extras=["cape"])
    if rng >= 3: return dict(hs=0.95, bw=0.85, robe=True, weapons=["staff"], extras=["hat"])
    return dict(hs=1.0, bw=1.0, weapons=["sword"], extras=["hood"])

# ------------------------------------------------------------------------------------------------------------------------------ non-humanoids

def build_skeleton(m):
    build_humanoid(m, dict(bw=0.6, weapons=["sword"], extras=[]))
    m.tris = [(t, m.pal["K"] if c == m.pal["P"] else c) for t, c in m.tris]
def build_ghost(m):
    m.add(cyl(0, 0.15, 0, 0.32, 0.0, 0.9, 8), "P"); m.add(sphere(0, 1.15, 0, 0.26, 0.24, 0.26), "G")
    for sx in (-1, 1): m.add(box(sx * 0.08, 1.18, 0.23, 0.07, 0.08, 0.02), "D")
    m.sockets = {"feet": (0, 0, 0), "chest": (0, 0.7, 0.1), "cast_origin": (0, 0.8, 0.3), "muzzle": (0, 0.9, 0.4), "head_top": (0, 1.45, 0), "overhead": (0, 1.8, 0)}
def build_blob(m):
    m.add(sphere(0, 0.45, 0, 0.55, 0.45, 0.55, 5, 10), "P")
    for sx in (-1, 1): m.add(sphere(sx * 0.18, 0.62, 0.42, 0.09), "K"); m.add(sphere(sx * 0.18, 0.62, 0.5, 0.04), "D")
    m.sockets = {"feet": (0, 0, 0), "chest": (0, 0.45, 0.3), "cast_origin": (0, 0.5, 0.55), "muzzle": (0, 0.5, 0.6), "head_top": (0, 0.9, 0), "overhead": (0, 1.3, 0)}
def build_spitter(m):
    m.add(sphere(0, 0.4, 0, 0.36, 0.34, 0.36), "P"); m.add(cyl(0, 0.55, 0.25, 0.05, 0.16, 0.3), "S");
    m.add(sphere(0.14, 0.55, 0.3, 0.06), "K"); m.add(sphere(-0.14, 0.55, 0.3, 0.06), "K")
    m.sockets = {"feet": (0, 0, 0), "chest": (0, 0.4, 0.2), "cast_origin": (0, 0.7, 0.35), "muzzle": (0, 0.8, 0.45), "head_top": (0, 0.85, 0), "overhead": (0, 1.2, 0)}
def build_boulder(m):
    m.add(box(0, 0.55, 0, 1.0, 1.1, 0.8), "P"); m.add(box(0.05, 1.3, 0.05, 0.6, 0.5, 0.55), "S")
    for sx in (-1, 1): m.add(box(sx * 0.62, 0.7, 0.1, 0.3, 0.95, 0.3), "S"); m.add(box(sx * 0.15, 1.34, 0.34, 0.1, 0.08, 0.05), "G")
    m.sockets = {"feet": (0, 0, 0), "chest": (0, 0.8, 0.4), "cast_origin": (0, 0.9, 0.6), "muzzle": (0, 0.9, 0.7), "head_top": (0, 1.6, 0), "overhead": (0, 2.0, 0)}
def build_wraith(m):
    build_humanoid(m, dict(bw=1.3, robe=True, weapons=["fist"], extras=["hood", "horns", "shoulders", "wings"]))
    for sx in (-1, 1): m.add(sphere(sx * 0.5, 0.7, 0.25, 0.2), "G")

# ------------------------------------------------------------------------------------------------------------------------------ glTF (.glb) writer + reader (for the self-check)

def write_glb(path, name, tris, base_rgb, alpha=1.0):
    pos, nrm, col = bytearray(), bytearray(), bytearray()
    lo, hi = [1e9] * 3, [-1e9] * 3
    for (a, b, c), colour in tris:
        n = norm(cross(sub(b, a), sub(c, a)))
        for p in (a, b, c):
            pos += struct.pack("<3f", *p); nrm += struct.pack("<3f", *n); col += struct.pack("<4f", *(lin(colour) + (alpha,)))
            for k in range(3): lo[k] = min(lo[k], p[k]); hi[k] = max(hi[k], p[k])
    count = len(tris) * 3
    idx = struct.pack("<%dI" % count, *range(count))
    blob = bytes(pos) + bytes(nrm) + bytes(col) + idx
    js = {
        "asset": {"version": "2.0", "generator": "W2F make_blockouts.py"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2}, "indices": 3, "material": 0, "mode": 4}]}],
        "materials": [dict({"name": "M_Blockout", "pbrMetallicRoughness": {"baseColorFactor": list(lin(base_rgb)) + [alpha], "metallicFactor": 0.0, "roughnessFactor": 0.85}, "doubleSided": False},
                           **({"alphaMode": "BLEND"} if alpha < 1 else {}))],
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos), "target": 34962}, {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nrm), "target": 34962},
                        {"buffer": 0, "byteOffset": len(pos) + len(nrm), "byteLength": len(col), "target": 34962}, {"buffer": 0, "byteOffset": len(pos) + len(nrm) + len(col), "byteLength": len(idx), "target": 34963}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3", "min": lo, "max": hi}, {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5126, "count": count, "type": "VEC4"}, {"bufferView": 3, "componentType": 5125, "count": count, "type": "SCALAR"}],
    }
    j = json.dumps(js, separators=(",", ":")).encode(); j += b" " * (-len(j) % 4)
    blob += b"\0" * (-len(blob) % 4)
    total = 12 + 8 + len(j) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(j), b"JSON") + j + struct.pack("<I4s", len(blob), b"BIN\0") + blob)
    return lo, hi

def read_glb(path):
    """Parses a .glb written above and checks it is well-formed; returns (triangle count, min, max). Raises AssertionError otherwise."""
    d = open(path, "rb").read()
    magic, ver, total = struct.unpack_from("<4sII", d, 0)
    assert magic == b"glTF" and ver == 2 and total == len(d), "bad header"
    jl, jt = struct.unpack_from("<I4s", d, 12); assert jt == b"JSON"
    js = json.loads(d[20:20 + jl]); off = 20 + jl
    bl, bt = struct.unpack_from("<I4s", d, off); assert bt == b"BIN\0"
    blob = d[off + 8:off + 8 + bl]
    assert js["buffers"][0]["byteLength"] <= len(blob)
    acc, views = js["accessors"], js["bufferViews"]
    for a in acc:
        v = views[a["bufferView"]]; size = {"VEC3": 12, "VEC4": 16, "SCALAR": 4}[a["type"]]
        assert v["byteOffset"] + a["count"] * size <= len(blob) and a["count"] % 3 == 0
    n = acc[0]["count"]
    pos = struct.unpack_from("<%df" % (n * 3), blob, views[0]["byteOffset"])
    nor = struct.unpack_from("<%df" % (n * 3), blob, views[1]["byteOffset"])
    col = struct.unpack_from("<%df" % (n * 4), blob, views[2]["byteOffset"])
    idx = struct.unpack_from("<%dI" % n, blob, views[3]["byteOffset"])
    assert max(idx) < n and all(0.0 <= c <= 1.0 for c in col), "index / colour out of range"
    for i in range(0, len(nor), 3): assert abs(math.sqrt(nor[i] ** 2 + nor[i + 1] ** 2 + nor[i + 2] ** 2) - 1) < 1e-3, "normal not unit length"
    lo = [min(pos[k::3]) for k in range(3)]; hi = [max(pos[k::3]) for k in range(3)]
    assert all(abs(lo[k] - acc[0]["min"][k]) < 1e-4 and abs(hi[k] - acc[0]["max"][k]) < 1e-4 for k in range(3)), "min/max do not match the data"
    assert js["materials"][0]["name"] == "M_Blockout"
    return n // 3, lo, hi, pos, nor, col

# ------------------------------------------------------------------------------------------------------------------------------ software preview (a z-buffered contact sheet, PNG)

FONT = {"0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111", "4": "101101111001001", "5": "111100111001111",
        "6": "111100111101111", "7": "111001001001001", "8": "111101111101111", "9": "111101111001111"}

def render_cell(model_data, w, h, yaw=35.0, pitch=18.0):
    """Renders (pos, nor, col) triangles orthographically into a w*h list of (r,g,b) with a z-buffer."""
    pos, nor, col = model_data
    lo = [min(pos[k::3]) for k in range(3)]; hi = [max(pos[k::3]) for k in range(3)]
    ctr = [(lo[k] + hi[k]) / 2 for k in range(3)]
    sc = min((h - 30) / max(hi[1] - lo[1] + 0.2, 1.0), (w - 20) / max(math.hypot(hi[0] - lo[0], hi[2] - lo[2]), 1.0))
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw)); cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    light = norm((0.4, 0.8, 0.6))
    px = [(0.93, 0.94, 0.96)] * (w * h); zb = [-1e9] * (w * h)
    def proj(i):
        x, y, z = pos[3 * i] - ctr[0], pos[3 * i + 1] - ctr[1], pos[3 * i + 2] - ctr[2]
        x, z = x * cy + z * sy, -x * sy + z * cy            # yaw about Y
        y, z = y * cp - z * sp, y * sp + z * cp             # pitch about X
        return (w / 2 + x * sc, h / 2 - 8 - y * sc, z)      # the viewer looks along -z, so a LARGER z is nearer
    for t in range(len(pos) // 9):
        i0 = 3 * t; a, b, c = proj(i0), proj(i0 + 1), proj(i0 + 2)
        # back-face cull with the rotated normal
        nx, ny, nz = nor[3 * i0], nor[3 * i0 + 1], nor[3 * i0 + 2]
        rz = -nx * sy + nz * cy
        rz2 = ny * sp + rz * cp
        if rz2 <= 0.0: continue
        shade = 0.42 + 0.58 * max(0.0, dot((nx, ny, nz), light))
        colour = tuple(min(1.0, (col[4 * i0 + k] ** (1 / 2.2)) * shade) for k in range(3))
        minx, maxx = int(max(0, min(a[0], b[0], c[0]))), int(min(w - 1, max(a[0], b[0], c[0]) + 1))
        miny, maxy = int(max(0, min(a[1], b[1], c[1]))), int(min(h - 1, max(a[1], b[1], c[1]) + 1))
        den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(den) < 1e-9: continue
        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                u = ((b[1] - c[1]) * (x + 0.5 - c[0]) + (c[0] - b[0]) * (y + 0.5 - c[1])) / den
                v = ((c[1] - a[1]) * (x + 0.5 - c[0]) + (a[0] - c[0]) * (y + 0.5 - c[1])) / den
                if u < -0.001 or v < -0.001 or u + v > 1.001: continue
                z = u * a[2] + v * b[2] + (1 - u - v) * c[2]
                k = y * w + x
                if z > zb[k]: zb[k] = z; px[k] = colour
    return px

def draw_number(px, w, x0, y0, text, colour=(0.15, 0.15, 0.2)):
    for ci, ch in enumerate(text):
        g = FONT[ch]
        for r in range(5):
            for c in range(3):
                if g[r * 3 + c] == "1":
                    for dy in range(2):
                        for dx in range(2): px[(y0 + r * 2 + dy) * w + x0 + ci * 8 + c * 2 + dx] = colour

def write_png(path, w, h, px):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            r, g, b = px[y * w + x]; raw += bytes((int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)))
    def chunk(t, d): c = struct.pack(">I", len(d)) + t + d; return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))

# ------------------------------------------------------------------------------------------------------------------------------ main

# ------------------------------------------------------------------------------------------------------------------------------ the arena set (board tiles, bench, island, backdrop)
# All symmetric about their own origin (so an importer that mirrors an axis cannot misplace them); the viewer positions them. Metres, +Y up, +Z = the "forward" (rows), +X = sideways (columns).

def coloured(tris, hexcolour):
    return [(t, srgb(hexcolour)) for t in tris]

def hex_tile(fill, outline, R=1.0 / math.sqrt(3)):
    """A pointy-top hex tile like an auto-battler board: a coloured outline ring around an inset fill, 12 cm thick, top face at y = 0."""
    pts = lambda r: [(r * math.sin(math.radians(60 * i)), r * math.cos(math.radians(60 * i))) for i in range(6)]
    outer, inner = pts(R), pts(R * 0.90)
    mid = (0.0, -0.06, 0.0)
    ring_t, fill_t, side_t = [], [], []
    for i in range(6):
        j = (i + 1) % 6
        (ax, az), (bx, bz), (cx_, cz), (dx, dz) = outer[i], outer[j], inner[j], inner[i]
        ring_t += _tri((ax, 0, az), (bx, 0, bz), (cx_, 0, cz), mid) + _tri((ax, 0, az), (cx_, 0, cz), (dx, 0, dz), mid)
        fill_t += _tri((0, -0.008, 0), (cx_, -0.008, cz), (dx, -0.008, dz), mid)
        side_t += _tri((ax, -0.12, az), (bx, -0.12, bz), (bx, 0, bz), mid) + _tri((ax, -0.12, az), (bx, 0, bz), (ax, 0, az), mid)
        side_t += _tri((0, -0.12, 0), (ax, -0.12, az), (bx, -0.12, bz), mid)
        side_t += _tri((dx, -0.008, dz), (cx_, -0.008, cz), (cx_, 0, cz), mid) + _tri((dx, -0.008, dz), (cx_, 0, cz), (dx, 0, dz), mid)   # the step down into the fill
    return coloured(fill_t, fill) + coloured(ring_t, outline) + coloured(side_t, outline)

def bench_slot():
    """One bench slot: a stone slab with a lighter raised plate, 80 cm square, top at y = 0."""
    return coloured(box(0, -0.0625, 0, 0.80, 0.125, 0.80), "8E897D") + coloured(box(0, 0.0, 0, 0.66, 0.012, 0.66), "B5B0A0")

def arena_base():
    """The floating island the board sits on (11.6 m along Z, 9 m along X, grass top at y = 0): room for both benches. A stone rim, four brazier pillars and a few pines."""
    L = 11.6
    t = []
    t += coloured(box(0, -0.25, 0, 9.0, 0.5, L), "5E8C4A")                                       # grass
    t += coloured(box(0, -1.2, 0, 8.4, 1.4, L - 0.6), "6B5B4A") + coloured(box(0, -2.4, 0, 6.0, 1.0, L - 3.0), "574A3C")   # rock underneath
    for sz in (-1, 1): t += coloured(box(0, 0.09, sz * (L / 2 - 0.15), 9.0, 0.18, 0.30), "9C978A")   # rim
    for sx in (-1, 1): t += coloured(box(sx * 4.35, 0.09, 0, 0.30, 0.18, L), "9C978A")
    for sx in (-1, 1):
        for sz in (-1, 1):
            px, pz = sx * 4.1, sz * (L / 2 - 0.4)
            t += coloured(box(px, 0.55, pz, 0.5, 1.1, 0.5), "9C978A") + coloured(cyl(px, 1.1, pz, 0.30, 0.22, 0.14, 8), "3A3A42") + coloured(cyl(px, 1.24, pz, 0.16, 0.0, 0.42, 6), "FFA733")
    for sx in (-1, 1):                                                                             # pines down both long sides
        for pz in (-4.0, -2.0, 0.0, 2.0, 4.0):
            px = sx * 4.15
            t += coloured(cyl(px, 0.0, pz, 0.09, 0.09, 0.4, 5), "6B4A2B") + coloured(cyl(px, 0.3, pz, 0.5, 0.0, 1.3, 7), "3F7A3A") + coloured(cyl(px, 0.9, pz, 0.38, 0.0, 1.0, 7), "4C8E45")
    return t

def backdrop():
    return coloured(box(0, -0.05, 0, 80.0, 0.1, 80.0), "A9CBEA")

def safe(name): return "".join(ch for ch in name if ch.isalnum())

def main():
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--data", default=os.path.join(here, "..", "data")); ap.add_argument("--out", default=os.path.join(here, "..", "docs", "blockouts"))
    ap.add_argument("--no-preview", action="store_true")
    args = ap.parse_args()
    champs = load_json(os.path.join(args.data, "champions.json"))["champions"]
    monsters = load_json(os.path.join(args.data, "pve.json"))["monsters"]
    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.startswith("SM_") and f.endswith(".glb"): os.remove(os.path.join(args.out, f))

    jobs = []   # (kind, id, name, model, alpha)
    for c in champs:
        cid, traits = c["id"], c.get("traits", [])
        if c.get("summon"):
            if c["name"] == "Skeleton": m = Model(palette([], True)); build_skeleton(m); jobs.append(("Summon", cid, c["name"], m, 1.0, 0.85))
            else: m = Model(palette(["Coregons"], True)); build_ghost(m); jobs.append(("Summon", cid, c["name"], m, 0.65, 1.0))
            continue
        rec = dict(RECIPES.get(cid) or fallback_recipe(c.get("role", ""), c["stats"].get("range", 1)))
        m = Model(palette(traits, rec.get("undead", False)))
        build_humanoid(m, rec)
        jobs.append(("Champion", cid, c["name"], m, 1.0, rec.get("hs", 1.0) * (0.92 + 0.05 * c["cost"])))
    monster_builders = {10001: (build_blob, ["Coregons"]), 10002: (build_spitter, ["Selini"]), 10003: (build_boulder, []), 10004: (build_wraith, ["Phaisa"])}
    for mo in monsters:
        fn, tr = monster_builders.get(mo["id"], (build_blob, ["Hexagon"]))
        m = Model(palette(tr)); fn(m)
        jobs.append(("Monster", mo["id"], mo["name"], m, 1.0, {10004: 1.5, 10003: 1.2}.get(mo["id"], 1.0)))

    manifest, sheet = {"units": "metres, +Y up, facing +Z, feet at the origin (glTF); Unreal converts to cm, +Z up, facing +X", "models": []}, []
    for kind, mid, name, m, alpha, s in jobs:
        tris = [(tuple(tuple(p * s for p in q) for q in t), c) for t, c in m.tris]
        fname = "SM_%s_%d_%s.glb" % (kind, mid, safe(name))
        lo, hi = write_glb(os.path.join(args.out, fname), fname[:-4], tris, m.pal["P"], alpha)
        n, rlo, rhi, pos, nor, col = read_glb(os.path.join(args.out, fname))
        height = rhi[1] - rlo[1]
        assert rlo[1] > -0.01 and 0.5 < rhi[1] < 3.5 and n > 20, "%s: implausible size %s %s" % (name, rlo, rhi)
        manifest["models"].append({"kind": kind.lower(), "id": mid, "name": name, "file": fname, "triangles": n, "height_m": round(height, 3),
                                   "width_m": round(rhi[0] - rlo[0], 3), "depth_m": round(rhi[2] - rlo[2], 3),
                                   "primary_colour": "#" + "".join("%02X" % round(v * 255) for v in m.pal["P"]),
                                   "sockets_m": {k: {"forward": round(v[2] * s, 3), "up": round(v[1] * s, 3)} for k, v in m.sockets.items()}})
        sheet.append((mid, pos, nor, col))

    # ---- board / effect helpers
    R = 1.0 / math.sqrt(3)                       # pointy-top hex: 1 m across the flats
    hexm = Model(palette([])); ring_pts = [(R * math.sin(math.radians(60 * i)), R * math.cos(math.radians(60 * i))) for i in range(6)]
    tile, mid = [], (0.0, -0.06, 0.0)          # the top face is at y = 0, the tile is 12 cm thick
    for i in range(6):
        a, b = ring_pts[i], ring_pts[(i + 1) % 6]
        tile += _tri((0, 0, 0), (b[0], 0, b[1]), (a[0], 0, a[1]), mid) + _tri((0, -0.12, 0), (a[0], -0.12, a[1]), (b[0], -0.12, b[1]), mid)
        tile += _tri((a[0], -0.12, a[1]), (b[0], -0.12, b[1]), (b[0], 0, b[1]), mid) + _tri((a[0], -0.12, a[1]), (b[0], 0, b[1]), (a[0], 0, a[1]), mid)
    tile = [(t, srgb("6C7A89")) for t in tile]
    extras = [("SM_HexTile", tile, (0.42, 0.48, 0.54), 1.0),
              ("SM_HexTile_Home", hex_tile("B7B26B", "3FB6B0"), srgb("B7B26B"), 1.0),      # warm field, teal outline: your half
              ("SM_HexTile_Away", hex_tile("B98F6A", "D6664F"), srgb("B98F6A"), 1.0),      # clay field, red outline: the enemy half
              ("SM_BenchSlot", bench_slot(), srgb("8E897D"), 1.0),
              ("SM_ArenaBase", arena_base(), srgb("5E8C4A"), 1.0),
              ("SM_Backdrop", backdrop(), srgb("A9CBEA"), 1.0),
              ("SM_ProjectileOrb", [(t, srgb("FFE9A8")) for t in sphere(0, 0, 0, 0.12, lat=4, lon=8)], srgb("FFE9A8"), 1.0),
              ("SM_AreaDisc", [(t, srgb("FF7043")) for t in cyl(0, 0, 0, 1.0, 1.0, 0.02, 24)], srgb("FF7043"), 0.5)]
    for name, tris, base, alpha in extras:
        write_glb(os.path.join(args.out, name + ".glb"), name, tris, base, alpha); read_glb(os.path.join(args.out, name + ".glb"))
        manifest["models"].append({"kind": "helper", "id": 0, "name": name, "file": name + ".glb"})
    manifest["board"] = {"hex_orientation": "pointy-top, odd rows shifted half a hex to the right (odd-r); 1 m across the flats",
                         "cell_centre_m": "x_m = (column + 0.5 * (row & 1)) * 1.0 ; z_m = row * 1.5 * (1 / sqrt(3))  (arena: 7 columns x 8 rows; rows 0-3 one side, 4-7 the other)"}
    json.dump(manifest, open(os.path.join(args.out, "manifest.json"), "w"), indent=1); open(os.path.join(args.out, "manifest.json"), "a").write("\n")
    print("wrote %d models + %d helpers to %s (largest %d triangles)" % (len(jobs), len(extras), args.out, max(m["triangles"] for m in manifest["models"] if "triangles" in m)))

    if not args.no_preview:
        cols, cw, ch = 6, 170, 240
        rows = (len(sheet) + cols - 1) // cols
        W, H = cols * cw, rows * ch
        px = [(0.93, 0.94, 0.96)] * (W * H)
        for i, (mid, pos, nor, col) in enumerate(sheet):
            cell = render_cell((pos, nor, col), cw, ch)
            ox, oy = (i % cols) * cw, (i // cols) * ch
            for y in range(ch):
                px[(oy + y) * W + ox:(oy + y) * W + ox + cw] = cell[y * cw:(y + 1) * cw]
            for x in range(cw): px[(oy + ch - 1) * W + ox + x] = (0.75, 0.77, 0.8)
            for y in range(ch): px[(oy + y) * W + ox + cw - 1] = (0.75, 0.77, 0.8)
            draw_number(px, W, ox + 6, oy + 6, str(mid))
        write_png(os.path.join(args.out, "contact_sheet.png"), W, H, px)
        print("wrote contact_sheet.png (%dx%d, %d models)" % (W, H, len(sheet)))

if __name__ == "__main__":
    main()
