# Match snapshots (save / restore)

```cpp
std::vector<std::uint8_t> bytes = match.Snapshot();                       // the ENTIRE authoritative state
auto again = MatchManager::Restore(bytes, config, championDb,             // or nullptr + *error
                                   std::make_unique<CombatSimulator>(config.combat, traits, items),
                                   &error, items, encounters, motherNature);
```

Use it for reconnects (resume a seat's match on another process), rollback while debugging ("go back to round 12"), and regression fixtures.

## What "perfect" means
The restored match has the same `StateHash()`, produces the same bytes if snapshotted again, and every later tick, purchase, shop roll, fight and elimination is identical to the
original's. The tests prove it three ways: 300+ snapshots through a whole bot match (every phase, eliminations, items, merges) each restored and re-snapshotted byte-for-byte;
restored twins run in lock-step with the original through to the end of the match (one twin re-restored every 137 ticks); and a random-action fuzz where one match is replaced
by its own restored copy every 25 actions.

`StateHash()` covers hidden state too: the match RNG, every shop's RNG, every player's unit-id counter and the match seed. Two matches that *look* the same but would
diverge on the next roll hash differently.

## What is in the snapshot
* header — magic `W2FS`, format version (currently 3), seed, hashes of the game config / champion data / item data / PvE data / Mother Nature data, the `StateHash`, phase, round, tick in phase, player count
* body — match RNG, pool counts, every player (health, level, XP, gold, streak, placement, units in roster order with their ids and items, item bag, shop offer, shop RNG, Mother Nature offers and whether the pick is settled),
  this round's matchups (PvE ones name their encounter), and every fight's outcome (result, survivors, damage dealt, PvE drop) **including the full event log** (a client reconnecting mid-round needs the stream it missed; Resolution needs the results)
* trailer — FNV-1a checksum of everything before it

The encoding is fixed-width little-endian integers written byte by byte: no floats, no padding, the same bytes on every compiler and platform. A large mid-round snapshot is
a few hundred KB, almost all of it the fight logs.

## What is not in it
Listeners (attach them after restoring), the combat simulator object (pass a new one to `Restore`), the champion / item / trait data files, and **clients** — AI bots and scripted
players are not match state. Save a bot with `AIBotController::GetState()` and give it back with `SetState()`.
Data files are represented by hashes only: `Restore` refuses a snapshot taken under a different game config, champion data (ids, names, costs, traits, base stats, which abilities
exist), item data, PvE data (monsters, encounters, drop tables) or Mother Nature data (the gifts and their weights), because the recorded fights and the pool were built from the old data. The numbers *inside* an ability's effects are deliberately not hashed, so re-tuning a spell
keeps old snapshots restorable. `RestoreOptions opts; opts.requireMatchingData = false;` skips the hash check for deliberate "replay this under retuned data" debugging; the match then
continues under the new rules, and a snapshot whose champions no longer exist is still refused.

## Never trust the buffer
`Restore` returns `nullptr` with a message and never crashes on garbage: the trailer checksum (any flipped bit or truncation), magic/version, every count against the bytes that could hold
it, every id resolved in the loaded data, every unit's placement and ownership, level/XP consistency, RNG states, item ids — and finally the rebuilt match must pass the pool-integrity and
roster-layout checks **and** hash to exactly the `StateHash` in the header, so a buffer that decodes cleanly but describes an impossible match is refused too.
`ReadSnapshotInfo(bytes, info)` reads and verifies only the header (seed, round, phase, hashes) without restoring.

## Automatic snapshots
The MatchManager takes one itself at the start of every Planning phase and offers it through `IMatchListener::OnAutoSnapshot` and `LastPlanningSnapshot()` - see `docs/game-loop.md`. It is
derived data: it is not part of a snapshot or of `StateHash()`.

## Changing the format
Bump `kSnapshotVersion` for any change to the layout (version 2 added the PvE fields and the PvE data hash; version 3 the Mother Nature offers and data hash, and replaced the Draft phase; version 4 the presentation fields of every combat event: windup, flight, kind, shape, size). Old versions are refused with a clear message; there is deliberately no in-place migration yet.
