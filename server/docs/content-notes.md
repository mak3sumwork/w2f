# Content notes: what the 30-champion roster assumes

`data/champions.json` and `data/traits.json` follow `W2F_GameDesignDocument.md` (sections 2 and 3). Where the doc is silent or ambiguous the data
picks the most literal reading and marks it `ASSUMED` in a comment. These are the ones worth a designer's eye — change the data, not the code:

## Champions
* **Cast animation** — every spell that is not an "empower" or a buff has a 0.5 s cast lock (ASSUMED, as for the earlier champions). Buff-only casts (Solis, Kryx, Mortis) have no lock.
* **Null (Range 3)** — the design doc says range 1, but "pulls target 1 hex closer" needs a target that is not already adjacent, so the designer set Null's range to 3 in the data. The doc should be updated to match.
* **Solis / Mortis** — the empowered attack's damage is a **bonus hit on top of** the normal attack (the doc says "deals X damage" for Solis, "bonus" for Mortis; both are treated as bonus). Unused charges lapse after 8 s / 10 s (ASSUMED).
  Mortis's "ignore 50% Armor" applies to that bonus hit, not to the ordinary attack itself.
* **Poison / drain (Rot, Lich)** — a damage-over-time with one tick every 0.5 s (ASSUMED); the client shows every DoT as the generic burn status (`Burn`) for now.
* **Xul** — the bolt bounces to the nearest *other* enemy adjacent to the first target; it hits as hard as the bolt.
* **Grave's Skeleton** — armor / magic resist 0 and attack speed 0.70 are ASSUMED (the doc gives only HP and AD).
* **Lum** — the passive follows the doc (+15 / 30 / 100 % Armor, MR and AD). The active keeps the designer's earlier, more detailed spec (punch, knock-up of the units behind the target, splash): the doc only says "heavy Physical Damage" and gives no numbers.

## Synergies
* **Helios** — every basic attack of a holder starts its *own* burn (2 % / 5 % of the target's max HP over 3 s, one tick a second), and burns stack. With fast attackers that is a lot of true damage; if it is too strong, give the burn one shared "key" (refresh instead of stack) or lower the numbers.
* **Phaisa** — +4 % / +10 % of the holder's *base* attack damage and ability power per death, permanent for the fight, counting every death of either team (summons included).
* **Hexagon** — the shield is a permanent, non-expiring shield. "When the shield breaks" is read as the **first shield to break, once per unit** (a later shield from Byte / Ignis does not detonate again).
* **Selini** — "invisible (drop aggro)" is implemented as `AggroDrop` (enemies' automatic targeting skips the unit; area effects still hit it), not `Untargetable`.
* **Coregons**
  * 6 Coregons champions exist (Bone, Rot, Grave, Soul, Lich, Mortis), so the **8** breakpoint needs two Coregons Emblems on other champions. `data/items.json`'s placeholder *Coregons Emblem* still carries +300 HP / +25 AD from the old doc; the new doc makes an emblem "+1 to the trait" only — say when to change it.
  * "Your tankiest ally's Max HP" is read when the fight starts (after items, before passives) from the team's non-summon units.
  * The Lost Soul's own stats (30 AD, 0.8 attack speed, range 1, no armor) are ASSUMED. Souls attack, echo, and cannot be targeted; each of the 3 Souls echoes every hit by another unit of its team — including the other Souls' basic attacks, but never its own echoes.
  * The zone's per-second damage / healing and the execute are cast **once for the team by its lowest-id Coregons unit** at tick 0 and last the whole fight even if that unit dies. Enemy units that appear later (summons) are not touched. Zone damage is true damage and is not counted as damage the caster "dealt" (so it does not skew Astra's "highest DPS" or Cyla's rocket).
  * Enemies with no mana bar (Baira) are not affected by "+15 mana to cast".
* **Omnilium** — the doc gives it no synergy of its own; it stays a plain tag (Protector is Les / Lum's).
* **Assassin Emblem** — the doc lists it, but there is no Assassin trait; nothing to grant yet.
