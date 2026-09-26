"""Splash art and portraits for every Mixamo hero, for the shop cards, the info panel and the scoreboard avatars.

    ~/w2f_bpy/venv/bin/python tools/mixamo/render_art.py [--only 9001,9014] [--out DIR]

For each hero of config.json it rebuilds the model the way build_heroes.py does (character, proportions, recolours, props; no decimation, no export), poses it at the
impact frame of its attack, renders it with EEVEE on a transparent film (a key light plus two rim lights in the hero's ability colour from tools/mixamo/fx.json), and
composites it over a painted backdrop: a dark gradient in the hero's colour, a light burst behind it, energy streaks, bokeh sparks, a vignette.

Output, next to the hero's build: <out>/<id>_<Name>/T_Splash_<id>.png (768 x 432) and T_Portrait_<id>.png (256 x 256, replaces the plain portrait build_heroes.py made).
tools/unreal/import_blockouts.py import_hero_portraits() brings both into /Game/W2F/Icons; the designer's own splash arts (docs/icons/T_Splash_<id>.png) win over these.
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_heroes as bh   # noqa: E402  (bpy is imported by it)
import bpy                  # noqa: E402
import mathutils            # noqa: E402
import numpy as np          # noqa: E402

SPLASH = (768, 432)
PORTRAIT = (256, 256)


# ------------------------------------------------------------------------------------------------------------------------------ painting
def noise(w, h, cells, rng, aspect=1.0):
    """Smooth value noise, bilinear-upsampled from a random grid (numpy only)."""
    gx, gy = max(2, int(cells)), max(2, int(cells * h / w * aspect))
    g = rng.random((gy + 1, gx + 1)).astype(np.float32)
    xs = np.linspace(0, gx, w, endpoint=False, dtype=np.float32); ys = np.linspace(0, gy, h, endpoint=False, dtype=np.float32)
    x0 = xs.astype(int); y0 = ys.astype(int); fx = xs - x0; fy = ys - y0
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy)
    a = g[y0][:, x0]; b = g[y0][:, x0 + 1]; c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
    top = a + (b - a) * fx[None, :]; bot = c + (d - c) * fx[None, :]
    return top + (bot - top) * fy[:, None]


def fbm(w, h, cells, octaves, rng, aspect=1.0):
    out = np.zeros((h, w), np.float32); amp = 1.0; tot = 0.0
    for o in range(octaves):
        out += amp * noise(w, h, cells * 2 ** o, rng, aspect); tot += amp; amp *= 0.5
    return out / tot


def backdrop(w, h, colour, accent, centre, seed):
    """The painted background: rows top -> bottom, display values 0..1."""
    rng = np.random.default_rng(seed)
    col = np.array(colour, np.float32); acc = np.array(accent, np.float32)
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    u = xs / w; v = ys / h
    base = col * 0.1 + (col * 0.28 - col * 0.1) * (v[..., None] ** 1.2)                  # dark at the top, the hero's colour pooling below
    img = base + np.array([0.02, 0.02, 0.03], np.float32)
    cx, cy = centre
    r = np.sqrt(((u - cx) * w / h) ** 2 + (v - cy) ** 2)
    img += acc * (np.exp(-(r / 0.32) ** 2) * 0.75)[..., None]                               # the light burst behind the hero
    ang = np.arctan2(v - cy, (u - cx) * w / h)
    rays = (0.5 + 0.5 * np.sin(ang * 9 + fbm(w, h, 3, 2, rng) * 6)) ** 6 * np.exp(-r / 0.55)
    img += acc * (rays * 0.25)[..., None]
    streak = fbm(w, h, 4, 4, rng, aspect=0.25)                                              # energy streaks
    img += col * (np.clip((streak - 0.55) * 4, 0, 1) * 0.35)[..., None]
    clouds = fbm(w, h, 2.5, 5, rng)
    img *= (0.75 + 0.5 * clouds)[..., None]
    for _ in range(int(w * h / 3500)):                                                      # bokeh sparks
        px, py = rng.random() * w, rng.random() * h
        rad = rng.uniform(1.0, 4.5) * (w / 768)
        a = rng.uniform(0.15, 0.7)
        x0, x1 = int(max(0, px - rad * 3)), int(min(w, px + rad * 3 + 1)); y0, y1 = int(max(0, py - rad * 3)), int(min(h, py + rad * 3 + 1))
        if x0 >= x1 or y0 >= y1: continue
        d = np.hypot(xs[y0:y1, x0:x1] - px, ys[y0:y1, x0:x1] - py)
        img[y0:y1, x0:x1] += acc * (np.exp(-(d / rad) ** 2) * a)[..., None]
    return img


def finish(img, colour):
    """Bloom on the highlights, a vignette, a touch of colour grading; clamps to 0..1."""
    h, w = img.shape[:2]
    bright = np.clip(img - 0.7, 0, None)
    small = bright[::8, ::8]
    for _ in range(3):                                                                        # a cheap blur of the bright pass
        small = (small + np.roll(small, 1, 0) + np.roll(small, -1, 0) + np.roll(small, 1, 1) + np.roll(small, -1, 1)) / 5
    glow = np.repeat(np.repeat(small, 8, 0), 8, 1)[:h, :w]
    img = img + glow * 0.8
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    vig = 1.0 - 0.55 * np.clip(np.hypot((xs / w - 0.5) * 1.3, ys / h - 0.5) - 0.2, 0, 1) ** 1.5
    img *= vig[..., None]
    img = img * 1.05 + np.array(colour, np.float32) * 0.03
    return np.clip(img, 0, 1)


def over(bg, fg_rgba):
    a = fg_rgba[..., 3:4]
    return bg * (1 - a) + fg_rgba[..., :3] * a


def rim_glow(fg_rgba, accent, strength=0.9):
    """A soft glow of the hero's colour around its silhouette (painted, like the splash arts' rim light)."""
    a = fg_rgba[..., 3]
    small = a[::4, ::4]
    for _ in range(4):
        small = (small + np.roll(small, 1, 0) + np.roll(small, -1, 0) + np.roll(small, 1, 1) + np.roll(small, -1, 1)) / 5
    halo = np.repeat(np.repeat(small, 4, 0), 4, 1)[:a.shape[0], :a.shape[1]]
    return np.clip(halo - a, 0, 1)[..., None] * np.array(accent, np.float32) * strength


# ------------------------------------------------------------------------------------------------------------------------------ rendering
def setup_render(sc):
    for eng in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        try:
            sc.render.engine = eng
            break
        except TypeError:
            continue
    sc.view_settings.view_transform = "Standard"
    sc.render.film_transparent = True
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    world = bpy.data.worlds.new("w"); sc.world = world; world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    bg.inputs[0].default_value = (0.4, 0.42, 0.5, 1); bg.inputs[1].default_value = 0.45


def lights(target, h, colour, accent):
    made = []
    for name, loc, energy, rgb in (("key", (-1.2, -2.2, 1.6), 140.0, (1.0, 0.95, 0.88)),
                                   ("rimL", (-1.6, 1.8, 1.2), 130.0, accent),
                                   ("rimR", (1.8, 1.6, 0.9), 110.0, colour),
                                   ("fill", (1.5, -1.5, -0.2), 35.0, (0.7, 0.75, 0.95))):
        ld = bpy.data.lights.new(name, "AREA"); ld.energy = energy * h * h; ld.size = 1.2 * h; ld.color = rgb
        lo = bpy.data.objects.new(name, ld); bpy.context.scene.collection.objects.link(lo)
        lo.location = target + mathutils.Vector(loc) * h
        lo.rotation_euler = (target - lo.location).to_track_quat("-Z", "Y").to_euler()
        made.append(lo)
    return made


def shoot(cam_loc, target, lens, size, path):
    sc = bpy.context.scene
    cam = bpy.data.objects.get("artcam") or bpy.data.objects.new("artcam", bpy.data.cameras.new("artcam"))
    if cam.name not in sc.collection.objects:
        sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.lens = lens
    cam.location = cam_loc
    cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
    sc.render.resolution_x, sc.render.resolution_y = size
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    arr = np.array(img.pixels[:], np.float32).reshape(size[1], size[0], 4)[::-1].copy()
    bpy.data.images.remove(img)
    return arr


def screen_pos(cam_loc, target, point):
    """Roughly where a point lands in the frame (0..1, y down), to centre the light burst on the hero's chest."""
    from bpy_extras.object_utils import world_to_camera_view
    sc = bpy.context.scene
    co = world_to_camera_view(sc, sc.camera, point)
    return float(co.x), float(1.0 - co.y)


def save(arr, path):
    h, w = arr.shape[:2]
    img = bpy.data.images.new("out", w, h, alpha=False)
    rgba = np.concatenate([arr[::-1], np.ones((h, w, 1), np.float32)], axis=2)
    img.pixels.foreach_set(rgba.reshape(-1))
    img.filepath_raw = path; img.file_format = "PNG"; img.save()
    bpy.data.images.remove(img)


def art(hid, hero, cfg, fx, out_root, tmp):
    stem = "%s_%s" % (hid, hero["name"])
    out_dir = os.path.join(out_root, stem)
    os.makedirs(out_dir, exist_ok=True)
    bh.reset()
    arm, meshes = bh.load_character(os.path.join(bh.MODELS, hero["model"]))
    bh.normalise_height(arm, meshes, float(hero.get("height", 1.8)))
    look = hero.get("look", {})
    bh.scale_bones(arm, meshes, look.get("scale_bones"))
    bh.process_textures(meshes, look, tmp, stem)
    for i, p in enumerate(look.get("props", [])):
        bh.add_prop(arm, meshes, p, stem, i)
    h = bh.model_height(meshes)
    clip = hero["attack"]
    data = bh.clip_data(clip, cfg["clips"][clip])
    act = bh.apply_clip(arm, "A_" + clip, data)
    arm.animation_data.action = act
    if hasattr(arm.animation_data, "action_slot") and act.slots:
        arm.animation_data.action_slot = act.slots[0]
    sc = bpy.context.scene
    sc.frame_set(max(1, int(round(data["impact"] * 30))))
    fxc = fx["champions"].get(hid, fx.get("default", {}))
    colour = [min(1.0, c) for c in fxc.get("colour", [0.6, 0.7, 1.0])]
    accent = [min(1.0, c) for c in fxc.get("accent", colour)]
    setup_render(sc)
    head = arm.matrix_world @ arm.pose.bones[bh.PREFIX + "Head"].head
    chest = arm.matrix_world @ arm.pose.bones[bh.PREFIX + "Spine2"].head if (bh.PREFIX + "Spine2") in arm.pose.bones else head
    lights(chest, h, colour, accent)
    seed = int(hid)

    # splash: a low 3/4 shot from the thighs up, the hero a little left of centre
    target = chest + mathutils.Vector((h * 0.12, 0, -h * 0.08))
    a = math.radians(-28)
    cam = target + mathutils.Vector((math.sin(a) * h * 2.1, -math.cos(a) * h * 2.1, -h * 0.12))
    fg = shoot(cam, target, 50, SPLASH, os.path.join(tmp, "splash.png"))
    c = screen_pos(cam, target, chest)
    bg = backdrop(SPLASH[0], SPLASH[1], colour, accent, c, seed)
    img = over(bg + rim_glow(fg, accent), fg)
    save(finish(img, colour), os.path.join(out_dir, "T_Splash_%s.png" % hid))

    # portrait: head and shoulders, straight on
    neck = arm.matrix_world @ arm.pose.bones[bh.PREFIX + "Neck"].head if (bh.PREFIX + "Neck") in arm.pose.bones else head
    target = (head + neck) / 2 + mathutils.Vector((0, 0, h * 0.02))
    a = math.radians(-18)
    cam = target + mathutils.Vector((math.sin(a) * h * 0.95, -math.cos(a) * h * 0.95, h * 0.02))
    fg = shoot(cam, target, 70, PORTRAIT, os.path.join(tmp, "portrait.png"))
    bg = backdrop(PORTRAIT[0], PORTRAIT[1], colour, accent, (0.5, 0.45), seed + 1)
    img = over(bg + rim_glow(fg, accent, 0.7), fg)
    save(finish(img, colour), os.path.join(out_dir, "T_Portrait_%s.png" % hid))
    bh.log("%s: splash + portrait" % stem)


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    only, out_root = None, bh.DEFAULT_OUT
    if "--only" in argv: only = set(argv[argv.index("--only") + 1].split(","))
    if "--out" in argv: out_root = argv[argv.index("--out") + 1]
    with open(os.path.join(HERE, "config.json")) as f:
        cfg = json.load(f)
    with open(os.path.join(HERE, "fx.json")) as f:
        fx = json.load(f)
    tmp = os.path.join(out_root, "_art_tmp")
    os.makedirs(tmp, exist_ok=True)
    for hid, hero in cfg["heroes"].items():
        if hid.startswith("_") or (only and hid not in only):
            continue
        try:
            art(hid, hero, cfg, fx, out_root, tmp)
        except Exception as ex:   # one broken hero must not stop the batch
            bh.log("%s: art failed: %s" % (hid, ex))


if __name__ == "__main__":
    main()
