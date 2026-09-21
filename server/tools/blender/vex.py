"""Vex ("Cull"), the shadow assassin, after splash_arts/Vex.jpg: a slim, hooded figure in black leather and grey wraps, glowing purple eyes above a mask, ragged cloth streaming behind, and a curved
purple-black dagger in each hand trailing swirls of dark energy, caught mid-spin in a low crouch. Human height (about 1.7 m).

    ~/w2f_bpy/venv/bin/python tools/blender/vex.py [--preview DIR] [--no-bake]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *

LEATHER = material("Leather", (0.045, 0.04, 0.055), 0.0, 0.55, kind="leather", color2=(0.14, 0.12, 0.17))
LEATHER2 = material("LeatherLight", (0.09, 0.08, 0.1), 0.0, 0.55, kind="leather")
WRAP = material("Wrap", (0.42, 0.42, 0.46), 0.0, 0.85, kind="cloth")
CLOAK = material("Cloak", (0.035, 0.03, 0.05), 0.0, 0.85, kind="cloth", color2=(0.1, 0.07, 0.16))
BLADE = material("Blade", (0.1, 0.055, 0.17), 0.5, 0.35, kind="metal", wear=0.1, color2=(0.35, 0.2, 0.55))
STEEL = material("Steel", (0.12, 0.11, 0.14), 0.5, 0.4, kind="metal", wear=0.3)
GLOWP = glow("Purple", (0.72, 0.25, 1.0))
GLOWD = glow("PurpleDeep", (0.4, 0.1, 0.7))
SKIN = material("Skin", (0.16, 0.12, 0.13), 0.0, 0.6, kind="skin")

# A low crouch: the body leans into a spin; the right leg (x < 0) bent forward, the left (x > 0) pushed back.
def leg(hip, knee, ankle, s):
    limb("thigh%d" % s, hip, knee, 0.085, 0.065, LEATHER, seg=18)
    limb("shin%d" % s, knee, ankle, 0.065, 0.048, LEATHER, seg=18)
    sphere("kneePad%d" % s, (knee[0], knee[1] - 0.05, knee[2]), (0.07, 0.05, 0.075), STEEL, seg=16, rings=10)
    limb("greave%d" % s, (knee[0], knee[1] - 0.01, knee[2] - 0.05), (ankle[0], ankle[1] - 0.02, ankle[2] + 0.1), 0.07, 0.056, LEATHER2, seg=16, caps=False)
    for k in range(4):
        t = 0.25 + 0.2 * k
        ring("legWrap%d_%d" % (s, k), (knee[0] * (1 - t) + ankle[0] * t, knee[1] * (1 - t) + ankle[1] * t, knee[2] * (1 - t) + ankle[2] * t), 0.06, 0.008, WRAP, rot=(80, 0, 0), seg=14, tseg=5)
    box("boot%d" % s, (ankle[0], ankle[1] - 0.07, 0.05), (0.09, 0.26, 0.1), LEATHER, bevel=0.025, subsurf=1)
leg((-0.1, 0.0, 0.86), (-0.16, -0.38, 0.5), (-0.14, -0.3, 0.08), -1)
leg((0.1, 0.06, 0.86), (0.26, 0.3, 0.5), (0.36, 0.62, 0.08), 1)
for s in (-1, 1): sphere("hip%d" % s, (0.1 * s, 0.02, 0.88), (0.09, 0.09, 0.09), LEATHER, seg=16, rings=10)

box("belt", (0, 0.0, 0.95), (0.34, 0.22, 0.06), LEATHER2, rot=(0, 0, 4), bevel=0.012, subsurf=1)
for k, x in enumerate((-0.12, -0.04, 0.04, 0.12)): box("pouch%d" % k, (x, -0.13, 0.94), (0.05, 0.03, 0.06), LEATHER, bevel=0.008, subsurf=0)
for k in range(4):                                                        # wrap tails hanging from the belt
    ribbon("beltTail%d" % k, [(-0.15 + 0.1 * k, 0.0, 0.94), (-0.17 + 0.1 * k, 0.05, 0.74), (-0.2 + 0.1 * k, 0.12, 0.54)], [0.06, 0.06, 0.04], WRAP, up=(0, 1, 0), thick=0.005, wave=0.02)

limb("abdomen", (0, 0, 0.95), (0, -0.03, 1.15), 0.13, 0.14, LEATHER, seg=24)
sphere("chest", (0, -0.05, 1.3), (0.19, 0.13, 0.17), LEATHER, seg=28, rings=16, rot=(10, 0, -6))
box("chestPlate", (0, -0.12, 1.3), (0.26, 0.05, 0.2), LEATHER2, rot=(-8, 0, 0), bevel=0.02, subsurf=1)
for k in range(3): box("chestStrap%d" % k, (0, -0.145, 1.2 + 0.05 * k), (0.28, 0.02, 0.014), STEEL, rot=(-8, 0, 0), bevel=0.004, subsurf=0)
tube("chestRune", [(-0.04, -0.16, 1.32), (0, -0.165, 1.36), (0.04, -0.16, 1.32)], 0.005, GLOWP)
limb("neck", (0, -0.03, 1.42), (0, -0.05, 1.5), 0.055, 0.05, LEATHER, seg=14)
for s in (1, -1):
    sphere("shoulder%d" % s, (0.19 * s, -0.02, 1.38), (0.07, 0.07, 0.065), STEEL, seg=16, rings=10)
    box("padPlate%d" % s, (0.2 * s, -0.02, 1.42), (0.11, 0.1, 0.05), LEATHER2, rot=(0, 0, -18 * s), bevel=0.015, subsurf=1)

# ---------------------------------------------------------------- the hood and face
sphere("head", (0, -0.08, 1.58), (0.1, 0.11, 0.12), LEATHER, seg=28, rings=18)
box("mask", (0, -0.155, 1.54), (0.14, 0.035, 0.09), LEATHER2, bevel=0.012, subsurf=1)
box("mask2", (0, -0.16, 1.5), (0.11, 0.03, 0.06), LEATHER, bevel=0.01, subsurf=1)
for s in (1, -1): box("eye%d" % s, (0.045 * s, -0.163, 1.6), (0.05, 0.012, 0.018), GLOWP, rot=(0, 0, -18 * s), bevel=0.004, subsurf=0)
# the hood: a peaked cowl and a cloth cape collar
sphere("hood", (0, -0.04, 1.63), (0.16, 0.17, 0.17), CLOAK, seg=32, rings=18)
limb("hoodPeak", (0, -0.05, 1.68), (0, 0.06, 1.85), 0.1, 0.008, CLOAK, seg=20)
box("hoodBrow", (0, -0.16, 1.66), (0.22, 0.04, 0.05), CLOAK, rot=(10, 0, 0), bevel=0.012, subsurf=1)
ribbon("hoodDrape", [(0, 0.02, 1.7), (0, 0.16, 1.56), (0, 0.26, 1.34), (0, 0.32, 1.1)], [0.2, 0.26, 0.3, 0.24], CLOAK, up=(1, 0, 0), thick=0.008, wave=0.02)
ring("scarf", (0, -0.05, 1.5), 0.08, 0.03, WRAP, rot=(0, 0, 0), scale=(1.15, 1, 0.8))

# ---------------------------------------------------------------- cloak and wraps streaming back
ribbon("cloakL", [(0.16, 0.0, 1.4), (0.3, 0.2, 1.3), (0.5, 0.5, 1.2), (0.72, 0.85, 1.14), (0.9, 1.15, 1.1)], [0.16, 0.3, 0.42, 0.44, 0.34], CLOAK, up=(0, 0, 1), thick=0.008, wave=0.05, subsurf=1)
ribbon("cloakR", [(-0.16, 0.0, 1.4), (-0.28, 0.24, 1.36), (-0.44, 0.55, 1.32), (-0.62, 0.9, 1.3), (-0.8, 1.2, 1.28)], [0.16, 0.3, 0.4, 0.4, 0.3], CLOAK, up=(0, 0, 1), thick=0.008, wave=0.05, subsurf=1)
ribbon("cloakBack", [(0, 0.1, 1.42), (0, 0.24, 1.2), (0, 0.4, 0.98), (0, 0.6, 0.8)], [0.3, 0.42, 0.4, 0.3], CLOAK, up=(1, 0, 0), thick=0.008, wave=0.03)
for k, (dx, dz) in enumerate(((0.1, 1.3), (-0.12, 1.34), (0.04, 1.2), (-0.05, 1.42), (0.14, 1.22))):
    ribbon("wrapTail%d" % k, [(dx, 0.05, dz), (dx * 1.5, 0.3, dz + 0.06), (dx * 2.2, 0.62, dz + 0.14), (dx * 3.0, 0.98, dz + 0.24)], [0.07, 0.07, 0.06, 0.03], WRAP, up=(0, 0, 1), thick=0.004, wave=0.04, subsurf=0)

# ---------------------------------------------------------------- arms flung wide, a curved dagger in each hand
r_sh, r_el, r_wr = (-0.18, -0.02, 1.38), (-0.42, -0.2, 1.2), (-0.5, -0.5, 1.3)
l_sh, l_el, l_wr = (0.18, -0.02, 1.38), (0.42, -0.1, 1.5), (0.52, -0.4, 1.44)
for name, sh, el, wr, s in (("R", r_sh, r_el, r_wr, -1), ("L", l_sh, l_el, l_wr, 1)):
    limb("upperArm" + name, sh, el, 0.06, 0.05, LEATHER, seg=16)
    limb("foreArm" + name, el, wr, 0.05, 0.042, LEATHER2, seg=16)
    for k, t in enumerate((0.15, 0.4, 0.65)): ring("armWrap%s%d" % (name, k), (el[0] * (1 - t) + wr[0] * t, el[1] * (1 - t) + wr[1] * t, el[2] * (1 - t) + wr[2] * t), 0.05, 0.009, WRAP, rot=(0, 90, 0), seg=12, tseg=5)
    sphere("hand" + name, wr, (0.048, 0.05, 0.045), LEATHER, seg=12, rings=8)
    # a curved dagger: a tapering swoop of dark purple steel with a glowing edge
    pts = [(wr[0], wr[1] - 0.04, wr[2] + 0.02), (wr[0] + s * 0.03, wr[1] - 0.16, wr[2] + 0.08), (wr[0] + s * 0.1, wr[1] - 0.3, wr[2] + 0.1), (wr[0] + s * 0.2, wr[1] - 0.4, wr[2] + 0.04), (wr[0] + s * 0.3, wr[1] - 0.44, wr[2] - 0.08)]
    swoop("dagger" + name, pts, [0.035, 0.06, 0.06, 0.04, 0.005], BLADE, res=8)
    swoop("daggerEdge" + name, pts, [0.014, 0.026, 0.026, 0.016, 0.003], GLOWP, res=6)
    box("guard" + name, (wr[0] + s * 0.02, wr[1] - 0.06, wr[2] + 0.02), (0.1, 0.03, 0.04), STEEL, rot=(0, 0, 20 * s), bevel=0.008, subsurf=0)
    swoop("trail" + name, [(wr[0] + s * 0.3, wr[1] - 0.44, wr[2] - 0.08), (wr[0] + s * 0.5, wr[1] - 0.3, wr[2] - 0.3), (wr[0] + s * 0.5, wr[1] - 0.0, wr[2] - 0.5), (wr[0] + s * 0.3, wr[1] + 0.3, wr[2] - 0.55)], [0.03, 0.045, 0.04, 0.005], GLOWD, res=8)

tube("swirlBody", [(-0.4, -0.3, 0.3), (-0.3, 0.1, 0.5), (0.0, 0.34, 0.7), (0.36, 0.2, 0.9), (0.5, -0.2, 1.1)], 0.02, GLOWD)
tube("swirlBody2", [(0.4, -0.3, 0.3), (0.3, 0.1, 0.55), (0.0, 0.3, 0.8), (-0.3, 0.18, 1.0)], 0.014, GLOWP)

finalize("Vex", "SM_Champion_9015_Vex.glb", scale=1.2)
