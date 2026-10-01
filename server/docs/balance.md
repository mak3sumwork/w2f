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


## Roster pass v3 (September 2026, design doc section 2D)

Five champions joined (Pulsar, Rampart, Skarn, Maren, Vector), Faire and Lich lost Sorcerer (Lich became a Mystic), Les and Lum lost the Omnilium tag and
Protector gained a (1) tier (15% less damage taken). The design sheet's numbers stay in `tests/data/designer_spec_champions.json`. At the sheet's numbers
(tanks HP x0.68, armor x0.78; carries HP x0.9) all five won 61-63% and overtime rose to 15.6%. After two trims (`make balance ARGS="--matches 300 --seed 1000"`,
full report in `balance/roster-v3.txt`):

| champion | changed (balance pass) | win rate |
|---|---|---|
| Pulsar | HP 430/775/1395, shield 175/260/400 (sheet 250/375/560) | 54.9% |
| Rampart | HP 430/775/1395, armor/MR 28, shields 200/300/450 self and 80/120/180 allies (sheet 300/450/675, 120/180/270) | 54.8% |
| Skarn | none | 54.3% |
| Maren | HP 600/1080/1944, 20% damage reduction (sheet 30%), wave 190/285/680 +8% max HP (sheet 250/375/900 +10%) | 56.5% |
| Vector | barrage shots 35/55/250 +20% AD (sheet 60/90/400 +30%) | 61.5% (5-costs run high: Aureon 60%, Yggra 59%) |
| Faire | AP 38/58/75 (sheet 28/45/60) after losing Sorcerer | 40.9% -- still weak |

Fights: mean 20.1 s, overtime 12.7% (8.7% before trait system v2 + v3), 0.6% reach the safety limit; shield units (Moss, Ignis, Sunna, Fern, Rampart,
Aureon) are 2-3x as common in long fights. Left for the full balance pass: Faire, the shield stall, Vector / Aureon / Yggra / Aphel near 60%.

## Demo 1.7: the full balance pass (FEEDBACK V7, 2026-09-28..10-01)

Two yardsticks now. `make balance` (bot matches, above) mixes a champion's power with how the bots play it, and the bots almost never build a trait past its
first breakpoints (out of ~8,000 final boards Helios (5) came up 39 times, Coregons (6) never). So there is a second tool:

**`make ladder ARGS="--fights 1000 --mode both"`** (`tools/trait_ladder.cpp`, `build/w2f_ladder`, ~1 min): fights between two boards of **exactly the same gold, cost by cost**.
* `traits`: for every trait tier, board A holds that many different trait champions (a prismatic tier gets one emblem holder; a tier with too few champions gets
  emblems on fillers), filled to 8 units (or the tier's count) with random others; board B is one random champion of the same cost for each of A's units. A's win
  rate is the tier's worth: 50% = nothing.
* `champions`: the champion + 7 random others against the same costs at random.
* 1- to 3-costs are 2-star, 4- and 5-costs 1-star; tanks stand in front. `--items` adds a random finished item per unit. **Nature's plants are not fielded** (the
  match puts them on the board, not the duel), so read Nature from `make balance`. At 400 fights a rate moves about +-5 points between seeds; use 1,000.

### Rules changed
* **Sudden death** (`CombatConfig::overtimeHealingCutPercent` 50, `overtimeDamageRampPercent` 10): in overtime heals and shields received are halved and every
  hit grows +10% per whole second of overtime. Shield / heal teams used to stall to the 120 s safety limit.
* **Ability power scales the whole ability** (TFT's AP): a champion's own cast multiplies the flat part of its damage, heals, shields and DoTs by its ability power
  (base + AbilityPower statuses: Sorcerer, Selini, Continuum Cogs ...). Item / trait / passive hooks keep their flat numbers. (`TestAbilityPowerScalesFlatAbilities`)
* **PvE has teeth**: from stage 2 a LOST monster round costs health like a lost fight (`MatchConfig::pveLossDamageFromStage` 2; stage 1 stays free). Every monster
  has an ability (Regrow, Acid Glob, Rockfall ...), each X-7 round has its own boss board per stage (2..5, plus one for stage 6+) at 2-3 stars, bosses always drop 2 items.
* Helios and Nature top tiers **11 -> 10** (a board holds 10 units: 11 could never switch on).

### Traits (equal-gold win rate of the trait board, ladder, 1,000 fights)
| trait | changed | ladder after |
|---|---|---|
| Helios | (5) 35 -> 28 resists; Solar Smite 15% -> 5% max HP per Rally | 57 / 69 / 89 / 100% |
| Selini | Enlightenment 5/8/12% + 1.5/1.8/2.5 per level; Prosperity 6/14/28% + 2 per gold | 60 / 72 / 86% and 55 / 65 / 85% |
| Coregons | (3) 3 Souls at 30% HP, heal 18%, echo 6/8/12%; (6) mana tax 15 -> 5, regen 2 -> 0.5/s, **no HP drain / execute**; (8) drain 2%/s, execute 6% | 63 / 89 / 100% (was 51 / 95 / 100) |
| Assassin | crit damage / chance 30/25 and 50/40 (was 20/15, 30/25) | |
| Bastion | every ally 10 / 15 / 25 resists | |
| Bruiser | every ally +100 / 150 / 200 HP, holders 20 / 45 / 70% | 56 / 61 / 78% |
| Sorcerer | holders 20 / 40 / 60 / 90% AP | |
| Marksman | (2) 20% AS, 12% amp | |
| Mystic | 25 / 60 MR, 2 / 4 mana per second | |
| Duelist | + 8 / 15 / 20% damage reduction; 6 / 9 / 12% AS per attack | 47 / 61 / 86% |
| Gunslinger | 28 / 38% AD, Quick Draw 160 / 200 | |

A trait's natural top tier (every champion of it, no emblem) now wins **84-89%** of equal-gold fights (Hexagon 6 84%, Selini 7 85-86%, Duelist 6 86%, Helios 7 89%,
Coregons 6 89%); emblem-only tiers (Helios 10, Phaisa 9, Coregons 8) ~100%. That is on purpose: a full vertical should beat a random board of the same gold, as in TFT.
Full report: `balance/ladder-1.7.txt`.

### Champions
Scaled in rounds (each marked `// balance 1.7:` above the champion in `data/champions.json`; only champions weak or strong in BOTH tools were touched).
Final round of this session: Rot DoT x1.32 and HP x1.21, Pyra x1.15 / AD x1.1, Null HP x1.12, Bit x1.1 / HP x1.1, Kryx HP x1.19 / AD x1.08 / Frenzy 60/90/170% AS,
Mortis bonus hits x1.32, Alesk HP x1.19 / shield 20/31/50% max HP, Astra HP x1.15 / tether 30/40/60% AS, Faire AP 42/64/83, Cyla HP x1.08 / rockets 64/97/175,
Grave's Skeleton 450/750/1150 HP and 45 AD (the earlier "abil x1.2" notes on Grave never touched the summon: they did nothing); Ignis x0.9 / HP x0.95,
Sunna heal x0.79, Aphel x0.87 / AD x0.95, Baira x0.93, Oakheart HP x0.96.

### Where it stands (`make balance ARGS="--matches 300 --seed 1000"`, report `balance/demo-1.7.txt`; roster v3 before it in brackets)
* Fights: mean 19.2 s (20.1), 99th percentile 36.0 s (46.3), overtime 12.0% (12.7%), **no fight hit the safety limit (154)**, longest 60.8 s.
* Champions: every one of the 53 between **45.7% (Astra) and 61.4% (Yggra)** (40-64% before); only 5-costs are above 58% (Yggra, Vector, Umbra, Nihila: 5-costs
  run high as a group, 56.7%). Ladder: every champion 40-57% at equal gold; the lowest there (Bit 40%, Faire 41%, Solis 42%, Talon 42%) win 50-53% in bot games, so they were left alone.
* Synergies in bot games: Selini (3) 58.5%, Nature (3) 58.0% (free plants), Protector (2) 59.4%, Duelist (4) 61.1%, Najmi (4) 60.6%.
* PvE: the bots still win 97-100% of monster rounds (99.6% at 3-7, 98.8% at 4-7, 98.1% at 5-7). A loss now costs health, so the rounds were made harder but not
  turned into a coin flip; how hard they should be is the designer's call.
* The last 120 s fight (demo 1.7.1): a lone Alesk with **two Guardians Armors** against four Lost Souls. Every 6th attack taken gave +6 Armor / +8 MR with no cap,
  and the Souls' fast hits in 4x overtime stacked his resists faster than sudden death ramps damage (hits of 8 at +900%). Guardians Armor and Soldiers' Soul now
  stack at most **15 times**. Two 300-match runs (seeds 1000 and 5000, ~53,000 fights): 0 reach the safety limit. `make balance` now prints who was standing
  in any fight the safety limit decides ("safety limit, round N: ...").
