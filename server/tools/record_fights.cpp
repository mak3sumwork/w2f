// Records the sample combat logs in docs/sample_fights/: real `combat` messages, byte for byte what the server sends, plus the `catalog` message a viewer needs to show names.
// For UE5 developers: a 3D viewer can be built and tested against these files before it is connected to a WebSocket.
//
//   w2f_record_fights OUTPUT_DIR [--data DIR]        (make sample-fights)
//
// The fights are fixed (fixed boards, fixed seeds), so the same data gives the same files on every platform; re-record after a change to data/*.json or to the combat rules.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "w2f/ChampionLoader.h"
#include "w2f/CombatSimulator.h"
#include "w2f/Rng.h"
#include "w2f/net/Messages.h"

#ifndef W2F_DATA_DIR
#define W2F_DATA_DIR "data"
#endif

using namespace w2f;

namespace {

struct Placed {
    ChampionId champion;
    int star;
    int x, y;                        // board cell (y = 3 is the front row)
    std::vector<ItemId> items = {};
};

struct Scenario {
    const char* file;
    const char* title;
    std::vector<Placed> home, away;
    std::uint64_t seed;
    FightSetup setup = {};   // trait system v2: modules, levels, paths
};

FightResult Play(const std::vector<Placed>& home, const std::vector<Placed>& away, const ChampionDatabase& champions, const ItemDatabase& items, const TraitDatabase& traits,
                 std::uint64_t seed, int maxTicks, const FightSetup& setup = FightSetup{}) {
    std::vector<FightUnitSpec> specs;
    for (int team = 0; team < 2; ++team) {
        UnitId next = static_cast<UnitId>((team + 1) << 24);
        for (const Placed& p : team == 0 ? home : away) {
            FightUnitSpec spec;
            spec.id = ++next;
            spec.champion = champions.Find(p.champion);
            spec.starLevel = p.star;
            spec.team = team;
            spec.position = BoardToArena(p.x, p.y, team == 0 ? ArenaSide::Home : ArenaSide::Away);
            for (ItemId id : p.items) {
                if (const ItemDefinition* def = items.Find(id)) spec.items.push_back(def);
            }
            specs.push_back(spec);
        }
    }
    GameConfig config;
    return CombatSimulator(config.combat, &traits, &items, &champions).RunFight(specs, maxTicks, seed, setup);
}

// Exactly what the server would send for this fight: home = seat 0, away = seat 1, round 5, fight 0.
std::string CombatMessage(const FightResult& r) {
    CombatOutcome outcome;
    outcome.matchup.home = 0;
    outcome.matchup.away = 1;
    outcome.winner = r.winner;
    outcome.winnerSurvivors = r.winner == CombatWinner::Home ? r.log.survivors[0] : r.winner == CombatWinner::Away ? r.log.survivors[1] : 0;
    outcome.log = r.log;
    return net::msg::Combat(5, 0, outcome);
}

bool Write(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text << "\n";
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: w2f_record_fights OUTPUT_DIR [--data DIR]\n"); return 2; }
    const std::string outDir = argv[1];
    std::string dataDir = W2F_DATA_DIR;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--data") dataDir = argv[i + 1];
    }
    std::string error;
    auto champions = LoadChampionDatabaseFromFile(dataDir + "/champions.json", &error);
    auto traits = champions ? LoadTraitDatabaseFromFile(dataDir + "/traits.json", &error) : nullptr;
    auto items = traits ? LoadItemDatabaseFromFile(dataDir + "/items.json", &error) : nullptr;
    auto encounters = items ? LoadEncounterDatabaseFromFile(dataDir + "/pve.json", champions.get(), items.get(), &error) : nullptr;
    auto text = encounters ? TextTable::FromFile(dataDir + "/text_en.json", &error) : nullptr;   // the catalog carries the display text too
    if (!encounters) { std::fprintf(stderr, "Cannot load the data: %s\n", error.c_str()); return 2; }

    // Champion ids: Alesk 9001, Baira 9002, Les 9006, Lum 9007, Lunis 9009, Soul 9010, Vega 9012, Ignis 9013, Pyra 9014, Vex 9015, Null 9016, Bone 9017, Rot 9018, Bit 9019,
    // Solis 9020, Orion 9024, Nyx 9025, Lich 9027, Kryx 9029, Umbra 9031, and in 05: Cyla 9003, Faire 9005, Astra 9008, Myna 9011, Xul 9021, Grave 9022, Byte 9023, Flare 9026,
    // Raa 9028, Mortis 9030 (see the catalog).
    std::vector<Scenario> scenarios = {
        {"01-melee-brawl", "Two front lines of melee tanks and bruisers meet in the middle: walking, melee blows, shields, small circle and cone spells, deaths.",
         {{9013, 2, 2, 3}, {9017, 2, 3, 3}, {9020, 2, 4, 3}, {9025, 1, 1, 3}, {9029, 1, 5, 3}, {9015, 2, 3, 0}},
         {{9019, 2, 2, 3}, {9016, 2, 3, 3}, {9031, 1, 4, 3}, {9006, 1, 1, 3}, {9010, 2, 5, 3}, {9009, 2, 3, 0}}, 11},
        {"02-ranged-projectiles-and-dots", "Archers and casters behind a wall: projectiles of different speeds, Rot's poison, Lich's life drain, Baira's crashing tide and Pyra's piercing arrow.",
         {{9001, 2, 3, 3}, {9002, 2, 2, 1}, {9018, 2, 4, 1}, {9014, 2, 3, 1}, {9027, 2, 5, 1}, {9024, 1, 1, 1}},
         {{9013, 2, 3, 3}, {9020, 2, 4, 3}, {9018, 2, 2, 1}, {9027, 1, 3, 1}, {9014, 1, 4, 1}, {9015, 2, 5, 0}}, 22},
        {"03-spell-areas", "Spell areas (line, circle, cone), burn and poison, and items and synergies (Assassin, Helios) in play.",
         {{9012, 2, 3, 0}, {9014, 3, 2, 1}, {9009, 2, 3, 3}, {9015, 2, 4, 3}, {9013, 2, 2, 3}, {9020, 2, 5, 3, {3, 3, 8}}},
         {{9001, 2, 3, 3}, {9018, 2, 2, 1}, {9002, 2, 4, 1}, {9024, 2, 3, 1}, {9007, 2, 2, 3}, {9029, 2, 4, 3, {12}}}, 33},
        {"05-everyone-else", "The champions the other files leave out, so a viewer can check every basic attack and every ability: Cyla's rocket, Faire's stun, Astra's tether, Xul's bouncing bolt, "
         "Grave's skeleton, Byte's shield, Flare's sunbeam and Raa's leap against Soul, Myna, Null, Bone, Bit, Nyx, Mortis and Umbra.",
         {{9003, 2, 2, 1}, {9005, 2, 4, 1}, {9008, 2, 3, 0}, {9021, 2, 1, 1}, {9022, 2, 5, 1}, {9023, 2, 3, 1}, {9026, 2, 2, 0}, {9028, 2, 3, 3}},
         {{9010, 2, 2, 3}, {9011, 2, 3, 1}, {9016, 2, 4, 3}, {9017, 2, 3, 3}, {9019, 2, 1, 3}, {9025, 2, 5, 3}, {9030, 2, 2, 1}, {9031, 2, 3, 2}}, 55},
        {"06-september-2026", "The September 2026 champions: Tide, Sunna, Kael, Aphel, Sola with Lunis at her side (Blade Brothers: Teleport subtype 2), Morrah, Aureon and Nihila "
         "(the densest-cluster areas, the crit bounce, the pull toward a rift, the execute).",
         {{9032, 2, 3, 3}, {9034, 2, 2, 3}, {9036, 2, 4, 3}, {9009, 2, 5, 3}, {9033, 2, 3, 1}, {9035, 2, 2, 1}, {9038, 2, 4, 0}},
         {{9037, 2, 3, 1}, {9039, 2, 4, 1}, {9002, 2, 2, 1}, {9006, 2, 3, 3}, {9013, 2, 2, 3}, {9020, 2, 4, 3}, {9029, 2, 5, 3}}, 66},
    };
    {   // Trait system v2: Nature (7) with its plants (two Stonebark Trees, the Blossom, the Protector) against Hexagon (5) with the Invention's modules and Hexa
        // piloted by Ignis (the hex behind it), plus a Phaisa pair (Vex, Kryx: one mutation) and Rivet's shred.
        Scenario v2{"07-trait-system-v2", "Trait system v2: Nature's plants (stationary trees, the Blossom's blessing, the Protector; Stonebark's death stun), Hexa with a pilot "
                    "(the Piloting status, the eject), the Hexagon Invention (a summon on the back corner firing Electrical Overload and Magnetron Coil 8 s in), "
                    "Rivet's armour shred and a Phaisa mutation.",
                    {{9046, 2, 2, 3}, {9050, 2, 3, 3}, {9048, 2, 4, 3}, {9053, 2, 5, 3}, {9047, 2, 1, 1}, {9049, 2, 3, 1}, {9051, 2, 4, 1},
                     {9110, 2, 1, 3}, {9110, 2, 6, 3}, {9111, 2, 3, 0}, {9112, 2, 0, 3}},
                    {{9045, 2, 3, 3}, {9013, 2, 3, 2}, {9019, 2, 2, 3}, {9023, 2, 2, 1}, {9003, 2, 4, 1}, {9005, 2, 5, 1}, {9015, 2, 5, 3}, {9029, 2, 1, 3}, {9040, 2, 3, 1}}, 77};
        v2.setup.teams[1].modules = {401, 411};
        v2.setup.teams[0].playerLevel = v2.setup.teams[1].playerLevel = 8;
        scenarios.push_back(v2);
    }

    if (!Write(outDir + "/catalog.json", net::msg::Catalog(*champions, items.get(), traits.get(), encounters.get(), GameConfig{}.combat, text.get()))) { std::fprintf(stderr, "cannot write to %s\n", outDir.c_str()); return 2; }
    std::string readme =
        "# Sample fights\n\n"
        "Real `combat` messages, exactly as `w2f_server` sends them (one JSON object per file), plus `catalog.json`, the `catalog` message that says what every id in them means (names, costs, presentation timings, the\n"
        "vocabularies of the log). Use them to build and test an offline viewer before any WebSocket exists: parse `columns` / `events`, spawn actors from the `Spawn` rows, and play the timeline as described in\n"
        "[`../UE5-Integration.md`](../UE5-Integration.md), section 7 and 8. In every file the home team (team 0, seat 0) fights the away team (team 1, seat 1) in round 5. `checksum` lets you verify your parsing.\n"
        "Regenerate with `make sample-fights` (after a change to `data/*.json` or the combat rules); they are validated against `docs/schemas/server-message.schema.json` by the test suite.\n\n"
        "| file | what it shows | result | events | notes |\n|---|---|---|---|---|\n";
    const auto summarize = [&](const char* file, const char* title, const FightResult& r) {
        int overtimeAt = -1, projectiles = 0, casts = 0, dotDamage = 0;
        for (const CombatEvent& e : r.log.events) {
            if (e.type == CombatEventType::Overtime) overtimeAt = e.tick;
            if (e.type == CombatEventType::Attack && e.kind == 1) ++projectiles;
            if (e.type == CombatEventType::SpellCast) ++casts;
            if (e.type == CombatEventType::Damage && e.kind != 0) ++dotDamage;
        }
        char line[900];
        std::snprintf(line, sizeof(line), "| `%s.json` | %s | %s wins with %d left (%.1f s) | %zu | %d projectile attacks, %d spell casts, %d typed-DoT ticks%s |\n", file, title,
                      r.winner == CombatWinner::Home ? "home" : r.winner == CombatWinner::Away ? "away" : "nobody (a draw)", r.winner == CombatWinner::Home ? r.log.survivors[0] : r.log.survivors[1],
                      static_cast<double>(r.log.endTick) / kTicksPerSecond, r.log.events.size(), projectiles, casts, dotDamage,
                      overtimeAt >= 0 ? (", **overtime from tick " + std::to_string(overtimeAt) + "**").c_str() : "");
        readme += line;
        std::printf("%s: winner=%d end=%d events=%zu projectiles=%d casts=%d dots=%d overtime=%d\n", file, static_cast<int>(r.winner), r.log.endTick, r.log.events.size(), projectiles, casts, dotDamage, overtimeAt);
    };
    for (const Scenario& s : scenarios) {
        const FightResult r = Play(s.home, s.away, *champions, *items, *traits, s.seed, Seconds(125), s.setup);
        if (!Write(outDir + "/" + s.file + ".json", CombatMessage(r))) return 2;
        summarize(s.file, s.title, r);
    }

    // The overtime fight: the first of a fixed series of random boards that is still undecided after 40 s and ends with one team wiped out (the seeds are fixed: the same file every time).
    std::vector<ChampionId> real;
    for (const ChampionDefinition& c : champions->All()) {
        if (c.IsPooled()) real.push_back(c.id);
    }
    for (std::uint64_t attempt = 1; attempt <= 4000; ++attempt) {
        Rng rng(0x5A3B1E00ull + attempt, 3);
        std::vector<Placed> sides[2];
        for (int team = 0; team < 2; ++team) {
            std::vector<std::pair<int, int>> cells;
            for (int y = 0; y < kBoardRows; ++y) {
                for (int x = 0; x < kBoardColumns; ++x) cells.push_back({x, y});
            }
            const int count = 6 + static_cast<int>(rng.NextBelow(3));
            for (int k = 0; k < count; ++k) {
                const std::size_t cell = rng.NextBelow(static_cast<std::uint32_t>(cells.size()));
                sides[team].push_back({real[rng.NextBelow(static_cast<std::uint32_t>(real.size()))], 2 + static_cast<int>(rng.NextBelow(4) == 0), cells[cell].first, cells[cell].second});
                cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(cell));
            }
        }
        const FightResult r = Play(sides[0], sides[1], *champions, *items, *traits, attempt, Seconds(125));
        const bool wiped = r.log.survivors[0] == 0 || r.log.survivors[1] == 0;
        if (r.log.endTick > Seconds(40) && r.log.endTick < Seconds(70) && wiped && r.winner != CombatWinner::Draw) {
            const char* title = "A fight that is still undecided after 30 s: the Overtime row, then everything at 4x speed (attacks, steps, mana) until one team is wiped out.";
            if (!Write(outDir + "/04-overtime.json", CombatMessage(r))) return 2;
            summarize("04-overtime", title, r);
            break;
        }
        if (attempt == 4000) { std::fprintf(stderr, "no overtime fight found\n"); return 2; }
    }
    return Write(outDir + "/README.md", readme) ? 0 : 2;
}
