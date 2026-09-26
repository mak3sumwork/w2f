# W2F DEMO 1.4 (2026-09-26)

The answer to FEEDBACK V4 (`feedback/FEEDBACK_V4.md`), the designer's playtest of DEMO 1.3. The server and the rules did not change. "Measured" / "screenshot" = checked by the
developer in the running game; "self-test" = the virtual-mouse test (`-w2fselftest` against `w2f_server --bots 7 --give-items`); things that need a real mouse are marked
**to test by hand**.

| # | Feedback | Change | Checked |
|---|---|---|---|
| 1 | Runs at ~24 fps | Measured first: the game thread took ~2.5 ms, the GPU ~31 ms (Lumen with ray tracing, virtual shadow maps, full-resolution TSR). New **Settings** menu (gear button, top right of the match and of the client) with **Low / Medium / High / Ultra**, saved between sessions. **Medium is the default**: it looks almost the same as the old look. Presets = UE's scalability groups + render resolution (Low 60%, Medium 80%, upscaled by TAA / TSR) + shadows (virtual shadow maps only on High / Ultra) + Low drops the effects' point lights and half the small particles. The busiest recorded fight (06, 1600 x 900): **Low ~120 fps, Medium ~58 fps, High ~28 fps** (the old setting ~25). A live match at Medium: 55-73 fps. | measured (`-w2ffps` logs fps, worst frame, game / render / GPU ms) |
| 1 | Settings | Also an **FPS counter** (on / off) and the controls at a glance. | screenshot |
| 2 | Camera too low: the enemy bench is cut off | The default camera shows the whole arena, both benches included. **Mouse wheel = zoom** (smooth; closer in it aims a little nearer the middle). | screenshots (default, zoomed in) |
| 3 | Item slots should be replaceable | The ten item slots are now yours to arrange: drop an item on an **empty slot** to move it there, on **another item** to swap them -- or to forge them if they are two components that make an item. While you carry an item the empty slots light up. | self-test (forge); moving / swapping **to test by hand** |
| 4 | Equipping only works over the head / lands on the wrong unit | Units are now hit by their **whole body on screen** (a capsule from the feet to the top of the head, the closest body wins), for picking up units and for dropping items. The item pedestals no longer steal a click that is on a unit's body next to them. | self-test: equip + merge on a unit PASS; **to feel by hand** |
| 5 | Trait / unit / damage menus vanish, want to check details like TFT | New **hover panels**: hovering a trait, a shop card, a damage-chart row, a player or a planner portrait opens a details panel **beside it**, which **stays while the cursor moves into it** (0.3 s grace between the row and the panel). The trait panel lists its breakpoints (the active one lit) and every champion that carries it; **hovering one of those opens its champion card** next to it (splash, rank, cost, traits, a stat strip, the ability, the passive). | screenshots (`-w2fdemohover=trait|card|damage`); **the feel of it by hand** |
| 6 | Damage chart: show the unit's name | Every row shows the champion's name next to its number; hovering a row opens its breakdown (physical / magic / true, health lost / absorbed, healed / shielded). | code; screenshot of the row |
| 6 | Menus lag / open badly | Panels open with a short fade and slide (0.12 s); the unit details panel slides in when you pick a unit. The frame rate fix above helps here too. | screenshots |
| 7 | Map and units a little dark | Exposure +0.45 stops over DEMO 1.3, sky light stronger. | screenshots |
| 8 | A cursor like the old League one | A **golden gauntlet arrow** with a blue gem (original art, `server/tools/make_cursor.py` -> `Content/W2F/Cursor`), and a closed gauntlet while you carry a unit or an item. | art preview; **to see by hand** (hardware cursors do not show in screenshots) |
| 9 | Planner sign on the card's top left | Planned champions' shop cards show a cyan **PLAN** tag with the clipboard in the **top left** (the star-up badge moved to the top right). | screenshot |
| 10 | Click to pick up, click to drop | **Click** a unit (or an item) and it rides the cursor until the **next click** puts it down; press-drag-release still works. Picking a unit up shows its details. **Esc** puts it back. **Right-click** a unit: its details (anywhere else the Little Legend still hops there). | self-test for drag; click-carry **to test by hand** |
| 11 | Scouting shows my units | Like TFT: scouting shows **their** arena -- their board on the near half, their bench on your bench -- and hides your units, items and Little Legend. A banner says whose board it is; **Space** (or a click on the banner / your portrait) brings you back; a fight brings you back by itself. | screenshot (`-w2fscout`) |

Also: **Esc** closes the open panel (settings, planner) or ends scouting; the effects' flat quads no longer log a Nanite warning (no change needed: they were not Nanite).

## Files
- Client (Unreal): `W2FHud.h/.cpp` (hover panels: `FW2FHoverState`, champion card, trait / damage / player panels, settings gear, FPS counter, scouting banner, info panel
  animation, empty item slots, planner tag), `W2FSettings.h/.cpp` (new), `W2FPlanner.h/.cpp` (portraits open champion cards), `W2FClient.cpp` (settings gear),
  `W2FArena.h/.cpp` (settings + presets, camera zoom, click-carry, item slots, body hit test, right-click details, scouting, cursor, exposure, `ChampionCard`, `TraitRowFor`,
  dev flags `-w2ffps -w2fquality= -w2fzoom= -w2fscout -w2fsettings -w2fdemohover=`), `W2FFx.h/.cpp` (detail level), `work2fightgame.Build.cs` (RHI, RenderCore for the
  GPU timing), `Config/DefaultGame.ini` (stage `W2F/Cursor`), `Content/W2F/Cursor/*.png`, `Content/W2F/Icons/T_UI_Gear`.
- Server repo (tools only): `tools/make_cursor.py` (new), `tools/make_hud_art.py` (gear), `tools/unreal/import_fx.py` (Nanite off for effect meshes on re-import).

## To test by hand
* Settings: switch Low / Medium / High / Ultra and see the frame rate (FPS counter on); it should be remembered next time.
* The mouse: click-to-carry and click-to-drop for units and items; dragging an item onto a unit anywhere on its body; moving / swapping items between slots; right-click
  details; the new cursor; the wheel zoom.
* Hover panels: move from a trait row into its panel and onto a champion icon; shop cards; damage rows; players.
* Scouting: click a player, Space to come back.
