"""Builds the effect meshes for the fight VFX (tools/unreal/import_heroes.py imports them into /Game/W2F/FX, AW2FArena's UW2FFx plays them).

    ~/w2f_bpy/venv/bin/python tools/mixamo/build_fx_meshes.py [--out DIR]

Every mesh is unit sized (1 m = 100 cm in UE) so the game scales it freely, and made for an additive, unlit, two-sided material:
  FX_Ring      flat ring, radius 1 (XY plane)                FX_Disc     filled circle, radius 1, soft centre (vertex alpha not needed: additive)
  FX_Cone      120-degree sector toward +X, radius 1          FX_Beam     cylinder radius 1 from x = 0 to x = 1 (stretched between two points)
  FX_Pillar    cylinder radius 1 from z = 0 to z = 1           FX_Sphere   sphere radius 1
  FX_Dome      half sphere radius 1 (z >= 0)                   FX_Arrow    an arrow along +X, length 1, tip at x = 1
  FX_Bolt      a comet: head at the origin, tail toward -X      FX_Slash    a 150-degree crescent band around +X, radius 1
  FX_Spike     a cone base at z = 0, tip at z = 1              FX_Star     a flat 4-point star, radius 1
  FX_Shard     a crystal (stretched octahedron), z 0..1        FX_Swirl    a spiral ribbon around Z, radius 1, z 0..1, 1.5 turns
The glTF importer keeps +X and +Z; Blender's +Y becomes UE's -Y, so every directional mesh points along +X.
"""
import math
import os
import sys

import bpy  # noqa: I001
import bmesh

OUT = os.path.expanduser("~/Desktop/work2fightgame/work2fightgame/SourceArt/FX")


def new_bm():
    return bmesh.new()


def ring_verts(bm, r, z, n, a0=0.0, a1=2 * math.pi, closed=True):
    count = n if closed else n + 1
    return [bm.verts.new((r * math.cos(a0 + (a1 - a0) * i / n), r * math.sin(a0 + (a1 - a0) * i / n), z)) for i in range(count)]


def bridge(bm, a, b, closed=True):
    n = len(a)
    for i in range(n if closed else n - 1):
        j = (i + 1) % n
        bm.faces.new((a[i], a[j], b[j], b[i]))


def fx_ring(bm):
    inner, outer = ring_verts(bm, 0.9, 0, 64), ring_verts(bm, 1.0, 0, 64)
    bridge(bm, inner, outer)


def fx_disc(bm):
    rings = [ring_verts(bm, r, 0, 48) for r in (0.25, 0.6, 1.0)]
    centre = bm.verts.new((0, 0, 0))
    for i in range(48):
        bm.faces.new((centre, rings[0][i], rings[0][(i + 1) % 48]))
    bridge(bm, rings[0], rings[1])
    bridge(bm, rings[1], rings[2])


def fx_cone(bm):
    a0, a1 = math.radians(-60), math.radians(60)
    arc = ring_verts(bm, 1.0, 0, 24, a0, a1, closed=False)
    mid = ring_verts(bm, 0.5, 0, 24, a0, a1, closed=False)
    tip = bm.verts.new((0, 0, 0))
    for i in range(24):
        bm.faces.new((tip, mid[i], mid[i + 1]))
    bridge(bm, mid, arc, closed=False)


def fx_beam(bm):
    n = 12
    a = [bm.verts.new((0.0, math.cos(2 * math.pi * i / n), math.sin(2 * math.pi * i / n))) for i in range(n)]
    b = [bm.verts.new((1.0, math.cos(2 * math.pi * i / n), math.sin(2 * math.pi * i / n))) for i in range(n)]
    bridge(bm, a, b)


def fx_pillar(bm):
    a, b = ring_verts(bm, 1.0, 0.0, 16), ring_verts(bm, 1.0, 1.0, 16)
    bridge(bm, a, b)


def fx_sphere(bm):
    bmesh.ops.create_uvsphere(bm, u_segments=20, v_segments=12, radius=1.0)


def fx_dome(bm):
    rings = []
    for k in range(7):
        phi = (math.pi / 2) * k / 6
        rings.append(ring_verts(bm, math.cos(phi) + 1e-4, math.sin(phi), 24))
    for k in range(6):
        bridge(bm, rings[k], rings[k + 1])


def fx_arrow(bm):
    def tube(x0, x1, r0, r1, n=6):
        a = [bm.verts.new((x0, r0 * math.cos(2 * math.pi * i / n), r0 * math.sin(2 * math.pi * i / n))) for i in range(n)]
        b = [bm.verts.new((x1, r1 * math.cos(2 * math.pi * i / n), r1 * math.sin(2 * math.pi * i / n))) for i in range(n)]
        bridge(bm, a, b)
    tube(0.0, 0.8, 0.018, 0.018)
    tube(0.78, 1.0, 0.055, 0.0005)
    for k in range(3):   # fletching
        ang = 2 * math.pi * k / 3
        c, s = math.cos(ang), math.sin(ang)
        v = [bm.verts.new(p) for p in ((0.02, 0.018 * c, 0.018 * s), (0.2, 0.018 * c, 0.018 * s), (0.08, 0.08 * c, 0.08 * s))]
        bm.faces.new(v)


def fx_bolt(bm):
    bmesh.ops.create_uvsphere(bm, u_segments=12, v_segments=8, radius=0.28)
    n = 10
    base = [bm.verts.new((0.0, 0.24 * math.cos(2 * math.pi * i / n), 0.24 * math.sin(2 * math.pi * i / n))) for i in range(n)]
    tail = bm.verts.new((-1.0, 0, 0))
    for i in range(n):
        bm.faces.new((base[i], base[(i + 1) % n], tail))


def fx_slash(bm):
    a0, a1 = math.radians(-75), math.radians(75)
    n = 32
    inner, outer = [], []
    for i in range(n + 1):
        t = i / n
        a = a0 + (a1 - a0) * t
        w = 0.28 * math.sin(math.pi * t) + 0.01     # thick in the middle, sharp at the ends
        inner.append(bm.verts.new(((1.0 - w) * math.cos(a), (1.0 - w) * math.sin(a), 0)))
        outer.append(bm.verts.new((math.cos(a), math.sin(a), 0)))
    bridge(bm, inner, outer, closed=False)


def fx_spike(bm):
    n = 8
    base = ring_verts(bm, 0.18, 0.0, n)
    tip = bm.verts.new((0, 0, 1.0))
    for i in range(n):
        bm.faces.new((base[i], base[(i + 1) % n], tip))


def fx_star(bm):
    pts = []
    for i in range(8):
        r = 1.0 if i % 2 == 0 else 0.3
        a = math.pi / 2 * (i / 2)
        pts.append(bm.verts.new((r * math.cos(a), r * math.sin(a), 0)))
    c = bm.verts.new((0, 0, 0))
    for i in range(8):
        bm.faces.new((c, pts[i], pts[(i + 1) % 8]))


def fx_shard(bm):
    top, bottom = bm.verts.new((0, 0, 1.0)), bm.verts.new((0, 0, 0.0))
    mid = [bm.verts.new((0.22 * math.cos(a), 0.22 * math.sin(a), 0.32)) for a in [2 * math.pi * i / 5 for i in range(5)]]
    for i in range(5):
        j = (i + 1) % 5
        bm.faces.new((mid[i], mid[j], top))
        bm.faces.new((mid[j], mid[i], bottom))


def fx_swirl(bm):
    n = 60
    inner, outer = [], []
    for i in range(n + 1):
        t = i / n
        a = 3 * math.pi * t
        z = t
        inner.append(bm.verts.new((0.85 * math.cos(a), 0.85 * math.sin(a), z)))
        outer.append(bm.verts.new((1.0 * math.cos(a), 1.0 * math.sin(a), z + 0.06)))
    bridge(bm, inner, outer, closed=False)


def fx_quad(bm):
    """A 1 m quad in the YZ plane facing +X (a camera-facing sprite: the game turns +X toward the camera), with UVs."""
    uv = bm.loops.layers.uv.new("UVMap")
    v = [bm.verts.new(p) for p in ((0, -0.5, -0.5), (0, 0.5, -0.5), (0, 0.5, 0.5), (0, -0.5, 0.5))]
    f = bm.faces.new(v)
    for loop, (a, b) in zip(f.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
        loop[uv].uv = (a, b)


MESHES = {"FX_Quad": fx_quad, "FX_Ring": fx_ring, "FX_Disc": fx_disc, "FX_Cone": fx_cone, "FX_Beam": fx_beam, "FX_Pillar": fx_pillar, "FX_Sphere": fx_sphere, "FX_Dome": fx_dome,
          "FX_Arrow": fx_arrow, "FX_Bolt": fx_bolt, "FX_Slash": fx_slash, "FX_Spike": fx_spike, "FX_Star": fx_star, "FX_Shard": fx_shard, "FX_Swirl": fx_swirl}


# ---- soft sprite textures (grey masks: the material multiplies them by the particle's colour) ----
def textures(out):
    import numpy as np
    n = 256
    y, x = np.mgrid[-1:1:n * 1j, -1:1:n * 1j]
    r = np.sqrt(x * x + y * y)
    ang = np.arctan2(y, x)
    rng = np.random.default_rng(7)

    def noise(scale):
        g = rng.random((scale, scale))
        from numpy import kron
        up = kron(g, np.ones((n // scale, n // scale)))
        return up

    def fbm():
        acc = np.zeros((n, n))
        for k, w in ((4, 0.5), (8, 0.25), (16, 0.15), (32, 0.1)):
            acc += w * noise(k)
        # a cheap blur so the blocks melt
        for _ in range(6):
            acc = (acc + np.roll(acc, 1, 0) + np.roll(acc, -1, 0) + np.roll(acc, 1, 1) + np.roll(acc, -1, 1)) / 5
        return (acc - acc.min()) / (acc.max() - acc.min())
    def smoke():
        """a soft cloud: overlapping gaussian puffs, fading toward the edge"""
        acc = np.zeros((n, n))
        for _ in range(28):
            cx, cy = rng.uniform(-0.45, 0.45, 2)
            rad = rng.uniform(0.12, 0.35)
            acc += rng.uniform(0.4, 1.0) * np.exp(-(((x - cx) ** 2 + (y - cy) ** 2) / (rad * rad)))
        acc = acc / acc.max()
        return np.clip(acc * np.clip(1.15 - r, 0, 1) ** 1.2, 0, 1)
    tex = {
        "T_FX_Glow": np.exp(-(r ** 2) * 5.0) * (r < 1),
        "T_FX_Spark": np.clip(np.exp(-(r ** 2) * 60) + 0.8 * np.exp(-(x ** 2) * 900) * np.exp(-(y ** 2) * 4) + 0.8 * np.exp(-(y ** 2) * 900) * np.exp(-(x ** 2) * 4), 0, 1),
        "T_FX_Ring": np.exp(-((r - 0.78) ** 2) * 180) + 0.12 * np.exp(-(r ** 2) * 2) * (r < 0.78),
        "T_FX_Shock": np.clip(np.where(r < 0.92, (r / 0.92) ** 4, np.exp(-((r - 0.92) ** 2) * 900)), 0, 1),
        "T_FX_Smoke": smoke(),
        "T_FX_Streak": np.exp(-(y ** 2) * 70 - (x ** 2) * 1.6),
        "T_FX_Flare": np.clip(np.exp(-(r ** 2) * 14) + 0.5 * np.exp(-(y ** 2) * 400 - (x ** 2) * 1.2) + 0.25 * np.exp(-(r ** 2) * 3), 0, 1),
    }
    # a rune circle: two rings, six spokes, a hexagram (summons, runes, Najmi / Hexagon flourishes)
    rune = np.exp(-((r - 0.9) ** 2) * 900) + np.exp(-((r - 0.78) ** 2) * 1500)
    for k in range(6):
        a = k * np.pi / 3
        d = np.abs(x * np.sin(a) - y * np.cos(a))
        rune += np.exp(-(d ** 2) * 3000) * (r < 0.78) * (r > 0.2)
    for k in range(2):
        pts = [(0.72 * np.cos(np.pi / 2 + k * np.pi / 3 + i * 2 * np.pi / 3), 0.72 * np.sin(np.pi / 2 + k * np.pi / 3 + i * 2 * np.pi / 3)) for i in range(3)]
        for i in range(3):
            (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % 3]
            dx, dy = x1 - x0, y1 - y0
            t = np.clip(((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy), 0, 1)
            d = np.sqrt((x - x0 - t * dx) ** 2 + (y - y0 - t * dy) ** 2)
            rune += np.exp(-(d ** 2) * 2500)
    tex["T_FX_Rune"] = np.clip(rune, 0, 1)
    for name, a in tex.items():
        a = np.clip(a, 0, 1).astype(np.float32)
        img = bpy.data.images.new(name, n, n, alpha=True)
        px = np.stack([a, a, a, np.ones_like(a)], -1)[::-1]
        img.pixels[:] = px.ravel()
        img.filepath_raw = os.path.join(out, name + ".png")
        img.file_format = "PNG"
        img.save()
        print("[fx] texture %s" % name)


def main():
    out = OUT
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out, exist_ok=True)
    textures(out)
    for name, build in MESHES.items():
        bpy.ops.wm.read_factory_settings(use_empty=True)
        me = bpy.data.meshes.new(name)
        bm = new_bm()
        build(bm)
        bm.normal_update()
        bm.to_mesh(me)
        bm.free()
        obj = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(obj)
        mat = bpy.data.materials.new("M_" + name)
        me.materials.append(mat)
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        path = os.path.join(out, name + ".glb")
        bpy.ops.export_scene.gltf(filepath=path, use_selection=True, export_format="GLB", export_materials="PLACEHOLDER", export_normals=True, export_texcoords=True)
        print("[fx] %s: %d faces -> %s" % (name, len(me.polygons), path))


if __name__ == "__main__":
    main()
