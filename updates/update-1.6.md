# W2F DEMO 1.6

Answers FEEDBACK V6 (`feedback/FEEDBACK_V6.md`, 2026-09-28): the roster compared with TFT and fixed, a client that no longer looks cheap
(a live 3D background, a new Home and Profile, the Collection's champion on stage), splash art for every champion, and a Mac app that starts the game by itself.

| # | Asked | Done | How it was checked |
|---|---|---|---|
| 1 | Compare our traits and champions with TFT | TFT Set 18 (65 champions, 25 shared + 10 one-champion traits) against ours (48 champions, 17 traits). Kept on purpose: emblem-only top breakpoints, Coregons 8 = 2 emblems, the Protector duo. Found: every 5-cost carry was a Sorcerer, Sorcerer had 11 champions, 2- and 4-cost had 2 tanks each, Najmi had no frontliner, Omnilium was an empty trait. The plan is design doc section 2D. | the discussion in FEEDBACK V6 |
| 2 | The roster pass (approved) | **Pulsar** (2, Najmi / Bruiser tank), **Rampart** (2, Hexagon / Bastion tank), **Skarn** (3, Phaisa / Marksman), **Maren** (4, Selini / Bruiser tank), **Vector** (5, Hexagon / Gunslinger carry). Faire and Lich lost Sorcerer (Lich is a Mystic now), Les and Lum lost the Omnilium tag, **Protector (1)**: one Protector takes 15% less damage. 48 -> 53 shop champions (11/12/11/11/8); tanks per cost 6/4/4/3/4. | `make test` green (6,438 engine + 1,976 network checks) incl. a new test of every new ability; golden fights and sample fights re-recorded; 3 x 300-match balance runs |
| 3 | Balance the newcomers | Two trims (docs/balance.md "Roster pass v3"): Pulsar 55%, Rampart 55%, Skarn 54%, Maren 57%, Vector 62% (5-costs run high: Aureon 60%, Yggra 59%). | `make balance ARGS="--matches 300 --seed 1000"`, report in `server/docs/balance/roster-v3.txt` |
| 4 | Splash art ("so important") | **The splash studio** (UE, `-w2fstudio=all`): each champion posed at the peak of its cast in front of a painted sky of its origin (8 skies, `tools/mixamo/make_studio_skies.py`), warm key + two rim lights in its own colours, volumetric fog, embers and motes, depth of field; 1920x1080 splash + 1024 portrait for all 58 units, published into the game. Plus **one AI-image prompt per champion** in `splash_arts/PROMPTS.md`: a painted splash dropped in as `splash_arts/<Name>.jpg` always wins over the render. The designer's own splash arts are now imported at up to 1920 px (they were shrunk to 480, part of why the home page looked blurry). | contact sheets of all 58 renders; the new art on the client (screenshots) |
| 5 | The client looks cheap / a better background | **The client stage**: the client's background is a live 3D scene (the same set as the studio, built far from the arena, its own camera): the featured champion breathes, casts its ability with the real effects every ~7 s, and changes every 16 s with a quick dip to black. Soft gradients sit behind the text instead of the old flat veil over a blurry picture. | screenshots of Home, Collection, Profile in the real client |
| 6 | Home | The featured champion's name, title, cost, traits and ability over the stage, PLAY and VIEW IN COLLECTION, the next six champions (click = bring one on stage), a "new in 1.6" card. | screenshot |
| 7 | A better Profile | A header straight on the stage (rank crest, name, status, rank + LP bar, peak, big stats) with the player's **most played champion** standing behind; cards: **last 20 games** as placement bars, **top champions** (games, average place, top 4 %), the match history. | screenshots with an empty account and with a played match |
| 8 | Collection | The picked champion steps onto the stage next to a narrower panel. | screenshot |
| 9 | Package the Mac app so the game just starts | The app carries **its own server** (`w2f_server --queue` with accounts, built by `make server-release`, staged into `Content/W2F/Server`): when the address is this Mac the client starts it, retries the connection while it boots, and stops it on quit. If a server already runs on the port, that one is used. The sandbox now allows local listening. One command builds it all: `server/scripts/package_mac.sh`. | the dev client started its own server, logged in, and the server was gone after quitting; the app: see below |

## Running it
* **The app:** double-click `~/Desktop/W2F_App/W2F.app`, CREATE ACCOUNT, play. Everything runs on this Mac. The app's accounts live in its container
  (`~/Library/Containers/com.YourCompany.work2fightgame/Data/.../Saved/W2F/local_accounts.json`).
* **Building the app again:** `cd server && scripts/package_mac.sh` (editor closed).
* **New splash renders:** UE `-game -windowed -w2fstudio=all` (or `=9001,9059`) -> `Saved/W2F/Studio`; `python3 tools/mixamo/studio_publish.py`;
  `UnrealEditor-Cmd <project> -run=pythonscript -script=tools/unreal/import_portraits.py`.
* **Painted splash arts:** follow `splash_arts/PROMPTS.md`; import with `python3 tools/make_icons.py --splash-only` + `import_portraits.py`
  (a full `make_icons.py` run rewrites every icon: do not use it for this).
* Dev flags: `-w2fstudio=`, `-w2fstudiosky=<brightness>`, `-w2fnostage` (old flat background), `-w2fnolocalserver`.

## Files
* Server repo: `data/champions.json`, `data/traits.json`, `data/text_en.json`, `tests/data/designer_spec_*.json`, `tests/tests.cpp` (TestRosterPassV3Champions, updated
  roster/pool/Protector tests), `tests/golden/fights.golden`, `docs/sample_fights/*`, `docs/balance.md` + `docs/balance/roster-v3.txt`, `Makefile` (`server-release`),
  `scripts/package_mac.sh`, `tools/mixamo/config.json` + `fx.json` (5 heroes), `tools/mixamo/make_studio_skies.py`, `tools/mixamo/studio_publish.py`,
  `tools/unreal/import_studio.py`, `tools/unreal/import_portraits.py`, `tools/make_icons.py` (`--splash-only`, 1920 px), `docs/icons/studio/`,
  `W2F_GameDesignDocument.md` (2D, roster), `splash_arts/PROMPTS.md`, `CLAUDE.md`.
* UE: `W2FStudio.cpp` (new: the set, the splash studio, the client stage), `W2FArena.h/.cpp` (stage hooks, FX locate / cell offset, flags), `W2FFx.h/.cpp`
  (`ChampionColours`, `StudioAmbience`), `W2FClient.cpp` (Home, Profile, Collection, background, the app's own server), `Config/DefaultGame.ini` (stage `W2F/Server`,
  version 1.6.0), `Build/Mac/Resources/Sandbox.Client.entitlements` (network.server), `.gitignore`, `Content/W2F/Heroes/905x_*`, `Content/W2F/Studio/`, `Content/W2F/Icons`.

## Honest limits / to test by hand
* The renders are the Mixamo stand-in models well lit, not painted illustrations; the prompts are there for painted ones.
* The new heroes reuse Mixamo bodies with new colours and props (all 48 characters were already cast).
* Balance: Faire is weak (41%) after losing Sorcerer; overtime is up (12.7%, was 8.7%) with the extra shields; the full balance pass is next.
* By hand: the stage while clicking around (switching tabs quickly, a match found while it swaps), the Collection card with a picked champion, the app on a fresh
  account, performance on Low quality (the stage uses volumetric fog and four shadowed lights).
