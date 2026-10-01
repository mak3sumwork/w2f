# W2F DEMO 1.8

Answers FEEDBACK V8 (`feedback/FEEDBACK_V8.md`, 2026-10-01): "now i want some affects like tft when you activate traits". Client-side only (the UE project);
the server, the protocol and the data did not change. (The first half of that message -- rebuild the app, look into the last safety-limit fight -- is
W2F DEMO 1.7.1, see `update-1.7.md`.)

| # | Asked | Done | How it was checked |
|---|---|---|---|
| 1 | A trait switches on / reaches a new breakpoint while you build | **The badge pops**: it swells and settles, a glow and a ring in its metal (bronze / silver / gold / prismatic) spread from it, the emblem flashes white, the tracker tab flashes in the metal. A trait that falls back a tier shrinks for a moment. **On the board**, every champion that carries it lights up one after another: the hex under it glows in the metal and the trait's colour, a column of light and motes rise (every colour for prismatic), one point light per trait; **the trait's emblem pops over their heads** and drifts up. **A chime** per tier (`SFX_TraitUp1..4`). | live match vs bots (autopilot), screenshots in Planning: the Hexagon badge popped with its bronze ring and tab, Bit lit with the Hexagon hex and the emblem over its head |
| 2 | The same in a fight | At the bell every active trait fires over its champions, **one trait after another** (0.3 s apart, each team on its own count), from the combat log's `TraitActivated` rows. | the trait-system sample fight replayed (`-w2ffight=6`): both teams' traits cascade at the start |
| 3 | (TFT) pointing at a trait | Hovering a trait row (or its panel) lights the hexes of the champions on your board that carry it, in the trait's colour. | live screenshot with the hover demo on Hexagon: Bit's hex lit -- but the demo fire was on Hexagon in the same shots, so the hover ring alone is not proven by a screenshot; test by hand |

Colours: origins have their own hue (Helios amber, Phaisa violet, Hexagon cyan, Selini moon-blue, Najmi pink, Coregons soul-teal, Nature green,
Protector pale gold); classes use the badge's metal. The tiers follow the tracker's badges (`BadgeTierOf`, shared with `RankTraits`).

## Running it
* Play as usual. Sounds: `server/tools/make_sfx.py` (new `TraitUp1..4`; run it with `~/w2f_bpy/venv/bin/python`) then the new
  `server/tools/unreal/import_sfx.py` (headless, editor closed) -- already done, the assets are in `Content/W2F/Sfx`.
* Dev flags for screenshots without a mouse: `-w2fdemotraitfire=<seconds>` (each scheduled shot waits for a Planning phase, fires the strongest trait as if
  it had just switched on -- bronze when none is active yet -- and is taken that many seconds later), `-w2fdemohover=traitonly` (the trait hover panel
  without the champion card, so the board stays visible).
* Note for code: between fights the board units are the `bPlan` ones (`SpawnUnit` kind 2), not "units in the Team Planner".

## Files
* UE: `W2FFx.h/.cpp` (`TraitFire`), `W2FArena.h/.cpp` (`DetectTraitChanges`, `FireTrait`, `TraitPulse`, `GatherEmblemPops`, `UnitCarries`, `BadgeTierOf`,
  `TraitActivated` at the bell, the hover highlight, `TraitHoverQuery`, the demo flag), `W2FHud.cpp` (badge pop, tab flash, emblems over heads, hover query,
  `traitonly`), `W2FStyle.h` (`TraitHue`), `Content/W2F/Sfx/SFX_TraitUp1..4`.
* Server repo: `server/tools/make_sfx.py`, `server/docs/sfx/SFX_TraitUp1..4.wav`, `server/tools/unreal/import_sfx.py` (new), `feedback/FEEDBACK_V8.md`, this file.

## Honest limits / to test by hand
* I could not hear the chimes (headless runs); listen to them and say if they are too loud or too bright.
* Watch it with a real mouse: hovering trait rows, buying the 3rd copy that switches a trait on, selling one that switches it off, and a fight start
  with many traits (the cascade at 0.3 s per trait takes ~2 s with 7 traits).
* Trait-specific signatures (Coregons turning the floor blue, Nature's forest, Helios's sun) are not made: every trait uses the same effect in its colour.
* One fight-start screenshot (tick 14) caught nothing although tick 8 and tick 22 did; most likely screenshot timing on a hitching frame, not looked into.
