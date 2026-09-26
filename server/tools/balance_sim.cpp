// The bulk match simulator behind the balance passes: plays many seeded 8-bot matches under the real rules and reports what the fights say.
//
//   w2f_balance [--matches N] [--seed S] [--data DIR] [--json FILE] [--min-fights N]
//
// It measures, over every PvP fight of every match (PvE fights and the monsters are left out):
//   * fight length (mean / median / 90th percentile / longest), how many fights needed overtime, how many ran past 35 s, and mutual wipe-outs (the only draws);
//   * per champion: how often it was fielded, the win rate of the fights it was fielded in, its average star level and the damage it dealt per fight;
//   * per synergy tier: how often a team had it active and how those teams did;
//   * win rate by champion cost and by role, the win share of the home side (a bias check), and the length of a match in rounds.
// It only READS what the engine logs: nothing here can change a match. Bots are the players (all 8 seats), so the numbers describe how the bots
// play the game, which is a good relative yardstick (which champion or synergy is far ahead or behind) and not a claim about human play.
// Same seeds -> same report on every platform.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "w2f/AIBotController.h"
#include "w2f/ChampionLoader.h"
#include "w2f/CombatSimulator.h"
#include "w2f/MatchManager.h"

#ifndef W2F_DATA_DIR
#define W2F_DATA_DIR "data"
#endif

using namespace w2f;

namespace {

struct Tally {
    long appearances = 0;
    long wins = 0;
    long starSum = 0;
    long long damage = 0;
    double Rate() const { return appearances > 0 ? 100.0 * static_cast<double>(wins) / static_cast<double>(appearances) : 0.0; }
};

struct Collector : IMatchListener {
    std::map<ChampionId, Tally> champions;
    std::map<std::pair<TraitId, int>, Tally> traitTiers;   // (trait, tier) -> teams that had at least that tier, and how they did
    std::map<int, Tally> byCost;
    std::map<int, Tally> byRole;
    std::map<ChampionId, long> inLongFights;   // champions fielded in fights that lasted more than 60 s (the stall suspects)
    long longFights = 0, championSlots = 0;
    std::map<ChampionId, long> slots;          // every fielded champion, for comparison
    std::vector<int> fightTicks;
    long pvpFights = 0, overtimeFights = 0, over35s = 0, mutualWipes = 0, homeWins = 0, decided = 0, hardLimit = 0;
    const ChampionDatabase* db = nullptr;

    void OnCombatSimulated(int, const CombatOutcome& outcome) override {
        if (outcome.matchup.awayIsMonsters || outcome.log.events.empty()) return;
        ++pvpFights;
        const CombatLog& log = outcome.log;
        fightTicks.push_back(log.endTick);
        bool overtime = false;
        std::map<UnitId, ChampionId> championOf;
        std::map<UnitId, int> teamOf;
        std::map<std::pair<int, ChampionId>, int> stars;           // (team, champion) -> highest star fielded
        std::map<std::pair<int, ChampionId>, long long> damage;    // (team, champion) -> damage dealt
        std::map<std::pair<int, TraitId>, int> tiers;              // (team, trait) -> tier reached
        for (const CombatEvent& e : log.events) {
            switch (e.type) {
                case CombatEventType::Spawn:
                    if ((e.flags & kFlagSummon) != 0) break;
                    championOf[e.unit] = e.champion;
                    teamOf[e.unit] = e.team;
                    stars[{e.team, e.champion}] = std::max(stars[{e.team, e.champion}], static_cast<int>(e.star));
                    break;
                case CombatEventType::TraitActivated: tiers[{e.team, e.traitId}] = e.subtype; break;
                case CombatEventType::Damage: {
                    const auto attacker = championOf.find(e.other);
                    if (attacker != championOf.end() && teamOf[e.other] != teamOf[e.unit]) damage[{teamOf[e.other], attacker->second}] += e.amount;
                    break;
                }
                case CombatEventType::Overtime: overtime = true; break;
                default: break;
            }
        }
        overtimeFights += overtime ? 1 : 0;
        over35s += log.endTick > Seconds(35) ? 1 : 0;
        hardLimit += log.endTick >= Seconds(120) ? 1 : 0;
        const bool draw = outcome.winner == CombatWinner::Draw;
        mutualWipes += draw ? 1 : 0;
        if (!draw) {
            ++decided;
            homeWins += outcome.winner == CombatWinner::Home ? 1 : 0;
        }
        const bool isLong = log.endTick > Seconds(60);
        longFights += isLong ? 1 : 0;
        for (const auto& [key, star] : stars) {
            (void)star;
            ++slots[key.second];
            ++championSlots;
            if (isLong) ++inLongFights[key.second];
        }
        if (draw) return;   // a mutual wipe says nothing about who is stronger
        for (const auto& [key, star] : stars) {
            const bool won = (key.first == 0) == (outcome.winner == CombatWinner::Home);
            const ChampionDefinition* def = db->Find(key.second);
            for (Tally* t : {&champions[key.second], def != nullptr ? &byCost[def->cost] : nullptr, def != nullptr ? &byRole[static_cast<int>(def->role)] : nullptr}) {
                if (t == nullptr) continue;
                ++t->appearances;
                t->wins += won ? 1 : 0;
                t->starSum += star;
                t->damage += damage[key];
            }
        }
        for (const auto& [key, tier] : tiers) {
            const bool won = (key.first == 0) == (outcome.winner == CombatWinner::Home);
            Tally& t = traitTiers[{key.second, tier}];   // (each tier counts the teams whose HIGHEST active tier it was)
            ++t.appearances;
            t.wins += won ? 1 : 0;
        }
    }
};

double Pct(long part, long whole) { return whole > 0 ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0; }

double Percentile(std::vector<int> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t index = std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
    return static_cast<double>(v[index]) / kTicksPerSecond;
}

}  // namespace

int main(int argc, char** argv) {
    int matches = 60;
    std::uint64_t baseSeed = 1;
    std::string dataDir = W2F_DATA_DIR, jsonPath;
    long minFights = 150;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--matches") matches = std::atoi(value());
        else if (a == "--seed") baseSeed = std::strtoull(value(), nullptr, 10);
        else if (a == "--data") dataDir = value();
        else if (a == "--json") jsonPath = value();
        else if (a == "--min-fights") minFights = std::atol(value());
        else { std::fprintf(stderr, "usage: w2f_balance [--matches N] [--seed S] [--data DIR] [--json FILE] [--min-fights N]\n"); return 2; }
    }
    if (matches < 1) matches = 1;

    std::string error;
    auto champions = LoadChampionDatabaseFromFile(dataDir + "/champions.json", &error);
    auto traits = champions ? LoadTraitDatabaseFromFile(dataDir + "/traits.json", &error) : nullptr;
    auto items = traits ? LoadItemDatabaseFromFile(dataDir + "/items.json", &error) : nullptr;
    auto encounters = items ? LoadEncounterDatabaseFromFile(dataDir + "/pve.json", champions.get(), items.get(), &error) : nullptr;
    auto motherNature = encounters ? LoadMotherNatureDatabaseFromFile(dataDir + "/mother_nature.json", items.get(), &error) : nullptr;
    if (!motherNature || !ValidateChampionTraits(*champions, *traits, &error)) {
        std::fprintf(stderr, "Cannot load the data: %s\n", error.c_str());
        return 2;
    }

    Collector collector;
    collector.db = champions.get();
    long rounds = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int m = 0; m < matches; ++m) {
        const std::uint64_t seed = baseSeed + static_cast<std::uint64_t>(m);
        GameConfig config;
        auto match = MatchManager::Create(config, *champions, seed, std::make_unique<CombatSimulator>(config.combat, traits.get(), items.get()), &error, items.get(),
                                          encounters.get(), motherNature.get(), traits.get());
        if (!match) { std::fprintf(stderr, "Cannot start a match: %s\n", error.c_str()); return 2; }
        match->AddListener(&collector);
        std::vector<AIBotController> bots;
        for (int seat = 0; seat < kMaxPlayers; ++seat) bots.emplace_back(static_cast<PlayerId>(seat), seed, BotProfile{}, traits.get());
        match->Start();
        for (long tick = 0; !match->IsFinished() && tick < 20'000'000; ++tick) {
            for (AIBotController& bot : bots) bot.Tick(*match);
            match->Tick();
        }
        rounds += match->Round();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // ---- report ----
    double mean = 0;
    for (int t : collector.fightTicks) mean += t;
    mean = collector.fightTicks.empty() ? 0 : mean / static_cast<double>(collector.fightTicks.size()) / kTicksPerSecond;
    std::printf("== W2F balance report: %d matches (seeds %llu..%llu), %ld PvP fights, %.1f s of computing ==\n", matches, static_cast<unsigned long long>(baseSeed),
                static_cast<unsigned long long>(baseSeed + static_cast<std::uint64_t>(matches) - 1), collector.pvpFights, seconds);
    std::printf("match length: %.1f rounds on average\n", static_cast<double>(rounds) / matches);
    std::printf("fight length: mean %.1f s | median %.1f s | 90th percentile %.1f s | 99th %.1f s | longest %.1f s\n", mean, Percentile(collector.fightTicks, 0.5),
                Percentile(collector.fightTicks, 0.9), Percentile(collector.fightTicks, 0.99), Percentile(collector.fightTicks, 1.0));
    std::printf("overtime in %.1f%% of fights | %.1f%% ran past 35 s | %ld hit the safety limit | %.2f%% mutual wipe-outs (draws)\n", Pct(collector.overtimeFights, collector.pvpFights),
                Pct(collector.over35s, collector.pvpFights), collector.hardLimit, Pct(collector.mutualWipes, collector.pvpFights));
    std::printf("home side won %.1f%% of the decided fights (50 = no bias)\n", Pct(collector.homeWins, collector.decided));

    std::printf("\n-- win rate by cost (fights each champion of that cost was fielded in) --\n");
    for (const auto& [cost, t] : collector.byCost) std::printf("  %d-cost   %6ld fielded   %5.1f%% won   avg star %.2f\n", cost, t.appearances, t.Rate(), static_cast<double>(t.starSum) / static_cast<double>(std::max<long>(t.appearances, 1)));
    for (const auto& [role, t] : collector.byRole) std::printf("  %-8s %6ld fielded   %5.1f%% won\n", role == static_cast<int>(ChampionRole::Tank) ? "tanks" : "damage", t.appearances, t.Rate());

    struct Row { std::string name; int cost; const Tally* t; ChampionRole role; };
    std::vector<Row> rows;
    for (const auto& [id, t] : collector.champions) {
        const ChampionDefinition* def = champions->Find(id);
        if (def != nullptr) rows.push_back({def->name, def->cost, &t, def->role});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.t->Rate() > b.t->Rate(); });
    std::printf("\n-- champions, best win rate first (a fight counts once per champion fielded) --\n");
    std::printf("  %-12s cost %-6s %8s %8s %6s %10s\n", "champion", "role", "fielded", "won %", "star", "dmg/fight");
    for (const Row& r : rows) {
        const bool outlier = r.t->appearances >= minFights && (r.t->Rate() > 58.0 || r.t->Rate() < 42.0);
        std::printf("  %-12s %4d %-6s %8ld %7.1f%% %6.2f %10lld %s\n", r.name.c_str(), r.cost, r.role == ChampionRole::Tank ? "tank" : "damage", r.t->appearances, r.t->Rate(),
                    static_cast<double>(r.t->starSum) / static_cast<double>(std::max<long>(r.t->appearances, 1)),
                    r.t->damage / std::max<long>(r.t->appearances, 1), outlier ? (r.t->Rate() > 50 ? "  <- strong outlier" : "  <- weak outlier") : "");
    }

    if (collector.longFights > 0) {
        std::printf("\n-- stall suspects: the %ld fights longer than 60 s, and the champions over-represented in them (share of long fights / share of all fights) --\n", collector.longFights);
        std::vector<std::pair<double, ChampionId>> lift;
        long longSlots = 0;
        for (const auto& [id, n] : collector.inLongFights) longSlots += n;
        for (const auto& [id, n] : collector.inLongFights) {
            const double shareLong = static_cast<double>(n) / static_cast<double>(std::max<long>(longSlots, 1));
            const double shareAll = static_cast<double>(collector.slots[id]) / static_cast<double>(std::max<long>(collector.championSlots, 1));
            lift.push_back({shareAll > 0 ? shareLong / shareAll : 0.0, id});
        }
        std::sort(lift.rbegin(), lift.rend());
        for (std::size_t i = 0; i < lift.size() && i < 6; ++i) {
            const ChampionDefinition* def = champions->Find(lift[i].second);
            std::printf("  %-12s %.1fx as common in long fights\n", def != nullptr ? def->name.c_str() : "?", lift[i].first);
        }
    }

    std::printf("\n-- synergies: teams whose highest active tier was this one --\n");
    std::printf("  %-14s %-10s %9s %8s\n", "synergy", "tier (n)", "teams", "won %");
    for (const auto& [key, t] : collector.traitTiers) {
        const TraitDefinition* trait = traits->FindById(key.first);
        if (trait == nullptr || key.second < 1 || static_cast<std::size_t>(key.second) > trait->Breakpoints(0).size()) continue;
        const bool outlier = t.appearances >= minFights && (t.Rate() > 58.0 || t.Rate() < 42.0);
        std::printf("  %-14s %d (%d)%*s %9ld %7.1f%% %s\n", trait->name.c_str(), key.second, trait->Breakpoints(0)[static_cast<std::size_t>(key.second - 1)].count, 5, "", t.appearances, t.Rate(),
                    outlier ? (t.Rate() > 50 ? " <- strong outlier" : " <- weak outlier") : "");
    }
    std::printf("\n(Outliers: >= %ld samples and a win rate outside 42-58%%. Not everything has to be 50%%: a slightly stronger champion or synergy is fine.)\n", minFights);

    if (!jsonPath.empty()) {
        std::ofstream out(jsonPath);
        out << "{\"matches\":" << matches << ",\"pvp_fights\":" << collector.pvpFights << ",\"fight_seconds\":{\"mean\":" << mean << ",\"median\":" << Percentile(collector.fightTicks, 0.5)
            << ",\"p90\":" << Percentile(collector.fightTicks, 0.9) << ",\"max\":" << Percentile(collector.fightTicks, 1.0) << "},\"overtime_percent\":" << Pct(collector.overtimeFights, collector.pvpFights)
            << ",\"champions\":[";
        bool first = true;
        for (const Row& r : rows) {
            out << (first ? "" : ",") << "{\"name\":\"" << r.name << "\",\"cost\":" << r.cost << ",\"fielded\":" << r.t->appearances << ",\"win_percent\":" << r.t->Rate() << "}";
            first = false;
        }
        out << "]}\n";
    }
    return 0;
}
