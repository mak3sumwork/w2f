"""Rot, the swamp horror, after splash_arts/Rot.jpg: a hulking, hunched treant of black bark and moss, thorned vines wrapped round its limbs, a horned skull-like head with glowing green eyes and a tendril
maw, huge clawed arms that reach the ground, a tree trunk sprouting from its back, and green slime dripping from everything. A big monster (about 2.5 m hunched).

    ~/w2f_bpy/venv/bin/python tools/blender/rot.py [--preview DIR] [--no-bake]
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bl_kit import *

BARK = material("Bark", (0.075, 0.065, 0.055), 0.0, 0.8, kind="bark", color2=(0.26, 0.24, 0.2))
BARK2 = material("BarkDark", (0.04, 0.035, 0.03), 0.0, 0.85, kind="bark", color2=(0.14, 0.12, 0.1))
VINE = material("Vine", (0.06, 0.16, 0.035), 0.0, 0.6, kind="leather", color2=(0.2, 0.4, 0.08))
THORN = material("Thorn", (0.07, 0.05, 0.03), 0.0, 0.6)
SLIME = glow("Slime", (0.45, 1.0, 0.12))
EYE = glow("Eye", (0.6, 1.0, 0.2))
D = (0.02, 0.05)      # bark relief (strength, scale)

# ---------------------------------------------------------------- legs: thick trunk legs ending in root feet
for s in (1, -1):
    x = 0.42 * s
    limb("thigh%d" % s, (x * 0.8, 0.05, 1.1), (x, -0.05, 0.62), 0.3, 0.26, BARK, seg=24, displace=D)
    sphere("knee%d" % s, (x, -0.12, 0.62), (0.28, 0.24, 0.26), BARK2, seg=20, rings=14, displace=D)
    limb("shin%d" % s, (x, -0.05, 0.62), (x * 1.05, -0.12, 0.2), 0.25, 0.3, BARK, seg=24, displace=D)
    for k in range(5):                                                                   # root toes
        a = math.radians(-50 + 25 * k)
        swoop("root%d_%d" % (s, k), [(x * 1.05, -0.12, 0.22), (x * 1.05 + 0.2 * math.sin(a), -0.12 - 0.22 * math.cos(a), 0.1), (x * 1.05 + 0.42 * math.sin(a), -0.12 - 0.44 * math.cos(a), 0.03)], [0.09, 0.06, 0.03], BARK2, res=6)
    for k in range(3):
        swoop("legVine%d_%d" % (s, k), [(x * 0.9 + 0.2, 0.0 + 0.05 * k, 1.05), (x * 1.1 - 0.15, -0.28, 0.85 - 0.1 * k), (x * 1.0 + 0.2, -0.3, 0.6 - 0.1 * k), (x * 1.0 - 0.2, -0.25, 0.4)], [0.03, 0.03, 0.028, 0.02], VINE, res=6)

# ---------------------------------------------------------------- the torso: a huge hunched trunk with a tree growing from the back
sphere("belly", (0, 0.05, 1.22), (0.55, 0.45, 0.42), BARK, seg=32, rings=20, displace=D)
sphere("chest", (0, -0.05, 1.68), (0.78, 0.6, 0.55), BARK, seg=36, rings=22, rot=(18, 0, 0), displace=(0.03, 0.06))
sphere("hump", (0, 0.42, 1.98), (0.66, 0.5, 0.42), BARK2, seg=32, rings=20, rot=(-10, 0, 0), displace=(0.035, 0.06))
for s in (1, -1):
    sphere("shoulder%d" % s, (0.9 * s, 0.02, 1.9), (0.42, 0.4, 0.38), BARK, seg=28, rings=18, displace=(0.03, 0.06))
    sphere("shoulderKnob%d" % s, (1.05 * s, 0.0, 2.15), (0.16, 0.16, 0.18), BARK2, seg=16, rings=10, displace=D)
# the trunk on its back with branches
limb("trunk", (0.25, 0.55, 2.05), (0.4, 0.62, 2.85), 0.24, 0.13, BARK2, seg=20, displace=(0.03, 0.05))
for k, (dx, dz, ang) in enumerate(((0.3, 2.45, 30), (0.12, 2.6, -35), (0.5, 2.7, 40))):
    swoop("branch%d" % k, [(0.32 + 0.02 * k, 0.6, dz - 0.05), (0.32 + dx * 0.6, 0.62, dz + 0.15), (0.32 + dx * 1.2, 0.6, dz + 0.35), (0.32 + dx * 1.5, 0.58, dz + 0.5)], [0.07, 0.05, 0.03, 0.008], BARK2, res=6)
for k in range(5):
    swoop("spine%d" % k, [(-0.5 + 0.25 * k, 0.55, 2.1 - 0.05 * (k % 2)), (-0.5 + 0.25 * k, 0.6, 2.3), (-0.5 + 0.25 * k + 0.05, 0.62, 2.55)], [0.07, 0.045, 0.006], BARK2, res=5)
# vines wrapped round the chest, ending in thorns
for k in range(4):
    z0 = 1.35 + 0.16 * k
    swoop("chestVine%d" % k, [(-0.65, -0.3, z0 + 0.15), (-0.3, -0.62, z0), (0.2, -0.62, z0 + 0.1), (0.68, -0.28, z0 - 0.05), (0.7, 0.2, z0 + 0.1)], [0.035, 0.04, 0.04, 0.035, 0.025], VINE, res=6)
    for j in range(5):
        p = (-0.5 + 0.28 * j, -0.6, z0 + 0.06 * (j % 2))
        swoop("thorn%d_%d" % (k, j), [p, (p[0], p[1] - 0.06, p[2] + 0.06), (p[0], p[1] - 0.12, p[2] + 0.1)], [0.02, 0.012, 0.002], THORN, res=4)

# ---------------------------------------------------------------- the head: low and forward, horned, with a tendril maw
sphere("skull", (0, -0.62, 1.78), (0.34, 0.36, 0.32), BARK, seg=32, rings=20, rot=(20, 0, 0), displace=(0.02, 0.04))
box("brow", (0, -0.86, 1.86), (0.5, 0.14, 0.12), BARK2, rot=(15, 0, 0), bevel=0.03, subsurf=1, displace=(0.012, 0.03))
box("jaw", (0, -0.78, 1.5), (0.34, 0.3, 0.14), BARK2, rot=(-10, 0, 0), bevel=0.03, subsurf=1, displace=(0.012, 0.03))
for s in (1, -1):
    sphere("eyeSocket%d" % s, (0.15 * s, -0.9, 1.84), (0.1, 0.06, 0.08), BARK2, seg=14, rings=10)
    sphere("eye%d" % s, (0.15 * s, -0.94, 1.84), (0.065, 0.03, 0.05), EYE, seg=14, rings=10)
    swoop("horn%d" % s, [(0.22 * s, -0.55, 2.0), (0.42 * s, -0.55, 2.25), (0.55 * s, -0.5, 2.55), (0.5 * s, -0.42, 2.8)], [0.075, 0.06, 0.042, 0.008], BARK2, res=6)
    swoop("hornB%d" % s, [(0.4 * s, -0.55, 2.2), (0.6 * s, -0.55, 2.3), (0.78 * s, -0.5, 2.5)], [0.045, 0.03, 0.006], BARK2, res=6)
    swoop("hornC%d" % s, [(0.5 * s, -0.5, 2.5), (0.36 * s, -0.5, 2.7), (0.3 * s, -0.5, 2.9)], [0.03, 0.02, 0.005], BARK2, res=6)
for k in range(7):                                                                     # the maw: hanging tendrils and teeth
    x = -0.18 + 0.06 * k
    swoop("tendril%d" % k, [(x, -0.9, 1.56), (x * 1.2, -0.95, 1.36), (x * 1.4, -0.9, 1.1 - 0.05 * (k % 3)), (x * 1.5, -0.85, 0.9)], [0.032, 0.03, 0.022, 0.005], VINE, res=5)
    swoop("tooth%d" % k, [(x, -0.92, 1.58), (x, -0.95, 1.5)], [0.018, 0.002], material("Bone", (0.6, 0.6, 0.45), 0.0, 0.6), res=4)
box("maw", (0, -0.9, 1.6), (0.24, 0.04, 0.06), EYE, bevel=0.008, subsurf=0)

# ---------------------------------------------------------------- the arms: huge, reaching down, clawed hands on the ground
for s in (1, -1):
    sh, el, wr = (0.95 * s, 0.0, 1.85), (1.25 * s, -0.4, 1.25), (1.2 * s, -0.75, 0.42)
    limb("upperArm%d" % s, sh, el, 0.3, 0.24, BARK, seg=24, displace=D)
    sphere("elbow%d" % s, el, (0.26, 0.24, 0.24), BARK2, seg=18, rings=12, displace=D)
    limb("foreArm%d" % s, el, wr, 0.24, 0.2, BARK, seg=24, displace=D)
    sphere("hand%d" % s, (wr[0], wr[1] - 0.05, wr[2] - 0.05), (0.26, 0.24, 0.16), BARK2, seg=18, rings=12, displace=D)
    for k in range(4):                                                                 # long claws
        a = -0.25 + 0.17 * k
        swoop("claw%d_%d" % (s, k), [(wr[0] + a * s * 0.9, wr[1] - 0.16, wr[2] - 0.06), (wr[0] + a * s * 1.15, wr[1] - 0.36, wr[2] - 0.12), (wr[0] + a * s * 1.3, wr[1] - 0.5, wr[2] - 0.32)], [0.06, 0.045, 0.006], THORN, res=6)
    for k in range(3): swoop("armVine%d_%d" % (s, k), [(sh[0] + 0.1 * s, 0.1, sh[2] - 0.1), (el[0] - 0.1 * s + 0.1 * k, -0.4 + 0.1 * k, el[2] + 0.3), (wr[0] + 0.1 * s, wr[1], wr[2] + 0.5)], [0.03, 0.028, 0.02], VINE, res=6)

# ---------------------------------------------------------------- slime, dripping from the mouth, hands, elbows, and belly
for (x, y, z, l) in ((0.3, -0.92, 1.3, 0.34), (-0.25, -0.9, 1.4, 0.26), (1.15, -0.85, 0.4, 0.3), (-1.15, -0.85, 0.4, 0.3), (1.2, -0.42, 1.0, 0.24), (-1.25, -0.42, 1.0, 0.24), (0.0, -0.62, 1.0, 0.3), (0.55, -0.55, 1.5, 0.22)):
    swoop("drip_%.2f_%.2f" % (x, z), [(x, y, z), (x, y, z - l * 0.6), (x, y, z - l)], [0.016, 0.012, 0.024], SLIME, res=6)
    sphere("drop_%.2f_%.2f" % (x, z), (x, y, z - l - 0.03), (0.025, 0.025, 0.034), SLIME, seg=10, rings=8)
for k in range(5): tube("moss%d" % k, [(-0.6 + 0.3 * k, -0.6, 1.9), (-0.55 + 0.3 * k, -0.62, 1.75)], 0.02, material("Moss%d" % k, (0.12, 0.3, 0.05), 0.0, 0.7))

finalize("Rot", "SM_Champion_9018_Rot.glb", scale=0.92)
