# W2F DEMO 1.7

Answers FEEDBACK V7 (`feedback/FEEDBACK_V7.md`, 2026-09-28): "now lets do balance tests and improve the game and balance it". A full balance pass over
the demo 1.6 roster (53 champions, trait system v2, roster pass v3), a second balance tool for what bot matches cannot measure, and the weak spots found
on the way (stalling shield fights, PvE without stakes, unreachable trait tiers, tooltips that showed the design sheet instead of the real numbers).
Server-side only: the UE client did not change.

| # | Asked / found | Done | How it was checked |
|---|---|---|---|
| 1 | Balance tests the bots can't do | **`make ladder`** (`server/tools/trait_ladder.cpp`): equal-gold duels, cost by cost, for every trait tier (incl. the high breakpoints the bots never reach) and every champion. Nature's plants are not fielded there (noted in its output). | 1,000 fights per row, two seeds compared for noise (about +-5 points at 400 fights) |
| 2 | Shield / heal teams stalled for minutes | **Sudden death** in overtime: heals and shields received -50%, every hit +10% per second of overtime. | fights hitting the 120 s safety limit: 154 -> 1 in 26,664; 99th percentile 46.3 s -> 36.0 s; the log validator and the Fishbones test account for the ramp |
| 3 | Ability power only scaled the terms that read it | AP now scales a champion's whole own ability (flat damage, heals, shields, DoTs), like TFT. Hooks (items, traits, passives) keep flat numbers. | new test `TestAbilityPowerScalesFlatAbilities` |
| 4 | PvE was free (bots won 100%) | From stage 2, a **lost** monster round costs health like a lost fight (stage 1 stays free). Every monster has an ability; a boss board per stage at X-7 (2..5, and one for stage 6+), 2- and 3-star; bosses drop 2 items. | PvE test (a loss from stage 2 costs health, a win or stage 1 never does); bots now lose 0.4-2% of rounds 3-7..5-7 |
| 5 | Helios 11 / Nature 11 could never switch on (max 10 units) | Top tier at **10** (data, text, design doc, frozen spec). | ladder: Helios (10) and Nature (10) are built and measured |
| 6 | Traits | Helios (5)/(7) trimmed, Selini both paths trimmed, **Coregons (6)** from 95% to 89% at equal gold (its HP drain/execute moved to (8), mana tax halved) and the dead **Coregons (3)** from 51% to 63%; Assassin, Bastion, Bruiser, Sorcerer, Marksman, Mystic, Duelist (+damage reduction), Gunslinger buffed. Every natural top tier now wins 84-89% at equal gold (a full vertical should beat a random board). | `docs/balance.md` "Demo 1.7" table; `server/docs/balance/ladder-1.7.txt` |
| 7 | Champions | Scaled in rounds, only where bot games AND the ladder agreed. Rot, Pyra, Null, Bit, Kryx, Mortis, Alesk, Astra, Faire, Cyla, Grave up; Ignis, Sunna, Aphel, Baira, Oakheart down. Found: Grave's earlier "buffs" never reached its Skeleton (now 450/750/1150 HP, 45 AD). | 300 bot matches (seeds 1000..1299): every champion 45.7-61.4% (was 40-64%), only 5-costs above 58%; `server/docs/balance/demo-1.7.txt` |
| 8 | Tooltips showed the design sheet's numbers | Every champion ability / passive / trigger text and every trait text in `data/text_en.json` now matches the data (86 lines of `text_en.json` changed, incl. the new monster abilities; e.g. Rot's poison said 120/180/280, it deals 309/462/707). | a script cross-checked every "a/b/c" in the texts against the champion's data |

## Running it
* `cd server && make test` (6,436 engine + 1,976 network checks), `make balance ARGS="--matches 300 --seed 1000"`, `make ladder ARGS="--fights 1000 --mode both"`.
* The Mac app carries its own server and data: rebuild it with `server/scripts/package_mac.sh` to play 1.7's numbers there (not done in this pass).

## Files
* Server repo: `server/include/w2f/Config.h` (sudden death, `pveLossDamageFromStage`), `server/src/CombatSimulator.cpp` (sudden death, AP scaling),
  `server/src/MatchManager.cpp` (PvE loss damage), `server/src/Config.cpp` (content hash), `server/data/champions.json`, `traits.json`, `pve.json`, `text_en.json`,
  `server/tests/tests.cpp`, `server/tests/data/designer_spec_traits.json` (structure only: Helios/Nature 10, Duelist reduction, Coregons drain at 8),
  `server/tests/golden/fights.golden`, `server/docs/sample_fights/*`, `server/tools/trait_ladder.cpp` (new), `server/tools/balance_sim.cpp` (final-board placements,
  traits on final boards, items, PvE report), `server/Makefile` (`ladder`), `server/docs/balance.md` + `docs/balance/demo-1.7.txt` + `ladder-1.7.txt`,
  `W2F_GameDesignDocument.md` (Helios / Nature 10, Coregons numbers), `CLAUDE.md`.
* UE: nothing.

## Honest limits / to test by hand
* All of this was measured with bots and duels, not people. Play a few matches and say what feels off; that beats any number here.
* PvE: bots still win 97-100% of monster rounds. Losing now hurts, so it was not pushed further. How hard should the X-7 bosses be?
* Nature can only be read from bot games (58% at (3), the free plants); the ladder does not field plants.
* One fight in 26,664 still reached the 120 s safety limit; the cause was not looked into (plants or untargetable Souls are the suspects).
* `make ladder` is a Makefile target only (not in CMake / CI).
* In the UE client, by hand (not looked at in this pass): the monsters' new abilities (their effect entries in `fx.json` predate them), the PvE loss damage,
  and sudden death (no new visuals: the usual damage numbers and the OVERTIME banner).
* Strict build: the changed engine and tool files compile with `-Werror -fno-exceptions -fno-rtti` and the project's warning set; Windows / Linux are CI only.
