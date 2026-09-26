"""Baira, the Tidecaller: a sea sorceress after splash_arts/Baira.jpg, feminine in the proportions of a TFT champion (a larger head with big eyes, an hourglass torso, long flowing hair). Light blue skin, a
long tail of teal-to-violet scales coiling behind her, a shell bra and hip plates in bronze and teal, pink fins for ears and shoulders, a coral trident staff crowned with a glowing orb in her raised right hand,
and a wave orb swirling in her left. Human height: about 1.9 m without the staff.

The game model is at most 8,000 triangles (big silhouette shapes: the coiling tail, fins, the hair masses, the lofted torso, shell armour, staff, orbs); the fine detail (the face, single hair strands, scales,
shell trim, glowing patterns) is baked from a high-poly source built by the same script into 2048 px albedo / AO / normal / emission textures. The head and hair get extra texture density (`uv_boost`).

    ~/w2f_bpy/venv/bin/python tools/blender/baira.py [--preview DIR] [--no-bake]
"""
import os, sys, math, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *
from mathutils import Matrix

SKIN = material("Skin", (0.42, 0.62, 0.92), 0.0, 0.5, kind="skin")
SCALE = material("Scale", (0.05, 0.42, 0.55), 0.15, 0.4, kind="scales", color2=(0.38, 0.16, 0.6))
FIN = material("Fin", (0.85, 0.22, 0.72), 0.0, 0.5, kind="cloth", color2=(0.2, 0.7, 0.95))
GOLD = material("Gold", (0.62, 0.46, 0.16), 0.5, 0.35, kind="metal", wear=0.15)
SHELL = material("Shell", (0.06, 0.3, 0.38), 0.3, 0.4, kind="metal", wear=0.1, color2=(0.2, 0.6, 0.65))
HAIR = material("Hair", (0.045, 0.075, 0.17), 0.0, 0.4)
HAIR2 = material("HairLight", (0.08, 0.24, 0.36), 0.0, 0.4)
CORAL = material("Coral", (0.78, 0.22, 0.26), 0.0, 0.55, kind="bark", color2=(0.95, 0.5, 0.45))
STAFF = material("Staff", (0.08, 0.1, 0.2), 0.3, 0.45, kind="metal", wear=0.2)
LIPS = material("Lips", (0.62, 0.22, 0.42), 0.0, 0.35)
SCLERA = material("Sclera", (0.88, 0.96, 0.98), 0.0, 0.35)
IRIS = material("Iris", (0.1, 0.72, 0.88), 0.0, 0.25)
BLUSH = material("Blush", (0.5, 0.5, 0.7), 0.0, 0.5)
LASH = material("Lash", (0.02, 0.03, 0.08), 0.0, 0.5)
WATER = glow("Water", (0.1, 0.5, 0.9))
ORB = glow("Orb", (0.1, 0.45, 0.85))
PEARL = glow("Pearl", (0.6, 0.9, 1.0))
DARK = material("Dark", (0.02, 0.03, 0.05), 0.0, 0.8)

# ---- proportions: torso sections (z, half width, half depth, centre y) from the hips up to the neck: an hourglass with a bust
TORSO = [(1.08, 0.165, 0.12, 0.0), (1.17, 0.152, 0.11, 0.0), (1.25, 0.122, 0.09, 0.0), (1.31, 0.112, 0.085, 0.0), (1.38, 0.122, 0.095, 0.0), (1.46, 0.135, 0.10, -0.005),
         (1.53, 0.145, 0.09, 0.0), (1.585, 0.15, 0.08, 0.0), (1.615, 0.10, 0.068, -0.005), (1.66, 0.052, 0.05, -0.008), (1.735, 0.047, 0.047, -0.014)]
HC = (0.0, -0.025, 1.86)                     # head centre
HR = (0.112, 0.122, 0.145)                   # head radii
JC, JR = (0.0, -0.058, 1.773), (0.06, 0.076, 0.058)     # jaw / chin


def torso_at(z):
    """(half width, half depth, centre y) of the torso at height z."""
    for a, b in zip(TORSO[:-1], TORSO[1:]):
        if a[0] <= z <= b[0]:
            t = (z - a[0]) / (b[0] - a[0]); return tuple(a[k] * (1 - t) + b[k] * t for k in (1, 2, 3))
    return TORSO[-1][1:]


def torso_pt(z, ang, s=1, lift=0.0):
    """A point on the torso surface at height z, `ang` degrees round from the front toward the side s (+1 = her left), `lift` out of the skin."""
    rx, ry, cy = torso_at(z); a = math.radians(ang)
    return (s * (rx + lift) * math.sin(a), cy - (ry + lift) * math.cos(a), z)


def face_y(x, z):
    """The front of the face (y) at (x, z): the head sphere or the jaw sphere, whichever sticks out further."""
    ys = []
    for c, r in ((HC, HR), (JC, JR)):
        t = 1 - ((x - c[0]) / r[0]) ** 2 - ((z - c[2]) / r[2]) ** 2
        if t > 0: ys.append(c[1] - r[1] * math.sqrt(t))
    return min(ys) if ys else HC[1] - HR[1] * 0.15


def transform_about(objs, pivot, local_matrix):
    """Apply a local-space transform (already-built parts) about a pivot: `Matrix.Scale`, `Matrix.Rotation`, or a product of both."""
    M = Matrix.Translation(Vector(pivot)) @ local_matrix @ Matrix.Translation(-Vector(pivot))
    for o in objs:
        o.matrix_world = M @ o.matrix_world
        bpy.ops.object.select_all(action="DESELECT"); o.select_set(True); bpy.context.view_layer.objects.active = o
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def scale_about(objs, pivot, k):
    """Scale already built parts about `pivot` (the head grows a size: TFT champions have big heads)."""
    transform_about(objs, pivot, Matrix.Scale(k, 4))


def lean_about(objs, pivot, deg, axis="X"):
    """Rotate already built parts about `pivot` by `deg`: the arched, chest-forward 'swimming' pose (after Nami's), applied to everything from the hips up."""
    transform_about(objs, pivot, Matrix.Rotation(math.radians(deg), 4, axis))


HEAD_SCALE, HEAD_PIVOT = 1.25, (0.0, -0.01, 1.72)
HIP_PIVOT = (0.0, 0.0, 1.08)
LEAN_DEG = 13          # the torso/head/arms arch forward and up from the hips, like a mermaid gliding
HEAD_COUNTER_DEG = -5  # the head leans back a little less far, so she looks forward instead of down


def hair_lock(name, pts, radii, mat=None, strands=9, strand_r=0.0065, low_u=4, low_res=2):
    """A thick lock of hair (in the game model too) with `strands` thin ridges around it (only in the baked source)."""
    mat = mat or HAIR
    swoop(name, pts, radii, mat, res=8, low_u=low_u, low_res=low_res)
    rnd = random.Random(sum(ord(c) for c in name))
    for k in range(strands):
        a = 2 * math.pi * k / strands + rnd.uniform(-0.2, 0.2)
        sp = [(p[0] + math.cos(a) * r * 0.88, p[1] + math.sin(a) * r * 0.88, p[2]) for p, r in zip(pts, radii)]
        swoop("%sStrand%d" % (name, k), sp, [strand_r * (0.4 if i == len(sp) - 1 else 1.0) for i in range(len(sp))], HAIR2 if k % 3 == 0 else mat, res=4, micro=True)


def build():
    # ------------------------------------------------------------- the tail: an S that coils on the floor behind her, ending in a tall fin
    tail_pts = [(0, 0.02, 1.12), (0, 0.0, 0.85), (0.02, 0.06, 0.55), (0.12, 0.25, 0.28), (0.32, 0.55, 0.16), (0.3, 0.95, 0.2), (0.08, 1.25, 0.38), (-0.12, 1.4, 0.62), (-0.2, 1.42, 0.86)]
    tail_r = [0.165, 0.165, 0.16, 0.145, 0.13, 0.105, 0.078, 0.052, 0.028]
    swoop("tail", tail_pts, tail_r, SCALE, res=14, low_u=5, low_res=3)
    ribbon("dorsal", [(0, 0.16, 0.95), (0.02, 0.24, 0.6), (0.14, 0.4, 0.3), (0.32, 0.66, 0.22), (0.3, 1.0, 0.26), (0.1, 1.28, 0.44)], [0.3, 0.42, 0.42, 0.36, 0.28, 0.2], FIN, up=(0, 1, 0.3), thick=0.008, wave=0.02, subsurf=0)
    for s in (1, -1):
        ribbon("fluke%d" % s, [(-0.2, 1.42, 0.86), (-0.2 + 0.13 * s, 1.44, 1.0), (-0.2 + 0.26 * s, 1.44, 1.13), (-0.2 + 0.36 * s, 1.42, 1.2)], [0.13, 0.22, 0.23, 0.16], FIN, up=(0, 1, 0), thick=0.008, wave=0.01, subsurf=0)
        ribbon("flukeLow%d" % s, [(-0.2, 1.42, 0.86), (-0.2 + 0.14 * s, 1.44, 0.74), (-0.2 + 0.26 * s, 1.44, 0.64)], [0.12, 0.18, 0.12], FIN, up=(0, 1, 0), thick=0.008, subsurf=0)
        ribbon("hipFin%d" % s, [(0.14 * s, 0.02, 1.08), (0.32 * s, 0.14, 0.9), (0.44 * s, 0.32, 0.66), (0.4 * s, 0.5, 0.42)], [0.2, 0.26, 0.24, 0.14], FIN, up=(0, 0, 1), thick=0.008, wave=0.02, subsurf=0)

    lean_start = len(parts)
    # ------------------------------------------------------------- torso: a lofted hourglass, a real bust under shell cups, a golden band and sash
    loft("torso", [(z, rx, ry, 0.0, cy) for z, rx, ry, cy in TORSO], SKIN, seg=32, low=16)
    for s in (1, -1):
        c = (0.068 * s, -0.09, 1.455)
        sphere("bustSkin%d" % s, c, (0.074, 0.07, 0.076), SKIN, seg=24, rings=16, low=(10, 8))
        sphere("bustCup%d" % s, (c[0] + 0.004 * s, c[1] - 0.004, c[2] - 0.006), (0.082, 0.078, 0.084), SHELL, rot=(112, 0, 0), cut=0.28, seg=24, rings=16, low=(14, 9))
    loft("bustBand", [(1.372, 0.131, 0.104, 0.0, 0.0), (1.4, 0.133, 0.106, 0.0, 0.0)], GOLD, seg=32, low=16, subsurf=0, caps=False)
    sphere("bustPearl", (0, -0.148, 1.462), (0.022, 0.018, 0.022), PEARL, seg=14, rings=10, micro=True)
    loft("sash", [(1.262, 0.117, 0.093, 0.0, 0.0), (1.292, 0.118, 0.094, 0.0, 0.0)], GOLD, seg=32, low=16, subsurf=0, caps=False)
    for k, dz in enumerate((1.19, 1.13, 1.07)):                                                       # shell plates over the front of the hips
        box("plate%d" % k, (0, -0.125, dz), (0.27 - 0.025 * k, 0.03, 0.085), SHELL if k % 2 == 0 else GOLD, rot=(-8, 0, 0), bevel=0.012, subsurf=1)
    for s in (1, -1):
        box("hipShell%d" % s, (0.185 * s, -0.05, 1.13), (0.09, 0.1, 0.15), SHELL, rot=(0, 0, -20 * s), bevel=0.015, subsurf=1)
        tube("waistGlow%d" % s, [torso_pt(1.2, 55, s, 0.004), torso_pt(1.27, 70, s, 0.004), torso_pt(1.34, 82, s, 0.004), torso_pt(1.42, 88, s, 0.004)], 0.0055, WATER)
        tube("ribGlow%d" % s, [torso_pt(1.5, 42, s, 0.004), torso_pt(1.54, 62, s, 0.004), torso_pt(1.56, 78, s, 0.004)], 0.005, WATER)
    tube("throatGlow", [(-0.03, -0.042, 1.66), (0, -0.05, 1.63), (0.03, -0.042, 1.66)], 0.005, WATER)

    # ------------------------------------------------------------- head: a heart-shaped face with big eyes, a small nose, full lips
    head_start = len(parts)
    sphere("head", HC, HR, SKIN, seg=32, rings=20, low=(22, 15))
    sphere("jaw", JC, JR, SKIN, seg=20, rings=12, low=(12, 8))
    for s in (1, -1):
        ex, ez = 0.05 * s, 1.848
        ey = face_y(ex, ez)
        sphere("eyeWhite%d" % s, (ex, ey + 0.001, ez), (0.027, 0.0075, 0.019), SCLERA, rot=(0, -10 * s, 0), seg=16, rings=10, micro=True)
        sphere("iris%d" % s, (ex - 0.002 * s, ey - 0.0045, ez - 0.001), (0.0175, 0.0065, 0.0175), IRIS, seg=16, rings=10, micro=True)
        sphere("pupil%d" % s, (ex - 0.002 * s, ey - 0.008, ez - 0.001), (0.008, 0.004, 0.009), LASH, seg=10, rings=8, micro=True)
        sphere("glint%d" % s, (ex + 0.006 * s, ey - 0.0105, ez + 0.007), (0.0045, 0.002, 0.0045), SCLERA, seg=8, rings=6, micro=True)
        lash = [(ex - 0.028 * s, ez - 0.003), (ex - 0.02 * s, ez + 0.014), (ex + 0.0 * s, ez + 0.02), (ex + 0.022 * s, ez + 0.016), (ex + 0.034 * s, ez + 0.02), (ex + 0.042 * s, ez + 0.03)]
        swoop("lash%d" % s, [(x, face_y(x, z) - 0.004, z) for x, z in lash], [0.0032, 0.0048, 0.0052, 0.0048, 0.0036, 0.0016], LASH, res=4, micro=True)
        brow = [(0.024 * s, 1.895), (0.05 * s, 1.912), (0.078 * s, 1.905), (0.098 * s, 1.885)]
        swoop("brow%d" % s, [(x, face_y(x, z) - 0.003, z) for x, z in brow], [0.004, 0.0055, 0.005, 0.0025], HAIR, res=4, micro=True)
    sphere("nose", (0, face_y(0, 1.803) - 0.007, 1.803), (0.0125, 0.017, 0.0135), SKIN, seg=14, rings=10, micro=True)
    sphere("noseBridge", (0, face_y(0, 1.83) - 0.002, 1.83), (0.008, 0.008, 0.024), SKIN, seg=10, rings=8, micro=True)
    sphere("lipUpper", (0, face_y(0, 1.762) - 0.002, 1.764), (0.024, 0.0075, 0.0072), LIPS, seg=14, rings=8, micro=True)
    sphere("lipLower", (0, face_y(0, 1.746) - 0.002, 1.747), (0.021, 0.0085, 0.0088), LIPS, seg=14, rings=8, micro=True)
    swoop("mouthLine", [(x, face_y(x, 1.755) - 0.0055, 1.755 + (0.002 if abs(x) > 0.015 else 0)) for x in (-0.026, -0.013, 0.0, 0.013, 0.026)], [0.0018, 0.0026, 0.003, 0.0026, 0.0018], LASH, res=4, micro=True)
    for s in (1, -1):                                                                                # pink fin ears
        ribbon("finEar%d" % s, [(0.108 * s, -0.02, 1.86), (0.17 * s, 0.0, 1.9), (0.22 * s, 0.03, 1.99)], [0.06, 0.075, 0.03], FIN, up=(0, 1, 0), thick=0.005, subsurf=0)
    for k, (dx, h_) in enumerate(((-0.06, 0.09), (0.0, 0.13), (0.06, 0.09))):                       # a shell tiara
        swoop("crown%d" % k, [(dx, -0.1, 1.965), (dx * 1.25, -0.1, 1.965 + h_ * 0.6), (dx * 1.6, -0.1, 1.965 + h_)], [0.016, 0.012, 0.004], GOLD, res=6, low_u=2, low_res=2, low_scale=1.5)

    # ------------------------------------------------------------- hair: a cap, a fringe, two locks over the shoulders and three long waves down her back
    cap_c, cap_r = (0.0, 0.004, 1.89), (0.13, 0.136, 0.142)
    sphere("hairCap", cap_c, cap_r, HAIR, seg=32, rings=20, low=(16, 11))
    rnd = random.Random(7)
    for k in range(26):                                                                              # single strands combed from the crown down the cap (baked only)
        b = 0.35 + (2 * math.pi - 0.7) * k / 25.0
        pts = []
        for a in (0.12, 0.45, 0.8, 1.1, 1.3):
            if abs(b - math.pi) < 0.7 and a > 0.75: continue                                        # keep the face clear
            pts.append((cap_c[0] + (cap_r[0] + 0.004) * math.sin(a) * math.sin(b), cap_c[1] + (cap_r[1] + 0.004) * math.sin(a) * math.cos(b), cap_c[2] + (cap_r[2] + 0.004) * math.cos(a)))
        if len(pts) >= 3: swoop("capStrand%d" % k, pts, [0.0065, 0.0075, 0.0075, 0.006, 0.003][:len(pts)], HAIR2 if k % 3 == 0 else HAIR, res=4, micro=True)
    for s in (1, -1):
        hair_lock("bang%d" % s, [(0.005 * s, -0.112, 1.99), (0.05 * s, -0.135, 1.965), (0.095 * s, -0.128, 1.92), (0.118 * s, -0.09, 1.87)], [0.026, 0.03, 0.028, 0.016], strands=6, low_u=3)
    scale_about(parts[head_start:], HEAD_PIVOT, HEAD_SCALE)                                          # the whole head assembly, bigger
    for s in (1, -1):
        hair_lock("lockFront%d" % s, [(0.175 * s, -0.01, 1.94), (0.245 * s, -0.04, 1.76), (0.26 * s, -0.07, 1.6), (0.23 * s, -0.07, 1.44), (0.19 * s, -0.045, 1.30)], [0.034, 0.046, 0.042, 0.034, 0.009], strands=8, low_u=3)
    for i, (x, w) in enumerate(((-0.085, 0.085), (0.085, 0.085), (0.0, 0.095))):
        hair_lock("lockBack%d" % i, [(x, 0.15, 2.0), (x * 1.5, 0.21, 1.84), (x * 1.9 + 0.03, 0.25, 1.56), (x * 1.6 - 0.03, 0.265, 1.3), (x * 1.9, 0.25, 1.05)], [w * 0.8, w, w * 0.95, w * 0.75, 0.02], strands=10, low_u=3)

    # ------------------------------------------------------------- shoulders and arms (slim)
    for s in (1, -1):
        sphere("shoulder%d" % s, (0.178 * s, 0.0, 1.572), (0.055, 0.055, 0.055), SKIN, seg=20, rings=14, low=(10, 7))
        ribbon("finShoulder%d" % s, [(0.19 * s, 0.0, 1.62), (0.28 * s, -0.02, 1.72), (0.35 * s, -0.02, 1.84)], [0.08, 0.1, 0.05], FIN, up=(0, 1, 0), thick=0.008, subsurf=0)
        sphere("padShell%d" % s, (0.2 * s, -0.005, 1.6), (0.09, 0.08, 0.06), SHELL, seg=20, rings=10, cut=-0.1, low=(14, 8))
    r_sh, r_el, r_wr = (-0.18, 0.0, 1.56), (-0.34, -0.06, 1.7), (-0.41, -0.14, 1.86)
    limb("upperArmR", r_sh, r_el, 0.046, 0.038, SKIN, seg=16, low=10); limb("foreArmR", r_el, r_wr, 0.038, 0.031, SKIN, seg=16, low=10)
    sphere("handR", (-0.41, -0.15, 1.88), (0.048, 0.054, 0.054), SKIN, seg=16, rings=12, low=(8, 6))
    box("bracerR", (-0.395, -0.11, 1.85), (0.045, 0.045, 0.09), GOLD, rot=(0, 0, 10), bevel=0.01, subsurf=1)
    l_sh, l_el, l_wr = (0.18, 0.0, 1.56), (0.37, -0.13, 1.42), (0.46, -0.4, 1.46)
    limb("upperArmL", l_sh, l_el, 0.046, 0.038, SKIN, seg=16, low=10); limb("foreArmL", l_el, l_wr, 0.038, 0.03, SKIN, seg=16, low=10)
    sphere("handL", (0.48, -0.46, 1.47), (0.054, 0.06, 0.03), SKIN, seg=16, rings=12, rot=(20, 0, 0), low=(8, 6))
    for k in range(4): swoop("finger%d" % k, [(0.46 + (k - 1.5) * 0.02, -0.48, 1.485), (0.46 + (k - 1.5) * 0.028, -0.53, 1.5), (0.46 + (k - 1.5) * 0.03, -0.56, 1.5)], [0.008, 0.007, 0.004], SKIN, res=4, low_u=2, low_res=2, low_scale=1.8)
    box("bracerL", (0.42, -0.27, 1.44), (0.045, 0.09, 0.045), GOLD, bevel=0.01, subsurf=1)
    sphere("waveOrb", (0.49, -0.6, 1.53), (0.11, 0.11, 0.11), ORB, seg=24, rings=14, low=(14, 9))
    for k in range(3): ring("waveRing%d" % k, (0.49, -0.6, 1.53), 0.2 + 0.04 * k, 0.006, WATER, rot=(20 * k + 40, 30 * k, 15 * k), seg=36, tseg=6, micro=(k == 2))
    tube("waveSwirl", [(0.3, -0.5, 1.2), (0.42, -0.62, 1.3), (0.6, -0.68, 1.42), (0.66, -0.5, 1.58), (0.56, -0.4, 1.66)], 0.012, WATER)

    # ------------------------------------------------------------- the coral trident staff
    limb("staffPole", (-0.42, -0.15, 0.85), (-0.42, -0.15, 2.05), 0.03, 0.024, STAFF, seg=14, low=8)
    for z in (1.05, 1.35, 1.65, 1.9): ring("staffBand%d" % int(z * 10), (-0.42, -0.15, z), 0.034, 0.009, GOLD, seg=14, tseg=6, micro=True)
    prongs = [((-0.42, -0.15, 2.0), (-0.5, -0.15, 2.12), (-0.62, -0.15, 2.3), (-0.58, -0.15, 2.5)),
              ((-0.42, -0.15, 2.0), (-0.42, -0.15, 2.2), (-0.42, -0.15, 2.4), (-0.42, -0.15, 2.68)),
              ((-0.42, -0.15, 2.0), (-0.34, -0.15, 2.12), (-0.22, -0.15, 2.3), (-0.26, -0.15, 2.5)),
              ((-0.42, -0.15, 2.05), (-0.46, -0.08, 2.2), (-0.5, 0.0, 2.38), (-0.48, 0.04, 2.55)),
              ((-0.42, -0.15, 2.05), (-0.38, -0.22, 2.2), (-0.34, -0.3, 2.38), (-0.36, -0.34, 2.52))]
    for k, pts in enumerate(prongs):
        swoop("coral%d" % k, list(pts), [0.026, 0.022, 0.016, 0.006], CORAL, res=8, low_u=3, low_res=2, low_scale=1.3)
        mid = pts[2]
        swoop("coralTwig%da" % k, [mid, (mid[0] - 0.07, mid[1], mid[2] + 0.08), (mid[0] - 0.11, mid[1], mid[2] + 0.2)], [0.014, 0.01, 0.004], CORAL, res=6, micro=(k != 1), low_u=3, low_res=2, low_scale=1.5)
        swoop("coralTwig%db" % k, [mid, (mid[0] + 0.06, mid[1], mid[2] + 0.1), (mid[0] + 0.09, mid[1], mid[2] + 0.22)], [0.014, 0.01, 0.004], CORAL, res=6, micro=(k != 1), low_u=3, low_res=2, low_scale=1.5)
    sphere("staffOrb", (-0.42, -0.15, 2.28), (0.085, 0.085, 0.085), ORB, seg=24, rings=14, low=(14, 9))
    for k in range(2): ring("orbRing%d" % k, (-0.42, -0.15, 2.28), 0.17 + 0.03 * k, 0.005, WATER, rot=(70 * k + 30, 20 * k, 40 * k), seg=36, tseg=6, micro=(k == 1))

    lean_about(parts[lean_start:], HIP_PIVOT, LEAN_DEG)                                          # arch the torso/arms/staff forward and up together
    lean_about(parts[head_start:], (0.0, -0.01, 1.68), HEAD_COUNTER_DEG)                          # tilt the head back up so she looks ahead, not down

    # ------------------------------------------------------------- water swirling round the tail (only in the source)
    tube("swirl1", [(0.34, -0.1, 0.4), (0.3, 0.2, 0.55), (-0.1, 0.5, 0.42), (-0.4, 0.3, 0.5), (-0.44, -0.05, 0.7), (-0.2, -0.3, 0.6)], 0.014, WATER)
    tube("swirl2", [(-0.3, -0.2, 0.3), (-0.1, -0.32, 0.5), (0.25, -0.26, 0.42), (0.42, 0.0, 0.62), (0.3, 0.3, 0.9)], 0.011, WATER)


H = 2.62   # the model's height, for the preview cameras
finalize_lod("Baira", "SM_Champion_9002_Baira.glb", build, budget=8000, ao_strength=0.35,
             uv_boost={"head": 2.2, "jaw": 2.2, "hairCap": 1.5, "bang": 1.5, "lockFront": 1.2, "torso": 1.4, "bustSkin": 1.5, "bustCup": 1.4},
             extra_views=[("face", (0.06, -1.5, 1.87), (0.0, -0.33, 1.85), (900, 900), 50)])
