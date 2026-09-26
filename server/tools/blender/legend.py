"""Pip, the player's Little Legend (tactician): a small round spirit-fox sprite that stands on the arena, hops where you right-click and carries your name plate.
Original design: a chubby teal teardrop body with a cream belly, big glossy eyes, leaf-shaped ears with glowing tips, a golden sprout-orb antenna, glowing cheeks,
stubby feet and hands, and a flame-wisp tail. About 0.8 m tall; faces -Y (the game turns it to face where it walks).

    ~/w2f_bpy/venv/bin/python tools/blender/legend.py [--preview DIR] [--no-bake]     -> docs/models/SM_Legend.glb
"""
import bl_kit
from bl_kit import *

bl_kit.TEX = 1024

TEAL = (0.16, 0.62, 0.78)
TEAL_DARK = (0.08, 0.36, 0.5)
CREAM = (0.98, 0.9, 0.74)
NAVY = (0.03, 0.04, 0.1)

m_fur = material("fur", TEAL, 0.0, 0.6, kind="skin")
m_dark = material("furdark", TEAL_DARK, 0.0, 0.65, kind="skin")
m_belly = material("belly", CREAM, 0.0, 0.7, kind="skin")
m_eye = material("eye", NAVY, 0.0, 0.15)
g_shine = glow("shine", (1.0, 1.0, 1.0))
g_cheek = glow("cheek", (1.0, 0.45, 0.6))
g_tip = glow("tip", (0.45, 1.0, 0.95))
g_orb = glow("orb", (1.0, 0.82, 0.3))
m_wisp = material("wisp", (0.35, 0.85, 0.95), 0.0, 0.4, emit=(0.2, 0.7, 0.8), strength=1.0)

# the body: one chubby teardrop, the head is part of it (sections: z, half width x, half depth y)
loft("body", [(0.03, 0.12, 0.11), (0.1, 0.24, 0.22), (0.22, 0.3, 0.27), (0.36, 0.3, 0.27), (0.5, 0.26, 0.24), (0.61, 0.18, 0.17), (0.67, 0.08, 0.08), (0.69, 0.01, 0.01)], m_fur, seg=32, subsurf=1)
sphere("belly", (0, -0.15, 0.24), (0.2, 0.14, 0.19), m_belly, seg=24, rings=14)

# the face: big glossy eyes with two highlights each, glowing cheeks, a tiny mouth
for sx in (-1, 1):
    sphere("eye%d" % sx, (sx * 0.105, -0.225, 0.43), (0.075, 0.05, 0.095), m_eye, seg=24, rings=14)
    sphere("shine%d" % sx, (sx * 0.105 + 0.025, -0.268, 0.465), (0.024, 0.012, 0.028), g_shine, seg=12, rings=8)
    sphere("shine2%d" % sx, (sx * 0.105 - 0.02, -0.268, 0.4), (0.011, 0.008, 0.012), g_shine, seg=10, rings=6)
    sphere("cheek%d" % sx, (sx * 0.19, -0.19, 0.35), (0.04, 0.015, 0.025), g_cheek, rot=(0, 0, sx * -35), seg=14, rings=8)
sphere("mouth", (0, -0.262, 0.355), (0.018, 0.01, 0.01), m_eye, seg=12, rings=6)

# leaf-shaped ears sweeping up and out, glowing tips
for sx in (-1, 1):
    swoop("ear%d" % sx, [(sx * 0.1, 0.0, 0.56), (sx * 0.2, 0.02, 0.72), (sx * 0.3, 0.06, 0.88), (sx * 0.36, 0.1, 0.98)], [0.06, 0.075, 0.05, 0.008], m_fur, res=10)
    swoop("earin%d" % sx, [(sx * 0.12, -0.03, 0.6), (sx * 0.2, -0.02, 0.72), (sx * 0.27, 0.01, 0.84)], [0.03, 0.04, 0.012], m_belly, res=8)
    sphere("eartip%d" % sx, (sx * 0.355, 0.1, 0.975), (0.03, 0.03, 0.04), g_tip, seg=12, rings=8)

# the sprout antenna with a golden orb: the silhouette you recognise from across the board
swoop("sprout", [(0, 0.0, 0.67), (0.02, 0.02, 0.76), (0.07, 0.04, 0.84), (0.1, 0.02, 0.88)], [0.02, 0.018, 0.014, 0.012], m_dark, res=8)
sphere("sproutorb", (0.11, 0.015, 0.905), (0.045, 0.045, 0.045), g_orb, seg=16, rings=10)
ring("sprouthalo", (0.11, 0.015, 0.905), 0.07, 0.006, g_orb, rot=(70, 0, 20), seg=24, tseg=6)

# stubby hands and feet
for sx in (-1, 1):
    sphere("hand%d" % sx, (sx * 0.29, -0.06, 0.26), (0.06, 0.055, 0.075), m_fur, rot=(0, sx * 25, 0), seg=16, rings=10)
    sphere("foot%d" % sx, (sx * 0.11, -0.07, 0.035), (0.075, 0.1, 0.045), m_dark, seg=16, rings=10)

# the flame-wisp tail curling up behind, glowing at the tip
swoop("tail", [(0, 0.2, 0.13), (0, 0.36, 0.2), (0.04, 0.47, 0.36), (0.02, 0.46, 0.52), (-0.04, 0.4, 0.62)], [0.08, 0.1, 0.09, 0.06, 0.012], m_wisp, res=10)
sphere("tailtip", (-0.04, 0.4, 0.62), (0.035, 0.035, 0.05), g_tip, seg=12, rings=8)

finalize("Legend", "SM_Legend.glb", views=[("front", (0.55, -1.9, 0.75), (0, 0, 0.42), (640, 640), 50), ("side", (1.9, -0.6, 0.7), (0, 0.05, 0.42), (640, 640), 50),
                                            ("back", (-0.9, 1.7, 0.9), (0, 0.1, 0.45), (640, 640), 50)])
