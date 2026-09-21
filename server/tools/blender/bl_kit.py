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
def finish(obj, mat, bevel=0.0, subsurf=0, smooth=True, segments=3, displace=None):
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


def box(name, loc, size, mat, rot=(0, 0, 0), bevel=0.025, subsurf=1, taper=None, displace=None):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    if taper:
        for v in bm.verts:
            if v.co.z > 0: v.co.x *= taper; v.co.y *= taper
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    o = make(name, bm); o.location = loc; o.rotation_euler = _euler(rot)
    return finish(o, mat, bevel, subsurf, displace=displace)


def sphere(name, loc, size, mat, rot=(0, 0, 0), seg=28, rings=16, subsurf=0, cut=None, displace=None):
    bm = bmesh.new(); bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=rings, radius=1.0)
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    if cut is not None:
        geom = [f for f in bm.faces if all(v.co.z < cut * size[2] - 1e-6 for v in f.verts)]
        bmesh.ops.delete(bm, geom=geom, context="FACES")
    o = make(name, bm); o.location = loc; o.rotation_euler = _euler(rot)
    return finish(o, mat, 0.0, subsurf, displace=displace)


def limb(name, p0, p1, r0, r1, mat, seg=24, bevel=0.0, subsurf=0, caps=True, displace=None):
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=caps, cap_tris=False, segments=seg, radius1=r0, radius2=r1, depth=d.length)
    o = make(name, bm); o.location = (p0 + p1) / 2
    o.rotation_mode = "QUATERNION"; o.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(d.normalized())
    return finish(o, mat, bevel, subsurf, displace=displace)


def ring(name, loc, radius, tube_r, mat, rot=(0, 0, 0), scale=(1, 1, 1), seg=32, tseg=10):
    bpy.ops.mesh.primitive_torus_add(major_radius=radius, minor_radius=tube_r, major_segments=seg, minor_segments=tseg, location=loc, rotation=[math.radians(a) for a in rot])
    o = bpy.context.active_object; o.name = name; o.scale = scale
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat); bpy.ops.object.shade_smooth(); parts.append(o)
    return o


def tube(name, points, radius, mat, res=6):
    """A constant-thickness line along a polyline: runes and cracks."""
    cu = bpy.data.curves.new(name, "CURVE"); cu.dimensions = "3D"; cu.bevel_depth = radius; cu.bevel_resolution = res; cu.resolution_u = 6
    sp = cu.splines.new("POLY"); sp.points.add(len(points) - 1)
    for i, p in enumerate(points): sp.points[i].co = (p[0], p[1], p[2], 1)
    o = bpy.data.objects.new(name, cu); scene.collection.objects.link(o)
    bpy.context.view_layer.objects.active = o; o.select_set(True)
    bpy.ops.object.convert(target="MESH")
    o = bpy.context.active_object; o.data.materials.append(mat); parts.append(o)
    return o


def swoop(name, points, radii, mat, res=8, cyclic=False, displace=None):
    """A smooth tube through `points` whose thickness follows `radii` (one per point): tails, hair, branches, tentacles, tapered blades."""
    cu = bpy.data.curves.new(name, "CURVE"); cu.dimensions = "3D"; cu.bevel_depth = 1.0; cu.bevel_resolution = res; cu.resolution_u = 14; cu.use_fill_caps = True
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


def ribbon(name, spine, widths, mat, up=(0, 1, 0), thick=0.012, wave=0.0, subsurf=1):
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
def finalize(name, out_file, center_x=0.0, views=None, scale=1.0):
    """Join all parts into one mesh called `name`, put its feet on the floor, unwrap, render previews (--preview DIR), then bake and export docs/models/<out_file>."""
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
    obj.location -= Vector(((lo.x + hi.x) / 2 - center_x, (lo.y + hi.y) / 2, lo.z))
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
