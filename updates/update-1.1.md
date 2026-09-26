# W2F DEMO 1.1 (2026-09-26)

The answer to FEEDBACK V1 (`feedback/FEEDBACK_V1.md`). Every point of the feedback, what changed, and how it was checked.
Server tests: 6,359 engine + 1,932 network checks green, also with `-Werror -fno-exceptions -fno-rtti` (sanitizers on). "Screenshot" = seen in the running game (live match
against 7 bots) by the developer; things that need a real mouse or ears are marked **to test by hand**.

## Rules (server, `server/`)
| # | Feedback | Change | Checked |
|---|---|---|---|
| 4 | Shop open in Planning | The shop is open in **every** Planning phase (Combat and Resolution too), round 1 and Mother Nature's rounds included; only her gift phase closes it. | tests; screenshot (1-1 with 5 cards) |
| 6 | Najmi | Najmi **2/4/6**. Star dust as before (+20 + 5 x loss streak, +2 per Najmi takedown), (4) also +4 after every player combat, (6) doubles it. **Cash-outs only at 100, 200, 300, 400, 500, 600** star dust, loot growing with the bank (100: a component + 2 gold ... 600+: a choice of 3 completed items + 2 more + 20 gold). The offer never pops up, never blocks the shop and is never settled automatically: a star dust bar (marks at every 100) and a **CASH OUT READY** button open it; "Keep saving" leaves it open. | tests (bank 95 -> 120 = tier 1, 590 -> tier 6, taking it pays 3 items + 20 gold); screenshot (the bar) |
| 7 | Defeats hurt more later | Player damage by stage **0 / 2 / 5 / 8 / 11 / 14 / 18** (was 0 / 2 / 3 / 5 / 8 / 12), +1 per surviving enemy unit. Matches: 33.8 rounds on average (150-match balance run). | tests; balance run |
| 9 | Items in combat | Items can be equipped on **board** units during Combat / Resolution (the fight was decided when combat began, so the item counts from the next fight). The client lets you drop an orb on a fighting unit. | tests; **to test by hand** (mouse) |
| 11 | Prismatic only with emblems | The top breakpoint of a trait with 4+ breakpoints needs at least one holder that has the trait from an emblem (e.g. 9 natural Phaisa stop at (6); 8 + an emblem reach (9)). | tests |
| 12, 14, 15 | PvE items | PvE rounds now always drop loot, win or lose: **1-1 three Item Removers, 1-2 and 1-3 three components each, every X-7 boss two items** (plus the usual drop for a win). | tests; screenshot (removers + components on the item platform) |
| 20 | Hexa unlock | Hexa is **unlockable like TFT's T-Hex**: not in anyone's shop, drop or gift until your board's Hexagon units add up to **7 star levels at level 8**; then it can appear in your own shop. A banner announces the unlock. | tests (150 rerolls at 100% 5-cost odds: never before, yes after) |
| - | (found while testing) | A board crowded with Nature plants (they take a hex but no slot) could accept a unit with nowhere to stand and crash the server; fixed and covered by a test. | tests |

Protocol **revision 6** (additive): `trait_choice.bonus_items`, `state.traits.unlocked`, message `champion_unlocked`; snapshot format **v7**. Schemas updated (`docs/schemas/`).

## The client (Unreal project)
| # | Feedback | Change | Checked |
|---|---|---|---|
| 1 | Champion stats | The unit panel: health and mana bars with numbers, and a TFT-style stat grid with icons (attack damage, ability power, armor, magic resist, attack speed, range). | screenshot |
| 2 | Graphics not smooth | Anti-aliasing was **off** (`r.AntiAliasingMethod=0`); now TSR (temporal super resolution). Edges are clean. | screenshot |
| 3 | Level-up effect + stars on the bar | A star-up effect (ground rings, burst, rising sparks, a column of light; silver for 2 stars, gold for 3) and a sound when 3 copies merge; the health bar shows the rank as chevrons at its left end (1 bronze, 2 silver, 3 gold) and the frame takes the rank's colour. | bar: screenshot; effect: **to see in play** |
| 5 | Traits vanish when units die | The trait tracker keeps the fight's traits until the fight ends. | screenshot (combat) |
| 8 | Items bottom left | The front-left brazier corner is now a flat stone platform with 10 gold item slots (2 x 5, nearest the bench first); the items float over them as orbs. | screenshot |
| 10 | Trait hover shows champions | Hovering a trait shows its description, breakpoints and the portraits of every champion that carries it (the ones on your board lit). | **to test by hand** (tooltips do not show in screenshots) |
| 13 | Sound effects | 24 original sound effects (synthesised, `server/tools/make_sfx.py`): buy, sell, reroll, buy XP, lock, move, item pick-up / equip / forge, star-up (2 and 3), loot, unlock, round start, combat start, victory, defeat, hits, crits, casts, heals, shields, deaths. UI sounds play when the server confirms the action. | imported; **to hear by hand** |
| 16 | Names far from units | Names sit right on the unit's plate; bench units get a compact tag (rank chevrons + name). | screenshot |
| 17 | No bars in Planning | Board units show their health and starting mana in Planning too. | code; bench tags in screenshot |
| 18 | Item icons | All 55 item icons redrawn TFT-style (`server/tools/make_item_icons.py`): one big painted object per item (bevelled metal, cut gems, wood), glowing background in the item's colour, steel frame for components, gold for completed items, crests for emblems, a magnet for the Item Remover. | screenshot |
| 19 | Hotkeys | **D** rerolls, **F** buys XP, **E** sells the unit under the cursor (R still rerolls). | **to test by hand** |
| 21 | Damage stats | A TFT-style damage chart during combat: Damage Dealt (physical / magic / true), Damage Taken (lost / absorbed by shields), Healing & Shielding; your units or the enemy's; portraits with stars; updates live; closable. | screenshot |
| - | Extra | Cash-out loot preview (bonus items + gold) in the Najmi window; toasts for loot, unlocks and star-ups. | |

## Files
- Server: `src/MatchManager.cpp`, `MatchTraits.cpp`, `Trait.cpp`, `ShopManager.cpp`, `SharedChampionPool.cpp`, `UnitRoster.cpp`, `Snapshot.cpp`, `Pve.cpp`, `ChampionLoader.cpp`, `AIBotController.cpp`;
  `net/src/Messages.cpp`, `GameServer.cpp`; `data/champions.json` (Hexa `unlock`), `traits.json` (Najmi), `pve.json` (`guaranteedDrops`), `text_en.json`; tests; docs (GDD 2B/2C, design lock,
  champions-json, network-protocol, UE5-Integration, CLAUDE.md).
- Tools: `server/tools/make_sfx.py`, `make_item_icons.py`, `make_ui_icons.py` (rank, star and stat icons), `blender/arena_classic.py` (item platform), `unreal/import_blockouts.py` (sounds, splash art).
- Client: `W2FArena.*` (sounds, star-up, damage chart data, item slots, hotkeys, combat equips, Najmi bar data, unit stats), `W2FHud.cpp` (plates, stat grid, trait tooltip, star dust bar,
  choice window, damage chart), `W2FFx.*` (star-up effect), `Config/DefaultEngine.ini` (anti-aliasing).
