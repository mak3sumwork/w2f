# Sample fights

Real `combat` messages, exactly as `w2f_server` sends them (one JSON object per file), plus `catalog.json`, the `catalog` message that says what every id in them means (names, costs, presentation timings, the
vocabularies of the log). Use them to build and test an offline viewer before any WebSocket exists: parse `columns` / `events`, spawn actors from the `Spawn` rows, and play the timeline as described in
[`../UE5-Integration.md`](../UE5-Integration.md), section 7 and 8. In every file the home team (team 0, seat 0) fights the away team (team 1, seat 1) in round 5. `checksum` lets you verify your parsing.
Regenerate with `make sample-fights` (after a change to `data/*.json` or the combat rules); they are validated against `docs/schemas/server-message.schema.json` by the test suite.

| file | what it shows | result | events | notes |
|---|---|---|---|---|
| `01-melee-brawl.json` | Two front lines of melee tanks and bruisers meet in the middle: walking, melee blows, shields, small circle and cone spells, deaths. | away wins with 6 left (14.1 s) | 762 | 8 projectile attacks, 27 spell casts, 0 typed-DoT ticks |
| `02-ranged-projectiles-and-dots.json` | Archers and casters behind a wall: projectiles of different speeds, Rot's poison, Lich's life drain, Baira's crashing tide and Pyra's piercing arrow. | home wins with 5 left (14.2 s) | 600 | 78 projectile attacks, 22 spell casts, 15 typed-DoT ticks |
| `03-spell-areas.json` | Spell areas (line, circle, cone), burn and poison, and items and synergies (Assassin, Helios) in play. | home wins with 4 left (15.3 s) | 710 | 39 projectile attacks, 35 spell casts, 6 typed-DoT ticks |
| `05-everyone-else.json` | The champions the other files leave out, so a viewer can check every basic attack and every ability: Cyla's rocket, Faire's stun, Astra's tether, Xul's bouncing bolt, Grave's skeleton, Byte's shield, Flare's sunbeam and Raa's leap against Soul, Myna, Null, Bone, Bit, Nyx, Mortis and Umbra. | away wins with 6 left (17.7 s) | 1188 | 96 projectile attacks, 35 spell casts, 0 typed-DoT ticks |
| `06-september-2026.json` | The September 2026 champions: Tide, Sunna, Kael, Aphel, Sola with Lunis at her side (Blade Brothers: Teleport subtype 2), Morrah, Aureon and Nihila (the densest-cluster areas, the crit bounce, the pull toward a rift, the execute). | home wins with 3 left (10.6 s) | 887 | 39 projectile attacks, 30 spell casts, 15 typed-DoT ticks |
| `07-trait-system-v2.json` | Trait system v2: Nature's plants (stationary trees, the Blossom's blessing, the Protector; Stonebark's death stun), Hexa with a pilot (the Piloting status, the eject), the Hexagon Invention (a summon on the back corner firing Electrical Overload and Magnetron Coil 8 s in), Rivet's armour shred and a Phaisa mutation. | home wins with 6 left (44.0 s) | 2153 | 269 projectile attacks, 77 spell casts, 5 typed-DoT ticks, **overtime from tick 900** |
| `04-overtime.json` | A fight that is still undecided after 30 s: the Overtime row, then everything at 4x speed (attacks, steps, mana) until one team is wiped out. | away wins with 3 left (55.1 s) | 3433 | 245 projectile attacks, 129 spell casts, 0 typed-DoT ticks, **overtime from tick 900** |

