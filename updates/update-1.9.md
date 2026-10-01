# W2F DEMO 1.9

Answers FEEDBACK V9 (`feedback/FEEDBACK_V9.md`, 2026-10-01): "comit it. i want better cleint just like current tft client. i dont like the current one. need a
big client overhaul. affects,sound etc. then commit again. update the mac game. add a icon to the game app". DEMO 1.8 was committed first (server c921b74,
UE 854423b). This demo rebuilds the out-of-match client (the UE project), adds two small things to the server (protocol revision 9), new art, sounds and
music, and gives the Mac app its icon.

| # | Asked | Done | How it was checked |
|---|---|---|---|
| 1 | A client like the current TFT client | **New layout**: the big hextech **PLAY** button top left (it breathes, a sheen sweeps over it; it reads LOBBY / IN QUEUE 0:12 / FOUND! as you go), the W2F crest, the tabs HOME / COLLECTION / PROFILE with a gold underline and a teal glow, the server status, settings, log out. **Social column on the right** like the TFT client: you on top (profile icon in a gold ring, name, live status: Online / In lobby / In queue / In a match, rank emblem), then SOCIAL with the add box, requests and the GENERAL (online/total) list; each friend has their icon in a ring in their status colour. Every panel is a gold-rimmed frame, buttons are hextech plates. **Login** is the Riot client's: a sign-in column on the left over the live stage. | screenshots of every screen (login, home, collection, profile, mode select, lobby, queue, ready check, loading screen, icon picker) |
| 2 | The PLAY flow | TFT's: **PLAY -> choose your mode** (three tall tiles with splash art in gold frames; the chosen one rises and glows) -> **CONFIRM -> the lobby** (your seat with your icon and rank, seven open seats that pulse while searching, the last match's result card: placement, VICTORY / TOP 4, LP) -> **FIND MATCH** turns into the queue box (spinning ring, timer, estimate, ✕) -> **MATCH FOUND**: the **ready check** -- rotating rays, a dial that drains over 10 s (teal, gold, red), ACCEPT! / DECLINE, one pip per player that lights when they accept; the alarm repeats and the window flashes / the dock icon bounces -> **the loading screen**: every player's card (their icon champion's art, name, YOU / AI TACTICIAN / PLAYER) sliding in one after another over a hex pattern, then the arena. After a match you land back in the lobby of the same mode. Solo vs AI has no ready check (it starts at once). | client self-test: PLAY, NORMAL + CONFIRM, FIND MATCH -> ready check, DECLINE -> back in the lobby, FIND MATCH -> ACCEPT! -> loading screen with 8 seats; screenshots of each |
| 3 | Profile icons (TFT has them) | **CHANGE ICON** on your profile opens a grid of every champion; the one you pick is your icon on your profile, in the social column, in your friends' lists, in the lobby, on the loading screen and on the in-match scoreboard. Never chosen = a champion picked from your name. | self-test: the picker opens, clicking Alesk sets it and the server confirms |
| 4 | Effects | Sparks rising through the client, pages slide up and fade in, buttons light on hover and press in on click, the PLAY glow and sheen, the selected mode's glow, the ready check's rays / dial / pop-in title, staggered loading cards. | screenshots (the motion itself: by eye) |
| 5 | Sound | New sounds (`tools/make_sfx.py`, synthesised): hover tick, tab click, PLAY whoosh, confirm / back, lobby enter, queue start / cancel, **MATCH FOUND fanfare** (repeats until you answer), accept, decline, welcome (log in), friend request ping, icon set, loading. **Client music**: a 48 s ambient loop (pads, drone, bell arpeggio) that plays while the client shows and fades out in the match; Settings -> "Music in the game client" switches it off (saved). | I cannot hear them here: levels checked by peak / RMS only |
| 6 | Commit, update the Mac app, an icon for the app | Committed as W2F DEMO 1.9 (both repos). The app is rebuilt; `scripts/package_mac.sh` now writes the project's own asset catalog (`Build/Mac/Resources/Assets.xcassets`) from `docs/app_icon/W2F_1024.png`: the W2F crest (gold hexagon, crossed swords, teal gems) on a dark rounded square. | see "Mac app" below |

## Server: protocol revision 9 (additive)
* **Ready check**: `queue` takes `ready_check: true`; the made match waits for `accept_match` from everyone (`match_found` carries `ready_check`, `accept_ms`;
  progress comes as `ready_check`). A decline, the 10 s timeout, leaving, logging out or disconnecting sends the decliner idle (`declined`) and puts the others
  back in the queue in their old place (`requeued`). Clients that do not ask for it behave exactly as before.
* **Profile icons**: `set_icon` (any pooled champion), `icon` in `auth` / `profile` / `friends`, `match_started.player_icons`.
* Docs: `docs/network-protocol.md` "Ready check and profile icons", `docs/UE5-Integration.md` revision history, the JSON Schemas; tests in `net/tests/net_tests.cpp`.
  `make test`: 6436 engine + 2007 network checks, also with `-Werror -fno-exceptions -fno-rtti`.

## Running it
* The app: `~/Desktop/W2F_App/W2F.app` (starts its own server). In the editor: `w2f_server --queue --accounts FILE` + `-w2flive`.
* Art: `~/w2f_bpy/venv/bin/python tools/make_client_art.py` (`--2d` for the flat pieces only), then `tools/unreal/import_ui.py`. Sounds: `make_sfx.py`, then
  `tools/unreal/import_sfx.py` (both headless, editor closed; done, the assets are in the UE project).
* Dev flags: `-w2flobby` (the PLAY tab opens on the lobby), `-w2fqueue=normal` now also accepts the ready check after 3 s.
* `scripts/client_selftest.sh` runs its server with `--fill-seconds 5`; 34 / 34 steps pass (8 new).

## Files
* UE: `W2FClientUi.cpp` (new: the whole widget, `SW2FEmbers`, `SW2FDial`), `W2FClient.cpp` (the logic: lobby, ready check, icons, loading screen, music,
  `UiBox` 9-slice brushes), `W2FArena.h/.cpp` (state, `ready_check` routing, `player_icons`, scoreboard icons, `-w2flobby`), `W2FSettings.cpp` (Music),
  `W2FClientTest.cpp` (8 new steps), `Content/W2F/Icons/T_UI_Client*`, `Content/W2F/Sfx/SFX_*` (15 new), `Build/Mac/Resources/Assets.xcassets`.
* Server repo: `net/` (revision 9), `docs/schemas/`, `docs/network-protocol.md`, `docs/UE5-Integration.md`, `tools/make_client_art.py` (new),
  `docs/icons/T_UI_Client*.png`, `docs/app_icon/W2F_1024.png`, `tools/make_sfx.py`, `docs/sfx/SFX_*.wav`, `scripts/package_mac.sh`, `scripts/client_selftest.sh`,
  `feedback/FEEDBACK_V9.md`, `CLAUDE.md`, this file.

## Honest limits / to test by hand
* Listen to the sounds and the music; say if anything is too loud, too bright or annoying (the hover tick plays on every button).
* The lobby's seven other seats are placeholders: there are no party invites yet (friends can see you but not invite you). Store, loot and missions of the
  TFT client are not made.
* The ready check was tested with one human and AI fill; two humans accepting / declining against each other is covered by the server tests, not by a UE run.
* The match HUD itself was not redesigned in this demo (only its scoreboard shows the profile icons).
