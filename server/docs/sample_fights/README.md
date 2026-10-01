# Sample fights

Real `combat` messages, exactly as `w2f_server` sends them (one JSON object per file), plus `catalog.json`, the `catalog` message that says what every id in them means (names, costs, presentation timings, the
vocabularies of the log). Use them to build and test an offline viewer before any WebSocket exists: parse `columns` / `events`, spawn actors from the `Spawn` rows, and play the timeline as described in
[`../UE5-Integration.md`](../UE5-Integration.md), section 7 and 8. In every file the home team (team 0, seat 0) fights the away team (team 1, seat 1) in round 5. `checksum` lets you verify your parsing.
Regenerate with `make sample-fights` (after a change to `data/*.json` or the combat rules); they are validated against `docs/schemas/server-message.schema.json` by the test suite.

| file | what it shows | result | events | notes |
|---|---|---|---|---|
| `01-melee-brawl.json` | Two front lines of melee tanks and bruisers meet in the middle: walking, melee blows, shields, small circle and cone spells, deaths. | away wins with 5 left (17.5 s) | 883 | 10 projectile attacks, 35 spell casts, 0 typed-DoT ticks |
| `02-ranged-projectiles-and-dots.json` | Archers and casters behind a wall: projectiles of different speeds, Rot's poison, Lich's life drain, Baira's crashing tide and Pyra's piercing arrow. | home wins with 4 left (18.1 s) | 741 | 90 projectile attacks, 31 spell casts, 32 typed-DoT ticks |
| `03-spell-areas.json` | Spell areas (line, circle, cone), burn and poison, and items and synergies (Assassin, Helios) in play. | away wins with 3 left (14.7 s) | 717 | 34 projectile attacks, 37 spell casts, 6 typed-DoT ticks |
| `05-everyone-else.json` | The champions the other files leave out, so a viewer can check every basic attack and every ability: Cyla's rocket, Faire's stun, Astra's tether, Xul's bouncing bolt, Grave's skeleton, Byte's shield, Flare's sunbeam and Raa's leap against Soul, Myna, Null, Bone, Bit, Nyx, Mortis and Umbra. | away wins with 5 left (21.0 s) | 1542 | 105 projectile attacks, 42 spell casts, 0 typed-DoT ticks |
| `06-september-2026.json` | The September 2026 champions: Tide, Sunna, Kael, Aphel, Sola with Lunis at her side (Blade Brothers: Teleport subtype 2), Morrah, Aureon and Nihila (the densest-cluster areas, the crit bounce, the pull toward a rift, the execute). | away wins with 3 left (13.7 s) | 915 | 40 projectile attacks, 31 spell casts, 28 typed-DoT ticks |
| `07-trait-system-v2.json` | Trait system v2: Nature's plants (stationary trees, the Blossom's blessing, the Protector; Stonebark's death stun), Hexa with a pilot (the Piloting status, the eject), the Hexagon Invention (a summon on the back corner firing Electrical Overload and Magnetron Coil 8 s in), Rivet's armour shred and a Phaisa mutation. | home wins with 6 left (37.6 s) | 1848 | 220 projectile attacks, 69 spell casts, 7 typed-DoT ticks, **overtime from tick 900** |
| `04-overtime.json` | A fight that is still undecided after 30 s: the Overtime row, then everything at 4x speed (attacks, steps, mana) until one team is wiped out. | away wins with 2 left (56.6 s) | 2575 | 162 projectile attacks, 119 spell casts, 19 typed-DoT ticks, **overtime from tick 900** |

