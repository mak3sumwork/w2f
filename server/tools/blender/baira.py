"""Baira, the Tidecaller: a sea sorceress after splash_arts/Baira.jpg. A blue-skinned woman with a long tail of teal-to-violet scales coiling behind her, bronze-and-teal shell armour with fin
shoulders, a mane of dark hair, a coral trident staff crowned with a glowing blue orb in her raised right hand, and a wave orb swirling in her left. Human height: about 1.9 m without the staff.

    ~/w2f_bpy/venv/bin/python tools/blender/baira.py [--preview DIR] [--no-bake]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *

SKIN = material("Skin", (0.33, 0.52, 0.6), 0.0, 0.5, kind="skin")
SCALE = material("Scale", (0.05, 0.42, 0.55), 0.15, 0.4, kind="scales", color2=(0.38, 0.16, 0.6))
FIN = material("Fin", (0.85, 0.22, 0.72), 0.0, 0.5, kind="cloth", color2=(0.2, 0.7, 0.95))
GOLD = material("Gold", (0.62, 0.46, 0.16), 0.5, 0.35, kind="metal", wear=0.15)
SHELL = material("Shell", (0.06, 0.3, 0.38), 0.3, 0.4, kind="metal", wear=0.1, color2=(0.2, 0.6, 0.65))
HAIR = material("Hair", (0.025, 0.035, 0.09), 0.0, 0.45)
CORAL = material("Coral", (0.78, 0.22, 0.26), 0.0, 0.55, kind="bark", color2=(0.95, 0.5, 0.45))
STAFF = material("Staff", (0.08, 0.1, 0.2), 0.3, 0.45, kind="metal", wear=0.2)
WATER = glow("Water", (0.25, 0.75, 1.0))
EYE = glow("Eye", (0.55, 0.95, 1.0))
ORB = glow("Orb", (0.35, 0.8, 1.0))
DARK = material("Dark", (0.02, 0.03, 0.05), 0.0, 0.8)

# ---------------------------------------------------------------- the tail: an S that coils on the floor behind her, ending in a tall fin
tail_pts = [(0, 0.02, 1.12), (0, 0.0, 0.85), (0.02, 0.06, 0.55), (0.12, 0.25, 0.28), (0.32, 0.55, 0.16), (0.3, 0.95, 0.2), (0.08, 1.25, 0.38), (-0.12, 1.4, 0.62), (-0.2, 1.42, 0.86)]
tail_r = [0.19, 0.19, 0.18, 0.16, 0.14, 0.11, 0.08, 0.055, 0.03]
swoop("tail", tail_pts, tail_r, SCALE, res=14)
# belly plates along the tail
swoop("tailBelly", [(0, -0.09, 1.1), (0, -0.14, 0.85), (0.02, -0.1, 0.55), (0.1, 0.08, 0.28)], [0.05, 0.06, 0.055, 0.045], GOLD, res=8)
# the dorsal fin runs down the back of the tail, the tail fluke fans at its end
ribbon("dorsal", [(0, 0.16, 0.95), (0.02, 0.24, 0.6), (0.14, 0.4, 0.3), (0.32, 0.66, 0.22), (0.3, 1.0, 0.26), (0.1, 1.28, 0.44)], [0.3, 0.42, 0.42, 0.36, 0.28, 0.2], FIN, up=(0, 1, 0.3), thick=0.008, wave=0.02)
for s, ang in ((1, 1), (-1, -1)):
    ribbon("fluke%d" % s, [(-0.2, 1.42, 0.86), (-0.2 + 0.18 * s, 1.44, 1.05), (-0.2 + 0.36 * s, 1.44, 1.22), (-0.2 + 0.5 * s, 1.42, 1.32)], [0.18, 0.3, 0.32, 0.22], FIN, up=(0, 1, 0), thick=0.008, wave=0.015)
    ribbon("flukeLow%d" % s, [(-0.2, 1.42, 0.86), (-0.2 + 0.2 * s, 1.44, 0.7), (-0.2 + 0.36 * s, 1.44, 0.58)], [0.16, 0.24, 0.16], FIN, up=(0, 1, 0), thick=0.008)
# hip fins
for s in (1, -1):
    ribbon("hipFin%d" % s, [(0.14 * s, 0.02, 1.08), (0.32 * s, 0.14, 0.9), (0.44 * s, 0.32, 0.66), (0.4 * s, 0.5, 0.42)], [0.2, 0.26, 0.24, 0.14], FIN, up=(0, 0, 1), thick=0.008, wave=0.02)

# ---------------------------------------------------------------- torso
limb("waist", (0, 0, 1.08), (0, -0.01, 1.32), 0.17, 0.15, SKIN, seg=28)
sphere("ribs", (0, -0.01, 1.42), (0.19, 0.13, 0.17), SKIN, seg=28, rings=16)
limb("neck", (0, -0.01, 1.58), (0, -0.02, 1.68), 0.055, 0.05, SKIN, seg=16)
sphere("bustL", (0.085, -0.1, 1.44), (0.085, 0.07, 0.085), SHELL, seg=20, rings=12)
sphere("bustR", (-0.085, -0.1, 1.44), (0.085, 0.07, 0.085), SHELL, seg=20, rings=12)
box("bustBand", (0, -0.1, 1.37), (0.3, 0.05, 0.035), GOLD, bevel=0.01, subsurf=1)
sphere("bustPearl", (0, -0.15, 1.42), (0.03, 0.03, 0.03), ORB, seg=12, rings=8)
for k, dz in enumerate((1.22, 1.15, 1.08)):                                                       # a belt of overlapping shell plates at the waist
    box("plate%d" % k, (0, -0.13, dz), (0.32 - 0.02 * k, 0.03, 0.09), SHELL if k % 2 == 0 else GOLD, rot=(-8, 0, 0), bevel=0.012, subsurf=1)
for s in (1, -1):
    box("hipShell%d" % s, (0.19 * s, -0.06, 1.13), (0.1, 0.1, 0.16), SHELL, rot=(0, 0, -20 * s), bevel=0.015, subsurf=1)
    tube("waistGlow%d" % s, [(0.15 * s, -0.1, 1.26), (0.19 * s, -0.09, 1.32), (0.2 * s, -0.07, 1.38)], 0.006, WATER)
# glowing patterns on the skin
for s in (1, -1):
    tube("ribGlow%d" % s, [(0.05 * s, -0.135, 1.52), (0.1 * s, -0.12, 1.56), (0.15 * s, -0.09, 1.55)], 0.005, WATER)
tube("throatGlow", [(-0.03, -0.055, 1.62), (0, -0.06, 1.6), (0.03, -0.055, 1.62)], 0.005, WATER)

# ---------------------------------------------------------------- head, hair, crown
sphere("head", (0, -0.03, 1.79), (0.105, 0.115, 0.13), SKIN, seg=28, rings=18)
sphere("jaw", (0, -0.06, 1.72), (0.075, 0.075, 0.06), SKIN, seg=16, rings=10)
for s in (1, -1):
    box("eye%d" % s, (0.042 * s, -0.135, 1.805), (0.04, 0.01, 0.014), EYE, rot=(0, 0, -12 * s), bevel=0.003, subsurf=0)
    ribbon("finEar%d" % s, [(0.1 * s, -0.02, 1.82), (0.16 * s, 0.0, 1.86), (0.2 * s, 0.03, 1.94)], [0.05, 0.06, 0.03], FIN, up=(0, 1, 0), thick=0.005)
    swoop("brow%d" % s, [(0.03 * s, -0.135, 1.835), (0.07 * s, -0.132, 1.845), (0.11 * s, -0.12, 1.84)], [0.006, 0.006, 0.004], DARK, res=4)
box("lips", (0, -0.14, 1.735), (0.04, 0.01, 0.012), material("Lips", (0.45, 0.2, 0.35), 0.0, 0.5), bevel=0.003, subsurf=0)
# a crown of shell spikes
for k, (dx, ang, h_) in enumerate(((-0.07, -25, 0.13), (0.0, 0, 0.17), (0.07, 25, 0.13))):
    swoop("crown%d" % k, [(dx, -0.07, 1.9), (dx * 1.3, -0.07, 1.9 + h_ * 0.6), (dx * 1.8, -0.07, 1.9 + h_)], [0.016, 0.012, 0.004], GOLD, res=6)
# long dark hair: a mane down the back and a lock over each shoulder
for k in range(9):
    x = -0.09 + 0.0225 * k
    swoop("hair%d" % k, [(x, 0.03, 1.9), (x * 1.3, 0.1, 1.78), (x * 1.6, 0.14, 1.55), (x * 1.8 + 0.02 * (k % 3), 0.15, 1.28), (x * 1.6, 0.13, 1.0)], [0.028, 0.03, 0.028, 0.02, 0.006], HAIR, res=6)
for s in (1, -1):
    swoop("lock%d" % s, [(0.09 * s, -0.04, 1.86), (0.14 * s, -0.08, 1.7), (0.17 * s, -0.09, 1.5), (0.15 * s, -0.07, 1.34)], [0.022, 0.024, 0.02, 0.006], HAIR, res=6)

# ---------------------------------------------------------------- shoulders and arms
for s in (1, -1):
    sphere("shoulder%d" % s, (0.2 * s, 0.0, 1.58), (0.06, 0.06, 0.06), SKIN, seg=16, rings=10)
    ribbon("finShoulder%d" % s, [(0.2 * s, 0.0, 1.62), (0.29 * s, -0.02, 1.72), (0.36 * s, -0.02, 1.84)], [0.08, 0.1, 0.05], FIN, up=(0, 1, 0), thick=0.008)
    sphere("padShell%d" % s, (0.22 * s, -0.01, 1.6), (0.09, 0.08, 0.06), SHELL, seg=20, rings=10, cut=-0.1)
# right arm (x < 0): raised, holding the staff
r_sh, r_el, r_wr = (-0.2, 0.0, 1.56), (-0.36, -0.06, 1.7), (-0.42, -0.14, 1.86)
limb("upperArmR", r_sh, r_el, 0.045, 0.038, SKIN, seg=16); limb("foreArmR", r_el, r_wr, 0.038, 0.032, SKIN, seg=16)
sphere("handR", (-0.42, -0.15, 1.88), (0.045, 0.05, 0.05), SKIN, seg=14, rings=10)
box("bracerR", (-0.4, -0.11, 1.86), (0.05, 0.05, 0.1), GOLD, rot=(0, 0, 10), bevel=0.01, subsurf=1)
# left arm (x > 0): forward and low, an open hand with a swirling orb
l_sh, l_el, l_wr = (0.2, 0.0, 1.56), (0.38, -0.13, 1.42), (0.46, -0.4, 1.46)
limb("upperArmL", l_sh, l_el, 0.045, 0.038, SKIN, seg=16); limb("foreArmL", l_el, l_wr, 0.038, 0.03, SKIN, seg=16)
sphere("handL", (0.48, -0.46, 1.47), (0.05, 0.055, 0.03), SKIN, seg=14, rings=10, rot=(20, 0, 0))
for k in range(4): swoop("finger%d" % k, [(0.46 + (k - 1.5) * 0.02, -0.48, 1.485), (0.46 + (k - 1.5) * 0.028, -0.53, 1.5), (0.46 + (k - 1.5) * 0.03, -0.56, 1.5)], [0.008, 0.007, 0.004], SKIN, res=4)
box("bracerL", (0.42, -0.27, 1.44), (0.05, 0.1, 0.05), GOLD, rot=(0, 0, 0), bevel=0.01, subsurf=1)
sphere("waveOrb", (0.49, -0.6, 1.53), (0.11, 0.11, 0.11), ORB, seg=24, rings=14)
for k in range(3):                                                                                # rings of water around the orb
    ring("waveRing%d" % k, (0.49, -0.6, 1.53), 0.16 + 0.04 * k, 0.006, WATER, rot=(20 * k + 40, 30 * k, 15 * k), seg=36, tseg=6)
tube("waveSwirl", [(0.3, -0.5, 1.2), (0.42, -0.62, 1.3), (0.6, -0.68, 1.42), (0.66, -0.5, 1.58), (0.56, -0.4, 1.66)], 0.012, WATER)

# ---------------------------------------------------------------- the coral trident staff
limb("staffPole", (-0.42, -0.15, 0.85), (-0.42, -0.15, 2.05), 0.03, 0.024, STAFF, seg=14)
for z in (1.05, 1.35, 1.65, 1.9): ring("staffBand%d" % int(z * 10), (-0.42, -0.15, z), 0.034, 0.009, GOLD, seg=14, tseg=6)
prongs = [((-0.42, -0.15, 2.0), (-0.5, -0.15, 2.12), (-0.62, -0.15, 2.3), (-0.58, -0.15, 2.5)),
          ((-0.42, -0.15, 2.0), (-0.42, -0.15, 2.2), (-0.42, -0.15, 2.4), (-0.42, -0.15, 2.68)),
          ((-0.42, -0.15, 2.0), (-0.34, -0.15, 2.12), (-0.22, -0.15, 2.3), (-0.26, -0.15, 2.5)),
          ((-0.42, -0.15, 2.05), (-0.46, -0.08, 2.2), (-0.5, 0.0, 2.38), (-0.48, 0.04, 2.55)),
          ((-0.42, -0.15, 2.05), (-0.38, -0.22, 2.2), (-0.34, -0.3, 2.38), (-0.36, -0.34, 2.52))]
for k, pts in enumerate(prongs):
    swoop("coral%d" % k, list(pts), [0.026, 0.022, 0.016, 0.006], CORAL, res=8)
    mid = pts[2]
    swoop("coralTwig%da" % k, [mid, (mid[0] - 0.07, mid[1], mid[2] + 0.08), (mid[0] - 0.11, mid[1], mid[2] + 0.2)], [0.014, 0.01, 0.004], CORAL, res=6)
    swoop("coralTwig%db" % k, [mid, (mid[0] + 0.06, mid[1], mid[2] + 0.1), (mid[0] + 0.09, mid[1], mid[2] + 0.22)], [0.014, 0.01, 0.004], CORAL, res=6)
sphere("staffOrb", (-0.42, -0.15, 2.28), (0.085, 0.085, 0.085), ORB, seg=24, rings=14)
for k in range(2): ring("orbRing%d" % k, (-0.42, -0.15, 2.28), 0.13 + 0.03 * k, 0.005, WATER, rot=(70 * k + 30, 20 * k, 40 * k), seg=36, tseg=6)

# ---------------------------------------------------------------- water swirling round the tail
tube("swirl1", [(0.34, -0.1, 0.4), (0.3, 0.2, 0.55), (-0.1, 0.5, 0.42), (-0.4, 0.3, 0.5), (-0.44, -0.05, 0.7), (-0.2, -0.3, 0.6)], 0.014, WATER)
tube("swirl2", [(-0.3, -0.2, 0.3), (-0.1, -0.32, 0.5), (0.25, -0.26, 0.42), (0.42, 0.0, 0.62), (0.3, 0.3, 0.9)], 0.011, WATER)

finalize("Baira", "SM_Champion_9002_Baira.glb")
