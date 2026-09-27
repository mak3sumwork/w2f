# W2F DEMO 1.5.1 (2026-09-27)

Follow-up to DEMO 1.5, no new feedback file. The designer asked for the "to test by hand" list of update-1.5.md to be done without them.
The client's buttons, text boxes and hovers are now driven by an automated **client self-test**, which found and fixed four real bugs. The packaged app
played a whole match.

## The client self-test (`-w2fclienttest=NAME`, `Source/work2fightgame/W2FClientTest.cpp`)
It drives the real Slate widgets the way a player does. It finds a button or text box by the text on screen, moves the cursor there, and sends
mouse presses, releases and typed characters through `FSlateApplication`, one event per frame. Each step waits for the client state it should cause.
Results go to `Saved/W2F/clienttest.txt` plus a screenshot per step. A second, headless client logs in as "Rival" (`-nullrhi`) and sends friend
requests at the right moments. Last run: **26 / 26 PASS**.

| Step | Result |
|---|---|
| login form shown when logged out | PASS |
| typing an unreachable server address + CONNECT shows the offline screen with that address | PASS |
| typing `localhost:7777` on the offline screen + RECONNECT comes back online | PASS (after fix 1) |
| CREATE ACCOUNT with typed name + password logs in | PASS |
| LOG OUT back to the login form | PASS |
| a wrong password is refused with a message | PASS |
| LOG IN with the right password | PASS |
| friend request by typing a name + "+" (shown as pending) | PASS |
| CANCEL takes it back | PASS |
| an incoming request shows up; the tick accepts it | PASS |
| the friend's live status online, then offline when they quit | PASS |
| clicking a friend opens their profile; MY PROFILE goes back | PASS |
| hovering a friend row shows its x; the x removes the friend | PASS |
| the x on an incoming request declines it | PASS |
| COLLECTION tab, clicking a champion (card), TRAITS (incl. Selini's two paths), ITEMS | PASS (after fix 2) |
| PROFILE shows the match history; clicking a match opens its lobby | PASS (new, fix 3) |
| clicking an AI player's name in the lobby says it is an AI player | PASS (fix 4) |

Running it: `server/scripts/client_selftest.sh` (after `make server` and building the editor). It starts its own queue server with the accounts in
`server/tests/data/client_selftest_accounts.json` (Tester with one finished match, Rival; both password `secret1`). It then starts client A
(`-w2flive -w2fclienttest=CTnnnn`) and, at the right moments, the headless client B (`-nullrhi -w2flive -w2flogin=Rival:secret1 -w2ffriend=CTnnnn`),
prints the results, and puts the developer's own saved login back. Dev flag `-w2fcteststart=N` starts at step N.
The harness copes with a busy desktop: it brings the window back to the front when another app takes focus, and it retries a step up to three times,
the way a player would click again. Retries are listed in the results.

## Bugs found and fixed
1. **"localhost" never connected.** macOS resolves it to IPv6 `::1` first, and `w2f_server` listens on IPv4 only. The client now uses `127.0.0.1` for `localhost`.
   Also, both server-address boxes (login screen, offline screen) now show the address in use. The offline screen used to show the old default,
   not the address that failed.
2. **Selini's breakpoints had no text** in the Collection and in the match's trait tooltip: its texts are per path (`trait.6.path<n>.bp<count>`). Both
   paths are now listed, e.g. "(3) Path of Enlightenment: ... / Path of Prosperity: ...".
3. **The match lobby in the history was a tooltip only.** Clicking a match now opens its lobby (8 players by placement) under the row, and every name opens
   that player's profile.
4. **Clicking an AI player** (their names are in the lobby too) said "There is no player with that name". It now says "<name> is an AI player: AI players
   have no profile".

## The app, a whole match
`~/Desktop/W2F_App/W2F.app` rebuilt with the fixes. Launched like a double-click, it logged in (`-w2flogin`) and played a Solo vs AI match on autopilot
(`-w2fauto`) to the end, with screenshots every 45 s (see "Checked" below).

## Files
- Client (Unreal): `W2FClientTest.cpp` (new), `W2FArena.h/.cpp` (test state, `-w2fclienttest=` / `-w2fcteststart=`, Selini path text), `W2FClient.cpp`
  (clickable lobbies, localhost, address boxes, AI-player notice, friend-action log line), `work2fightgame.Build.cs` (ApplicationCore).
- Server repo: `scripts/client_selftest.sh` + `tests/data/client_selftest_accounts.json` (new), this file. The server code did not change.

## Still for human hands
* The feel of it: hover timings, the real cursor, typing speed, full screen vs window (the app starts in a 1600 x 900 window).
* Two real Macs over a network (the self-test ran both clients on one Mac).
