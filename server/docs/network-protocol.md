# Network protocol and server

> **Protocol 1 is frozen** (Phase C). The machine-readable contract is `docs/schemas/server-message.schema.json` and `client-command.schema.json`, checked against real traffic by the test suite; the integration guide for a
> Unreal Engine client, with the combat log and its animation timings explained, is `docs/UE5-Integration.md`. This file is the prose reference. Within protocol 1 the server only adds (fields, messages, appended enum values);
> `welcome.protocol` is the major version (1) and `welcome.protocol_revision` counts the additive revisions.

`build/w2f_server` hosts one lobby and one match at a time. Clients (Unreal Engine 5, a web page, a test script) connect over a
**WebSocket** and exchange **JSON text messages**: one JSON object per WebSocket text frame, in both directions. There is no
binary format, no floating point anywhere, and nothing a client sends can change the game except the commands below, each of which
the engine validates again by itself.

```
w2f_server [--port 7777] [--bind 0.0.0.0] [--players 8] [--bots N] [--data DIR] [--seed N] [--autosave FILE] [--resume FILE] [--join-code CODE] [--fast]
```
`--players 2..8` seats per match (default 8). `--bots 0..players-1` makes that many seats AI players (see "Bot seats"): `--bots 7` lets one person play a whole match alone. `--seed` fixes every match's seed (default: random per match). `--data` is the folder with
`champions.json`, `traits.json`, `items.json`, `pve.json`. `--autosave FILE` writes the restart point (the engine's start-of-Planning snapshot plus the seats) there
(atomically; deleted when the match ends), `--resume FILE` restores a match from it after a crash, `--join-code CODE` makes the server private (`?code=CODE` on every connection; HTTP 403 otherwise). See `docs/deploy.md`. `--fast` shortens every phase for client development. Ctrl-C / SIGTERM stop it cleanly (clients get close code 1001).

## Architecture: the engine does not know the network exists
```
 client ──ws──▶ TcpServer ──▶ WebSocket codec ──▶ GameServer ──▶ MatchManager (engine)
 client ◀──ws── TcpServer ◀── (frames) ◀──────── GameServer ◀── IMatchListener events
   net/ (sockets, RFC 6455, JSON protocol, lobby)                    src/ + include/w2f/ (rules)
```
* **`src/` and `include/w2f/`** (the engine) contain no socket header and no reference to the network layer; `make check-isolation`
  greps for it, and the engine's own tests (`make test-engine`) link *without* the network objects.
* **`net/`** uses only the engine's public interface: `MatchManager::Try*` to act, `IMatchListener` to observe, const getters to read.
* **`GameServer`** is the only code that talks to the engine and it never touches a socket: it is fed `OnConnect / OnMessage /
  OnDisconnect` and answers through a two-method `IServerTransport`. So the entire protocol is tested without a network, and
  `TestNetworkedMatchEqualsBareEngine` records every command a networked match executes, replays them on a bare `MatchManager`, and
  requires the identical final state hash.
* **`TcpServer`** is one thread and one `poll()` loop with non-blocking sockets. The game tick (30 Hz) runs on the same thread between
  network reads, so a message is always handled entirely before or entirely after a tick. Wall-clock time is read only by the loop
  that paces ticks (`RunServerLoop`); the engine never reads a clock.

## Bot seats
With `--bots N` the **last N seats are AI players**; the humans are seats `0 .. players-N-1`, the lobby waits for `players - N` connections and the match starts when the last human arrives.
A bot is an `AIBotController` driven by `GameServer` once per tick *after* the engine's tick; it plays only through the same `MatchManager::Try*` calls a client uses, so it cannot break a rule. Each round, in Planning, it
banks gold for interest ((stage - 1) x 10, at most 50; it spends it when its health is 50 or less), buys XP with everything above 50 and up to the level it wants for the stage, buys units (star-ups and champions that advance its synergies first; when its roster is full it sells its weakest single 1-star unit to make room for something better), rerolls the shop with the gold above its savings from stage 2 on, fields the best `level` units (strength plus synergy; tanks in front),
and puts every item in its bag on a fielded unit (defence on tanks, damage on damage dealers, two components that combine first). In Mother Nature's phase it takes a unit it has room for, then an item, gold, XP (a heal when it is hurt).
Nothing about a bot is private information the server hands out: it has no connection, so its gold, shop and bench are never sent to anyone, and its board is public like any player's. `match_started.bot_seats` says which seats are AI.
The bots' commands are not reported to `SetCommandObserver` (that hook is for client commands). After a match the lobby reopens with the same bot seats.

## Connecting
`ws://host:7777/` (any path; plain `ws`, no TLS: put a TLS-terminating proxy in front for `wss`).
* The server assigns the **lowest free human seat** (`player_id`) the moment a client connects and answers with `welcome`.
* When all human seats are taken the **match starts by itself**.
* A client that leaves the lobby frees its seat. A client that leaves during a match keeps its seat reserved.
* Every new player gets a random **reconnect token** in `welcome`. Reconnect with `ws://host:7777/?token=<token>`: the client gets its
  seat back plus a full resync (see below). A token is 32 lowercase hex characters; anything else is treated as "no token".
  If the same token connects twice, the newer connection wins and the older one receives error `replaced` and close code 4001.
* Once the match is running, connections without a valid token are refused (error `match_in_progress`, close code 1013).
* After the match ends the server waits 20 s (players read the result), disconnects everyone with 1000, and opens a fresh lobby.

## Client -> server
Every command is an object with `"action"` and that action's fields, and optionally `"id"` (integer 0..2^53-1) which is copied into the
answer. **Unknown actions, unknown fields, missing fields, wrong types, non-integers and out-of-range numbers are all refused** (see errors).
A message is at most 4096 bytes.

| action | fields | notes |
|---|---|---|
| `buy_unit` | `shop_index` 0..63 | Planning, **Combat and Resolution** (else `WrongPhase`); `ShopClosed` in round 1 and Mother Nature's rounds. Outside Planning the champion goes to the **bench** and may only merge with bench units (`RosterFull` if the bench is full, `UnitInCombat` if it would merge into a fighter) |
| `reroll_shop` | | Planning, Combat, Resolution; `ShopClosed` in round 1 and Mother Nature's rounds |
| `pick_gift` | `gift_index` 0..3 | Mother Nature's phase only (else `WrongPhase`): take one of the offered gifts, free. `AlreadyPicked` / `InvalidSlot` when refused |
| `buy_xp` | | Planning, Combat, Resolution |
| `sell_unit` | `unit_id` | Planning; in Combat / Resolution bench units only (`UnitInCombat` for a unit on the board) |
| `move_unit` | `unit_id`, `location` `"bench"`\|`"board"`, `x`, `y` | bench: `x` 0..8 (`y` optional, must be 0); board: `x` 0..6, `y` 0..3 (3 = front row). Swaps with whatever is there. In Combat / Resolution only bench <-> bench (`UnitInCombat` otherwise). The board holds as many units as the player's level (`BoardFull`) |
| `equip_item` | `unit_id`, `item_id` | the item comes from the player's item bag. Planning; in Combat / Resolution onto bench units only. An **Item Remover** (consumable) takes all items off the unit instead (`NoItemsToRemove` if it has none) |
| `unequip_item` | `unit_id`, `slot` 0..2 | back to the bag. Same phase rule as `equip_item` |
| `combine_items` | `first`, `second` (item ids) | (revision 2) combine two base components that are both **in the item bag** into the finished item (recipes in items.json, either order; the same id twice needs two copies). `InvalidItem` (nothing changes) if either is not in the bag or there is no recipe. Answered with `result`, a `bag_event` (`event` `combined`, `first`, `second`, `result`) and the new `state`. Same phase rules as `equip_item` |
| `get_state` | | re-send `state` and `public_state` |
| `get_fight` | `fight_index` 0..7 | this round's combat log of any fight (they are public) |
| `get_catalog` | | answered with `catalog`: what every champion / item / trait id means. Works in the lobby too (costs 5 rate-limit tokens) |
| `ping` | | answered with `pong`, works in the lobby too |

Every command that reaches the engine is answered with a `result` (below). Commands are only ever executed for the connection's own
seat: a player cannot name another player, and another player's unit id is simply `InvalidUnit` for them.

### Example
```json
{"action": "buy_unit", "shop_index": 2, "id": 17}
{"type": "result", "id": 17, "action": "buy_unit", "result": "Ok", "ok": true}
```

## Server -> client
Each message has a `"type"`.

### Sent to one player only (private)
| type | when | fields |
|---|---|---|
| `welcome` | on connect | `protocol` (1), `protocol_revision`, `player_id`, `token`, `reconnected`, `seats` (all seats), `bots` (how many of them are AI), `connected` (humans), `match_running` |
| `result` | answer to a command | `id` (if given), `action`, `result`, `ok`. `result` is the engine's `ActionResult`: `Ok`, `WrongPhase`, `InvalidPlayer`, `PlayerEliminated`, `NotEnoughGold`, `InvalidSlot`, `EmptySlot`, `RosterFull`, `BoardFull`, `MaxLevel`, `InvalidUnit`, `ItemsFull`, `InvalidItem`, `ShopClosed`, `AlreadyPicked`, `NoItemsToRemove` (an Item Remover on a unit with no items: refused, kept) |
| `error` | a bad message | `code`, `detail`, `id` (if readable) |
| `pong` | answer to `ping` | `id` |
| `state` | whenever anything private changed | `player_id`, `alive`, `health`, `gold`, `level`, `xp`, `xp_to_next`, `streak`, `shop` (champion ids, 0 = empty), `bench` (9 entries: a unit or `null`), `board` (units), `item_bag` (item ids), `gifts` (Mother Nature: the offers still open, each `{index, gift, name, kind: gold\|xp\|heal\|item\|unit, amount \| item \| champion + cost}`; empty once picked and outside her phase), `gift_settled` |
| `income` | start of every round | `player_id`, `round`, `base_gold`, `interest_gold`, `streak_gold`, `passive_xp`, `total_gold` |
| `pve_drop` | won a PvE round | `drop` (`gold`\|`champion`\|`item`) and `gold` / `champion` / `item` |
| `gift_event` | Mother Nature | `event` `offered` (`gifts`: as in `state`) when her phase opens, `picked` (`gift`, `automatic`: the time ran out and the first offer was taken, `gold_converted`: > 0 when a unit gift had no room and was paid as gold) |
| `unit_event` | the player's own units changed | `event`: `bought`, `sold`, `moved`, `merged`, `item_equipped`, `item_unequipped`, `items_combined` (`first`, `second`, `result`: two items on the unit became one), `consumable_used` (an Item Remover was used on the `unit`: `item`, `returned`: the items that came off, in slot order; each also came as an `item_unequipped`), plus the `unit` (as `{id, champion, star, location, x, y, items}`) and event-specific fields |

A **unit** is `{"id", "champion", "star", "location": "bench"|"board", "x", "y", "items": [item ids]}`.

### Sent to everyone (public: visible to every player anyway)
| type | when | fields |
|---|---|---|
| `lobby` | someone joined / left before the match | `seats`, `bots`, `connected` (humans), `players` (the human seats taken) |
| `catalog` | answer to `get_catalog` | `champions` (`id`, `name`, `cost`, `role` `tank`\|`damage`, `monster`, `summon`, `traits`, `hp[3]`, `attack_damage[3]`, `attack_speed_milli`, `range`, `max_mana`, `ability`, `passive` names; the PvE monsters are included, flagged `monster`), `items` (`id`, `name`, `components` (two ids for a finished item), `stats`, `traits` granted, `has_effect`, `consumable`: the Item Remover, used up by `equip_item`), `traits` (`id`, `name`, `breakpoints`), `text` (`language`, `entries`: the display text, a flat key -> string map from `data/text_en.json`; keys in `docs/UE5-Integration.md` section 5) |
| `match_started` | the match begins (and on reconnect) | `player_id`, `seats`, `bot_seats` (the AI seats), `tick_rate` (30), `phase_ticks` (`mother_nature`, `planning`, `combat`, `resolution`), `mother_nature_every` (every N rounds; 0 = no Mother Nature data), `board` dimensions, `combat_event_types` (names, index = event type number) |
| `phase` | every phase change (and on reconnect) | `phase` (`MotherNature`\|`Planning`\|`Combat`\|`Resolution`\|`MatchOver`), `round`, `stage`, `round_in_stage`, `pve`, `mother_nature` (this is one of her rounds: a gift phase first, and no shop until the round is over), `shop_closed` (no shop this round: Mother Nature's, or the opening round 1 where everyone is dealt a free unit), `duration_ticks` (the phase's REAL length: a Combat phase lasts as long as the round's longest fight plus a 2 s linger, at least 3 s and at most 35 s, and this is known from its first tick), `ticks_remaining`, `server_tick`. Clients count the phase down themselves at 30 ticks/s |
| `public_state` | whenever it changed | `round` and `players`: `player_id`, `alive`, `health`, `level`, `streak`, `placement`, **`board`** and (revision 2) **`bench`** (units, with their items: public, as in TFT, so a client can show a scouted arena). Gold, XP, shop and item bag are **not** in it |
| `combat_summary` | a Combat phase begins | `round`, `fights`: `index`, `home`, `away` (`null` for monsters), `away_is_ghost`, `away_is_monsters`, `encounter`, `events` |
| `player_damaged` | a PvP round resolved | `player_id`, `damage`, `health` |
| `player_eliminated` | | `player_id`, `placement` |
| `match_over` | the match ended | `winner`, `placements` (per seat) |

### Combat logs
The `combat` message carries a whole fight, ready to play back: the client just replays the events at their ticks (tick 0 = start of
the Combat phase) and simulates nothing. It is sent to the players *in* that fight (both sides of a PvP fight; the home player of a PvE
or ghost fight); anyone can fetch any fight with `get_fight`, and a reconnecting player gets theirs again.

```
{"type": "combat", "round": 5, "fight_index": 1, "home": 3, "away": 6, "away_is_ghost": false, "away_is_monsters": false, "encounter": 0,
 "unit_items": {"16777217": [4]}, "winner": "home"|"away"|"draw", "winner_survivors": 4, "end_tick": 812, "survivors": [4, 0], "checksum": "1f2e...16 hex digits",
 "columns": ["tick", "type", "team", "unit", "other", "from_x", "from_y", "to_x", "to_y", "amount", "hp_after", "champion", "star",
             "absorbed", "subtype", "flags", "ability", "duration", "mana_max", "mana_regen", "reduced", "trait_id", "windup", "flight", "kind", "shape", "size"],
 "events": [[0, 0, 0, 16777217, 0, 0, 0, 3, 5, 500, 500, 9008, 1, 0, 0, 0, 0, 0, 60000, 0, 0, 0, 0, 0, 0, 0, 0], ...]}
```
Each event is an array in the order of `columns`; `type` indexes `combat_event_types` in `match_started` (`Spawn, Move, Attack, Damage, Death,
SpellCast, ShieldApplied, ShieldEnded, StatusApplied, StatusEnded, ManaChanged, Heal, TraitActivated, Teleport, SpellInterrupted, Overtime`), and which
columns each type uses is documented in `include/w2f/Combat.h`. `checksum` is the engine's 64-bit FNV-1a over the events (hex, because 64
bits do not fit a JSON number): a client can recompute it to verify the stream arrived intact. Damage and eliminations are *not* in the log:
they arrive as `player_damaged` / `player_eliminated` when the round resolves.

The last five columns are the **presentation contract**: `windup`, `flight`, `kind`, `shape`, `size` (an Attack's swing and projectile timings, a SpellCast's area, a typed damage-over-time's visual). **A row's tick is the moment its effect
lands**; an animation may start earlier by `windup` (+ `flight`). A fight goes into **overtime** at tick 900 (an `Overtime` row: everything acts 4x faster) and continues until one team is wiped out: there is no time-out and no draw by time
(only a same-tick mutual wipe-out; a 120 s safety limit decides a fight that could never end). Everything, with the animation rules and a playback design, is in `docs/UE5-Integration.md`.

### Who gets what (privacy)
| | owner | other players |
|---|---|---|
| gold, XP, shop offer, item bag, income, PvE drops, Mother Nature offers and picks, own unit events | yes | **never** |
| board and bench (units, positions, items; the bench is public since revision 2), health, level, streak, alive / placement | yes | yes |
| phase, damage, eliminations, combat summary, combat logs, results | yes | yes |

This is enforced where the messages are built: private messages are queued *for a seat* and cannot reach another connection, and `public_state`
is generated without the private fields. `TestPrivacyAndDelivery` audits every message every client receives across a whole match.

### Errors
`error` messages carry a stable `code`:

| code | meaning |
|---|---|
| `invalid_json`, `not_an_object`, `missing_action`, `unknown_action`, `missing_field`, `unknown_field`, `wrong_type`, `out_of_range`, `too_large` | the message failed validation; nothing reached the game |
| `not_in_match` | a game command before the match started |
| `no_such_fight` | `get_fight` for a fight that does not exist this round |
| `rate_limited` | too many messages (a bucket of 30, refilling 15/s; `get_state` costs 3, `get_fight` 5) |
| `too_many_violations` | 50 errors on one connection: it is disconnected (close 1008) |
| `match_in_progress`, `match_finished` | connection refused (close 1013) |
| `replaced` | a newer connection took this seat (close 4001) |

An error never closes the connection by itself (except `too_many_violations`).

### Reconnecting
Connect with `?token=...`. The server sends, in this order: `welcome` (`reconnected: true`), `match_started`, `phase` (with the real `ticks_remaining`),
`state`, `public_state`, and during Combat / Resolution `combat_summary` and the player's own `combat`. That is everything needed to redraw the game as it is now.

## WebSocket details and limits
RFC 6455, server side, no extensions or subprotocols. Client frames must be masked, text only (binary is refused), messages at most 8 KiB, fragmentation allowed.
The handshake must arrive within 5 s, at most 8 KiB. The server pings every 15 s; a connection silent for 60 s is dropped; a client that stops reading (more than 16 MiB queued for it) is dropped;
at most 64 sockets are open at once. Protocol violations are answered with the RFC's close code: 1002 protocol error, 1003 binary, 1007 bad UTF-8, 1009 too large, 1008 policy.

| close code | why |
|---|---|
| 1000 | normal: match over, lobby reset |
| 1001 | server shutting down |
| 1002 / 1003 / 1007 / 1009 | malformed WebSocket traffic |
| 1008 | policy: too many violations, idle timeout, too slow to read |
| 1013 | try again later: match in progress |
| 4001 | replaced by a newer connection with the same token |

## Crash recovery and private servers
`--autosave FILE` writes ONE file at the start of every Planning phase: the engine snapshot plus a small JSON "seats" record (each human seat's reconnect token, which seats are bots, the bots' state). `--resume FILE` restores
a match from it; the players reconnect with their tokens and are resynced exactly as after a dropped connection. A finished match deletes the file. `--join-code CODE` makes every connection bring `?code=CODE`.
Details, Docker and TLS: `docs/deploy.md`.

## Not built yet (deliberately)
* **TLS.** Terminate it in a proxy (`docs/deploy.md` has a Caddy setup).
* **Windows.** The socket layer has a Winsock branch that has never been compiled or run; Linux and macOS are tested.
* **Several matches in one process.** One process hosts one lobby / match; run several on different ports (`scripts/run_matches.sh`, `docker-compose.yml`) behind a proxy that routes by path.

## Testing
`make test-net` (`net/tests/net_tests.cpp`): the encoders against RFC vectors; the handshake and frame parser against the RFC's own examples and ~30 malformed cases each, plus fuzzing;
command validation with ~90 cases and 20,000 mutated messages; the lobby, reconnect, routing, privacy and rate limiting over a fake transport; a whole 3-player match audited message by message;
the networked-match-equals-bare-engine replay; every message kind checked against the JSON Schemas (and every command the parser accepts against the command schema); crash recovery (a resumed server plays on identically); the join code; an eight-client soak test over real sockets with reconnects; and real loopback sockets for handshakes, close codes, fragmentation, abrupt disconnects, timeouts, the connection cap, slow readers and the production loop on a thread.
