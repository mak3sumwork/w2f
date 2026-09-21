# Blockout models (placeholder 3D art)

Low-poly stand-ins for every champion, summon and PvE monster, so a UE5 client can be built and tested before real art exists. They are **generated from the data**
(`data/champions.json`, `data/pve.json`) by `tools/make_blockouts.py`: a new champion automatically gets a model the next time you run `make blockouts`.
`contact_sheet.png` shows all of them (each cell is labelled with the champion id).

| File | What |
|---|---|
| `SM_Champion_<id>_<Name>.glb` | the 30 champions (about 300-600 triangles each) |
| `SM_Summon_9101_Skeleton.glb`, `SM_Summon_9102_LostSoul.glb` | fight-only summons |
| `SM_Monster_100xx_<Name>.glb` | the PvE monsters (Gloop, Spitter, Boulder, Elder Wraith) |
| `SM_HexTile.glb` | one board tile: pointy-top hexagon, 1 m across the flats, top face at height 0 |
| `SM_ProjectileOrb.glb` | a 12 cm ball for projectiles |
| `SM_AreaDisc.glb` | a flat disc of radius 1 m, translucent: scale it to show a spell area (circle / cone / line shapes) |
| `manifest.json` | per model: id, name, file, height/width/depth, main colour, and **sockets** (see below) |

**Conventions.** Metres, +Y up, feet at the origin, facing +Z (the glTF convention; the UE importer converts to centimetres, +Z up, facing +X). Bodies grow with the
cost (1-cost about 1.5 m, 5-cost about 2 m; `height_m` in the manifest includes staffs, hats and floating orbs, so up to 2.8 m); the tanks are broader. The model is ONE static mesh with no skeleton: animate the whole actor
(lunge for an attack, squash for a hit, rise and glow for a cast). The colours are **vertex colours**: each synergy has a colour (Helios orange, Phaisa purple,
Hexagon teal, Coregons pale green, Selini blue, Najmi pink, Omnilium gold, Protector steel, Assassin dark accent), a second trait becomes the secondary colour,
undead (Coregons) champions have bone-coloured skin. The glTF material `M_Blockout` also carries the main colour as a fallback.

**Sockets** (manifest `sockets_m`, in metres on the model's centre line, `forward` along the facing direction and `up` from the feet): `feet`, `chest`, `cast_origin`
(where a spell effect starts), `muzzle` (where a projectile leaves), `head_top`, `overhead` (health bar / star marker). In UE add a `SceneComponent` per socket at
`(forward, 0, up) * 100` cm, or use them as offsets when spawning projectiles and effects.

**Board layout.** The arena is 7 columns x 8 rows, "odd-r" (odd rows shifted half a hex to the right). With `SM_HexTile` (1 m across the flats): centre of cell
`(column, row)` = `Y = (column + 0.5 * (row & 1)) * 100 cm`, `X = row * 86.6 cm` (1.5 x the hex radius 57.7 cm). Rows 0-3 are one side, rows 4-7 the other.

## Importing into Unreal Engine 5.8

1. **glTF support.** Unreal 5.x imports `.glb` through Interchange. Drag one `.glb` from Finder into the Content Browser to test. If UE says it cannot import the file, enable
   *Edit > Plugins*, search "glTF", enable the glTF importer / Interchange glTF plugin, and restart the editor.
2. **Automatic (recommended).** Enable *Edit > Plugins > "Python Editor Script Plugin"* (and restart) if it is not on yet. Then in the editor choose *Tools > Execute Python Script...*
   and pick `server/tools/unreal/import_blockouts.py` from this repository (or type `py "/Users/<you>/Desktop/work2fight/server/tools/unreal/import_blockouts.py"` in the
   Output Log's Cmd box). It imports every model into `/Game/W2F/Blockouts`, creates `/Game/W2F/Materials/M_BlockoutVC` (Base Color = vertex colour) and puts it on all of them.
3. **Manual fallback.** Drag all `SM_*.glb` into `Content/W2F/Blockouts`; in the import dialog turn on **Import Vertex Colors** (Replace) if there is such an option.
   Create a Material `M_BlockoutVC`: add a **Vertex Color** node, connect its **RGB** output to **Base Color**, save. Open each mesh and set its material slot to `M_BlockoutVC`
   (select all meshes in the Content Browser, right-click > *Asset Actions > Bulk Edit via Property Matrix* > Static Materials makes it one step).
4. **Check.** Drag `SM_Champion_9014_Pyra` into a level: an orange archer about 1.5 m (150 units) tall, facing +X (the red arrow of the actor). If it is one flat colour
   the vertex colours were not imported (repeat step 3 with vertex colours on). If it is mirrored, that is only cosmetic (the weapon hand): fix it with a Y scale of -1 on the
   mesh component.
5. **Team colours later.** Make `M_BlockoutVC` multiply its colour by a `TeamTint` vector parameter and drive it from a Material Instance Dynamic per unit.

**Not tested in the editor here:** the generator validates its own output (well-formed glTF, index and colour ranges, unit normals, plausible size) and the contact sheet is
rendered from the very files it wrote, but the Unreal import script was written against the 5.x Python API without an editor at hand. If a call fails, the Output Log names
the line: send it to me and I will fix the script.
