# The game loop: stages, PvE rounds, damage, streaks, recovery

All numbers are in `GameConfig` (`include/w2f/Config.h`); none are hard-coded in the rules.

## Stages and rounds
Rounds are counted 1, 2, 3 ... and shown as stage-round: **1-1, 1-2, 1-3, 2-1 ... 2-7, 3-1 ...** (`MatchConfig::StageOf`).
Stage 1 has `firstStageRounds` = 3 rounds, every later stage `roundsPerStage` = 7. `MatchManager::CurrentStageRound()` and `IsPveRound()` tell a viewer where it is.

**PvE rounds** are the first `firstStagePveRounds` rounds of stage 1 (all three) and round `pveRoundInLaterStages` (7) of every later stage,
so 1-1, 1-2, 1-3, 2-7, 3-7 ... (`pveRoundInLaterStages = 0` means none after stage 1). Every other round is PvP.

Each round runs Draft (only on draft rounds) -> Planning -> Combat -> Resolution. Income is paid at the start of the round (see Streaks).

## PvE rounds
* Everyone alive fights the **same encounter** (chosen from `pve.json` by stage/round, ties broken by the match seed) on their own copy of the board. The fight is an ordinary log played back by the client; the monsters have unit ids above `kMonsterUnitBase` and are on team 1.
  The `Matchup` is `home = player, away = kInvalidPlayerId, awayIsMonsters = true, encounter = <id>`.
* **Win** (all monsters dead): one random drop is added to the player's gold, roster (a champion, drawn from the shared pool) or item bag (`PveDrop`, stored on the `CombatOutcome` and announced by `IMatchListener::OnPveDrop`). Drop tables and their fallbacks: `docs/champions-json.md`, section `pve.json`.
* **Loss or draw**: nothing. PvE never damages a player and never changes a streak.
* With no PvE data loaded (`encounters` null, or nothing defined for that round) the round is a round nobody fights: no result, no drop.
* Drops use their own random streams (`kRngStreamPveBase`, `kRngStreamPveDropBase`, per round), so PvE never disturbs the pairing shuffle of the PvP rounds around it.

## Player damage and elimination (PvP)
When a PvP round resolves, the loser takes **`baseDamageByStage[stage]` + `perSurvivingUnit` x (units the winner has left alive)** (`GameConfig::PlayerDamage`;
the table's last entry repeats for later stages). The simulator only reports the winner and the survivor count (`CombatOutcome::winnerSurvivors`); the rule lives in the MatchManager,
which stores the result in `CombatOutcome::damageToLoser` and fires `OnPlayerDamaged`. Draws hurt nobody; a ghost (the odd player's opponent copy) is never hurt.

Damage is applied the moment the Resolution phase begins (the phase itself is the window in which a viewer shows the result). A player at 0 HP or below is **eliminated** right then:
their units and shop offer go back to the shared pool, they get a placement (the biggest overkill / lowest health places worst), `OnPlayerEliminated` fires, and the match ends when one player is left.
Items on an eliminated player's units and in their bag leave the match with them.

## Streaks
A win extends a win streak and a loss extends a loss streak (a result of the other kind restarts it at 1; a draw leaves it alone). PvE rounds do not touch streaks, and a ghost's owner gets no result from being copied.
At the start of every round each living player is paid `base + interest + streak` gold (`PlayerConfig::streakBonuses`, default 2-3 in a row = +1, 4 = +2, 5+ = +3, for win **and** loss streaks) and passive XP.
`IMatchListener::OnIncomeGranted(player, round, IncomeBreakdown)` reports every part of it, streak gold included.

## Automatic snapshots (crash recovery)
When a **Planning** phase begins - shops refreshed, before anyone can act - the MatchManager takes a snapshot of the whole match (`GameConfig::snapshot.atPlanningStart`, on by default) and
1. keeps it: `LastPlanningSnapshot()` / `LastPlanningSnapshotRound()`;
2. hands it to listeners: `IMatchListener::OnAutoSnapshot(round, bytes)` - **this is where the server persists it** (a file, a database row). The library does no file I/O of its own.

After a crash, `MatchManager::Restore(bytes, ...)` puts the match back at the start of that round's Planning phase (a restored match adopts the snapshot as its own recovery point). Players re-issue whatever they did since; AI bots
are saved and restored with `AIBotController::GetState/SetState`. `tools/sim_demo.cpp` runs this drill: it discards the match in the middle of a PvE fight and carries on from the snapshot, ending in exactly the same state as an uninterrupted run.
Format, validation and what is (not) inside a snapshot: `docs/snapshots.md`.
