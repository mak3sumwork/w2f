# Design lock v1.0: the rules as they stand (2026-09-21)

This is the list of rules the engine implements and the designer has decided, frozen as version 1.0 before the Unreal Engine client is built on them. A change to anything here after this point is a
*design change* to be made deliberately (data first, engine second) and announced in `UE5-Integration.md` if it touches what a client sees. Numbers that are tuned by the balance pass live in the data files, not here.

## Match and economy
* 8 players (any 2..8 with bots filling seats). Health 100. Stages: 1-1 to 1-3, then 7-round stages (2-1..2-7, ...). **PvE rounds**: 1-1, 1-2, 1-3 and X-7 of every later stage.
* **Round 1 (the opening)**: every player is dealt one random 1-cost unit (free, onto the bench). **Demo 1.1 (FEEDBACK V1): the shop is open in every round's Planning, Combat and Resolution -- round 1 and Mother Nature's rounds included; only Mother Nature's gift phase has no shop.** (Until demo 1.1: no shop in round 1 and in Mother Nature's rounds; `MatchConfig::shopClosedOpeningRounds` can still close the opening.)
* **Board = level**: level 1 fields 1 unit ... level 10 fields 10; the bench holds 9. XP table 2/2/6/10/20/36/48/76/84, +2 passive XP from round 2, buy 4 XP for 4 gold. Shop odds and pool sizes as TFT (29/22/18/12/10 copies for 1..5-cost).
* Income 2/2/3/4 then 5, +1 interest per 10 gold (max +5), streak gold 1/2/3 at 2/4/5. Sell value = cost x copies (minus 1 for merged units above 1-cost). Three copies merge into the next star (max 3).
* **While units fight** (Combat and Resolution) the shop stays open and a bought champion goes to the bench and may only merge with bench units; selling and moving are allowed for **bench** units only; **items may be equipped on board units too (demo 1.1): they count from the next fight.** The board is otherwise locked.
* **Mother Nature** (replaces the carousel and augments): every 3rd round, 2 free gifts of the tier for the stage (early 1-2, mid 3-4, late 5+); the unit gift scales with the stage (1 / 1-2 / 3-4 / 5 cost). The shop is closed during her gift phase only (demo 1.1; it used to be closed for the whole round).
* **Items**: 8 components, the Seed, 28 finished items, 8 emblems; up to 3 per unit; two components combine on contact; the **Item Remover** (consumable, not craftable, dropped by monsters) takes all items off a unit.
* **Eliminations and placement**: a player at 0 HP is out; their units and shop return to the shared pool.

## Combat
* A fight is a pure function of both boards, the traits, the items and the round's seed; 30 ticks/s; integer math; identical on every platform (proven by the golden fights).
* **Length**: 30 s at normal speed, then **overtime at 4x** (attack speed, movement, mana regeneration) **until one team is wiped out. No draws by time.** Only a same-tick mutual wipe-out is a draw; a 120 s safety limit decides a fight that can never end. The Combat phase lasts as long as the fights plus 2 s.
* **Timers**: Mother Nature 20 s, Planning 30 s, Resolution 3 s.
* Damage to the loser of a PvP round: the stage's base + 1 per surviving enemy unit; draws hurt nobody; PvE never hurts.

## Bots (the test opponents)
Bank interest, level to a per-stage target, reroll, buy for star-ups and synergies, field the best `level` units, equip items, sell to make room, take heals when hurt. Heuristics, not strong players.

## Decisions the designer made in this phase
| question | decision |
|---|---|
| Shop during Combat? | Yes: buy XP, reroll and buy champions in Combat and Resolution (champions go to the bench). No selling or equipping on units fighting on the board |
| Fights that do not finish? | No draws, no timeouts: endless 4x overtime until a team is wiped out |
| Fights too long? | Nerf tank stats and tune numbers to speed fights up; do not chase 50/50 |
| Mother Nature unit gifts | Scale with the stage: 1-2-cost early (stage 2), 3-4-cost mid (stages 3-4), 5-cost late (stage 5+) |
| Round 1 | Everyone gets a random unit; no shop |

## Still assumed (a designer's eye wanted, none of it blocks the client)
Opening unit cost (1-cost); Mother Nature gift weights and amounts; the cast lock of 0.5 s; Lum's active; presentation timings (windup, projectile speed); the champions and items marked `ASSUMED` in the data and in `content-notes.md`;
PvE difficulty (bots beat every monster board: the encounters are placeholders); the higher synergy breakpoints (Coregons 6/8, Helios 6, Hexagon 4-5) were never reached by bots and are unmeasured.

## Demo 1.1 additions (FEEDBACK V1, 2026-09-26)
* Player damage by stage 0 / 2 / 5 / 8 / 11 / 14 / 18 (+1 per surviving enemy unit).
* Prismatic tiers (the top breakpoint of a trait with 4+ breakpoints) need at least one emblem holder.
* PvE loot, win or lose: 1-1 three Item Removers, 1-2 / 1-3 three components each, X-7 two items (pve.json `guaranteedDrops`).
* Najmi 2/4/6, cash-outs at every 100 star dust (100-600), the offer stays open until taken (GDD 2B).
* Hexa is unlockable (champions.json `unlock`): 7 Hexagon star levels at level 8; only the unlocking player's shop offers it.
