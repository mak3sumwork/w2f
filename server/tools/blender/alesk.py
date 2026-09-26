"""Alesk, Protector of Helios, after splash_arts/Alesk.jpg: a dark-iron golem in a wide combat stance. The game model is at most 8,000 triangles, made of big silhouette shapes (ribbed domed
pauldrons, a great helm with a peaked crest, a cracked tower shield, huge gauntlets, plated limbs); the micro detail (rivets, plate seams, ribs, carved runes, worn metal, rust) is modelled in a
high-poly SOURCE built by the same script and baked into 2048 px albedo, ambient-occlusion, normal and emission textures.

    ~/w2f_bpy/venv/bin/python tools/blender/alesk.py [--preview DIR] [--no-bake]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *

IRON = material("Iron", (0.085, 0.095, 0.11), 0.35, 0.5, kind="metal", wear=0.5)
IRON2 = material("IronLight", (0.16, 0.175, 0.2), 0.35, 0.45, kind="metal", wear=0.3)
TRIM = material("Trim", (0.30, 0.235, 0.13), 0.45, 0.4, kind="metal", wear=0.2)
UNDER = material("Under", (0.02, 0.022, 0.026), 0.0, 0.85)
RIVET = material("Rivet", (0.34, 0.27, 0.15), 0.5, 0.4)
RUNE = glow("Rune", (0.08, 1.0, 0.8))


# ---- the golem
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
        sphere("rivet%d" % i, (0.17 * math.cos(a), -0.358, 1.74 + 0.17 * math.sin(a)), (0.034, 0.022, 0.034), RIVET, seg=10, rings=8, micro=True)
    tube("chestRuneL", [(0.14, -0.36, 1.78), (0.28, -0.35, 1.84), (0.36, -0.34, 1.76), (0.33, -0.33, 1.62)], 0.011, RUNE)
    tube("chestRuneR", [(-0.14, -0.36, 1.78), (-0.28, -0.35, 1.84), (-0.36, -0.34, 1.76), (-0.33, -0.33, 1.62)], 0.011, RUNE)
    # gorget and neck
    limb("neck", (0, 0, 2.0), (0, -0.02, 1.86), 0.13, 0.17, UNDER, seg=18)
    ring("gorget", (0, -0.01, 1.98), 0.2, 0.06, IRON2, scale=(1.05, 0.9, 0.6), rot=(0, 0, 0))
    ring("gorget2", (0, -0.01, 1.9), 0.27, 0.06, IRON, scale=(1.1, 0.95, 0.55), rot=(0, 0, 0))


def head():
    """A knight's great helm: a rounded dome rising to a point, a heavy brow, a narrow T-visor with slanted glowing eyes, cheek guards, a crest fin."""
    sphere("helm", (0, -0.02, 2.19), (0.25, 0.27, 0.27), IRON2, seg=36, rings=22, low=(20, 13))
    sphere("helmTop", (0, -0.02, 2.36), (0.15, 0.18, 0.2), IRON2, seg=24, rings=14, low=(12, 7))                 # the dome rises to a peak
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
    sphere("pauld%dTop" % s, (x, 0.0, 1.95), (0.47, 0.41, 0.36), IRON2, seg=44, rings=22, cut=-0.02, low=(22, 9))
    for k in range(7):                                                                            # radial ribs: half-tori standing on the dome like the seams of a shell
        a = -90 + 30 * k
        ring("pauld%dRib%d" % (s, k), (x, 0.0, 1.95), 0.44, 0.022, TRIM if k % 3 == 1 else IRON, rot=(90, 0, a), scale=(1, 0.8, 1.0), micro=True)
    limb("pauld%dSkirt" % s, (x, 0.0, 1.72), (x, 0.0, 1.97), 0.55, 0.46, IRON, seg=44, caps=False)   # the flared skirt: an open cone under the dome
    ring("pauld%dSkirtEdge" % s, (x, 0.0, 1.73), 0.55, 0.03, TRIM, scale=(1, 0.88, 0.6))
    ring("pauld%dSkirtEdge2" % s, (x, 0.0, 1.85), 0.5, 0.02, IRON2, scale=(1, 0.88, 0.6), micro=True)
    box("pauld%dDisc" % s, (x, -0.44, 1.98), (0.34, 0.06, 0.34), TRIM, rot=(-14, 0, 0), bevel=0.02)   # the rune disc on the front
    ring("pauld%dDiscRune" % s, (x, -0.482, 1.985), 0.125, 0.012, RUNE, rot=(76, 0, 0))
    tube("pauld%dGlyph" % s, [(x - 0.05, -0.49, 1.93), (x, -0.495, 2.05), (x + 0.05, -0.49, 1.93)], 0.009, RUNE)
    for k in range(7):
        a = math.radians(-100 + 200 * k / 6)
        sphere("pRivet%d_%d" % (s, k), (x + 0.5 * math.sin(a) * s * 0.9, -0.44 * math.cos(a) - 0.02, 1.76), (0.042, 0.042, 0.042), RIVET, seg=10, rings=8, micro=True)
    sphere("pauld%dSpike" % s, (x * 1.12, 0.0, 2.34), (0.07, 0.07, 0.21), TRIM, seg=10, rings=8)
    sphere("pauld%dSpike2" % s, (x * 0.94, -0.12, 2.31), (0.05, 0.05, 0.12), TRIM, seg=10, rings=8, micro=True)


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
            sphere("knuckle%s%d" % (name, k), (wr[0] + (k - 1.5) * 0.095, wr[1] - 0.16, wr[2] + 0.1), (0.04, 0.04, 0.04), RIVET, seg=8, rings=6, micro=True)
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
        finish(o, mat, 0.018, 1, keep=True)
        bpy.context.view_layer.objects.active = o
        mod = o.modifiers.new("Bend", "SIMPLE_DEFORM"); mod.deform_method = "BEND"; mod.deform_axis = "Z"; mod.angle = math.radians(38)
        bpy.ops.object.modifier_apply(modifier=mod.name)
        o.location = (cx, cy, 1.0); o.rotation_euler = Euler((0, 0, math.radians(ang)))
    ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
    def plate_out(dx, dz):
        """How far the shield's front surface stands in front of its centre plane at (dx, dz): the plate where the raised plate is, else the body; both bent by the 38 degree bend."""
        inside = abs(dx) < 0.72 * 0.46 * (1.0 - 0.22 * max(0.0, -dz)) and -0.71 < dz < 0.69
        return (0.088 - 0.49 * dx * dx) if inside else (0.049 - 0.42 * dx * dx)
    def P(dx, dz, lift=0.0):
        """A point on the shield's front surface, `lift` in front of it (the shield is tilted by `ang` and faces -Y)."""
        out = plate_out(dx, dz) + lift
        return (cx + ca * dx + sa * out, cy + sa * dx - ca * out, 1.0 + dz)
    box("shieldDiamond", P(0, 0.12, 0.022), (0.34, 0.05, 0.34), TRIM, rot=(0, 45, ang), bevel=0.02, subsurf=0)
    box("shieldCore", P(0, 0.12, 0.046), (0.21, 0.03, 0.21), RUNE, rot=(0, 45, ang), bevel=0.01, subsurf=0)
    for i, (dx, dz) in enumerate(((-0.3, 0.8), (0.3, 0.8), (-0.32, 0.4), (0.32, 0.4), (-0.28, -0.3), (0.28, -0.3), (0, -0.66), (0, 0.84))):
        sphere("bolt%d" % i, P(dx, dz, 0.0), (0.06, 0.04, 0.06), RIVET, seg=10, rings=8, micro=True)
    L = 0.006                                                                          # the cracks sit on the surface, half sunk
    tube("shieldCrack1", [P(0.08, 0.32, L), P(0.17, 0.52, L), P(0.11, 0.66, L), P(0.2, 0.86, L)], 0.011, RUNE)
    tube("shieldCrack2", [P(-0.06, -0.06, L), P(-0.2, -0.26, L), P(-0.13, -0.5, L), P(-0.16, -0.66, L)], 0.009, RUNE)
    tube("shieldCrack3", [P(0.14, -0.22, L), P(0.26, -0.34, L), P(0.22, -0.52, L)], 0.008, RUNE)
    tube("shieldRune", [P(-0.22, 0.6, L), P(-0.11, 0.68, L), P(0.0, 0.6, L), P(0.11, 0.68, L), P(0.22, 0.6, L)], 0.008, RUNE)
    box("shieldGrip", (cx - 0.02 * ca, cy + 0.1, 1.06), (0.07, 0.07, 0.36), UNDER, rot=(0, 0, ang), bevel=0.01, subsurf=0)




def build():
    legs(); pelvis(); torso(); head(); shoulders_and_arms(); shield()


finalize_lod("Alesk", "SM_Champion_9001_Alesk.glb", build, budget=8000)
