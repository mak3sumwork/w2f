# The game loop: stages, PvE rounds, damage, streaks, recovery

All numbers are in `GameConfig` (`include/w2f/Config.h`); none are hard-coded in the rules.

## Stages and rounds
Rounds are counted 1, 2, 3 ... and shown as stage-round: **1-1, 1-2, 1-3, 2-1 ... 2-7, 3-1 ...** (`MatchConfig::StageOf`).
Stage 1 has `firstStageRounds` = 3 rounds, every later stage `roundsPerStage` = 7. `MatchManager::CurrentStageRound()` and `IsPveRound()` tell a viewer where it is.

**PvE rounds** are the first `firstStagePveRounds` rounds of stage 1 (all three) and round `pveRoundInLaterStages` (7) of every later stage,
so 1-1, 1-2, 1-3, 2-7, 3-7 ... (`pveRoundInLaterStages = 0` means none after stage 1). Every other round is PvP.

Each round runs [MotherNature] -> Planning -> Combat -> Resolution, where MotherNature only exists on Mother Nature's rounds (below). Income is paid at the start of the round (see Streaks).

## Timing, combat length and overtime
| phase | length (`MatchConfig`) |
|---|---|
| Mother Nature (her rounds only) | 20 s (ends sooner once everybody has picked) |
| Planning | 30 s |
| Combat | as long as the round's longest fight + `combatLingerTicks` (2 s), at least `combatMinTicks` (3 s), at most `combatTicks` (**125 s**: the safety limit + the linger; normal fights are ~20 s); known from the first tick (`MatchManager::PhaseTicks()`). `combatEndsWithFights = false` makes every Combat phase exactly `combatTicks` |
| Resolution | 3 s: damage, gold and streaks are shown, then the next round starts |

* **Fight length**: regulation is `CombatConfig::regulationTicks` (30 s). A fight still undecided then goes into **overtime**: from that tick every unit's **attack speed, movement and mana regeneration run `overtimeSpeed` (4) times faster**
  (attack intervals and step times are divided by 4; cast animations, damage-over-time and status durations keep their normal length). The stream carries one `Overtime` event at tick 900 (`amount` = the factor; `duration` 0: it has no end) so a viewer can announce it, and every later Move / Attack
  simply comes 4x as often. **Overtime lasts until one team is wiped out: there are no timeouts and no draws by time.** Only both teams dying on the same tick is a draw. `CombatConfig::hardLimitTicks` (120 s) is a safety net against a fight that can never end (two teams that cannot
  hurt each other): when it is reached the fight is decided deterministically (more surviving units, then more total HP, then a coin from the fight's seed), so a fight ALWAYS has a winner. The fight is a pure function of its inputs, overtime included, so determinism is unchanged.
* **The shop** is open in every round except round 1 (the opening, below) and Mother Nature's rounds; in those two it is locked completely (`ShopClosed`, the offers are empty) in every phase. **It stays open while units fight**: in Combat and Resolution a player can buy XP, reroll and buy champions.
  A champion bought then goes to the **bench** (never the board) and may only merge with bench units; a copy that would have to merge into a unit on the board is refused (`UnitInCombat`). **The board is locked** during Combat and Resolution: selling, moving and equipping (or unequipping) are allowed for bench units only.
  (Mother Nature's own phase accepts only `pick_gift`.)

## The opening and the board size (TFT rules)
* **Round 1 is the opening**: `MatchConfig::shopClosedOpeningRounds` (1) rounds have **no shop** (`TryBuyShopUnit` / `TryRerollShop` answer `ShopClosed`, exactly as in a Mother Nature round), and in `MatchManager::Start()` every player is
  dealt one random unit of each cost in `openingUnitCosts` ({1}, **ASSUMED**: the designer said "a random unit") from the shared pool, free (`OnUnitBought` with 0 gold). It lands on the bench; the player puts it on the board. The shop opens in round 2.
  The deal has its own RNG stream (`kRngStreamOpening`), so it is part of the match seed. `MatchManager::IsShopClosed()` covers both the opening and Mother Nature's rounds.
* **The board holds as many units as the player's level** (`PlayerConfig::limitBoardToLevel`, now on by default): level 1 = 1 unit ... level 10 = 10 (`kMaxPlayerLevel`). Moving a bench unit onto an empty cell at capacity answers `BoardFull`; swaps are always allowed.

## Mother Nature (replaces the carousel and augments)
Every `MatchConfig::motherNatureEveryRounds` rounds (**3**: rounds 3, 6, 9 ... whatever their stage) the round opens with a **MotherNature phase** (`motherNatureTicks`, 20 s) *instead of a shop*.
Every living player is privately offered **2 random gifts** (`options` in `data/mother_nature.json`) and may take exactly **one**, for free (`TryPickGift`). The **shop stays closed for the whole round**: `TryBuyShopUnit` / `TryRerollShop` answer
`ShopClosed` during its Planning phase (buying XP, selling, moving and equipping still work), and the players' shop offers are returned to the pool when Planning begins.
Without `mother_nature.json` loaded there are no such rounds: every round has its shop.

* **The gifts** (data, tweakable weights): a tier is used from its `fromStage`; tier 1 = a random component item, +5 gold, +4 XP, Mother's Blessing (+3 player HP), a random 2/3-cost unit; tier 3 (from stage 4) = a completed legendary item, an emblem (low chance), +15 gold, Mother's Miracle (+7 HP), a 5-cost unit.
  The two options are **distinct gift kinds**, chosen by weight; a kind that cannot be handed out right now (no item of its class, its unit tiers sold out) is never offered. Offers are **concrete** ("Omnilium Heart", "Soul"), rolled from the round's own random stream (`kRngStreamMotherNatureBase + round`).
* **Units** on offer are checked out of the shared pool exactly like shop slots (`VerifyPoolIntegrity` counts them), and go back when another gift is picked or the phase ends. A unit with no room for it (bench and board full, no merge) is paid as gold equal to its cost, like a PvE champion drop.
  Heal never takes a player above their starting health.
* **The phase ends** as soon as every living player has settled (picked, or had nothing on offer), or when its time is up: whoever has not picked then gets their **first offer automatically** (`OnGiftPicked(..., automatic = true)`).
* **Events**: `IMatchListener::OnGiftsOffered(player, offers)` for each living player when the phase opens, `OnGiftPicked(player, index, offer, automatic, goldConverted)` for each pick. A unit gift is also announced through `OnUnitBought` (0 gold).
  Offers and picks are private to their owner. Bots (`AIBotController`) take a unit, then an item, then gold, then XP, then healing.

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
