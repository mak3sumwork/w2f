# Item coverage: the design doc against `data/items.json`

`data/items.json` is the real game data: **8 base components + the Omnilium Seed, 28 legendary items and 8 emblems** (45 entries), written from
`W2F_GameDesignDocument.md` section 4 and tested one by one. (It used to be a proof-of-concept file under `tests/data/`; that file is gone.)

Legend: **full** = exactly as the doc says. **near** = as the doc says with one small, documented guess (marked `ASSUMED` in the file).

## Components, Seed and emblems
| item | status | notes |
|---|---|---|
| 8 base components | full | flat stats; `manaRegen` is mana per second on the 100-mana scale |
| Omnilium Seed | full | no effect of its own; legal because recipes use it |
| 8 emblems (Coregons, Helios, Najmi, Omnilium, Phaisa, Hexagon, **Assassin**, Protector) | full | `components: [seed, X]` + `grantsTraits`; **+1 to the trait and nothing else** (the old placeholder's +300 HP / +25 AD is gone); they count toward synergies |

## Legendary items (28)
| item | status | how |
|---|---|---|
| Sayona's Casket | full | stats + aura: enemies within 2 hexes, MR -30% |
| Soldiers' Soul | full | `EveryNthAttack` hook, N=3, +7 armor / +7 MR stacking |
| Guardians Armor | full | `OnTakeBasicAttackDamage`, N=6 |
| Gylachster | full | `EveryNthAttack` N=25, once: **permanent** CC immunity ("for the rest of combat") |
| Mage Shield | full | after 4 abilities hit the holder: a 10% max-HP shield (5 s: ASSUMED). An `OnShieldBreak` hook limited to *that* shield (`onlyShieldsFrom`) detonates it when broken: magic damage to whoever broke it = the damage the shield stored (`TriggerDamage`) + 150% of the holder's AP. A shield that merely expires does not detonate |
| Mother's Hands | full | stats + passive `MaxHp` +10% |
| Big Helmet | full | passive `CritDamageTakenReduction` 50 |
| Head Shot | full | stats + passive `DamageAmp` 10 |
| Tear Of Mother | full | 2nd cast: shield of 200% AP for 5 s; a delayed `AbilityPower` +15% for 3 s that only happens if the `NoDamageTakenSinceCast` condition holds (no damage taken, absorbed or not, during the 5 s) |
| Unalive Sword | full | `OnBasicAttack` + `Mana` effect (+4) |
| Fishtank | full | `OnCast`, +7 armor |
| Water Gun | full | `OnDealDamage`: target MR -30% for 4 s (refreshing, not stacking) |
| Divine Magic | full | `OnCast`: +1 mana regen, +3% AP |
| Blue Whale | near | passive +10% max HP; `OnHpDropBelowPercent` 30, once: heal 30% over 2 s (3 pulses: the 2 s is ASSUMED to be 3 pulses) and +1 mana regen |
| Fishscale | near | +25% crit, +3 regen; **25% Omnivamp** (`OnDealDamage` heals 25% of the damage dealt); shield of 10% max HP per cast (5 s: ASSUMED) |
| Soul's Sword | full | passive `AttackDamage` +20% |
| Deadbeat | full | passive `SpellShield` (blocks the first enemy ability hit) |
| Gunfire | full | `OnBasicAttack` true damage of 0.5% of the target's max HP (`"permille": 5`) |
| Electroblade | full | `OnDealDamage` (basic only): heal 10% of the damage dealt |
| HeartBroke | full | `OnHpDropBelowPercent` 30, once: +10% AD and a permanent shield of 25% max HP |
| Full Kit | full | passive `CritDamage` +30 |
| Twin Snipers | full | Wound 30% on attacks + an `EveryInterval` hook (1 s): the holder's current target — a living enemy within its attack range — takes **3% of its max HP as true damage** every second. Two of them stack |
| Phaisa's Magic | full | `OnBasicAttack`, +1% AS, `maxTriggers: 60` |
| Betrayed Heart | full | `OnDealDamage`: 5% of the damage re-dealt as true damage (shields still absorb it) |
| Guardian Destroyer | near | expressed as "on attack, the target's armor -30% for 4 s"; the doc says "ignore 30% of armor for 4 s" (same numbers) |
| Magic Stick | full | passive `AbilityPower` +30% |
| Omnilium's Book | full | `OnDealDamage` (abilities only): Wound 30% for 3 s |
| Resist Puncher | full | passive `AbilityCrit` + 25% crit chance |

**Totals: 28 legendaries — 25 full, 3 near (Blue Whale, Fishscale, Guardian Destroyer). No engine gaps remain.**

## The primitives that closed the gaps
* **`OnShieldBreak` with stored damage**: every shield remembers the ability that made it and the damage it has absorbed; when damage uses it up the hook fires with `TriggerAttacker` = the unit that broke it and `TriggerDamage` = the stored damage. `"onlyShieldsFrom": <ability id>` restricts a hook to one ability's shields.
* **`"condition": "NoDamageTakenSinceCast"`** on any effect that has a delay: when the delayed effect comes due, it only runs if the caster has taken no damage (any hit that landed, even a fully absorbed one) since the cast that scheduled it began.
* **`"trigger": "EveryInterval"`** with `intervalSeconds` / `intervalTicks`: a repeating hook, on tick N, 2N ... for a holder that is alive, not stunned, and has a living enemy target within attack range; `CurrentTarget` is that target.

## Things the designer should know
* **Item slots**: a unit carries 3 items. A combination frees a slot (two components -> one item).
* **Auras and balance**: several Sayona's Casket holders stack (-30% each); Twin Snipers stack too (3% a second each). A finished item never combines again.
* **PvE "any item" drops** now draw from all 45 entries, the Seed and the components included (list `items` in `pve.json` to narrow them).
* **The old doc contradictions are resolved**: emblems are +1 trait only; Sayona's Casket is the MR aura.
