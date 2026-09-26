"""The default W2F stage, in the style of TFT's classic arena: a sunny hilltop plaza with a hand-painted grass-and-dirt board, flagstone walkways for the two benches,
stepped stone terraces on the sides, brazier pillars on the corners, crystal lanterns, a rocky plateau edge and a ring of stylised trees and a river below.

One piece per run (the scene is not reset between pieces):
    ~/w2f_bpy/venv/bin/python tools/blender/arena_classic.py --piece floor|base|scenery|trees|home|away|bench|sky [--preview DIR] [--no-bake]

Pieces -> docs/models/: floor = SM_ArenaFloor (a flat plane, texture painted directly with numpy, no bake), base = SM_ArenaBase (the stone structure), scenery = SM_ArenaScenery
(the valley ground and river, painted from the height map), trees = SM_ArenaTrees (trees and boulders, vertex colours), home/away = SM_HexTile_Home/_Away (faint hex outlines), bench = SM_BenchSlot, sky = SM_Backdrop. The space stage (arena_space.py) is the
alternative theme: the same pieces with the suffix _Space.

Orientation (same as arena_space.py): the game spawns the pieces with yaw 90 at the middle of board + bench; Blender +Y is the home -> away axis (rows), Blender X runs along a row.
Board: 7.06 m (Y) x 7.5 m (X) around the origin; the benches sit at Y = -4.48 / +4.48 and span X = -3.7 .. 3.7. The floor's top is z = 0.
"""
import math, os, random
import numpy as np
import bl_kit
from bl_kit import *
import paint as P

PIECE = ARGS[ARGS.index("--piece") + 1] if "--piece" in ARGS else "floor"
random.seed(11)

FLOOR_W, FLOOR_L = 12.0, 12.8          # the painted floor plane (X, Y), metres
# The item slots (FEEDBACK V1): a flat stone platform on the front-left corner (where a brazier pillar stood), ten slots in two columns of five,
# nearest the bench first. AW2FArena::ItemSlotWorld mirrors these numbers.
ITEM_COLS = (-4.5, -5.2)                  # x of the two columns
ITEM_Y0, ITEM_STEP, ITEM_Z = -5.45, 0.55, 0.14   # first row's y (the front), row spacing towards the board, the slots' top
PLATFORM = (-5.75, -4.05, -5.95, -3.0)    # x0, x1, y0, y1 of the item platform
BOARD_X, BOARD_Y = 3.95, 3.7           # half extents of the grass board
WALK_X, WALK_Y = 4.45, 5.3             # half extents of the flagstone walkways (they run from BOARD_Y to WALK_Y)

GRASS = [(0.0, (0.29, 0.36, 0.17)), (0.35, (0.4, 0.47, 0.22)), (0.65, (0.52, 0.57, 0.28)), (1.0, (0.66, 0.67, 0.38))]
DIRT = [(0.0, (0.55, 0.46, 0.32)), (0.45, (0.68, 0.59, 0.43)), (1.0, (0.8, 0.72, 0.55))]
STONE = [(0.0, (0.47, 0.44, 0.39)), (0.5, (0.62, 0.59, 0.52)), (1.0, (0.75, 0.72, 0.63))]


# ------------------------------------------------------------------------------------------------------------------------------ the painted floor
def paint_floor(size=4096):
    W = H = size
    r = P.rng(5)
    xs = -FLOOR_W / 2 + (np.arange(W, dtype=np.float32) + 0.5) / W * FLOOR_W
    ys = FLOOR_L / 2 - (np.arange(H, dtype=np.float32) + 0.5) / H * FLOOR_L          # row 0 = the far (away) edge
    X = np.broadcast_to(xs[None, :], (H, W)); Y = np.broadcast_to(ys[:, None], (H, W))
    AX, AY = np.abs(X), np.abs(Y)
    px_m = W / FLOOR_W                                                                   # pixels per metre

    # --- grass: big soft colour masses, lighter sunny patches, fine painted strokes and specks
    g_big = P.fbm(W, H, 3, 6, r)
    g_sun = P.fbm(W, H, 2, 3, r)
    dabs = P.fbm(W, H, 28, 3, r)                                                        # painterly brush dabs
    strokes = P.fbm(W, H, 110, 2, r)
    grass = P.palette(np.clip(g_big * 1.25 - 0.12 + (g_sun - 0.5) * 0.35, 0, 1), GRASS)
    grass = P.blend(grass, P.col((0.3, 0.4, 0.3), H, W), P.smoothstep(0.55, 0.8, P.fbm(W, H, 2, 3, r)) * 0.35)   # cool blue-green masses
    grass *= (0.92 + 0.12 * dabs + 0.06 * strokes)[..., None]
    specks = P.fbm(W, H, 400, 1, r)
    grass = P.blend(grass, grass * 0.72, P.smoothstep(0.78, 0.86, specks) * 0.8)
    flowers = P.fbm(W, H, 520, 1, r)
    grass = P.blend(grass, P.col((0.93, 0.88, 0.62), H, W), P.smoothstep(0.9, 0.93, flowers) * (g_sun > 0.55))

    # --- dirt: a big worn patch in the middle of the board with a painted rim
    radial = np.sqrt((X / 3.6) ** 2 + (Y / 3.3) ** 2)
    d_noise = P.fbm(W, H, 2.5, 5, r)
    dirt_field = d_noise + (1.0 - radial) * 0.45 - 0.08 * (Y > 1.5)
    dirt_m = P.smoothstep(0.62, 0.626, dirt_field) * ((AX < BOARD_X) & (AY < BOARD_Y))
    rim = P.smoothstep(0.62, 0.626, dirt_field) - P.smoothstep(0.63, 0.66, dirt_field)                  # a darker band just inside the dirt's edge
    tuft = P.smoothstep(0.585, 0.618, dirt_field) - P.smoothstep(0.618, 0.621, dirt_field)             # sunlit grass tips just outside it
    dirt = P.palette(np.clip(P.fbm(W, H, 5, 5, r) * 1.3 - 0.15, 0, 1), DIRT)
    dirt *= (0.93 + 0.12 * P.fbm(W, H, 60, 2, r))[..., None]
    f1, f2, cid = P.cells(W, H, 260, 260, r, jitter=0.9)                              # pebbles
    peb = (f1 < 0.18) & (P.rng(9).random(260 * 260)[cid] > 0.82)
    dirt = P.blend(dirt, dirt * 1.18, peb.astype(np.float32))
    ground = P.blend(grass, dirt, dirt_m)
    ground = P.blend(ground, ground * 0.72, np.clip(rim, 0, 1) * 0.6 * ((AX < BOARD_X) & (AY < BOARD_Y)))
    ground = P.blend(ground, ground * 1.22, np.clip(tuft, 0, 1) * 0.7 * ((AX < BOARD_X) & (AY < BOARD_Y)))

    # --- old flagstones peeking through the board here and there
    f1, f2, cid = P.cells(W, H, 30, 32, P.rng(21), jitter=0.55, square=0.65)
    seam = f2 - f1
    stone_h = P.smoothstep(0.0, 0.14, seam)
    tone = P.rng(22).random(30 * 32).astype(np.float32)[cid]
    stones = P.palette(np.clip(tone * 0.8 + P.fbm(W, H, 8, 4, r) * 0.4 - 0.1, 0, 1), STONE)
    stones = stones * (1.0 + 0.28 * P.bevel_light(stone_h * 6.0, 2.2))[..., None]
    stones = P.blend(stones, stones * 0.45, 1.0 - P.smoothstep(0.02, 0.05, seam))
    field = P.fbm(W, H, 3.2, 4, P.rng(23)) + 0.12 * (1 - radial)
    per_cell = np.bincount(cid.ravel(), weights=field.ravel(), minlength=30 * 32) / np.maximum(1, np.bincount(cid.ravel(), minlength=30 * 32))
    patch = ((per_cell > 0.64) & (P.rng(24).random(30 * 32) > 0.25))[cid].astype(np.float32)          # whole stones in or out
    patch *= P.smoothstep(0.01, 0.03, seam)                                                               # grass shows in the seams
    board = (AX < BOARD_X) & (AY < BOARD_Y)
    ground = P.blend(ground, ground * 0.6, P.smoothstep(0.0, 0.05, seam) * (1 - P.smoothstep(0.05, 0.09, seam)) * ((per_cell > 0.64)[cid]) * board * 0.5)
    ground = P.blend(ground, stones, patch * board)

    # --- the walkways: big flagstones, mossy seams, painted bevels
    f1, f2, cid = P.cells(W, H, 24, 26, P.rng(31), jitter=0.5, square=0.8)
    seam = f2 - f1
    h = P.smoothstep(0.0, 0.1, seam)
    tone = P.rng(32).random(24 * 26).astype(np.float32)[cid]
    walk = P.palette(np.clip(tone * 0.7 + P.fbm(W, H, 10, 5, r) * 0.5 - 0.1, 0, 1), STONE)
    walk *= (0.95 + 0.1 * P.fbm(W, H, 120, 2, r))[..., None]
    walk = walk * (1.0 + 0.32 * P.bevel_light(h * 7.0, 2.4))[..., None]
    cracks = P.smoothstep(0.985, 1.0, 1.0 - np.abs(P.fbm(W, H, 14, 3, r) - 0.5) * 2) * (P.rng(33).random(24 * 26)[cid] > 0.75)
    walk = P.blend(walk, walk * 0.55, cracks * 0.8)
    mortar = 1.0 - P.smoothstep(0.015, 0.045, seam)
    moss = P.smoothstep(0.5, 0.65, P.fbm(W, H, 6, 4, r))
    walk = P.blend(walk, P.blend(P.col((0.2, 0.19, 0.16), H, W), P.col((0.3, 0.4, 0.15), H, W), moss), mortar)
    walk_m = ((AY > BOARD_Y) & (AY < WALK_Y) & (AX < WALK_X)).astype(np.float32)
    ground = P.blend(ground, walk, walk_m)

    # --- painted contact shadows: a soft dark band along the board's curb and the walkways' outer wall, and under the side terraces
    def band(dist, width):
        return np.clip(1.0 - dist / width, 0, 1) ** 1.6
    shade = np.zeros((H, W), np.float32)
    in_board = board.astype(np.float32)
    shade += band(np.minimum(BOARD_X - AX, BOARD_Y - AY).clip(0), 0.35) * in_board * 0.45
    shade += band((AY - BOARD_Y).clip(0), 0.25) * walk_m * 0.35 + band((WALK_Y - AY).clip(0), 0.3) * walk_m * 0.5
    ground *= (1.0 - np.clip(shade, 0, 0.6))[..., None]
    # a gentle warm sun glow in the middle, cooler edges
    glow = np.clip(1.0 - np.sqrt((X / 7.0) ** 2 + (Y / 7.0) ** 2), 0, 1)
    ground *= (0.9 + 0.16 * glow)[..., None]
    ground[..., 0] *= 1.0 + 0.03 * glow; ground[..., 2] *= 1.0 - 0.04 * glow
    return np.clip(ground, 0, 1)


def floor():
    img = paint_floor(2048 if "--draft" in ARGS else 4096)
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True)
        P.save_png(os.path.join(PREVIEW, "floor_albedo.png"), img)
    if "--paint-only" in ARGS: return
    albedo = P.to_image("arenafloor_albedo", img)
    emit = bpy.data.images.new("arenafloor_emit", 16, 16, alpha=False); emit.generated_color = (0, 0, 0, 1); emit.pack()
    bpy.ops.mesh.primitive_plane_add(size=1.0)
    obj = bpy.context.active_object; obj.name = "ArenaFloor"
    obj.scale = (FLOOR_W, FLOOR_L, 1.0); bpy.ops.object.transform_apply(scale=True)
    m = bpy.data.materials.new("M_ArenaFloor"); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit
    nt.links.new(t1.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], b.inputs["Emission Color"]); b.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    obj.data.materials.append(m)
    export(obj, "SM_ArenaFloor.glb")


# ------------------------------------------------------------------------------------------------------------------------------ painted tileable textures + materials
def tile_stone(size=1024, seed=41):
    """Tileable cut-stone blocks: warm grey, painted bevels, dark mortar."""
    r = P.rng(seed)
    f1, f2, cid = P.cells(size, size, 4, 5, r, jitter=0.45, square=0.85)
    seam = f2 - f1
    tone = P.rng(seed + 1).random(20).astype(np.float32)[cid]
    img = P.palette(np.clip(tone * 0.6 + P.fbm(size, size, 6, 5, r) * 0.55 - 0.05, 0, 1), STONE)
    img = img * (1.0 + 0.35 * P.bevel_light(P.smoothstep(0.0, 0.12, seam) * 5.0, 2.0))[..., None]
    img *= (0.94 + 0.1 * P.fbm(size, size, 40, 2, r))[..., None]
    img = P.blend(img, P.col((0.22, 0.2, 0.17), size, size), 1.0 - P.smoothstep(0.012, 0.035, seam))
    return np.clip(img, 0, 1)


def tile_rock(size=1024, seed=51):
    """Tileable cliff rock: layered warm grey-brown masses with painted cracks."""
    r = P.rng(seed)
    f1, f2, cid = P.cells(size, size, 3, 6, r, jitter=0.9, square=0.3)
    seam = f2 - f1
    tone = P.rng(seed + 1).random(18).astype(np.float32)[cid]
    rock = [(0.0, (0.34, 0.31, 0.28)), (0.5, (0.5, 0.46, 0.4)), (1.0, (0.66, 0.61, 0.52))]
    img = P.palette(np.clip(tone * 0.5 + P.fbm(size, size, 4, 6, r) * 0.7 - 0.1, 0, 1), rock)
    img = img * (1.0 + 0.4 * P.bevel_light(P.smoothstep(0.0, 0.25, seam) * 4.0 + P.fbm(size, size, 12, 3, r), 1.6))[..., None]
    img = P.blend(img, P.col((0.18, 0.16, 0.14), size, size), 1.0 - P.smoothstep(0.01, 0.04, seam))
    return np.clip(img, 0, 1)


def tile_grass(size=1024, seed=61):
    r = P.rng(seed)
    img = P.palette(np.clip(P.fbm(size, size, 3, 6, r) * 1.3 - 0.15, 0, 1), GRASS)
    img *= (0.9 + 0.14 * P.fbm(size, size, 30, 3, r) + 0.06 * P.fbm(size, size, 120, 2, r))[..., None]
    return np.clip(img, 0, 1)


def tile_water(size=1024, seed=71):
    """Tileable river water: teal with lighter, stretched ripple strokes and a few white glints."""
    r = P.rng(seed)
    img = P.palette(P.fbm(size, size, 3, 4, r), [(0.0, (0.2, 0.46, 0.55)), (1.0, (0.36, 0.66, 0.72))])
    rip = P.fbm(size, size, 18, 3, r, aspect=0.25)
    img = P.blend(img, P.col((0.62, 0.86, 0.88), size, size), P.smoothstep(0.66, 0.72, rip) * 0.6)
    img = P.blend(img, P.col((0.95, 0.98, 0.98), size, size), P.smoothstep(0.8, 0.83, P.fbm(size, size, 40, 2, r, aspect=0.3)) * 0.7)
    return np.clip(img, 0, 1)


_TILES = {}
def tile(name):
    if name not in _TILES:
        _TILES[name] = P.to_image("tile_" + name, {"stone": tile_stone, "rock": tile_rock, "grass": tile_grass, "water": tile_water}[name]())
    return _TILES[name]


def _box_tex(nt, image, scale, coord):
    mapping = nt.nodes.new("ShaderNodeMapping"); mapping.inputs["Scale"].default_value = (scale, scale, scale)
    nt.links.new(coord, mapping.inputs["Vector"])
    t = nt.nodes.new("ShaderNodeTexImage"); t.image = image; t.projection = "BOX"; t.projection_blend = 0.3
    nt.links.new(mapping.outputs["Vector"], t.inputs["Vector"])
    return t.outputs["Color"]


def painted(name, side, tile_m, top=None, top_tile_m=2.0, top_from=0.55, tint=(1, 1, 1), emit=None):
    """A material from painted tiles, box-projected in object (= world, after the join) space: `side` everywhere, and `top` (e.g. grass, moss) on faces that look up."""
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    b.inputs["Roughness"].default_value = 0.9; b.inputs["Metallic"].default_value = 0.0
    coord = nt.nodes.new("ShaderNodeTexCoord").outputs["Object"]
    colour = _box_tex(nt, tile(side), 1.0 / tile_m, coord)
    tint_n = nt.nodes.new("ShaderNodeMix"); tint_n.data_type = "RGBA"; tint_n.blend_type = "MULTIPLY"; tint_n.inputs["Factor"].default_value = 1.0
    nt.links.new(colour, tint_n.inputs["A"]); tint_n.inputs["B"].default_value = (*tint, 1.0)
    colour = tint_n.outputs["Result"]
    if top:
        geo = nt.nodes.new("ShaderNodeNewGeometry")
        sep = nt.nodes.new("ShaderNodeSeparateXYZ"); nt.links.new(geo.outputs["Normal"], sep.inputs["Vector"])
        wob = nt.nodes.new("ShaderNodeTexNoise"); wob.inputs["Scale"].default_value = 3.0; nt.links.new(coord, wob.inputs["Vector"])
        add = nt.nodes.new("ShaderNodeMath"); add.operation = "ADD"
        nt.links.new(sep.outputs["Z"], add.inputs[0])
        wob_c = nt.nodes.new("ShaderNodeMath"); wob_c.operation = "MULTIPLY_ADD"; wob_c.inputs[1].default_value = 0.35; wob_c.inputs[2].default_value = -0.175
        nt.links.new(wob.outputs["Fac"], wob_c.inputs[0]); nt.links.new(wob_c.outputs[0], add.inputs[1])
        rng_ = nt.nodes.new("ShaderNodeMapRange"); rng_.inputs["From Min"].default_value = top_from; rng_.inputs["From Max"].default_value = top_from + 0.08
        nt.links.new(add.outputs[0], rng_.inputs["Value"])
        mix = nt.nodes.new("ShaderNodeMix"); mix.data_type = "RGBA"
        nt.links.new(rng_.outputs["Result"], mix.inputs["Factor"]); nt.links.new(colour, mix.inputs["A"])
        nt.links.new(_box_tex(nt, tile(top), 1.0 / top_tile_m, coord), mix.inputs["B"])
        colour = mix.outputs["Result"]
    nt.links.new(colour, b.inputs["Base Color"])
    if emit:
        b.inputs["Emission Color"].default_value = (*emit, 1.0); b.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    return m


def foliage(name, dark, light, dabs=6.0):
    """Stylised leaves: dark underneath, sunlit on top, broken up by painterly noise dabs."""
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    b.inputs["Roughness"].default_value = 0.85
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ"); nt.links.new(geo.outputs["Normal"], sep.inputs["Vector"])
    noise = nt.nodes.new("ShaderNodeTexNoise"); noise.inputs["Scale"].default_value = dabs; noise.inputs["Detail"].default_value = 3.0
    nt.links.new(nt.nodes.new("ShaderNodeTexCoord").outputs["Object"], noise.inputs["Vector"])
    f = nt.nodes.new("ShaderNodeMath"); f.operation = "MULTIPLY_ADD"; f.inputs[1].default_value = 0.45; f.inputs[2].default_value = 0.5
    nt.links.new(sep.outputs["Z"], f.inputs[0])
    g = nt.nodes.new("ShaderNodeMath"); g.operation = "MULTIPLY_ADD"; g.inputs[1].default_value = 0.6; g.inputs[2].default_value = -0.3
    nt.links.new(noise.outputs["Fac"], g.inputs[0])
    h = nt.nodes.new("ShaderNodeMath"); h.operation = "ADD"; h.use_clamp = True
    nt.links.new(f.outputs[0], h.inputs[0]); nt.links.new(g.outputs[0], h.inputs[1])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].position = 0.25; ramp.color_ramp.elements[0].color = (*dark, 1)
    ramp.color_ramp.elements[1].position = 0.85; ramp.color_ramp.elements[1].color = (*light, 1)
    mid = ramp.color_ramp.elements.new(0.55); mid.color = tuple(0.5 * (a + c) for a, c in zip(dark, light)) + (1,)
    ramp.color_ramp.interpolation = "CONSTANT"                                   # posterised bands = painted look
    nt.links.new(h.outputs[0], ramp.inputs["Fac"]); nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    return m


def lin(c):
    """Display (sRGB) colour -> linear, for Blender material colour sockets."""
    return tuple((v / 12.92) if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in c)


# ------------------------------------------------------------------------------------------------------------------------------ shapes
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
    return finish(make(name, bm), mat, bevel=bevel, smooth=True)


def stone_block(name, loc, size, mat, rot=(0, 0, 0), jag=0.03):
    """A chunky hand-cut block: bevelled, slightly irregular."""
    o = box(name, loc, size, mat, rot=rot, bevel=min(0.05, min(size) * 0.2), subsurf=0)
    for v in o.data.vertices:
        v.co.x += random.uniform(-jag, jag); v.co.y += random.uniform(-jag, jag); v.co.z += random.uniform(-jag, jag) * 0.5
    return o


def rock(name, loc, size, mat, seed=0, flat=0.0):
    """A lumpy boulder: a subdivided icosphere pushed around by noise; `flat` squashes its top."""
    bm = bmesh.new(); bmesh.ops.create_icosphere(bm, subdivisions=3, radius=1.0)
    rr = random.Random(seed)
    ph = [rr.uniform(0, 6.28) for _ in range(6)]
    for v in bm.verts:
        x, y, z = v.co
        k = 1.0 + 0.16 * math.sin(3.1 * x + ph[0]) * math.cos(2.7 * y + ph[1]) + 0.1 * math.sin(5.3 * z + ph[2]) + 0.05 * math.sin(9.0 * (x + y) + ph[3])
        v.co = Vector((x * size[0] * k, y * size[1] * k, z * size[2] * k))
        if flat and v.co.z > size[2] * (1 - flat): v.co.z = size[2] * (1 - flat) + (v.co.z - size[2] * (1 - flat)) * 0.15
    o = make(name, bm); o.location = loc; o.rotation_euler = (0, 0, rr.uniform(0, 6.28))
    return finish(o, mat, smooth=True)


def flame(name, loc, scale, mat_outer, mat_inner):
    """A stylised fire: a few twisted teardrop tongues around a bright core."""
    for k in range(5):
        a = k * 72 + random.uniform(-15, 15)
        dx, dy = math.cos(math.radians(a)) * 0.08 * scale, math.sin(math.radians(a)) * 0.08 * scale
        hgt = scale * random.uniform(0.35, 0.55)
        o = limb("%s_t%d" % (name, k), (loc[0] + dx, loc[1] + dy, loc[2]), (loc[0] + dx * 1.6, loc[1] + dy * 1.6, loc[2] + hgt), 0.09 * scale, 0.005, mat_outer, seg=8)
    limb(name + "_core", loc, (loc[0], loc[1], loc[2] + scale * 0.7), 0.13 * scale, 0.01, mat_inner, seg=10)


# ------------------------------------------------------------------------------------------------------------------------------ the stone structure
def base():
    bl_kit.TEX = 4096
    m_block = painted("block", "stone", 1.2, top="grass", top_from=0.93, tint=(1.02, 1.0, 0.96))
    m_curb = painted("curb", "stone", 0.9, tint=(1.05, 1.03, 0.98))
    m_cliff = painted("cliff", "rock", 2.5, top="grass", top_from=0.7)
    m_rim = painted("rim", "rock", 1.5, top="grass", top_from=0.86)
    m_pillar = painted("pillar", "stone", 1.0, tint=(1.08, 1.05, 1.0))
    m_bronze = material("bronze", lin((0.62, 0.45, 0.22)), 0.8, 0.35, kind="metal", wear=0.2, color2=lin((0.85, 0.66, 0.34)))
    m_gold = material("gold", lin((0.86, 0.66, 0.3)), 0.9, 0.3, kind="metal", wear=0.0, color2=lin((1.0, 0.85, 0.5)))
    m_wood = material("wood", lin((0.4, 0.27, 0.16)), 0.0, 0.8, kind="bark", color2=lin((0.55, 0.4, 0.25)))
    g_fire = glow("fire", lin((1.0, 0.55, 0.12))); g_core = glow("firecore", lin((1.0, 0.9, 0.45)))
    g_cry = glow("crystal", lin((0.45, 0.85, 1.0))); g_lamp = glow("lamp", lin((1.0, 0.82, 0.45)))

    # the board's curb: low worn stones along all four edges
    x = -BOARD_X
    while x < BOARD_X - 0.05:
        ln = random.uniform(0.42, 0.7); ln = min(ln, BOARD_X - x)
        for sy in (-1, 1):
            stone_block("curb_y%d_%.2f" % (sy, x), (x + ln / 2, sy * (BOARD_Y + 0.02), 0.015), (ln - 0.03, 0.16, 0.07 + random.uniform(0, 0.03)), m_curb, jag=0.012)
        x += ln
    # the walkways' outer walls (knee-high, grass on top) and a short step at both ends
    for sy in (-1, 1):
        x = -WALK_X - 0.1
        while x < WALK_X + 0.1:
            ln = random.uniform(0.55, 0.95); ln = min(ln, WALK_X + 0.1 - x)
            stone_block("wall%d_%.2f" % (sy, x), (x + ln / 2, sy * (WALK_Y + 0.2), 0.08), (ln - 0.03, 0.42, 0.3 + random.uniform(-0.03, 0.05)), m_block, jag=0.025)
            x += ln
    # the side terraces: three tiers of big blocks stepping up away from the board, with gaps and a few fallen stones
    for sx in (-1, 1):
        for tier, (x0, x1, h) in enumerate(((4.02, 4.55, 0.16), (4.55, 5.15, 0.36), (5.15, 5.95, 0.58))):
            y = -4.75
            while y < 4.75:
                ln = random.uniform(0.6, 1.3); ln = min(ln, 4.75 - y)
                if sx < 0 and y + ln / 2 < PLATFORM[3] + 0.1:   # the item platform takes the front-left corner
                    y += ln; continue
                if random.random() > 0.12 or tier == 2:
                    hh = h + random.uniform(-0.04, 0.06)
                    stone_block("ter%d_%d_%.2f" % (sx, tier, y), (sx * (x0 + x1) / 2, y + ln / 2, hh / 2 - 0.04), (x1 - x0 - 0.02, ln - 0.04, hh + 0.08), m_block,
                                rot=(0, 0, random.uniform(-2.5, 2.5)), jag=0.035)
                y += ln
        for k in range(5):
            rock("rubble%d_%d" % (sx, k), (sx * random.uniform(4.1, 4.5), random.uniform(-3.5, 3.5), 0.03), (0.12, 0.09, 0.07), m_cliff, seed=k * 7 + sx)
    # corner pillars with bronze braziers (the game puts its flickering fire lights at x = +-4.75, y = +-5.25, z = 1.75)
    for sx in (-1, 1):
        for sy in (-1, 1):
            if sx < 0 and sy < 0: continue   # the front-left corner holds the item platform instead
            px, py = sx * 4.75, sy * 5.25
            stone_block("plinth%d%d" % (sx, sy), (px, py, 0.12), (1.15, 1.15, 0.34), m_pillar, jag=0.02)
            stone_block("shaft%d%d" % (sx, sy), (px, py, 0.62), (0.78, 0.78, 0.7), m_pillar, jag=0.015)
            stone_block("cap%d%d" % (sx, sy), (px, py, 1.04), (0.98, 0.98, 0.16), m_pillar, jag=0.015)
            limb("bowl%d%d" % (sx, sy), (px, py, 1.1), (px, py, 1.36), 0.2, 0.4, m_bronze, seg=16)
            ring("bowl_rim%d%d" % (sx, sy), (px, py, 1.36), 0.4, 0.035, m_bronze, seg=24, tseg=6)
            limb("coals%d%d" % (sx, sy), (px, py, 1.3), (px, py, 1.36), 0.36, 0.36, material("coal%d%d" % (sx, sy), lin((0.18, 0.08, 0.04)), 0.0, 0.9, emit=lin((0.9, 0.25, 0.05)), strength=1.0), seg=16)
            flame("fire%d%d" % (sx, sy), (px, py, 1.34), 1.1, g_fire, g_core)
    # lanterns on the upper terraces: crystal lamps on the left (-X), golden lamps on the right (+X)
    for sx, ys, lamp in ((-1, (1.6, -1.2), g_cry), (1, (1.1, -2.1), g_lamp)):
        for k, ly in enumerate(ys):
            lx = sx * 5.55
            stone_block("lpost%d%d" % (sx, k), (lx, ly, 0.9), (0.2, 0.2, 0.9), m_pillar, jag=0.01)
            box("lcage%d%d" % (sx, k), (lx, ly, 1.5), (0.3, 0.3, 0.05), m_gold, bevel=0.01, subsurf=0)
            for cx, cy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
                limb("lbar%d%d%d%d" % (sx, k, cx, cy), (lx + cx * 0.12, ly + cy * 0.12, 1.52), (lx + cx * 0.09, ly + cy * 0.09, 1.9), 0.018, 0.015, m_gold, seg=6)
            limb("lroof%d%d" % (sx, k), (lx, ly, 1.9), (lx, ly, 2.08), 0.22, 0.02, m_gold, seg=8)
            sphere("lcry%d%d" % (sx, k), (lx, ly, 1.7), (0.1, 0.1, 0.17), lamp, seg=6, rings=4)
    # the item platform on the front-left corner: one flat slab with a gold edge, ten inset slots with a warm glow
    px0, px1, py0, py1 = PLATFORM
    stone_block("itemplatform", ((px0 + px1) / 2, (py0 + py1) / 2, 0.02), (px1 - px0, py1 - py0, 0.2), m_pillar, jag=0.01)
    for k in range(10):
        cx, cy = ITEM_COLS[k % 2], ITEM_Y0 + (k // 2) * ITEM_STEP
        limb("slot%d" % k, (cx, cy, 0.115), (cx, cy, 0.13), 0.21, 0.21, m_gold, seg=6)
        limb("slotin%d" % k, (cx, cy, 0.125), (cx, cy, 0.14), 0.17, 0.17, material("slotglow%d" % k, lin((0.28, 0.24, 0.18)), 0.0, 0.6, emit=lin((0.5, 0.38, 0.16)), strength=1.0), seg=6)
    # the plateau's edge: a rocky cliff skirt under the floor's rim, going down to the valley, with boulders stacked along it
    def rounded(w, l, rad, z, seg=6):
        pts = []
        for cx, cy, a0 in ((w / 2 - rad, l / 2 - rad, 0), (-w / 2 + rad, l / 2 - rad, 90), (-w / 2 + rad, -l / 2 + rad, 180), (w / 2 - rad, -l / 2 + rad, 270)):
            for k in range(seg + 1):
                a = math.radians(a0 + 90.0 * k / seg); pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a), z))
        return pts
    skirt = prism("skirt", rounded(FLOOR_W + 1.4, FLOOR_L + 1.4, 1.6, -3.6), rounded(FLOOR_W + 0.1, FLOOR_L + 0.1, 0.9, -0.03), m_cliff)
    bpy.context.view_layer.objects.active = skirt
    mod = skirt.modifiers.new("Sub", "SUBSURF"); mod.levels = 3; mod.subdivision_type = "SIMPLE"; bpy.ops.object.modifier_apply(modifier=mod.name)
    tex = bpy.data.textures.new("skirt_d", "CLOUDS"); tex.noise_scale = 0.9
    mod = skirt.modifiers.new("Disp", "DISPLACE"); mod.texture = tex; mod.strength = 0.35; mod.mid_level = 0.5; bpy.ops.object.modifier_apply(modifier=mod.name)
    for v in skirt.data.vertices:                                                    # keep the top rim flush with the floor
        if v.co.z > -0.1: v.co.z = -0.03
    for k in range(70):
        t = k / 70.0 * 2 * math.pi
        ex, ey = math.cos(t), math.sin(t)
        sc = 1.0 / max(abs(ex) / (FLOOR_W / 2 + 0.45), abs(ey) / (FLOOR_L / 2 + 0.45))
        rock("edge%d" % k, (ex * sc, ey * sc, random.uniform(-0.55, -0.25)), (random.uniform(0.35, 0.6), random.uniform(0.35, 0.6), random.uniform(0.3, 0.45)), m_rim, seed=k, flat=0.3)
    finalize("ArenaBase", "SM_ArenaBase.glb", keep_z=True, views=[
        ("top", (0, -13.5, 11.0), (0, 0.3, 0), (1280, 800), 30),
        ("low", (8, -10, 3.0), (0, 0, -0.3), (1280, 800), 30)])


# ------------------------------------------------------------------------------------------------------------------------------ the valley around the plateau
WATER = -3.75                          # the river's surface (z)
SPAN = 70.0                            # the valley square (metres)


def valley_height(x, y):
    """Ground height of the valley (z, metres; floats or numpy arrays): about -3.4, a river winding across the front, hills rising at the back and the far sides."""
    h = -3.4 + 0.35 * np.sin(x * 0.45 + 1.3) * np.cos(y * 0.38) + 0.2 * np.sin(x * 1.1 + y * 0.7)
    river = -9.2 + 2.2 * np.sin(x / 4.5 + 0.8)
    h = h - 1.5 * np.maximum(0.0, 1.0 - np.abs(y - river) / 3.0) ** 1.2
    h = h + np.minimum(6.5, np.maximum(0.0, y - 8.0) ** 1.35 * 0.45)          # hills behind the enemy bench
    h = h + np.minimum(4.5, np.maximum(0.0, np.abs(x) - 10.5) ** 1.3 * 0.4)   # and on the far sides
    return np.maximum(h, WATER)                                                  # the river is flat water


def tree_layout():
    """Where the trees and boulders stand: [(kind, x, y, z, height)], [(x, y, z, size)]. Deterministic; the ground painter uses it for the tree shadows."""
    rr = random.Random(17)
    trees = []; tries = 0
    def on_plateau(x, y, margin):
        return abs(x) < FLOOR_W / 2 + margin and abs(y) < FLOOR_L / 2 + margin
    while len(trees) < 95 and tries < 6000:
        tries += 1
        x = rr.uniform(-26, 26); y = rr.uniform(-20, 26)
        if on_plateau(x, y, 1.6): continue
        h = float(valley_height(x, y))
        if h <= WATER + 0.1: continue                                      # not in the river
        d = max(abs(x) - FLOOR_W / 2, abs(y) - FLOOR_L / 2)                # denser near the plateau's sides and at the back, sparse in front (the shop covers it)
        if y < -8 and rr.random() < 0.6: continue
        if d > 12 and rr.random() < 0.5: continue
        k = rr.random()
        hgt = rr.uniform(3.2, 5.8) * (1.25 if d > 6 else 1.0)
        kind = "pine" if k < 0.42 else "oak" if k < 0.8 else "birch" if k < 0.94 else "blossom"
        trees.append((kind, x, y, h, hgt * (0.85 if kind == "birch" else 0.8 if kind == "blossom" else 1.0)))
    for sx in (-1, 1):                                                     # right behind the corner pillars, like the reference
        for k, (x, y, hgt) in enumerate(((6.6, 5.6, 4.2), (7.4, 3.2, 3.6), (6.9, 7.2, 4.8), (7.8, -1.0, 3.4))):
            trees.append(("pine" if (k + (sx > 0)) % 2 else "oak", sx * x, y, float(valley_height(sx * x, y)) + 1.2, hgt + 1.2))
    rocks = []
    for k in range(40):
        x = rr.uniform(-22, 22); y = rr.uniform(-16, 22)
        if on_plateau(x, y, 1.2): continue
        rocks.append((x, y, float(valley_height(x, y)) + 0.1, (rr.uniform(0.5, 1.4), rr.uniform(0.4, 0.9))))
    return trees, rocks


def pine(name, x, y, z, height, mat_leaf, mat_bark):
    limb(name + "_trunk", (x, y, z - 0.2), (x, y, z + height * 0.35), 0.1 * height / 4, 0.07 * height / 4, mat_bark, seg=6)
    tiers = 4
    for k in range(tiers):
        t = k / (tiers - 1)
        z0 = z + height * (0.22 + 0.19 * k); rad = height * (0.3 - 0.2 * t)
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=9, radius1=rad, radius2=0.0, depth=height * 0.36)
        for v in bm.verts:                                                  # a jagged, drooping rim
            if v.co.z < 0:
                v.co.x *= random.uniform(0.85, 1.15); v.co.y *= random.uniform(0.85, 1.15); v.co.z -= random.uniform(0.0, 0.12) * height / 4
        o = make("%s_c%d" % (name, k), bm); o.location = (x, y, z0 + height * 0.18); o.rotation_euler = (0, 0, random.uniform(0, 6.28))
        finish(o, mat_leaf, smooth=False)


def broadleaf(name, x, y, z, height, mat_leaf, mat_bark):
    limb(name + "_trunk", (x, y, z - 0.2), (x + random.uniform(-0.2, 0.2), y + random.uniform(-0.2, 0.2), z + height * 0.55), 0.14 * height / 4, 0.08 * height / 4, mat_bark, seg=7)
    cz = z + height * 0.68; rad = height * 0.26
    for k in range(random.randint(6, 9)):
        a = random.uniform(0, 6.28); d = random.uniform(0.2, 0.75) * rad
        s_ = rad * random.uniform(0.55, 0.8)
        bm = bmesh.new(); bmesh.ops.create_icosphere(bm, subdivisions=2, radius=1.0)
        for v in bm.verts:
            v.co = Vector((v.co.x * s_ * random.uniform(0.95, 1.05), v.co.y * s_ * random.uniform(0.95, 1.05), v.co.z * s_ * 0.8))
        o = make("%s_b%d" % (name, k), bm)
        o.location = (x + math.cos(a) * d, y + math.sin(a) * d, cz + random.uniform(-0.35, 0.45) * rad)
        finish(o, mat_leaf, smooth=True)


def paint_valley(size=4096):
    """The valley ground texture, planar over SPAN x SPAN metres: grass (lighter up the hills), rock on the steep slopes, sandy banks, the river with ripples
    and foam, and soft painted shadows under every tree."""
    W = H = size
    r = P.rng(81)
    xs = -SPAN / 2 + (np.arange(W, dtype=np.float32) + 0.5) / W * SPAN
    ys = SPAN / 2 - (np.arange(H, dtype=np.float32) + 0.5) / H * SPAN
    X = np.broadcast_to(xs[None, :], (H, W)); Y = np.broadcast_to(ys[:, None], (H, W))
    Hh = valley_height(X, Y).astype(np.float32)
    gy, gx = np.gradient(Hh, SPAN / H, SPAN / W)
    slope = np.hypot(gx, gy)
    img = P.palette(np.clip(P.fbm(W, H, 10, 6, r) * 1.2 - 0.1 + (Hh + 3.4) * 0.05, 0, 1), GRASS)
    img *= (0.92 + 0.12 * P.fbm(W, H, 90, 3, r))[..., None]
    img = P.blend(img, P.col((0.3, 0.4, 0.3), H, W), P.smoothstep(0.55, 0.8, P.fbm(W, H, 6, 3, r)) * 0.3)
    rock = P.palette(np.clip(P.fbm(W, H, 40, 5, r) * 1.2 - 0.1, 0, 1), [(0.0, (0.36, 0.33, 0.29)), (1.0, (0.62, 0.58, 0.5))])
    img = P.blend(img, rock, P.smoothstep(0.7, 1.0, slope + (P.fbm(W, H, 20, 3, r) - 0.5) * 0.4))
    bank = P.smoothstep(WATER + 0.45, WATER + 0.08, Hh)
    img = P.blend(img, P.palette(P.fbm(W, H, 30, 3, r), DIRT), bank)
    water = (Hh <= WATER + 0.005).astype(np.float32)
    rip = P.fbm(W, H, 60, 3, r, aspect=0.3)
    wcol = P.palette(P.fbm(W, H, 12, 4, r), [(0.0, (0.2, 0.47, 0.56)), (1.0, (0.35, 0.65, 0.72))])
    wcol = P.blend(wcol, P.col((0.62, 0.86, 0.88), H, W), P.smoothstep(0.66, 0.72, rip) * 0.55)
    img = P.blend(img, wcol, water)
    # foam where the water meets the bank
    near = P.smoothstep(WATER + 0.12, WATER + 0.01, Hh) * (1 - water)
    img = P.blend(img, P.col((0.9, 0.95, 0.93), H, W), near * 0.6)
    # tree shadows: soft dark blobs, pushed away from the sun (the sun comes from the front-left)
    trees, rocks = tree_layout()
    shade = np.zeros((H, W), np.float32)
    px = W / SPAN
    for kind, x, y, z, hgt in trees:
        cr = hgt * (0.3 if kind == "pine" else 0.3)
        cx, cy = x + hgt * 0.25, y + hgt * 0.3
        i0 = int((cx + SPAN / 2) * px); j0 = int((SPAN / 2 - cy) * px); rad = int(cr * px * 1.3) + 2
        i1, i2 = max(0, i0 - rad), min(W, i0 + rad); j1, j2 = max(0, j0 - rad), min(H, j0 + rad)
        if i1 >= i2 or j1 >= j2: continue
        d = np.hypot((X[j1:j2, i1:i2] - cx) / cr, (Y[j1:j2, i1:i2] - cy) / (cr * 0.85))
        shade[j1:j2, i1:i2] = np.maximum(shade[j1:j2, i1:i2], P.smoothstep(1.15, 0.6, d) * 0.5)
    img *= (1.0 - shade)[..., None]
    return np.clip(img, 0, 1)


def scenery():
    """SM_ArenaScenery: the valley ground (and the river) as one displaced grid with planar UVs and a texture painted straight from the height map. No bake."""
    img = paint_valley(2048 if "--draft" in ARGS else 4096)
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True); P.save_png(os.path.join(PREVIEW, "valley_albedo.png"), img)
    albedo = P.to_image("arenascenery_albedo", img)
    emit = bpy.data.images.new("arenascenery_emit", 16, 16, alpha=False); emit.generated_color = (0, 0, 0, 1); emit.pack()
    N = 160
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    verts = [[bm.verts.new((-SPAN / 2 + i * SPAN / N, -SPAN / 2 + j * SPAN / N, 0.0)) for i in range(N + 1)] for j in range(N + 1)]
    for j in range(N):
        for i in range(N):
            f = bm.faces.new((verts[j][i], verts[j][i + 1], verts[j + 1][i + 1], verts[j + 1][i]))
            for loop in f.loops:
                loop[uv].uv = ((loop.vert.co.x + SPAN / 2) / SPAN, (loop.vert.co.y + SPAN / 2) / SPAN)
    for v in bm.verts: v.co.z = float(valley_height(v.co.x, v.co.y))
    bm.normal_update()
    obj = make("ArenaScenery", bm); scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj; obj.select_set(True); bpy.ops.object.shade_smooth()
    m = bpy.data.materials.new("M_ArenaScenery"); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit
    nt.links.new(t1.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], b.inputs["Emission Color"]); b.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    obj.data.materials.append(m)
    export(obj, "SM_ArenaScenery.glb")


def trees():
    """SM_ArenaTrees: every tree and boulder, with colour and ambient occlusion baked into the VERTEX colours (the flat-shaded, painted look; no texture).
    The game gives it the vertex-colour material."""
    m_rock = painted("vrock", "rock", 2.0, top="grass", top_from=0.75)
    m_bark = material("bark", lin((0.36, 0.25, 0.16)), 0.0, 0.85, kind="bark", color2=lin((0.5, 0.36, 0.22)))
    leaves = {"pine": foliage("pine", lin((0.08, 0.2, 0.14)), lin((0.3, 0.46, 0.22))),
              "oak": foliage("oak", lin((0.16, 0.3, 0.1)), lin((0.55, 0.66, 0.24))),
              "birch": foliage("birch", lin((0.22, 0.34, 0.1)), lin((0.68, 0.72, 0.3))),
              "blossom": foliage("blossom", lin((0.5, 0.25, 0.35)), lin((0.95, 0.72, 0.78)))}
    layout, rocks = tree_layout()
    random.seed(23)
    for n, (kind, x, y, z, hgt) in enumerate(layout):
        (pine if kind == "pine" else broadleaf)("%s%d" % (kind, n), x, y, z, hgt, leaves[kind], m_bark)
    for k, (x, y, z, (w, h)) in enumerate(rocks):
        rock("boulder%d" % k, (x, y, z), (w, w, h), m_rock, seed=300 + k)
    obj = bl_kit._join("ArenaTrees")
    print("trees: %d faces" % len(obj.data.polygons))
    stage()
    if PREVIEW:
        render(os.path.join(PREVIEW, "trees_top.png"), (0, -16.5, 13.0), (0, 1.5, -2), size=(1280, 800), lens=26)
    if not BAKE: return
    me = obj.data
    scene.cycles.samples = 32
    scene.render.bake.target = "VERTEX_COLORS"

    def bake_attr(name, kind):
        attr = me.color_attributes.new(name, "FLOAT_COLOR", "CORNER")
        me.color_attributes.active_color = attr
        if kind == "DIFFUSE":
            scene.render.bake.use_pass_direct = False; scene.render.bake.use_pass_indirect = False; scene.render.bake.use_pass_color = True
        bpy.ops.object.bake(type=kind)
        vals = np.empty(len(me.loops) * 4, np.float32); attr.data.foreach_get("color", vals)
        return vals.reshape(-1, 4)

    bpy.ops.object.select_all(action="DESELECT"); obj.select_set(True); bpy.context.view_layer.objects.active = obj
    colour = bake_attr("BakeCol", "DIFFUSE")
    ao = bake_attr("BakeAO", "AO")
    colour[:, :3] *= (0.42 + 0.58 * ao[:, :1])
    final = me.color_attributes.new("Col", "BYTE_COLOR", "CORNER")
    final.data.foreach_set("color", colour.reshape(-1))
    for n in ("BakeCol", "BakeAO"):
        me.color_attributes.remove(me.color_attributes[n])
    me.color_attributes.active_color = me.color_attributes["Col"]
    me.color_attributes.render_color_index = me.color_attributes.active_color_index
    plain = bpy.data.materials.new("M_ArenaTrees"); plain.use_nodes = True
    nt = plain.node_tree; b = nt.nodes["Principled BSDF"]
    vc = nt.nodes.new("ShaderNodeVertexColor"); vc.layer_name = "Col"; nt.links.new(vc.outputs["Color"], b.inputs["Base Color"])
    me.materials.clear(); me.materials.append(plain)
    if PREVIEW:
        render(os.path.join(PREVIEW, "trees_baked.png"), (0, -16.5, 13.0), (0, 1.5, -2), size=(1280, 800), lens=26)
    OUT = os.path.join(MODELS, "SM_ArenaTrees.glb")
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_materials="EXPORT",
                              export_vertex_color="ACTIVE", export_cameras=False, export_lights=False)
    print("exported", OUT, os.path.getsize(OUT) // 1024, "KB")


# ------------------------------------------------------------------------------------------------------------------------------ hex outlines, bench marks, sky
def hex_pts(r, z):
    return [(r * math.sin(math.radians(60 * i)), r * math.cos(math.radians(60 * i)), z) for i in range(6)]


def outline(name, pts_out, pts_in, z0, z1, mat):
    """A flat frame between two matching rings of points."""
    bm = bmesh.new()
    n = len(pts_out)
    rings = [[bm.verts.new((p[0], p[1], z)) for p in pts] for pts, z in ((pts_out, z0), (pts_in, z0), (pts_out, z1), (pts_in, z1))]
    ob, ib, ot, it = rings
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new([ot[i], ot[j], it[j], it[i]]); bm.faces.new([ob[j], ob[i], ib[i], ib[j]])
        bm.faces.new([ob[i], ob[j], ot[j], ot[i]]); bm.faces.new([ib[j], ib[i], it[i], it[j]])
    bm.normal_update()
    return finish(make(name, bm), mat, smooth=False)


LINE = (0.68, 0.66, 0.54)            # the faint hex / bench lines: a pale, low-contrast chalk

def hex_tile(tag):
    bl_kit.TEX = 256
    R = 1.0 / math.sqrt(3)
    m = material("line", lin(LINE), 0.0, 0.8)
    outline("rim", hex_pts(R * 0.955, 0), hex_pts(R * 0.935, 0), 0.0, 0.004, m)
    finalize("HexTile_" + tag, "SM_HexTile_%s.glb" % tag, keep_z=True, views=[("top", (0, -1.2, 1.2), (0, 0, 0), (256, 256), 40)])


def bench_slot():
    bl_kit.TEX = 256
    m = material("line", lin(LINE), 0.0, 0.8)
    def sq(h, c):
        return [(-h + c, -h, 0), (h - c, -h, 0), (h, -h + c, 0), (h, h - c, 0), (h - c, h, 0), (-h + c, h, 0), (-h, h - c, 0), (-h, -h + c, 0)]
    outline("mark", sq(0.34, 0.08), sq(0.325, 0.075), 0.0, 0.004, m)
    finalize("BenchSlot", "SM_BenchSlot.glb", keep_z=True, views=[("top", (0, -1.2, 1.2), (0, 0, 0), (256, 256), 40)])


def sky():
    """A 90 m dome seen from inside: a pastel afternoon sky (blue zenith, warm pink horizon, soft clouds), painted with numpy, emission only."""
    Wt, Ht = 4096, 2048
    r = P.rng(13)
    lat = np.linspace(1, -1, Ht, dtype=np.float32)[:, None] * np.ones((1, Wt), np.float32)
    img = P.palette(np.clip((lat + 0.15) / 1.0, 0, 1), [(0.0, (0.93, 0.8, 0.78)), (0.18, (0.98, 0.86, 0.8)), (0.45, (0.72, 0.8, 0.9)), (1.0, (0.42, 0.6, 0.86))])
    clouds = P.fbm(Wt, Ht, 6, 6, r, aspect=0.5)
    band = np.exp(-((lat - 0.2) / 0.3) ** 2)
    cm = P.smoothstep(0.52, 0.66, clouds) * band
    shade = P.fbm(Wt, Ht, 12, 3, r)
    cloud_col = P.blend(P.col((1.0, 0.97, 0.95), Ht, Wt), P.col((0.93, 0.78, 0.82), Ht, Wt), P.smoothstep(0.4, 0.7, shade) * 0.6)
    img = P.blend(img, cloud_col, cm * 0.85)
    img = P.blend(img, P.col((0.78, 0.86, 0.9), Ht, Wt), P.smoothstep(0.0, -0.4, lat))       # haze below the horizon
    emit = P.to_image("backdrop_emit", np.clip(img, 0, 1))
    albedo = bpy.data.images.new("backdrop_albedo", 16, 16, alpha=False); albedo.generated_color = (0, 0, 0, 1); albedo.pack()
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True); P.save_png(os.path.join(PREVIEW, "sky_emit.png"), img)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=90.0)
    obj = bpy.context.active_object; obj.name = "Backdrop"
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT"); bpy.ops.mesh.flip_normals(); bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.shade_smooth()
    m = bpy.data.materials.new("M_Backdrop"); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit
    nt.links.new(t1.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], b.inputs["Emission Color"]); b.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(b.outputs["BSDF"], out.inputs["Surface"])
    obj.data.materials.append(m)
    export(obj, "SM_Backdrop.glb")


def export(obj, out_file):
    OUT = os.path.join(MODELS, out_file)
    bpy.ops.object.select_all(action="DESELECT"); obj.select_set(True); bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_materials="EXPORT", export_image_format="AUTO", export_cameras=False, export_lights=False)
    print("exported", OUT, os.path.getsize(OUT) // 1024, "KB")


if PIECE == "floor": floor()
elif PIECE == "base": base()
elif PIECE == "scenery": scenery()
elif PIECE == "trees": trees()
elif PIECE == "home": hex_tile("Home")
elif PIECE == "away": hex_tile("Away")
elif PIECE == "bench": bench_slot()
elif PIECE == "sky": sky()
