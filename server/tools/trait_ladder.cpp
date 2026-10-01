// Controlled fights for the balance passes (demo 1.7), where the bot matches cannot say anything: the bots rarely build a trait past its first
// breakpoints (out of ~8,000 final boards, Helios (5) came up 39 times and Coregons (6) never), and in bot matches a champion's numbers mix its
// power with how the bots use it. Here every fight is between two boards of EXACTLY the same gold (cost by cost), so only the champions and traits differ.
//
//   w2f_ladder [--fights N] [--seed S] [--data DIR] [--mode traits|champions|both] [--items]
//
//  * traits:    for every trait tier, board A is built on that trait (the tier's count of different holders; a prismatic tier gets one emblem holder,
//               a tier with too few holders gets emblems on fillers), filled up to the board size with random other champions; board B has one random
//               champion of the SAME cost for each of A's units. The table is A's win rate: 50% = the tier is worth nothing, the tier's power is the excess.
//  * champions: board A = the champion + random others, board B = the same costs picked at random; the champion's win rate at equal gold.
// Stars: 1- to 3-cost units are 2-star, 4- and 5-costs 1-star (a level 8 board). Board size = max(8, the tier's count). Tanks and Bastion/Bruiser units
// stand in the front row. --items gives every unit a random finished item (off by default: items are measured by w2f_balance).
// Deterministic: same seed, same report.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "w2f/ChampionLoader.h"
#include "w2f/CombatSimulator.h"
#include "w2f/Config.h"
#include "w2f/Hex.h"
#include "w2f/Rng.h"

#ifndef W2F_DATA_DIR
#define W2F_DATA_DIR "data"
#endif

using namespace w2f;

namespace {

struct Built {
    std::vector<const ChampionDefinition*> units;
    std::vector<std::vector<const ItemDefinition*>> items;
};

bool HasTrait(const ChampionDefinition* c, const std::string& t) { return std::find(c->traits.begin(), c->traits.end(), t) != c->traits.end(); }
int StarFor(const ChampionDefinition* c) { return c->cost <= 3 ? 2 : 1; }
bool Frontliner(const ChampionDefinition* c) { return c->role == ChampionRole::Tank; }

// Places a team: tanks in the front row first, the rest from the back row forward; columns spread from the middle.
std::vector<FightUnitSpec> Place(const Built& b, int team, UnitId firstId) {
    static const int cols[kBoardColumns] = {3, 2, 4, 1, 5, 0, 6};
    std::vector<int> order(b.units.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return Frontliner(b.units[static_cast<std::size_t>(x)]) > Frontliner(b.units[static_cast<std::size_t>(y)]); });
    std::vector<FightUnitSpec> out;
    int front = 0, back = 0;
    for (int idx : order) {
        const ChampionDefinition* c = b.units[static_cast<std::size_t>(idx)];
        int row, col;
        if (Frontliner(c) && front < kBoardColumns) { row = kBoardRows - 1; col = cols[front++]; }
        else {
            const int slot = back++;
            row = slot / kBoardColumns;                 // rows 0, 1, 2 from the back
            col = cols[slot % kBoardColumns];
            if (row >= kBoardRows - 1) { row = kBoardRows - 1; col = cols[(front++) % kBoardColumns]; }
        }
        FightUnitSpec s;
        s.id = static_cast<UnitId>(firstId + static_cast<UnitId>(out.size()));
        s.champion = c;
        s.starLevel = StarFor(c);
        s.team = team;
        s.position = BoardToArena(col, row, team == 0 ? ArenaSide::Home : ArenaSide::Away);
        s.items = b.items[static_cast<std::size_t>(idx)];
        out.push_back(s);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    int fights = 300;
    std::uint64_t seed = 1;
    std::string dataDir = W2F_DATA_DIR, mode = "both";
    bool withItems = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--fights") fights = std::max(10, std::atoi(value()));
        else if (a == "--seed") seed = std::strtoull(value(), nullptr, 10);
        else if (a == "--data") dataDir = value();
        else if (a == "--mode") mode = value();
        else if (a == "--items") withItems = true;
        else { std::fprintf(stderr, "usage: w2f_ladder [--fights N] [--seed S] [--data DIR] [--mode traits|champions|both] [--items]\n"); return 2; }
    }
    std::string error;
    auto champions = LoadChampionDatabaseFromFile(dataDir + "/champions.json", &error);
    auto traits = champions ? LoadTraitDatabaseFromFile(dataDir + "/traits.json", &error) : nullptr;
    auto items = traits ? LoadItemDatabaseFromFile(dataDir + "/items.json", &error) : nullptr;
    if (!items) { std::fprintf(stderr, "Cannot load the data: %s\n", error.c_str()); return 2; }
    GameConfig config;
    CombatSimulator sim(config.combat, traits.get(), items.get(), champions.get());

    std::vector<const ChampionDefinition*> pool;
    std::map<int, std::vector<const ChampionDefinition*>> byCost;
    for (const ChampionDefinition& c : champions->All()) {
        if (!c.IsPooled() || c.unlock.trait.size() > 0) continue;   // the shop's champions (Hexa, which must be unlocked, is added for Hexagon below)
        pool.push_back(&c);
        byCost[c.cost].push_back(&c);
    }
    std::vector<const ItemDefinition*> finished;
    for (const ItemDefinition& it : items->All()) { if (it.IsCombined() && it.grantsTraits.empty()) finished.push_back(&it); }
    const auto emblemFor = [&](const std::string& trait) -> const ItemDefinition* {
        for (const ItemDefinition& it : items->All()) { if (std::find(it.grantsTraits.begin(), it.grantsTraits.end(), trait) != it.grantsTraits.end()) return &it; }
        return nullptr;
    };
    Rng rng(seed);
    const auto pick = [&](const std::vector<const ChampionDefinition*>& from) { return from[static_cast<std::size_t>(rng.NextBelow(static_cast<std::uint32_t>(from.size())))]; };
    const auto randomItems = [&]() {
        std::vector<const ItemDefinition*> v;
        if (withItems && !finished.empty()) v.push_back(finished[static_cast<std::size_t>(rng.NextBelow(static_cast<std::uint32_t>(finished.size())))]);
        return v;
    };
    // B mirrors A's costs with random champions (never the same champion twice)
    const auto mirror = [&](const Built& a) {
        Built b;
        for (const ChampionDefinition* c : a.units) {
            const ChampionDefinition* m = nullptr;
            for (int tries = 0; tries < 50; ++tries) {
                m = pick(byCost[c->cost]);
                if (std::find(b.units.begin(), b.units.end(), m) == b.units.end()) break;
            }
            b.units.push_back(m);
            b.items.push_back(randomItems());
        }
        return b;
    };
    // a random filler that is not already on the board and does not carry `avoid`
    const auto filler = [&](const Built& b, const std::string& avoid) {
        for (int tries = 0; tries < 200; ++tries) {
            const ChampionDefinition* c = pick(pool);
            if ((avoid.empty() || !HasTrait(c, avoid)) && std::find(b.units.begin(), b.units.end(), c) == b.units.end()) return c;
        }
        return pick(pool);
    };
    const auto duel = [&](const Built& a, const Built& b, int level, std::uint64_t fightSeed, const std::vector<std::uint32_t>& modulesA, int path) {
        std::vector<FightUnitSpec> units = Place(a, 0, 1);
        const std::vector<FightUnitSpec> away = Place(b, 1, 101);
        units.insert(units.end(), away.begin(), away.end());
        FightSetup setup;
        setup.teams[0].playerLevel = setup.teams[1].playerLevel = level;
        setup.teams[0].modules = modulesA;
        const TraitDefinition* selini = traits->FindByName("Selini");
        if (selini != nullptr && !selini->paths.empty()) setup.traitPaths.push_back({selini->id, path});
        return sim.RunFight(units, config.combat.hardLimitTicks, fightSeed, setup);
    };

    if (mode == "traits" || mode == "both") {
        std::printf("== trait ladder: %d fights per tier, equal gold (A = the trait board, B = random boards of the same costs) ==\n", fights);
        std::printf("  %-12s %-9s %6s %8s %7s   %s\n", "trait", "tier", "units", "A won", "draws", "notes");
        for (const TraitDefinition& t : traits->All()) {
            const int paths = std::max<int>(1, static_cast<int>(t.paths.size()));
            for (int path = 0; path < paths; ++path) {
                const std::vector<TraitBreakpoint>& bps = t.Breakpoints(path);
                std::vector<const ChampionDefinition*> holders;
                for (const ChampionDefinition* c : pool) { if (HasTrait(c, t.name)) holders.push_back(c); }
                for (const ChampionDefinition& c : champions->All()) { if (c.unlock.trait.size() > 0 && HasTrait(&c, t.name)) holders.push_back(&c); }   // Hexa
                if (holders.empty() || bps.empty()) continue;
                const ItemDefinition* emblem = emblemFor(t.name);
                for (std::size_t k = 0; k < bps.size(); ++k) {
                    const int count = bps[k].count;
                    const bool prismatic = bps.size() >= kPrismaticMinBreakpoints && k + 1 == bps.size();
                    const int needEmblems = std::max(prismatic ? 1 : 0, count - static_cast<int>(holders.size()));
                    const int size = std::max(8, count);
                    if (size > kMaxPlayerLevel) { std::printf("  %-12s %d (%2d)    -- cannot be reached: a board holds %d units\n", t.name.c_str(), static_cast<int>(k + 1), count, kMaxPlayerLevel); continue; }
                    if (needEmblems > 0 && emblem == nullptr) continue;
                    // the modules a Hexagon player would have by this tier: one random module of each tier up to it
                    long won = 0, draws = 0;
                    for (int f = 0; f < fights; ++f) {
                        Built a;
                        std::vector<const ChampionDefinition*> h = holders;
                        for (std::size_t i = h.size(); i > 1; --i) std::swap(h[i - 1], h[static_cast<std::size_t>(rng.NextBelow(static_cast<std::uint32_t>(i)))]);
                        for (int i = 0; i < count - needEmblems; ++i) { a.units.push_back(h[static_cast<std::size_t>(i)]); a.items.push_back(randomItems()); }
                        for (int i = 0; i < needEmblems; ++i) { a.units.push_back(filler(a, t.name)); a.items.push_back({emblem}); }
                        while (static_cast<int>(a.units.size()) < size) { a.units.push_back(filler(a, t.name)); a.items.push_back(randomItems()); }
                        std::vector<std::uint32_t> modules;
                        for (int tier = 1; tier <= static_cast<int>(k + 1) && tier <= 3; ++tier) {
                            std::vector<std::uint32_t> of;
                            for (const TraitModule& m : t.modules) { if (m.tier == tier) of.push_back(m.id); }
                            if (!of.empty()) modules.push_back(of[static_cast<std::size_t>(rng.NextBelow(static_cast<std::uint32_t>(of.size())))]);
                        }
                        const Built b = mirror(a);
                        const FightResult r = duel(a, b, std::min(kMaxPlayerLevel, size), seed * 1000003ull + static_cast<std::uint64_t>(f), modules, path);
                        won += r.winner == CombatWinner::Home ? 1 : 0;
                        draws += r.winner == CombatWinner::Draw ? 1 : 0;
                    }
                    const double rate = 100.0 * static_cast<double>(won) / static_cast<double>(fights);
                    std::string note = needEmblems > 0 ? std::to_string(needEmblems) + " emblem" + (needEmblems > 1 ? "s" : "") : "";
                    if (paths > 1) note += (note.empty() ? "" : ", ") + std::string("path ") + std::to_string(path);
                    // Nature's power is its plants, which the match (MatchTraits) puts on the board -- these duels do not field them: use w2f_balance.
                    if (!bps[k].plants.empty()) note += (note.empty() ? "" : ", ") + std::string("plants NOT simulated: see w2f_balance");
                    std::printf("  %-12s %d (%2d)   %6d %7.1f%% %6ld   %s%s\n", t.name.c_str(), static_cast<int>(k + 1), count, size, rate, draws, note.c_str(),
                                rate > 80.0 ? "   <- very strong" : (rate < 55.0 ? "   <- barely helps" : ""));
                }
            }
        }
    }
    if (mode == "champions" || mode == "both") {
        std::printf("\n== champions at equal gold: %d fights each (A = the champion + 7 random others, B = the same costs at random) ==\n", fights);
        std::vector<std::pair<double, const ChampionDefinition*>> rows;
        for (const ChampionDefinition* c : pool) {
            long won = 0;
            for (int f = 0; f < fights; ++f) {
                Built a;
                a.units.push_back(c); a.items.push_back(randomItems());
                while (a.units.size() < 8) { a.units.push_back(filler(a, "")); a.items.push_back(randomItems()); }
                Built b = mirror(a);
                for (int tries = 0; tries < 50 && std::find(b.units.begin(), b.units.end(), c) != b.units.end(); ++tries) b = mirror(a);   // B never has the champion
                const FightResult r = duel(a, b, 8, seed * 7919ull + static_cast<std::uint64_t>(f), {}, 0);
                won += r.winner == CombatWinner::Home ? 1 : 0;
            }
            rows.push_back({100.0 * static_cast<double>(won) / static_cast<double>(fights), c});
        }
        std::sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.second->cost != y.second->cost ? x.second->cost < y.second->cost : x.first > y.first; });
        int lastCost = 0;
        for (const auto& [rate, c] : rows) {
            if (c->cost != lastCost) { std::printf("  -- %d-cost --\n", c->cost); lastCost = c->cost; }
            std::printf("  %-12s %5.1f%%%s\n", c->name.c_str(), rate, rate > 60.0 ? "   <- strong" : (rate < 40.0 ? "   <- weak" : ""));
        }
    }
    return 0;
}
