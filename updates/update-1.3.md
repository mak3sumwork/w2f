# W2F DEMO 1.3 (2026-09-26)

The answer to FEEDBACK V3 (`feedback/FEEDBACK_V3.md`): a big polish pass on the UI, the VFX, the ability visuals and the graphics, a HUD that looks more like TFT's,
and TFT's Team Planner. The server and the rules did not change (no protocol change; the server tests are untouched). "Screenshot" = seen in the running game by the
developer (live matches against 7 AI, and close-ups of the recorded sample fights); things that need a real mouse are marked **to test by hand**.

## UI: a TFT-style overhaul (Unreal project, `W2FHud.cpp`, new `W2FStyle.h`)
| Area | Change | Checked |
|---|---|---|
| Type | Titles and numbers use a flared display serif close to TFT's (Cinzel, free OFL font in `Content/W2F/Fonts`); body text stays Roboto. Numbers over art get a dark outline. The game client (home / play / queue) uses it too. | screenshots |
| Trait tracker | Each trait is a metal hex badge in its tier (**bronze / silver / gold / prismatic**, dark when inactive) with the trait's own **emblem** (19 new emblems: sun, void claw, gear, moon, star, leaf, dagger, orb, crystal, shield, tower, hammer, staff, crosshair, eye, rapiers, revolver, hex bolt, sprout), the count in the tier's colour, the name and the breakpoints; an inactive trait shows "1 / 3". Hover: its text and every champion that carries it. | screenshots; hover **to test by hand** |
| Stage tracker | "STAGE 2-3" in the display font, the round icons in rings joined by a line (the current one big and gold), the timer turns red in the last 5 s, a thin phase bar and the phase name. | screenshots |
| Shop | Rebuilt like TFT's: the Team Planner button, level + XP and the tier odds on the left, the gold tab (with the streak) in the middle, the lock on the right; **Buy XP (F)** and **Refresh (D)** with their hotkeys and prices; bigger cards with the splash art, the traits with their emblems, the name and price in the display font. A card **glows gold with a ★★ badge when buying it completes a 2-star (★★★ for a 3-star)**, glows **cyan with a clipboard** when the champion is in your plan, brightens with a sheen on hover, shows the champion's ability on hover, and an empty slot shows a faint coin. | screenshots; hover **to test by hand** |
| Unit plates | TFT's plate: a tick every 300 health (a heavier one every 1000), a white **damage ghost** that drains after each hit, **shields as a white block** after the health, the mana bar glows when the ability is ready, the rank chevrons, the items in a row under the bar; planned champions' names show in cyan. | screenshots (bars); the ghost and shield **to see in play** |
| Floating numbers | Display font with an outline; crits pop in bigger, then settle and fade. | screenshots |
| Scoreboard, info panel, damage chart | Restyled: your health big and gold with a gold ring, hover highlights; the info panel shows the champion's traits with emblems, a cost pill, the ★ rank, the health and mana as labelled bars, the stat grid in 3 columns, the ability in its own box. | screenshots |
| Items on the arena | The orbs have a glassy shell and bob gently; **hovering an orb shows the item's card** (icon, name, what it does). | code; hover **to test by hand** |
| Tooltips | Every tooltip is a dark panel with a gold trim (Slate's default was a white box). | screenshots |
| Messages | No permanent help line any more; a refused action shows a short readable message ("Not Enough Gold") for a few seconds. | screenshots |
| Drag targets | Dragging a unit or an item lights the hex (or bench slot) it would land on (cyan for a unit, gold for an item) instead of a thin debug circle. | code; **to test by hand** |

New art: `server/tools/make_hud_art.py` -> `docs/icons/T_UI_Trait_*`, `T_UI_Badge*`, `T_UI_Planner`, `T_UI_Shine`, `T_UI_Orb`, `T_UI_Frame`, `T_UI_Vignette`
(imported with `tools/unreal/import_ui.py`, which re-imports only the UI art).

## The Team Planner (new, like TFT's)
* Open it with the clipboard button left of the level box, or **T**. Pick up to **10 champions**; every champion the shop can sell is listed by cost with its portrait
  (hover: traits and ability). Click to add or remove; the plan's slots are across the top; the plan's traits are counted as if it were on the board
  (badges in the tier colours, "3 / 5" toward the next breakpoint).
* Shop cards of planned champions glow cyan with a clipboard; planned units on your bench and board show their names in cyan.
* The plan is kept between games (`Saved/W2F/planner.json`). CLEAR empties it. The shop stays usable while it is open.
* Checked: screenshots (the panel, the plan's traits, cyan cards in the shop). **To test by hand**: clicking in the panel, T.

## VFX and ability visuals (`W2FFx.cpp`, `tools/mixamo/make_fx_textures.py`, `tools/unreal/import_fx.py`)
* **New effect textures** (16, 512 px, was 8 at 256 px): each carries a shape, an erosion field and a heat mask. New shapes: thin ring, flame tongue, spiral, slash crescent,
  four-point glint, hexagon, fireball puff, contact shadow; the old ones redrawn (rune circle with tick marks and a hexagram, a torn shock front, a domain-warped smoke cloud).
* **New materials**: sprites now **dissolve** through their noise as they age (smoke thins into wisps, rings and shock fronts tear apart) and have a hotter core; the effect
  meshes (shields, beams, domes, walls) carry **flowing world-space noise** instead of looking like plastic, and **fade softly where they cut the ground or a unit**.
* **Every champion's effects got richer building blocks**: impacts add a fireball puff that burns away, a four-point glint and lingering embers; ground shockwaves get a torn
  front and a crisp leading ring; spell areas get a turning rune circle at their rim; smoke erodes; sparks have hot cores; projectiles leave a second, wispy trail; melee
  slashes get a painted crescent in the swing's plane.
* **A rune circle blooms under every caster** while the spell winds up and tears away after it lands.
* **Signature layers by element** on top of the per-champion recipes: fire abilities (Ignis, Pyra, Flare, Kael, Raa, Aureon, Cyla, Solis) throw up burning flame tongues;
  void abilities (Null, Morrah, Nihila, Xul, Umbra, the Queen, Baron) open a turning dark spiral with motes drawn in; water (Baira, Tide, Myna) a whirlpool and spray;
  nature (Moss, Briar, Fern, Oakheart, Willow, Yggra, Thorn, Rot) green spirit-flames.
* The whole set was toned so bursts no longer blow out to white under bloom.
* **Ground marks**: units stand on a soft contact shadow and a faint team ring (cyan yours, red the enemy's; none on the bench); the selected unit gets a turning gold ring.
  These replace the old aliased debug circles.
* Checked: close-up screenshots of Alesk, Lum, Vega, Tide, Kael, Aureon, Nihila, Sola casts and whole fights (`-w2ffight`, `-w2fshotticks`, `-w2ffocusseq`). **To see in play**: the motion (dissolves, spirals, flames rising).

## Graphics
* **Camera**: closer and a little flatter, TFT framing: the board fills more of the screen and the bench sits right above the shop.
* **Lighting**: a softer sun (wider source angle: soft shadow edges) with contact shadows that pin the heroes to the ground; a sky light (sky atmosphere, real-time capture)
  gives the shadows a cool blue fill; a graded post-process (more contrast, warm highlights, cooler and richer shadows, slightly lower exposure, local exposure, a sharpening
  pass).
* **Arena** (`tools/blender/arena_classic.py`, re-imported with the new `tools/unreal/import_arena.py`): a deeper, cooler grass and stone palette (the old lime grass read as
  cheap), less moss on the boulders round the rim, smooth-shaded trees with rounder canopies and soft painted colour (the faceted cones and posterised bands are gone).
* Checked: screenshots. The trees are mostly off to the sides of the new camera.

## Files
- Client (Unreal): `W2FHud.cpp` (rewritten), `W2FStyle.h` (new: palette, fonts, brushes, tooltips), `W2FPlanner.h/.cpp` (new), `W2FArena.h/.cpp` (planner, card states,
  plate data: shields / ghost, ground marks, drop targets, lighting / post / camera, T key, `-w2fplanner`, `-w2fdemodrag`), `W2FFx.h/.cpp` (textures, erosion / heat,
  Mark, Flames, Vortex, upgraded building blocks, cast circles, element layers), `W2FClient.cpp` (fonts), `Config/DefaultGame.ini` (stage the fonts), `Content/W2F/Fonts`,
  `Content/W2F/FX` (textures, materials), `Content/W2F/Icons` (HUD art), `Content/W2F/Blockouts/SM_Arena*`.
- Server repo (tools and art only): `tools/make_hud_art.py`, `tools/mixamo/make_fx_textures.py` (new), `tools/mixamo/build_fx_meshes.py` (no longer writes the textures),
  `tools/unreal/import_fx.py` (materials), `tools/unreal/import_blockouts.py` + `import_ui.py` + `import_arena.py` (partial re-imports), `tools/blender/arena_classic.py`,
  `docs/icons/T_UI_*.png`, `docs/models/SM_Arena*.glb`.
- Dev flags: `-w2fplanner` (open the planner with a sample plan), `-w2fdemodrag` (the old `-w2fdemoui` drag, now opt-in).

## To test by hand
* The planner: open (button / T), add, remove, clear, close; the shop glow while you play.
* Hovers: shop cards (ability), trait rows, item orbs on the arena, planner portraits.
* Dragging units / items: the lit target hex.
* In motion: the new effects (dissolving smoke and rings, flames, spirals), the damage ghost on the health bars, shields on the bars.
