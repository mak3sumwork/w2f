# Item coverage: the design doc (v2.2) against the engine

Every item in `W2F_GameDesignDocument.md` section 4, and whether the Phase 10 primitives can carry it. "In the test file" means it is written out in
`tests/data/phase10_items.json` (an expressiveness proof - **not** the game data; the designer owns `data/items.json`) and loads, validates and fights.

> **The design doc exists in two contradictory versions.** `W2F_GameDesignDocument.md` was **v2.2** (item system in a table, 8 emblems, Sayona's Casket as an MR-reducing aura) when this
> matrix was written, and was replaced by **v2.1** (informal item text, 1 emblem with stats, Sayona's Casket as "starts combat with 20% mana") minutes later. The engine supports both readings;
> the data must follow ONE. Differences that matter: Casket (aura vs 20% starting mana - the latter needs a "% of max mana" formula source, not built), Coregons Emblem (+300 HP / +25 AD in v2.1 and in today's
> `data/items.json`, no stats in v2.2), emblem list (1 vs 8), Betrayed Heart (v2.1: damage goes "directly to HP", ignoring shields; v2.2: plain true damage - shields would still absorb it here).
> This matrix follows v2.2, the version the Phase 10 request was written against.

Legend: **full** = expressed as the doc says. **near** = expressed, with a small documented difference or a guessed number. **gap** = the engine cannot do it yet.

## Components and emblems
| item | status | notes |
|---|---|---|
| 8 base components | full | flat stats; `manaRegen` is mana per second (the doc's "+1 Mana Regen" is read as 1 mana/s on the 100-mana scale) |
| Omnilium Seed | full | an item with no effect, legal because recipes use it |
| Emblems x7 (Coregons, Helios, Najmi, Omnilium, Phaisa, Hexagon, Protector) | full | `components: [seed, X]` + `grantsTraits`; they count toward synergies |
| **Assassin Emblem** | **blocked by data** | there is no `Assassin` trait in `traits.json` (or in the doc's trait list): the engine refuses an item that grants an undeclared trait, on purpose |

## Legendary items (28 in the doc)
| item | status | how / what is missing |
|---|---|---|
| Sayona's Casket | full | stats + aura: enemies within 2 hexes, MR -30% |
| Soldiers' Soul | full | `EveryNthAttack` hook, N=3, +7 armor / +7 MR stacking |
| Guardians Armor | full | `OnTakeBasicAttackDamage`, N=6 |
| Gylachster | near | `EveryNthAttack` N=25, once, CC immunity - **the doc gives no duration** (10 s assumed) |
| Mage Shield | **gap (half)** | the shield after 4 abilities taken works (`OnTakeAbilityDamage`); "detonates, dealing stored damage + 150% AP to the attacker" needs a shield-break hook with stored damage - not built |
| Mother's Hands | full | stats + passive `MaxHp` +10% |
| Big Helmet | full | passive `CritDamageTakenReduction` 50 |
| Head Shot | full | stats + passive `DamageAmp` 10 |
| Tear Of Mother | **gap (half)** | 2nd cast -> shield of 200% AP works (`OnCast`, N=2, once); "if the user takes no damage, +15% AP for 3 s" needs a "no damage taken during the shield" condition - not built |
| Unalive Sword | full | `OnBasicAttack` + `Mana` effect (+4) |
| Fishtank | full | `OnCast`, +7 armor |
| Water Gun | full | `OnDealDamage`: target MR -30% for 4 s (refreshing, not stacking) |
| Divine Magic | full | `OnCast`: +1 mana regen, +3% AP |
| Blue Whale | full | passive +10% max HP; `OnHpDropBelowPercent` 30, once: heal 30% over 2 s (3 pulses) and +1 mana regen |
| Fishscale | near | +25% crit, +3 regen, shield of 10% max HP per cast (duration not given, 5 s assumed); **"+25% bite(?)" is unclear in the doc** and not modelled |
| Soul's Sword | full | passive `AttackDamage` +20% |
| Deadbeat | full | passive `SpellShield` (blocks the first enemy ability hit) |
| Gunfire | full | `OnBasicAttack` true damage of 0.5% of the target's max HP (`"permille": 5`) |
| Electroblade | full | `OnDealDamage` (basic only): heal 10% of the damage dealt |
| HeartBroke | full | `OnHpDropBelowPercent` 30, once: +10% AD and a permanent shield of 25% max HP |
| Full Kit | full | passive `CritDamage` +30 |
| Twin Snipers | **gap (half)** | Wound on attacks works; "a % of the target's max HP as true damage every second" - **the doc gives no percent**, and a per-second damage over the whole fight from a hook needs a repeating effect that targets the current target (passives have none) |
| Phaisa's Magic | full | `OnBasicAttack`, +1% AS, `maxTriggers: 60` |
| Betrayed Heart | full | `OnDealDamage`: 5% of the damage re-dealt as true damage (once - no chain reactions) |
| Guardian Destroyer | near | expressed as "on attack, the target's armor -30% for 4 s"; the doc says "ignore 30% of armor" (same effect on the numbers) |
| Magic Stick | full | passive `AbilityPower` +30% |
| Omnilium's Book | full | `OnDealDamage` (abilities only): Wound 30% for 3 s |
| Resist Puncher | full | passive `AbilityCrit` + 25% crit chance |

**Totals: 28 legendaries -> 22 full, 3 near (Gylachster, Fishscale, Guardian Destroyer), 3 half-done (Mage Shield, Tear Of Mother, Twin Snipers).** The two engine gaps are a shield-break detonation and a "took no damage" condition; everything else in the doc is data.

## What else the designer should know
* **The doc's numbers were used as written**; where it is silent or ambiguous (durations of Gylachster's immunity and Fishscale's shield, Fishscale's "bite", Twin Snipers' %) the file has a comment saying what was guessed.
* **Emblems need their trait to exist.** Only `Assassin` is missing today.
* **Item slots**: a unit carries 3 items. A combination frees a slot (two components -> one item), so a unit can end up with up to 3 finished items.
* **Auras and balance**: several Sayona's Casket holders stack (-30% each). If that is not wanted, say so: it is one flag away.
* **A finished item never combines again** (only base components do). If the design ever wants "upgrade" recipes, that is a small extension.
