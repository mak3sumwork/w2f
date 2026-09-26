"""Builds the Mixamo stand-in heroes for Unreal: one FBX per hero (skinned mesh + its own copy of every clip it uses) plus its textures and an entry in unit_visuals.json.

Run with Blender as a Python module (the venv from docs/models/README.md):
    ~/w2f_bpy/venv/bin/python tools/mixamo/build_heroes.py [--only 9001,9014] [--out DIR] [--preview]

Input:  tools/mixamo/config.json, docs/examples/*.fbx (characters), docs/animations/** (clips). Both folders are git-ignored Mixamo downloads.
Output: <out>/<id>_<Name>/SKM_<id>_<Name>.fbx + T_<id>_<Name>_{albedo,emit}.png, and <out>/unit_visuals.json (the UE project reads it; see AW2FArena).
        Default <out> = ~/Desktop/work2fightgame/work2fightgame/SourceArt/Mixamo (inside the UE project, outside Content/).

What happens to a hero, in order:
  1. import the Mixamo FBX, rename every bone to the plain "mixamorig:" prefix (some downloads say mixamorig1:, mixamorig5: ...), drop the bind action, apply the 0.01 scale and the
     axis rotation so the armature works in metres,
  2. look edits from the config: bone proportions (pose scale baked into a new rest pose), recolour of the albedo, a glow (emissive) mask, props (shield, ...) skinned rigidly to a bone,
  3. heavy meshes are decimated to about 20k triangles and textures capped at 2048,
  4. every clip the hero uses is copied onto its own armature: local rotations one to one (all Mixamo rigs share the bone axes), the hips translation scaled to the hero's hip height and
     made "in place" (the game moves the unit; the drift from start to end is removed),
  5. export: FBX with the armature named "Armature" (Unreal drops that node, so the root bone is mixamorig:Hips), no leaf bones, one take per clip (A_<clip>).
"""
import json
import math
import os
import sys

import bpy  # noqa: I001  (bpy first: it makes bmesh / mathutils importable)
import bmesh
import mathutils
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.normpath(os.path.join(HERE, "..", ".."))
MODELS = os.path.join(SERVER, "docs", "examples")
CLIPS = os.path.join(SERVER, "docs", "animations")
DEFAULT_OUT = os.path.expanduser("~/Desktop/work2fightgame/work2fightgame/SourceArt/Mixamo")
PREFIX = "mixamorig:"
TRI_BUDGET = 20000
TEX_MAX = 2048


def log(*a):
    print("[mixamo]", *a, flush=True)


# ------------------------------------------------------------------------------------------------------------------------------ scene helpers

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = 30


def import_fbx(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, use_anim=True, ignore_leaf_bones=True, automatic_bone_orientation=False)
    return [o for o in bpy.data.objects if o not in before]


def normalise_bone_names(arm):
    for b in arm.data.bones:
        if ":" in b.name:
            short = b.name.split(":")[-1]
            if b.name != PREFIX + short:
                b.name = PREFIX + short       # the RNA setter renames the vertex groups and the fcurves too


def bone(arm, short):
    return arm.pose.bones.get(PREFIX + short)


def select_only(objs, active=None):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active or objs[0]


# ------------------------------------------------------------------------------------------------------------------------------ 1. the character

def load_character(path):
    objs = import_fbx(path)
    arm = next(o for o in objs if o.type == "ARMATURE")
    meshes = [o for o in objs if o.type == "MESH"]
    normalise_bone_names(arm)
    if arm.animation_data:
        arm.animation_data.action = None
    for a in list(bpy.data.actions):
        bpy.data.actions.remove(a)
    arm.name = "Armature"
    arm.data.name = "Armature"
    select_only([arm] + meshes, arm)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    for m in meshes:     # the children keep a parent-inverse from the old scale: fold it in
        mw = m.matrix_world.copy()
        m.parent = arm
        m.matrix_parent_inverse = arm.matrix_world.inverted()
        m.matrix_world = mw
    select_only(meshes, meshes[0])
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return arm, meshes


def normalise_height(arm, meshes, metres):
    """Some downloads come in odd units (Dreyar is 17 m, Medea 17 cm): scale every hero to its target height so the exported mesh is the real size."""
    k = metres / max(1e-6, model_height(meshes))
    if abs(k - 1.0) < 1e-3:
        return
    arm.scale = (k, k, k)
    select_only([arm] + meshes, arm)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    for m in meshes:
        select_only([m], m)
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def hips_height(arm):
    return (arm.matrix_world @ arm.data.bones[PREFIX + "Hips"].head_local).z


def model_height(meshes):
    zs = [(m.matrix_world @ mathutils.Vector(c)).z for m in meshes for c in m.bound_box]
    return max(zs) - min(zs)


# ------------------------------------------------------------------------------------------------------------------------------ 2. look edits

def scale_bones(arm, meshes, factors):
    """Pose-scales bones (the factors multiply down the chain) and makes that the new rest pose, so every clip plays on the new proportions."""
    if not factors:
        return
    for short, f in factors.items():
        pb = bone(arm, short)
        if pb is None:
            log("  no bone", short)
            continue
        pb.scale = (f, f, f)
    bpy.context.view_layer.update()
    for m in meshes:
        select_only([m], m)
        mod = next((x for x in m.modifiers if x.type == "ARMATURE"), None)
        if mod is None:
            continue
        name = mod.name
        bpy.ops.object.modifier_apply(modifier=name)
        new = m.modifiers.new(name, "ARMATURE")
        new.object = arm
    select_only([arm], arm)
    bpy.ops.object.mode_set(mode="POSE")
    bpy.ops.pose.armature_apply(selected=False)
    bpy.ops.object.mode_set(mode="OBJECT")


def base_colour_image(mat):
    if not mat or not mat.use_nodes:
        return None
    for n in mat.node_tree.nodes:
        if n.type == "BSDF_PRINCIPLED":
            links = n.inputs["Base Color"].links
            stack = [links[0].from_node] if links else []
            while stack:
                x = stack.pop()
                if x.type == "TEX_IMAGE" and x.image:
                    return x
                for i in x.inputs:
                    stack += [k.from_node for k in i.links]
    return None


def image_array(img):
    w, h = img.size
    return np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)


def colour_class(arr, name):
    """Texel masks by colour family, used by recolour swaps and glow masks. 'all' = everything."""
    rgb = arr[..., :3]
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx, mn = rgb.max(-1), rgb.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1e-4)
    if name == "red":
        return (r > 0.22) & (r > g * 1.7) & (r > b * 1.7)
    if name == "orange":
        return (r > 0.3) & (r > g * 1.25) & (g > b * 1.3) & (sat > 0.35)
    if name == "yellow":       # gold / yellow trim
        return (r > 0.3) & (g > 0.22) & (b < g * 0.7) & (np.abs(r - g) < 0.3 * r) & (sat > 0.3)
    if name == "green":
        return (g > 0.18) & (g > r * 1.2) & (g > b * 1.15)
    if name == "blue":
        return (b > 0.2) & (b > r * 1.3) & (b > g * 1.05)
    if name == "purple":
        return (b > 0.15) & (r > g * 1.2) & (b > g * 1.2)
    if name == "bright":
        return mx > 0.8
    if name == "saturated":
        return (sat > 0.45) & (mx > 0.2)
    if name == "all":
        return np.ones(r.shape, bool)
    return np.zeros(r.shape, bool)


def luminance(rgb):
    return (rgb * np.array([0.299, 0.587, 0.114], dtype=np.float32)).sum(-1, keepdims=True)


def recolour(arr, spec):
    """mode 'metal': desaturate towards a tinted grey (dark iron); 'tint': the luminance times a colour; 'none'. keep = how much of the original colour survives."""
    rgb = arr[..., :3]
    lum = luminance(rgb)
    keep = float(spec.get("keep", 0.3))
    tint = np.array(spec.get("tint", [1, 1, 1]), dtype=np.float32)
    mode = spec.get("mode", "tint")
    if mode == "metal":
        target = np.clip(lum * 1.35, 0, 1) ** 1.15 * (tint / max(1e-3, tint.mean())) * float(spec.get("value", 0.55))
    elif mode == "tint":
        target = lum * (tint / max(1e-3, luminance(tint[None])[0, 0])) * float(spec.get("value", 1.0))
    else:
        target = rgb
    arr[..., :3] = np.clip(target * (1 - keep) + rgb * keep, 0, 1)
    return arr


def swap_colours(arr, swaps):
    """[{"from": "red", "to": [r, g, b], "value": 1.0}]: texels of one colour family take a new hue, keeping their light and shade."""
    for sw in swaps:
        mask = colour_class(arr, sw["from"])
        if not mask.any():
            continue
        to = np.array(sw["to"], dtype=np.float32)
        lum = luminance(arr[mask, :3])
        arr[mask, :3] = np.clip(lum * (to / max(1e-3, luminance(to[None])[0, 0])) * float(sw.get("value", 1.0)), 0, 1)
        log("  swap %s: %d texels" % (sw["from"], int(mask.sum())))
    return arr


def glow_mask(arr, spec):
    return colour_class(arr, spec.get("from", "red"))


def process_textures(meshes, look, out_dir, stem):
    """Recolour + glow on the base-colour texture of every material; writes T_<stem>_albedo[_i].png / _emit[_i].png and points the Blender materials at them. Returns [(material, albedo, emit)]."""
    done = {}
    results = []
    for m in meshes:
        for slot in m.material_slots:
            mat = slot.material
            if mat is None or mat.name in done:
                continue
            node = base_colour_image(mat)
            if node is None:
                done[mat.name] = None
                continue
            img = node.image
            if max(img.size) > TEX_MAX:
                img.scale(*[max(1, int(v * TEX_MAX / max(img.size))) for v in img.size])
            arr = image_array(img)
            glow = look.get("glow")
            mask = glow_mask(arr, glow) if glow else None
            if look.get("recolour"):
                arr = recolour(arr, look["recolour"])
            if look.get("swaps"):
                arr = swap_colours(arr, look["swaps"])
            idx = len(results)
            suffix = "" if idx == 0 else "_%d" % idx
            emit = None
            if mask is not None and mask.any():
                col = np.array(glow["colour"], dtype=np.float32)
                arr[mask, :3] = col * 0.6 + 0.4 * arr[mask, :3]
                e = np.zeros_like(arr)
                e[..., 3] = 1
                e[mask, :3] = col
                emit = save_png(e, out_dir, "T_%s_emit%s" % (stem, suffix))
                wire_emission(mat, emit, float(glow.get("strength", 4.0)))
                log("  glow: %d texels" % int(mask.sum()))
            albedo = save_png(arr, out_dir, "T_%s_albedo%s" % (stem, suffix))
            node.image = albedo
            done[mat.name] = (albedo, emit)
            results.append((mat, albedo, emit))
    return results


def save_png(arr, out_dir, name):
    h, w = arr.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=True)
    img.pixels[:] = arr.ravel()
    img.filepath_raw = os.path.join(out_dir, name + ".png")
    img.file_format = "PNG"
    img.save()
    return img


def wire_emission(mat, img, strength):
    nt = mat.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    tex = nt.nodes.new("ShaderNodeTexImage")
    tex.image = img
    nt.links.new(tex.outputs["Color"], bsdf.inputs["Emission Color"])
    bsdf.inputs["Emission Strength"].default_value = strength


def make_material(name, colour, glow=None, metallic=0.6):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*colour, 1)
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = 0.45
    if glow:
        bsdf.inputs["Emission Color"].default_value = (*glow, 1)
        bsdf.inputs["Emission Strength"].default_value = 3.0
    return mat


# ---- props: simple hard-surface meshes built in the bone's rest frame and skinned 100% to that bone

def _box(bm, centre, size):
    """An axis-aligned box in the prop frame; returns its vertices."""
    r = bmesh.ops.create_cube(bm, size=1.0)
    for v in r["verts"]:
        v.co = mathutils.Vector((centre[0] + v.co.x * size[0], centre[1] + v.co.y * size[1], centre[2] + v.co.z * size[2]))
    return set(r["verts"])


def prop_tower_shield(bm, colour_glow):
    """Alesk's tower shield after the splash art: a tall dark-iron kite shield (about 1.15 m x 0.62 m), bowed, with a raised rim, a glowing rune diamond in the middle and
    rune lines running to the top and the point. Built in the bone frame (+Y along the forearm), outer face toward +Z. Returns the glowing vertices."""
    w, h, d = 0.31, 0.56, 0.03
    rows = [(-1.0, 0.55), (-0.75, 0.95), (0.6, 1.0), (1.0, 0.72)]   # (y, half-width factor): narrow bottom, square top
    outline = [(0.0, -1.18 * h)] + [(-w * f, y * h) for y, f in rows] + [(w * f, y * h) for y, f in reversed(rows)]

    def bow(x):
        return -0.09 * (x / w) ** 2     # the face curves back toward the edges

    def ring(scale, z):
        return [bm.verts.new((x * scale, y * scale, z + bow(x * scale))) for x, y in outline]
    outer_f, outer_b = ring(1.0, d), ring(1.0, -d)
    inner_f = ring(0.9, d - 0.012)                  # the field sits a little lower than the rim
    n = len(outline)
    for i in range(n):                              # rim top (between outer and inner outline), sides, back
        j = (i + 1) % n
        bm.faces.new((outer_f[i], outer_f[j], inner_f[j], inner_f[i]))
        bm.faces.new((outer_f[j], outer_f[i], outer_b[i], outer_b[j]))
    bm.faces.new(inner_f)
    bm.faces.new(list(reversed(outer_b)))
    glow = set()
    # the rune diamond: a flat pyramid on the field
    zc = d - 0.012
    tip = bm.verts.new((0.0, 0.02, zc + 0.07))
    corners = [bm.verts.new(c) for c in ((0.0, 0.23, zc), (0.13, 0.02, zc), (0.0, -0.19, zc), (-0.13, 0.02, zc))]
    for i in range(4):
        bm.faces.new((corners[i], corners[(i + 1) % 4], tip))
    glow |= set(corners) | {tip}
    # rune lines up to the top and down to the point, and a cross bar
    glow |= _box(bm, (0.0, 0.40, zc + 0.006), (0.022, 0.30, 0.012))
    glow |= _box(bm, (0.0, -0.38, zc + 0.006), (0.022, 0.30, 0.012))
    glow |= _box(bm, (0.0, 0.02, zc + 0.004), (0.44, 0.018, 0.010))
    return glow


# ---- the weapon / trinket library. Every builder works in a "prop frame": the grip (or the attachment point) at the origin, the long axis along +Y, the face / edge
#      toward +Z. It adds faces to the bmesh and returns {"glow": verts, "alt": verts}: glow = emissive material, alt = the second colour (wood, leather, bone).

def _cyl(bm, r0, r1, y0, y1, seg=10, x=0.0, z=0.0):
    """A cylinder / cone along +Y from y0 (radius r0) to y1 (radius r1)."""
    res = bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=r0, radius2=r1, depth=abs(y1 - y0))
    mid = (y0 + y1) / 2
    for v in res["verts"]:
        v.co = mathutils.Vector((v.co.x + x, v.co.z + mid, -v.co.y + z))
    return set(res["verts"])


def _sphere(bm, r, centre, sub=2, squash=(1, 1, 1)):
    res = bmesh.ops.create_icosphere(bm, subdivisions=sub, radius=r)
    for v in res["verts"]:
        v.co = mathutils.Vector((centre[0] + v.co.x * squash[0], centre[1] + v.co.y * squash[1], centre[2] + v.co.z * squash[2]))
    return set(res["verts"])


def _blade(bm, y0, y1, width, thick, tip):
    """A flat double-edged blade from y0 to y1 with a pointed tip of length tip. Returns (body verts, edge verts)."""
    w, t = width / 2, thick / 2
    ring = lambda y, k: [bm.verts.new((-w * k, y, 0)), bm.verts.new((0, y, t)), bm.verts.new((w * k, y, 0)), bm.verts.new((0, y, -t))]
    a, b = ring(y0, 1.0), ring(y1 - tip, 1.0)
    point = bm.verts.new((0, y1, 0))
    for i in range(4):
        j = (i + 1) % 4
        bm.faces.new((a[i], a[j], b[j], b[i]))
        bm.faces.new((b[i], b[j], point))
    bm.faces.new(list(reversed(a)))
    return set(a + b + [point]), {a[0], a[2], b[0], b[2], point}


def prop_sword(bm, spec, length=0.95, width=0.07):
    alt = _cyl(bm, 0.02, 0.02, -0.10, 0.12)                           # grip
    alt |= _sphere(bm, 0.03, (0, -0.12, 0), 1)                         # pommel
    body = _box(bm, (0, 0.13, 0), (width * 3.2, 0.03, 0.04))          # cross guard
    blade, edge = _blade(bm, 0.14, 0.14 + length, width, 0.016, length * 0.14)
    glow = set()
    if spec.get("glow"):
        glow = _box(bm, (0, 0.14 + length * 0.42, 0.0), (width * 0.18, length * 0.68, 0.02))   # a glowing fuller down the middle
    return {"glow": glow, "alt": alt}


def prop_greatsword(bm, spec):
    return prop_sword(bm, spec, length=1.25, width=0.13)


def prop_dagger(bm, spec):
    alt = _cyl(bm, 0.017, 0.017, -0.06, 0.07)
    _box(bm, (0, 0.075, 0), (0.12, 0.02, 0.03))
    _blade(bm, 0.08, 0.40, 0.05, 0.012, 0.09)
    glow = _box(bm, (0, 0.22, 0), (0.012, 0.22, 0.016)) if spec.get("glow") else set()
    return {"glow": glow, "alt": alt}


def prop_axe(bm, spec):
    alt = _cyl(bm, 0.025, 0.022, -0.35, 0.85)                          # haft
    head = []
    for sgn in (1, -1) if spec.get("double", True) else (1,):
        pts = [(0.03 * sgn, 0.55), (0.28 * sgn, 0.45), (0.33 * sgn, 0.70), (0.28 * sgn, 0.95), (0.03 * sgn, 0.85)]
        f = [bm.verts.new((x, y, 0.025)) for x, y in pts]
        bk = [bm.verts.new((x, y, -0.025)) for x, y in pts]
        bm.faces.new(f if sgn > 0 else list(reversed(f)))
        bm.faces.new(list(reversed(bk)) if sgn > 0 else bk)
        for i in range(len(pts)):
            j = (i + 1) % len(pts)
            bm.faces.new((f[i], f[j], bk[j], bk[i]) if sgn > 0 else (f[j], f[i], bk[i], bk[j]))
        head += f + bk
    glow = set()
    if spec.get("glow"):
        for sgn in (1, -1):
            glow |= _box(bm, (0.30 * sgn, 0.70, 0), (0.025, 0.40, 0.056))   # glowing cutting edges
    return {"glow": glow, "alt": alt}


def prop_staff(bm, spec):
    top = spec.get("top", "orb")
    alt = _cyl(bm, 0.022, 0.018, -0.75, 0.95)
    glow = set()
    if top == "orb":
        _cyl(bm, 0.03, 0.05, 0.93, 1.02)
        glow |= _sphere(bm, 0.085, (0, 1.10, 0))
    elif top == "crescent":
        for i in range(9):             # a crescent moon made of short segments, open side up
            a = math.radians(200 + i * 17.5)
            glow |= _box(bm, (math.cos(a) * 0.14, 1.10 + math.sin(a) * 0.14, 0), (0.035, 0.05, 0.03))
        glow |= _sphere(bm, 0.035, (0, 1.10, 0), 1)
    elif top == "skull":
        body = _sphere(bm, 0.085, (0, 1.06, 0), 2, (1.0, 1.1, 1.0))
        body |= _box(bm, (0, 0.98, 0.02), (0.09, 0.05, 0.07))
        glow |= _sphere(bm, 0.018, (-0.03, 1.07, 0.075), 1) | _sphere(bm, 0.018, (0.03, 1.07, 0.075), 1)
        return {"glow": glow, "alt": alt | body, "bone_white": True}
    elif top == "sun":
        glow |= _cyl(bm, 0.11, 0.11, 1.08, 1.11, 16)
        for i in range(12):
            a = math.radians(i * 30)
            c = (math.cos(a) * 0.17, 1.095 + 0.0, math.sin(a) * 0.17)
            glow |= _box(bm, (c[0], 1.095, c[2]), (0.03, 0.02, 0.03))
        glow |= _sphere(bm, 0.06, (0, 1.095, 0), 1)
    elif top == "flower":          # petals around a glowing heart (Fern, the Blossom)
        for i in range(8):
            a = math.radians(i * 45)
            _sphere(bm, 0.05, (math.cos(a) * 0.08, 1.10, math.sin(a) * 0.08), 1, (1.0, 0.35, 1.0))
        glow |= _sphere(bm, 0.045, (0, 1.12, 0), 1)
    elif top == "leaf":            # a sprouting branch: two leaves and a glowing bud (Willow)
        _sphere(bm, 0.07, (0.07, 1.04, 0), 1, (0.45, 1.0, 0.2))
        _sphere(bm, 0.07, (-0.07, 1.10, 0), 1, (0.45, 1.0, 0.2))
        glow |= _sphere(bm, 0.04, (0, 1.16, 0), 1)
    elif top == "star":
        glow |= _sphere(bm, 0.07, (0, 1.10, 0), 1)
        for dx, dy in ((0.15, 0), (-0.15, 0), (0, 0.15), (0, -0.15)):
            glow |= _box(bm, (dx / 2, 1.10 + dy / 2, 0), (abs(dx) + 0.02, abs(dy) + 0.02, 0.02))
    return {"glow": glow, "alt": alt}


def prop_trident(bm, spec):
    alt = _cyl(bm, 0.02, 0.018, -0.75, 0.95)
    _box(bm, (0, 0.95, 0), (0.30, 0.04, 0.035))
    glow = set()
    for x, h in ((-0.13, 0.25), (0.0, 0.34), (0.13, 0.25)):
        _cyl(bm, 0.016, 0.012, 0.95, 0.95 + h * 0.7, 6, x=x)
        glow |= _cyl(bm, 0.013, 0.001, 0.95 + h * 0.7, 0.95 + h, 6, x=x)   # glowing prong tips
    return {"glow": glow, "alt": alt}


def prop_bow(bm, spec):
    glow, alt = set(), set()
    n, half, bend = 14, 0.68, 0.16
    pts = []
    for i in range(n + 1):
        t = -1 + 2 * i / n
        pts.append((0.0, t * half, -bend * (1 - t * t)))          # limbs curve away from the string (toward -Z)
    for i in range(n):
        (x0, y0, z0), (x1, y1, z1) = pts[i], pts[i + 1]
        seg = _cyl(bm, 0.02 * (1.2 - abs(pts[i][1]) / half * 0.6), 0.02 * (1.2 - abs(pts[i + 1][1]) / half * 0.6), y0, y1, 6, z=(z0 + z1) / 2)
        alt |= seg
    glow |= _box(bm, (0, 0, 0.0), (0.006, half * 2, 0.006))           # the string
    return {"glow": glow if spec.get("glow") else set(), "alt": alt}


def prop_rifle(bm, spec):
    body = _box(bm, (0, 0.18, 0.03), (0.06, 0.45, 0.09))                # receiver
    alt = _box(bm, (0, -0.12, 0.0), (0.05, 0.28, 0.12))                 # stock
    body |= _cyl(bm, 0.018, 0.016, 0.40, 0.92, 8, z=0.05)               # barrel
    glow = _box(bm, (0, 0.30, 0.08), (0.02, 0.3, 0.02)) if spec.get("glow") else set()
    return {"glow": glow, "alt": alt}


def prop_club(bm, spec):
    alt = _cyl(bm, 0.03, 0.075, -0.12, 0.62, 10)
    glow = set()
    for i in range(6):
        a = math.radians(i * 60)
        _cyl(bm, 0.02, 0.0, 0.0, 0.08, 5, x=math.cos(a) * 0.07)
    return {"glow": glow, "alt": alt, "bone_white": True}


def prop_hammer(bm, spec):
    """A two-handed war hammer: a long haft and a heavy head with glowing faces (Kael)."""
    alt = _cyl(bm, 0.025, 0.022, -0.3, 0.95)
    _box(bm, (0, 0.95, 0), (0.34, 0.2, 0.2))
    glow = set()
    if spec.get("glow"):
        glow |= _box(bm, (0.175, 0.95, 0), (0.012, 0.16, 0.16)) | _box(bm, (-0.175, 0.95, 0), (0.012, 0.16, 0.16))
    return {"glow": glow, "alt": alt}


def prop_gauntlet(bm, spec):
    """A mech sleeve over the forearm (bone frame: +Y from elbow to wrist)."""
    body = _box(bm, (0, 0.14, 0), (0.17, 0.30, 0.17))
    body |= _box(bm, (0, 0.30, 0), (0.20, 0.06, 0.20))
    body |= _box(bm, (0.0, 0.02, 0), (0.19, 0.05, 0.19))
    glow = _box(bm, (0, 0.14, 0.088), (0.06, 0.24, 0.01)) | _box(bm, (0.088, 0.14, 0), (0.01, 0.24, 0.06)) | _box(bm, (-0.088, 0.14, 0), (0.01, 0.24, 0.06))
    return {"glow": glow, "alt": set()}


def prop_round_shield(bm, spec):
    body = _cyl(bm, 0.30, 0.30, -0.02, 0.02, 20)
    glow = _cyl(bm, 0.09, 0.05, 0.02, 0.08, 12) if spec.get("glow") else set()
    for v in body | glow:
        v.co = mathutils.Vector((v.co.x, v.co.z, v.co.y))    # face toward +Z
    return {"glow": glow, "alt": set()}


def prop_halo(bm, spec):
    """A ring floating over the head (Head bone frame: +Y up)."""
    glow = set()
    for i in range(16):
        a = math.radians(i * 22.5)
        glow |= _box(bm, (math.cos(a) * 0.16, 0.0, math.sin(a) * 0.16), (0.07, 0.018, 0.03))
    return {"glow": glow, "alt": set()}


def prop_crescent(bm, spec):
    """A crescent moon behind the head, open side up."""
    glow = set()
    for i in range(13):
        a = math.radians(195 + i * 12.5)
        r = 0.26
        glow |= _box(bm, (math.cos(a) * r, math.sin(a) * r, 0), (0.06, 0.06, 0.025 + 0.02 * math.sin(math.radians(i * 15))))
    return {"glow": glow, "alt": set()}


def prop_crown(bm, spec):
    body = _cyl(bm, 0.12, 0.12, 0.0, 0.045, 16)
    glow = set()
    for i in range(8):
        a = math.radians(i * 45)
        glow |= _cyl(bm, 0.022, 0.0, 0.045, 0.12, 5, x=math.cos(a) * 0.115, z=math.sin(a) * 0.115)
    return {"glow": glow, "alt": set()}


def prop_orb(bm, spec):
    glow = _sphere(bm, spec.get("radius", 0.09), (0, 0, 0), 2)
    return {"glow": glow, "alt": set()}


def prop_rocket(bm, spec):
    body = _cyl(bm, 0.07, 0.07, -0.25, 0.35, 12)
    glow = _cyl(bm, 0.055, 0.055, 0.35, 0.37, 12)
    return {"glow": glow, "alt": set()}


def _limb(bm, p0, p1, r0, r1, seg=6):
    """A tapered cylinder between two points (for antlers, branches, claws)."""
    a, b = mathutils.Vector(p0), mathutils.Vector(p1)
    d = b - a
    res = bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=seg, radius1=r0, radius2=r1, depth=d.length)
    rot = mathutils.Vector((0, 0, 1)).rotation_difference(d.normalized()).to_matrix()
    mid = (a + b) / 2
    for v in res["verts"]:
        v.co = rot @ v.co + mid
    return set(res["verts"])


def prop_antlers(bm, spec):
    """Branching bark antlers (Head frame, +Y up): two main beams curving outward with two tines each."""
    alt, glow = set(), set()
    k = spec.get("spread", 1.0)
    for s in (1, -1):
        p0, p1, p2 = (0.05 * s, 0.0, 0.0), (0.16 * s * k, 0.16, -0.02), (0.22 * s * k, 0.34, -0.06)
        alt |= _limb(bm, p0, p1, 0.028, 0.022) | _limb(bm, p1, p2, 0.022, 0.012)
        alt |= _limb(bm, p1, (0.10 * s * k, 0.30, 0.04), 0.016, 0.006)
        alt |= _limb(bm, ((p1[0] + p2[0]) / 2, 0.25, -0.04), (0.32 * s * k, 0.36, 0.0), 0.014, 0.005)
        if spec.get("glow"):
            glow |= _sphere(bm, 0.022, p2, 1)
    return {"glow": glow, "alt": alt}


def prop_leaf_crown(bm, spec):
    """A ring of leaves around the head (crown slot); the leaves are the main colour, small glowing buds between them."""
    glow = set()
    n = spec.get("leaves", 10)
    for i in range(n):
        a = math.radians(i * 360 / n)
        c = (math.cos(a) * 0.12, 0.03 + 0.03 * (i % 2), math.sin(a) * 0.12)
        _sphere(bm, 0.06, c, 1, (0.45, 1.0, 0.25))
        if spec.get("glow"):
            b = math.radians((i + 0.5) * 360 / n)
            glow |= _sphere(bm, 0.018, (math.cos(b) * 0.125, 0.0, math.sin(b) * 0.125), 1)
    return {"glow": glow, "alt": set()}


def prop_canopy(bm, spec):
    """A tree canopy: a cluster of leafy blobs (main colour) with glowing blossoms, above the head / shoulders."""
    r = spec.get("radius", 0.4)
    glow = set()
    blobs = [(0, 0.35, 0, 1.0), (0.28, 0.25, 0.05, 0.75), (-0.28, 0.25, 0.05, 0.75), (0.0, 0.3, -0.25, 0.8), (0.15, 0.55, -0.05, 0.7), (-0.15, 0.55, 0.05, 0.7), (0, 0.2, 0.22, 0.6)]
    for x, y, z, f in blobs:
        _sphere(bm, r * f, (x * r / 0.4, y * r / 0.4, z * r / 0.4), 2, (1.0, 0.8, 1.0))
    alt = _limb(bm, (0, -0.05, 0), (0, 0.25 * r / 0.4, 0), 0.05, 0.035)
    if spec.get("glow"):
        for i in range(9):
            a = math.radians(i * 40)
            glow |= _sphere(bm, 0.035, (math.cos(a) * r * 0.9, 0.35 * r / 0.4 + 0.12 * math.sin(i), math.sin(a) * r * 0.9), 1)
    return {"glow": glow, "alt": alt}


def prop_claws(bm, spec):
    """Three curved claws past the knuckles (hand slot frame: +Y along the fingers)."""
    glow = set()
    for x in (-0.03, 0.0, 0.03):
        glow |= _limb(bm, (x, 0.06, 0.0), (x * 1.3, 0.22, 0.03), 0.012, 0.002, 5)
    return {"glow": glow if spec.get("glow") else set(), "alt": glow if not spec.get("glow") else set(), "bone_white": not spec.get("glow")}


def prop_gatling(bm, spec):
    """A rotary gun: a drum receiver and six barrels in a ring (held like the rifle)."""
    body = _box(bm, (0, 0.10, 0.03), (0.09, 0.34, 0.12))
    alt = _box(bm, (0, -0.14, 0.0), (0.05, 0.22, 0.11))
    body |= _cyl(bm, 0.06, 0.06, 0.27, 0.33, 12, z=0.05)
    for i in range(6):
        a = math.radians(i * 60)
        body |= _cyl(bm, 0.012, 0.012, 0.30, 0.88, 6, x=math.cos(a) * 0.035, z=0.05 + math.sin(a) * 0.035)
    glow = _cyl(bm, 0.05, 0.05, 0.86, 0.9, 12, z=0.05) if spec.get("glow") else set()
    return {"glow": glow, "alt": alt}


def prop_pauldron(bm, spec):
    """A chunky shoulder guard (bark or plate) with three spikes / thorns."""
    body = _sphere(bm, 0.14, (0, 0, 0), 2, (1.0, 0.55, 1.0))
    glow, alt = set(), set()
    for dx in (-0.06, 0.0, 0.06):
        spike = _limb(bm, (dx, 0.04, 0.0), (dx * 1.4, 0.16, 0.02), 0.022, 0.003, 5)
        (glow if spec.get("glow") else alt).update(spike)
    return {"glow": glow, "alt": alt}


def prop_mech_pack(bm, spec):
    """A mech backpack: an armoured box with two exhaust stacks (glowing) and a rocket pod."""
    _box(bm, (0, 0.0, 0.0), (0.34, 0.42, 0.18))
    glow = set()
    for x in (-0.1, 0.1):
        _cyl(bm, 0.045, 0.04, 0.2, 0.36, 10, x=x, z=-0.02)
        glow |= _cyl(bm, 0.035, 0.035, 0.36, 0.38, 10, x=x, z=-0.02)
    alt = _box(bm, (0.24, 0.1, 0.0), (0.12, 0.2, 0.16))
    return {"glow": glow, "alt": alt}


def prop_tower_shield_lib(bm, spec):
    return {"glow": prop_tower_shield(bm, spec.get("glow")), "alt": set()}


PROPS = {"tower_shield": prop_tower_shield_lib, "hammer": prop_hammer, "sword": prop_sword, "greatsword": prop_greatsword, "dagger": prop_dagger, "axe": prop_axe, "staff": prop_staff,
         "trident": prop_trident, "bow": prop_bow, "rifle": prop_rifle, "club": prop_club, "gauntlet": prop_gauntlet, "round_shield": prop_round_shield, "halo": prop_halo,
         "crescent": prop_crescent, "crown": prop_crown, "orb": prop_orb, "rocket": prop_rocket,
         # trait system v2 (Nature, Rivet, Hexa)
         "antlers": prop_antlers, "leaf_crown": prop_leaf_crown, "canopy": prop_canopy, "claws": prop_claws, "gatling": prop_gatling, "pauldron": prop_pauldron,
         "mech_pack": prop_mech_pack}

# where a prop sits by default: bone, offset in the bone frame, rotation (degrees) that turns the prop frame's +Y into the right direction.
# Mixamo hands (checked on the rest pose): bone +Y runs along the fingers, +X points out of the front of the fist, -Z is the palm side. A held weapon runs through the fist,
# so its +Y has to become the hand's +X (right hand) or -X (left hand).
SLOTS = {
    "right_hand": {"bone": "RightHand", "offset": [0.0, 0.075, -0.025], "rotate": [0, 0, -90]},
    "left_hand":  {"bone": "LeftHand",  "offset": [0.0, 0.075, -0.025], "rotate": [0, 0, 90]},
    "right_arm":  {"bone": "RightForeArm", "offset": [0.0, 0.0, 0.0], "rotate": [0, 0, 0]},
    "left_arm":   {"bone": "LeftForeArm", "offset": [0.0, 0.14, -0.10], "rotate": [0, 180, 0]},
    "head":       {"bone": "Head", "offset": [0.0, 0.30, 0.0], "rotate": [0, 0, 0]},
    "behind_head": {"bone": "Head", "offset": [0.0, 0.14, -0.16], "rotate": [0, 0, 0]},
    "crown":      {"bone": "Head", "offset": [0.0, 0.17, 0.0], "rotate": [0, 0, 0]},
    "over_left_palm": {"bone": "LeftHand", "offset": [0.0, 0.10, -0.16], "rotate": [0, 0, 0]},
    "right_shoulder": {"bone": "RightShoulder", "offset": [0.0, 0.12, 0.14], "rotate": [-90, 0, 0]},
    "left_shoulder": {"bone": "LeftShoulder", "offset": [0.0, 0.12, 0.14], "rotate": [-90, 0, 0]},
    "back":       {"bone": "Spine2", "offset": [0.0, 0.05, -0.20], "rotate": [0, 0, 0]},
}


def add_prop(arm, meshes, spec, stem, idx):
    kind = spec["kind"]
    slot = dict(SLOTS.get(spec.get("slot", ""), {}))
    slot.update({k: spec[k] for k in ("bone", "offset", "rotate") if k in spec})
    me = bpy.data.meshes.new("%s_prop%d" % (stem, idx))
    bm = bmesh.new()
    parts = PROPS[kind](bm, spec)
    glow_verts, alt_verts = parts["glow"], parts["alt"]
    for f in bm.faces:
        vs = f.verts
        f.material_index = 1 if all(v in glow_verts for v in vs) else (2 if all(v in alt_verts for v in vs) else 0)
    k = float(spec.get("scale", 1.0))
    for v in bm.verts:
        v.co *= k
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(me.name, me)
    bpy.context.scene.collection.objects.link(obj)
    bone_white = [0.78, 0.74, 0.64]
    me.materials.append(make_material(me.name + "_M", spec.get("colour", bone_white if parts.get("bone_white") else [0.25, 0.25, 0.27]), metallic=spec.get("metallic", 0.7)))
    me.materials.append(make_material(me.name + "_G", [c * 0.5 for c in spec.get("glow", [1, 1, 1])], spec.get("glow")))
    me.materials.append(make_material(me.name + "_A", spec.get("colour2", bone_white if parts.get("bone_white") else [0.23, 0.14, 0.08]), metallic=0.0))
    b = arm.data.bones[PREFIX + slot["bone"]]
    frame = arm.matrix_world @ b.matrix_local
    off = mathutils.Matrix.Translation(slot.get("offset", [0, 0, 0]))
    rot = mathutils.Euler([math.radians(a) for a in slot.get("rotate", [0, 0, 0])]).to_matrix().to_4x4()
    obj.matrix_world = frame @ off @ rot
    select_only([obj], obj)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    # drop unused material slots (a prop without glow or a second colour)
    used = {p.material_index for p in me.polygons}
    for i in reversed(range(len(me.materials))):
        if i not in used:
            me.materials.pop(index=i)
            for p in me.polygons:
                if p.material_index > i:
                    p.material_index -= 1
    vg = obj.vertex_groups.new(name=b.name)
    vg.add(list(range(len(me.vertices))), 1.0, "REPLACE")
    obj.parent = arm
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm
    meshes.append(obj)
    log("  prop %s on %s" % (kind, b.name))


def decimate(meshes):
    tris = sum(sum(len(p.vertices) - 2 for p in m.data.polygons) for m in meshes)
    if tris <= TRI_BUDGET:
        return tris
    ratio = TRI_BUDGET / tris
    for m in meshes:
        if m.data.shape_keys:
            continue
        mod = m.modifiers.new("Decimate", "DECIMATE")
        mod.ratio = ratio
        mod.use_collapse_triangulate = True
        select_only([m], m)
        bpy.ops.object.modifier_move_to_index(modifier="Decimate", index=0)
        bpy.ops.object.modifier_apply(modifier="Decimate")
    after = sum(sum(len(p.vertices) - 2 for p in m.data.polygons) for m in meshes)
    log("  decimated %d -> %d triangles" % (tris, after))
    return after


# ------------------------------------------------------------------------------------------------------------------------------ 4. clips

_clip_cache = {}


def clip_data(key, spec):
    """Imports a clip once and returns its keys: {bone: {'rot': [(frames, w, x, y, z)], 'loc': ...}} in plain arrays, plus length and the clip's hip height (armature units)."""
    if key in _clip_cache:
        return _clip_cache[key]
    path = os.path.join(CLIPS, spec["file"])
    objs = import_fbx(path)
    arm = next(o for o in objs if o.type == "ARMATURE")
    normalise_bone_names(arm)
    act = arm.animation_data.action
    f0, f1 = [int(round(x)) for x in act.frame_range]
    fps = 30.0
    start = int(round(spec.get("start", 0.0) * fps)) + f0
    end = min(f1, int(round(spec["end"] * fps)) + f0) if "end" in spec else f1
    frames = np.arange(start, end + 1)
    curves = {}
    for fc in fcurves_of(act):
        path_ = fc.data_path
        if not path_.startswith('pose.bones["'):
            continue
        bname = path_.split('"')[1]
        prop = path_.rsplit(".", 1)[-1]
        if prop not in ("rotation_quaternion", "location"):
            continue
        if prop == "location" and not bname.endswith(":Hips"):
            continue
        curves.setdefault((bname, prop), {})[fc.array_index] = np.array([fc.evaluate(float(f)) for f in frames], dtype=np.float64)
    hip_h = arm.data.bones[PREFIX + "Hips"].head_local.length   # armature units (the clip armature is still in its FBX units)
    # automatic impact: the frame where a hand or foot moves fastest
    impact = spec.get("impact")
    if impact is None:
        impact = fastest_limb_time(arm, act, frames) if not spec.get("loop") else 0.0
    data = {"curves": curves, "frames": len(frames), "length": (len(frames) - 1) / fps, "hip_h": hip_h, "impact": float(impact),
            "loop": bool(spec.get("loop")), "hold": bool(spec.get("hold"))}
    for o in objs:
        bpy.data.objects.remove(o)
    bpy.data.actions.remove(act)
    _clip_cache[key] = data
    return data


def fcurves_of(act):
    if hasattr(act, "fcurves") and len(getattr(act, "fcurves", [])) > 0:
        return list(act.fcurves)
    out = []
    for layer in act.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                out += list(bag.fcurves)
    return out


def fastest_limb_time(arm, act, frames):
    arm.animation_data.action = act
    if hasattr(arm.animation_data, "action_slot") and act.slots:
        arm.animation_data.action_slot = act.slots[0]
    sc = bpy.context.scene
    names = [PREFIX + n for n in ("LeftHand", "RightHand", "LeftFoot", "RightFoot")]
    prev, best, best_f = None, -1.0, frames[0]
    for f in frames:
        sc.frame_set(int(f))
        cur = [arm.matrix_world @ arm.pose.bones[n].head for n in names if n in arm.pose.bones]
        if prev is not None:
            s = max((a - b).length for a, b in zip(cur, prev))
            if s > best:
                best, best_f = s, f
        prev = cur
    return (best_f - frames[0]) / 30.0


def apply_clip(arm, name, data):
    """A new action on the hero's armature with the clip's rotations and its in-place, rescaled hip translation."""
    act = bpy.data.actions.new(name)
    arm.animation_data_create()
    arm.animation_data.action = act
    slot = None
    if hasattr(act, "slots"):
        slot = act.slots.new(id_type="OBJECT", name=arm.name) if len(act.slots) == 0 else act.slots[0]
        arm.animation_data.action_slot = slot
    hero_h = arm.data.bones[PREFIX + "Hips"].head_local.length
    k = hero_h / max(1e-6, data["hip_h"])
    n = data["frames"]
    xs = np.arange(n, dtype=np.float64) + 1.0
    for (bname, prop), chans in data["curves"].items():
        if bname not in arm.pose.bones:
            continue
        for idx, values in chans.items():
            v = values.copy()
            if prop == "location":
                v = v * k
                if idx in (0, 2):      # hips local X / Z are horizontal (the bone's Y points up): remove the drift so the clip plays in place
                    v = v - np.linspace(v[0], v[-1], n)
            fc = act.fcurve_ensure_for_datablock(arm, 'pose.bones["%s"].%s' % (bname, prop), index=idx)
            fc.keyframe_points.add(n)
            co = np.empty(2 * n)
            co[0::2] = xs
            co[1::2] = v
            fc.keyframe_points.foreach_set("co", co)
            fc.update()
    act.use_fake_user = True
    act.frame_range = (1, n)
    act.use_frame_range = True
    return act


# ------------------------------------------------------------------------------------------------------------------------------ 5. export + preview

def export(arm, meshes, path):
    select_only([arm] + meshes, arm)
    bpy.ops.export_scene.fbx(filepath=path, use_selection=True, object_types={"ARMATURE", "MESH"}, add_leaf_bones=False,
                             bake_anim=True, bake_anim_use_all_actions=True, bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True,
                             bake_anim_simplify_factor=0.0, bake_anim_step=1.0, path_mode="COPY", embed_textures=True, mesh_smooth_type="FACE",
                             apply_unit_scale=True, apply_scale_options="FBX_SCALE_NONE", primary_bone_axis="Y", secondary_bone_axis="X",
                             armature_nodetype="NULL", use_armature_deform_only=False)


def preview(arm, meshes, actions, out_png, stem):
    """Contact strip: idle, attack at its impact, cast at its impact, T-pose-free rest. Workbench, textured."""
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_WORKBENCH"
    sh = sc.display.shading
    sh.light = "STUDIO"
    sh.color_type = "TEXTURE"
    for m in meshes:
        for slot in m.material_slots:
            node = base_colour_image(slot.material)
            if node:
                slot.material.node_tree.nodes.active = node
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    h = model_height(meshes)
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = h * 1.35
    W = 360
    sc.render.resolution_x = W
    sc.render.resolution_y = int(W * 1.2)
    tiles = []
    for key, t in actions:
        act = bpy.data.actions[key]
        arm.animation_data.action = act
        if hasattr(arm.animation_data, "action_slot") and act.slots:
            arm.animation_data.action_slot = act.slots[0]
        sc.frame_set(1 + int(round(t * 30)))
        for ang in (-35,):
            a = math.radians(ang)
            cam.location = (math.sin(a) * h * 4, -math.cos(a) * h * 4, h * 0.5)
            cam.rotation_euler = (math.radians(90), 0, a)
            p = out_png + "_tmp.png"
            sc.render.filepath = p
            bpy.ops.render.render(write_still=True)
            img = bpy.data.images.load(p)
            tiles.append(image_array(img))
            bpy.data.images.remove(img)
            os.remove(p)
    strip = np.concatenate(tiles, axis=1)
    save_png(strip, os.path.dirname(out_png), os.path.splitext(os.path.basename(out_png))[0])


def portrait(arm, meshes, actions, idle, out_png):
    """A head-and-shoulders portrait (256 x 256, transparent background, EEVEE) for the shop cards and the info panel: T_Portrait_<id>.png."""
    sc = bpy.context.scene
    act = bpy.data.actions[idle]
    arm.animation_data.action = act
    if hasattr(arm.animation_data, "action_slot") and act.slots:
        arm.animation_data.action_slot = act.slots[0]
    sc.frame_set(12)
    head = arm.matrix_world @ arm.pose.bones[PREFIX + "Head"].head
    neck = arm.matrix_world @ arm.pose.bones[PREFIX + "Neck"].head if (PREFIX + "Neck") in arm.pose.bones else head
    h = model_height(meshes)
    target = (head + neck) / 2 + mathutils.Vector((0, 0, h * 0.02))
    for eng in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE", "BLENDER_WORKBENCH"):
        try:
            sc.render.engine = eng
            break
        except TypeError:
            continue
    if sc.render.engine == "BLENDER_WORKBENCH":
        sc.display.shading.light = "STUDIO"
        sc.display.shading.color_type = "TEXTURE"
    else:
        world = sc.world or bpy.data.worlds.new("w")
        sc.world = world
        world.use_nodes = True
        bg = world.node_tree.nodes.get("Background")
        if bg:
            bg.inputs[0].default_value = (0.35, 0.37, 0.42, 1)
            bg.inputs[1].default_value = 0.35
        for name, loc, energy in (("key", (-1.5, -2.5, 2.5), 90.0), ("rim", (1.8, 1.5, 2.0), 80.0)):
            ld = bpy.data.lights.new(name, "AREA")
            ld.energy = energy * h
            ld.size = 1.5
            lo = bpy.data.objects.new(name, ld)
            sc.collection.objects.link(lo)
            lo.location = target + mathutils.Vector(loc) * h * 0.6
            lo.rotation_euler = (target - lo.location).to_track_quat("-Z", "Y").to_euler()
    cam = bpy.data.objects.get("pcam") or bpy.data.objects.new("pcam", bpy.data.cameras.new("pcam"))
    if cam.name not in sc.collection.objects:
        sc.collection.objects.link(cam)
    sc.camera = cam
    cam.data.type = "PERSP"
    cam.data.lens = 70
    d = h * 0.95
    a = math.radians(-20)
    cam.location = target + mathutils.Vector((math.sin(a) * d, -math.cos(a) * d, h * 0.03))
    cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
    sc.render.resolution_x = 256
    sc.render.resolution_y = 256
    sc.render.film_transparent = True
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    sc.render.filepath = out_png
    bpy.ops.render.render(write_still=True)


# ------------------------------------------------------------------------------------------------------------------------------ driver

def build(hid, hero, cfg, out_root, want_preview):
    stem = "%s_%s" % (hid, hero["name"])
    out_dir = os.path.join(out_root, stem)
    os.makedirs(out_dir, exist_ok=True)
    reset()
    log("%s: %s" % (stem, hero["model"]))
    arm, meshes = load_character(os.path.join(MODELS, hero["model"]))
    normalise_height(arm, meshes, float(hero.get("height", 1.8)))
    look = hero.get("look", {})
    scale_bones(arm, meshes, look.get("scale_bones"))
    process_textures(meshes, look, out_dir, stem)
    for i, p in enumerate(look.get("props", [])):
        add_prop(arm, meshes, p, stem, i)
    tris = decimate(meshes)
    height = model_height(meshes)
    roles = dict(cfg["sets"][hero["set"]])
    roles["attack"] = hero["attack"]
    roles["cast"] = hero["cast"]
    for r in ("idle", "run", "hit", "death", "victory"):
        roles[r] = hero.get(r, roles[r])
    anims = {}
    made = {}
    for role, clip in roles.items():
        data = clip_data(clip, cfg["clips"][clip])
        name = "A_" + clip
        if name not in made:
            made[name] = apply_clip(arm, name, data)
        anims[role] = {"clip": name, "length": round(data["length"], 4), "impact": round(data["impact"], 4), "loop": data["loop"], "hold": data["hold"]}
    arm.animation_data.action = made["A_" + roles["idle"]]
    if hasattr(arm.animation_data, "action_slot"):
        arm.animation_data.action_slot = arm.animation_data.action.slots[0]
    fbx = os.path.join(out_dir, "SKM_%s.fbx" % stem)
    export(arm, meshes, fbx)
    try:
        portrait(arm, meshes, made, "A_" + roles["idle"], os.path.join(out_dir, "T_Portrait_%s.png" % hid))
    except Exception as ex:   # a portrait is a nicety: never lose the build over it
        log("  portrait failed: %s" % ex)
    if want_preview:
        preview(arm, meshes, [("A_" + roles["idle"], 0.5), ("A_" + roles["attack"], anims["attack"]["impact"]), ("A_" + roles["cast"], anims["cast"]["impact"]),
                              ("A_" + roles["run"], 0.2), ("A_" + roles["death"], anims["death"]["length"])], os.path.join(out_dir, "preview_%s.png" % stem), stem)
    log("  %d triangles, %.2f m tall, hips %.2f m, %d clips -> %s" % (tris, height, hips_height(arm), len(made), fbx))
    return {"name": hero["name"], "mesh": "SKM_%s" % stem, "folder": stem, "height": hero.get("height", height), "modelHeight": round(height, 4), "anims": anims,
            "materials": material_manifest(meshes)}


def material_manifest(meshes):
    """What UE needs to rebuild each material slot: the texture files (albedo / emissive) or a flat colour (props)."""
    out, seen = [], set()
    for m in meshes:
        for slot in m.material_slots:
            mat = slot.material
            if mat is None or mat.name in seen:
                continue
            seen.add(mat.name)
            entry = {"slot": mat.name}
            node = base_colour_image(mat)
            bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
            if node is not None:
                entry["albedo"] = node.image.name
                em = bsdf.inputs["Emission Color"].links if bsdf else []
                if em and em[0].from_node.type == "TEX_IMAGE" and em[0].from_node.image.name.startswith("T_"):   # only our own glow masks (some downloads carry an emissive map we do not export)
                    entry["emit"] = em[0].from_node.image.name
                    entry["emitBoost"] = round(bsdf.inputs["Emission Strength"].default_value, 3)
            elif bsdf is not None:
                entry["colour"] = [round(v, 4) for v in bsdf.inputs["Base Color"].default_value[:3]]
                if bsdf.inputs["Emission Strength"].default_value > 0:
                    entry["glow"] = [round(v, 4) for v in bsdf.inputs["Emission Color"].default_value[:3]]
                entry["metallic"] = round(bsdf.inputs["Metallic"].default_value, 3)
                entry["roughness"] = round(bsdf.inputs["Roughness"].default_value, 3)
            out.append(entry)
    return out


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    only, out_root, want_preview = None, DEFAULT_OUT, False
    i = 0
    while i < len(argv):
        if argv[i] == "--only":
            only = set(argv[i + 1].split(",")); i += 2
        elif argv[i] == "--out":
            out_root = argv[i + 1]; i += 2
        elif argv[i] == "--preview":
            want_preview = True; i += 1
        else:
            i += 1
    with open(os.path.join(HERE, "config.json")) as f:
        cfg = json.load(f)
    os.makedirs(out_root, exist_ok=True)
    index_path = os.path.join(out_root, "unit_visuals.json")
    for hid, hero in cfg["heroes"].items():
        if hid.startswith("_") or (only and hid not in only):
            continue
        entry = build(hid, hero, cfg, out_root, want_preview)
        with open(os.path.join(out_root, entry["folder"], "visual.json"), "w") as f:
            json.dump({hid: entry}, f, indent=1, sort_keys=True)
    # the index = every hero built so far (each writes its own visual.json, so several builds can run in parallel)
    index = {"version": 1, "units": {}}
    for name in sorted(os.listdir(out_root)):
        one = os.path.join(out_root, name, "visual.json")
        if os.path.exists(one):
            with open(one) as f:
                index["units"].update(json.load(f))
    with open(index_path, "w") as f:
        json.dump(index, f, indent=1, sort_keys=True)
    log("wrote", index_path, "(%d heroes)" % len(index["units"]))


if __name__ == "__main__":
    main()
