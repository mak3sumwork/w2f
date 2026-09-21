# Content notes: what the 30-champion roster assumes

`data/champions.json` and `data/traits.json` follow `W2F_GameDesignDocument.md` (sections 2 and 3). Where the doc is silent or ambiguous the data
picks the most literal reading and marks it `ASSUMED` in a comment. These are the ones worth a designer's eye — change the data, not the code:

## Champions
* **Cast animation** — every spell that is not an "empower" or a buff has a 0.5 s cast lock (ASSUMED, as for the earlier champions). Buff-only casts (Solis, Kryx, Mortis) have no lock.
* **Null (Range 3)** — the design doc says range 1, but "pulls target 1 hex closer" needs a target that is not already adjacent, so the designer set Null's range to 3 in the data. The doc should be updated to match.
* **Solis / Mortis** — the empowered attack's damage is a **bonus hit on top of** the normal attack (the doc says "deals X damage" for Solis, "bonus" for Mortis; both are treated as bonus). Unused charges lapse after 8 s / 10 s (ASSUMED).
  Mortis's "ignore 50% Armor" applies to that bonus hit, not to the ordinary attack itself.
* **Items with a guessed number** — Mage Shield's shield lasts 5 s, Fishscale's shield 5 s, Blue Whale's "regens 30% over 2 s" is 3 pulses of 10% (all marked `ASSUMED` in `items.json`). Twin Snipers' 3% a second needs the target in attack range; Tear of Mother's "no damage" counts hits the shield absorbs.
* **Poison / drain (Rot, Lich)** — a damage-over-time with one tick every 0.5 s (ASSUMED). They are now typed (`"visual": "Poison"` / `"Drain"`) so a client can draw them differently from a burn.
* **Xul** — the bolt bounces to the nearest *other* enemy adjacent to the first target; it hits as hard as the bolt.
* **Grave's Skeleton** — armor / magic resist 0 and attack speed 0.70 are ASSUMED (the doc gives only HP and AD).
* **Lum** — the passive follows the doc (+15 / 30 / 100 % Armor, MR and AD). The active keeps the designer's earlier, more detailed spec (punch, knock-up of the units behind the target, splash): the doc only says "heavy Physical Damage" and gives no numbers.

## Synergies
* **Helios** — every basic attack of a holder RE-LIGHTS the burn (2 % / 5 % of the target's max HP over 3 s, one tick a second): it **refreshes instead of stacking**, so a target has at most one Helios burn at a time (across all Helios units; whoever lit it last gets the credit) and the burn keeps its once-a-second rhythm.
  While it is re-lit it deals a third of 2% (5%) per second, i.e. about 0.67% (1.67%) of max HP a second.
* **Phaisa** — +4 % / +10 % of the holder's *base* attack damage and ability power per death, permanent for the fight, counting every death of either team (summons included).
* **Hexagon** — the shield is a permanent, non-expiring shield. "When the shield breaks" is read as the **first shield to break, once per unit** (a later shield from Byte / Ignis does not detonate again).
* **Selini** — "invisible (drop aggro)" is implemented as `AggroDrop` (enemies' automatic targeting skips the unit; area effects still hit it), not `Untargetable`.
* **Coregons**
  * 6 Coregons champions exist (Bone, Rot, Grave, Soul, Lich, Mortis), so the **8** breakpoint needs two Coregons Emblems on other champions (emblems are now "+1 to the trait" only, as the doc says).
  * "Your tankiest ally's Max HP" is read when the fight starts (after items, before passives) from the team's non-summon units.
  * The Lost Soul's own stats (30 AD, 0.8 attack speed, range 1, no armor) are ASSUMED. Souls attack, echo, and cannot be targeted; each of the 3 Souls echoes every hit by another unit of its team — including the other Souls' basic attacks, but never its own echoes.
  * The zone's per-second damage / healing and the execute are cast **once for the team by its lowest-id Coregons unit** at tick 0 and last the whole fight even if that unit dies. Enemy units that appear later (summons) are not touched. Zone damage is true damage and is not counted as damage the caster "dealt" (so it does not skew Astra's "highest DPS" or Cyla's rocket).
  * Enemies with no mana bar (Baira) are not affected by "+15 mana to cast".
* **Omnilium** — the doc gives it no synergy of its own; it stays a plain tag (Protector is Les / Lum's).
* **Assassin (2/4)** — the Assassins are Vex, Lunis and Raa (the three champions the doc calls "Assassin"); the 4 breakpoint needs the Assassin Emblem. The synergy makes abilities able to crit, adds +20% / +50% crit damage **and +15% / +30% crit chance** (no champion has a base crit chance, so the chance comes from the synergy).
* **The opening (round 1)** — everyone is dealt one random **1-cost** unit and the shop is closed for round 1 (`openingUnitCosts`, `shopClosedOpeningRounds` in `GameConfig`; the designer said "a random unit", the cost is ASSUMED). The board holds `level` units (level 1 = 1 ... level 10 = 10).
* **Mother Nature** — three tiers by stage, as the designer decided: early game (stages 1-2), mid game (stages 3-4), late game (stage 5+). The unit gift scales with the stage: stage 1 a 1-cost unit, stage 2 a 1- or 2-cost, stages 3-4 a 3- or 4-cost, stage 5+ a 5-cost (`costsByStage` in `mother_nature.json`). All gift weights are ASSUMED. The shop is closed for the whole of a Mother Nature round, buying XP is still allowed, and a timeout auto-picks the first offer.
  The carousel (the old placeholder Draft phase) and augments are gone: the phase that used to be Draft is now MotherNature.

## Presentation numbers (Phase B): all ASSUMED, all only for animation
Attack windup 0.2 s, ranged projectile speed 12 hexes/s (champions with range >= 2), cast windup 0.3 s are the defaults (`CombatConfig`); a champion or ability overrides them in the data (`stats.attackWindup`, `stats.projectileSpeed`, ability `windup`;
Baira, Rot and Lich have examples). They never change the outcome of a fight, only the timings a viewer animates with, and they are not part of the data hash (retuning them does not invalidate a saved match).

## Balance pass (Phase A)
`data/champions.json` and `data/traits.json` were retuned with the bulk simulator: see `balance.md` for what changed and why. The designer's original numbers are frozen in `tests/data/designer_spec_champions.json` / `designer_spec_traits.json`
(the mechanics tests use them). Where the pass departs from the design document on purpose: **Baira** (HP, attack and AP raised: the sheet's 100/150/200 HP made her die in two hits), **tanks** (HP and armor cut to speed the fights), **Coregons 3** (2 souls instead of 3, weaker echo).
