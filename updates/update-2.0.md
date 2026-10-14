# W2F DEMO 2.0 -- the League-style client

Answers FEEDBACK V10 (`feedback/FEEDBACK_V10.md`, 2026-10-10): "its time to league style client. pls do it well ... i need a final version i dont wanna
mess with the cleint again. perfect pls. sound effects menus all that". Asked first which League-client features to add; the answer was **all of them**
(parties + invites, chat, post-game + match details + leaderboard, full settings + Esc menu) and an **earned-currency store** (coins from matches and missions;
no real money). Both repos were clean at DEMO 1.9 (server 11a6069, UE b47bff7), so nothing had to be committed first.

| # | Asked | Done | How it was checked |
|---|---|---|---|
| 1 | Parties and invites | Invite a friend from the lobby's open seats (**+ INVITE** opens a picker of your friends), from their profile (**INVITE**) or from their row in the social panel (**+**). The invite pops up as a **toast** with ACCEPT / DECLINE (and in the social panel: JOIN / DECLINE); the window flashes if W2F is in the background. The lobby's seats are the party: icons in their borders, the leader's **crown**, level, title and rank; the leader hovers a member for PROMOTE / KICK; **LEAVE PARTY**. Only the leader picks the mode and starts the queue (members see WAITING FOR LEADER); the whole party searches, gets the ready check and lands in the same match. Ranked takes duos. | server tests (TestParties: invite, accept, decline, lead, queue together, ready-check decline, kick, expiry, disconnect); UE screenshots of the invite toast and the 3-player party lobby with a scripted second and third player; client self-test steps (invite Rival, the party lobby, LEAVE PARTY) |
| 2 | Chat | A **chat window** (bottom right; Enter in a match opens it): one tab per conversation, the party's tab, unread counts, times, Enter sends. Whispers between friends (kept for a friend who is offline and delivered when they log in), party chat; "X joined / left the party" lines. **Toasts** for new lines, an unread badge on the friend's row and on the CHAT button. | server tests (TestChat); UE screenshot of the party chat with lines from two scripted players; self-test step (type + Enter, the server echoes it) |
| 3 | Post-game, match details, leaderboard | **POST-GAME** screen when you are back from a match: the placement (rays, a pop), VICTORY / TOP 4 / DEFEAT, Ranked LP counting up on the LP bar, coins and XP counting up, the level bar (LEVEL UP!), the missions it moved (DONE!), and **the whole lobby** with every player's final board (stars, items); PLAY AGAIN / CONTINUE. Every match in your history opens its **MATCH DETAILS** (all eight boards). **RANKED** tab: the leaderboard, a podium for the top 3, your place. | server tests (post_game, match_detail, ladder); UE screenshots (post-game after a real match, the leaderboard) |
| 4 | Full settings + Esc menu | **Settings** in four tabs: VIDEO (quality, fullscreen / borderless / windowed, resolution, frame-rate cap, v-sync, FPS), AUDIO (master / client music / game effects / interface sliders; mute in the background -- the match-found alarm and invites still sound), CONTROLS (the hotkeys are **rebindable**: click, press a key; reset), ACCOUNT (change password, log out, quit). **Esc** in a match: RESUME, SETTINGS, CHAT, LEAVE MATCH (asks twice), QUIT GAME (asks twice). | screenshots; self-test steps (the tabs, rebinding D -> G and back, Esc / RESUME in a match) |
| 5 | Store (earned coins) | Coins and account XP for every match (by placement; Solo vs AI 50%, Normal 100%, Ranked 125%), **account levels** (150 coins each), **3 daily + 3 weekly missions** (the same for everyone; paid at once), 500 coins to start. The **STORE**: 9 profile **borders** (bronze .. prismatic, with rays / gems / void spikes / a moon), 10 **titles**, 2 **arenas** (the classic plaza; the space platform, 2000 coins). Borders show around your icon everywhere (profile, social panel, friends, lobby, leaderboard, post-game, loading screen); titles under your name; the arena you equip is the stage of your matches. Coins and level in the top bar. | server tests (TestProgression); UE screenshots (store, missions); self-test (buy + equip a border) |
| 6 | Sound effects | 18 new sounds (synthesised like the others): chat in / out, party invite / join / leave, purchase, coins, mission complete, level up, toast, post-game reveal, LP gain / loss, menu open / close, error, friend online, slider tick. | levels checked by peak / RMS; **not heard by me** |

## Server: protocol revision 10 (additive)
Parties, chat, match records + `post_game`, the leaderboard, progression (`wallet`, `missions`, `store`), `change_password`. Every message and command is in
`docs/network-protocol.md` ("Parties, chat, match records, the leaderboard and progression") and the JSON Schemas. New: `net/include/w2f/net/Progression.h`,
`net/src/Progression.cpp`, `data/progression.json` (rewards, the level curve, the store, the mission pool). `w2f_server --queue --accounts FILE` loads
`data/progression.json` when it is there. `make test`: 6436 engine + 2133 network checks (3 new tests), also with `-Werror -fno-exceptions -fno-rtti`.

## UE client
* `W2FSocial.cpp` (new): the logic -- messages, parties, chat, toasts, store, missions, post-game, settings (audio channels, window, keys), the arena theme swap,
  the Esc menu. `W2FClientKit.h/.cpp` (new): shared widgets (hextech button, frame, icon in its border, board strip, bars, coins). `W2FClientPages.cpp` (new):
  the store, leaderboard, missions card, post-game, match details, chat window, toasts, lobby seats, invite picker, top-bar badge, Esc menu.
  `W2FSettings.cpp` rewritten (tabs). `W2FClientUi.cpp`: the tabs (HOME, COLLECTION, PROFILE, STORE, RANKED), the lobby, the social panel, the profile,
  the overlays. `W2FArena.cpp`: message routing, the rebindable keys, Esc, sound volumes.
* Art (`tools/make_client_art.py`): `T_UI_Border_<id>` x 9, `T_UI_ClientLevel`, `T_UI_ClientCrown`, `T_UI_ClientChat`. Sounds (`tools/make_sfx.py`, appended
  after the music so every older sound stays byte-identical). Imported with `W2F_ONLY=... tools/unreal/import_ui.py / import_sfx.py` (new: `W2F_ONLY`
  limits an import to a few files, so the other assets are not re-saved).
* Dev flags: `-w2fdemo=step+step...` (chat:NAME, settings:TAB, matchdetail, invitepicker, purchase:ID, storetab:N, acceptinvite, tab:N, profile:NAME, esc),
  `-w2fshotpost` (the screenshot waits for the post-game screen). Fixed: `-w2furl=` now wins over the remembered server.

## Running it
* The app: `~/Desktop/W2F_App/W2F.app` (rebuilt 2026-10-14, 2.3 GB, signed; starts its own server). Launch check: it opens on the sign-in screen with
  "W2F DEMO 2.0", its bundled `w2f_server --queue` listens on 127.0.0.1:7777 and stops when the app quits. In the editor: `w2f_server --queue --accounts FILE`
  + `-w2flive`.
* Packaging from an automated shell still needs the Xcode mirror (`W2F_DEVELOPER_DIR`, see `scripts/package_mac.sh`); a normal Terminal does not.
* `scripts/client_selftest.sh`: the first 18 steps pass (log in, tabs, store buy + equip, RANKED tab, missions, ...). The new party / chat / settings / Esc
  steps did **not** run to the end: macOS will not give the test window focus while someone is using the Mac, so the run stalls. Run it while the Mac is free.

## Files
* UE: `W2FSocial.cpp`, `W2FClientKit.h/.cpp`, `W2FClientPages.cpp` (new), `W2FClientUi.cpp`, `W2FClient.cpp`, `W2FSettings.cpp`, `W2FArena.h/.cpp`, `W2FHud.cpp`,
  `W2FClientTest.cpp`, `Content/W2F/Icons/T_UI_Border_*` + `T_UI_Client{Level,Crown,Chat}`, `Content/W2F/Sfx/SFX_*` (18 new).
* Server repo: `net/` (revision 10, `Progression.h/.cpp` new), `data/progression.json` (new), `docs/schemas/`, `docs/network-protocol.md`,
  `docs/UE5-Integration.md`, `tools/make_client_art.py`, `tools/make_sfx.py`, `tools/unreal/import_blockouts.py`, `docs/icons/T_UI_Border_*`,
  `docs/icons/T_UI_Client*`, `docs/sfx/SFX_*.wav`, `tools/w2f_server.cpp`, `scripts/package_mac.sh`, `scripts/client_selftest.sh`, `feedback/FEEDBACK_V10.md`,
  `CLAUDE.md`, this file.

## Honest limits / to test by hand
* **Run `scripts/client_selftest.sh` while nobody uses the Mac** (see above): the party, chat, settings and Esc steps have not run in the self-test yet
  (each screen was checked by a screenshot).
* **Listen** to the new sounds; tell me what is too loud, too long or annoying.
* Chat history and offline whispers live in the server's memory (a restart forgets them). Match records (the newest 400) and everything else are in the accounts file.
* The parties were tested with scripted players and a second headless client, not with two people at two Macs.
* The store has no real-money path at all, by design. Prices and mission rewards are first guesses in `data/progression.json`: easy to tune.
* Window mode / resolution / v-sync change UE's own settings; they were exercised in the editor's game mode, not in the packaged app full screen.
