# W2F DEMO 1.5 (2026-09-27)

The answer to FEEDBACK V5 (`feedback/FEEDBACK_V5.md`): a finished game client (accounts, profile, match history, friends, collection), a rank system, usernames for
the AI players, the custom cursor switched off, and a macOS app. "Screenshot" = checked by the developer in the running game against a real
`w2f_server --queue --accounts`; "test" = the server's automated tests; things that need a real mouse / keyboard are marked **to test by hand**.

| # | Feedback | Change | Checked |
|---|---|---|---|
| 1 | Profile, friends, the other client pages -- finished | **Accounts**: a login screen (username + password, CREATE ACCOUNT, the server address). The client remembers the session ("remember me", never the password) in `Saved/W2F/account.json`. One device per account: logging in elsewhere logs the older one out. | screenshots (login, stale session -> login screen); test |
| 1 | | **Profile**: rank emblem, tier / division / LP bar, peak, online / offline, games, wins, top 4, average place, ranked games / top 4. Anyone's profile can be opened (from the friends list or a match's lobby); ADD FRIEND on someone else's. | screenshots |
| 1 | | **Friends** (right side of every client page, like League): add by name, incoming requests (accept / decline), outgoing (cancel), the list with rank and status (online / in queue / in match / offline, live), remove. Works from inside a match too (server side). | screenshots (a request arriving, then friends after a request back); test |
| 1 | | **Collection**: every champion by cost (hover / click = its full card), every trait with its breakpoints, every item. No "coming soon" pages left. | screenshot |
| 2 | Match history | The last 20 matches on the profile, newest first: mode, placement, level, round, LP change, the board you last fought with (stars, items), the lobby in placement order. The server records it at the end of every match (the board is captured at every combat start). | test; a real Ranked match ended up in the history (confirmed by the designer) |
| 3 | 7 bots have usernames | The AI players get names like real accounts (`ShadowFox`, `LunarSage42`, ...; never a real player's name, never twice in a lobby). Every seat's name is sent in `match_started.player_names` (scoreboard, scouting, your Little Legend's plate shows your username). | screenshot (plate "TESTER"); test |
| 4 | Rank system | A new **Ranked** queue (Play page, third card). Tiers Iron .. Diamond with divisions IV .. I of 100 LP, then Master (2800), Grandmaster (3000), Challenger (3300). LP by placement: 1st +40, 2nd +30, 3rd +20, 4th +10, 5th -10, 6th -20, 7th -30, 8th -40; never below Iron IV 0. Solo vs AI and Normal stay unranked. Emblems for every tier (`tools/make_hud_art.py`). | test; screenshot (Ranked card, Iron IV after an 8th place) |
| 5 | Disable the cursor update | The system arrow again; `-w2fcursor` brings the gauntlet back. | code |
| 6 | Package it, make it an app (macOS) | **`~/Desktop/W2F_App/W2F.app`** (2.1 GB, Shipping, signed to run locally, not notarized). It always starts in the client (live mode), in a 1600 x 900 window (resizable); the server address is on the login screen (default `ws://127.0.0.1:7777/`). It carries its own copy of the catalog (`Content/W2F/Data/catalog.json`). The app is sandboxed with the network-client permission (UE's default for Shipping is "no network": the first build could not reach the server). | launched like a double-click (`open`) on this Mac: window, login screen, logged in, the profile with the Ranked match in the history (the app's own screenshots) |

## Server (protocol revision 8, additive)
* `w2f_server --queue --accounts FILE`: accounts in ONE JSON file (rewritten atomically after every change), passwords as PBKDF2-HMAC-SHA-256 with a random
  salt (20 000 rounds). New commands `register`, `login`, `resume_session`, `logout`, `get_profile`, `get_friends`, `friend_request` / `_accept` / `_decline` /
  `_remove`, `queue` mode `ranked`; messages `auth`, `profile`, `friends`, `logged_out`; `match_started.player_names`. Documented in
  `docs/network-protocol.md`, schemas updated. A server without `--accounts` answers the account commands with `no_accounts` and the client plays as a guest.
* Passwords travel in the clear over `ws://`: an online server must sit behind TLS (`wss://`, `docs/deploy.md`).
* Tests: 6,359 engine + 2,048 network checks green, also with `-Werror -fno-exceptions -fno-rtti`.

## Running it
1. Server (the machine everyone connects to): `cd server && make server && ./build/w2f_server --queue --accounts accounts.json` (add `--fast` for short rounds).
2. App: double-click `~/Desktop/W2F_App/W2F.app` (on another Mac, the first time: right-click -> Open, it is not notarized). Type the server address if the
   server is not on this Mac, CONNECT, then CREATE ACCOUNT. The app keeps its login in
   `~/Library/Containers/com.YourCompany.work2fightgame/Data/Library/Application Support/Epic/work2fightgame/Saved/W2F/account.json`.

## Building the app again
`Build.sh work2fightgame Mac Shipping -Project=...` then `RunUAT.sh BuildCookRun -project=... -platform=Mac -clientconfig=Shipping -skipbuild -cook -stage -pak -iostore
-package -archive -archivedirectory=~/Desktop/W2F_App -map=/Game/W2F/Maps/L_Viewer`, then rename `Mac/work2fightgame-Mac-Shipping.app` to `W2F.app`. (A full
`BuildCookRun -build` from a Terminal should work too. From the developer's automated shell, UE's Xcode step failed with "Failed to launch task /bin/sh: Bad file
descriptor"; it was worked around with a DEVELOPER_DIR mirror of Xcode whose `xcodebuild` is a wrapper giving it a clean stdin.)

## Files
- Server repo: `net/include/w2f/net/Accounts.h` + `net/src/Accounts.cpp` (new), `BotNames.h/.cpp` (new), `Encoding.h/.cpp` (SHA-256, HMAC, PBKDF2),
  `Protocol.h/.cpp`, `Messages.h/.cpp`, `GameServer.h/.cpp` (seat names, match result), `QueueServer.h/.cpp` (accounts, ranked, presence, history),
  `tools/w2f_server.cpp` (`--accounts`), `net/tests/net_tests.cpp`, `docs/schemas/*`, `docs/network-protocol.md`, `docs/UE5-Integration.md`,
  `tools/make_hud_art.py` + `docs/icons/T_UI_Rank_*.png`, `CLAUDE.md`.
- Client (Unreal): `W2FClient.cpp` (login, profile + history, friends panel, collection, Ranked card), `W2FArena.h/.cpp` (auth state, seat names,
  packaged = live, cursor off, catalog fallback), `W2FHud.h/.cpp` (the champion card shared with the Collection), `Config/DefaultGame.ini` (name W2F, version
  1.5.0, packaging: Shipping, stage `W2F/Data`, cook `/Game/W2F`), `Config/DefaultEngine.ini` + `Build/Mac/Resources/Sandbox.Client.entitlements` (network),
  `Config/DefaultGameUserSettings.ini` (start windowed 1600 x 900), `Content/W2F/Icons/T_UI_Rank_*`, `Content/W2F/Data/catalog.json`.
  Dev flags: `-w2flogin=USER:PASS` (creates the account if new; wins over a remembered session), `-w2ffriend=NAME`, `-w2fqueue=ranked`, `-w2freplay` (packaged: the fight viewer).

## To test by hand
* Typing in the login boxes, LOG IN / CREATE ACCOUNT / LOG OUT, the SERVER box + CONNECT.
* Friends: type a name + "+", accept / decline / cancel / remove, clicking a friend to open their profile; two apps on two machines to see live status.
* Profile: clicking a name in a match's lobby row; ADD FRIEND on someone else's profile.
* Collection: hovering / clicking champions, the TRAITS and ITEMS tabs.
* The app itself: double-click start, full screen / window, Settings, a whole match; on another Mac (Gatekeeper: right-click -> Open).
