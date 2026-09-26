# W2F DEMO 1.0 (2026-09-26)

The first playable demo: the Unreal client (repo `work2fightgame`, local) against `w2f_server` with 7 bots. The baseline FEEDBACK V1 (`feedback/FEEDBACK_V1.md`) was written against.
Everything below was uncommitted work since the last commits and goes into this version.

## Game rules and content (server, `server/`)
- Trait system v2 (GDD section 2B): 49 pooled champions (11/10/10/10/8), class traits on every champion, the Nature plants, Hexagon modules + Hexa, Najmi star dust and prototypes,
  Selini's random path per match, Phaisa mutations / Rift Herald / Queen, Gunslinger (Rivet), Omnilium Orb + 7 class emblems.
- Protocol revision 5 (`pick_trait_choice`, `trait_choice`, `trait_rewards`, ...), snapshot format v6, JSON Schemas updated.
- Balance pass after trait v2 (`server/docs/balance.md`); sample fights 05-07.

## Unreal client
- 58 animated Mixamo stand-in heroes (`server/tools/mixamo/`), fight VFX for every basic attack and ability, hit flashes, camera shake.
- The arena: a TFT-style hilltop plaza as the default (`server/tools/blender/arena_classic.py` + `paint.py`: painted floor, stone terraces, brazier pillars, lanterns,
  a wooded valley with a river) and the space platform as an option (`-w2farena=space`, `arena_space.py`).
- A TFT-style HUD (`W2FHud.cpp`): stage bar with round icons and timer, hex trait badges, round avatars with health, the shop with level / XP / tier odds / gold / streak / lock.
- The Little Legend "Pip" (`server/tools/blender/legend.py`): idles, hops where you right-click, name plate with level and health.
- Items as orbs over ten pedestals on the left side of the board.
- Splash art and painted portraits for all 58 units (`server/tools/mixamo/render_art.py`); the designer's six hand-made splash arts win.
- UI glyphs (`server/tools/make_ui_icons.py`).

## How to play
`cd server && ./build/w2f_server --bots 7`, then start the UE project with `-game -w2flive` (map `/Game/W2F/Maps/L_Viewer`).
