"""Soul, the Lost Vessel, after splash_arts/Soul.jpg: a rust-black armoured knight, spiked pauldrons, a visored helm with green fire eyes, chains wrapped over the plates, glowing green runes,
and a colossal rusty greatsword with a notched blade. Big (about 2.3 m).

    ~/w2f_bpy/venv/bin/python tools/blender/soul.py [--preview DIR] [--no-bake]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *

STEEL = material("Steel", (0.075, 0.07, 0.07), 0.45, 0.5, kind="metal", wear=0.75)
STEEL2 = material("SteelLight", (0.15, 0.14, 0.135), 0.45, 0.45, kind="metal", wear=0.55)
RUST = material("Rust", (0.3, 0.13, 0.06), 0.35, 0.6, kind="metal", wear=0.9, color2=(0.45, 0.22, 0.1))
CHAIN = material("Chain", (0.12, 0.1, 0.09), 0.5, 0.5, kind="metal", wear=0.6)
CLOTH = material("Cloth", (0.05, 0.045, 0.045), 0.0, 0.85, kind="cloth")
UNDER = material("Under", (0.015, 0.014, 0.014), 0.0, 0.85)
GREEN = glow("Green", (0.35, 1.0, 0.6))
BONE = material("Bone", (0.5, 0.48, 0.38), 0.0, 0.7)

# ---------------------------------------------------------------- legs
for s in (1, -1):
    x = 0.3 * s
    limb("thigh%d" % s, (x, 0.0, 1.05), (x * 1.1, -0.02, 0.6), 0.2, 0.17, STEEL, seg=24)
    box("thighPlate%d" % s, (x * 1.05, -0.15, 0.85), (0.3, 0.09, 0.4), STEEL2, rot=(-6, 0, 0), bevel=0.03, subsurf=1)
    box("thighRust%d" % s, (x * 1.05, -0.2, 0.85), (0.2, 0.03, 0.26), RUST, rot=(-6, 0, 0), bevel=0.02, subsurf=1)
    sphere("knee%d" % s, (x * 1.1, -0.15, 0.58), (0.2, 0.15, 0.2), STEEL2, seg=24, rings=14)
    for k in range(3): swoop("kneeSpike%d_%d" % (s, k), [(x * 1.1 + (k - 1) * 0.1, -0.28, 0.6), (x * 1.1 + (k - 1) * 0.13, -0.4, 0.62), (x * 1.1 + (k - 1) * 0.15, -0.5, 0.63)], [0.045, 0.03, 0.004], STEEL, res=5)
    tube("kneeRune%d" % s, [(x * 1.1 - 0.05, -0.29, 0.66), (x * 1.1, -0.3, 0.6), (x * 1.1 + 0.05, -0.29, 0.66)], 0.008, GREEN)
    limb("shin%d" % s, (x * 1.1, -0.02, 0.58), (x * 1.12, -0.05, 0.15), 0.17, 0.14, STEEL, seg=24)
    box("shinPlate%d" % s, (x * 1.11, -0.17, 0.36), (0.27, 0.09, 0.42), STEEL2, rot=(8, 0, 0), bevel=0.03, subsurf=1)
    box("boot%d" % s, (x * 1.12, -0.12, 0.09), (0.34, 0.62, 0.18), STEEL, bevel=0.04, subsurf=1)
    box("toe%d" % s, (x * 1.12, -0.4, 0.09), (0.32, 0.2, 0.15), STEEL2, bevel=0.04)
    box("sole%d" % s, (x * 1.12, -0.12, 0.02), (0.36, 0.66, 0.05), UNDER, bevel=0.01, subsurf=0)
    ribbon("tatter%d" % s, [(x, -0.14, 1.02), (x * 1.05, -0.17, 0.82), (x * 1.08, -0.16, 0.62)], [0.22, 0.2, 0.12], CLOTH, up=(0, 1, 0), thick=0.008, wave=0.03)

# ---------------------------------------------------------------- torso: heavy plate
box("pelvis", (0, 0, 1.08), (0.62, 0.4, 0.26), STEEL, bevel=0.04, subsurf=1)
box("belt", (0, -0.02, 1.22), (0.68, 0.44, 0.12), RUST, bevel=0.03, subsurf=1)
box("beltRune", (0, -0.25, 1.22), (0.16, 0.03, 0.12), STEEL2, bevel=0.015)
box("beltGlow", (0, -0.275, 1.22), (0.08, 0.012, 0.07), GREEN, bevel=0.005, subsurf=0)
ribbon("tabard", [(0, -0.24, 1.2), (0, -0.28, 0.98), (0, -0.28, 0.72), (0.02, -0.26, 0.5)], [0.26, 0.28, 0.22, 0.12], CLOTH, up=(0, 1, 0), thick=0.008, wave=0.03)
sphere("belly", (0, 0.0, 1.42), (0.42, 0.3, 0.24), STEEL, seg=28, rings=16)
sphere("chest", (0, 0.0, 1.72), (0.56, 0.36, 0.38), STEEL, seg=32, rings=18)
box("breastplate", (0, -0.25, 1.72), (0.66, 0.14, 0.5), STEEL2, rot=(-8, 0, 0), bevel=0.06, subsurf=1, taper=0.9)
box("breastRust", (0, -0.33, 1.7), (0.36, 0.04, 0.34), RUST, rot=(-8, 0, 0), bevel=0.03, subsurf=1)
box("breastRuneBox", (0, -0.37, 1.74), (0.2, 0.03, 0.2), STEEL2, rot=(-8, 0, 45), bevel=0.012)
box("breastRune", (0, -0.39, 1.74), (0.11, 0.015, 0.11), GREEN, rot=(-8, 0, 45), bevel=0.006, subsurf=0)
for k in range(2): box("abdomen%d" % k, (0, -0.22, 1.36 - 0.11 * k), (0.5 - 0.04 * k, 0.1, 0.13), STEEL2 if k == 0 else STEEL, rot=(-4, 0, 0), bevel=0.03, subsurf=1)
limb("neck", (0, 0, 2.0), (0, -0.02, 1.88), 0.12, 0.16, UNDER, seg=18)
ring("gorget", (0, -0.01, 1.96), 0.2, 0.06, STEEL2, scale=(1.05, 0.9, 0.6))
ring("gorget2", (0, -0.01, 1.88), 0.27, 0.06, STEEL, scale=(1.1, 0.95, 0.55))
# chains crossing the chest and wrapped round the arms
for k, (p0, p1, p2) in enumerate((((-0.42, -0.3, 1.95), (0.0, -0.4, 1.7), (0.42, -0.3, 1.5)), ((0.42, -0.3, 1.95), (0.0, -0.4, 1.66), (-0.42, -0.3, 1.46)))):
    swoop("chain%d" % k, [p0, ((p0[0] + p1[0]) / 2, -0.4, (p0[2] + p1[2]) / 2 + 0.02), p1, ((p1[0] + p2[0]) / 2, -0.4, (p1[2] + p2[2]) / 2 - 0.02), p2], [0.028] * 5, CHAIN, res=6)
    for j in range(9):
        t = j / 8
        pos = (p0[0] * (1 - t) ** 2 + 2 * p1[0] * t * (1 - t) + p2[0] * t ** 2, -0.41, p0[2] * (1 - t) ** 2 + 2 * p1[2] * t * (1 - t) + p2[2] * t ** 2)
        ring("link%d_%d" % (k, j), pos, 0.032, 0.008, CHAIN, rot=(90 if j % 2 else 0, 0, 20 if k else -20), seg=10, tseg=5)
swoop("chainHang", [(-0.05, -0.3, 1.24), (-0.1, -0.34, 1.0), (0.02, -0.36, 0.85), (0.1, -0.33, 0.95)], [0.025] * 4, CHAIN, res=5)

# ---------------------------------------------------------------- head: a visored helm with green fire behind the slit
sphere("helm", (0, -0.02, 2.17), (0.24, 0.26, 0.26), STEEL2, seg=36, rings=22)
sphere("helmTop", (0, -0.02, 2.33), (0.15, 0.17, 0.18), STEEL2, seg=24, rings=14)
box("faceplate", (0, -0.22, 2.1), (0.34, 0.1, 0.3), STEEL, rot=(-6, 0, 0), bevel=0.03, subsurf=1)
box("visorSlit", (0, -0.285, 2.14), (0.28, 0.02, 0.05), UNDER, bevel=0.005, subsurf=0)
for s in (1, -1):
    box("eye%d" % s, (0.075 * s, -0.291, 2.142), (0.09, 0.012, 0.028), GREEN, rot=(0, 0, -12 * s), bevel=0.004, subsurf=0)
    box("cheek%d" % s, (0.19 * s, -0.14, 2.02), (0.09, 0.2, 0.26), STEEL, rot=(0, 0, 12 * s), bevel=0.025)
    swoop("flame%d" % s, [(0.16 * s, -0.16, 2.36), (0.24 * s, -0.12, 2.5), (0.3 * s, -0.08, 2.66), (0.26 * s, -0.04, 2.8)], [0.045, 0.04, 0.026, 0.004], GREEN, res=6)   # green fire licking from the helm
box("crest", (0, 0.0, 2.52), (0.045, 0.3, 0.16), RUST, rot=(-10, 0, 0), bevel=0.012, subsurf=1)
tube("browRune", [(-0.12, -0.29, 2.24), (0, -0.305, 2.28), (0.12, -0.29, 2.24)], 0.008, GREEN)

# ---------------------------------------------------------------- pauldrons: big, spiked, with glowing runes
for s in (1, -1):
    x = 0.78 * s
    sphere("pauld%dTop" % s, (x, 0.0, 1.98), (0.4, 0.36, 0.3), STEEL2, seg=36, rings=18, cut=-0.05)
    limb("pauld%dSkirt" % s, (x, 0.0, 1.72), (x, 0.0, 1.98), 0.5, 0.42, STEEL, seg=36, caps=False)
    ring("pauld%dRim" % s, (x, 0.0, 1.73), 0.5, 0.03, RUST, scale=(1, 0.9, 0.6))
    for k in range(5):
        a = math.radians(-70 + 35 * k)
        swoop("pauld%dSpike%d" % (s, k), [(x + 0.38 * math.sin(a) * s * 0.9, -0.3 * math.cos(a), 1.98 + 0.1 * math.cos(a)), (x + 0.5 * math.sin(a) * s * 0.9, -0.4 * math.cos(a), 2.05 + 0.1 * math.cos(a)), (x + 0.62 * math.sin(a) * s * 0.9, -0.5 * math.cos(a), 2.16 + 0.12 * math.cos(a))], [0.05, 0.035, 0.004], STEEL, res=5)
    swoop("pauld%dHorn" % s, [(x, 0.0, 2.25), (x * 1.05, 0.0, 2.4), (x * 1.08, 0.0, 2.6)], [0.07, 0.045, 0.005], STEEL, res=6)
    box("pauld%dDisc" % s, (x, -0.35, 1.96), (0.26, 0.05, 0.26), RUST, rot=(-14, 0, 0), bevel=0.02)
    ring("pauld%dRuneRing" % s, (x, -0.385, 1.96), 0.1, 0.011, GREEN, rot=(76, 0, 0))
    tube("pauld%dRune" % s, [(x - 0.04, -0.395, 1.92), (x, -0.4, 2.02), (x + 0.04, -0.395, 1.92)], 0.008, GREEN)

# ---------------------------------------------------------------- arms: the sword arm (x < 0) low and forward, the left arm (x > 0) hanging with a clenched fist
r_sh, r_el, r_wr = (-0.8, 0.0, 1.7), (-1.0, -0.22, 1.35), (-0.9, -0.5, 1.02)
l_sh, l_el, l_wr = (0.8, 0.0, 1.7), (1.02, 0.02, 1.3), (1.0, -0.18, 0.92)
for name, sh, el, wr, s in (("R", r_sh, r_el, r_wr, -1), ("L", l_sh, l_el, l_wr, 1)):
    limb("upperArm" + name, sh, el, 0.2, 0.17, STEEL, seg=24)
    limb("upperArmUnder" + name, sh, el, 0.18, 0.15, UNDER, seg=20)
    sphere("elbow" + name, el, (0.21, 0.21, 0.21), STEEL2, seg=18, rings=12)
    swoop("elbowSpike" + name, [(el[0] + s * 0.1, el[1] + 0.05, el[2]), (el[0] + s * 0.22, el[1] + 0.1, el[2] + 0.02), (el[0] + s * 0.34, el[1] + 0.14, el[2] + 0.04)], [0.05, 0.035, 0.004], STEEL, res=5)
    limb("foreArm" + name, el, wr, 0.18, 0.15, STEEL, seg=24)
    for k, t in enumerate((0.3, 0.55, 0.8)):
        c = (el[0] * (1 - t) + wr[0] * t, el[1] * (1 - t) + wr[1] * t, el[2] * (1 - t) + wr[2] * t)
        ring("bracer%s%d" % (name, k), c, 0.19 - 0.012 * k, 0.03, STEEL2 if k % 2 == 0 else RUST, rot=(90 - 14, 0, s * 8))
    box("palm" + name, (wr[0], wr[1] - 0.06, wr[2] - 0.08), (0.34, 0.34, 0.3), STEEL2, bevel=0.05, subsurf=1)
    for k in range(4): box("finger%s%d" % (name, k), (wr[0] + (k - 1.5) * 0.08, wr[1] - 0.24, wr[2] - 0.04), (0.07, 0.15, 0.09), STEEL, bevel=0.018, subsurf=0, rot=(22, 0, 0))
    tube("armRune" + name, [(el[0] + s * 0.02, el[1] - 0.18, el[2] - 0.02), (el[0] * 0.5 + wr[0] * 0.5, el[1] * 0.5 + wr[1] * 0.5 - 0.19, el[2] * 0.5 + wr[2] * 0.5), (wr[0], wr[1] - 0.16, wr[2] + 0.12)], 0.009, GREEN)
    swoop("armChain" + name, [(sh[0] + 0.1 * s, -0.16, sh[2] - 0.05), (el[0] * 0.5 + sh[0] * 0.5, -0.22, (el[2] + sh[2]) / 2 + 0.05), (el[0] - 0.05 * s, -0.2, el[2] + 0.1)], [0.028] * 3, CHAIN, res=5)

# ---------------------------------------------------------------- the greatsword: a slab of notched, rusted iron pointing forward and down from the right fist
hand = Vector((-0.9, -0.55, 1.0))
direction = Vector((-0.12, -0.86, -0.5)).normalized()
side = Vector((0, 0, 1)).cross(direction).normalized()
up_ = direction.cross(side).normalized()
def along(t, u=0.0, v=0.0): return tuple(hand + direction * t + side * u + up_ * v)
blade_pts = [along(0.1), along(0.6), along(1.2), along(1.7), along(2.0)]
ribbon("blade", [along(0.12), along(0.7), along(1.3), along(1.85), along(2.15)], [0.3, 0.42, 0.44, 0.42, 0.3], RUST, up=tuple(up_), thick=0.075, wave=0.0, subsurf=1)
ribbon("bladeCore", [along(0.3), along(0.9), along(1.5), along(1.95)], [0.09, 0.1, 0.1, 0.08], STEEL2, up=tuple(up_), thick=0.09, subsurf=1)
tube("bladeRune", [along(0.45, 0, 0.055), along(0.8, 0, 0.055), along(1.15, 0, 0.055), along(1.5, 0, 0.055)], 0.009, GREEN)
for k in range(5):                                                                        # nicks along the edge
    t = 0.55 + 0.32 * k
    box("nick%d" % k, along(t, 0.22 + 0.01 * (k % 2), 0), (0.07, 0.05, 0.08), STEEL, bevel=0.01, subsurf=0)
box("guard", along(0.02), (0.6, 0.12, 0.12), STEEL, rot=(0, 0, 0), bevel=0.03, subsurf=1)
for s in (1, -1): swoop("guardHorn%d" % s, [along(0.02, 0.3 * s), along(0.02, 0.42 * s, 0.1), along(0.02, 0.5 * s, 0.24)], [0.05, 0.035, 0.005], STEEL, res=5)
limb("grip", tuple(hand - direction * 0.15), tuple(hand + direction * 0.05), 0.045, 0.045, UNDER, seg=12)
sphere("pommel", tuple(hand - direction * 0.2), (0.07, 0.07, 0.07), RUST, seg=14, rings=10)

# ---------------------------------------------------------------- skulls and bones at the belt
for x in (-0.3, 0.3):
    sphere("skull%.1f" % x, (x, -0.28, 1.05), (0.06, 0.06, 0.07), BONE, seg=14, rings=10)
    box("skullJaw%.1f" % x, (x, -0.3, 1.0), (0.045, 0.03, 0.03), BONE, bevel=0.005, subsurf=0)

finalize("Soul", "SM_Champion_9010_Soul.glb")
