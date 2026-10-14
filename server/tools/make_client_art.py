"""The game client's art (demo 1.9, FEEDBACK V9 "better client just like current TFT"): docs/icons/T_UI_Client*.png and the app icon.

    ~/w2f_bpy/venv/bin/python tools/make_client_art.py            (everything)
    ~/w2f_bpy/venv/bin/python tools/make_client_art.py --2d       (only the flat UI pieces: fast, no Blender render)

The look follows the League / TFT client's hextech style: dark blue-black plates, a bevelled gold rim (#F0E6D2 .. #C8AA6E .. #785A28), teal light
(#0AC8B9) for what is active, chamfered corners.

* Flat pieces (numpy signed distance fields, 4x supersampled):
  T_UI_ClientPlay / ClientPlayHover (the big PLAY button), ClientBtn / ClientBtnHover (gold primary button, 9-sliced), ClientBtn2 (dark secondary
  button), ClientFrame (a panel with a gold rim and corner gems, 9-sliced), ClientTile (a gold frame with an open centre, over the mode art),
  ClientRing (the ready check's dial), ClientRays (a sunburst), ClientGlow (a soft round light), ClientEmber (a spark), ClientHex (a hexagon pattern
  tile), ClientDivider (a gold rule with a gem), ClientIconRing (the round frame around a profile icon), ClientLobbySlot (an empty lobby seat).
* Rendered in Blender (Eevee, the Cinzel font from the UE project): T_UI_ClientCrest (the W2F crest on transparent ground: the client's logo) and
  docs/app_icon/W2F_1024.png (the crest on a dark rounded square: the macOS app icon; scripts/package_mac.sh turns it into the app's icon set).

* Demo 2.0 (the League-style client, FEEDBACK V10): the store's profile borders T_UI_Border_<store id> (320 px: the ring sits where ClientIconRing's does
  on a 256 canvas, with 32 px around it for rays, spikes and gems), ClientLevel (the account level's badge), ClientCrown (the party leader) and
  ClientChat (a speech bubble, white: the client tints it).

tools/unreal/import_ui.py brings the T_UI_ textures into /Game/W2F/Icons (AW2FArena::UiBrush finds them by name).
"""
import math
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "docs", "icons"))
APP = os.path.normpath(os.path.join(HERE, "..", "docs", "app_icon"))
FONT = os.path.expanduser(os.environ.get("W2F_CREST_FONT", "~/W2F/work2fightgame/work2fightgame/Content/W2F/Fonts/Cinzel-700.ttf"))
SS = 4

GOLD_HI = np.array([0.94, 0.90, 0.82])
GOLD = np.array([0.78, 0.67, 0.43])
GOLD_LO = np.array([0.47, 0.35, 0.16])
GOLD_DK = np.array([0.27, 0.22, 0.08])
TEAL = np.array([0.04, 0.78, 0.73])
NIGHT = np.array([0.012, 0.035, 0.07])
NAVY = np.array([0.035, 0.08, 0.16])


# ------------------------------------------------------------------------------------------------ png + sdf helpers

def save_png(path, rgba):
    """rgba: float array (h, w, 4) in 0..1, straight alpha."""
    a = (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w = a.shape[:2]
    raw = b"".join(b"\x00" + a[y].tobytes() for y in range(h))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)
    print("wrote", os.path.relpath(path, os.path.join(HERE, "..")), "%dx%d" % (w, h))


def coords(w, h):
    """Pixel-centre coordinates in pixels of the FINAL image (supersampled grid), y down."""
    xs = (np.arange(w * SS, dtype=np.float32) + 0.5) / SS
    ys = (np.arange(h * SS, dtype=np.float32) + 0.5) / SS
    return np.meshgrid(xs, ys)


def down(a):
    h, w = a.shape[0] // SS, a.shape[1] // SS
    return a.reshape(h, SS, w, SS, *a.shape[2:]).mean(axis=(1, 3))


def cover(d, soft=0.6):
    """Coverage of the inside of a signed distance (pixels, negative inside)."""
    return np.clip(0.5 - d / soft, 0.0, 1.0)


def chamfer_box(x, y, cx, cy, hw, hh, cut):
    """A rectangle with its four corners cut at 45 degrees (the hextech plate). Distance in pixels."""
    qx, qy = np.abs(x - cx) - hw, np.abs(y - cy) - hh
    box = np.maximum(qx, qy)
    corner = (np.abs(x - cx) + np.abs(y - cy) - (hw + hh - cut)) / math.sqrt(2)
    return np.maximum(box, corner)


def circle(x, y, cx, cy, r):
    return np.hypot(x - cx, y - cy) - r


def lerp3(a, b, t):
    t = np.clip(t, 0, 1)[..., None]
    return a * (1 - t) + b * t


def gold_ramp(t):
    """t 0 (top) .. 1 (bottom): the bevelled gold of the League client."""
    t = np.clip(t, 0, 1)
    up = lerp3(GOLD_HI, GOLD, t / 0.45)
    lo = lerp3(GOLD, GOLD_LO, (t - 0.45) / 0.55)
    return np.where((t < 0.45)[..., None], up, lo)


def over(dst, rgb, alpha):
    """Composites a layer (rgb, coverage) over dst (premultiplied rgba)."""
    a = np.clip(alpha, 0, 1)[..., None]
    dst[..., :3] = rgb * a + dst[..., :3] * (1 - a)
    dst[..., 3:4] = a + dst[..., 3:4] * (1 - a)
    return dst


def finish(prem):
    """premultiplied rgba (supersampled) -> straight rgba (final size)."""
    p = down(prem)
    a = p[..., 3:4]
    rgb = np.where(a > 1e-4, p[..., :3] / np.maximum(a, 1e-4), 0.0)
    return np.concatenate([rgb, a], axis=-1)


def canvas(w, h):
    return np.zeros((h * SS, w * SS, 4), np.float32)


def noise(w, h, seed, scale=6.0):
    """Smooth value noise on the supersampled grid (for a little life in the fills)."""
    rng = np.random.default_rng(seed)
    gw, gh = int(w / scale) + 3, int(h / scale) + 3
    g = rng.random((gh, gw)).astype(np.float32)
    x, y = coords(w, h)
    fx, fy = x / scale, y / scale
    ix, iy = fx.astype(int), fy.astype(int)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a, b = g[iy, ix], g[iy, ix + 1]
    c, d = g[iy + 1, ix], g[iy + 1, ix + 1]
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


# ------------------------------------------------------------------------------------------------ the pieces

def plate(w, h, cut, rim, fill_top, fill_bottom, glow=None, glow_strength=0.0, inner_line=True, seed=1, gems=False):
    """The hextech plate: chamfered, a bevelled gold rim, a thin second gold line inside, a dark gradient fill with an optional light from below."""
    x, y = coords(w, h)
    cx, cy = w / 2, h / 2
    hw, hh = w / 2 - 1.5, h / 2 - 1.5
    d = chamfer_box(x, y, cx, cy, hw, hh, cut)
    c = canvas(w, h)
    # rim: gold, lit from above
    t = (y - (cy - hh)) / (2 * hh)
    c = over(c, gold_ramp(t), cover(d))
    # bevel highlight on the rim's outer edge
    edge = np.clip(1 - np.abs(d + 0.8) / 0.9, 0, 1) * np.clip(1 - t * 1.6, 0, 1)
    c[..., :3] += (edge * 0.35)[..., None] * cover(d)[..., None]
    # dark inside
    din = d + rim
    n = noise(w, h, seed, 9.0)
    fill = lerp3(fill_top, fill_bottom, t) * (0.92 + 0.16 * n)[..., None]
    if glow is not None:
        g = np.clip(1 - np.hypot((x - cx) / (hw * 1.1), (y - (cy + hh)) / (hh * 1.4)), 0, 1) ** 1.6 * glow_strength
        fill = fill + glow * g[..., None]
    c = over(c, fill, cover(din))
    if inner_line:
        dl = np.abs(d + rim + 2.2) - 0.55
        c = over(c, gold_ramp(t) * 0.85, cover(dl) * 0.8)
    if gems:   # small teal gems on the middle of the left and right rims
        for gx in (cx - hw + rim * 0.5, cx + hw - rim * 0.5):
            dg = (np.abs(x - gx) + np.abs(y - cy)) - rim * 0.9
            c = over(c, lerp3(TEAL * 1.3, TEAL * 0.5, (y - cy + rim) / (2 * rim)), cover(dg))
    return c


def make_flat():
    # PLAY (TFT/League: the big gold-rimmed button top left): 448 x 128 (shown 196 x 56)
    for name, glow, top, bottom in (("ClientPlay", 0.55, NAVY * 1.2, np.array([0.02, 0.16, 0.2])), ("ClientPlayHover", 1.0, NAVY * 1.6, np.array([0.03, 0.26, 0.3]))):
        c = plate(448, 128, 26, 9, top, bottom, TEAL, glow, seed=3, gems=True)
        save_png(os.path.join(OUT, "T_UI_%s.png" % name), finish(c))
    # primary button (FIND MATCH, ACCEPT, CONFIRM...), 9-sliced: 256 x 80
    save_png(os.path.join(OUT, "T_UI_ClientBtn.png"), finish(plate(256, 80, 16, 5, np.array([0.03, 0.12, 0.16]), np.array([0.02, 0.2, 0.24]), TEAL, 0.45, seed=4)))
    save_png(os.path.join(OUT, "T_UI_ClientBtnHover.png"), finish(plate(256, 80, 16, 5, np.array([0.05, 0.2, 0.25]), np.array([0.04, 0.32, 0.36]), TEAL, 0.9, seed=4)))
    # secondary button: a dark plate with a thin gold line
    x, y = coords(256, 80)
    d = chamfer_box(x, y, 128, 40, 126.5, 38.5, 14)
    c = canvas(256, 80)
    c = over(c, lerp3(NIGHT * 1.6, NIGHT, (y - 2) / 76), cover(d) * 0.94)
    c = over(c, gold_ramp((y - 2) / 76) * 0.9, cover(np.abs(d + 1.0) - 0.7))
    save_png(os.path.join(OUT, "T_UI_ClientBtn2.png"), finish(c))
    # panel frame, 9-sliced: 256 x 256, a gold rim with gems on the corners
    x, y = coords(256, 256)
    d = chamfer_box(x, y, 128, 128, 126, 126, 14)
    c = canvas(256, 256)
    n = noise(256, 256, 7, 12.0)
    c = over(c, lerp3(NAVY * 0.7, NIGHT * 0.8, (y - 2) / 252) * (0.94 + 0.12 * n)[..., None], cover(d) * 0.92)
    c = over(c, gold_ramp((y - 2) / 252), cover(np.abs(d + 1.2) - 1.0))
    for gx, gy in ((7, 7), (249, 7), (7, 249), (249, 249)):
        dg = (np.abs(x - gx) + np.abs(y - gy)) - 6.5
        c = over(c, gold_ramp((y - gy + 6) / 12), cover(dg))
        c = over(c, TEAL * 0.9, cover(dg + 3.2))
    save_png(os.path.join(OUT, "T_UI_ClientFrame.png"), finish(c))
    # mode tile frame (an open centre over the splash art): 256 x 384
    x, y = coords(256, 384)
    d = chamfer_box(x, y, 128, 192, 126, 190, 18)
    ring = np.maximum(d, -(d + 5))
    c = canvas(256, 384)
    shade = np.clip(1 - (-d) / 70, 0, 1) ** 2 * 0.55   # dark inner edge: the art sinks into the frame
    c = over(c, np.zeros(3), shade * cover(d))
    c = over(c, gold_ramp((y - 2) / 380), cover(ring))
    c = over(c, gold_ramp((y - 2) / 380) * 0.8, cover(np.abs(d + 9) - 0.6))
    for gy in (2.5, 381.5):   # gem notches top and bottom centre
        dg = (np.abs(x - 128) + np.abs(y - gy)) - 10
        c = over(c, gold_ramp((y - gy + 10) / 20), cover(dg))
        c = over(c, TEAL, cover(dg + 4))
    save_png(os.path.join(OUT, "T_UI_ClientTile.png"), finish(c))
    # the ready check's dial: 512, a gold ring with ticks (the countdown arc is drawn over it by Slate)
    x, y = coords(512, 512)
    r = np.hypot(x - 256, y - 256)
    ang = np.arctan2(y - 256, x - 256)
    c = canvas(512, 512)
    c = over(c, NIGHT * 1.2, cover(r - 236) * 0.9)
    c = over(c, gold_ramp((y - 20) / 472), cover(np.abs(r - 244) - 6))
    c = over(c, gold_ramp((y - 20) / 472) * 0.75, cover(np.abs(r - 214) - 1.2))
    ticks = np.abs(((ang / (2 * math.pi) * 48) % 1.0) - 0.5) * 2   # 48 ticks
    tick = cover(np.abs(r - 224) - 5) * cover((1 - ticks) * 30 - 1.5)
    c = over(c, GOLD * 0.8, tick)
    save_png(os.path.join(OUT, "T_UI_ClientRing.png"), finish(c))
    # sunburst rays: 512, white, alpha falls off outwards
    x, y = coords(512, 512)
    r = np.hypot(x - 256, y - 256) / 256
    ang = np.arctan2(y - 256, x - 256)
    rays = (0.5 + 0.5 * np.cos(ang * 18)) ** 6 + 0.5 * (0.5 + 0.5 * np.cos(ang * 7 + 1.0)) ** 10
    a = np.clip(rays, 0, 1) * np.clip(1 - r, 0, 1) ** 1.5 * np.clip(r * 6, 0, 1)
    c = canvas(512, 512); c = over(c, np.ones(3), a)
    save_png(os.path.join(OUT, "T_UI_ClientRays.png"), finish(c))
    # soft round light: 256
    x, y = coords(256, 256)
    r = np.hypot(x - 128, y - 128) / 128
    c = canvas(256, 256); c = over(c, np.ones(3), np.clip(1 - r, 0, 1) ** 2.2)
    save_png(os.path.join(OUT, "T_UI_ClientGlow.png"), finish(c))
    # spark: 64
    x, y = coords(64, 64)
    r = np.hypot(x - 32, y - 32) / 32
    star = np.clip(1 - np.minimum(np.abs(x - 32), np.abs(y - 32)) / 3.0, 0, 1) * np.clip(1 - r, 0, 1) ** 2
    c = canvas(64, 64); c = over(c, np.ones(3), np.clip(np.clip(1 - r, 0, 1) ** 3 + 0.6 * star, 0, 1))
    save_png(os.path.join(OUT, "T_UI_ClientEmber.png"), finish(c))
    # hexagon pattern tile (pointy-top hexes; tiles seamlessly): 192 x 222
    w, h = 192, 222
    x, y = coords(w, h)
    best = np.full(x.shape, 1e9, np.float32)
    pts = []   # the lattice: rows half a tile apart, every other row shifted by a quarter tile
    for row in range(-1, 4):
        for col in range(-1, 4):
            pts.append((col * w / 2 + (row % 2) * w / 4, row * h / 2 * 1.0))
    # distance to the nearest hex outline (regular hex SDF)
    k = np.array([-0.8660254, 0.5, 0.57735027], np.float32)
    rad = 52.0
    for px, py in pts:
        qx, qy = np.abs(y - py), np.abs(x - px)   # rotate 90: pointy top
        dot = np.minimum(k[0] * qx + k[1] * qy, 0.0)
        qx2, qy2 = qx - 2 * dot * k[0], qy - 2 * dot * k[1]
        qx2 = qx2 - np.clip(qx2, -k[2] * rad, k[2] * rad)
        qy2 = qy2 - rad
        dist = np.hypot(qx2, qy2) * np.sign(qy2)
        best = np.minimum(best, np.abs(dist))
    c = canvas(w, h); c = over(c, np.ones(3), cover(best - 0.7) * 0.9)
    save_png(os.path.join(OUT, "T_UI_ClientHex.png"), finish(c))
    # gold rule with a gem: 512 x 32
    x, y = coords(512, 32)
    taper = np.clip(1 - np.abs(x - 256) / 250, 0, 1)
    line = cover(np.abs(y - 16) - 0.8 * taper - 0.2) * np.clip(taper * 3, 0, 1)
    c = canvas(512, 32)
    c = over(c, GOLD, line)
    dg = (np.abs(x - 256) / 1.6 + np.abs(y - 16)) - 9
    c = over(c, gold_ramp((y - 7) / 18), cover(dg))
    c = over(c, TEAL, cover(dg + 3.5))
    save_png(os.path.join(OUT, "T_UI_ClientDivider.png"), finish(c))
    # profile icon ring: 256, open centre (the portrait is drawn round under it)
    x, y = coords(256, 256)
    r = np.hypot(x - 128, y - 128)
    c = canvas(256, 256)
    c = over(c, gold_ramp((y - 6) / 244), cover(np.abs(r - 118) - 8))
    c = over(c, GOLD_DK, cover(np.abs(r - 109.5) - 1.0))
    c = over(c, GOLD_HI * 0.9, cover(np.abs(r - 125.5) - 0.8) * np.clip(1 - (y - 6) / 200, 0, 1))
    save_png(os.path.join(OUT, "T_UI_ClientIconRing.png"), finish(c))
    # an empty lobby seat: a dashed hex outline with a plus: 256
    x, y = coords(256, 256)
    ang = np.arctan2(y - 128, x - 128)
    k = np.array([-0.8660254, 0.5, 0.57735027], np.float32)
    qx, qy = np.abs(x - 128), np.abs(y - 128)
    dot = np.minimum(k[0] * qx + k[1] * qy, 0.0)
    qx2, qy2 = qx - 2 * dot * k[0], qy - 2 * dot * k[1]
    qx2 = qx2 - np.clip(qx2, -k[2] * 110, k[2] * 110)
    qy2 = qy2 - 110
    hexd = np.hypot(qx2, qy2) * np.sign(qy2)
    dash = (np.sin(ang * 24) > -0.2).astype(np.float32)
    c = canvas(256, 256)
    c = over(c, NIGHT, cover(hexd) * 0.55)
    c = over(c, GOLD * 0.8, cover(np.abs(hexd) - 2.0) * dash)
    plus = np.minimum(np.maximum(np.abs(x - 128) - 22, np.abs(y - 128) - 3.5), np.maximum(np.abs(x - 128) - 3.5, np.abs(y - 128) - 22))
    c = over(c, GOLD * 0.9, cover(plus))
    save_png(os.path.join(OUT, "T_UI_ClientLobbySlot.png"), finish(c))


# ------------------------------------------------------------------------------------------------ the crest (Blender)

def make_crest():
    import bpy
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    for eng in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            sc.render.engine = eng
            break
        except TypeError:
            continue
    sc.render.resolution_x = sc.render.resolution_y = 1024
    sc.render.film_transparent = True
    sc.view_settings.view_transform = "Standard"
    sc.view_settings.look = "None"
    try:
        sc.eevee.taa_render_samples = 64
    except AttributeError:
        pass

    def material(name, base, metal, rough, emit=None, strength=0.0):
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        p = m.node_tree.nodes["Principled BSDF"]
        p.inputs["Base Color"].default_value = (*base, 1)
        p.inputs["Metallic"].default_value = metal
        p.inputs["Roughness"].default_value = rough
        if emit is not None:
            p.inputs["Emission Color"].default_value = (*emit, 1)
            p.inputs["Emission Strength"].default_value = strength
        return m

    gold = material("Gold", (1.0, 0.7, 0.28), 0.75, 0.3)
    gold_dark = material("GoldDark", (0.6, 0.38, 0.12), 0.7, 0.38)
    enamel = material("Enamel", (0.012, 0.03, 0.075), 0.1, 0.35)
    gem = material("Gem", (0.02, 0.6, 0.6), 0.0, 0.1, (0.05, 0.95, 0.9), 6.0)
    steel = material("Steel", (0.82, 0.86, 0.95), 0.6, 0.22)
    grip = material("Grip", (0.12, 0.05, 0.03), 0.0, 0.6)

    def hex_prism(name, radius, depth, z, mat, bevel=0.0):
        bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=radius, depth=depth, location=(0, 0, z), rotation=(0, 0, math.radians(30)))
        o = bpy.context.active_object
        o.name = name
        o.data.materials.append(mat)
        if bevel > 0:
            b = o.modifiers.new("b", "BEVEL"); b.width = bevel; b.segments = 3; b.limit_method = "ANGLE"
        return o

    # the crest: a gold hexagon, an enamel inner hexagon, a gold inner rim
    outer = hex_prism("Outer", 1.0, 0.16, 0.0, gold, 0.03)
    hex_prism("Inner", 0.84, 0.18, 0.012, enamel, 0.01)
    rim = hex_prism("Rim", 0.78, 0.2, 0.0, gold_dark, 0.0)
    cut = hex_prism("RimCut", 0.74, 0.4, 0.0, enamel)
    m = rim.modifiers.new("cut", "BOOLEAN"); m.object = cut; m.operation = "DIFFERENCE"
    bpy.context.view_layer.objects.active = rim
    bpy.ops.object.modifier_apply(modifier="cut")
    bpy.data.objects.remove(cut)
    # crossed swords behind the letters: a tapered blade, a gold guard, a grip and a pommel
    for sign in (-1, 1):
        ang = math.radians(38 * sign)
        ux, uy = -math.sin(ang), math.cos(ang)            # along the sword, towards the tip
        def at(t, z):
            return (ux * t, uy * t, z)
        bpy.ops.mesh.primitive_cube_add(size=1, location=at(0.17, 0.115))
        blade = bpy.context.active_object
        blade.scale = (0.11, 1.42, 0.03)
        blade.rotation_euler = (0, 0, ang)
        blade.data.materials.append(steel)
        bv = blade.modifiers.new("b", "BEVEL"); bv.width = 0.03; bv.segments = 2
        bpy.ops.mesh.primitive_cube_add(size=1, location=at(-0.6, 0.14))
        guard = bpy.context.active_object
        guard.scale = (0.42, 0.07, 0.06)
        guard.rotation_euler = (0, 0, ang)
        guard.data.materials.append(gold)
        bv = guard.modifiers.new("b", "BEVEL"); bv.width = 0.02; bv.segments = 2
        bpy.ops.mesh.primitive_cylinder_add(radius=0.04, depth=0.3, location=at(-0.79, 0.12))
        g = bpy.context.active_object
        g.rotation_euler = (math.radians(90), 0, ang)
        g.data.materials.append(grip)
        bpy.ops.mesh.primitive_uv_sphere_add(radius=0.07, location=at(-0.97, 0.12))
        bpy.context.active_object.data.materials.append(gold)
    # gems: on the left and right points and the middle of the top edge
    for gx, gy, r in ((-0.92, 0.0, 0.07), (0.92, 0.0, 0.07), (0.0, 0.8, 0.085)):
        bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=3, radius=r, location=(gx, gy, 0.11))
        o = bpy.context.active_object
        o.scale = (1, 1, 0.6)
        o.data.materials.append(gem)
    # the letters. Cinzel draws its W from overlapping strokes, and Blender fills a text's outlines even-odd (the overlaps would be holes): so every
    # contour becomes its own filled, extruded shape (W2F has no counters that would need to stay open).
    if os.path.exists(FONT):
        bpy.ops.object.text_add(location=(0, -0.02, 0.22))
        t = bpy.context.active_object
        t.data.body = "W2F"
        t.data.font = bpy.data.fonts.load(FONT)
        t.data.align_x = "CENTER"
        t.data.align_y = "CENTER"
        t.data.size = 0.62
        bpy.ops.object.convert(target="CURVE")
        outline = bpy.context.active_object
        for k, spline in enumerate(outline.data.splines):
            cu = bpy.data.curves.new("Letter%d" % k, "CURVE")
            cu.dimensions = "2D"
            cu.fill_mode = "BOTH"
            cu.extrude = 0.06
            cu.bevel_depth = 0.006
            cu.bevel_resolution = 2
            sp = cu.splines.new("BEZIER")
            sp.bezier_points.add(len(spline.bezier_points) - 1)
            for src, dst in zip(spline.bezier_points, sp.bezier_points):
                dst.co, dst.handle_left, dst.handle_right = src.co.copy(), src.handle_left.copy(), src.handle_right.copy()
                dst.handle_left_type, dst.handle_right_type = src.handle_left_type, src.handle_right_type
            sp.use_cyclic_u = True
            o = bpy.data.objects.new("Letter%d" % k, cu)
            o.location = outline.location
            o.location.z += 0.0005 * k   # (coplanar overlaps would flicker)
            bpy.context.collection.objects.link(o)
            cu.materials.append(gold)
        bpy.data.objects.remove(outline)
    # camera and lights
    bpy.ops.object.camera_add(location=(0, -0.55, 6.0))
    cam = bpy.context.active_object
    cam.rotation_euler = (math.radians(5), 0, 0)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 2.35
    sc.camera = cam
    for loc, energy, size in (((-2.5, 2.5, 5), 260, 2.5), ((3.5, -2.5, 3), 120, 2), ((0, -4, 2), 70, 5)):
        bpy.ops.object.light_add(type="AREA", location=loc)
        L = bpy.context.active_object
        L.data.energy = energy
        L.data.size = size
        direction = -np.array(loc, dtype=np.float64)
        L.rotation_euler = (math.atan2(math.hypot(direction[0], direction[1]), -direction[2]), 0, math.atan2(direction[1], direction[0]) + math.pi / 2)
    world = bpy.data.worlds.new("W")
    sc.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs["Color"].default_value = (0.45, 0.5, 0.62, 1)
    bg.inputs["Strength"].default_value = 0.45
    path = os.path.join(OUT, "T_UI_ClientCrest_full.png")
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("rendered the crest")
    return path


def read_png(path):
    import bpy
    img = bpy.data.images.load(path)
    w, h = img.size
    a = np.array(img.pixels[:], np.float32).reshape(h, w, 4)[::-1]
    return a


def make_icons_from_crest(full):
    rgba = read_png(full)   # 1024, straight alpha (Blender writes straight RGBA PNGs)
    # the client logo: 256
    prem = rgba.copy(); prem[..., :3] *= prem[..., 3:4]
    p = prem.reshape(256, 4, 256, 4, 4).mean(axis=(1, 3))
    a = p[..., 3:4]
    logo = np.concatenate([np.where(a > 1e-4, p[..., :3] / np.maximum(a, 1e-4), 0), a], axis=-1)
    save_png(os.path.join(OUT, "T_UI_ClientCrest.png"), logo)
    # the app icon: a dark rounded square (macOS: 824 of 1024 with a margin), a teal-violet glow, the crest
    n = 1024
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32) + 0.5
    half, rad = 412.0, 185.0
    qx, qy = np.abs(xx - 512) - (half - rad), np.abs(yy - 500) - (half - rad)
    d = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - rad
    inside = np.clip(0.5 - d, 0, 1)
    t = np.clip((yy - 88) / 824, 0, 1)
    bgc = lerp3(np.array([0.09, 0.07, 0.22]), np.array([0.01, 0.025, 0.06]), t)
    glow = np.clip(1 - np.hypot((xx - 512) / 430, (yy - 470) / 430), 0, 1) ** 2
    bgc = bgc + (np.array([0.05, 0.45, 0.5]) * 0.55)[None, None, :] * glow[..., None]
    rim = np.clip(1 - np.abs(d + 7) / 3.5, 0, 1)
    bgc = bgc * (1 - rim[..., None]) + gold_ramp(t) * rim[..., None]
    out = np.zeros((n, n, 4), np.float32)
    out[..., :3] = bgc * inside[..., None]
    out[..., 3] = inside
    # crest scaled to 78% of the square, centred
    s = int(n * 0.74)
    idx = (np.arange(s) * (n / s)).astype(int)
    cr = rgba[idx][:, idx]
    crp = cr.copy(); crp[..., :3] *= crp[..., 3:4]
    o = (n - s) // 2
    region = out[o - 12:o - 12 + s, o:o + s]
    a = crp[..., 3:4]
    region[..., :3] = crp[..., :3] + region[..., :3] * (1 - a)
    region[..., 3:4] = a + region[..., 3:4] * (1 - a)
    a = out[..., 3:4]
    straight = np.concatenate([np.where(a > 1e-4, out[..., :3] / np.maximum(a, 1e-4), 0), a], axis=-1)
    save_png(os.path.join(APP, "W2F_1024.png"), straight)
    os.remove(full)


# ------------------------------------------------------------------------------------------------ demo 2.0: borders, level badge, crown, chat

def ramp(stops, t):
    """A colour ramp through (position, rgb) stops."""
    t = np.clip(t, 0, 1)
    out = np.zeros(t.shape + (3,), np.float32)
    for (p0, c0), (p1, c1) in zip(stops[:-1], stops[1:]):
        m = (t >= p0) & (t <= p1)
        k = ((t - p0) / max(p1 - p0, 1e-6))[..., None]
        out = np.where(m[..., None], np.array(c0) * (1 - k) + np.array(c1) * k, out)
    return out


def hsv(h, s, v):
    h = (h % 1.0) * 6.0
    i = np.floor(h).astype(int) % 6
    f = h - np.floor(h)
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r = np.choose(i, [v, q, p, p, t, v]); g = np.choose(i, [t, v, v, q, p, p]); b = np.choose(i, [p, p, t, v, v, q])
    return np.stack([r, g, b], -1)


def make_borders():
    S, C = 320, 160.0
    x, y = coords(S, S)
    r = np.hypot(x - C, y - C)
    ang = np.arctan2(y - C, x - C)               # 0 = right, +pi/2 = down
    lit = np.clip((y - (C - 126)) / 252, 0, 1)   # 0 top .. 1 bottom: light from above
    band = np.abs(r - 118) - 8                   # the ring itself (as on ClientIconRing)

    def ring(c, top, mid, low, width=8.0, inner=True):
        rgb = ramp([(0.0, top), (0.45, mid), (1.0, low)], lit)
        c = over(c, rgb, cover(np.abs(r - 118) - width))
        c = over(c, np.array(low) * 0.45, cover(np.abs(r - (118 - width - 1.5)) - 1.0) if inner else np.zeros_like(r))
        c = over(c, np.array(top), cover(np.abs(r - (118 + width - 0.8)) - 0.7) * np.clip(1 - lit * 1.3, 0, 1))
        return c

    def spikes(c, n, r0, r1, half_width, rgb, phase=0.0, jitter=None):
        """n triangular points from r0 out to r1 (pixels), each half_width radians wide at its base."""
        k = (ang - phase) * n / (2 * np.pi)
        frac = np.abs(k - np.round(k))                       # 0 at a spike's axis .. 0.5 between
        reach = r1 if jitter is None else r0 + (r1 - r0) * jitter[(np.round(k).astype(int)) % n]
        width = half_width * n / (2 * np.pi) * np.clip((reach - r) / np.maximum(reach - r0, 1), 0, 1)
        d = np.where((r > r0 - 2) & (r < reach), (frac - width) * (2 * np.pi * r / n), 99.0)
        return over(c, rgb, cover(d, 0.9))

    def gem(c, cx, cy, size, rgb):
        d = (np.abs(x - cx) + np.abs(y - cy)) - size
        c = over(c, np.array(GOLD_LO), cover(d - 2.2))
        c = over(c, lerp3(np.array(rgb) * 1.4, np.array(rgb) * 0.55, (y - cy + size) / (2 * size)), cover(d))
        return over(c, np.array([1.0, 1.0, 1.0]), cover(np.hypot(x - cx + size * 0.3, y - cy + size * 0.3) - size * 0.22) * 0.8)

    def at(deg, radius):
        a = math.radians(deg)
        return C + radius * math.cos(a), C + radius * math.sin(a)

    made = {}
    # bronze: the default rim
    c = canvas(S, S)
    made["border_bronze"] = ring(c, (0.95, 0.72, 0.5), (0.72, 0.45, 0.26), (0.36, 0.2, 0.1))
    # silver: cool polished silver
    c = canvas(S, S)
    made["border_silver"] = ring(c, (0.97, 0.98, 1.0), (0.72, 0.76, 0.82), (0.36, 0.4, 0.46))
    # gold: thicker gold, four teal gems
    c = canvas(S, S)
    c = ring(c, tuple(GOLD_HI), tuple(GOLD), tuple(GOLD_LO), 10.0)
    for deg in (-90, 0, 90, 180):
        c = gem(c, *at(deg, 118), 9, TEAL)
    made["border_gold"] = c
    # platinum: pale teal-silver with a halo of twelve points
    c = canvas(S, S)
    c = spikes(c, 12, 124, 150, 0.09, np.array([0.75, 0.95, 0.92]), -math.pi / 2)
    made["border_platinum"] = ring(c, (0.95, 1.0, 1.0), (0.62, 0.84, 0.82), (0.25, 0.4, 0.42), 9.0)
    # hextech: gold ring, a teal inner line and six crystals
    c = canvas(S, S)
    c = ring(c, tuple(GOLD_HI), tuple(GOLD), tuple(GOLD_LO), 9.0)
    c = over(c, TEAL * 1.2, cover(np.abs(r - 106) - 1.2))
    for deg in range(-90, 270, 60):
        c = gem(c, *at(deg, 128), 12, TEAL)
    made["border_hextech"] = c
    # helios: sixteen sun rays behind a warm gold ring
    c = canvas(S, S)
    c = spikes(c, 16, 120, 158, 0.12, np.array([1.0, 0.62, 0.18]), -math.pi / 2)
    c = spikes(c, 16, 120, 140, 0.07, np.array([1.0, 0.86, 0.45]), -math.pi / 2 + math.pi / 16)
    made["border_helios"] = ring(c, (1.0, 0.92, 0.62), (0.95, 0.62, 0.2), (0.55, 0.28, 0.06), 10.0)
    # phaisa: violet, jagged void spikes of uneven length
    c = canvas(S, S)
    rng = np.random.default_rng(7)
    c = spikes(c, 9, 118, 160, 0.16, np.array([0.55, 0.18, 0.9]), 0.3, jitter=rng.uniform(0.45, 1.0, 9))
    c = spikes(c, 9, 118, 145, 0.1, np.array([0.85, 0.5, 1.0]), 0.65, jitter=rng.uniform(0.4, 1.0, 9))
    made["border_phaisa"] = ring(c, (0.88, 0.7, 1.0), (0.5, 0.2, 0.85), (0.18, 0.04, 0.32), 9.0)
    # selini: silver-blue, a crescent on top and small stars
    c = canvas(S, S)
    c = ring(c, (0.92, 0.95, 1.0), (0.55, 0.65, 0.95), (0.2, 0.25, 0.5), 8.0)
    mx, my = C, C - 132
    moon = np.maximum(np.hypot(x - mx, y - my) - 20, -(np.hypot(x - mx - 9, y - my + 6) - 17))
    c = over(c, np.array([0.3, 0.35, 0.6]), cover(moon - 2.5))
    c = over(c, lerp3(np.array([1.0, 1.0, 0.95]), np.array([0.7, 0.78, 1.0]), (y - my + 20) / 40), cover(moon))
    for deg, rad, sz in ((-35, 140, 5), (-145, 138, 6), (25, 136, 4), (160, 137, 4), (95, 140, 5), (60, 142, 3), (125, 141, 3)):
        sx, sy = at(deg, rad)
        star = np.minimum(np.abs(x - sx) * 3 + np.abs(y - sy), np.abs(x - sx) + np.abs(y - sy) * 3) - sz * 2
        c = over(c, np.array([0.95, 0.97, 1.0]), cover(star))
    made["border_selini"] = c
    # prismatic: every colour around the ring, white sparkles
    c = canvas(S, S)
    hue = hsv((ang / (2 * np.pi)) + 0.15, 0.55, 1.0)
    c = spikes(c, 8, 122, 152, 0.11, np.array([1.0, 1.0, 1.0]), -math.pi / 2)
    c = over(c, hue * (1.05 - 0.35 * lit[..., None]), cover(np.abs(r - 118) - 10))
    c = over(c, np.array([1.0, 1.0, 1.0]), cover(np.abs(r - 127.3) - 0.8) * 0.9)
    c = over(c, np.array([0.25, 0.15, 0.35]), cover(np.abs(r - 106.5) - 1.0))
    for deg in (-90, -30, 30, 90, 150, 210):
        sx, sy = at(deg, 118)
        star = np.minimum(np.abs(x - sx) * 4 + np.abs(y - sy), np.abs(x - sx) + np.abs(y - sy) * 4) - 14
        c = over(c, np.array([1.0, 1.0, 1.0]), cover(star))
    made["border_prismatic"] = c
    for name, img in made.items():
        save_png(os.path.join(OUT, "T_UI_Border_%s.png" % name), finish(img))


def make_badges():
    # the account level: a gold-rimmed hexagon (128)
    x, y = coords(128, 128)
    k = np.array([-0.8660254, 0.5, 0.57735027], np.float32)
    def hexagon(R):
        qx, qy = np.abs(y - 64), np.abs(x - 64)   # pointy left / right
        dot = np.minimum(k[0] * qx + k[1] * qy, 0.0)
        qx2, qy2 = qx - 2 * dot * k[0], qy - 2 * dot * k[1]
        qx2 = qx2 - np.clip(qx2, -k[2] * R, k[2] * R)
        qy2 = qy2 - R
        return np.hypot(qx2, qy2) * np.sign(qy2)
    c = canvas(128, 128)
    c = over(c, gold_ramp((y - 6) / 116), cover(hexagon(58)))
    c = over(c, lerp3(NAVY * 1.4, NIGHT, (y - 14) / 100), cover(hexagon(50)))
    c = over(c, TEAL * 0.9, cover(np.abs(hexagon(46)) - 1.0) * 0.8)
    save_png(os.path.join(OUT, "T_UI_ClientLevel.png"), finish(c))
    # the party leader's crown (128 x 96)
    x, y = coords(128, 96)
    c = canvas(128, 96)
    body = np.maximum(np.abs(x - 64) - 46, np.abs(y - 66) - 14)
    pts = np.full(x.shape, 99.0, np.float32)
    for px, top in ((22, 22), (43, 34), (64, 12), (85, 34), (106, 22)):
        tri = np.maximum(np.abs(x - px) - (y - top) * 0.42, top - y)
        pts = np.minimum(pts, np.where(y < 70, tri, 99.0))
    crown = np.minimum(body, np.maximum(pts, y - 70))
    c = over(c, GOLD_DK, cover(crown - 2.5))
    c = over(c, gold_ramp((y - 10) / 74), cover(crown))
    for px, top in ((22, 22), (64, 12), (106, 22)):
        c = over(c, TEAL * 1.2, cover(np.hypot(x - px, y - top) - 5.5))
    c = over(c, TEAL, cover((np.abs(x - 64) + np.abs(y - 66)) - 7))
    save_png(os.path.join(OUT, "T_UI_ClientCrown.png"), finish(c))
    # chat: a speech bubble (white, 64)
    x, y = coords(64, 64)
    bubble = np.maximum(np.hypot((x - 32) / 1.15, y - 28) - 22, -100)
    tail = np.maximum(np.maximum(x - 26, 14 - x + (y - 44) * 0.0), np.maximum(44 - y, (y - 58) + (x - 14) * 0.9))
    shape = np.minimum(bubble, tail)
    c = canvas(64, 64)
    c = over(c, np.array([1.0, 1.0, 1.0]), cover(shape))
    for dx in (-10, 0, 10):
        c[..., 3] *= 1.0 - cover(np.hypot(x - 32 - dx, y - 28) - 3.2)
    save_png(os.path.join(OUT, "T_UI_ClientChat.png"), finish(c))


def main():
    make_flat()
    make_borders()
    make_badges()
    if "--2d" in sys.argv:
        return
    full = make_crest()
    make_icons_from_crest(full)


main()
