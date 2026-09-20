# `champions.json`, `traits.json`, `items.json` and `pve.json` reference

The server reads its champions from `data/champions.json`, its synergies from `data/traits.json` and its items from
`data/items.json` on start-up (`LoadChampionDatabaseFromFile`, `LoadTraitDatabaseFromFile`, `LoadItemDatabaseFromFile`, then
`ValidateChampionTraits` / `ValidateItemTraits` to check that every trait a champion or an item lists is declared in `traits.json`).
If the file is wrong the server refuses to start and prints exactly where, e.g.

```
data/champions.json: champions[2].ability.effects[0].amount.terms[1].source (line 88, column 21):
  unknown value "SelfMaxHP"; expected one of: SelfMaxHp, SelfCurrentHp, ...
```

Unknown keys are errors too (a typo such as `"magicResit"` would otherwise silently become 0).
`//` and `/* */` comments are allowed. Numbers are read as exact decimals; nothing is ever a float.

## Conventions
* **Per star** values: one number (same at every star) or exactly three, `[1★, 2★, 3★]`.
* **Times** can be written in seconds (`"durationSeconds": 1.75`) or ticks (`"durationTicks": 52`). The server runs at
  30 ticks/s. Seconds convert exactly and round **half up**: 1.75 s = 52.5 → **53** ticks, 2.25 s → 68.
* **Mana** is on the design sheet's scale ("0/60", "0/100"): a basic attack gives +10, and every 10 points of (pre-mitigation) damage
  taken gives +1 (at most 20 per tick), so `maxMana: 100` means "about ten attacks". The scale lives in `CombatConfig`, not in the data.
* `attackSpeed` and `manaRegen` are decimals with at most 3 decimal places (`0.78` → 780 thousandths).
* Percentages are whole numbers (`20` = 20 %).

## Top level
```json
{ "version": 1, "champions": [ <champion>, ... ] }
```

## Champion
| key | required | notes |
|---|---|---|
| `id` | yes | unique, > 0 |
| `name`, `cost` | yes | cost 1–5 (also its shop tier) |
| `role` | no | `"Tank"` or `"Damage"` (bots put Tanks in the front row). Default `Damage` |
| `traits` | no | array of strings, e.g. `["Helios"]`. Each must be declared in `traits.json` (see below) |
| `stats` | yes | see below |
| `ability` | no | the active spell |
| `passive` | no | a start-of-combat ability (trigger defaults to `StartOfCombat`; `"Passive"` is an alias) |
| `triggers` | no | array of **hook** abilities: silent, always-on reactions (see "Event triggers"). Each needs its own `trigger` |
| `auras` | no | standing effects on the units around it (see "Auras") |
| `summon` | no | `true`: this champion can only be created by a `Summon` effect. Never sold, not in the pool; `cost` is optional; no `traits` allowed |
| `onAttack` | no | a rider on **every basic attack** (trigger defaults to `OnBasicAttack`): its effects apply as the attack is made — no spell cast, no mana use. May target `CurrentTarget` (the unit just hit). Soul's shield refill is one |

### `stats`
Required: `hp`, `armor`, `magicResist`, `attackDamage` (per star), `attackSpeed` (attacks/s), `range` (hexes, 1 = melee).
Optional: `abilityDamage` (per star; the sheet's "AP"), `abilityPowerPercent` (default 100), `crit` (per star, % chance),
`maxMana` (0 or absent = no mana bar), `startMana` (mana at the start of a fight; the sheet's "0/60" is start/max), `manaRegen` (mana/s), `attackSpreadSeconds` / `attackSpreadTicks`
(deal each basic attack's damage spread over this long).

## Ability / passive
`id`, `name`, `effects` are required. `trigger`: `"Mana"` (cast at full mana; needs `maxMana`), `"EveryNthAttack"` (needs
`attackCount`), `"StartOfCombat"`/`"Passive"` (passives only) or `"OnBasicAttack"` (`onAttack` riders only). Optional: `resetOnTargetChange` (every-Nth only: the attack
count restarts when the target changes or dies), `castLockSeconds`/`castLockTicks` (caster can't act that long after casting; 0 =
keeps attacking), `castOnDeath`, and:
* `channelSeconds`/`channelTicks` — a **channel**: the caster is locked that long, and the ability's *delayed* effects only happen if the
  channel is not broken. Being stunned or knocked up (and not immune) interrupts it — the client gets a `SpellInterrupted` event — and a
  caster that dies just dies. Effects placed at the channel's end are its "if it completes" finale. A channel is its own lock, so it
  cannot be combined with `castLock*`. Vega: eight 0.5 s pulses, then the board-wide finale.

A **passive** cannot have a cast lock/channel/castOnDeath, and can only target things that need no "current target": `Self`, `AreaAroundSelf`,
`ClosestEnemies`, `AlliesInStartLine`, `LowestHpAlly`, `HighestDamageAlly`, `RandomEnemy`.

## Effects
Every effect has `type`, `target`, and optionally `delaySeconds`/`delayTicks` (happens later, for sequential effects) and
`"repeat": { "count": N, "everySeconds": s | "everyTicks": n }` (the effect runs `count` times, the first at its delay, then at that spacing;
each run **re-resolves its targets** — Vega's eight pulses each pick a new random enemy).

**target**:
* `"Self"`, `"CurrentTarget"`, `"AlliesInStartLine"` (allies that began the fight in the team's busiest row; a tie goes to the row nearest the middle),
  `"LowestHpAlly"` (least current HP, the caster included), `"HighestDamageAlly"` (the ally — never the caster — that dealt the most damage recently — Astra's "highest DPS"; ties go to
  the higher attack damage, then the lower unit id; long form `{ "mode": "HighestDamageAlly", "windowSeconds": 5 }` sets the look-back window), `"RandomEnemy"` (uniform,
  from the fight's own seeded generator, so replays agree);
* `{ "mode": "AreaAroundTarget" | "AreaAroundSelf", "radius": N, "includeCenter": true|false, "side": "Enemies"|"Allies"|"All" }`
  ("adjacent hexes" = radius 1, `includeCenter` false; the whole board is radius 15);
* `{ "mode": "ClosestEnemies", "count": N }` (nearest first; ties go to the lowest unit id);
* `{ "mode": "LineBehindTarget", "length": N }` (the N hexes in a straight line behind the target, as seen from the caster);
* `{ "mode": "ConeTowardTarget", "length": N }` (a 120° cone opening from the caster toward its target: 3 hexes at length 1, 8 at length 2, 15 at length 3; the caster's own hex is not in it);
* `{ "mode": "HighestHpEnemyNearTarget", "radius": N }` (the enemy with the most current HP among those within N hexes of the cast target — a "targeted zone").

Enemies that are **Untargetable** are never picked by any of these, area effects included.

| type | keys |
|---|---|
| `Damage` | `damageType` (`Physical`/`Magic`/`True`), `amount`, `multiplierPercent` (default 100), `canCrit`, `onKill` (statuses the **caster** gains if this damage kills: `[ { "status": "AggroDrop", "durationSeconds": 1.5 } ]`, same keys as a `Status` effect) |
| `Shield` | `amount`, a duration **or** `"permanent": true`, `damageReductionPercent` (per star; also cuts damage taken while the shield holds), `cap` (a permanent shield can be refilled by later shield effects, never above this total) |
| `Teleport` | `destination` (`BehindFarthestEnemy` / `BehindClosestEnemy`); target must be `Self`. Lands on a free hex next to that enemy, on the side away from where it started; stays put if nothing is free |
| `Status` | `status`, `percent` (per star, signed; not for Stun/Root/Knockup/CcImmunity), a duration **or** `"permanent": true`, `multiplierPercent` (scales the duration), `"stacking": "add"\|"refresh"`, and `value` (BonusAttackDamage only) |
| `DoT` | `damageType`, `amount`, `amountIsTotal` (amount is the total over the duration, split evenly), a duration, `intervalSeconds`/`intervalTicks`, `stackBonusPercent` |
| `Heal` | `amount` (flat, or a formula such as 5% of `TargetMaxHp`). Reduced by the target's Wound; capped at max HP |

**status**: `Stun`, `AttackDamage`, `AttackSpeed`, `Armor`, `MagicResist`, `MaxHp` (percent of base max HP; current HP moves with it),
`Wound` (−healing %; the strongest one applies), `InflictsWound` (marker: this unit's DoTs also wound their victims by this % while they burn — Baira's passive),
`DamageAmp` (± % damage the unit DEALS; multiplies every hit before armor / resist), `Root` (cannot move, can still attack and cast),
`Knockup` (airborne: cannot move / attack / cast; the displacement itself is not simulated), `CcImmunity` (ignores Stun / Root / Knockup applied by
**other** units; its own self-applied effects still work), `DamageTakenRegen` (heals this % of every hit it takes, after armor and damage reduction),
`BonusAttackDamage` (+ flat attack damage; the size is the `value` formula, e.g. 15% of `SelfArmor`).
`Tether` (`percent` of every hit the holder takes, after armor / resist, goes to the status's **source** as true damage instead — the hit is reported as two `Damage`
events, the source's flagged `kFlagRedirected`; nothing is redirected once the source is dead), `Untargetable` (enemies cannot target the unit at all),
`AggroDrop` (enemies' *automatic* targeting skips the unit; area effects still hit it).
The **flat family** `BonusArmor`, `BonusMagicResist`, `BonusMaxHp`, `BonusAbilityDamage`, `BonusCritChance` work like `BonusAttackDamage`: the size is a `value` formula
(signed, so a negative value is a debuff), evaluated when applied. Items use them, and so does Soul's "steal 15 % of the target's attack damage and armor"
(a gain for the caster and an equal loss for the target, both permanent).

**stacking**: statuses of one type normally **add up** (two +10% Armor = +20%; the Protector synergy relies on this). `"stacking": "refresh"`
makes re-applying keep the larger magnitude and extend the end time instead — for "modes" that can be re-entered, like Les's wall.
Effects that grant `CcImmunity` always land first on a tick, so immunity beats same-tick crowd control whatever the unit ids.

**a duration** is one of `durationSeconds`, `durationTicks` (per star), or `"duration": <amount>` (a formula).

**amount**: a number/array (flat, per star) or an object `{ "flat": ..., "flatSeconds": ..., "terms": [ ... ] }` meaning
`flat + Σ (source × percent / 100)`. A term is `{ "source": ..., "percent": <per star>, "windowSeconds"|"windowTicks": n }` with source
`SelfMaxHp`, `SelfCurrentHp`, `SelfAttackDamage`, `SelfAbilityDamage`, `SelfArmor`, `SelfMagicResist`, `TargetMaxHp`, `TargetCurrentHp`,
`DamageDealtInWindow` (all damage the caster dealt in the last N; needs a window), `DamageDealtToTargetInWindow` (only to the cast target; needs a window),
`RawDamageDealtToTarget` (raw basic-attack damage to the cast target since the last cast), `CastTargetAttackDamage`, `CastTargetArmor` (the cast target's current stat, e.g. 15 % of it for Soul's steal).

## Worked example: Alesk's spell
```json
{ "type": "Shield", "target": "Self",
  "amount": { "terms": [ { "source": "SelfMaxHp", "percent": [7, 15, 30] } ] },
  "durationSeconds": [1.75, 2.0, 2.25],
  "damageReductionPercent": [5, 12, 25] }
```

## DoT timing
A DoT hits `max(1, duration / interval)` times, evenly spread so the **last hit lands exactly when the duration ends**
(a 53-tick burn with a 15-tick interval hits at +17, +35, +53). With `amountIsTotal`, remainders go to the later hits.

## `traits.json`
```json
{ "version": 1, "traits": [ { "id": 5, "name": "Protector", "breakpoints": [ { "count": 2, "effects": [ <trait effect>, ... ] } ] } ] }
```
A trait needs a unique `id` (it appears in the event stream) and a `name`; `breakpoints` is optional (a tag with no synergy yet).
At the start of a fight, for each team, the server counts the **different** champions (two copies of one champion count once) on the
board that have each trait, and applies the **highest** breakpoint whose `count` is reached (a higher breakpoint *replaces* lower ones).
This happens before passives fire.

A **trait effect** is an ordinary `Status`, `Shield` or `Heal` effect (no `target`; it applies to each qualifying unit itself) plus
`"scope"`: `"AllAllies"` (every unit on the team) or `"TraitHolders"` (only units that have this trait). Statuses add up, so an
`AllAllies` +10 % and a `TraitHolders` +10 % give holders +20 %.

```json
{ "scope": "AllAllies",    "type": "Status", "status": "DamageAmp", "percent": 10, "permanent": true },
{ "scope": "TraitHolders", "type": "Status", "status": "DamageAmp", "percent": 10, "permanent": true }
```
Every trait tag a champion lists must exist here (a typo like `"Protecter"` stops the server from starting).

## `items.json`
```json
{ "version": 1, "items": [
  { "id": 1, "name": "Example Sword",   "stats": { "attackDamage": 15 } },
  { "id": 3, "name": "Coregons Emblem", "grantsTraits": ["Coregons"] } ] }
```
An item needs a unique `id` (> 0) and a `name`, plus `stats` and/or `grantsTraits` (an item that does nothing is an error).
Stats are flat integers and all optional: `hp`, `armor`, `magicResist`, `attackDamage`, `abilityDamage` (adds to the champion's per-star "AP"),
`attackSpeedPercent` (+%), `critChance` (+ percentage points), `startMana` (+ mana at the start of the fight, whole-mana scale).
`grantsTraits` lists trait tags the holder gains; each must exist in `traits.json` (`ValidateItemTraits`).

* A unit carries up to **3** items (`kMaxItemsPerUnit`). Players hold spare items in an **item bag**; `TryEquipItem(player, unit, item)` moves one from the bag onto a unit
  and `TryUnequipItem(player, unit, slot)` moves it back (Planning phase only). Where items *come from* (carousel, drops, shop) is not built yet; `PlayerState::AddItemToBag` is the entry point.
* **Merging**: the survivor keeps its items and takes the consumed copies' items up to 3; whatever does not fit goes to the bag. **Selling** a unit puts its items in the bag. No item is ever lost (an *eliminated* player's units and their items leave the match with them).
* **In combat**, each unit's items become permanent statuses on tick 0 (`StatusApplied`, duration 0), exactly like passives, so a viewer and the log validator see them.
  Items apply first, then synergies, then passives. A trait granted by an item counts toward synergies exactly like a native one: the count is of *different champions*
  with the trait, so an emblem on a champion that already has the trait adds nothing, and two emblem-wearing copies of one champion count once.
* Item-related data is part of what a snapshot records a hash of (see `docs/snapshots.md`).

## `pve.json`
```json
{ "version": 1,
  "monsters":     [ <champion, cost optional>, ... ],
  "defaultDrops": [ { "type": "Gold", "weight": 40, "minGold": 2, "maxGold": 4 }, { "type": "Champion", "weight": 30, "tiers": [1, 2] }, { "type": "Item", "weight": 30 } ],
  "encounters":   [ { "id": 1, "name": "Gloop Nest", "stage": 1, "round": 1, "units": [ { "monster": 10001, "x": 2, "y": 3, "star": 1 } ], "drops": [ ... ] } ] }
```
See `docs/game-loop.md` for how PvE rounds work. The file has three parts:

* **monsters** are ordinary champion definitions (same `stats` / `ability` / `passive` keys), kept in their own list: never in the shop, never in the pool, no traits, and their `id`s must not clash with `champions.json` (the sample uses 10000+). `cost` is accepted but not needed.
* **encounters** are the boards. `units` stand on the same board a player has — `x` 0–6, `y` 0–3 with `y` = 3 the front row — and the fight mirrors them to the far side. `stage` / `round` choose when an encounter is used, **0 or absent = any**: (1, 2) is round 1-2, (0, 7) is X-7 of every stage. The most specific match wins (stage+round > stage only > round only > neither); if several tie, the match seed picks one, so every player in a round meets the same board. `drops` optionally replaces the default table for that encounter.
* **drop tables**: winning a PvE round gives **one** drop, chosen from the table by `weight`. `Gold` needs `minGold` and `maxGold` (uniform between); `Champion` needs `tiers` (cost tiers to draw from — the copy comes out of the shared pool, and a full roster gets gold equal to its cost instead); `Item` optionally lists `items` (item ids; empty = any item in `items.json`). Lines that cannot pay out right now — no item data loaded, every listed tier sold out — are skipped for that roll.

## Event triggers (hooks)
An ability's `trigger` says when it fires. `Mana`, `EveryNthAttack` (as a champion's `ability`) *cast*: a SpellCast event, a lock, mana drained. Every other trigger is a **hook**:
silent (no cast, no mana use, no lock) - its effects just happen, on the tick the event happens. Hooks live in a champion's `triggers`, an item's `abilities`, or a synergy's `triggers`.

| trigger | fires when the holder ... |
|---|---|
| `OnBasicAttack` | launches a basic attack (its `CurrentTarget` is the unit it hit) |
| `EveryNthAttack` | (as a hook) makes its Nth basic attack: `attackCount` = N |
| `OnCast` | casts its ability |
| `OnTakeBasicAttackDamage` | is hit by a basic attack (damage over time is not "being hit") |
| `OnTakeAbilityDamage` | is hit by an ability's damage (not by damage over time) |
| `OnDealDamage` | deals damage; `damageFilter` `"Basic"` / `"Ability"` / `"Any"` (default) picks which |
| `OnCritTaken` | is hit by a critical strike |
| `OnHpDropBelowPercent` | drops below `thresholdPercent` (1-99) of its max HP; re-arms when it climbs back above (unless `maxTriggers` is 1) |
| `OnAllyDealDamage` | has an ally (not itself) that deals damage; `damageFilter` as above |

Counted triggers: `attackCount` N = "every Nth occurrence" (absent / 1 = every one); `maxTriggers` caps the firings per fight (absent = no cap, 1 = once).

Inside a hook, effects can use the event:
* targets `"TriggerAttacker"` (who dealt the damage; for `OnDealDamage` / `OnBasicAttack`, the holder) and `"TriggerVictim"` (who was hit; for the take-hooks, the holder). `"CurrentTarget"` means the other party: whoever hurt the holder, or whoever the holder / its ally hurt;
* the formula source `"TriggerDamage"`: the damage (after armor / shields' reduction) of the hit that fired it - "heal for 10% of damage dealt" is `{"source": "TriggerDamage", "percent": 10}`.

**No chain reactions**: damage caused by a hook's own effects (flagged `kFlagTriggered` in the stream) fires no hooks (except that it may push a unit below an HP threshold), so a hook that deals damage on "dealing damage" fires exactly once per real hit.
Hooks resolve the same tick, right after that tick's damage has landed.

```json
{ "id": 12, "name": "Guardian's stacks", "trigger": "OnTakeBasicAttackDamage", "attackCount": 6, "effects": [
    { "type": "Status", "target": "Self", "status": "BonusArmor", "value": 6, "permanent": true } ] }
```

## `Summon` and `Mana` effects
`{ "type": "Summon", "target": "Self", "champion": 9101, "count": 3, "star": 0, "maxHp": <amount>, "attackDamage": <amount> }` spawns units for the caster's team **instantly, mid-fight**, on the free hexes nearest the caster.
`champion` must be a champion marked `"summon": true`. `count` is a formula (0-8; `[1, 2, 3]` scales with star), `star` 0 = the summoner's star. `maxHp` / `attackDamage`, when they evaluate to more than 0, **replace** the summon's own stat, and can read the new source
`HighestAllyMaxHp` (the largest max HP among the caster's living allies, summons excluded): "Souls have 25% of your tankiest ally's max HP" is `"maxHp": { "terms": [ { "source": "HighestAllyMaxHp", "percent": 25 } ] }`.

A summon: has no traits and no items, gets no synergy, **does not count as one of the team's units** (it cannot keep a fight going, it is never a "survivor", a team whose real units are all dead has lost even if its summons live), never counts toward a player's board (it never exists outside the fight),
and **vanishes when the fight ends**. Its own `passive` and `triggers` work from the moment it appears. The event stream shows a `Spawn` with flag `kFlagSummon` (`other` = the summoner) on any tick, and a flagged `Death` for each summon still standing at the end. At most 48 summons per fight.

`{ "type": "Mana", "target": "Self", "amount": 4 }` grants whole mana (negative drains; clamped to the bar; nothing for a unit without a mana bar).

## Auras
`"auras": [ { "side": "Enemies", "radius": 2, "status": "MagicResist", "amount": -30, "includeSelf": false } ]` on a champion or an item: while the holder lives, every unit of that `side` within `radius` hexes has the status (a percent for percent statuses, points for `Bonus...` statuses).
Re-evaluated every tick: units gain it entering the radius (a permanent `StatusApplied`, `other` = the holder) and lose it leaving it or when the holder dies (a `StatusEnded`). Several holders stack. Enemy auras skip untargetable units. `includeSelf` (Allies only) gives the holder its own aura. An aura cannot apply Stun, Knockup, Burn or Tether.

## More statuses and crits
`AbilityCrit` - the holder's *damage abilities* can critically strike (they roll its crit chance; a crit deals base + the crit bonus). Without it only effects marked `"canCrit": true` can.
`CritDamage` (+% added to the crit bonus, 21% by default), `CritDamageTakenReduction` (% of the crit bonus the holder does not take), `AbilityPower` (+/- % of its ability power), `BonusManaRegen` (flat family, in thousandths of a mana per second: `"value": 1000` = +1 mana/s),
`SpellShield` (the next enemy *ability* hit is blocked entirely and the status is used up). Item / champion stat `manaRegen` is mana per second with up to 3 decimals.

## Formula terms: `permille`
A term is `source x percent / 100`. Use `"permille"` instead of `"percent"` for thousandths: `{ "source": "TargetMaxHp", "permille": 5 }` is 0.5% of the target's max HP.

## Item recipes
An item with `"components": [idA, idB]` is what two **base** components turn into (either order; the same component twice is fine: Helmet + Helmet). Only base items combine - a recipe cannot use another recipe's result. A base item may have no effect at all (the Omnilium Seed) as long as some recipe uses it.
When a unit that holds one component is given the other, **both are consumed at once** and the finished item takes the first one's slot - even on a unit already carrying three items. The same happens to items carried over by a unit merge. Unequipping / selling returns the *finished* item (combinations are not undone).
Events: `OnItemEquipped` for the second component, then `OnItemsCombined(player, unit, {first, second, result})`; on the network a `unit_event` `items_combined`.
An item can also carry `abilities` (hooks or `StartOfCombat` passives - an item has no mana, so no `Mana` trigger) and `auras`; see `docs/item-coverage.md` for how the design doc's items map onto these.

## Synergies: `Team` scope and `triggers`
A breakpoint may have `effects`, `triggers`, or both. `"scope": "Team"` (Summon effects only) applies **once for the whole team**, cast by its first (lowest UnitId) trait holder: "summon 3 souls", not "3 souls per holder".
`"triggers": [ { "scope": "TraitHolders", "ability": { ...an ability with a hook trigger... } } ]` hands a hook to every unit in scope for the fight ("holders heal for 10% of damage dealt" is an `OnDealDamage` hook). Summons get no synergy effects.
