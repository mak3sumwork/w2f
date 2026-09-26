"""The W2F stage in the style of TFT's space arenas: a floating dark-navy metal platform with panel seams, gold trim, cyan / magenta light strips, four
crystal pylons (the game puts its point lights on them), a thruster hull underneath, subtle hex plates with a glowing rim, gold-framed bench pads and a
nebula sky dome.

The alternative theme (the default is arena_classic.py): the game uses these pieces (suffix _Space) with -w2farena=space.

One piece per run (the scene is not reset between pieces):
    ~/w2f_bpy/venv/bin/python tools/blender/arena_space.py --piece base|home|away|bench|sky [--preview DIR] [--no-bake]

Orientation: the game spawns SM_ArenaBase with yaw 90, so Blender +Y is the home -> away axis (rows) and Blender X runs along a row (columns).
Board: 7.06 m (Y) x 7.5 m (X) around the origin; the benches sit at Y = -4.48 / +4.48 and span X = -3.7 .. 3.7. The floor's top is z = 0.
"""
import math, os, random
import bl_kit
from bl_kit import *

PIECE = ARGS[ARGS.index("--piece") + 1] if "--piece" in ARGS else "base"
random.seed(7)

NAVY = (0.028, 0.04, 0.085)
NAVY_2 = (0.045, 0.062, 0.125)
STEEL = (0.1, 0.12, 0.19)
GOLD = (0.78, 0.55, 0.2)
CYAN = (0.15, 0.85, 1.0)
MAGENTA = (0.85, 0.2, 0.95)
VIOLET = (0.42, 0.22, 1.0)


def hex_pts(r, z):
    return [(r * math.sin(math.radians(60 * i)), r * math.cos(math.radians(60 * i)), z) for i in range(6)]


def prism(name, pts_lo, pts_hi, mat, bevel=0.0):
    """A closed prism from two matching rings of points (bottom, top)."""
    bm = bmesh.new()
    lo = [bm.verts.new(p) for p in pts_lo]; hi = [bm.verts.new(p) for p in pts_hi]
    bm.faces.new(list(reversed(lo))); bm.faces.new(hi)
    n = len(lo)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new([lo[i], lo[j], hi[j], hi[i]])
    bm.normal_update()
    return finish(make(name, bm), mat, bevel=bevel, smooth=False)


def hex_ring(name, r_out, r_in, z0, z1, mat):
    """A flat hexagonal frame (outline) between two radii."""
    bm = bmesh.new()
    rings = [[bm.verts.new(p) for p in hex_pts(r, z)] for r, z in ((r_out, z0), (r_in, z0), (r_out, z1), (r_in, z1))]
    ob, ib, ot, it = rings
    for i in range(6):
        j = (i + 1) % 6
        bm.faces.new([ot[i], ot[j], it[j], it[i]])        # top
        bm.faces.new([ob[j], ob[i], ib[i], ib[j]])        # bottom
        bm.faces.new([ob[i], ob[j], ot[j], ot[i]])        # outer wall
        bm.faces.new([ib[j], ib[i], it[i], it[j]])        # inner wall
    bm.normal_update()
    return finish(make(name, bm), mat, smooth=False)


def octagon(w, l, cut, z):
    """An octagon w (X) by l (Y) with corners cut by `cut`."""
    x, y = w / 2, l / 2
    return [(-x + cut, -y, z), (x - cut, -y, z), (x, -y + cut, z), (x, y - cut, z), (x - cut, y, z), (-x + cut, y, z), (-x, y - cut, z), (-x, -y + cut, z)]


def strip(name, p0, p1, width, mat, z=0.004, h=0.012):
    """A thin straight strip on the floor from p0 to p1 (x, y)."""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    length = math.hypot(dx, dy)
    return box(name, ((p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2, z), (width, length, h), mat, rot=(0, 0, -math.degrees(math.atan2(dx, dy))), bevel=0.0, subsurf=0)


# ------------------------------------------------------------------------------------------------------------------------------ the platform
def base():
    bl_kit.TEX = 4096
    W, L, CUT = 10.8, 12.0, 1.3
    m_navy = material("navy", NAVY, 0.7, 0.4, kind="metal", wear=0.03, color2=NAVY_2)
    m_panel = material("panel", NAVY_2, 0.7, 0.35, kind="metal", wear=0.02)
    m_board = material("board", (0.035, 0.05, 0.11), 0.6, 0.45, kind="metal", wear=0.0)
    m_steel = material("steel", STEEL, 0.8, 0.35, kind="metal", wear=0.05)
    m_gold = material("gold", GOLD, 0.95, 0.25, kind="metal", wear=0.0, color2=(1.0, 0.8, 0.4))
    m_dark = material("dark", (0.012, 0.015, 0.03), 0.3, 0.7)
    g_cyan, g_mag, g_violet = glow("g_cyan", CYAN), glow("g_mag", MAGENTA), glow("g_violet", VIOLET)
    g_gold = glow("g_gold", (1.0, 0.72, 0.3))

    # the slab: an octagon, a gold lip, then a stepped hull going down
    prism("slab", octagon(W, L, CUT, -0.45), octagon(W, L, CUT, -0.06), m_navy, bevel=0.02)
    prism("lip", octagon(W + 0.16, L + 0.16, CUT + 0.07, -0.2), octagon(W + 0.16, L + 0.16, CUT + 0.07, -0.08), m_gold, bevel=0.015)
    for k, (dz, shrink) in enumerate(((-0.8, 0.5), (-1.25, 1.5), (-1.7, 2.9), (-2.1, 4.4))):
        prism("hull%d" % k, octagon(W - shrink - 0.5, L - shrink - 0.6, CUT, dz - 0.4), octagon(W - shrink, L - shrink, CUT, dz), m_steel if k % 2 else m_navy, bevel=0.03)
        if k < 3:   # a glowing band around each step
            prism("band%d" % k, octagon(W - shrink + 0.02, L - shrink + 0.02, CUT, dz - 0.07), octagon(W - shrink + 0.02, L - shrink + 0.02, CUT, dz - 0.03), g_violet if k % 2 else g_cyan)
    for sx in (-1, 1):   # thrusters under the hull
        for sy in (-1, 1):
            cyl_loc = (sx * 1.6, sy * 2.2, -2.75)
            limb("thr%d%d" % (sx, sy), (cyl_loc[0], cyl_loc[1], -2.4), (cyl_loc[0], cyl_loc[1], -2.9), 0.45, 0.32, m_steel, seg=20)
            limb("flame%d%d" % (sx, sy), (cyl_loc[0], cyl_loc[1], -2.9), (cyl_loc[0], cyl_loc[1], -2.96), 0.3, 0.3, g_cyan, seg=20)

    # the floor: 1 m panels with seams (the dark slab shows between them); a darker, flatter plate under the board
    for ix in range(-5, 5):
        for iy in range(-6, 6):
            cx, cy = ix + 0.5, iy + 0.5
            if abs(cx) > W / 2 - 0.1 or abs(cy) > L / 2 - 0.1: continue
            if abs(cx) + abs(cy) > (W + L) / 2 - CUT - 0.55: continue           # the cut corners
            if abs(cx) < 3.8 and abs(cy) < 3.55: continue                         # the board plate goes here
            if abs(cy) > 4.0 and abs(cy) < 4.95 and abs(cx) < 3.9: continue       # the bench channels
            box("p%d_%d" % (ix, iy), (cx, cy, -0.035), (0.96, 0.96, 0.06), m_panel if (ix + iy) % 3 else m_navy, bevel=0.012, subsurf=0)
    box("boardplate", (0, 0, -0.04), (7.72, 7.24, 0.06), m_board, bevel=0.01, subsurf=0)
    for sy in (-1, 1):                                                           # gold frame around the board, glowing strip inside it
        box("bf_y%d" % sy, (0, sy * 3.66, -0.01), (7.96, 0.12, 0.05), m_gold, bevel=0.012, subsurf=0)
        strip("bgl_y%d" % sy, (-3.85, sy * 3.57), (3.85, sy * 3.57), 0.03, g_cyan if sy < 0 else g_mag)
    for sx in (-1, 1):
        box("bf_x%d" % sx, (sx * 3.92, 0, -0.01), (0.12, 7.44, 0.05), m_gold, bevel=0.012, subsurf=0)
        strip("bgl_x%d" % sx, (sx * 3.83, -3.55), (sx * 3.83, 3.55), 0.03, g_violet)
    strip("mid", (-3.8, 0), (3.8, 0), 0.02, g_violet)                             # the line between the two halves
    for sy in (-1, 1):                                                           # the bench channels: recessed, gold rails, a light strip
        y = sy * 4.48
        box("bench_ch%d" % sy, (0, y, -0.05), (7.9, 0.95, 0.03), m_dark, bevel=0.0, subsurf=0)
        for r in (-1, 1):
            box("bench_rail%d%d" % (sy, r), (0, y + r * 0.49, -0.02), (7.95, 0.07, 0.06), m_gold, bevel=0.01, subsurf=0)
        strip("bench_gl%d" % sy, (-3.9, y - sy * 0.43), (3.9, y - sy * 0.43), 0.025, g_cyan if sy < 0 else g_mag, z=-0.03)
    # circuit lines on the outer panels: short glowing traces that turn corners
    for k in range(26):
        sx, sy = random.choice((-1, 1)), random.choice((-1, 1))
        x0 = sx * random.uniform(4.15, 4.95); y0 = sy * random.uniform(0.3, 5.0)
        if abs(x0) + abs(y0) > (W + L) / 2 - CUT - 0.7: continue
        y1 = y0 + sy * random.uniform(0.3, 1.1)
        strip("tr%d" % k, (x0, y0), (x0, y1), 0.018, g_cyan if k % 3 else g_violet)
        strip("tr%db" % k, (x0, y1), (x0 - sx * 0.25, y1), 0.018, g_cyan if k % 3 else g_violet)
        sphere("tn%d" % k, (x0 - sx * 0.25, y1, 0.004), (0.035, 0.035, 0.012), g_cyan if k % 3 else g_mag, seg=10, rings=6)
    # side conduits: long glowing tubes in gold housings along both long edges
    for sx in (-1, 1):
        x = sx * (W / 2 - 0.22)
        box("cond_house%d" % sx, (x, 0, 0.05), (0.26, L - 2 * CUT - 0.4, 0.14), m_steel, bevel=0.03, subsurf=0)
        box("cond_glow%d" % sx, (x, 0, 0.125), (0.08, L - 2 * CUT - 0.6, 0.03), g_cyan, bevel=0.0, subsurf=0)
        for k in range(7):
            box("cond_clamp%d_%d" % (sx, k), (x, -3.6 + k * 1.2, 0.08), (0.32, 0.1, 0.16), m_gold, bevel=0.02, subsurf=0)
    # the far and near edges: a raised gold-trimmed ledge with a glyph plate
    for sy in (-1, 1):
        y = sy * (L / 2 - 0.35)
        box("ledge%d" % sy, (0, y, 0.06), (W - 2 * CUT - 0.3, 0.4, 0.16), m_navy, bevel=0.03, subsurf=0)
        box("ledge_trim%d" % sy, (0, y - sy * 0.21, 0.1), (W - 2 * CUT - 0.3, 0.05, 0.1), m_gold, bevel=0.01, subsurf=0)
        box("ledge_glow%d" % sy, (0, y, 0.145), (2.2, 0.08, 0.02), g_mag if sy > 0 else g_cyan, bevel=0.0, subsurf=0)
        ring("emblem%d" % sy, (0, y, 0.15), 0.16, 0.025, g_gold, seg=24, tseg=6)
    # four crystal pylons on the cut corners (the game's point lights sit at x = +-4.75, y = +-5.25, z = 2.15)
    for sx in (-1, 1):
        for sy in (-1, 1):
            px, py = sx * 4.55, sy * 5.05
            prism("pyl_base%d%d" % (sx, sy), [(px + 0.55 * math.cos(math.radians(a)), py + 0.55 * math.sin(math.radians(a)), -0.02) for a in range(0, 360, 60)],
                  [(px + 0.42 * math.cos(math.radians(a)), py + 0.42 * math.sin(math.radians(a)), 0.3) for a in range(0, 360, 60)], m_gold, bevel=0.02)
            limb("pyl_col%d%d" % (sx, sy), (px, py, 0.3), (px, py, 1.35), 0.26, 0.2, m_steel, seg=6)
            for k, z in enumerate((0.55, 0.95)):
                ring("pyl_ring%d%d%d" % (sx, sy, k), (px, py, z), 0.25, 0.035, m_gold, seg=24, tseg=6)
            for fx, fy in ((1, 0), (-1, 0), (0, 1), (0, -1)):                   # fins
                box("pyl_fin%d%d%d%d" % (sx, sy, fx, fy), (px + fx * 0.24, py + fy * 0.24, 1.0), (0.05 if fx else 0.22, 0.22 if fx else 0.05, 0.9), m_navy, taper=0.4, bevel=0.01, subsurf=0)
            crystal = g_mag if sy > 0 else g_cyan
            sphere("pyl_cry%d%d" % (sx, sy), (px, py, 1.75), (0.2, 0.2, 0.42), crystal, seg=6, rings=4)
            ring("pyl_halo%d%d" % (sx, sy), (px, py, 1.75), 0.34, 0.02, g_gold, rot=(90, 0, 45 * sx * sy), seg=32, tseg=6)
    finalize("ArenaBase_Space", "SM_ArenaBase_Space.glb", keep_z=True, views=[
        ("top", (0, -11.5, 9.0), (0, 0.5, 0), (1280, 800), 30),
        ("low", (7, -9, 2.5), (0, 0, -0.5), (1280, 800), 30)])


# ------------------------------------------------------------------------------------------------------------------------------ board hexes and bench pads
def hex_tile(rim):
    bl_kit.TEX = 512
    R = 1.0 / math.sqrt(3)
    m_plate = material("plate", (0.07, 0.1, 0.19), 0.7, 0.3, kind="metal", wear=0.0)
    m_inset = material("inset", (0.045, 0.065, 0.13), 0.6, 0.4)
    g_rim = glow("rim", rim)
    prism("plate", hex_pts(R * 0.95, 0.0), hex_pts(R * 0.92, 0.02), m_plate)
    prism("inset", hex_pts(R * 0.78, 0.0), hex_pts(R * 0.78, 0.024), m_inset)
    hex_ring("rim", R * 0.935, R * 0.905, 0.019, 0.026, g_rim)
    hex_ring("inner", R * 0.8, R * 0.78, 0.0235, 0.027, material("steelline", (0.16, 0.2, 0.32), 0.8, 0.3))
    name = "HexTile_Home_Space" if rim == CYAN_RIM else "HexTile_Away_Space"
    finalize(name, "SM_%s.glb" % name, views=[("top", (0, -1.2, 1.2), (0, 0, 0), (512, 512), 40)])


CYAN_RIM = (0.12, 0.75, 0.95)
RED_RIM = (1.0, 0.3, 0.35)


def bench_slot():
    bl_kit.TEX = 512
    m_pad = material("pad", (0.05, 0.07, 0.14), 0.7, 0.35, kind="metal", wear=0.0)
    m_gold = material("gold", GOLD, 0.95, 0.25, kind="metal", wear=0.0)
    g = glow("benchglow", (0.35, 0.3, 0.95))
    box("pad", (0, 0, 0.03), (0.74, 0.74, 0.06), m_pad, bevel=0.02, subsurf=0)
    for r in (-1, 1):
        box("fx%d" % r, (r * 0.36, 0, 0.045), (0.05, 0.78, 0.07), m_gold, bevel=0.012, subsurf=0)
        box("fy%d" % r, (0, r * 0.36, 0.045), (0.78, 0.05, 0.07), m_gold, bevel=0.012, subsurf=0)
    ring("dial", (0, 0, 0.062), 0.2, 0.008, g, seg=32, tseg=4)
    finalize("BenchSlot_Space", "SM_BenchSlot_Space.glb", views=[("top", (0, -1.2, 1.2), (0, 0, 0), (512, 512), 40)])


# ------------------------------------------------------------------------------------------------------------------------------ the sky dome
def sky():
    """A 90 m sphere seen from inside, its emission an equirectangular nebula painted with numpy (no bake), albedo black."""
    import numpy as np
    Wt, Ht = 4096, 2048
    rng = np.random.default_rng(11)

    def value_noise(cells_x, cells_y):
        g = rng.random((cells_y + 1, cells_x + 1)).astype(np.float32); g[:, -1] = g[:, 0]      # wraps around horizontally
        xs = np.linspace(0, cells_x, Wt, endpoint=False); ys = np.linspace(0, cells_y, Ht)
        x0 = np.floor(xs).astype(int); y0 = np.minimum(np.floor(ys).astype(int), cells_y - 1)
        fx = xs - x0; fy = ys - y0
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy)
        a = g[y0][:, x0]; b = g[y0][:, x0 + 1]; c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
        top = a + (b - a) * fx[None, :]; bot = c + (d - c) * fx[None, :]
        return top + (bot - top) * fy[:, None]

    def fbm(base_cells, octaves):
        out = np.zeros((Ht, Wt), np.float32); amp, total = 1.0, 0.0
        for o in range(octaves):
            out += amp * value_noise(base_cells * 2 ** o, max(2, base_cells * 2 ** o // 2)); total += amp; amp *= 0.5
        return out / total

    lat = np.linspace(1, -1, Ht)[:, None]                                              # +1 top, -1 bottom
    clouds = np.clip((fbm(4, 7) - 0.42) * 2.4, 0, 1)
    wisps = np.clip((fbm(7, 6) - 0.5) * 3.0, 0, 1)
    hue = fbm(3, 3)
    violet = np.array([0.34, 0.12, 0.62], np.float32); blue = np.array([0.08, 0.2, 0.62], np.float32); pink = np.array([0.72, 0.16, 0.5], np.float32)
    colr = violet[None, None, :] * (1 - hue[..., None]) + blue[None, None, :] * hue[..., None]
    colr = colr * (1 - wisps[..., None] * 0.6) + pink[None, None, :] * wisps[..., None] * 0.6
    band = np.exp(-((lat - 0.08) / 0.35) ** 2)                                          # the nebula hugs the horizon, the zenith and nadir stay dark
    img = colr * (clouds * (0.25 + 0.75 * band))[..., None] * 0.9
    img += np.array([0.012, 0.014, 0.04], np.float32)[None, None, :]                    # the deep-space floor
    stars = rng.random((Ht, Wt)) > 0.9985
    bright = rng.random((Ht, Wt)).astype(np.float32)
    img[stars] += (0.5 + 0.9 * bright[stars])[:, None] * np.array([0.9, 0.95, 1.0], np.float32)
    big = rng.random((Ht, Wt)) > 0.99994                                                # a few big stars with a small cross
    for yy, xx in zip(*np.nonzero(big)):
        for d in range(-4, 5):
            f = 1.0 - abs(d) / 5.0
            img[yy, (xx + d) % Wt] += f * np.array([0.8, 0.85, 1.0]); img[min(Ht - 1, max(0, yy + d)), xx] += f * np.array([0.8, 0.85, 1.0])
    img = np.clip(img, 0, 1)

    emit = bpy.data.images.new("backdrop_emit", Wt, Ht, alpha=False)
    rgba = np.concatenate([img[::-1], np.ones((Ht, Wt, 1), np.float32)], axis=2)       # Blender images start at the bottom row
    emit.pixels[:] = rgba.reshape(-1).tolist()
    albedo = bpy.data.images.new("backdrop_albedo", 16, 16, alpha=False); albedo.generated_color = (0, 0, 0, 1)
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True)
        emit.filepath_raw = os.path.join(PREVIEW, "backdrop_emit.png"); emit.file_format = "PNG"; emit.save()
    emit.pack(); albedo.pack()

    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=90.0)
    obj = bpy.context.active_object; obj.name = "Backdrop"
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT"); bpy.ops.mesh.flip_normals(); bpy.ops.object.mode_set(mode="OBJECT")
    m = bpy.data.materials.new("M_Backdrop"); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit
    nt.links.new(t1.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], b.inputs["Emission Color"]); b.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    obj.data.materials.append(m)
    bpy.ops.object.select_all(action="DESELECT"); obj.select_set(True)
    OUT = os.path.join(MODELS, "SM_Backdrop_Space.glb")
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_materials="EXPORT", export_image_format="AUTO", export_cameras=False, export_lights=False)
    print("exported", OUT, os.path.getsize(OUT) // 1024, "KB")


if PIECE == "base": base()
elif PIECE == "home": hex_tile(CYAN_RIM)
elif PIECE == "away": hex_tile(RED_RIM)
elif PIECE == "bench": bench_slot()
elif PIECE == "sky": sky()
