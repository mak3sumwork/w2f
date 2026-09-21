# Crafted models

Hand-crafted, textured models that replace the generated blockout of the same name (same `SM_Champion_<id>_<Name>` asset name). `tools/unreal/import_blockouts.py` imports every `*.glb` here after the
blockouts, gives it a fresh material instance (`M_CraftedPBR`: base colour + emissive from its own textures) and does NOT put the vertex-colour blockout material on it.

| Model | Made by | After |
|---|---|---|
| `SM_Champion_9001_Alesk.glb` | `tools/blender/alesk.py` | `splash_arts/Alesk.jpg`: a dark-iron golem, ribbed domed pauldrons with rune discs, a helm with glowing teal eyes, a teal gem in the breastplate, a cracked tower shield, huge gauntlets |

## Making one

Blender runs as a Python module (`bpy`), headless, so no Blender install is needed:

```
python3.11 -m venv ~/w2f_bpy/venv && ~/w2f_bpy/venv/bin/pip install bpy          # once (Python 3.11: e.g. the one inside Unreal Engine's Engine/Binaries/ThirdParty/Python3)
~/w2f_bpy/venv/bin/python tools/blender/alesk.py --preview /tmp/alesk_previews       # model + bake + export (about a minute); previews are PNG renders
~/w2f_bpy/venv/bin/python tools/blender/alesk.py --no-bake --preview /tmp/p           # geometry only, seconds: for iterating on the shape
```

The script builds the parts from beveled / subdivided primitives, joins them, unwraps, BAKES the worn-metal albedo (with ambient occlusion) and the emission of the runes into two 2048 px textures, puts them on one material
and exports a GLB (Blender -Y forward becomes glTF +Z, the facing every W2F model has; feet at 0; metres). In Unreal, run `tools/unreal/setup_viewer.py` (see `docs/blockouts/README.md`) to import it,
and `UnrealEditor ... -game -w2fshowcase=9001 -w2fshot=6` to look at it up close.
