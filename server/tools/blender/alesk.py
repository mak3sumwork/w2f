"""Alesk, Protector of Helios: a crafted model after the splash art (splash_arts/Alesk.jpg), modelled and painted in Blender (bpy), exported as glTF.

Run with a Python that has the `bpy` module (Blender as a module):   ~/w2f_bpy/venv/bin/python tools/blender/alesk.py [--preview DIR] [--no-bake]

What it builds: a dark-iron golem in a wide combat stance. Layered rounded pauldrons with carved rune circles, a helmet with a T-visor and glowing teal eyes, a hexagonal teal gem set in the
breastplate, a heavy gorget, plated arms with big gauntlets, thick legs with knee guards and boots, a belt with tassets, and an enormous tower shield with a glowing rune diamond and cracks.
Everything is beveled and subdivided, smooth-shaded, unwrapped, and BAKED: worn-metal albedo (rust in the crevices, bright edges), ambient occlusion and the emission of the runes go into two textures on
one material, so the glTF needs nothing else. Units: metres, Blender Z up, the model faces -Y (which the glTF exporter turns into +Z, the facing of every W2F model), feet at z = 0.
"""
import bpy, bmesh, math, sys, os
from mathutils import Vector, Euler

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "models", "SM_Champion_9001_Alesk.glb"))
PREVIEW = ARGS[ARGS.index("--preview") + 1] if "--preview" in ARGS else None
BAKE = "--no-bake" not in ARGS
TEX = 2048

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
parts = []


# ------------------------------------------------------------------------------------------------------------------------------ materials
def material(name, color, metallic=0.3, rough=0.55, emit=None, strength=6.0, wear=False, rust=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.inputs["Metallic"].default_value = metallic
    bsdf.inputs["Roughness"].default_value = rough
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    base = nt.nodes.new("ShaderNodeRGB")
    base.outputs[0].default_value = (*color, 1.0)
    colour_out = base.outputs[0]
    if wear:   # worn metal: large-scale mottling, grime in the crevices, bright scuffed edges (pointiness), a little rust
        tex = nt.nodes.new("ShaderNodeTexNoise"); tex.inputs["Scale"].default_value = 5.0; tex.inputs["Detail"].default_value = 10.0; tex.inputs["Roughness"].default_value = 0.65
        fine = nt.nodes.new("ShaderNodeTexNoise"); fine.inputs["Scale"].default_value = 60.0; fine.inputs["Detail"].default_value = 4.0
        ramp = nt.nodes.new("ShaderNodeValToRGB"); ramp.color_ramp.elements[0].position = 0.35; ramp.color_ramp.elements[1].position = 0.7
        ramp.color_ramp.elements[0].color = (0.55, 0.55, 0.6, 1); ramp.color_ramp.elements[1].color = (1.15, 1.1, 1.05, 1)
        nt.links.new(tex.outputs["Fac"], ramp.inputs["Fac"])
        mult = nt.nodes.new("ShaderNodeMix"); mult.data_type = "RGBA"; mult.blend_type = "MULTIPLY"; mult.inputs["Factor"].default_value = 1.0
        nt.links.new(colour_out, mult.inputs["A"]); nt.links.new(ramp.outputs["Color"], mult.inputs["B"])
        geo = nt.nodes.new("ShaderNodeNewGeometry")
        edge = nt.nodes.new("ShaderNodeValToRGB"); edge.color_ramp.elements[0].position = 0.5; edge.color_ramp.elements[1].position = 0.62
        edge.color_ramp.elements[0].color = (0, 0, 0, 1); edge.color_ramp.elements[1].color = (1, 1, 1, 1)
        nt.links.new(geo.outputs["Pointiness"], edge.inputs["Fac"])
        rusty = nt.nodes.new("ShaderNodeMix"); rusty.data_type = "RGBA"; rusty.blend_type = "MIX"
        rc = nt.nodes.new("ShaderNodeRGB"); rc.outputs[0].default_value = (0.34, 0.17, 0.09, 1)
        rmask = nt.nodes.new("ShaderNodeMath"); rmask.operation = "MULTIPLY"; rmask.inputs[1].default_value = rust
        inv = nt.nodes.new("ShaderNodeMath"); inv.operation = "SUBTRACT"; inv.inputs[0].default_value = 1.0
        nt.links.new(fine.outputs["Fac"], inv.inputs[1]); nt.links.new(inv.outputs[0], rmask.inputs[0])
        nt.links.new(mult.outputs["Result"], rusty.inputs["A"]); nt.links.new(rc.outputs[0], rusty.inputs["B"]); nt.links.new(rmask.outputs[0], rusty.inputs["Factor"])
        bright = nt.nodes.new("ShaderNodeMix"); bright.data_type = "RGBA"; bright.blend_type = "ADD"; bright.inputs["Factor"].default_value = 0.16
        nt.links.new(rusty.outputs["Result"], bright.inputs["A"]); nt.links.new(edge.outputs["Color"], bright.inputs["B"])
        colour_out = bright.outputs["Result"]
    nt.links.new(colour_out, bsdf.inputs["Base Color"])
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*emit, 1.0)
        bsdf.inputs["Emission Strength"].default_value = strength
    return m


IRON = material("Iron", (0.085, 0.095, 0.11), 0.35, 0.5, wear=True, rust=0.5)
IRON2 = material("IronLight", (0.16, 0.175, 0.2), 0.35, 0.45, wear=True, rust=0.3)
TRIM = material("Trim", (0.30, 0.235, 0.13), 0.45, 0.4, wear=True, rust=0.2)
UNDER = material("Under", (0.02, 0.022, 0.026), 0.0, 0.85)
RUNE = material("Rune", (0.05, 0.9, 0.72), 0.0, 0.3, emit=(0.08, 1.0, 0.8), strength=1.0)
MATS = [IRON, IRON2, TRIM, UNDER, RUNE]


# ------------------------------------------------------------------------------------------------------------------------------ primitives
def finish(obj, mat, bevel=0.0, subsurf=0, smooth=True, segments=3):
    obj.data.materials.append(mat)
    scene.collection.objects.link(obj) if obj.name not in scene.collection.objects else None
    bpy.context.view_layer.objects.active = obj
    for o in bpy.context.selected_objects: o.select_set(False)
    obj.select_set(True)
    if bevel > 0:
        mod = obj.modifiers.new("Bevel", "BEVEL"); mod.width = bevel; mod.segments = segments; mod.limit_method = "ANGLE"; mod.angle_limit = math.radians(35)
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if subsurf:
        mod = obj.modifiers.new("Sub", "SUBSURF"); mod.levels = subsurf; mod.render_levels = subsurf
        bpy.ops.object.modifier_apply(modifier=mod.name)
    if smooth: bpy.ops.object.shade_smooth()
    parts.append(obj)
    return obj


def make(name, bm):
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
    return bpy.data.objects.new(name, me)


def box(name, loc, size, mat, rot=(0, 0, 0), bevel=0.025, subsurf=1, taper=None):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    if taper:   # narrower at the top (taper < 1) or wider (> 1)
        for v in bm.verts:
            if v.co.z > 0: v.co.x *= taper; v.co.y *= taper
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    o = make(name, bm); o.location = loc; o.rotation_euler = Euler([math.radians(a) for a in rot])
    return finish(o, mat, bevel, subsurf)


def sphere(name, loc, size, mat, rot=(0, 0, 0), seg=28, rings=16, subsurf=0, cut=None):
    bm = bmesh.new(); bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=rings, radius=1.0)
    for v in bm.verts: v.co = Vector((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    if cut is not None:   # keep only the part with z >= cut * size[2] (a dome)
        geom = [f for f in bm.faces if all(v.co.z < cut * size[2] - 1e-6 for v in f.verts)]
        bmesh.ops.delete(bm, geom=geom, context="FACES")
    o = make(name, bm); o.location = loc; o.rotation_euler = Euler([math.radians(a) for a in rot])
    return finish(o, mat, 0.0, subsurf)


def limb(name, p0, p1, r0, r1, mat, seg=24, bevel=0.0, subsurf=0, caps=True):
    p0, p1 = Vector(p0), Vector(p1)
    d = p1 - p0
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=caps, cap_tris=False, segments=seg, radius1=r0, radius2=r1, depth=d.length)
    o = make(name, bm)
    o.location = (p0 + p1) / 2
    o.rotation_mode = "QUATERNION"; o.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(d.normalized())
    return finish(o, mat, bevel, subsurf)


def ring(name, loc, radius, tube, mat, rot=(0, 0, 0), scale=(1, 1, 1), seg=32, tseg=10):
    bpy.ops.mesh.primitive_torus_add(major_radius=radius, minor_radius=tube, major_segments=seg, minor_segments=tseg, location=loc, rotation=[math.radians(a) for a in rot])
    o = bpy.context.active_object; o.name = name; o.scale = scale
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat); bpy.ops.object.shade_smooth(); parts.append(o)
    return o


def tube(name, points, radius, mat, res=6):
    """A glowing line along a curve: runes and cracks."""
    cu = bpy.data.curves.new(name, "CURVE"); cu.dimensions = "3D"; cu.bevel_depth = radius; cu.bevel_resolution = res; cu.resolution_u = 6
    sp = cu.splines.new("POLY"); sp.points.add(len(points) - 1)
    for i, p in enumerate(points): sp.points[i].co = (p[0], p[1], p[2], 1)
    o = bpy.data.objects.new(name, cu); scene.collection.objects.link(o)
    bpy.context.view_layer.objects.active = o; o.select_set(True)
    bpy.ops.object.convert(target="MESH")
    o = bpy.context.active_object; o.data.materials.append(mat); parts.append(o)
    return o


def mirror_x(fn, *a, **k):
    """Call a part builder for both sides (x and -x)."""
    for s in (1, -1): fn(s, *a, **k)


# ------------------------------------------------------------------------------------------------------------------------------ the golem
def legs():
    for s in (1, -1):
        x = 0.34 * s
        foot_y = -0.08 if s < 0 else 0.06                                            # a wide stance, the right leg (x < 0) a step forward
        sphere("hip%d" % s, (x, 0, 1.0), (0.26, 0.24, 0.24), IRON)
        limb("thigh%d" % s, (x, 0.0, 1.0), (x * 1.08, foot_y * 0.5, 0.58), 0.22, 0.18, IRON, seg=20)
        limb("thighUnder%d" % s, (x, 0.0, 1.0), (x * 1.08, foot_y * 0.5, 0.58), 0.2, 0.16, UNDER, seg=20)
        box("thighPlate%d" % s, (x * 1.02, foot_y * 0.3 - 0.13, 0.82), (0.3, 0.1, 0.36), IRON2, rot=(-6, 0, 0))
        sphere("kneeGuard%d" % s, (x * 1.08, foot_y * 0.5 - 0.17, 0.55), (0.22, 0.13, 0.22), IRON2, seg=24, rings=14)
        ring("kneeRing%d" % s, (x * 1.08, foot_y * 0.5 - 0.06, 0.55), 0.19, 0.03, TRIM, rot=(0, 0, 0), scale=(1, 1, 1))
        limb("shin%d" % s, (x * 1.08, foot_y * 0.5, 0.55), (x * 1.1, foot_y, 0.16), 0.17, 0.14, IRON, seg=20)
        box("shinPlate%d" % s, (x * 1.09, foot_y - 0.15, 0.36), (0.26, 0.09, 0.4), IRON2, rot=(8, 0, 0))
        box("boot%d" % s, (x * 1.1, foot_y - 0.1, 0.09), (0.34, 0.6, 0.18), IRON, bevel=0.04, subsurf=1)
        box("toe%d" % s, (x * 1.1, foot_y - 0.36, 0.09), (0.32, 0.18, 0.15), IRON2, rot=(0, 0, 0), bevel=0.04)
        box("sole%d" % s, (x * 1.1, foot_y - 0.1, 0.02), (0.36, 0.66, 0.05), UNDER, bevel=0.01, subsurf=0)
        tube("kneeRune%d" % s, [(x * 1.08 - 0.08, foot_y * 0.5 - 0.29, 0.62), (x * 1.08, foot_y * 0.5 - 0.3, 0.55), (x * 1.08 + 0.08, foot_y * 0.5 - 0.29, 0.62)], 0.012, RUNE)


def pelvis():
    box("pelvis", (0, 0, 1.06), (0.7, 0.42, 0.26), IRON, bevel=0.05, subsurf=1)
    box("belt", (0, -0.02, 1.2), (0.78, 0.46, 0.13), TRIM, bevel=0.03, subsurf=1)
    box("buckle", (0, -0.26, 1.2), (0.2, 0.05, 0.16), IRON2, bevel=0.02)
    tube("buckleRune", [(0, -0.29, 1.14), (0, -0.29, 1.26)], 0.012, RUNE)
    for i, x in enumerate((-0.26, -0.09, 0.09, 0.26)):                                # tassets hanging in front
        box("tasset%d" % i, (x, -0.22, 0.92 - 0.02 * abs(x)), (0.16, 0.06, 0.34 - 0.05 * (abs(x) < 0.15)), IRON2, rot=(-8, 0, x * 25), bevel=0.02)


def torso():
    sphere("belly", (0, 0.0, 1.4), (0.44, 0.32, 0.26), IRON, seg=28, rings=16)
    sphere("chest", (0, 0.02, 1.68), (0.6, 0.38, 0.4), IRON, seg=32, rings=18)
    sphere("back", (0, 0.2, 1.68), (0.5, 0.22, 0.42), IRON2, seg=24, rings=14)
    box("breastplate", (0, -0.27, 1.7), (0.7, 0.16, 0.5), IRON2, rot=(-8, 0, 0), bevel=0.07, subsurf=1, taper=0.9)
    box("abdomenPlate", (0, -0.23, 1.3), (0.5, 0.12, 0.26), IRON, rot=(-6, 0, 0), bevel=0.04)
    box("abdomenPlate2", (0, -0.24, 1.16), (0.46, 0.12, 0.16), IRON2, rot=(-4, 0, 0), bevel=0.04)
    # the gem: a hexagonal teal crystal in a socket
    bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.13, depth=0.07, location=(0, -0.37, 1.74), rotation=(math.radians(90 - 8), 0, 0))
    socket = bpy.context.active_object; socket.name = "gemSocket"; socket.data.materials.append(TRIM); bpy.ops.object.shade_smooth(); parts.append(socket)
    sphere("gem", (0, -0.4, 1.74), (0.1, 0.06, 0.12), RUNE, seg=6, rings=6)
    for i in range(6):                                                                 # a ring of rivets around the socket
        a = math.radians(60 * i + 30)
        sphere("rivet%d" % i, (0.17 * math.cos(a), -0.375, 1.74 + 0.17 * math.sin(a)), (0.02, 0.02, 0.02), TRIM, seg=8, rings=6)
    tube("chestRuneL", [(0.14, -0.36, 1.78), (0.28, -0.35, 1.84), (0.36, -0.34, 1.76), (0.33, -0.33, 1.62)], 0.011, RUNE)
    tube("chestRuneR", [(-0.14, -0.36, 1.78), (-0.28, -0.35, 1.84), (-0.36, -0.34, 1.76), (-0.33, -0.33, 1.62)], 0.011, RUNE)
    # gorget and neck
    limb("neck", (0, 0, 2.0), (0, -0.02, 1.86), 0.13, 0.17, UNDER, seg=18)
    ring("gorget", (0, -0.01, 1.98), 0.2, 0.06, IRON2, scale=(1.05, 0.9, 0.6), rot=(0, 0, 0))
    ring("gorget2", (0, -0.01, 1.9), 0.27, 0.06, IRON, scale=(1.1, 0.95, 0.55), rot=(0, 0, 0))


def head():
    """A knight's great helm: a rounded dome rising to a point, a heavy brow, a narrow T-visor with slanted glowing eyes, cheek guards, a crest fin."""
    sphere("helm", (0, -0.02, 2.19), (0.25, 0.27, 0.27), IRON2, seg=36, rings=22)
    sphere("helmTop", (0, -0.02, 2.36), (0.15, 0.18, 0.2), IRON2, seg=24, rings=14)                 # the dome rises to a peak
    box("helmPeak", (0, 0.0, 2.55), (0.05, 0.16, 0.16), IRON, rot=(-20, 0, 0), bevel=0.012, subsurf=1)
    box("browRidge", (0, -0.23, 2.24), (0.42, 0.12, 0.07), IRON, rot=(-12, 0, 0), bevel=0.02, subsurf=1)
    box("visor", (0, -0.225, 2.12), (0.36, 0.09, 0.2), IRON, rot=(-4, 0, 0), bevel=0.02, subsurf=1)
    box("visorSlit", (0, -0.285, 2.15), (0.3, 0.02, 0.05), UNDER, bevel=0.005, subsurf=0)
    box("visorStem", (0, -0.285, 2.05), (0.05, 0.02, 0.16), UNDER, bevel=0.005, subsurf=0)          # the T of the visor
    for s_ in (1, -1):
        box("eye%d" % s_, (0.085 * s_, -0.293, 2.152), (0.1, 0.014, 0.032), RUNE, rot=(0, 0, -14 * s_), bevel=0.004, subsurf=0)
        box("cheek%d" % s_, (0.2 * s_, -0.14, 2.03), (0.09, 0.22, 0.26), IRON, rot=(0, 0, 12 * s_), bevel=0.025)
        box("cheekEdge%d" % s_, (0.235 * s_, -0.04, 2.13), (0.04, 0.24, 0.3), IRON2, rot=(0, 0, 12 * s_), bevel=0.015)
        box("earPlate%d" % s_, (0.25 * s_, 0.02, 2.2), (0.06, 0.16, 0.2), TRIM, rot=(0, 0, 8 * s_), bevel=0.012)
        tube("cheekRune%d" % s_, [(0.2 * s_, -0.255, 2.0), (0.22 * s_, -0.25, 1.94)], 0.008, RUNE)
    box("chin", (0, -0.23, 1.93), (0.26, 0.1, 0.13), IRON, rot=(-15, 0, 0), bevel=0.025)
    tube("browRune", [(-0.14, -0.29, 2.22), (0, -0.305, 2.27), (0.14, -0.29, 2.22)], 0.008, RUNE)


def pauldron(s):
    """A big shoulder guard like a segmented shell: a dome ribbed with radial bands, a flared skirt plate below it with a trim edge, a rune disc on the front, rivets and spikes."""
    x = 0.86 * s
    sphere("pauld%dTop" % s, (x, 0.0, 1.95), (0.47, 0.41, 0.36), IRON2, seg=44, rings=22, cut=-0.02)
    for k in range(7):                                                                            # radial ribs: half-tori standing on the dome like the seams of a shell
        a = -90 + 30 * k
        ring("pauld%dRib%d" % (s, k), (x, 0.0, 1.95), 0.44, 0.022, TRIM if k % 3 == 1 else IRON, rot=(90, 0, a), scale=(1, 0.8, 1.0))
    limb("pauld%dSkirt" % s, (x, 0.0, 1.72), (x, 0.0, 1.97), 0.55, 0.46, IRON, seg=44, caps=False)   # the flared skirt: an open cone under the dome
    ring("pauld%dSkirtEdge" % s, (x, 0.0, 1.73), 0.55, 0.03, TRIM, scale=(1, 0.88, 0.6))
    ring("pauld%dSkirtEdge2" % s, (x, 0.0, 1.85), 0.5, 0.02, IRON2, scale=(1, 0.88, 0.6))
    box("pauld%dDisc" % s, (x, -0.44, 1.98), (0.34, 0.06, 0.34), TRIM, rot=(-14, 0, 0), bevel=0.02)   # the rune disc on the front
    ring("pauld%dDiscRune" % s, (x, -0.482, 1.985), 0.125, 0.012, RUNE, rot=(76, 0, 0))
    tube("pauld%dGlyph" % s, [(x - 0.05, -0.49, 1.93), (x, -0.495, 2.05), (x + 0.05, -0.49, 1.93)], 0.009, RUNE)
    for k in range(7):
        a = math.radians(-100 + 200 * k / 6)
        sphere("pRivet%d_%d" % (s, k), (x + 0.5 * math.sin(a) * s * 0.9, -0.44 * math.cos(a) - 0.02, 1.76), (0.03, 0.03, 0.03), TRIM, seg=8, rings=6)
    sphere("pauld%dSpike" % s, (x * 1.12, 0.0, 2.34), (0.07, 0.07, 0.21), TRIM, seg=10, rings=8)
    sphere("pauld%dSpike2" % s, (x * 0.94, -0.12, 2.31), (0.05, 0.05, 0.12), TRIM, seg=10, rings=8)


def shoulders_and_arms():
    for s in (1, -1): pauldron(s)
    # right arm (x < 0): pulled back with the fist clenched at the hip; left arm (x > 0): down and forward, the fist on the shield's grip
    r_sh, r_el, r_wr = (-0.82, 0.0, 1.66), (-1.02, 0.16, 1.26), (-0.94, -0.26, 1.06)
    l_sh, l_el, l_wr = (0.82, 0.0, 1.66), (1.06, 0.06, 1.24), (1.06, -0.3, 1.1)
    for name, sh, el, wr, s in (("R", r_sh, r_el, r_wr, -1), ("L", l_sh, l_el, l_wr, 1)):
        sphere("shoulderBall" + name, sh, (0.22, 0.22, 0.22), UNDER)
        limb("upperArm" + name, sh, el, 0.21, 0.18, IRON, seg=24)
        limb("upperArmUnder" + name, sh, el, 0.19, 0.16, UNDER, seg=20)
        box("bicepPlate" + name, ((sh[0] + el[0]) / 2, (sh[1] + el[1]) / 2 - 0.15, (sh[2] + el[2]) / 2 + 0.02), (0.24, 0.09, 0.34), IRON2, rot=(-10, 0, 0), bevel=0.025)
        sphere("elbow" + name, el, (0.22, 0.22, 0.22), IRON2)
        sphere("elbowSpike" + name, (el[0] + s * 0.14, el[1] + 0.06, el[2]), (0.06, 0.06, 0.16), TRIM, rot=(0, 90, 0), seg=10, rings=8)
        limb("foreArm" + name, el, wr, 0.2, 0.17, IRON, seg=24)
        for k, t in enumerate((0.28, 0.5, 0.72)):                                                      # a stack of bracer rings
            c = (el[0] * (1 - t) + wr[0] * t, el[1] * (1 - t) + wr[1] * t, el[2] * (1 - t) + wr[2] * t)
            ring("bracer%s%d" % (name, k), c, 0.2 - 0.012 * k, 0.03, IRON2 if k % 2 == 0 else TRIM, rot=(90 - 18, 0, s * 8), scale=(1, 1, 1))
        # the gauntlet: a huge fist
        box("palm" + name, (wr[0], wr[1] - 0.08, wr[2] - 0.1), (0.42, 0.4, 0.34), IRON, bevel=0.06, subsurf=1)
        for k in range(4):
            box("finger%s%d" % (name, k), (wr[0] + (k - 1.5) * 0.095, wr[1] - 0.27, wr[2] - 0.04), (0.085, 0.17, 0.11), IRON2, bevel=0.02, subsurf=0, rot=(22, 0, 0))
            sphere("knuckle%s%d" % (name, k), (wr[0] + (k - 1.5) * 0.095, wr[1] - 0.16, wr[2] + 0.1), (0.04, 0.04, 0.04), TRIM, seg=8, rings=6)
        box("thumb" + name, (wr[0] - s * 0.22, wr[1] - 0.14, wr[2] - 0.02), (0.1, 0.16, 0.11), IRON2, bevel=0.02, subsurf=0, rot=(0, 0, s * 20))
        tube("armRune" + name, [(el[0] + s * 0.02, el[1] - 0.2, el[2] - 0.02), (el[0] * 0.5 + wr[0] * 0.5, el[1] * 0.5 + wr[1] * 0.5 - 0.22, el[2] * 0.5 + wr[2] * 0.5), (wr[0], wr[1] - 0.2, wr[2] + 0.14)], 0.009, RUNE)


SHIELD_OUTLINE = [(-0.46, 0.98), (0.46, 0.98), (0.5, 0.6), (0.46, 0.1), (0.34, -0.42), (0.14, -0.84), (0.0, -0.99), (-0.14, -0.84), (-0.34, -0.42), (-0.46, 0.1), (-0.5, 0.6)]


def outline_slab(name, scale, thickness, y_front, mat, bevel=0.02):
    """The shield outline (x, z) scaled and extruded backwards from y_front: the layers of the shield are made from it."""
    bm = bmesh.new()
    vs = [bm.verts.new((x * scale, y_front, z * scale)) for x, z in SHIELD_OUTLINE]
    f = bm.faces.new(vs)
    if f.normal.y > 0: f.normal_flip()
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=0)
    res = bmesh.ops.extrude_face_region(bm, geom=[f])
    ext = [g for g in res["geom"] if isinstance(g, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, verts=ext, vec=(0, thickness, 0))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    o = make(name, bm)
    return o


def shield():
    """The tower shield planted at the left (x > 0): a curved slab (outline, raised rim, inner plate) with a glowing rune diamond, bolts and cracks."""
    cx, cy, ang = 1.2, -0.18, -16
    layers = [("shieldRim", 1.0, 0.15, 0.0, TRIM), ("shieldBody", 0.9, 0.13, -0.05, IRON), ("shieldPlate", 0.72, 0.1, -0.09, IRON2)]
    for name, sc, th, yf, mat in layers:
        o = outline_slab(name, sc, th, yf, mat)
        finish(o, mat, 0.018, 1)
        bpy.context.view_layer.objects.active = o
        mod = o.modifiers.new("Bend", "SIMPLE_DEFORM"); mod.deform_method = "BEND"; mod.deform_axis = "Z"; mod.angle = math.radians(38)
        bpy.ops.object.modifier_apply(modifier=mod.name)
        o.location = (cx, cy, 1.0); o.rotation_euler = Euler((0, 0, math.radians(ang)))
    ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    def on_face(dx, dz, out=0.2):
        """A point on the shield's front, dx to its right, dz up; the front is bent, so the point sits a little forward at the middle."""
        bend = -0.09 * (1 - (dx / 0.5) ** 2)
        lx, ly = dx, -(out + bend * -1) + 0.02
        return (cx + lx * ca - ly * sa * -1 * 0 + (-ly) * sa * -1 * 0, cy + lx * sa + ly * ca, 1.0 + dz)
    def face_point(dx, dz, out):
        rx, ry = ca, sa                                   # the shield's right, in the floor plane
        fx, fy = sa, -ca                                  # the direction the front faces
        return (cx + rx * dx + fx * (-out) * -1, cy + ry * dx + fy * (-out) * -1, 1.0 + dz)
    P = lambda dx, dz, out=0.2: face_point(dx, dz, out)
    box("shieldDiamond", P(0, 0.12), (0.34, 0.05, 0.34), TRIM, rot=(0, 45, ang), bevel=0.02, subsurf=0)
    box("shieldCore", P(0, 0.12, 0.225), (0.21, 0.03, 0.21), RUNE, rot=(0, 45, ang), bevel=0.01, subsurf=0)
    for i, (dx, dz) in enumerate(((-0.3, 0.8), (0.3, 0.8), (-0.32, 0.4), (0.32, 0.4), (-0.28, -0.3), (0.28, -0.3), (0, -0.66), (0, 0.84))):
        sphere("bolt%d" % i, P(dx, dz, 0.2), (0.04, 0.03, 0.04), TRIM, seg=10, rings=8)
    tube("shieldCrack1", [P(0.08, 0.32, 0.215), P(0.17, 0.52, 0.215), P(0.11, 0.7, 0.215), P(0.2, 0.86, 0.215)], 0.011, RUNE)
    tube("shieldCrack2", [P(-0.06, -0.06, 0.215), P(-0.2, -0.26, 0.215), P(-0.13, -0.5, 0.215), P(-0.16, -0.66, 0.215)], 0.009, RUNE)
    tube("shieldCrack3", [P(0.14, -0.22, 0.215), P(0.26, -0.34, 0.215), P(0.22, -0.52, 0.215)], 0.008, RUNE)
    tube("shieldRune", [P(-0.22, 0.6, 0.215), P(-0.11, 0.68, 0.215), P(0.0, 0.6, 0.215), P(0.11, 0.68, 0.215), P(0.22, 0.6, 0.215)], 0.008, RUNE)
    box("shieldGrip", (cx - 0.02 * ca, cy + 0.1, 1.06), (0.07, 0.07, 0.36), UNDER, rot=(0, 0, ang), bevel=0.01, subsurf=0)


legs(); pelvis(); torso(); head(); shoulders_and_arms(); shield()

# ------------------------------------------------------------------------------------------------------------------------------ join, smooth, unwrap
bpy.ops.object.select_all(action="DESELECT")
for o in parts:
    if o.name in bpy.data.objects: o.select_set(True)
bpy.context.view_layer.objects.active = parts[0]
bpy.ops.object.join()
alesk = bpy.context.active_object; alesk.name = "Alesk"
# the joined mesh has the five materials as slots (deduplicated)
slots = {}
for i, m in enumerate(alesk.data.materials): slots.setdefault(m.name, i)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.mesh.remove_doubles(threshold=0.0002)
bpy.ops.mesh.normals_make_consistent(inside=False)
bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.object.shade_smooth()

# put the feet on the floor and centre the model on x / y
bb = [alesk.matrix_world @ Vector(c) for c in alesk.bound_box]
lo = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb))); hi = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
alesk.location -= Vector(((lo.x + hi.x) / 2 - 0.35, (lo.y + hi.y) / 2, lo.z))
bpy.ops.object.transform_apply(location=True)
print("size m: %.2f wide, %.2f deep, %.2f tall, %d faces" % (hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, len(alesk.data.polygons)))


# ------------------------------------------------------------------------------------------------------------------------------ lights + camera (previews and baking)
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


stage()
if PREVIEW:
    os.makedirs(PREVIEW, exist_ok=True)
    render(os.path.join(PREVIEW, "alesk_front.png"), (2.2, -6.2, 2.3), (0.35, 0, 1.15))
    render(os.path.join(PREVIEW, "alesk_side.png"), (7.0, -2.5, 2.1), (0.35, 0, 1.15))
    render(os.path.join(PREVIEW, "alesk_close.png"), (1.4, -3.0, 2.1), (0.2, -0.1, 1.7), size=(720, 640), lens=70)

# ------------------------------------------------------------------------------------------------------------------------------ bake albedo + AO + emission into two textures, one material, export
if BAKE:
    bpy.ops.object.select_all(action="DESELECT"); alesk.select_set(True); bpy.context.view_layer.objects.active = alesk
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.004)
    bpy.ops.object.mode_set(mode="OBJECT")
    scene.cycles.samples = 48
    scene.render.bake.margin = 6

    def bake_to(image_name, kind, pass_filter=None):
        img = bpy.data.images.new(image_name, TEX, TEX, alpha=False)
        img.generated_color = (0.0, 0.0, 0.0, 1.0) if kind == "EMIT" else (0.5, 0.5, 0.5, 1.0)
        for m in alesk.data.materials:
            nt = m.node_tree
            n = nt.nodes.new("ShaderNodeTexImage"); n.image = img; nt.nodes.active = n
        if kind == "DIFFUSE":
            scene.render.bake.use_pass_direct = False; scene.render.bake.use_pass_indirect = False; scene.render.bake.use_pass_color = True
            bpy.ops.object.bake(type="DIFFUSE")
        elif kind == "EMIT":
            bpy.ops.object.bake(type="EMIT")
        elif kind == "AO":
            bpy.ops.object.bake(type="AO")
        for m in alesk.data.materials:
            nt = m.node_tree
            for n in [n for n in nt.nodes if n.type == "TEX_IMAGE" and n.image == img]: nt.nodes.remove(n)
        return img

    albedo = bake_to("alesk_albedo", "DIFFUSE")
    ao = bake_to("alesk_ao", "AO")
    emit = bake_to("alesk_emit", "EMIT")
    # albedo * (0.35 + 0.65 * AO): the ambient occlusion goes into the colour, like painted shading
    import numpy as np
    a = np.array(albedo.pixels[:], dtype=np.float32).reshape(-1, 4)
    o = np.array(ao.pixels[:], dtype=np.float32).reshape(-1, 4)
    shade = 0.35 + 0.65 * o[:, :1]
    a[:, :3] = np.clip(a[:, :3] * shade, 0, 1)
    albedo.pixels[:] = a.reshape(-1).tolist()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    if PREVIEW:   # keep the textures next to the previews for inspection; the GLB embeds them
        albedo.filepath_raw = os.path.join(PREVIEW, "alesk_albedo.png"); albedo.file_format = "PNG"; albedo.save()
        emit.filepath_raw = os.path.join(PREVIEW, "alesk_emit.png"); emit.file_format = "PNG"; emit.save()
    albedo.pack(); emit.pack()

    # one material for the export
    final = bpy.data.materials.new("M_Alesk"); final.use_nodes = True
    nt = final.node_tree; nt.nodes.clear()
    o_out = nt.nodes.new("ShaderNodeOutputMaterial"); b = nt.nodes.new("ShaderNodeBsdfPrincipled")
    t1 = nt.nodes.new("ShaderNodeTexImage"); t1.image = albedo
    t2 = nt.nodes.new("ShaderNodeTexImage"); t2.image = emit; t2.image.colorspace_settings.name = "sRGB"
    nt.links.new(t1.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t2.outputs["Color"], b.inputs["Emission Color"]); b.inputs["Emission Strength"].default_value = 1.0
    b.inputs["Metallic"].default_value = 0.25; b.inputs["Roughness"].default_value = 0.6
    nt.links.new(b.outputs["BSDF"], o_out.inputs["Surface"])
    alesk.data.materials.clear(); alesk.data.materials.append(final)
    if PREVIEW:
        render(os.path.join(PREVIEW, "alesk_baked.png"), (2.2, -6.2, 2.3), (0.35, 0, 1.15))
    bpy.ops.object.select_all(action="DESELECT"); alesk.select_set(True); bpy.context.view_layer.objects.active = alesk
    bpy.ops.export_scene.gltf(filepath=OUT, export_format="GLB", use_selection=True, export_yup=True, export_apply=True, export_materials="EXPORT", export_image_format="AUTO", export_cameras=False, export_lights=False)
    print("exported", OUT, os.path.getsize(OUT) // 1024, "KB")
