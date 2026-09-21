# Sample fights

Real `combat` messages, exactly as `w2f_server` sends them (one JSON object per file), plus `catalog.json`, the `catalog` message that says what every id in them means (names, costs, presentation timings, the
vocabularies of the log). Use them to build and test an offline viewer before any WebSocket exists: parse `columns` / `events`, spawn actors from the `Spawn` rows, and play the timeline as described in
[`../UE5-Integration.md`](../UE5-Integration.md), section 7 and 8. In every file the home team (team 0, seat 0) fights the away team (team 1, seat 1) in round 5. `checksum` lets you verify your parsing.
Regenerate with `make sample-fights` (after a change to `data/*.json` or the combat rules); they are validated against `docs/schemas/server-message.schema.json` by the test suite.

| file | what it shows | result | events | notes |
|---|---|---|---|---|
| `01-melee-brawl.json` | Two front lines of melee tanks and bruisers meet in the middle: walking, melee blows, shields, small circle and cone spells, deaths. | away wins with 5 left (13.3 s) | 607 | 7 projectile attacks, 23 spell casts, 0 typed-DoT ticks |
| `02-ranged-projectiles-and-dots.json` | Archers and casters behind a wall: projectiles of different speeds, Rot's poison, Lich's life drain, Baira's burn and Pyra's piercing arrow. | home wins with 4 left (25.0 s) | 902 | 116 projectile attacks, 33 spell casts, 87 typed-DoT ticks |
| `03-spell-areas.json` | Spell areas (line, circle, cone), burn and poison, and items and synergies (Assassin, Helios) in play. | home wins with 3 left (16.0 s) | 730 | 45 projectile attacks, 30 spell casts, 53 typed-DoT ticks |
| `04-overtime.json` | A fight that is still undecided after 30 s: the Overtime row, then everything at 4x speed (attacks, steps, mana) until one team is wiped out. | away wins with 3 left (46.1 s) | 2023 | 186 projectile attacks, 91 spell casts, 0 typed-DoT ticks, **overtime from tick 900** |

