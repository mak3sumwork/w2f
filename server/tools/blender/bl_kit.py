"""Shared Blender (bpy) toolkit for the crafted W2F models: procedural materials (worn metal, bark, scales, leather, cloth, skin, emissive glows), beveled / subdivided primitives, tapered curves
(limbs, tails, hair, branches), cloth ribbons, and `finalize()`, which joins the parts, unwraps, BAKES albedo + ambient occlusion + emission into two textures on one material and exports a glTF.

Blender Z up; models face -Y (the glTF exporter turns that into +Z, the facing of every W2F model), feet at z = 0, metres. A hero script does:

    from bl_kit import *
    ...build the model with box() / sphere() / limb() / swoop() / ribbon() ...
    finalize("Baira", "SM_Champion_9002_Baira.glb")

Run it with a Python that has the `bpy` module:   ~/w2f_bpy/venv/bin/python tools/blender/<hero>.py [--preview DIR] [--no-bake]
"""
import bpy, bmesh, math, sys, os
from mathutils import Vector, Euler

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "models"))
PREVIEW = ARGS[ARGS.index("--preview") + 1] if "--preview" in ARGS else None
BAKE = "--no-bake" not in ARGS
TEX = 2048

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
parts = []
LOD = "high"          # "high": the detailed source model (bevels, subdivision, micro parts, runes); "low": the game model (silhouette shapes only, a few hundred polygons per part)


def is_low():
    return LOD == "low"


def set_lod(level):
    global LOD
    LOD = level


def low_seg(n, low=None, factor=0.42, minimum=8):
    """Segment count for the current LOD: the given count for the high model, a much smaller one (or `low`) for the game model."""
    if LOD == "high": return n
    return low if low is not None else max(minimum, int(round(n * factor)))


# ------------------------------------------------------------------------------------------------------------------------------ materials
def _mix(nt, a, b, fac, blend="MIX"):
    n = nt.nodes.new("ShaderNodeMix"); n.data_type = "RGBA"; n.blend_type = blend
    if isinstance(fac, (int, float)): n.inputs["Factor"].default_value = fac
    else: nt.links.new(fac, n.inputs["Factor"])
    nt.links.new(a, n.inputs["A"]); nt.links.new(b, n.inputs["B"])
    return n.outputs["Result"]


def _rgb(nt, c):
    n = nt.nodes.new("ShaderNodeRGB"); n.outputs[0].default_value = (*c, 1.0); return n.outputs[0]


def _ramp(nt, fac, stops):
    r = nt.nodes.new("ShaderNodeValToRGB")
    while len(r.color_ramp.elements) < len(stops): r.color_ramp.elements.new(0.5)
    for e, (pos, col) in zip(r.color_ramp.elements, stops): e.position = pos; e.color = (*col, 1)
    nt.links.new(fac, r.inputs["Fac"])
    return r.outputs["Color"]


def _noise(nt, scale, detail=6.0, rough=0.6, coord=None):
    n = nt.nodes.new("ShaderNodeTexNoise"); n.inputs["Scale"].default_value = scale; n.inputs["Detail"].default_value = detail; n.inputs["Roughness"].default_value = rough
    if coord is not None: nt.links.new(coord, n.inputs["Vector"])
    return n.outputs["Fac"]


def material(name, color, metallic=0.2, rough=0.55, emit=None, strength=1.0, kind="plain", color2=None, wear=0.5):
    """kind: plain | metal (worn: mottled, grimy crevices, scuffed bright edges, rust) | bark | scales (voronoi, colour gradient bottom -> top) | leather | cloth | skin."""
    m = bpy.data.materials.new(name); m.use_nodes = True
    nt = m.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Metallic"].default_value = metallic; bsdf.inputs["Roughness"].default_value = rough
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    col = _rgb(nt, color)
    col2 = _rgb(nt, color2 if color2 else tuple(min(1.0, c * 1.6 + 0.03) for c in color))
    coord = nt.nodes.new("ShaderNodeTexCoord")
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    if kind == "metal":
        mottle = _ramp(nt, _noise(nt, 5.0, 10.0, 0.65), [(0.35, (0.55, 0.55, 0.6)), (0.7, (1.15, 1.1, 1.05))])
        base = _mix(nt, col, mottle, 1.0, "MULTIPLY")
        rust = _rgb(nt, (0.34, 0.17, 0.09))
        rmask = nt.nodes.new("ShaderNodeMath"); rmask.operation = "MULTIPLY"; rmask.inputs[1].default_value = wear
        inv = nt.nodes.new("ShaderNodeMath"); inv.operation = "SUBTRACT"; inv.inputs[0].default_value = 1.0
        nt.links.new(_noise(nt, 60.0, 4.0), inv.inputs[1]); nt.links.new(inv.outputs[0], rmask.inputs[0])
        base = _mix(nt, base, rust, rmask.outputs[0])
        edge = _ramp(nt, geo.outputs["Pointiness"], [(0.5, (0, 0, 0)), (0.62, (1, 1, 1))])
        colour_out = _mix(nt, base, edge, 0.16, "ADD")
    elif kind == "bark":
        ridges = _noise(nt, 9.0, 12.0, 0.7, coord.outputs["Object"])
        base = _mix(nt, col, col2, _ramp(nt, ridges, [(0.3, (0, 0, 0)), (0.65, (1, 1, 1))]))
        moss = _rgb(nt, (0.18, 0.3, 0.1))
        base = _mix(nt, base, moss, _ramp(nt, _noise(nt, 3.0, 5.0, 0.5, coord.outputs["Object"]), [(0.55, (0, 0, 0)), (0.75, (0.7, 0.7, 0.7))]))
        edge = _ramp(nt, geo.outputs["Pointiness"], [(0.5, (0, 0, 0)), (0.65, (1, 1, 1))])
        colour_out = _mix(nt, base, edge, 0.1, "ADD")
    elif kind == "scales":
        vor = nt.nodes.new("ShaderNodeTexVoronoi"); vor.feature = "F1"; vor.inputs["Scale"].default_value = 22.0
        nt.links.new(coord.outputs["Object"], vor.inputs["Vector"])
        sep = nt.nodes.new("ShaderNodeSeparateXYZ"); nt.links.new(coord.outputs["Object"], sep.inputs["Vector"])
        grad = nt.nodes.new("ShaderNodeMapRange"); grad.inputs["From Min"].default_value = 0.0; grad.inputs["From Max"].default_value = 1.4
        nt.links.new(sep.outputs["Z"], grad.inputs["Value"])
        base = _mix(nt, col, col2, grad.outputs["Result"])
        cell = _ramp(nt, vor.outputs["Distance"], [(0.0, (0.55, 0.55, 0.55)), (0.55, (1.0, 1.0, 1.0)), (0.62, (0.25, 0.25, 0.25))])
        colour_out = _mix(nt, base, cell, 1.0, "MULTIPLY")
    elif kind == "leather":
        creases = _ramp(nt, _noise(nt, 28.0, 8.0, 0.55), [(0.35, (0.6, 0.6, 0.6)), (0.65, (1.05, 1.05, 1.05))])
        base = _mix(nt, col, creases, 1.0, "MULTIPLY")
        edge = _ramp(nt, geo.outputs["Pointiness"], [(0.5, (0, 0, 0)), (0.66, (1, 1, 1))])
        colour_out = _mix(nt, base, edge, 0.1, "ADD")
    elif kind == "cloth":
        weave = _ramp(nt, _noise(nt, 90.0, 2.0, 0.5), [(0.4, (0.75, 0.75, 0.75)), (0.6, (1.05, 1.05, 1.05))])
        folds = _ramp(nt, geo.outputs["Pointiness"], [(0.42, (0.7, 0.7, 0.7)), (0.55, (1.1, 1.1, 1.1))])
        colour_out = _mix(nt, _mix(nt, col, weave, 1.0, "MULTIPLY"), folds, 1.0, "MULTIPLY")
    elif kind == "skin":
        blush = _ramp(nt, _noise(nt, 6.0, 4.0, 0.5), [(0.4, (0.9, 0.9, 0.95)), (0.7, (1.08, 1.02, 1.0))])
        colour_out = _mix(nt, col, blush, 1.0, "MULTIPLY")
    else:
        colour_out = col
    nt.links.new(colour_out, bsdf.inputs["Base Color"])
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*emit, 1.0); bsdf.inputs["Emission Strength"].default_value = strength
    return m


def glow(name, rgb):
    """An emissive material: the colour is what gets baked into the emission texture (strength 1: the game boosts it)."""
    return material(name, rgb, 0.0, 0.3, emit=rgb, strength=1.0)


# ------------------------------------------------------------------------------------------------------------------------------ primitives
def finish(obj, mat, bevel=0.0, subsurf=0, smooth=True, segments=3, displace=None, keep=False):
    if LOD == "low":   # the game model: no micro bevels, no subdivision unless the part is silhouette-critical (`keep`), and then only one level
        bevel = bevel if (bevel >= 0.03 or keep) else 0.0
        segments = 1
        subsurf = min(subsurf, 1) if keep else 0
        displace = None
    obj.data.materials.append(mat)
    if obj.name not in scene.collection.objects: scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    for o in bpy.context.selected_objects: o.select_set(False)
    obj.select_set(True)
    if bevel > 0:
        mod = obj.modifiers.new("Bevel", "BEVEL"); mod.width = bevel; mod.segments = segments; mod.limit_method = "ANGLE"; mod.angle_limit = math.radians(35)
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if subsurf:
        mod = obj.modifiers.new("Sub", "SUBSURF"); mod.levels = subsurf; mod.render_levels = subsurf
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if displace:   # (strength, texture scale): a noisy relief for bark, rock, cloth ...
        tex = bpy.data.textures.new("d", "CLOUDS"); tex.noise_scale = displace[1]
        mod = obj.modifiers.new("Disp", "DISPLACE"); mod.texture = tex; mod.strength = displace[0]; mod.mid_level = 0.5
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if smooth: bpy.ops.object.shade_smooth()
    parts.append(obj)
    return obj


def make(name, bm):
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
    return bpy.data.objects.new(name, me)


def _euler(rot): return Euler([math.radians(a) for a in rot])


def box(name, loc, size, mat, rot=(0, 0, 0), bevel=0.025, subsurf=1, taper=None, displace=None, micro=False, keep=False):
    if LOD == "low" and micro: return None
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    if taper:
        for v in bm.verts:
            if v.co.z > 0: v.co.x *= taper; v.co.y *= taper
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    o = make(name, bm); o.location = loc; o.rotation_euler = _euler(rot)
    return finish(o, mat, bevel, subsurf, displace=displace, keep=keep)


def sphere(name, loc, size, mat, rot=(0, 0, 0), seg=28, rings=16, subsurf=0, cut=None, displace=None, micro=False, low=None):
    if LOD == "low" and micro: return None
    seg, rings = (seg, rings) if LOD == "high" else (low if low else (max(8, int(round(seg * 0.4))), max(6, int(round(rings * 0.45)))))
    bm = bmesh.new(); bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=rings, radius=1.0)
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    if cut is not None:
        geom = [f for f in bm.faces if all(v.co.z < cut * size[2] - 1e-6 for v in f.verts)]
        bmesh.ops.delete(bm, geom=geom, context="FACES")
    o = make(name, bm); o.location = loc; o.rotation_euler = _euler(rot)
    return finish(o, mat, 0.0, subsurf, displace=displace)


def limb(name, p0, p1, r0, r1, mat, seg=24, bevel=0.0, subsurf=0, caps=True, displace=None, micro=False, low=None):
    if LOD == "low" and micro: return None
    seg = low_seg(seg, low)
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=caps, cap_tris=False, segments=seg, radius1=r0, radius2=r1, depth=d.length)
    o = make(name, bm); o.location = (p0 + p1) / 2
    o.rotation_mode = "QUATERNION"; o.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(d.normalized())
    return finish(o, mat, bevel, subsurf, displace=displace)


def loft(name, sections, mat, seg=28, low=14, subsurf=1, caps=True):
    """A smooth body shape from elliptical cross-sections: `sections` = [(z, half_width_x, half_depth_y, centre_x, centre_y), ...] bottom to top (the last two are optional). Torsos, necks, hips: waist,
    bust and hip curves come from the section sizes. The game model uses `low` segments round, the source `seg` and one subdivision."""
    n = seg if LOD == "high" else low
    bm = bmesh.new(); rings = []
    for sec in sections:
        z, rx, ry = sec[0], sec[1], sec[2]
        cx = sec[3] if len(sec) > 3 else 0.0; cy = sec[4] if len(sec) > 4 else 0.0
        rings.append([bm.verts.new((cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n), z)) for i in range(n)])
    for a, b in zip(rings[:-1], rings[1:]):
        for i in range(n): bm.faces.new((a[i], a[(i + 1) % n], b[(i + 1) % n], b[i]))
    if caps:
        bm.faces.new(rings[0][::-1]); bm.faces.new(rings[-1])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    o = make(name, bm)
    return finish(o, mat, 0.0, subsurf if LOD == "high" else 0)


def ring(name, loc, radius, tube_r, mat, rot=(0, 0, 0), scale=(1, 1, 1), seg=32, tseg=10, micro=False):
    if LOD == "low" and micro: return None
    if LOD == "low": seg, tseg = max(10, int(round(seg * 0.4))), 4
    bpy.ops.mesh.primitive_torus_add(major_radius=radius, minor_radius=tube_r, major_segments=seg, minor_segments=tseg, location=loc, rotation=[math.radians(a) for a in rot])
    o = bpy.context.active_object; o.name = name; o.scale = scale
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat); bpy.ops.object.shade_smooth(); parts.append(o)
    return o


def tube(name, points, radius, mat, res=6):
    """A constant-thickness line along a polyline: runes and cracks. Not in the game model: the emission texture is baked from the high model."""
    if LOD == "low": return None
    cu = bpy.data.curves.new(name, "CURVE"); cu.dimensions = "3D"; cu.bevel_depth = radius; cu.bevel_resolution = res; cu.resolution_u = 6
    sp = cu.splines.new("POLY"); sp.points.add(len(points) - 1)
    for i, p in enumerate(points): sp.points[i].co = (p[0], p[1], p[2], 1)
    o = bpy.data.objects.new(name, cu); scene.collection.objects.link(o)
    bpy.context.view_layer.objects.active = o; o.select_set(True)
    bpy.ops.object.convert(target="MESH")
    o = bpy.context.active_object; o.data.materials.append(mat); parts.append(o)
    return o


def swoop(name, points, radii, mat, res=8, cyclic=False, displace=None, micro=False, low_u=5, low_res=3, low_scale=1.0):
    """`low_u` = curve subdivisions per span, `low_res` = bevel resolution and `low_scale` = a radius multiplier in the GAME model (thin strands become chunky shapes; the bake keeps the fine look)."""
    if LOD == "low" and micro: return None
    res = res if LOD == "high" else low_res
    if LOD == "low": radii = [r * low_scale for r in radii]
    """A smooth tube through `points` whose thickness follows `radii` (one per point): tails, hair, branches, tentacles, tapered blades."""
    cu = bpy.data.curves.new(name, "CURVE"); cu.dimensions = "3D"; cu.bevel_depth = 1.0; cu.bevel_resolution = res; cu.resolution_u = 14 if LOD == "high" else low_u; cu.use_fill_caps = True
    sp = cu.splines.new("NURBS"); sp.points.add(len(points) - 1)
    for i, (p, r) in enumerate(zip(points, radii)):
        sp.points[i].co = (p[0], p[1], p[2], 1); sp.points[i].radius = r
    sp.order_u = 4; sp.use_endpoint_u = True; sp.use_cyclic_u = cyclic
    o = bpy.data.objects.new(name, cu); scene.collection.objects.link(o)
    bpy.context.view_layer.objects.active = o; o.select_set(True)
    bpy.ops.object.convert(target="MESH")
    o = bpy.context.active_object; o.data.materials.append(mat)
    if displace:
        tex = bpy.data.textures.new("d", "CLOUDS"); tex.noise_scale = displace[1]
        mod = o.modifiers.new("Disp", "DISPLACE"); mod.texture = tex; mod.strength = displace[0]; mod.mid_level = 0.5
        bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.object.shade_smooth(); parts.append(o)
    return o


def ribbon(name, spine, widths, mat, up=(0, 1, 0), thick=0.012, wave=0.0, subsurf=1, micro=False):
    if LOD == "low" and micro: return None
    subsurf = subsurf if LOD == "high" else 0
    """A cloth / fin / blade strip along `spine` (points), `widths` wide at each point, facing `up`-ish; `wave` ripples the edges. Solidified so it has a thickness."""
    bm = bmesh.new()
    rows = []
    n = len(spine)
    for i, p in enumerate(spine):
        p = Vector(p)
        t = (Vector(spine[min(i + 1, n - 1)]) - Vector(spine[max(i - 1, 0)])).normalized()
        side = t.cross(Vector(up)).normalized()
        w = widths[i]
        ripple = wave * math.sin(i * 1.7) if wave else 0.0
        rows.append((bm.verts.new(p - side * w / 2 + Vector(up) * ripple), bm.verts.new(p + side * w / 2 - Vector(up) * ripple)))
    for i in range(n - 1):
        bm.faces.new((rows[i][0], rows[i][1], rows[i + 1][1], rows[i + 1][0]))
    o = make(name, bm)
    scene.collection.objects.link(o)
    bpy.context.view_layer.objects.active = o; o.select_set(True)
    o.data.materials.append(mat)
    mod = o.modifiers.new("Solid", "SOLIDIFY"); mod.thickness = thick; bpy.ops.object.modifier_apply(modifier=mod.name)
    if subsurf:
        mod = o.modifiers.new("Sub", "SUBSURF"); mod.levels = subsurf; bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.object.shade_smooth(); parts.append(o)
    return o


# ------------------------------------------------------------------------------------------------------------------------------ stage, previews
def stage():
    sc = scene
    sc.render.engine = "CYCLES"; sc.cycles.device = "CPU"
    world = bpy.data.worlds.new("W"); world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.5, 0.58, 0.68, 1); world.node_tree.nodes["Background"].inputs[1].default_value = 0.7
    sc.world = world
    key = bpy.data.lights.new("key", "SUN"); key.energy = 3.2; ko = bpy.data.objects.new("key", key); sc.collection.objects.link(ko); ko.rotation_euler = (math.radians(50), 0, math.radians(-35))
    fill = bpy.data.lights.new("fill", "SUN"); fill.energy = 1.1; fo = bpy.data.objects.new("fill", fill); sc.collection.objects.link(fo); fo.rotation_euler = (math.radians(60), 0, math.radians(140))


def render(path, cam_loc, target, size=(640, 800), samples=24, lens=60):
    cam = bpy.data.cameras.new("cam"); cam.lens = lens
    co = bpy.data.objects.new("cam", cam); scene.collection.objects.link(co); scene.camera = co
    co.location = cam_loc
    d = Vector(target) - Vector(cam_loc)
    co.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    scene.render.resolution_x, scene.render.resolution_y = size
    scene.cycles.samples = samples; scene.render.filepath = path
    scene.render.image_settings.file_format = "PNG"
    bpy.ops.render.render(write_still=True)
    bpy.data.objects.remove(co)


# ------------------------------------------------------------------------------------------------------------------------------ join, unwrap, preview, bake, export
def finalize(name, out_file, center_x=0.0, views=None, scale=1.0, keep_z=False):
    """Join all parts into one mesh called `name`, put its feet on the floor (`keep_z`: leave the heights as built, e.g. a stage whose floor is z = 0), unwrap, render previews (--preview DIR), then bake and export docs/models/<out_file>."""
    OUT = os.path.join(MODELS, out_file)
    bpy.ops.object.select_all(action="DESELECT")
    for o in parts:
        if o.name in bpy.data.objects: o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    obj = bpy.context.active_object; obj.name = name
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)   # the joined mesh inherits the first part's rotation: bake it in
    if scale != 1.0:
        obj.scale = (scale, scale, scale); bpy.ops.object.transform_apply(scale=True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=0.0002)
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode="OBJECT")
    bpy.ops.object.shade_smooth()
    bb = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb))); hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    obj.location -= Vector(((lo.x + hi.x) / 2 - center_x, (lo.y + hi.y) / 2, 0.0 if keep_z else lo.z))
    bpy.ops.object.transform_apply(location=True)
    h = hi.z - lo.z
    print("size m: %.2f wide, %.2f deep, %.2f tall, %d faces" % (hi.x - lo.x, hi.y - lo.y, h, len(obj.data.polygons)))
    stage()
    views = views or [("front", (0.4 * h, -2.6 * h, 0.75 * h), (0, 0, 0.5 * h), (640, 800), 60), ("side", (2.6 * h, -0.8 * h, 0.75 * h), (0, 0, 0.5 * h), (640, 800), 60),
                      ("close", (0.35 * h, -1.1 * h, 0.95 * h), (0, -0.05 * h, 0.72 * h), (720, 640), 70)]
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True)
        for tag, cam_loc, target, size, lens in views: render(os.path.join(PREVIEW, "%s_%s.png" % (name.lower(), tag)), cam_loc, target, size=size, lens=lens)
    if not BAKE: return obj

    bpy.ops.object.select_all(action="DESELECT"); obj.select_set(True); bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.004)
    bpy.ops.object.mode_set(mode="OBJECT")
    scene.cycles.samples = 48
    scene.render.bake.margin = 6

    def bake_to(image_name, kind):
        img = bpy.data.images.new(image_name, TEX, TEX, alpha=False)
        img.generated_color = (0.0, 0.0, 0.0, 1.0) if kind == "EMIT" else (0.5, 0.5, 0.5, 1.0)
        for m in obj.data.materials:
            n = m.node_tree.nodes.new("ShaderNodeTexImage"); n.image = img; m.node_tree.nodes.active = n
        if kind == "DIFFUSE":
            scene.render.bake.use_pass_direct = False; scene.render.bake.use_pass_indirect = False; scene.render.bake.use_pass_color = True
        bpy.ops.object.bake(type=kind)
        for m in obj.data.materials:
            nt = m.node_tree
            for n in [n for n in nt.nodes if n.type == "TEX_IMAGE" and n.image == img]: nt.nodes.remove(n)
        return img

    albedo = bake_to(name.lower() + "_albedo", "DIFFUSE")
    ao = bake_to(name.lower() + "_ao", "AO")
    emit = bake_to(name.lower() + "_emit", "EMIT")
    import numpy as np
    a = np.array(albedo.pixels[:], dtype=np.float32).reshape(-1, 4)
    o = np.array(ao.pixels[:], dtype=np.float32).reshape(-1, 4)
    a[:, :3] = np.clip(a[:, :3] * (0.35 + 0.65 * o[:, :1]), 0, 1)     # the ambient occlusion goes into the colour, like painted shading
    albedo.pixels[:] = a.reshape(-1).tolist()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if PREVIEW:
        albedo.filepath_raw = os.path.join(PREVIEW, name.lower() + "_albedo.png"); albedo.file_format = "PNG"; albedo.save()
        emit.filepath_raw = os.path.join(PREVIEW, name.lower() + "_emit.png"); emit.file_format = "PNG"; emit.save()
    albedo.pack(); emit.pack()

    final = bpy.data.materials.new("M_" + name); final.use_nodes = True
    nt = final.node_tree; nt.nodes.clear()
    o_out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit
    nt.links.new(t1.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], b.inputs["Emission Color"]); b.inputs["Emission Strength"].default_value = 1.0
    b.inputs["Metallic"].default_value = 0.2; b.inputs["Roughness"].default_value = 0.6
    nt.links.new(b.outputs["BSDF"], o_out.inputs["Surface"])
    obj.data.materials.clear(); obj.data.materials.append(final)
    if PREVIEW:
        render(os.path.join(PREVIEW, name.lower() + "_baked.png"), views[0][1], views[0][2], size=views[0][3], lens=views[0][4])
    bpy.ops.object.select_all(action="DESELECT"); obj.select_set(True); bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_materials="EXPORT", export_image_format="AUTO", export_cameras=False, export_lights=False)
    print("exported", OUT, os.path.getsize(OUT) // 1024, "KB")
    return obj


# ------------------------------------------------------------------------------------------------------------------------------ the game-model pipeline: high-poly source -> low-poly model + baked maps
def _join(name):
    bpy.ops.object.select_all(action="DESELECT")
    for o in parts:
        if o.name in bpy.data.objects: o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    obj = bpy.context.active_object; obj.name = name
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.remove_doubles(threshold=0.0002)
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.object.mode_set(mode="OBJECT")
    return obj


def tri_count(obj):
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def finalize_lod(name, out_file, build, budget=8000, scale=1.0, views=None, uv_boost=None, ao_strength=0.6, extra_views=None):
    """`build()` makes the whole model with the kit's primitives, which honour LOD. It runs twice: "high" (bevels, subdivision, micro parts, runes: the detail source) and "low" (the game model: big
    silhouette shapes only). The low model is unwrapped and the high model's albedo, ambient occlusion, emission and NORMAL detail are baked onto it, so the game model reads as detailed as the source.
    Exits with an error if the low model is over `budget` triangles."""
    OUT = os.path.join(MODELS, out_file)
    set_lod("high"); parts.clear(); build(); high = _join(name + "_high")
    set_lod("low"); parts.clear(); build()
    for o in parts:                                                       # texel-density weights per part (name prefix -> factor): faces and hair get more of the 2048 px
        k = next((v for pre, v in (uv_boost or {}).items() if o.name.startswith(pre)), 1.0)
        att = o.data.attributes.new("uvboost", "FLOAT", "POINT"); att.data.foreach_set("value", [k] * len(o.data.vertices))
    low = _join(name)
    for o in (high, low):
        if scale != 1.0:
            o.scale = (scale, scale, scale)
            bpy.context.view_layer.objects.active = o; bpy.ops.object.select_all(action="DESELECT"); o.select_set(True); bpy.ops.object.transform_apply(scale=True)
    bb = [low.matrix_world @ Vector(c) for c in low.bound_box]
    lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb))); hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
    off = Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
    for o in (high, low):
        o.location -= off
        bpy.context.view_layer.objects.active = o; bpy.ops.object.select_all(action="DESELECT"); o.select_set(True); bpy.ops.object.transform_apply(location=True)
    h = hi.z - lo.z
    tris = tri_count(low)
    print("game model: %d triangles (budget %d), %.2f wide, %.2f deep, %.2f tall; source: %d triangles" % (tris, budget, hi.x - lo.x, hi.y - lo.y, h, tri_count(high)))
    if tris > budget * 1.02:
        raise SystemExit("OVER BUDGET: %d triangles > %d" % (tris, budget))
    for o in (high, low):
        bpy.context.view_layer.objects.active = o; bpy.ops.object.select_all(action="DESELECT"); o.select_set(True)
        bpy.ops.object.shade_smooth()
    bpy.context.view_layer.objects.active = low; bpy.ops.object.select_all(action="DESELECT"); low.select_set(True)
    try: bpy.ops.object.shade_smooth_by_angle(angle=math.radians(38))
    except Exception: pass
    stage()
    views = views or [("front", (0.4 * h, -2.6 * h, 0.75 * h), (0, 0, 0.5 * h), (640, 800), 60), ("side", (2.6 * h, -0.8 * h, 0.75 * h), (0, 0, 0.5 * h), (640, 800), 60),
                      ("close", (0.35 * h, -1.1 * h, 0.95 * h), (0, -0.05 * h, 0.72 * h), (720, 640), 70)]
    views = list(views) + list(extra_views or [])
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True)
        low.hide_render = True                                  # the source model, for comparison
        for tag, cam_loc, target, size, lens in views[:1]: render(os.path.join(PREVIEW, "%s_source_%s.png" % (name.lower(), tag)), cam_loc, target, size=size, lens=lens)
        low.hide_render = False; high.hide_render = True
        for tag, cam_loc, target, size, lens in views: render(os.path.join(PREVIEW, "%s_low_%s.png" % (name.lower(), tag)), cam_loc, target, size=size, lens=lens)
        high.hide_render = False
    if not BAKE: return low

    # ---- unwrap the game model, bake the source's detail onto it
    # the unwrap runs on a copy whose boosted parts are scaled up (UV density follows 3D size), then its UVs are copied to the real game model (same topology)
    dup = low.copy(); dup.data = low.data.copy(); scene.collection.objects.link(dup)
    bm = bmesh.new(); bm.from_mesh(dup.data); lay = bm.verts.layers.float.get("uvboost"); seen = set()
    for v0 in bm.verts:
        if v0.index in seen: continue
        comp, stack = [], [v0]; seen.add(v0.index)
        while stack:
            v = stack.pop(); comp.append(v)
            for e in v.link_edges:
                w = e.other_vert(v)
                if w.index not in seen: seen.add(w.index); stack.append(w)
        k = v0[lay] if lay else 1.0
        if k != 1.0:
            c = sum((v.co for v in comp), Vector()) / len(comp)
            for v in comp: v.co = c + (v.co - c) * k
    bm.to_mesh(dup.data); bm.free()
    bpy.ops.object.select_all(action="DESELECT"); dup.select_set(True); bpy.context.view_layer.objects.active = dup
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(80), island_margin=0.002)
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.average_islands_scale()                                    # one texel density everywhere (times the part's boost)
    try: bpy.ops.uv.pack_islands(rotate=True, margin=0.0025)              # tight pack: no dead space, so 2048 px go further
    except TypeError: bpy.ops.uv.pack_islands(margin=0.0025)
    bpy.ops.object.mode_set(mode="OBJECT")
    import numpy as np
    src = dup.data.uv_layers.active.data; arr = np.zeros(len(src) * 2, dtype=np.float32); src.foreach_get("uv", arr)
    dst = low.data.uv_layers.active if low.data.uv_layers else low.data.uv_layers.new(name="UVMap")
    dst.data.foreach_set("uv", arr)
    bpy.data.objects.remove(dup, do_unlink=True)
    bpy.ops.object.select_all(action="DESELECT"); low.select_set(True); bpy.context.view_layer.objects.active = low
    bake_mat = bpy.data.materials.new("bake_target"); bake_mat.use_nodes = True
    low.data.materials.clear(); low.data.materials.append(bake_mat)
    b = scene.render.bake
    scene.cycles.samples = 48
    b.use_selected_to_active = True; b.cage_extrusion = 0.055; b.max_ray_distance = 0.14; b.margin = 10
    b.use_pass_direct = False; b.use_pass_indirect = False; b.use_pass_color = True
    b.normal_space = "TANGENT"

    SS = int(os.environ.get("W2F_SS", "2"))
    def bake_to(image_name, kind, color=(0.5, 0.5, 0.5, 1.0)):
        img = bpy.data.images.new(image_name, TEX * SS, TEX * SS, alpha=False)
        img.generated_color = color
        if kind == "NORMAL": img.colorspace_settings.name = "Non-Color"
        nt = bake_mat.node_tree
        n = nt.nodes.new("ShaderNodeTexImage"); n.image = img; nt.nodes.active = n
        bpy.ops.object.select_all(action="DESELECT"); high.select_set(True); low.select_set(True); bpy.context.view_layer.objects.active = low
        bpy.ops.object.bake(type=kind)
        nt.nodes.remove(n)
        if SS > 1: img.scale(TEX, TEX)                                # baked at 2x and box-filtered down: clean edges on thin runes
        return img

    albedo = bake_to(name.lower() + "_albedo", "DIFFUSE")
    ao = bake_to(name.lower() + "_ao", "AO")
    normal = bake_to(name.lower() + "_normal", "NORMAL", (0.5, 0.5, 1.0, 1.0))
    emit = bake_to(name.lower() + "_emit", "EMIT", (0.0, 0.0, 0.0, 1.0))
    import numpy as np
    a = np.array(albedo.pixels[:], dtype=np.float32).reshape(-1, 4)
    o = np.array(ao.pixels[:], dtype=np.float32).reshape(-1, 4)
    a[:, :3] = np.clip(a[:, :3] * ((1.0 - ao_strength) + ao_strength * o[:, :1]), 0, 1)       # the ambient occlusion goes into the colour, like painted shading
    albedo.pixels[:] = a.reshape(-1).tolist()
    if PREVIEW:
        for img, tag in ((albedo, "albedo"), (normal, "normal"), (emit, "emit")):
            img.filepath_raw = os.path.join(PREVIEW, "%s_%s.png" % (name.lower(), tag)); img.file_format = "PNG"; img.save()
    for img in (albedo, normal, emit): img.pack()

    final = bpy.data.materials.new("M_" + name); final.use_nodes = True
    nt = final.node_tree; nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial"); bs = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit
    t3 = nt.nodes.new("ShaderNodeTexImage"); t3.image = normal
    nm = nt.nodes.new("ShaderNodeNormalMap")
    nt.links.new(t1.outputs["Color"], bs.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], bs.inputs["Emission Color"]); bs.inputs["Emission Strength"].default_value = 1.0
    nt.links.new(t3.outputs["Color"], nm.inputs["Color"])
    if not os.environ.get("W2F_NO_NORMAL"): nt.links.new(nm.outputs["Normal"], bs.inputs["Normal"])
    bs.inputs["Metallic"].default_value = 0.2; bs.inputs["Roughness"].default_value = 0.6
    nt.links.new(bs.outputs["BSDF"], out.inputs["Surface"])
    low.data.materials.clear(); low.data.materials.append(final)
    bpy.data.objects.remove(high, do_unlink=True)                      # only the game model is exported
    if PREVIEW:
        render(os.path.join(PREVIEW, name.lower() + "_baked.png"), views[0][1], views[0][2], size=views[0][3], lens=views[0][4])
        render(os.path.join(PREVIEW, name.lower() + "_baked_close.png"), views[2][1], views[2][2], size=views[2][3], lens=views[2][4])
        for tag, cam_loc, target, size, lens in views[3:]: render(os.path.join(PREVIEW, "%s_baked_%s.png" % (name.lower(), tag)), cam_loc, target, size=size, lens=lens)
    bpy.ops.object.select_all(action="DESELECT"); low.select_set(True); bpy.context.view_layer.objects.active = low
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_materials="EXPORT", export_image_format="AUTO", export_cameras=False, export_lights=False)
    print("exported", OUT, os.path.getsize(OUT) // 1024, "KB,", tris, "triangles")
    return low
