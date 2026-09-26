# Crafted models

Hand-crafted, textured models that replace the generated blockout of the same name (same `SM_Champion_<id>_<Name>` asset name). `tools/unreal/import_blockouts.py` imports every `*.glb` here after the
blockouts, gives it a fresh material instance (`M_CraftedPBR`: base colour + emissive from its own textures) and does NOT put the vertex-colour blockout material on it.

| Model | Made by | After |
|---|---|---|
| `SM_Champion_9001_Alesk.glb` | `tools/blender/alesk.py` (**LOD pipeline, 6,862 triangles**) | `splash_arts/Alesk.jpg`: a dark-iron golem, ribbed domed pauldrons with rune discs, a helm with glowing teal eyes, a teal gem in the breastplate, a cracked tower shield, huge gauntlets |
| `SM_Champion_9002_Baira.glb` | `tools/blender/baira.py` (**LOD pipeline, 7,280 triangles**) | Baira: a blue-skinned sea sorceress, an hourglass torso arched into a Nami-like swimming pose, a coiling scaled tail with a dorsal ridge, hip fins and a trailing fantail, shell armour, flowing hair, a coral trident staff with a glowing orb, a wave orb in her other hand |
| `SM_Champion_9014_Pyra.glb` | `tools/blender/pyra.py` (**LOD pipeline, 6,254 triangles**) | Pyra: a broad-shouldered archer in a wide lunge, leather and bronze armour, a pteruge skirt, a cape, a braid, a great recurved bow of living flame with a nocked arrow |
| `SM_Champion_9018_Rot.glb` | `tools/blender/rot.py` (**LOD pipeline, 7,300 triangles**) | Rot: a hulking hunched treant of black bark and moss, horns, glowing green eyes, tendril maw, clawed arms, dripping slime |
| `SM_Champion_9010_Soul.glb` | `tools/blender/soul.py` | Soul: a rust-black spiked knight, chains, green runes, a colossal notched greatsword |
| `SM_Champion_9015_Vex.glb` | `tools/blender/vex.py` | Vex: a hooded shadow assassin, purple eyes, grey wraps, ragged cloth, two curved purple daggers with dark-energy swirls |

**Triangle budget: TFT standard, at most ~8,000 triangles per champion** (6,000-10,000), silhouette first, no micro-geometry: rivets, ribs, plate seams, carved runes and cracks live in the textures. Alesk, Baira and Pyra are on the
new pipeline so far; the other two (Soul, Vex, 25-50k faces) are over budget and are redone ONE HERO AT A TIME after the designer approves the previous one.

All are built with `tools/blender/bl_kit.py` (materials, primitives, tapered curves, cloth ribbons, bake + export). Heroes are modelled at human size (about 1.8 m) or bigger for the giants; the viewer scales crafted models
by `CraftedScale` (0.7) so a hero stands taller than the 1 m hex, like an auto-battler champion.

## Placeholder-art pivot (2026-09-24)

The crafted models above are recognisable but read as primitive shapes stacked together (spheres, tapered limbs, swoop curves), not sculpted anatomy — there is no amount of extra scripting on this pipeline that
closes the gap to a real TFT/LoL champion (Garen, Sett). Decision: stop chasing bespoke quality with procedural bpy models for now. Use free, rigged, AAA-quality base meshes (Epic's Paragon character roster,
free via Fab / the Epic Games Launcher Marketplace, or Mixamo as a fallback for extra variety) as stand-ins for all 30 champions, get the game itself (UE integration, gameplay, UI, animations, sounds, playtest
polish) working well, and swap in bespoke per-champion art later (most likely AI image-to-3D from the splash arts: Meshy / Tripo3D / Rodin, since procedural primitive modelling is ruled out for final quality).

What this unlocks: it finally un-defers the "Mixamo/skeletal" roadmap item. The unit actor needs a real refactor from the current static-mesh-only system to `USkeletalMeshComponent` + an AnimBP driven by combat-log
events (attack/cast/move/hit/death), instead of the current rigid-mesh windup/flight visualization. Map the 30 champions onto a handful of Paragon archetypes (tank/bruiser, marksman, mage, assassin, support);
reuse across champions is fine for placeholders, differentiated with a material colour tint per hero. Standardize on one skeleton (retarget via UE5's IK Retargeter if the chosen characters don't already share
one) so a single animation set drives every placeholder hero. Keep the mapping config-driven (id -> mesh/skeleton/material), same principle as "a crafted model replaces the blockout of the same name" today, so
a later bespoke model is a data change, not a rewrite. Alesk, Baira, Pyra, Rot stay as-is (already-done work, not wasted); Soul and Vex do NOT get the LOD treatment now — this replaces that plan for the remaining
heroes until the game itself is solid.

## Making one

Blender runs as a Python module (`bpy`), headless, so no Blender install is needed:

```
python3.11 -m venv ~/w2f_bpy/venv && ~/w2f_bpy/venv/bin/pip install bpy          # once (Python 3.11: e.g. the one inside Unreal Engine's Engine/Binaries/ThirdParty/Python3)
~/w2f_bpy/venv/bin/python tools/blender/<hero>.py --preview /tmp/previews       # model + bake + export (about a minute each); previews are PNG renders
~/w2f_bpy/venv/bin/python tools/blender/<hero>.py --no-bake --preview /tmp/p           # geometry only, seconds: for iterating on the shape
```

The script builds the parts from beveled / subdivided primitives, joins them, unwraps, BAKES the worn-metal albedo (with ambient occlusion) and the emission of the runes into two 2048 px textures, puts them on one material
and exports a GLB (Blender -Y forward becomes glTF +Z, the facing every W2F model has; feet at 0; metres). In Unreal, run `tools/unreal/setup_viewer.py` (see `docs/blockouts/README.md`) to import it,
and `UnrealEditor ... -game -w2fshowcase=9001 -w2fshot=6` to look at it up close.

## The high-to-low pipeline (`finalize_lod`, Alesk first)

A hero script defines `build()` and ends with `finalize_lod("Alesk", "SM_Champion_9001_Alesk.glb", build, budget=8000)`. The kit's primitives honour `bl_kit.LOD`: `build()` runs twice, as **"high"** (bevels, subdivision,
`micro=True` parts such as rivets and ribs, tube runes and cracks: about 70k triangles, only a bake source) and as **"low"** (the game model: micro parts and `tube()` are dropped, bevels/segments cut down, `keep=True`
keeps a shape's bevel + one subdivision level, `low=(seg, rings)` sets the segment count of a dome so heads, pauldrons and shields stay round). The script exits if the low model is over budget. The low model is
unwrapped (smart project, one texel density, tight pack) and the high model is baked onto it, selected-to-active, at **4096 px and box-filtered down to 2048 px** (clean rune edges): albedo (ambient occlusion multiplied in),
tangent-space NORMAL and emission. Rules learnt the hard way: (1) every detail that is baked must stand less than the cage extrusion (0.055 m) above the low surface, or the bake rays start inside it, hit its back face and
store an inverted normal (black dots): keep studs shallow and put cracks/bolts ON the surface (`plate_out()` in alesk.py); (2) tiny (< 6 px) sculpted details turn into noise, make studs chunky; (3) procedural wear
materials (Pointiness) are unstable on tiny spheres, use a flat material for micro parts.

In Unreal the glTF normal map is imported with `TC_NORMALMAP`, sRGB off and **flip green** and the model gets a `MI_` instance of **`M_CraftedPBR_N`** (`M_CraftedPBR` + a `NormalTexture` parameter whose default MUST be
the engine's `DefaultNormal`, or the material fails to compile and the mesh renders default grey). Models without a normal map keep using `M_CraftedPBR`. `W2F_SS=1` makes the bake 4x faster while iterating (no supersampling).

Hero-script tips (from Baira): thin strands (hair, coral, crown spikes) get `low_u` / `low_res` / `low_scale` on `swoop()` so the game model has a few chunky tubes instead of dozens of thin ones, and the rest are `micro=True`
(baked from the source); fin / cloth `ribbon()`s must be IDENTICAL in both passes (`subsurf=0`), otherwise the bake rays miss their edges and leave black borders; floating VFX (`tube()` swirls, extra rings) exist only in the source.
`is_low()` tells `build()` which pass it is in.
