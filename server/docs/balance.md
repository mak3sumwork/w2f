# Balance: the bulk simulator, and the Phase A pass

## The tool
`make balance ARGS="--matches 300 --seed 1000 --json report.json"` builds `build/w2f_balance` (optimised, no sanitizers) and plays that many seeded matches with **8 bot players** under the real rules
(opening round, board = level, Mother Nature, items, endless overtime). About 20 seconds for 300 matches. It reads the engine's own combat logs, so it cannot disturb a match, and the same seeds give
the same report on every platform. It reports, over every PvP fight (PvE excluded):

* fight length (mean / median / 90th / 99th percentile / longest), how often overtime was needed, how many fights ran past 35 s, how many hit the 120 s safety limit, mutual wipe-outs (the only draws);
* per champion: fights fielded, **win rate of the fights it was fielded in**, average star, damage dealt per fight; the same by cost and by role;
* per synergy tier: teams whose *highest active tier* it was, and how they did;
* the champions over-represented in very long fights (the stall suspects); the home side's win share (a bias check).

Read it as a *relative* yardstick: bots play the game in a fixed, simple way, so the numbers say which champions and synergies are far ahead or behind under that play, not how humans will do.
A win rate is confounded (a champion that appears in strong comps looks strong), so it points at outliers; it does not prove one. Outliers are flagged at 42-58% with at least 150 samples.
The designer's brief for the pass: nerf massive outliers, do **not** chase 50/50 for everything; a slightly stronger champion or synergy is fine.

## The Phase A pass (2026-09-21): speed the fights up, nerf the outliers
Reports: [`balance/before.txt`](balance/before.txt) (the designer's numbers, with the new endless overtime), [`balance/after.txt`](balance/after.txt) (same 300 seeds after the pass),
[`balance/after-fresh-seeds.txt`](balance/after-fresh-seeds.txt) (300 different seeds, to check the pass did not overfit).

| | before | after | after, fresh seeds |
|---|---|---|---|
| mean / median fight | 23.4 s / 21.8 s | 19.9 s / 18.9 s | 20.1 s / 18.9 s |
| 90th / 99th percentile | 32.1 s / 47.9 s | 29.1 s / 34.7 s | 29.5 s / 35.4 s |
| fights that needed overtime | 20.5% | 8.7% | 9.4% |
| fights past 35 s | 4.7% | 0.9% | 1.1% |
| fights that hit the 120 s safety limit | 113 of 30,362 (0.37%) | 9 of 30,895 (0.03%) | 14 of 30,837 (0.05%) |
| matches | 37.6 rounds | 38.2 rounds | 38.1 rounds |
| tanks vs damage dealers (win rate) | 51.8% / 49.6% | 49.5% / 50.7% | |

What was changed (all in `data/champions.json` and `data/traits.json`; the designer's original numbers are frozen in `tests/data/designer_spec_*.json`, which the mechanics tests use):

* **Tanks** (10 of the 12: Les, Lum, Ignis, Null, Bone, Bit, Solis, Nyx, Kryx, Umbra): HP x0.68 and armor / magic resist x0.78. **Every other damage champion**: HP x0.9. This is what shortened the fights.
  Alesk and Soul keep the designer's numbers (Alesk was already weak; Soul is the Coregons engine, handled below).
* **Stall breakers**: Ignis' ward 200/300/450 for 4 s -> 120/180/270 for 3 s; Byte's ward 200/300/500 for 4 s -> 150/225/375 for 3 s (Ignis / Byte / Bone were 3-5x over-represented in 60 s+ fights).
* **Strong outliers**: Mortis (62%): attack damage 75 -> 60, Death's Volley hit 150/225/400 -> 115/175/300. Vega (64%): the channel 100/150/400 -> 75/115/300 per pulse and 500/800/2000 -> 375/600/1500 finale.
  Soul: spawn shield 800/1200/1800 -> 560/840/1300. **Coregons 3** (73%!): 2 Lost Souls instead of 3 at the first breakpoint, their max HP 25% -> 14% of the tankiest ally, echo 5/8/12% -> 3/5/8%, Lost Soul attack damage 30 -> 20,
  first-tier lifesteal 10% -> 7%. Hexagon 4 (79%): shield 600 -> 300 and detonation 200 -> 120. Selini 3 (67%): heal 20% -> 6%. Assassin 4 (61%): crit damage +50% -> +30%, crit chance +30 -> +25.
* **Weak outliers**: Baira (34%): her sheet had 100/150/200 HP and a 15/21/40 "AP" burn that did almost nothing; now 300/540/970 HP, attack damage 30/38/45 at 0.6 speed, AP 60/90/160 (a **deliberate deviation from the design document**, which lists her at 100/150/200 HP).
  Rot (43%): poison 90/140/220 -> 120/180/280. Alesk (42%): +10% HP, +15% armor / magic resist, shield 7/15/30% -> 12/22/40% of max HP. Phaisa 3 (42%): +6% instead of +4% attack damage and AP per death.

## Where it stands after the pass
Fights are shorter and rarely need overtime. Remaining outliers (fresh seeds): Vega 64% and Baira 40% among champions; Coregons 3 63%, Selini 3 67%, Hexagon 4 59%, Assassin 4 59% among synergies, all within
"slightly stronger / slightly weaker", which is the brief. The higher breakpoints (Coregons 6/8, Helios 6, Hexagon 4-5) are still almost never reached by bots (a handful of samples), so they remain unmeasured:
they need human play or a bot that builds for them. Re-run `make balance` after any data change and compare against `balance/after.txt`.

`tests/tests.cpp` (`TestProductionDataKeepsTheDesignerStructure`) makes sure the balance data keeps the designer's champions, abilities and synergy breakpoints: only numbers may move, and none may run away by more than 3x
(Baira 6x, documented above).


## September 2026 champions (first pass)

The 8 new champions and the reworked Baira came in at the design sheet's raw numbers while the rest of the roster had been through the balance pass. They got the same treatment: tanks (Tide, Kael) HP x0.68, armor / MR x0.78; the others HP x0.9. Their abilities were then trimmed twice; the sheet's numbers stay in `tests/data/designer_spec_champions.json`.

| champion | sheet | now |
|---|---|---|
| Baira (Crashing Tide, per wave) | 170/255/400 + 100% AP | 95/140/230 + 40% AP |
| Kael (Dawnbreaker) | 200/300/470 + 120% AD | 150/225/350 + 90% AD |
| Aphel (per arrow) | 160/240/600 + 110% AD | 120/180/450 + 80% AD |
| Sola (per slash) | 90/135/330 + 40% AD | 75/115/280 + 35% AD |
| Morrah (Rift Collapse) | 320/480/1200 + 100% AP | 200/300/760 + 60% AP |
| Aureon (Solar Judgment) | 600/900/4000 + 150% AP | 330/500/2200 + 90% AP |
| Nihila (Devour Reality) | 400/650/5000 + 120% AP | 300/480/3600 + 90% AP; execute 20/25/50% (engine cap 50%) |

After it (`make balance ARGS="--matches 300 --seed 1000"`): Aureon 63%, Baira 63%, Nihila 61%, Sunna 59%, Morrah 58%, Aphel 58%, Kael 57%, Sola 56%, Tide 55% (the old roster sits at 40-55%, Vega was 64%).
**Selini (3) wins 66%**: the old Selini bonus (drop aggro and heal once below 50% HP) now has 7 units to trigger it. The Selini / Helios / Phaisa reworks in the design doc replace these traits, so the real balance pass belongs after them.

## Trait system v2 (September 2026)

60 bot matches (`make balance`, seeds 1..60) with the whole v2 system on (bots answer module / prototype choices and ignore plants). After two tuning rounds every
champion and synergy with 150+ samples sits between about 41% and 64% (the old roster's spread was 40-66%); fights average 18.5 s, 8.7% reach overtime.
Numbers moved away from the design sheet (each marked `balance pass` in the data):
* Aureon 280/420/2000 +75% AP, ally shield 200/300/1500; Kael 125/190/300 +75% AD, shield 15% max HP; Sunna heal 120/180/300 +75% AP, resists 20/30/50;
  Baira waves 75/110/190 +30% AP; Tide shield 150/220/340; Talon 100/150/400 +35% AD.
* Rot poison 170/255/390; Nyx cleave 180/270/420; Lum's punch gets a flat 150/225/500 (+75/110/250 behind).
* Plants: Stonebark Tree 350/550/900 HP; the Blossom 8/12/25% then +2/4/8% every 4 s.
* Traits: Bruiser allies +60 HP and holders 12/40/65%; Marksman 8/30% AS, 6/20% amp; Phaisa (4) 25% AS; Selini Enlightenment 5/12/18% + 1.5/2.5/3.5 per level,
  Prosperity 6/20/40% + 2 per gold.
Still worth watching: Aureon (~62%), Kael / Oakheart (~58%), Nature (3) and Selini (3) (~59%), Rot (~41%).

