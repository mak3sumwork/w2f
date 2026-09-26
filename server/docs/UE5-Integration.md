# W2F for Unreal Engine 5: the integration guide

This is the document a UE5 developer needs to build a client for the W2F game server. It says exactly what goes over the wire, how to read a combat log, and
how to turn it into animation with correct timing. Everything here is checked by the test suite: the JSON Schemas in `docs/schemas/` are validated against
every message the server produces (including a whole eight-client match over real sockets), and the golden fights in `tests/golden/` pin the combat
results on every platform.

* Sample data for an offline viewer: [`sample_fights/`](sample_fights/README.md) (real combat logs + the catalog).
* Placeholder 3D models for every champion, summon and monster, plus a board tile, and the Unreal import steps: [`blockouts/`](blockouts/README.md).
* Machine-readable contract: [`schemas/server-message.schema.json`](schemas/server-message.schema.json) (every server -> client message) and
  [`schemas/client-command.schema.json`](schemas/client-command.schema.json) (every client -> server command). JSON Schema 2020-12.
* The reference for the same protocol in prose, with the rules of each command: [`network-protocol.md`](network-protocol.md).
* Running and deploying the server (Docker, TLS, crash recovery): [`deploy.md`](deploy.md).
* A working (test-tool) client in ~1,000 lines of JavaScript: [`../../client/index.html`](../../client/index.html). It is the easiest way to see every message in action.

## 1. The model in one paragraph

The **server is the game**. It owns every rule, every random number and every fight. A client sends *commands* ("buy the champion in shop slot 2") and receives
*messages* (the state of the world, and whole finished fights as timestamped event lists). **The client decides nothing**: it draws what it is told and
plays fights back. Because the whole log of a fight is delivered at the moment its Combat phase starts, playback is deterministic, can be paused, sped up,
rewound and replayed for spectators, and can start animations *before* the moment they take effect (see section 7.2).

```
 UE5 client  --WebSocket, JSON text frames-->  w2f_server   (rules, shop, fights, bots)
             <-------------------------------
```

**Two ways to integrate.** (a) *Recommended:* the UE client connects to the standalone `w2f_server` over WebSocket, as described here. The same client
then works for a dedicated server, for a local game (launch the server as a child process) and for spectating. (b) Compile the engine (`src/`, `include/w2f/`)
as a UE module: it has no networking, no exceptions, no RTTI and no floats, and the build is tested with `-DW2F_UE_COMPAT=ON`. Choose (b) only if you must
run offline without a process; the message shapes below are then what `MatchManager`'s `IMatchListener` events map to.

## 2. Connecting

| | |
|---|---|
| URL | `ws://host:7777/` (any path; plain `ws`, put a TLS proxy in front for `wss`; see `deploy.md`) |
| Query parameters | `token=<32 hex chars>` to come back to your seat; `code=<join code>` if the server is private (`--join-code`) |
| Frames | text only, one JSON object per frame, UTF-8. Client frames must be masked (every WebSocket client library does this) |
| Sizes | a client message is at most 4,096 bytes; server messages can be large (a combat log is 20-100 KB): do not cap your receive buffer |
| Keep-alive | the server pings every 15 s (the library answers automatically); a silent connection is dropped after 60 s |
| Rate limit | a token bucket per connection: 30 burst, 15 messages/s; `get_state` costs 3, `get_fight` / `get_catalog` 5. Breaking it answers `error` `rate_limited` |
| Close codes | 1000 match over, 1001 server stopping, 1002/1003/1007/1009 malformed traffic, 1008 policy (too many errors, idle, too slow), 1013 match in progress (no valid token), 4001 replaced by a newer connection with your token. HTTP 403 at the handshake = wrong or missing join code |

In UE: use the **WebSockets** module (`FWebSocketsModule::Get().CreateWebSocket(Url, TEXT("")).ToSharedRef()`), bind `OnMessage` (an `FString` per frame),
and parse with the **Json** module (`FJsonSerializer::Deserialize`). Build the URL with `?token=...&code=...` for reconnects.

### 2.1 Seats, lobby, start

1. You connect; the server gives you the lowest free human seat and sends **`welcome`** (`player_id`, `token`, `seats`, `bots`, `connected`, `match_running`).
   **Store the token**: it is your only key back into the match.
2. It also sends **`lobby`** (who is connected). Ask for the game data with **`get_catalog`** (section 5); it works in the lobby.
3. When the last *human* seat fills, the match starts by itself: **`match_started`**, then the first `phase`, `state`, `public_state`.
   With `--bots 7` one human is enough: the last seats are AI players (`match_started.bot_seats` lists them).
4. Round after round: `phase` messages, your private `state` whenever something changed, `public_state` for the boards and health of everyone, then the fights.
5. The match ends with **`match_over`**; about 20 s later the server closes every connection (code 1000) and opens a fresh lobby.

### 2.2 Reconnecting

Connect again with `?token=...`. The server *replaces* any older connection with that token (it gets close code 4001) and sends, in this order: `welcome`
(`reconnected: true`), `match_started`, `phase` (with the true `ticks_remaining`), `state`, `public_state`, and during Combat/Resolution the `combat_summary`
plus your own `combat`. That is everything needed to redraw the game as it is. The server never waits for a disconnected player; their seat is kept.
If the server itself crashed and was restarted with `--resume`, the same tokens work (section 12).

## 3. Time

The simulation runs at **30 ticks per second** (`match_started.tick_rate`). All durations in the protocol are in ticks. A `phase` message carries the phase's real
length (`duration_ticks`) and what is left (`ticks_remaining`); count down locally at 30 ticks/s, do not poll. `server_tick` is the server's tick counter
(useful for lag estimates and logging).

| Phase | Length | Notes |
|---|---|---|
| `MotherNature` | up to 20 s | only every 3rd round: pick 1 of 2 free gifts; ends sooner once everyone picked. The shop is closed for the whole round |
| `Planning` | 30 s | the whole economy: buy, sell, reroll, level, move, equip |
| `Combat` | the fights' length + 2 s, at least 3 s, at most 125 s (`duration_ticks` says exactly) | your fight's log arrives at the start; tick 0 of the log = the start of this phase |
| `Resolution` | 3 s | damage, gold, streaks and eliminations are applied on entry; `player_damaged` / `player_eliminated` arrive |

Round 1 is the **opening**: no shop, everyone is dealt one free random unit (it is on the bench: put it on the board). The shop is closed in round 1 and in
Mother Nature's rounds, and open in every other round's phases (`phase.shop_closed`).

## 4. Commands (client -> server)

Each command is one JSON object with an `"action"` and, optionally, an `"id"` (integer, echoed in the `result`/`error` answering it). Anything not in the table,
any unknown or missing field and any out-of-range number is refused with an `error` message and never reaches the game. Full grammar: `client-command.schema.json`.

| action | fields | allowed when |
|---|---|---|
| `buy_unit` | `shop_index` 0..63 | Planning, **Combat and Resolution** (shop open). In Combat/Resolution the champion goes to the **bench** and may only merge with bench units |
| `reroll_shop`, `buy_xp` | | Planning, Combat, Resolution (`reroll_shop` needs the shop open) |
| `pick_gift` | `gift_index` 0..3 | Mother Nature's phase only |
| `sell_unit` | `unit_id` | Planning: any unit. Combat/Resolution: bench units only |
| `move_unit` | `unit_id`, `location` `"bench"`/`"board"`, `x`, `y` | Planning: anywhere (swaps with what is there). Combat/Resolution: bench <-> bench only |
| `combine_items` | `first`, `second` | (revision 2) two base components in the item bag become the finished item; answered with `result`, a `bag_event`, and the new `state` |
| `equip_item` | `unit_id`, `item_id` | Planning; in Combat/Resolution only onto bench units. An **Item Remover** takes *all* items off the unit and is used up |
| `unequip_item` | `unit_id`, `slot` 0..2 | as `equip_item` |
| `pick_trait_choice` | `index` -1..7 | (revision 5) Planning, Combat, Resolution: answer `state.trait_choice` (a Hexagon module: pick an option; a Najmi cash-out: pick one; revision 6: it stays open -- no need to answer --, the star dust keeps growing). Open module choices are settled automatically when the next Combat starts |
| `get_state` | | any time: re-sends `state` and `public_state` |
| `get_fight` | `fight_index` 0..7 | any time: the combat log of any of this round's fights (they are public) |
| `get_catalog` | | any time, also in the lobby |
| `ping` | | any time: answered with `pong` |

**Every command that reaches the engine is answered with a `result`**: `{"type":"result","id":17,"action":"buy_unit","result":"Ok","ok":true}`. The `result` is one of
`Ok`, `WrongPhase`, `InvalidPlayer`, `PlayerEliminated`, `NotEnoughGold`, `InvalidSlot`, `EmptySlot`, `RosterFull`, `BoardFull`, `MaxLevel`, `InvalidUnit`,
`ItemsFull`, `InvalidItem`, `ShopClosed`, `AlreadyPicked`, `NoItemsToRemove`, `UnitInCombat`. A refused command changes nothing. Show the reason: `BoardFull` means the board
holds as many units as the player's level (level 1 = 1 unit ... level 10 = 10); `UnitInCombat` means the board is locked while units fight.

After an accepted command the server pushes the fresh private `state` (only if it changed) and, for changes to units and items, a `unit_event` (section 6.4).

## 5. The catalog: what the ids mean

Every id in the game (`champion`, `item_id`, `ability`, `trait_id`) is a number. **`get_catalog`** returns one **`catalog`** message with everything a client needs to
show names and to animate; it is the same for every player and does not change during a match. Ask once after `welcome` and keep it. Build your UE DataAssets /
DataTables from it (or from the same JSON files in `data/`).

```
catalog
 |- enums        the vocabularies of the combat log (section 7.4): status_types, damage_types, attack_styles, dot_visuals, area_shapes, damage_flags
 |- champions[]  id, name, cost 1..5, role tank|damage, summon, monster (a PvE monster), traits[], hp[3], attack_damage[3] (per star),
 |               attack_speed_milli, range (hexes), max_mana, ability / passive (names), and the PRESENTATION block:
 |                 presentation.attack   { style "melee"|"projectile", windup_ticks, projectile_speed_milli }
 |                 presentation.ability  null | { id, windup_ticks, cast_lock_ticks, channel_ticks, area { shape, size }, dot_visuals[] }
 |- items[]      id, name, components[] (two ids for a finished item), stats{}, traits[] granted, has_effect, consumable (the Item Remover)
 |- traits[]     id, name, breakpoints[] (how many DIFFERENT champions activate each tier)
 `- text         { language, entries } the DISPLAY TEXT: a flat map key -> string (descriptions, titles, gift wording), see below
```

**Display text and localisation.** `text.entries` is the content of `data/text_en.json`. Keys use the ids of the data, so they survive renames:

| key | meaning |
|---|---|
| `champion.<id>.name` / `.title` / `.desc` | every champion, summon and PvE monster has a name; a title ("Purple Sniper") and a summon's description are optional |
| `champion.<id>.ability.name` / `.ability.desc` | the active ability (names equal the catalog's `ability`) |
| `champion.<id>.passive.name` / `.passive.desc` | the passive |
| `trait.<id>.name` / `.tagline` / `.bp<count>` | a trait, its flavour line and one description per breakpoint (`bp3`, `bp6` ...) |
| `item.<id>.name` / `.desc` | every item |
| `gift.<id>.name` / `.desc` | every Mother Nature gift (the `gift` ids in `gifts_offered`) |

In a text, `a/b/c` means the value at 1 / 2 / 3 stars. Import the map as a UE **String Table** (namespace `W2F`, key = the key, source string = the text) and show it with
`FText::FromStringTable`; translating later means adding a `text_<lang>.json` with the same keys. The tests guarantee that every ability, passive, breakpoint, item and gift
has an entry and that names equal the data's names. The numbers inside descriptions are written by hand (a balance change to a champion means editing its sentence).

Champions with `monster: true` exist only in PvE fights; `summon: true` ones (Lost Souls, Skeletons) exist only inside fights. Neither appears in a shop or a roster.
All timings are **effective**: the champion's own numbers, else the server's defaults. Timings are hints for animation lead times, they never change the outcome of a fight.

## 6. Messages (server -> client)

Every message has a `"type"`. The complete, exact list of fields is in `server-message.schema.json`; this section says what each is for. **Ignore fields you do not
know**: within protocol 1 the server only ever adds fields (section 13).

### 6.1 Sent to one player only (private)
`welcome`, `result`, `error`, `pong`, `catalog`, and:

* **`state`**: your whole private state, sent whenever it changes: `alive`, `health`, `gold`, `level`, `xp`, `xp_to_next`, `streak`, `shop` (five champion ids, 0 = empty), `bench`
  (9 entries, a unit or `null`), `board` (units), `item_bag` (item ids, repeats allowed), `gifts` (Mother Nature's open offers), `gift_settled`.
  A **unit** is `{id, champion, star, location "bench"|"board", x, y, items[]}`. Bench: `x` 0..8, `y` 0. Board: `x` 0..6, `y` 0..3, **`y` = 3 is the front row** (nearest the enemy).
  Unit ids are unique for the whole match.
* **`income`** at the start of each round: base, interest and streak gold, passive XP.
* **`gift_event`** (`offered` / `picked`), **`pve_drop`** (gold, a champion or an item for beating the monsters).
* **`unit_event`**: something happened to one of *your* units: `bought`, `sold`, `moved`, `merged` (three copies became one: `unit` is the survivor, `consumed` the two that vanished),
  `item_equipped`, `item_unequipped`, `items_combined` (two components became a finished item), `consumable_used` (an Item Remover emptied a unit: `returned` lists the items now in the bag).
  These are for effects and sounds; the authoritative state is the next `state`.

### 6.2 Public (everyone may see it)
* **`match_started`** (`tick_rate`, `phase_ticks`, board dimensions, `mother_nature_every`, `bot_seats`, the names of the combat event types), **`lobby`**.
* **`phase`**: `phase`, `round`, `stage`, `round_in_stage`, `pve`, `mother_nature`, `shop_closed`, `duration_ticks`, `ticks_remaining`, `server_tick`.
* **`public_state`**: everyone's `health`, `level`, `streak`, `alive`, `placement` and **board** (bench and gold stay private). Draw opponents' boards from this.
* **`combat_summary`** at the start of Combat: the round's fights (`home`, `away` seat or `null`, `away_is_ghost`, `away_is_monsters`, event count).
* **`combat`**: one fight's whole log (section 7). You get yours automatically; ask for others with `get_fight`.
* **`player_damaged`**, **`player_eliminated`**, **`match_over`** (`winner`, `placements` by seat).

### 6.3 Privacy
Gold, shop, bench, item bag, income, drops and gifts are private to their owner: the server sends them only to that connection. Boards, health, level, phase, fights and results are public.

### 6.4 A round, message by message
`phase(Planning)` -> `income`, `state` -> your commands, each answered by `result` (+ `unit_event`s, a new `state`) -> `phase(Combat)` preceded by `combat_summary` and your
`combat` -> replay the fight -> `phase(Resolution)` -> `player_damaged`s, `state`, `public_state` -> `phase(Planning)` of the next round. On Mother Nature's rounds the round opens with
`phase(MotherNature)` and a `gift_event(offered)`.

## 7. Combat logs: how to play a fight

A `combat` message is a *complete* fight. Keys: `round`, `fight_index`, `home`, `away` (seats, `null` for monsters), `away_is_ghost`, `away_is_monsters`, `winner` (`home`/`away`/`draw`),
`winner_survivors`, `end_tick`, `survivors` `[home, away]`, `checksum`, `columns` and `events`.

`events` is a list of **rows**; each row is an array of 27 integers in the order of `columns`:

```
tick type team unit other from_x from_y to_x to_y amount hp_after champion star absorbed subtype flags ability duration mana_max mana_regen reduced trait_id windup flight kind shape size
```

Unused columns are 0. Rows are sorted by `tick`; **within a tick they are in the order they must be played**. Ticks are *combat-local*: tick 0 is the first tick of the Combat phase.
A short real excerpt (a fight of 5 v 5), reading the first columns of each row:

```
[0,0,0,16777217,0,0,0,1,3,525,525,9001,2,...]      Spawn   unit 16777217 (champion 9001 "Alesk", 2-star, team 0) appears at arena hex (1,3) with 525 HP
[0,1,0,16777217,0,1,3,2,3,15,...]                  Move    the same unit steps from hex (1,3) to (2,3); the step takes 15 ticks (amount)
[0,2,0,16777218,33554434,2,1,4,4,...,8,7,1,0,0]    Attack  16777218 (a ranged unit) hits 33554434: kind 1 (projectile), windup 8, flight 7 ticks, impact at tick 0
[48,5,0,16777217,33554435,2,3,2,3,...,1,15,...,9,0,0,0,0]  SpellCast  ability 1, locks the caster 15 ticks after tick 48, animation starts 9 ticks earlier
```

### 7.1 Identities, teams and the arena
* **Unit ids**: players' units are `(seat + 1) << 24 | serial`; monsters are above `0xF0000000`; summons above `0xE0000000`. `team` 0 is the **home** player, 1 the away side.
* **Arena**: 7 columns x 8 rows of *pointy-top hexes in "odd-r" offset coordinates*: odd rows are shifted half a hex to the right. `x` = column, `y` = row (0 at the top).
  The home board occupies rows 0-3 and the away board rows 4-7, mirrored: an away player's board cell `(x, y)` is arena hex `(6 - x, 7 - y)`. So the two front rows meet in the middle.
  To show *your* team at the bottom whichever side you are on, rotate the view: if you are home, draw arena row `7 - y`; if away, draw row `y` (keep the odd-r shift by the row's parity so hex
  geometry stays right). World position of hex `(x, y)`: `X = size * sqrt(3) * (x + 0.5 * (y & 1))`, `Y = size * 1.5 * y`.
* **Distance** between two hexes: convert to axial `q = x - (y - (y & 1)) / 2`, `r = y` and use `(|dq| + |dr| + |dq + dr|) / 2`.

### 7.2 The presentation contract: when things *land*, and animating ahead of time
**A row's tick is the moment its effect happens.** Because you hold the entire log, you can *start* the animation earlier so that it lands on the tick:

```
 Attack row at tick T, with windup W and flight F:
     T-F-W ............ T-F ............ T
     |swing starts      |projectile     |IMPACT: the Damage row(s) on the same tick T
                         leaves            (F = 0 for a melee blow: the swing simply ends at T)
```
* **Attack**: `unit` attacks `other`. `kind` 0 = melee / instant, 1 = projectile. `windup` = ticks *before* the impact that the swing animation should start (the release of a
  projectile is at `T - flight`). `flight` = ticks the projectile travels; fly it from the attacker's hex (`from_x, from_y`) to the target's (`to_x, to_y`, where the target stood when the
  attack was made). Damage arrives at `T`, in the `Damage` row(s) of the same tick.
* **SpellCast**: `unit` casts `ability` at `other` (0 if it has no target). (For a **"largest cluster"** spell, one with an `AreaAroundDensestEnemy` target, `to` is that cluster's centre when the cast starts, an enemy's hex, not the target's.) The **effects land at tick T**. Start the cast animation at `T - windup`; the caster is *locked* (cannot act) for
  `duration` ticks *after* `T` (the recovery part of the animation). `shape` / `size` describe the area (section 7.5) centred on `(to_x, to_y)`; `from` is the caster's hex.
* Where a start time would fall before tick 0, or overlap the unit's previous action, shorten the animation. Timings are hints: use your own animation lengths and *time-scale* clips to
  fit `windup` + `flight` if you like: only the impact tick is exact.
* **Overtime** speeds `windup` and `flight` up with everything else (section 7.6).

### 7.3 Event types
`type` indexes `match_started.combat_event_types` (`Spawn, Move, Attack, Damage, Death, SpellCast, ShieldApplied, ShieldEnded, StatusApplied, StatusEnded, ManaChanged, Heal, TraitActivated,
Teleport, SpellInterrupted, Overtime`). Columns each type uses (the rest are 0):

| type | columns and meaning |
|---|---|
| **0 Spawn** | `unit`, `team`, `champion`, `star`, `to` = arena hex, `amount` = max HP, `mana_max` and `mana_regen` in thousandths (0 = no mana bar), `flags` bit 128 = a summon (`other` = its summoner; summons appear at any tick) |
| **1 Move** | `unit`, `from` -> `to` (adjacent hexes), `amount` = ticks the step takes: tween the unit over that time |
| **2 Attack** | see 7.2. `unit` -> `other`, `from`, `to`, `kind`, `windup`, `flight` |
| **3 Damage** | `unit` = victim, `other` = attacker, `amount` = damage after armor / resist (before shields), `absorbed` = the part shields soaked (**HP lost = amount - absorbed**), `hp_after`, `subtype` = damage type (0 physical, 1 magic, 2 true), `flags` (below), `kind` = the DoT visual for a typed damage-over-time tick |
| **4 Death** | `unit` |
| **5 SpellCast** | see 7.2. `ability`, `duration`, `windup`, `shape`, `size`, `from`, `to` |
| **6 ShieldApplied** | `unit` (holder), `other` (caster), `amount` = capacity, `duration` in ticks (0 = permanent) |
| **7 ShieldEnded** | `unit`, `amount` = capacity left unused (0 = broken) |
| **8 StatusApplied** | `unit` (holder), `other` (source), `subtype` = status (index into `catalog.enums.status_types`), `duration` (0 = permanent), `amount` = magnitude %, `hp_after` |
| **9 StatusEnded** | `unit`, `subtype`, `hp_after` |
| **10 ManaChanged** | `unit`, `amount` = its mana now (thousandths). Sent on discrete changes; between events show the bar rising at `mana_regen` per second (not while casting) |
| **11 Heal** | `unit` healed, `other` healer, `amount` = HP restored, `reduced` = healing removed by Wound, `hp_after` |
| **12 TraitActivated** | tick 0 only: `team`, `trait_id`, `amount` = how many different champions have it, `subtype` = which breakpoint (1 = lowest) is active |
| **13 Teleport** | `unit`, `from` -> `to`: it is at `to` from this tick on. `subtype` 0 = a blink / dash / leap of its own, 1 = displaced by someone else (a pull or a knock-back: slide it). **`subtype` 2 = an assist blink (Sept. 2026, Sola's "Blade Brothers"): presentation only: the unit blinks to `to` (next to `other`, the enemy it strikes), hits, and is back on `from` at once; its position does NOT change** |
| **14 SpellInterrupted** | `unit`, `ability`: its channel was broken |
| **15 Overtime** | no unit. See 7.6 |

`flags` on a Damage row (`catalog.enums.damage_flags`): 1 crit, 2 damage-over-time tick, 4 dealt by an ability, 8 basic attack, 16 a caster's last cast as it died, 32 redirected (a tethered share),
64 caused by an item / synergy reaction, 128 summon.

**Typed damage over time.** Poison, Bleed, Burn and Drain differ only in how they look: a `StatusApplied` row with `subtype` = Burn / Poison / Bleed / Drain starts the effect on the victim, every tick of it is
a `Damage` row with `flags & 2` and `kind` = that status's index, and a `StatusEnded` row with the same `subtype` ends it. Use `kind` to pick the damage number colour and the particle system.

### 7.4 Status types
`subtype` on status rows indexes `catalog.enums.status_types` (Stun, Burn, AttackDamage, AttackSpeed, Armor, MagicResist, MaxHp, Wound, ..., Poison, Bleed, Drain). Crowd control worth an icon: `Stun`, `Root`,
`Knockup` (airborne), `Blind`, `Untargetable` (fade the unit), `AggroDrop`. Permanent statuses (duration 0) applied on tick 0 are passives, items and synergies: usually no effect is needed.
Trait system v2 (revision 5): `Piloting` = the unit climbed into Hexa (`other` = Hexa's unit id): **hide it** (or draw it in the cockpit) until its `StatusEnded`, when Hexa
has died and it ejects onto its own hex; it neither acts nor takes damage meanwhile. `ManaCost` (a Helios Rally: the bar is shorter; re-read `ManaChanged`), `Omnivamp`, `HealingAmp`
are plain buffs; `Awakened` = a plant came to life (the Nature trees start walking). Two summons are special: **Baron Nashor** (9042, Phaisa 9) and the **Invention** (9044,
Hexagon: untargetable, never moves; its modules fire 8 s in — draw a machine on the team's right-most back hex). A Superior Lifeform clone is a summon Spawn whose `champion`
is a real champion (draw it tinted). Plants (9110-9112) are ordinary units of the board (`catalog.champions[].plant`), stationary until `Awakened`.

### 7.5 Spell areas
`SpellCast.shape` indexes `catalog.enums.area_shapes`; `size` is in hexes:

| shape | draw |
|---|---|
| 0 None | only the caster (a self buff or shield) |
| 1 Single | one target unit |
| 2 Circle | `size` hexes around `(to_x, to_y)` (the target's hex) |
| 3 CircleSelf | `size` hexes around the caster |
| 4 Line | a straight line of `size` hexes *behind* the target, seen from the caster (`from` -> `to`, continuing past `to`) |
| 5 Cone | a 120-degree cone `size` hexes long from the caster toward the target |
| 6 Row | the caster's team's busiest row |
| 7 All | the whole enemy (or allied) team |

The area is for VFX; who actually gets hit shows up as the `Damage` / `Status*` / `Heal` rows.

### 7.6 Overtime, and how a fight ends
A fight runs 30 s at normal speed. If both teams still stand at tick 900 the server sends an **`Overtime`** row (`amount` = the speed factor, 4): **from that tick on every unit attacks, moves and gains mana
4x faster** (Move `amount`, the gaps between Attacks, `windup` and `flight` are all divided by 4; cast animations and damage-over-time keep their length). **Overtime never ends by itself: the fight
continues until one team is wiped out. There are no draws by time.** (Only two teams dying on the very same tick is a draw. As a safety net against a fight that can never end, the server decides it after 120 s:
more surviving units, then more total HP, then a coin from the fight's seed; `end_tick` says when.) Show an "OVERTIME" banner at the `Overtime` row.

### 7.7 Verifying a log
`checksum` is a 64-bit FNV-1a over the whole log, as 16 hex digits (JSON cannot carry 64 bits). Every 64-bit word below is fed as 8 little-endian bytes into `h = (h ^ byte) * 0x100000001b3` starting from
`0xcbf29ce484222325`; signed integers are sign-extended to 64 bits. Feed: the number of events; then for each row, in order, its 27 values in column order (`tick`, `type`, `team`, `unit`, `other`, `from_x`,
`from_y`, `to_x`, `to_y`, `amount`, `hp_after`, `champion`, `star`, `absorbed`, `subtype`, `flags`, `ability`, `duration`, `mana_max`, `mana_regen`, `reduced`, `trait_id`, `windup`, `flight`, `kind`, `shape`, `size`);
then `end_tick`, `survivors[0]`, `survivors[1]`. Compare in a debug build to catch a parsing mistake.

## 8. A playback design that works

1. On `combat` for your fight, build a **timeline**: for each row compute `t_visual` = its tick, minus `windup + flight` for an Attack, minus `windup` for a SpellCast. Sort by `t_visual` (stable).
2. When the `phase(Combat)` message arrives, start a clock at 0 (30 ticks/s x your speed setting). Each frame, run every timeline item whose `t_visual <= clock` in order.
3. Keep a table `unit id -> actor` created by `Spawn` rows; *state changes* (HP, shields, position, mana) are applied on the row's own tick, animations start on `t_visual`.
4. Moves are tweens of `amount` ticks; a Teleport is instant; a Death plays its animation then removes the actor; summons spawn mid-fight.
5. The fight is over at `end_tick`; keep the arena up until the `phase(Resolution)` message, then show damage from `player_damaged`.
6. A spectator (or a reconnecting player) can `get_fight` and start the same timeline at any clock value: it is a pure function of the log.

Sketch of the row -> struct step in UE C++ (adapt to your naming):

```cpp
struct FW2FEvent { int32 Tick, Type, Team; uint32 Unit, Other; int32 FromX, FromY, ToX, ToY, Amount, HpAfter; uint32 Champion; int32 Star, Absorbed, Subtype, Flags;
                   uint32 Ability; int32 Duration, ManaMax, ManaRegen, Reduced; uint32 TraitId; int32 Windup, Flight, Kind, Shape, Size; };

// Message text -> events. Column order comes from the message ("columns"), so a newer server that appends columns does not break you.
void ParseCombat(const TSharedPtr<FJsonObject>& Msg, TArray<FW2FEvent>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>& Rows = Msg->GetArrayField(TEXT("events"));
    for (const TSharedPtr<FJsonValue>& RowValue : Rows)
    {
        const TArray<TSharedPtr<FJsonValue>>& R = RowValue->AsArray();
        FW2FEvent E{};
        E.Tick = (int32)R[0]->AsNumber();  E.Type = (int32)R[1]->AsNumber();  E.Team = (int32)R[2]->AsNumber();
        E.Unit = (uint32)R[3]->AsNumber(); E.Other = (uint32)R[4]->AsNumber();   // ...and so on, columns 5..26
        Out.Add(E);
    }
}
```
JSON numbers arrive as doubles in UE: unit ids fit exactly (they are below 2^32), and so does everything else except the checksum, which is a string.

## 9. Suggested UE architecture
* `UW2FConnection` (GameInstance subsystem): the socket, reconnect with the stored token and join code, command sending with ids, a delegate per message type.
* `UW2FCatalog`: the catalog (or your DataAssets); lookups by id.
* `UW2FMatchState`: the last `state`, `public_state`, `phase`; a countdown clock; widgets bind to it. The server is the source of truth: never predict, just show the next state.
* `AW2FBoard`: hex grid from the odd-r formula; bench slots; drag and drop sends `move_unit` / `sell_unit` / `equip_item` and waits for the `result`.
* `AW2FBattleDirector`: owns the timeline of section 8 and spawns / drives champion actors; one instance per fight being watched.
* Mother Nature, shop and gift UIs are plain widgets over `state` (`shop`, `gifts`).

## 10. Testing your client without a server round trip
* `w2f_server --bots 7 --fast` gives a match with 4-second planning phases against 7 bots: a full game in about 15 minutes.
* **`docs/sample_fights/`** has four real `combat` messages (a melee brawl; ranged projectiles with poison, drain and burn; spell areas; a fight that goes into overtime) and `catalog.json`. Build and test the offline 3D viewer against them
  before any WebSocket exists; each file's `checksum` lets you verify your parsing (section 7.7 was checked against these files). Regenerate with `make sample-fights`.
* `tests/golden/fights.golden` lists the checksums of twelve fixed fights; the same log always has the same checksum, on every platform.

## 11. What is *not* in the log
The log has no camera, no sound, no model or animation names and no localisation: those belong to your client, keyed by champion / ability ids from the catalog. Ability *text* (descriptions) is not in the
data yet; names are. Fights are instant on the server: the log is produced in one go at the start of Combat, so there is nothing to stream.

## 12. Operating notes for the client developer
* **Private servers**: `w2f_server --join-code CODE` requires `?code=CODE` on every connection (HTTP 403 otherwise). Use it over `wss` only.
* **Crash recovery**: `--autosave FILE` writes the restart point at the start of every Planning phase; after a crash `--resume FILE` restores the match (bots included) and the players reconnect with
  their tokens. A finished match deletes the file. Your client should simply retry connecting with the stored token for a minute (a restart takes a second).
* **Several matches**: one server process hosts one lobby / match; run several on different ports (`scripts/run_matches.sh`) behind a proxy that routes by path (`deploy.md`).

## 13. Versioning and compatibility
* `welcome.protocol` is the **major** version (now **1**) and `welcome.protocol_revision` counts additive revisions within it. **Protocol 1 is frozen**: existing messages, fields, enums and
  numeric meanings do not change and are not removed. New fields, new message types, new appended enum values (event types, status types, area shapes, action results) may appear: a client must
  **ignore unknown fields and unknown message types**, and treat an unknown enum index as "no special effect".
* A breaking change would bump `protocol` to 2 and be announced in this file. The schema files are versioned with the protocol and updated in the same commit as any addition.
* **Revision history** (additive only): **1** the frozen protocol; **5** (trait system v2, Sept. 2026) command `pick_trait_choice`; messages `trait_choice` and `trait_rewards`;
  `state.traits` + `state.trait_choice`; `public_state.trait_paths` (the path the match picked for each trait with paths: Selini); catalog champions carry `plant`, `special`, `price`,
  `team_slots`, `stationary`, `pilot`, catalog traits `paths`, `modules`, `mutations`; status types `ManaCost`, `HealingAmp`, `Omnivamp`, `Awakened`, `Piloting` appended (snapshot format 6); **4** (Phase 22) the catalog champions also carry `armor[3]`, `magic_resist[3]`, `ability_damage[3]`, `start_mana`, `mana_regen_milli`; **3** (Phase 21) `set_shop_lock` + `state.shop_locked` (the lock rule; snapshot format 5); **2** (Phase 20) `combine_items` command + `bag_event` message, `public_state.players[].bench` (benches are public, so a client can show a
  scouted player's arena), `combat.unit_items` (unit id -> item ids of the fighters).
* The combat log's columns are named in every message: read them from `columns`, never by hard-coded position, and tolerate extra columns at the end.

## Revision 6 (demo 1.1, FEEDBACK V1)
* `trait_choice.choice.bonus_items` (a Najmi cash-out's extra items), `tier` up to 6 (hundreds of star dust); cash-outs are not settled automatically.
* `state.traits.unlocked` and the message `champion_unlocked` (Hexa unlocks like TFT's T-Hex).
* Guaranteed PvE loot: several `pve_drop` messages per PvE round.
* Rule changes a client should know: the shop is open in every Planning phase (only Mother Nature's gift phase closes it); `equip_item` works on board units during
  Combat / Resolution (the item counts from the next fight).
