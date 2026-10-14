# W2F DEMO 2.0 -- big test report (2026-10-14)

Asked after FEEDBACK V11: "list alllllll the problems ... detailed bug/performance test and a detailed feed back". Splash arts are parked (user's call).
Nothing in the game was changed for this report; the only new files are this report and `feedback/FEEDBACK_V11.md`.

## What was run
| Test | Result |
|---|---|
| `make test` with `-Werror -fno-exceptions -fno-rtti` + ASan/UBSan | green: 6436 engine + 2133 network checks |
| Determinism: `scripts/fingerprint.sh`, sanitized -O1 build vs -O2 build | identical |
| `make balance` 1000 bot matches (88,850 fights) | no crash; numbers below |
| Item economy: 300 bot matches, every item every player received, by source and kind (scratch tool) | numbers below |
| UE client: a whole match on autopilot (`-w2fauto -w2ffps`), FPS logged every 2 s, 12 screenshots | 131 samples, numbers below |
| UE client at Retina resolution (`bAllowHighDPIInGameMode=True`), Ultra and Medium | numbers below |
| Network load test: 64 and 256 WebSocket clients (register, queue, play, `get_profile` round trips) | 64 fine, 256 fails (bug S1) |
| All 62 sound files measured (length, peak, loudness, brightness) | numbers below |
| Code review of the HUD hover panels, item slots, settings, toasts, selection | findings below |
| Packaged app launch (DEMO 2.0) | starts, own server, sign-in screen |

**Not tested** (needs you or a free Mac): the self-test's party / chat / settings / Esc steps (macOS gives the test window no focus while you use the Mac),
pop-ups with a real mouse, two humans in one party, the late game (stage 5+) in the UE client (the autopilot died at 4-4), listening to the sounds, Windows.

---

## A. Your V11 points, checked

### A1. "The client is not crisp, resolution should be better" -- ROOT CAUSE FOUND (high)
1. **Retina is off.** UE's `bAllowHighDPIInGameMode` is `False` (engine default; the project never turns it on). On your 2560 x 1664 Retina screen the
   game renders at half the pixel density and macOS stretches it 2x: every text, icon and edge is soft. Proof: a 1600 x 900 window gave 1280 x 720 screenshots.
2. **Medium is the default and renders at 80%.** `LoadSettings()` defaults to quality 1 = `r.ScreenPercentage 80` + TSR upscale. Low = 60%.
3. Together: the 3D scene is drawn at about 40% of your screen's pixels per axis, then blown up twice.
4. Cost of fixing it (measured on your M4, Retina on): **Medium 60 FPS** (was 85), **Ultra 29 FPS** (GPU 35 ms). So crisp needs real performance work too (A5).

### A2. "Pop-up tabs don't disappear when they should" -- likely causes found, NOT reproduced (high)
The match HUD has 4 kinds of pop-ups: hover panels (trait, shop card, damage row, player; `FW2FHoverState`), Slate tooltips on buttons, the item card at an
orb, and the selected-unit panel. What can keep one open:
1. Hover panels close when Slate's `IsHovered()` turns false. Slate only updates that when the **mouse moves**: a card that changes or disappears under a
   still cursor (you buy the champion: the panel stays over an empty slot; the shop rerolls; a trait row re-sorts) keeps its panel. The 0.3 s grace and the
   "sub panel keeps the parent open" rule add to it.
2. The selected-unit panel closes only on Esc, a click on the board, right-click on the same unit or when the unit is gone. A click on the HUD never closes it.
3. **Tell me which pop-up sticks** (a screenshot is perfect): a real-mouse test is the only way to be sure. Fix direction: test the cursor against the
   source's rectangle every frame instead of trusting `IsHovered()`, close on any click outside, close all pop-ups on phase change.

### A3. "Item slots: not aligned, not consistent" -- CONFIRMED, 4 bugs (high)
1. **Items past the 10th are invisible.** The server's bag has no limit, the client draws 10 orbs (`kItemSlots`) and `SyncItemSlots()` silently drops the
   rest: you cannot see or use them. The autopilot had 10 items by stage 2-2; players get about 22 items a match (A4).
2. **Slots are hard-coded to the classic arena** (`ItemSlotWorld` copies Blender numbers). The **space arena has no item pedestals at all**: the orbs float in space.
3. Two staggered columns (x -4.5 / -5.2) + every orb bobbing with its own phase = a zig-zag that never lines up; one orb sat above the platform in a screenshot.
4. The pick radius (3.2% of the screen height) is bigger than the orb spacing: hovering between orbs picks the wrong one.

### A4. "Do we drop a lot of items or emblems?" -- YES, about twice TFT, and many more emblems (high)
Measured, per player per match (300 bot matches, 33.7 rounds):

| Source | Components | Finished items | Emblems | Item Removers |
|---|---|---|---|---|
| PvE drops | 7.2 | 4.5 | **2.6** | **3.7** |
| Mother Nature gifts | 2.8 | 1.5 | 0.1 | 0 |
| **Total** | **10.0** | **6.0** | **2.7** (2.96 counting crafted) | **3.7** |

That is about **29 component-equivalents (~14 finished items) per player per match**. Held at round start: 7.8 by round 5, 13.4 by round 12, 21 by round 20, 29 by round 28.

**Why:** a PvE drop of "any item" (`"type": "Item"` with no list; every X-7 boss gives 2 guaranteed + 70% for another) picks **uniformly from all 55 items**:
8 components, 28 finished, 16 emblems. So 29% of those drops are emblems and 80% are finished items.

**TFT, for comparison (approximate; Riot publishes no exact tables):** PvE / loot orbs pay mostly components and gold, with completed items mainly late. A
player typically ends with roughly 6-9 completed items. Spatulas and emblems are rare (an occasional orb, the carousel, or an augment). Item removers come in ones and twos.

Effect in our balance run: **all 9 of the best-performing items are emblems or emblem-like**, because emblems are common enough to stack traits.
Fix direction: "any item" -> components only; finished items only from late bosses (1 per boss); emblems through a rare drop line (a few %); 1 Item Remover early, not 3.

### A5. "Add detailed graphics options" -- CONFIRMED missing + settings bugs (high)
Today: 4 presets (Low / Medium / High / Ultra) that set UE's scalability, screen percentage, AA and virtual shadows together. Missing: separate **shadows,
effects, textures, post-processing, anti-aliasing, view distance, resolution scale**, frame-rate cap per mode. Bugs found:
1. **The anti-aliasing choice is ignored**: the log says `r.AntiAliasingMethod ... SetByGameSetting was ignored ... lower priority than SetByProjectSetting`.
2. **Resolution list**: only 16:9 sizes (1280x720 ... 3840x2160) capped by the *logical* desktop size, so on your 16:10 MacBook you get 1280x720, 1440x810 and
   the desktop size, nothing native.
3. Window mode / resolution apply the moment you click, with **no "keep these settings? 15 s" revert**: a bad pick can leave you with a broken window.
4. Fullscreen modes force the desktop resolution; you cannot render lower.

### A6. "Make the menu like League's settings panel" (medium)
Today: a centred box with 4 tabs (VIDEO / AUDIO / CONTROLS / ACCOUNT). League's: a full-height panel, a left sidebar grouped as CLIENT (General, Notifications,
Chat, Sound, Hotkeys) and IN-GAME (Video, Sound, Interface, Game), rows of label + control, RESTORE DEFAULTS and DONE at the bottom, changes previewed and
confirmed. Our controls tab also misses the hard-coded **P** (auto-place: not rebindable, not listed).

### A7. "The sounds are so bad, arcade" -- CONFIRMED by measurement (medium)
62 sounds, all synthesised in `tools/make_sfx.py`. The ones you hear most are the harshest:

| Sound | Plays | Brightness centre | Energy above 4 kHz | Peak |
|---|---|---|---|---|
| Reroll | every reroll | 9.5 kHz | 68% | -5 dB |
| Purchase | store | 10.5 kHz | 65% | -4 dB |
| Crit | every crit | 11.0 kHz | 65% | -5 dB |
| CoinGain | every coin | 10.1 kHz | 62% | -7 dB |
| Cast | every spell | 9.1 kHz | 58% | -6 dB |
| HitProjectile | every ranged hit | 8.4 kHz | 58% | -8 dB |
| QueuePop | match found | 9.0 kHz | 47% | -1.4 dB |

Pure tones, no room or texture, very bright and mastered loud: that is the chiptune / arcade sound. League's UI sounds are soft, low-mid, wooden / crystal / cloth,
with a short tail. Fix direction (to decide): CC0 sound packs, or a re-synthesis that is darker, quieter and more layered.

---

## B. Bugs and problems found by the tests

### Server / network
* **S1 (high, blocks online play): only 64 sockets.** `TcpServerConfig::maxConnections = 64` and nothing raises it. The queue server allows 64 matches
  (512 players), but the 65th player cannot connect. With 256 clients: 64 played, 64 got "connection reset", 128 hung. Fix: a `--max-connections` flag,
  default a few thousand.
* Server performance itself is excellent: 64 players in 8 matches = **4% CPU, 6 MB RAM, round trip 0.7 ms median / 1.3 ms p99**.
* PvE is still no threat: bots win **98-100%** of every PvE round (1-1 .. 6-7). Boss rounds should bite.
* One fight in 88,850 hit the 120 s safety limit (Ignis vs Byte, neither could kill the other); 12.4% of fights need overtime; 1.5% run past 35 s.

### Balance (1000 bot matches)
* Strong: **Selini 5 wins 73.5%** (3: 58.3%), Protector 2 63.2%, Bastion 4 61.8%, Marksman 4 60.2%, Bruiser 4 59.1%, Assassin 4 58.8%; Yggra 60.2%, Nature plants 59.5%.
* Weak: Astra 45.0%, Null 45.6%, Grave 46.0%, Xul 46.3%.
* **Hexa is almost never seen** (19 fieldings in 1000 matches): the unlock (7 Hexagon stars at level 8) is too hard to reach.
* Weakest items: Selinis Gloves, Omnilium Vest, Phaisa Stick (44-47% top-4).

### UE client: performance (1280 x 720, Medium, M4)
* Average **85 FPS**, game thread 3.6 ms, render thread 11.4 ms, GPU 10.7 ms.
* **First planning round runs at 15 FPS for ~4 s** (game thread 66 ms): heroes and effects load on the game thread when the match starts.
* **Hitches of 130-400 ms several times per round** (shaders / meshes loading when first seen; no PSO cache).
* **The arena's component count only grows** (5 -> 117, still 117 with 0 units): something is created per round and never destroyed (a leak over a long match).
* Render thread 11 ms with 300-545 actors: too many draw calls for this GPU budget; it gets worse at Retina (A1).

### UE client: visuals and log
* In fights the health bars, names and damage numbers pile on top of each other (screenshots at 2-2 and 3-6); "absorbed" text overlaps bars.
* 8 arena / blockout materials lack the **Nanite usage flag** ("Default Material will be used in game"): check the arena floor, tiles, bench, Little Legend.
* The Cinzel font has no **✓** (U+2713): a fallback glyph shows (6 times in one match). Same family as the old ✕ problem.
* A material instance recompiles on every launch until it is resaved (slower start).
* `unit_fx.json`: a `"_doc"` string where objects are expected (harmless log error).
* The autopilot (dev only) never equips items and keeps a full bench: it is a test tool, not a player, but it is why the late game was not covered.

---

## C. Priority list (my recommendation)
1. **Crisp + resolution + performance together**: Retina on, 100% default with a resolution-scale slider, fix the hitches, the start stall and the component leak.
2. **Item economy**: components-first drops, rare emblems, 1 remover (data only, measured again with the same tool).
3. **Item bench**: unlimited bag shown cleanly (10 slots + overflow page or a TFT-style grid), aligned row, slots per arena, steady orbs, exact hit boxes.
4. **Pop-ups**: rectangle-based hover, close on click outside and on phase change (after your repro).
5. **League-style settings panel** with detailed graphics options and a confirm / revert step; make AA actually apply; rebindable P.
6. **Sounds**: replace the bright synthetic set.
7. **Server socket limit** before any online test.
8. Balance: PvE bosses, Selini, Hexa unlock, weak 1-costs.
