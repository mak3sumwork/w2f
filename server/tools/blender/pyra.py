"""Pyra, the archer of the burning bow, after splash_arts/Pyra.jpg: a fierce, broad-shouldered woman in a wide lunge, leather-and-bronze armour, a tan cape, a long braid, boots with knee guards,
drawing a great bow of living flame. Human height (about 1.85 m).

The game model is at most 8,000 triangles (big silhouette shapes: the lunge legs, the pteruge skirt, the torso and pauldron, the braid, the cape, the bow and its main flame plumes); the fine detail
(rivets-equivalent bracer laces, braid ties, pauldron rings, the inner/side flame licks, sparks-adjacent glow) is baked from a high-poly source built by the same script into 2048 px albedo / AO /
normal / emission textures.

    ~/w2f_bpy/venv/bin/python tools/blender/pyra.py [--preview DIR] [--no-bake]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *

SKIN = material("Skin", (0.42, 0.24, 0.13), 0.0, 0.55, kind="skin")
LEATHER = material("Leather", (0.22, 0.11, 0.055), 0.0, 0.6, kind="leather")
LEATHER2 = material("LeatherLight", (0.36, 0.2, 0.1), 0.0, 0.6, kind="leather")
BRONZE = material("Bronze", (0.42, 0.27, 0.1), 0.5, 0.4, kind="metal", wear=0.3)
STEEL = material("Steel", (0.15, 0.15, 0.17), 0.4, 0.45, kind="metal", wear=0.4)
CAPE = material("Cape", (0.55, 0.42, 0.26), 0.0, 0.8, kind="cloth")
HAIR = material("Hair", (0.03, 0.02, 0.018), 0.0, 0.5)
FIRE = glow("Fire", (1.0, 0.42, 0.06))
FIRE2 = glow("FireYellow", (1.0, 0.82, 0.25))
EYE = glow("Eye", (1.0, 0.7, 0.15))
DARK = material("Dark", (0.02, 0.018, 0.016), 0.0, 0.8)
FEATHER = material("Feather", (0.6, 0.55, 0.45), 0.0, 0.7, kind="cloth")


# ---------------------------------------------------------------- legs: a wide lunge, the right leg (x < 0) forward and bent, the left leg (x > 0) stretched behind
def leg(hip, knee, ankle, s):
    limb("thigh%d" % s, hip, knee, 0.105, 0.085, SKIN, seg=20, low=10)
    limb("thighArmor%d" % s, hip, knee, 0.11, 0.09, LEATHER, seg=20, low=10, caps=False)
    sphere("kneeGuard%d" % s, (knee[0], knee[1] - 0.06, knee[2]), (0.085, 0.06, 0.09), BRONZE, seg=20, rings=12, low=(12, 8))
    limb("shin%d" % s, knee, ankle, 0.085, 0.06, LEATHER, seg=20, low=10)
    limb("greave%d" % s, (knee[0], knee[1] - 0.02, knee[2] - 0.05), (ankle[0], ankle[1] - 0.03, ankle[2] + 0.1), 0.09, 0.07, BRONZE, seg=20, low=10, caps=False)
    box("boot%d" % s, (ankle[0], ankle[1] - 0.09, 0.06), (0.11, 0.3, 0.12), LEATHER, bevel=0.03, subsurf=1)
    box("bootSole%d" % s, (ankle[0], ankle[1] - 0.09, 0.012), (0.12, 0.31, 0.025), DARK, bevel=0.008, subsurf=0)
    for k in range(3):
        ring("strap%d_%d" % (s, k), (knee[0] * (1 - 0.3 * (k + 1) / 3) + ankle[0] * 0.3 * (k + 1) / 3, knee[1] * (1 - 0.3 * (k + 1) / 3) + ankle[1] * 0.3 * (k + 1) / 3,
             knee[2] * (1 - 0.35 * (k + 1) / 3) + ankle[2] * 0.35 * (k + 1) / 3), 0.085, 0.008, LEATHER2, rot=(90, 0, 0), seg=18, tseg=6, micro=True)


def legs():
    leg((-0.13, 0.0, 0.95), (-0.2, -0.42, 0.58), (-0.2, -0.36, 0.09), -1)
    leg((0.13, 0.05, 0.95), (0.3, 0.42, 0.62), (0.44, 0.78, 0.09), 1)
    for s in (-1, 1): sphere("hip%d" % s, (0.13 * s, 0.02, 0.96), (0.11, 0.11, 0.11), SKIN, seg=18, rings=12, low=(10, 7))


# ---------------------------------------------------------------- pteruges: leather strips round the hips, a belt (the skirt silhouette; kept in the low model)
def hips():
    box("belt", (0, 0.0, 1.03), (0.4, 0.26, 0.07), LEATHER2, bevel=0.015, subsurf=1)
    box("buckle", (0, -0.14, 1.03), (0.07, 0.02, 0.06), BRONZE, bevel=0.005, subsurf=0)
    for k in range(11):
        a = math.radians(-190 + 200 * k / 10)
        ribbon("strip%d" % k, [(0.22 * math.sin(a), 0.16 * math.cos(a), 1.0), (0.27 * math.sin(a), 0.2 * math.cos(a), 0.88), (0.28 * math.sin(a), 0.21 * math.cos(a), 0.75)],
               [0.07, 0.075, 0.06], LEATHER if k % 2 else LEATHER2, up=(0, 0, 1), thick=0.008, subsurf=0)


# ---------------------------------------------------------------- torso: strong, twisted into the draw
def torso():
    limb("abdomen", (0, 0, 1.03), (0, 0.0, 1.28), 0.16, 0.16, SKIN, seg=28, low=14)
    sphere("chestBack", (0, 0.02, 1.42), (0.22, 0.14, 0.2), SKIN, seg=28, rings=16, rot=(0, 0, 12), low=(16, 9))
    box("chestArmor", (0, -0.1, 1.38), (0.34, 0.1, 0.24), LEATHER, rot=(-6, 0, 0), bevel=0.03, subsurf=1)
    box("chestPlate", (0, -0.16, 1.4), (0.2, 0.04, 0.18), BRONZE, rot=(-6, 0, 0), bevel=0.012, subsurf=1)
    for k in range(3): box("ab%d" % k, (0, -0.135, 1.2 + 0.05 * k), (0.28 - 0.02 * k, 0.03, 0.045), LEATHER2, bevel=0.01, subsurf=0)
    tube("chestGlow", [(-0.05, -0.185, 1.42), (0, -0.19, 1.46), (0.05, -0.185, 1.42)], 0.006, FIRE)
    limb("neck", (0, 0.0, 1.55), (0, -0.02, 1.65), 0.07, 0.06, SKIN, seg=16, low=9)
    sphere("padL", (0.25, 0.0, 1.55), (0.12, 0.11, 0.09), STEEL, seg=24, rings=12, cut=-0.2, low=(14, 8))       # a steel pauldron on the bow arm
    for k in range(3): ring("padRing%d" % k, (0.25, 0.0, 1.52 - 0.04 * k), 0.11 - 0.005 * k, 0.012, BRONZE, scale=(1, 0.95, 0.6), micro=True)
    sphere("shoulderR", (-0.23, 0.0, 1.53), (0.075, 0.075, 0.075), SKIN, seg=16, rings=10, low=(9, 6))
    box("padR", (-0.24, 0.0, 1.58), (0.14, 0.12, 0.06), LEATHER2, rot=(0, 0, -20), bevel=0.02, subsurf=1)


# ---------------------------------------------------------------- head: a determined snarl, a braid
def head_and_hair():
    sphere("head", (0, -0.04, 1.76), (0.1, 0.11, 0.125), SKIN, seg=28, rings=18, low=(16, 10))
    sphere("jaw", (0, -0.075, 1.69), (0.08, 0.08, 0.06), SKIN, seg=16, rings=10, low=(10, 7))
    for s in (1, -1):
        box("eye%d" % s, (0.04 * s, -0.145, 1.775), (0.036, 0.01, 0.014), EYE, rot=(0, 0, -16 * s), bevel=0.003, subsurf=0)
        swoop("brow%d" % s, [(0.012 * s, -0.148, 1.8), (0.045 * s, -0.146, 1.79), (0.075 * s, -0.135, 1.775)], [0.007, 0.007, 0.004], DARK, res=4, micro=True)
    box("mouth", (0, -0.15, 1.7), (0.05, 0.01, 0.016), DARK, bevel=0.003, subsurf=0, micro=True)
    sphere("hairCap", (0, 0.0, 1.8), (0.108, 0.118, 0.1), HAIR, seg=24, rings=12, cut=-0.1, low=(14, 8))
    swoop("braid1", [(0.0, 0.08, 1.8), (0.02, 0.14, 1.66), (-0.02, 0.17, 1.44), (0.03, 0.18, 1.22), (-0.02, 0.17, 1.0), (0.01, 0.16, 0.84)],
          [0.032, 0.036, 0.034, 0.03, 0.024, 0.014], HAIR, res=8, low_u=4, low_res=3)
    for k in range(6): ring("braidTie%d" % k, (0, 0.17, 1.62 - 0.1 * k), 0.03, 0.006, BRONZE, rot=(20, 0, 0), seg=10, tseg=5, micro=True)


# ---------------------------------------------------------------- cape: a tan cape streaming back from the left shoulder; a quiver on the back
def cape_and_quiver():
    ribbon("cape1", [(0.22, 0.02, 1.58), (0.3, 0.16, 1.4), (0.38, 0.28, 1.15), (0.42, 0.4, 0.9), (0.42, 0.5, 0.72)], [0.16, 0.34, 0.44, 0.46, 0.4], CAPE, up=(0, 0, 1), thick=0.01, wave=0.03, subsurf=1)
    ribbon("cape2", [(0.1, 0.06, 1.55), (0.14, 0.2, 1.34), (0.18, 0.34, 1.1), (0.2, 0.46, 0.88), (0.2, 0.54, 0.72)], [0.14, 0.28, 0.36, 0.38, 0.32], CAPE, up=(0, 0, 1), thick=0.01, wave=0.025, subsurf=1)
    limb("quiver", (-0.1, 0.16, 1.05), (-0.12, 0.2, 1.62), 0.05, 0.045, LEATHER, seg=14, low=8)
    ring("quiverRing", (-0.11, 0.185, 1.5), 0.05, 0.008, BRONZE, rot=(85, 0, 0), seg=14, tseg=5, micro=True)
    for k in range(5):
        limb("arrow%d" % k, (-0.11 + 0.02 * (k - 2), 0.19, 1.6), (-0.11 + 0.03 * (k - 2), 0.19, 1.86), 0.006, 0.006, LEATHER2, seg=6, low=6)
        ribbon("fletch%d" % k, [(-0.11 + 0.03 * (k - 2), 0.19, 1.8), (-0.11 + 0.032 * (k - 2), 0.19, 1.9)], [0.03, 0.03], FEATHER, up=(1, 0, 0), thick=0.003, subsurf=0)


# ---------------------------------------------------------------- arms: the left arm (x > 0) holds the bow out front, the right arm (x < 0) draws the string back to the cheek
def arms():
    l_sh, l_el, l_wr = (0.25, 0.0, 1.5), (0.27, -0.32, 1.5), (0.22, -0.66, 1.42)
    r_sh, r_el, r_wr = (-0.23, 0.0, 1.53), (-0.3, -0.22, 1.66), (-0.06, -0.3, 1.62)
    for name, sh, el, wr, s in (("L", l_sh, l_el, l_wr, 1), ("R", r_sh, r_el, r_wr, -1)):
        limb("upperArm" + name, sh, el, 0.075, 0.062, SKIN, seg=18, low=10)
        limb("foreArm" + name, el, wr, 0.062, 0.05, SKIN, seg=18, low=10)
        sphere("elbow" + name, el, (0.065, 0.065, 0.065), SKIN, seg=14, rings=10, low=(9, 6))
        limb("bracer" + name, (el[0] * 0.55 + wr[0] * 0.45, el[1] * 0.55 + wr[1] * 0.45, el[2] * 0.55 + wr[2] * 0.45), wr, 0.068, 0.056, LEATHER, seg=18, low=10, caps=False)
        sphere("hand" + name, wr, (0.05, 0.055, 0.05), SKIN, seg=14, rings=10, low=(9, 6))
        for k in range(3):
            c = (el[0] * (1 - (0.28, 0.5, 0.72)[k]) + wr[0] * (0.28, 0.5, 0.72)[k], el[1] * (1 - (0.28, 0.5, 0.72)[k]) + wr[1] * (0.28, 0.5, 0.72)[k],
                 el[2] * (1 - (0.28, 0.5, 0.72)[k]) + wr[2] * (0.28, 0.5, 0.72)[k])
            ring("bracerLace%s%d" % (name, k), c, 0.066, 0.006, LEATHER2, rot=(90, 0, s * 8), scale=(1, 1, 1), micro=True)


# ---------------------------------------------------------------- the bow of fire: the limbs are burning arcs, the flames lick back off them
def bow():
    bow_c = (0.22, -0.72, 1.42)
    up_limb = [bow_c, (bow_c[0], bow_c[1] + 0.03, bow_c[2] + 0.35), (bow_c[0], bow_c[1] + 0.16, bow_c[2] + 0.66), (bow_c[0], bow_c[1] + 0.34, bow_c[2] + 0.86)]
    dn_limb = [bow_c, (bow_c[0], bow_c[1] + 0.03, bow_c[2] - 0.35), (bow_c[0], bow_c[1] + 0.16, bow_c[2] - 0.66), (bow_c[0], bow_c[1] + 0.34, bow_c[2] - 0.86)]
    for tag, lp in (("Up", up_limb), ("Dn", dn_limb)):
        swoop("bow" + tag, lp, [0.045, 0.04, 0.032, 0.014], FIRE, res=8, low_u=4, low_res=3)
        swoop("bowCore" + tag, lp, [0.024, 0.02, 0.016, 0.006], FIRE2, res=6, low_u=3, low_res=2)
        for k, t in enumerate((0.25, 0.5, 0.75, 0.95)):                                       # flames: the main outward lick stays a chunky low-poly shape, the rest are baked-only
            i = min(int(t * 3), 2); f = t * 3 - i
            p = tuple(lp[i][j] * (1 - f) + lp[i + 1][j] * f for j in range(3))
            d = 1 if tag == "Up" else -1
            swoop("flame%s%d" % (tag, k), [p, (p[0] + 0.02, p[1] + 0.12, p[2] + d * 0.06), (p[0] - 0.02, p[1] + 0.26, p[2] + d * 0.13), (p[0], p[1] + 0.4, p[2] + d * 0.22)],
                  [0.05, 0.04, 0.024, 0.004], FIRE, res=6, low_u=4, low_res=2, low_scale=1.4)
            for sx in (-1, 1):                                                                # flames licking sideways too, so the bow burns from the front as well
                swoop("flameS%s%d_%d" % (tag, k, sx), [p, (p[0] + sx * 0.08, p[1] + 0.03, p[2] + d * 0.07), (p[0] + sx * 0.17, p[1] + 0.05, p[2] + d * 0.16), (p[0] + sx * 0.22, p[1] + 0.06, p[2] + d * 0.3)],
                      [0.045, 0.034, 0.02, 0.004], FIRE, res=6, micro=True)
            swoop("flameIn%s%d" % (tag, k), [p, (p[0], p[1] + 0.08, p[2] + d * 0.04), (p[0], p[1] + 0.17, p[2] + d * 0.08)], [0.028, 0.02, 0.004], FIRE2, res=6, micro=True)
    tube("bowStringU", [up_limb[-1], (0.12, -0.42, 1.55), (-0.06, -0.3, 1.62)], 0.004, FIRE2)
    tube("bowStringD", [dn_limb[-1], (0.12, -0.42, 1.55), (-0.06, -0.3, 1.62)], 0.004, FIRE2)
    limb("arrow", (-0.06, -0.3, 1.62), (0.22, -0.98, 1.42), 0.008, 0.008, STEEL, seg=8, low=6)
    swoop("arrowHead", [(0.22, -0.98, 1.42), (0.235, -1.06, 1.395), (0.245, -1.12, 1.38)], [0.03, 0.02, 0.003], FIRE2, res=6, low_u=3, low_res=2)
    ribbon("arrowFletch", [(-0.05, -0.32, 1.61), (0.0, -0.44, 1.585)], [0.04, 0.04], FEATHER, up=(0, 0, 1), thick=0.003, subsurf=0)
    box("bowGrip", (0.22, -0.7, 1.42), (0.045, 0.06, 0.14), LEATHER, bevel=0.012, subsurf=1)
    for k, (x, y, z) in enumerate(((0.5, -0.5, 1.9), (0.62, -0.3, 1.5), (0.4, -0.2, 2.0), (-0.3, -0.4, 1.2), (0.7, 0.1, 1.7), (0.55, 0.3, 1.3))):   # sparks
        sphere("spark%d" % k, (x, y, z), (0.018, 0.018, 0.018), FIRE2, seg=8, rings=6, low=(6, 5))


def build():
    legs(); hips(); torso(); head_and_hair(); cape_and_quiver(); arms(); bow()


H = 2.1   # the model's height, for the preview cameras
finalize_lod("Pyra", "SM_Champion_9014_Pyra.glb", build, budget=8000, ao_strength=0.35,
             uv_boost={"head": 2.0, "jaw": 2.0, "hairCap": 1.4, "braid": 1.2, "chestBack": 1.3},
             extra_views=[("face", (0.08, -1.4, 1.83), (0.0, -0.13, 1.77), (900, 900), 50)])
