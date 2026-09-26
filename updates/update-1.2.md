# W2F DEMO 1.2 (2026-09-26)

The answer to FEEDBACK V2 (`feedback/FEEDBACK_V2.md`): the first step of the League-style game client. You start on a client screen, press Play, queue, get into a match,
and come back to the client when it is over.
Server tests: 6,359 engine + 1,972 network checks green, also with `-Werror -fno-exceptions -fno-rtti` (sanitizers on). "Screenshot" = seen in the running game by the developer;
things that need a real mouse are marked **to test by hand**.

## Feedback
| # | Feedback | Change | Checked |
|---|---|---|---|
| 1 | Update the project on GitHub | The server repository (`github.com/mak3sumwork/w2f`) was pushed with DEMO 1.0 + 1.1 (and this demo). The Unreal project has **no remote yet**: create an empty GitHub repository and send its address, then it can be pushed too (Content/ is ~2.7 GB, so Git LFS is worth setting up first). | `git status` = in sync with origin |
| 2 | Queue, then play | A matchmaking server and a client front end: home screen -> PLAY -> choose a mode -> FIND MATCH -> queue timer -> MATCH FOUND -> the match -> "return to client". | tests; screenshots (play page; queue -> live match against 7 AI) |

## Server (`server/`)
* **Queue server**: `w2f_server --queue [--fill-seconds N] [--fast]` runs many matches in one process (`net/include/w2f/net/QueueServer.h`, `net/src/QueueServer.cpp`). Each match is an
  unchanged `GameServer`; the queue server only routes connections into it.
* Two queues: **Solo vs AI** (`bots`: starts at once, you + 7 AI) and **Normal** (`normal`: waits for other players; starts when 8 search or after `--fill-seconds`, default 30, with AI in the empty seats).
* A connection starts **idle** (the home screen). After the match (or `leave_match` once you are out) the socket stays open and you are back on the home screen, ready to queue again.
  Reconnecting with a match's token goes straight back into that match. Matches nobody is connected to close after 2 minutes; at most 64 run at once.
* Protocol **revision 7** (additive, queue server only): commands `queue`, `leave_queue`, `leave_match`; messages `queue_status`, `match_found`. A single-lobby server (no `--queue`) answers
  them with `error` `no_queue`, so everything from 1.1 works as before. Schemas updated (`docs/schemas/`), docs in `docs/network-protocol.md` ("Matchmaking"), `UE5-Integration.md`, `CLAUDE.md`.
* New network test: bots and normal queues, several matches in one server, cancel, leave a match, back to the home screen, reconnect by token.
* The browser client (`client/index.html`) got a "Find match (vs AI)" button for the queue server.

## The client (Unreal project)
* New `W2FClient.h/.cpp` (Slate): a League-style **top bar** (PLAY button, W2F, HOME / PLAY / COLLECTION / PROFILE, online players + running matches), a **home** page with a rotating
  splash-art background, a **play** page (Solo vs AI / Normal cards, FIND MATCH), a **queue** timer with Cancel, a **MATCH FOUND** banner, an **offline** screen with RECONNECT, and a
  **return to client** button after you are eliminated or the match ends.
* Collection and Profile are "coming soon" placeholders (they need accounts / saved data on the server first).
* Against an old single-lobby server the client goes straight into the match, as before.
* Dev flags: `-w2fclienttab=N` (0 home, 1 play, 2 collection, 3 profile), `-w2fqueue=bots|normal` (queue once on start).

## How to play it
    cd server && make server && ./build/w2f_server --queue
    # then start the Unreal project with -w2flive (or press Play in the editor with bLive on)

## To test by hand
* Clicking through the client with a real mouse: tabs, mode cards, FIND MATCH, Cancel, RECONNECT, return to client.
* Normal queue with two real clients (two Unreal windows, or one Unreal + the browser client).
* A second match after returning to the client (covered by the network test, not yet by a person in the Unreal client).

## Files
- Server: `net/include/w2f/net/QueueServer.h`, `net/src/QueueServer.cpp` (new); `GameServer.*`, `TcpServer.*`, `Messages.*`, `Protocol.*`; `tools/w2f_server.cpp` (`--queue`, `--fill-seconds`);
  `net/tests/net_tests.cpp`; `docs/schemas/*.json`, `docs/network-protocol.md`, `docs/UE5-Integration.md`, `CLAUDE.md`; `client/index.html`.
- Client: `W2FClient.h/.cpp` (new), `W2FArena.*` (client state, queue messages, dev flags).
