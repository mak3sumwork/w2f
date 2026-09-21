# Running and deploying the server

`w2f_server` is one process, one thread, no dependencies, one match at a time. Everything below is about running it for other people. (For development just run
`make server && ./build/w2f_server --bots 7` and open `client/index.html`.)

```
w2f_server [--port 7777] [--bind 0.0.0.0] [--players 8] [--bots N] [--data DIR] [--seed N] [--fast]
           [--autosave FILE] [--resume FILE] [--join-code CODE]
```

## Options that matter in production
| option | what it does |
|---|---|
| `--players N` / `--bots M` | seats per match (2..8) and how many of them are AI. The lobby waits for `N - M` humans; the match then starts by itself |
| `--join-code CODE` | a **private server**: every connection must bring `?code=CODE` in its URL, or the handshake is answered with HTTP 403. A shared secret that keeps strangers out of a lobby: it is not an account system and travels in the URL, so use it with `wss` (below) |
| `--autosave FILE` | writes the restart point to `FILE` (atomically) at the start of every Planning phase, and deletes it when the match ends |
| `--resume FILE` | after a crash or a restart: restores the match from `FILE`. Bots come back with their state; the humans reconnect with the tokens they were given (`?token=...`) and are resynced. It must be started with the same `--players`, `--bots`, data files and rules as the server that wrote the file; anything else is refused with the reason and exit code 2 |
| `--seed N` | fixes every match's seed (reproducible matches); default: random per match |

The server logs every connection and every autosave; `SIGINT` / `SIGTERM` stop it cleanly (players get close code 1001 and can reconnect after the restart).

## Crash recovery, precisely
* The restart point is the whole match as of the start of the round's Planning phase. After a crash the players lose *at most the current round's planning actions*: the shop, gold, bench and the bots' decisions
  are exactly as they were at the start of that round. (A client should keep retrying with its token for a minute.)
* One file holds both the engine snapshot and the small "seats" record (reconnect tokens, which seats are bots, the bots' random state), so a crash while saving can never pair a snapshot with the wrong seats.
* A finished match removes the file: a normal restart starts a fresh lobby. To force a fresh start anyway, delete the file (or set `W2F_RESUME=0` in the container).
* Tested: `TestResumeAfterACrash` (a fresh server resumed from the file plays on tick for tick identically to the one that wrote it, through fights and rounds), and by hand with `kill -9`.

## Docker
```
docker build -t w2f-server .
docker run --rm -p 7777:7777 -v w2f-data:/var/lib/w2f w2f-server --bots 7
```
The image is two stages (a one-command g++ build, then a slim runtime running as an unprivileged user). `deploy/entrypoint.sh` starts the server with `--autosave /var/lib/w2f/match.w2fsave` (a volume) and adds
`--resume` when that file exists, so `docker restart` / a crash loop brings a running match back. Environment: `W2F_JOIN_CODE`, `W2F_PORT`, `W2F_SAVE`, `W2F_RESUME=0`. Anything after the image name is passed to the server.

> The Docker files were written and syntax-checked but **not run** on the development machine (no Docker there). The build command inside the Dockerfile is the same single `g++`/`clang++` line that was compiled and run
> on macOS; expect to fix a typo on the first `docker build`, not a design problem.

## TLS (wss) and several matches: `docker-compose.yml`
`docker compose up -d --build` starts two match servers and a Caddy reverse proxy that gets a certificate for `W2F_DOMAIN` by itself and forwards WebSockets:

```
W2F_DOMAIN=play.example.com W2F_JOIN_CODE=secret docker compose up -d --build
   wss://play.example.com/match/1?code=secret      -> w2f-1 (one match)
   wss://play.example.com/match/2?code=secret      -> w2f-2 (another match)
```
The game server ignores the URL path, so the proxy routes by path. One process is one match; add a `w2f-3` service and a `handle /match/3*` block in `deploy/Caddyfile` for more.
(nginx works the same way: `proxy_pass` with `proxy_http_version 1.1; proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection "upgrade";` and a long `proxy_read_timeout`.)

## Several matches without Docker
`scripts/run_matches.sh 4 7777 --bots 0 --join-code secret` runs four servers on ports 7777-7780, each with its own autosave under `/tmp/w2f` (`W2F_SAVE_DIR`), and restarts any that
exits, resuming its match. Put a TLS proxy in front as above.

## Limits and safety notes
* At most 64 open sockets per process; a match is 8 players at most; a client that stops reading (16 MiB queued) is dropped; the rate limit is 30 burst / 15 per second per connection.
* The engine is authoritative and validates every command again; the protocol layer refuses malformed input before it reaches the engine (fuzzed in the test suite). There is no TLS in the server itself.
* Reconnect tokens are 128 random bits; anyone holding a token holds the seat. Treat them like session cookies; do not log URLs with tokens or codes.
* The 8-client soak test (`TestEightClientSoak`) plays a whole match over real sockets with three players dropping out and coming back, and checks every message against the published schemas.
