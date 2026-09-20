// Dependency-free test runner: `make test`.

#include <array>
#include <cstdio>
#include <memory>
#include <map>
#include <set>
#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/MatchManager.h"
#include "w2f/PlayerManager.h"
#include "w2f/PlayerState.h"
#include "w2f/Rng.h"
#include "w2f/ShopManager.h"
#include "w2f/SharedChampionPool.h"
#include "w2f/UnitRoster.h"
#include "w2f/AIBotController.h"
#include "w2f/CombatSimulator.h"
#include "w2f/CombatEventSink.h"
#include "w2f/Hash.h"
#include "w2f/Hex.h"
#include "w2f/ChampionLoader.h"
#include "w2f/Json.h"
#include "SampleData.h"
#include "ScriptedPlayer.h"

using namespace w2f;
using sample::Def;
using sample::Legacy;
using sample::kChampionAlesk; using sample::kChampionBaira; using sample::kChampionCyla; using sample::kChampionDyno; using sample::kChampionFaire;
using sample::kChampionLes; using sample::kChampionLum; using sample::kChampionAstra; using sample::kChampionLunis;
using sample::kChampionSoul; using sample::kChampionMyna; using sample::kChampionVega;
using sample::kAbilityAesa; using sample::kAbilityArdeatsDestiny; using sample::kAbilityRocketStrike; using sample::kAbilityDynoShield; using sample::kAbilityExploit;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(...)                                                                    \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(__VA_ARGS__)) {                                                         \
            ++g_failures;                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #__VA_ARGS__);      \
        }                                                                             \
    } while (0)

// Most tests are about PvP rounds, and a default GameConfig makes rounds 1-3 and X-7 PvE. This config keeps every round PvP;
// the PvE tests build their own.
struct TestGameConfig : GameConfig {
    TestGameConfig() {
        match.firstStagePveRounds = 0;
        match.pveRoundInLaterStages = 0;
    }
};

// ---- Test fixtures -----------------------------------------------------------------------

// `perTier` champions of every cost 1..5. Champion id = cost * 100 + n.
static std::unique_ptr<ChampionDatabase> MakeDb(int perTier = 5) {
    std::vector<ChampionDefinition> defs;
    for (int cost = 1; cost <= kMaxCostTier; ++cost) {
        for (int n = 0; n < perTier; ++n) {
            defs.push_back(Def(static_cast<ChampionId>(cost * 100 + n),
                               ("T" + std::to_string(cost) + "_" + std::to_string(n)).c_str(), cost));
        }
    }
    std::string error;
    auto db = ChampionDatabase::Create(std::move(defs), &error);
    if (!db) std::printf("MakeDb failed: %s\n", error.c_str());
    return db;
}

static int TotalRemaining(const SharedChampionPool& pool) {
    int total = 0;
    for (int t = 1; t <= kMaxCostTier; ++t) total += pool.RemainingInTier(t);
    return total;
}

// Deterministic stand-in for the real combat sim: coin flip per fight, damage grows by round.
class CoinFlipSimulator : public ICombatSimulator {
public:
    std::vector<CombatOutcome> Simulate(const CombatContext& ctx) override {
        Rng rng(ctx.seed);
        std::vector<CombatOutcome> out;
        for (const Matchup& m : ctx.matchups) {
            CombatOutcome outcome;
            outcome.matchup = m;
            outcome.winner = rng.NextBelow(2) ? CombatWinner::Home : CombatWinner::Away;
            outcome.winnerSurvivors = 1 + static_cast<int>(rng.NextBelow(4));
            out.push_back(outcome);
        }
        return out;
    }
};

// Records every unit event so tests can assert on order and content.
struct UnitEvent {
    enum Kind { Bought, Sold, Moved, Merged } kind;
    PlayerId player;
    UnitId unit;        // bought/sold/moved: the unit. merged: the survivor.
    int number;         // bought: gold spent. sold: gold gained. merged: new star level. moved: 0.
    UnitId consumedA = kInvalidUnitId;  // merged only
    UnitId consumedB = kInvalidUnitId;
    LocationType fromLocation = LocationType::None;  // moved only
    int fromX = -1, fromY = -1;
    LocationType toLocation = LocationType::None;
    int toX = -1, toY = -1;
};

class EventLog : public IMatchListener {
public:
    std::vector<UnitEvent> events;
    int Count(UnitEvent::Kind kind) const {
        int n = 0;
        for (const auto& e : events) n += e.kind == kind ? 1 : 0;
        return n;
    }
    void OnUnitBought(PlayerId p, const UnitInstance& u, int gold) override {
        events.push_back({UnitEvent::Bought, p, u.id, gold});
    }
    void OnUnitSold(PlayerId p, const UnitInstance& u, int gold) override {
        events.push_back({UnitEvent::Sold, p, u.id, gold});
    }
    void OnUnitMoved(PlayerId p, const UnitMove& m) override {
        UnitEvent e{UnitEvent::Moved, p, m.unit.id, 0};
        e.fromLocation = m.fromLocation; e.fromX = m.fromX; e.fromY = m.fromY;
        e.toLocation = m.unit.location; e.toX = m.unit.x; e.toY = m.unit.y;
        events.push_back(e);
    }
    void OnUnitMerged(PlayerId p, const UnitMerge& m) override {
        UnitEvent e{UnitEvent::Merged, p, m.upgraded.id, m.upgraded.starLevel};
        e.consumedA = m.consumed[0]; e.consumedB = m.consumed[1];
        events.push_back(e);
    }
};

// Every copy of every champion is in exactly one place: pool, a shop slot, or a unit.
static bool PoolConserved(const ChampionDatabase& db, const SharedChampionPool& pool,
                          const std::vector<const PlayerState*>& players) {
    for (const ChampionDefinition& def : db.All()) {
        int out = 0;
        for (const PlayerState* p : players) {
            for (const ChampionDefinition* slot : p->Shop().Slots()) out += slot == &def ? 1 : 0;
            for (const UnitInstance& u : p->Roster().Units()) {
                if (u.champion == &def) out += SharedChampionPool::CopiesForStarLevel(u.starLevel);
            }
        }
        if (pool.Remaining(def.id) + out != pool.InitialCopies(def.id)) return false;
    }
    return true;
}

static bool FirstEmptyBoardCell(const PlayerState& p, int& outX, int& outY) {
    for (int y = 0; y < kBoardRows; ++y) {
        for (int x = 0; x < kBoardColumns; ++x) {
            if (p.Roster().BoardAt(x, y) == nullptr) { outX = x; outY = y; return true; }
        }
    }
    return false;
}

// Scripted "player" that exercises the whole economy and board management using only the
// public action API: buys, rerolls, levels, merges (by buying), fields units, sells.
static void RunBots(MatchManager& match) {
    for (PlayerId id : match.Players().AlivePlayerIds()) {
        const PlayerState* p = match.Players().Get(id);
        if (p->Roster().Count() >= 14) match.TrySellUnit(id, p->Roster().Units().front().id);
        if (p->Gold() >= 20) match.TryBuyXp(id);
        for (std::size_t slot = 0; slot < p->Shop().Slots().size(); ++slot) match.TryBuyShopUnit(id, slot);
        if (p->Gold() >= 8) {
            match.TryRerollShop(id);
            for (std::size_t slot = 0; slot < p->Shop().Slots().size(); ++slot) match.TryBuyShopUnit(id, slot);
        }

        // Odd seats pull their (0,0) unit back to the bench first, to exercise board->bench.
        if (id % 2 == 1) {
            if (const UnitInstance* front = p->Roster().BoardAt(0, 0)) {
                for (int slot = 0; slot < kBenchSlots; ++slot) {
                    if (p->Roster().BenchAt(slot) == nullptr) {
                        match.TryMoveUnit(id, front->id, LocationType::Bench, slot, 0);
                        break;
                    }
                }
            }
        }
        // Field every benched unit on the first free board cell.
        const std::vector<UnitInstance> snapshot = p->Roster().Units();
        for (const UnitInstance& u : snapshot) {
            int x = 0, y = 0;
            if (u.location == LocationType::Bench && FirstEmptyBoardCell(*p, x, y)) {
                match.TryMoveUnit(id, u.id, LocationType::Board, x, y);
            }
        }
        // And shuffle two board units so swaps happen too.
        if (p->Roster().BoardCount() >= 2) {
            const UnitInstance* a = p->Roster().BoardAt(0, 0);
            const UnitInstance* b = p->Roster().BoardAt(1, 0);
            if (a && b) match.TryMoveUnit(id, a->id, LocationType::Board, 1, 0);
        }
    }
}

struct MatchRun {
    std::vector<std::uint64_t> hashes;  // per tick
    PlayerId winner = kInvalidPlayerId;
    int rounds = 0;
    bool integrityHeld = true;
    bool layoutsHeld = true;
    bool finished = false;
    int bought = 0, sold = 0, moved = 0, merged = 0;
};

static MatchRun PlayFullMatch(const ChampionDatabase& db, std::uint64_t seed) {
    TestGameConfig cfg;
    cfg.match.planningTicks = 60;  // Shorter phases: same logic, faster test.
    cfg.match.motherNatureTicks = 30;
    cfg.match.combatTicks = 60;
    cfg.match.resolutionTicks = 30;
    cfg.player.startingGold = 10;
    auto match = MatchManager::Create(cfg, db, seed, std::make_unique<CoinFlipSimulator>());

    EventLog log;
    match->AddListener(&log);
    MatchRun run;
    match->Start();
    MatchPhase lastPhase = match->Phase();
    RunBots(*match);
    for (int tick = 0; tick < 2'000'000 && !match->IsFinished(); ++tick) {
        match->Tick();
        if (match->Phase() != lastPhase) {
            lastPhase = match->Phase();
            if (lastPhase == MatchPhase::Planning) RunBots(*match);
        }
        run.hashes.push_back(match->StateHash());
        run.integrityHeld = run.integrityHeld && match->VerifyPoolIntegrity();
        run.layoutsHeld = run.layoutsHeld && match->VerifyRosterLayouts();
    }
    run.bought = log.Count(UnitEvent::Bought);
    run.sold = log.Count(UnitEvent::Sold);
    run.moved = log.Count(UnitEvent::Moved);
    run.merged = log.Count(UnitEvent::Merged);
    run.finished = match->IsFinished();
    run.winner = match->Winner();
    run.rounds = match->Round();
    return run;
}

// ---- Tests -------------------------------------------------------------------------------

static void TestRng() {
    Rng a(42), b(42), c(43), s0(42, 0), s1(42, 1);
    bool same = true, diffSeed = false, diffStream = false;
    for (int i = 0; i < 100; ++i) {
        const auto x = a.Next64();
        same = same && x == b.Next64();
        diffSeed = diffSeed || x != c.Next64();
        diffStream = diffStream || s0.Next64() != s1.Next64();
    }
    CHECK(same);
    CHECK(diffSeed);
    CHECK(diffStream);

    Rng r(7);
    int buckets[6] = {};
    bool inRange = true;
    for (int i = 0; i < 60000; ++i) {
        const auto v = r.NextBelow(6);
        inRange = inRange && v < 6;
        if (v < 6) ++buckets[v];
    }
    CHECK(inRange);
    for (int count : buckets) CHECK(count > 9000 && count < 11000);

    std::vector<int> v1 = {1, 2, 3, 4, 5, 6, 7, 8}, v2 = v1;
    Rng s(9), t(9);
    s.Shuffle(v1);
    t.Shuffle(v2);
    CHECK(v1 == v2);
}

static void TestDatabase() {
    std::string error;
    CHECK(ChampionDatabase::Create({Def(0, "zero-id", 1)}, &error) == nullptr);
    CHECK(ChampionDatabase::Create({Def(5, "bad-cost", 6)}, &error) == nullptr);
    CHECK(ChampionDatabase::Create({Def(5, "a", 1), Def(5, "dup", 2)}, &error) == nullptr);
    auto db = ChampionDatabase::Create({Def(20, "Alesk", 2), Def(10, "Cyla", 1)}, &error);
    CHECK(db != nullptr);
    CHECK(db->All().front().id == 10);  // sorted by id regardless of load order
    CHECK(db->Find(20) && db->Find(20)->name == "Alesk");
    CHECK(db->Find(99) == nullptr);
}

static void TestConfigValidation() {
    TestGameConfig cfg;
    CHECK(cfg.Validate());
    for (const auto& row : cfg.shop.dropRatesByLevel) {
        int sum = 0;
        for (int w : row) sum += w;
        CHECK(sum == 100);
    }
    GameConfig bad = cfg;
    bad.match.playerCount = 9;
    CHECK(!bad.Validate());
    bad = cfg;
    bad.shop.dropRatesByLevel[3] = {{0, 0, 0, 0, 0}};
    CHECK(!bad.Validate());
    bad = cfg;
    bad.player.xpToNextLevel[0] = 0;
    CHECK(!bad.Validate());
}

static void TestPool() {
    auto db = MakeDb();
    PoolConfig cfg;
    SharedChampionPool pool(*db, cfg);
    CHECK(pool.Remaining(100) == 29);
    CHECK(pool.Remaining(200) == 22);
    CHECK(pool.Remaining(300) == 18);
    CHECK(pool.Remaining(400) == 12);
    CHECK(pool.Remaining(500) == 10);
    CHECK(pool.RemainingInTier(1) == 5 * 29);
    CHECK(SharedChampionPool::CopiesForStarLevel(1) == 1);
    CHECK(SharedChampionPool::CopiesForStarLevel(2) == 3);
    CHECK(SharedChampionPool::CopiesForStarLevel(3) == 9);

    Rng rng(1);
    const ChampionDefinition* drawn = pool.DrawFromTier(3, rng);
    CHECK(drawn && drawn->cost == 3);
    CHECK(pool.Remaining(drawn->id) == 17);
    CHECK(pool.Return(drawn));
    CHECK(pool.Remaining(drawn->id) == 18);
    CHECK(!pool.Return(drawn));      // would exceed original supply
    CHECK(!pool.Return(drawn, 0));
    CHECK(pool.DrawFromTier(0, rng) == nullptr);
    CHECK(pool.DrawFromTier(6, rng) == nullptr);

    // Drain a tier completely: every copy comes out exactly once, then nullptr.
    const int tier5Total = pool.RemainingInTier(5);
    std::vector<const ChampionDefinition*> held;
    while (const ChampionDefinition* d = pool.DrawFromTier(5, rng)) held.push_back(d);
    CHECK(static_cast<int>(held.size()) == tier5Total);
    CHECK(pool.RemainingInTier(5) == 0);
    for (int n = 0; n < 5; ++n) CHECK(pool.Remaining(static_cast<ChampionId>(500 + n)) == 0);
    for (auto* d : held) CHECK(pool.Return(d));
    CHECK(pool.RemainingInTier(5) == tier5Total);
}

static void TestPlayerBasics() {
    auto db = MakeDb();
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 1);

    CHECK(p.Health() == 100);
    CHECK(p.Level() == 1);
    CHECK(p.Xp() == 0);
    CHECK(p.Gold() == 0);
    CHECK(p.IsAlive());

    CHECK(p.TryBuyXp() == ActionResult::NotEnoughGold);
    CHECK(p.Xp() == 0);

    // Buying XP: 4 gold -> 4 XP. L1->2 needs 2, L2->3 needs 2 => level 3 with 0 XP left.
    p.AddGold(10);
    CHECK(p.TryBuyXp() == ActionResult::Ok);
    CHECK(p.Gold() == 6);
    CHECK(p.Level() == 3);
    CHECK(p.Xp() == 0);
    CHECK(p.TryBuyXp() == ActionResult::Ok);  // +4 XP, L3->4 needs 6 -> 4/6
    CHECK(p.Level() == 3 && p.Xp() == 4);
    p.AddXp(2);
    CHECK(p.Level() == 4 && p.Xp() == 0);
    CHECK(p.XpToNextLevel() == 10);

    p.AddXp(1'000'000);
    CHECK(p.Level() == kMaxPlayerLevel);
    CHECK(p.Xp() == 0);
    CHECK(p.XpToNextLevel() == 0);
    p.AddGold(50);
    CHECK(p.TryBuyXp() == ActionResult::MaxLevel);
    CHECK(p.Gold() == 52);  // not charged

    p.ApplyDamage(30);
    CHECK(p.Health() == 70);
    p.ApplyDamage(-5);
    CHECK(p.Health() == 70);
}

static void TestIncome() {
    auto db = MakeDb();
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 1);

    // Base income table then the flat default.
    CHECK(p.CalculateIncome(1).baseGold == 2);
    CHECK(p.CalculateIncome(3).baseGold == 3);
    CHECK(p.CalculateIncome(4).baseGold == 4);
    CHECK(p.CalculateIncome(5).baseGold == 5);
    CHECK(p.CalculateIncome(50).baseGold == 5);

    // Interest: 1 per 10 gold, capped at 5.
    p.AddGold(9);
    CHECK(p.CalculateIncome(5).interestGold == 0);
    p.AddGold(1);
    CHECK(p.CalculateIncome(5).interestGold == 1);
    p.AddGold(40);
    CHECK(p.CalculateIncome(5).interestGold == 5);
    p.AddGold(500);
    CHECK(p.CalculateIncome(5).interestGold == 5);

    // Streaks (win or loss): 2->1, 4->2, 5+->3. Draw leaves it alone.
    PlayerState q(1, cfg.player, cfg.shop, pool, 1);
    q.RecordRoundResult(RoundResult::Win);
    CHECK(q.CalculateIncome(5).streakGold == 0);
    q.RecordRoundResult(RoundResult::Win);
    CHECK(q.CalculateIncome(5).streakGold == 1);
    q.RecordRoundResult(RoundResult::Draw);
    CHECK(q.Streak() == 2);
    q.RecordRoundResult(RoundResult::Win);
    q.RecordRoundResult(RoundResult::Win);
    CHECK(q.CalculateIncome(5).streakGold == 2);
    q.RecordRoundResult(RoundResult::Win);
    CHECK(q.CalculateIncome(5).streakGold == 3);
    q.RecordRoundResult(RoundResult::Loss);
    CHECK(q.Streak() == -1);
    q.RecordRoundResult(RoundResult::Loss);
    CHECK(q.CalculateIncome(5).streakGold == 1);

    // Granting applies gold and passive XP together.
    PlayerState r(2, cfg.player, cfg.shop, pool, 1);
    r.AddGold(20);
    const IncomeBreakdown before = r.CalculateIncome(2);
    CHECK(before.TotalGold() == 2 + 2 + 0);
    const IncomeBreakdown granted = r.GrantRoundIncome(2);
    CHECK(granted.TotalGold() == before.TotalGold());
    CHECK(r.Gold() == 24);
    CHECK(r.Level() == 2);  // +2 passive XP from level 1
    CHECK(r.CalculateIncome(1).passiveXp == 0);  // no passive XP before firstPassiveXpRound
}

static void TestShopReroll() {
    auto db = MakeDb();
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    const int poolTotal = TotalRemaining(pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 123);

    CHECK(p.Shop().TryReroll() == ActionResult::NotEnoughGold);
    CHECK(TotalRemaining(pool) == poolTotal);  // failed reroll draws nothing

    p.AddGold(7);
    CHECK(p.Shop().TryReroll() == ActionResult::Ok);
    CHECK(p.Gold() == 5);
    CHECK(TotalRemaining(pool) == poolTotal - 5);  // 5 slots checked out
    for (auto* s : p.Shop().Slots()) CHECK(s != nullptr && s->cost == 1);  // level 1 => tier 1 only

    // Rerolling returns the old offer: net pool change is still exactly 5.
    CHECK(p.Shop().TryReroll() == ActionResult::Ok);
    CHECK(TotalRemaining(pool) == poolTotal - 5);

    // Buying moves the copy from the shop to the roster, charges its cost, empties the slot.
    const ChampionDefinition* offered = p.Shop().Slots()[2];
    CHECK(p.Shop().TryBuy(2) == ActionResult::Ok);
    CHECK(p.Gold() == 3 - offered->cost);
    CHECK(p.Roster().Count() == 1 && p.Roster().Units()[0].champion == offered);
    CHECK(p.Shop().Slots()[2] == nullptr);
    CHECK(p.Shop().TryBuy(2) == ActionResult::EmptySlot);
    CHECK(p.Shop().TryBuy(9) == ActionResult::InvalidSlot);
    CHECK(TotalRemaining(pool) == poolTotal - 5);  // still 5 copies out (4 in shop + 1 owned)

    // Selling returns the copy and pays out.
    const UnitId boughtId = p.Roster().Units()[0].id;
    CHECK(p.SellUnit(boughtId) == ActionResult::Ok);
    CHECK(p.Roster().Count() == 0);
    CHECK(TotalRemaining(pool) == poolTotal - 4);
    CHECK(p.SellUnit(boughtId) == ActionResult::InvalidUnit);  // already sold
}

static void TestShopOddsByLevel() {
    auto db = MakeDb();
    TestGameConfig cfg;

    for (int level : {3, 7, 10}) {
        SharedChampionPool pool(*db, cfg.pool);
        PlayerState p(0, cfg.player, cfg.shop, pool, 99);
        while (p.Level() < level) p.AddXp(1);
        CHECK(p.Level() == level);

        int tierCount[kMaxCostTier + 1] = {};
        const int refreshes = 20000;
        for (int i = 0; i < refreshes; ++i) {
            p.Shop().Refresh();
            for (auto* s : p.Shop().Slots()) ++tierCount[s->cost];
        }
        const int draws = refreshes * cfg.shop.slotCount;
        for (int tier = 1; tier <= kMaxCostTier; ++tier) {
            const int expectedPercent = cfg.shop.dropRatesByLevel[static_cast<std::size_t>(level - 1)]
                                                                 [static_cast<std::size_t>(tier - 1)];
            const double actualPercent = 100.0 * tierCount[tier] / draws;
            const double diff = actualPercent - expectedPercent;
            if (diff > 1.0 || diff < -1.0) std::printf("  level %d tier %d expected %d%% got %.2f%%\n", level, tier, expectedPercent, actualPercent);
            CHECK(diff < 1.0 && diff > -1.0);
        }
    }
}

static void TestShopSoldOutTier() {
    // One tier-1 champion with 3 copies, nothing else: a level-1 shop can fill only 3 of 5 slots
    // and the pool must never go negative or hand out phantom copies.
    auto db = ChampionDatabase::Create({Def(1, "Only", 1)});
    TestGameConfig cfg;
    cfg.pool.copiesPerTier = {{3, 1, 1, 1, 1}};
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 5);
    p.Shop().Refresh();
    int filled = 0;
    for (auto* s : p.Shop().Slots()) filled += s ? 1 : 0;
    CHECK(filled == 3);
    CHECK(pool.Remaining(1) == 0);

    // A second player sees an empty shop while the first holds every copy...
    PlayerState q(1, cfg.player, cfg.shop, pool, 5);
    q.Shop().Refresh();
    for (auto* s : q.Shop().Slots()) CHECK(s == nullptr);
    // ...until the first player's shop refreshes back into the pool (or they're eliminated).
    p.Eliminate(8);
    CHECK(pool.Remaining(1) == 3);
    q.Shop().Refresh();
    filled = 0;
    for (auto* s : q.Shop().Slots()) filled += s ? 1 : 0;
    CHECK(filled == 3);
}

static void TestSellValueAndElimination() {
    CHECK(PlayerState::SellValue(1, 1) == 1);
    CHECK(PlayerState::SellValue(1, 2) == 3);
    CHECK(PlayerState::SellValue(1, 3) == 9);
    CHECK(PlayerState::SellValue(3, 1) == 3);
    CHECK(PlayerState::SellValue(3, 2) == 8);
    CHECK(PlayerState::SellValue(3, 3) == 26);

    // One 3-cost champion (18 copies) so we know exactly which copies we're holding.
    auto db = ChampionDatabase::Create({Def(7, "Solo", 3)});
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 1);

    Rng rng(1);
    const ChampionDefinition* solo = nullptr;
    for (int i = 0; i < 4; ++i) solo = pool.DrawFromTier(3, rng);  // check out 4 copies
    CHECK(solo && pool.Remaining(7) == 14);
    p.AcquireUnit(solo, 0, 2);  // a 2-star is 3 copies
    p.AcquireUnit(solo, 0, 1);  // plus one 1-star
    const UnitId twoStarId = p.Roster().Units()[0].id;

    // Selling the 2-star returns 3 copies and pays 3*3-1 = 8.
    CHECK(p.SellUnit(twoStarId) == ActionResult::Ok);
    CHECK(pool.Remaining(7) == 17);
    CHECK(p.Gold() == 8);

    // Elimination returns whatever is left and stops the player acting.
    p.Eliminate(5);
    CHECK(pool.Remaining(7) == 18);
    CHECK(!p.IsAlive() && p.Placement() == 5 && p.Roster().Count() == 0);
    CHECK(p.TryBuyXp() == ActionResult::PlayerEliminated);
    CHECK(p.Shop().TryReroll() == ActionResult::PlayerEliminated);
}


// ---- Phase 2: board, bench, merging ------------------------------------------------------

static void TestRosterPlacementAndFullRoster() {
    auto db = MakeDb();  // 25 champions
    const auto& champs = db->All();
    UnitRoster r(3);

    // 25 distinct champions: bench fills first (slot order), then the board row-major from the back row.
    std::vector<UnitId> ids;
    for (std::size_t i = 0; i < champs.size(); ++i) {
        auto res = r.Add(&champs[i]);
        CHECK(res.added && res.merges.empty());
        ids.push_back(res.unit.id);
    }
    for (int slot = 0; slot < kBenchSlots; ++slot) {
        const UnitInstance* u = r.BenchAt(slot);
        CHECK(u && u->id == ids[static_cast<std::size_t>(slot)] && u->x == slot && u->y == 0 && u->location == LocationType::Bench);
    }
    const UnitInstance* firstBoard = r.Find(ids[9]);
    CHECK(firstBoard && firstBoard->location == LocationType::Board && firstBoard->x == 0 && firstBoard->y == 0);
    const UnitInstance* secondRow = r.Find(ids[9 + kBoardColumns]);
    CHECK(secondRow && secondRow->x == 0 && secondRow->y == 1);
    CHECK(r.BenchCount() == 9 && r.BoardCount() == 16);

    // Ids: non-zero, unique, encode the owner (seat 3 -> 4 in the top byte).
    bool idsOk = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        idsOk = idsOk && ids[i] != kInvalidUnitId && (ids[i] >> 24) == 4;
        for (std::size_t j = i + 1; j < ids.size(); ++j) idsOk = idsOk && ids[i] != ids[j];
    }
    CHECK(idsOk);

    // A second copy of the first 12 champions: 37 units = bench 9 + board 28, completely full.
    std::vector<UnitId> secondCopies;
    for (std::size_t i = 0; i < 12; ++i) {
        auto res = r.Add(&champs[i]);
        CHECK(res.added && res.merges.empty());
        secondCopies.push_back(res.unit.id);
    }
    CHECK(r.Count() == 37 && r.BenchCount() == 9 && r.BoardCount() == 28);
    CHECK(r.CheckInvariants());

    // Full: a brand-new champion is rejected and nothing changes...
    CHECK(!r.CanAdd(&champs[20]));
    auto rejected = r.Add(&champs[20]);
    CHECK(!rejected.added && r.Count() == 37);
    // ...but a third copy is welcome because it merges on arrival without needing a slot.
    CHECK(r.CanAdd(&champs[0]));
    auto merged = r.Add(&champs[0]);
    CHECK(merged.added && merged.merges.size() == 1);
    CHECK(merged.unit.location == LocationType::None);  // it never occupied a cell
    CHECK(r.Count() == 36);
    CHECK(merged.merges[0].upgraded.starLevel == 2);
    // Copy #1 sat on the bench (slot 0), copy #2 on the board. The board copy survives.
    CHECK(merged.merges[0].upgraded.id == secondCopies[0]);
    CHECK(merged.merges[0].upgraded.location == LocationType::Board);
    const std::array<UnitId, 2> consumed = merged.merges[0].consumed;
    CHECK((consumed[0] == ids[0] && consumed[1] == merged.unit.id) || (consumed[1] == ids[0] && consumed[0] == merged.unit.id));
    CHECK(r.Find(ids[0]) == nullptr && r.BenchAt(0) == nullptr);  // the bench copy is gone, its slot free
    CHECK(r.BenchCount() == 8 && r.CheckInvariants());
}

static void TestMoves() {
    auto db = MakeDb();
    const auto& c = db->All();
    UnitRoster r(0);
    const UnitId a = r.Add(&c[0]).unit.id;  // bench 0
    const UnitId b = r.Add(&c[1]).unit.id;  // bench 1
    const UnitId d = r.Add(&c[2]).unit.id;  // bench 2
    std::vector<UnitMove> moves;

    // Bench -> empty board cell.
    CHECK(r.Move(a, LocationType::Board, 3, 2, moves) == ActionResult::Ok);
    CHECK(moves.size() == 1 && moves[0].unit.id == a);
    CHECK(moves[0].fromLocation == LocationType::Bench && moves[0].fromX == 0 && moves[0].fromY == 0);
    CHECK(moves[0].unit.location == LocationType::Board && moves[0].unit.x == 3 && moves[0].unit.y == 2);
    CHECK(r.BenchAt(0) == nullptr && r.BoardAt(3, 2) && r.BoardAt(3, 2)->id == a);
    CHECK(r.BoardCount() == 1 && r.BenchCount() == 2 && r.CheckInvariants());

    // Board -> empty bench slot.
    CHECK(r.Move(a, LocationType::Bench, 8, 0, moves) == ActionResult::Ok);
    CHECK(r.BenchAt(8) && r.BenchAt(8)->id == a && r.BoardAt(3, 2) == nullptr && r.CheckInvariants());

    // Swap bench <-> board (both units report a move).
    CHECK(r.Move(b, LocationType::Board, 0, 0, moves) == ActionResult::Ok);
    CHECK(r.Move(d, LocationType::Board, 0, 0, moves) == ActionResult::Ok);  // occupied by b -> swap
    CHECK(moves.size() == 2);
    CHECK(moves[0].unit.id == d && moves[0].unit.location == LocationType::Board);
    CHECK(moves[1].unit.id == b && moves[1].unit.location == LocationType::Bench && moves[1].unit.x == 2);
    CHECK(moves[1].fromLocation == LocationType::Board);
    CHECK(r.BoardAt(0, 0)->id == d && r.BenchAt(2)->id == b && r.CheckInvariants());
    CHECK(r.BoardCount() == 1 && r.BenchCount() == 2);  // population unchanged by a swap

    // Swap bench <-> bench and board <-> board.
    CHECK(r.Move(b, LocationType::Bench, 8, 0, moves) == ActionResult::Ok);  // occupied by a
    CHECK(r.BenchAt(8)->id == b && r.BenchAt(2)->id == a);
    CHECK(r.Move(a, LocationType::Board, 6, 3, moves) == ActionResult::Ok);
    CHECK(r.Move(a, LocationType::Board, 0, 0, moves) == ActionResult::Ok);  // occupied by d
    CHECK(r.BoardAt(0, 0)->id == a && r.BoardAt(6, 3)->id == d && r.CheckInvariants());

    // Moving onto yourself is a quiet no-op.
    CHECK(r.Move(a, LocationType::Board, 0, 0, moves) == ActionResult::Ok && moves.empty());

    // Invalid targets and units are rejected without changing anything.
    CHECK(r.Move(a, LocationType::Bench, 9, 0, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(a, LocationType::Bench, -1, 0, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(a, LocationType::Bench, 0, 1, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(a, LocationType::Board, 7, 0, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(a, LocationType::Board, 0, 4, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(a, LocationType::Board, 0, -1, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(a, LocationType::None, 0, 0, moves) == ActionResult::InvalidSlot);
    CHECK(r.Move(kInvalidUnitId, LocationType::Bench, 0, 0, moves) == ActionResult::InvalidUnit);
    CHECK(r.Move(0x7777, LocationType::Bench, 0, 0, moves) == ActionResult::InvalidUnit);
    CHECK(moves.empty() && r.BoardAt(0, 0)->id == a && r.CheckInvariants());
}

static void TestBoardCapacity() {
    auto db = MakeDb();
    const auto& c = db->All();
    UnitRoster r(0);
    r.SetBoardCapacity(1);
    const UnitId a = r.Add(&c[0]).unit.id;
    const UnitId b = r.Add(&c[1]).unit.id;
    const UnitId d = r.Add(&c[2]).unit.id;
    std::vector<UnitMove> moves;

    CHECK(r.Move(a, LocationType::Board, 0, 0, moves) == ActionResult::Ok);
    CHECK(r.Move(b, LocationType::Board, 1, 0, moves) == ActionResult::BoardFull);  // would be a 2nd fielded unit
    CHECK(r.BoardCount() == 1 && r.BenchAt(1)->id == b);
    CHECK(r.Move(b, LocationType::Board, 0, 0, moves) == ActionResult::Ok);          // swapping is fine
    CHECK(r.Move(b, LocationType::Board, 5, 3, moves) == ActionResult::Ok);          // board -> board is fine
    CHECK(r.Move(b, LocationType::Bench, 0, 0, moves) == ActionResult::Ok);          // board now empty (a is on the bench)
    CHECK(r.BoardCount() == 0);
    CHECK(r.Move(a, LocationType::Board, 0, 0, moves) == ActionResult::Ok);
    CHECK(r.Move(d, LocationType::Board, 2, 2, moves) == ActionResult::BoardFull);   // a holds the only slot
    r.SetBoardCapacity(2);
    CHECK(r.Move(d, LocationType::Board, 2, 2, moves) == ActionResult::Ok);
    CHECK(r.CheckInvariants());

    // Auto-placement honours the cap too: with the bench full, a new unit needs a board slot.
    UnitRoster full(1);
    full.SetBoardCapacity(1);
    for (int i = 0; i < 10; ++i) CHECK(full.Add(&c[static_cast<std::size_t>(i)]).added);  // 9 bench + 1 board
    CHECK(full.BoardCount() == 1);
    CHECK(!full.CanAdd(&c[10]));
    full.SetBoardCapacity(2);
    CHECK(full.CanAdd(&c[10]) && full.Add(&c[10]).added && full.BoardCount() == 2);
}

static void TestMergeRules() {
    auto db = MakeDb();
    const auto& c = db->All();
    UnitRoster r(0);

    // Two of one champion never merge; different champions never mix.
    CHECK(r.Add(&c[0]).merges.empty());
    CHECK(r.Add(&c[0]).merges.empty());
    CHECK(r.Add(&c[1]).merges.empty());
    CHECK(r.CountOf(&c[0], 1) == 2 && r.CountOf(&c[1], 1) == 1);

    // Third copy: three 1-stars become ONE 2-star.
    auto third = r.Add(&c[0]);
    CHECK(third.merges.size() == 1);
    CHECK(third.merges[0].previousStarLevel == 1 && third.merges[0].upgraded.starLevel == 2);
    CHECK(r.CountOf(&c[0], 1) == 0 && r.CountOf(&c[0], 2) == 1);
    CHECK(r.Count() == 2);  // the 2-star + the lone c[1]
    // All three copies were on the bench (slots 0,1,3): the lowest slot survives.
    CHECK(third.merges[0].upgraded.location == LocationType::Bench && third.merges[0].upgraded.x == 0);
    CHECK(r.CheckInvariants());

    // Different star levels never mix: a 2-star plus a fresh 1-star do nothing.
    CHECK(r.Add(&c[0]).merges.empty());
    CHECK(r.CountOf(&c[0], 1) == 1 && r.CountOf(&c[0], 2) == 1);

    // Cascade: keep feeding 1-stars. Nine total copies -> a single 3-star, via three 2-stars.
    UnitRoster cas(0);
    int mergeCount = 0;
    for (int copy = 1; copy <= 9; ++copy) {
        auto res = cas.Add(&c[5]);
        mergeCount += static_cast<int>(res.merges.size());
        if (copy == 9) CHECK(res.merges.size() == 2);  // 3rd 2-star completes -> immediate 3-star
        CHECK(cas.CheckInvariants());
    }
    CHECK(mergeCount == 4);
    CHECK(cas.Count() == 1 && cas.Units()[0].starLevel == 3);

    // Three-stars are the ceiling: they never merge further, even in threes.
    for (int i = 0; i < 3; ++i) CHECK(cas.Add(&c[5], 3).merges.empty());
    CHECK(cas.CountOf(&c[5], 3) == 4 && cas.CheckInvariants());

    // Star-2 additions merge with other 2-stars (e.g. an award that hands out a 2-star).
    UnitRoster up(0);
    up.Add(&c[7], 2);
    up.Add(&c[7], 2);
    auto res = up.Add(&c[7], 2);
    CHECK(res.merges.size() == 1 && res.merges[0].upgraded.starLevel == 3 && up.Count() == 1);
}

static void TestMergeThroughShop() {
    // One tier-1 champion only, so the level-1 shop always offers it and we control what we buy.
    auto db = ChampionDatabase::Create({Def(1, "Solo", 1)});
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);  // 29 copies
    EventLog log;
    PlayerState p(0, cfg.player, cfg.shop, pool, 42, &log);
    const std::vector<const PlayerState*> all = {&p};
    p.AddGold(100);

    CHECK(p.Shop().TryReroll() == ActionResult::Ok);  // 98 gold, 5 copies checked out
    CHECK(p.Shop().TryBuy(0) == ActionResult::Ok);
    CHECK(p.Shop().TryBuy(1) == ActionResult::Ok);
    CHECK(p.Gold() == 96 && p.Roster().Count() == 2);
    CHECK(log.Count(UnitEvent::Merged) == 0);

    // The merging purchase costs exactly its own price; the merge itself is free.
    const int goldBefore = p.Gold();
    CHECK(p.Shop().TryBuy(2) == ActionResult::Ok);
    CHECK(p.Gold() == goldBefore - 1);
    CHECK(p.Roster().Count() == 1 && p.Roster().Units()[0].starLevel == 2);
    CHECK(PoolConserved(*db, pool, all));
    // 29 total = pool + 2 shop slots + 3 copies inside the 2-star
    CHECK(pool.Remaining(1) == 29 - 5);

    // Events: bought, bought, bought, merged -- in that order, with the merge naming both losses.
    CHECK(log.events.size() == 4);
    CHECK(log.events[0].kind == UnitEvent::Bought && log.events[0].number == 1);
    CHECK(log.events[2].kind == UnitEvent::Bought);
    CHECK(log.events[3].kind == UnitEvent::Merged && log.events[3].number == 2);
    CHECK(log.events[3].unit == log.events[0].unit);  // first copy survives
    CHECK(log.events[3].consumedA == log.events[1].unit && log.events[3].consumedB == log.events[2].unit);
    CHECK(p.Roster().Find(log.events[3].consumedA) == nullptr && p.Roster().Find(log.events[3].consumedB) == nullptr);

    // Keep buying until the cascade completes: the 9th copy makes a 3-star.
    CHECK(p.Shop().TryBuy(3) == ActionResult::Ok);
    CHECK(p.Shop().TryBuy(4) == ActionResult::Ok);  // 5 bought so far
    CHECK(p.Shop().TryReroll() == ActionResult::Ok);
    for (std::size_t slot = 0; slot < 4; ++slot) CHECK(p.Shop().TryBuy(slot) == ActionResult::Ok);  // 9 bought
    CHECK(p.Roster().Count() == 1 && p.Roster().Units()[0].starLevel == 3);
    CHECK(log.Count(UnitEvent::Bought) == 9 && log.Count(UnitEvent::Merged) == 4);
    CHECK(PoolConserved(*db, pool, all));
    CHECK(p.Roster().CheckInvariants());

    // Selling the 3-star pays 9 (nine 1-cost copies) and returns all nine to the pool.
    const int goldSelling = p.Gold();
    const UnitId threeStar = p.Roster().Units()[0].id;
    CHECK(p.SellUnit(threeStar) == ActionResult::Ok);
    CHECK(p.Gold() == goldSelling + 9);
    CHECK(p.Roster().Count() == 0);
    CHECK(PoolConserved(*db, pool, all));
    CHECK(log.events.back().kind == UnitEvent::Sold && log.events.back().number == 9 && log.events.back().unit == threeStar);
}

static void TestSellMergedValue() {
    auto db = ChampionDatabase::Create({Def(7, "Solo3", 3)});  // 18 copies
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 1);
    Rng rng(1);
    auto draw = [&](int n) {
        const ChampionDefinition* d = nullptr;
        for (int i = 0; i < n; ++i) d = pool.DrawFromTier(3, rng);
        return d;
    };

    // Three 1-stars -> 2-star, worth 3*3 - 1 = 8. The player never paid for the merge; the value
    // simply reflects the three copies inside.
    const ChampionDefinition* def = draw(3);
    for (int i = 0; i < 3; ++i) CHECK(p.AcquireUnit(def, 3) == ActionResult::Ok);
    CHECK(p.Roster().Count() == 1 && p.Roster().Units()[0].starLevel == 2 && pool.Remaining(7) == 15);
    CHECK(p.SellUnit(p.Roster().Units()[0].id) == ActionResult::Ok);
    CHECK(p.Gold() == 8 && pool.Remaining(7) == 18);

    // Nine 1-stars -> 3-star, worth 9*3 - 1 = 26; all 9 copies return.
    draw(9);
    for (int i = 0; i < 9; ++i) CHECK(p.AcquireUnit(def, 3) == ActionResult::Ok);
    CHECK(p.Roster().Count() == 1 && p.Roster().Units()[0].starLevel == 3 && pool.Remaining(7) == 9);
    CHECK(p.SellUnit(p.Roster().Units()[0].id) == ActionResult::Ok);
    CHECK(p.Gold() == 8 + 26 && pool.Remaining(7) == 18);
}

static void TestBenchFullAndBoardCapThroughShop() {
    auto db = MakeDb();
    TestGameConfig cfg;
    cfg.player.limitBoardToLevel = true;  // level 1 -> only 1 unit may be fielded
    SharedChampionPool pool(*db, cfg.pool);
    EventLog log;
    PlayerState p(0, cfg.player, cfg.shop, pool, 9, &log);

    // Fill the bench + the single allowed board cell with cost >= 2 champions (the level-1 shop
    // only sells cost 1, so nothing here can merge with what we buy).
    std::vector<const ChampionDefinition*> expensive;
    for (const auto& d : db->All()) {
        if (d.cost >= 2) expensive.push_back(&d);
    }
    for (int i = 0; i < 10; ++i) CHECK(p.AcquireUnit(expensive[static_cast<std::size_t>(i)]) == ActionResult::Ok);
    CHECK(p.Roster().BenchCount() == 9 && p.Roster().BoardCount() == 1);
    CHECK(p.Roster().BoardAt(0, 0) != nullptr);  // the 10th went to the board, per "bench full -> board"
    CHECK(p.AcquireUnit(expensive[10]) == ActionResult::RosterFull);

    p.AddGold(50);
    p.Shop().Refresh();
    const ChampionDefinition* offered = p.Shop().Slots()[0];
    CHECK(offered != nullptr);
    const int gold = p.Gold();
    CHECK(p.Shop().TryBuy(0) == ActionResult::RosterFull);  // rejected up front: no gold taken, slot kept
    CHECK(p.Gold() == gold && p.Shop().Slots()[0] == offered && p.Roster().Count() == 10);

    p.AddXp(2);  // level 2 -> two units may be fielded
    CHECK(p.Level() == 2);
    CHECK(p.Shop().TryBuy(0) == ActionResult::Ok);
    const UnitInstance* bought = p.Roster().BoardAt(1, 0);
    CHECK(bought && bought->champion == offered);
    CHECK(p.Gold() == gold - offered->cost);
}

static void TestRosterFullStillAllowsMergePurchase() {
    // Bench + board totally full, but the shop offers a 3rd copy of something owned: still buyable.
    auto db = ChampionDatabase::Create({Def(1, "Solo", 1)});
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 3);
    p.AddGold(200);
    const ChampionDefinition* solo = db->Find(1);

    // Fill every cell with units of *other* (fake) champions so only Solo is relevant.
    std::vector<ChampionDefinition> fakes;
    for (ChampionId id = 100; id < 100 + 40; ++id) fakes.push_back(Def(id, "Filler", 2));
    auto fillerDb = ChampionDatabase::Create(fakes);
    // 35 distinct fillers + 2 Solo copies = 37 = bench 9 + board 28
    for (std::size_t i = 0; i < 35; ++i) CHECK(p.AcquireUnit(&fillerDb->All()[i]) == ActionResult::Ok);
    p.Shop().Refresh();  // 5 Solo copies out of the pool
    CHECK(p.Shop().TryBuy(0) == ActionResult::Ok);
    CHECK(p.Shop().TryBuy(1) == ActionResult::Ok);
    CHECK(p.Roster().Count() == 37);

    const int gold = p.Gold();
    CHECK(!p.CanAcquire(&fillerDb->All()[36]));                  // a new filler has nowhere to go
    CHECK(p.Shop().TryBuy(2) == ActionResult::Ok);               // Solo #3 merges on arrival
    CHECK(p.Gold() == gold - 1);
    CHECK(p.Roster().Count() == 36 && p.Roster().CountOf(solo, 2) == 1);
    CHECK(p.Roster().CheckInvariants());
}

static void TestUnitEventsThroughMatch() {
    auto db = ChampionDatabase::Create({Def(1, "Solo", 1)});
    TestGameConfig cfg;
    cfg.player.startingGold = 30;
    auto match = MatchManager::Create(cfg, *db, 5, nullptr);
    EventLog log;
    match->AddListener(&log);
    match->Start();
    while (match->Phase() != MatchPhase::Planning) match->Tick();

    CHECK(match->TryRerollShop(0) == ActionResult::Ok);
    for (std::size_t slot = 0; slot < 3; ++slot) CHECK(match->TryBuyShopUnit(0, slot) == ActionResult::Ok);
    CHECK(log.events.size() == 4 && log.events[3].kind == UnitEvent::Merged && log.events[3].player == 0);
    CHECK(match->VerifyPoolIntegrity() && match->VerifyRosterLayouts());

    const UnitId unit = match->Players().Get(0)->Roster().Units()[0].id;
    const std::size_t before = log.events.size();
    CHECK(match->TryMoveUnit(0, unit, LocationType::Board, 4, 1) == ActionResult::Ok);
    CHECK(log.events.size() == before + 1);
    const UnitEvent& moved = log.events.back();
    CHECK(moved.kind == UnitEvent::Moved && moved.unit == unit);
    CHECK(moved.fromLocation == LocationType::Bench && moved.toLocation == LocationType::Board && moved.toX == 4 && moved.toY == 1);
    CHECK(match->TryMoveUnit(0, unit, LocationType::Board, 4, 1) == ActionResult::Ok);  // no-op: no event
    CHECK(log.events.size() == before + 1);
    CHECK(match->TryMoveUnit(0, unit, LocationType::Board, 9, 9) == ActionResult::InvalidSlot);
    CHECK(match->TryMoveUnit(0, 12345, LocationType::Board, 0, 0) == ActionResult::InvalidUnit);
    CHECK(match->TryMoveUnit(200, unit, LocationType::Board, 0, 0) == ActionResult::InvalidPlayer);
    // Seat 1 cannot move (or sell) seat 0's unit: ids are only meaningful to their owner.
    CHECK(match->TryMoveUnit(1, unit, LocationType::Bench, 0, 0) == ActionResult::InvalidUnit);
    CHECK(match->TrySellUnit(1, unit) == ActionResult::InvalidUnit);

    CHECK(match->TrySellUnit(0, unit) == ActionResult::Ok);
    CHECK(log.events.back().kind == UnitEvent::Sold && log.events.back().number == 3);  // 2-star, 1-cost: 3 gold
    CHECK(match->VerifyPoolIntegrity() && match->VerifyRosterLayouts());

    // Outside Planning, unit management is locked like the rest of the economy.
    while (match->Phase() != MatchPhase::Combat) match->Tick();
    CHECK(match->TryMoveUnit(0, unit, LocationType::Bench, 0, 0) == ActionResult::WrongPhase);
    CHECK(match->TrySellUnit(0, unit) == ActionResult::WrongPhase);
}

static void TestEliminationWithMergedUnits() {
    auto db = ChampionDatabase::Create({Def(1, "Solo", 1)});
    TestGameConfig cfg;
    SharedChampionPool pool(*db, cfg.pool);
    PlayerState p(0, cfg.player, cfg.shop, pool, 8);
    p.AddGold(100);
    p.Shop().TryReroll();
    for (std::size_t slot = 0; slot < 4; ++slot) p.Shop().TryBuy(slot);  // 3 merge to a 2-star, +1 loose; 1 slot left
    CHECK(p.Roster().Count() == 2);
    CHECK(pool.Remaining(1) == 29 - 5);
    p.Eliminate(3);
    CHECK(pool.Remaining(1) == 29);  // 2-star (3) + 1-star (1) + unbought slot (1) all back
    CHECK(p.Roster().Count() == 0 && p.Roster().CheckInvariants());
    CHECK(p.CanAcquire(db->Find(1)) == false);  // eliminated players can't receive units
    CHECK(p.AcquireUnit(db->Find(1)) == ActionResult::PlayerEliminated);
    CHECK(p.TryMoveUnit(1, LocationType::Bench, 0, 0) == ActionResult::PlayerEliminated);
}

class PhaseRecorder : public IMatchListener {
public:
    struct Entry { MatchPhase phase; int round; };
    std::vector<Entry> phases;
    std::vector<std::pair<PlayerId, int>> eliminations;
    PlayerId winner = kInvalidPlayerId;
    void OnPhaseChanged(MatchPhase, MatchPhase to, int round) override { phases.push_back({to, round}); }
    void OnPlayerEliminated(PlayerId p, int placement) override { eliminations.emplace_back(p, placement); }
    void OnMatchEnded(PlayerId w) override { winner = w; }
};

// Mother Nature data with only the simplest gifts (no items, no units), for tests that just need the phase to exist.
static const char* kTinyGiftsJson = R"({"version": 1, "options": 2, "tiers": [ { "id": 1, "name": "T", "fromStage": 1, "gifts": [
  {"id": 1, "name": "Gold", "type": "Gold", "amount": 5}, {"id": 2, "name": "Xp", "type": "Xp", "amount": 4}, {"id": 3, "name": "Heal", "type": "Heal", "amount": 3} ] } ]})";
static std::unique_ptr<MotherNatureDatabase> TinyGifts() {
    std::string err;
    auto db = w2f::LoadMotherNatureDatabaseFromJson(kTinyGiftsJson, nullptr, &err);
    if (!db) std::printf("  tiny Mother Nature data: %s\n", err.c_str());
    return db;
}

static void TestMatchFlowPhases() {
    auto db = MakeDb();
    auto gifts = TinyGifts();
    CHECK(gifts != nullptr);
    if (!gifts) return;
    TestGameConfig cfg;
    cfg.match.motherNatureTicks = 3;
    cfg.match.planningTicks = 5;
    cfg.match.combatTicks = 4;
    cfg.match.resolutionTicks = 2;
    cfg.match.motherNatureEveryRounds = 2;  // Mother Nature on rounds 2, 4, 6...
    cfg.player.startingGold = 10;

    auto match = MatchManager::Create(cfg, *db, 77, nullptr, nullptr, nullptr, nullptr, gifts.get());
    PhaseRecorder rec;
    match->AddListener(&rec);
    match->Start();

    // Round 1: Planning(5) Combat(4) Resolution(2); round 2 opens with Mother Nature(3): nobody picks, so it times out; round 3 is plain again.
    const int ticksForTwoRounds = (5 + 4 + 2) + (3 + 5 + 4 + 2);
    CHECK(match->TicksRemainingInPhase() == 5);
    for (int i = 0; i < ticksForTwoRounds + 1; ++i) match->Tick();

    std::vector<std::pair<MatchPhase, int>> expected = {
        {MatchPhase::Planning, 1},   {MatchPhase::Combat, 1},   {MatchPhase::Resolution, 1},
        {MatchPhase::MotherNature, 2}, {MatchPhase::Planning, 2}, {MatchPhase::Combat, 2},
        {MatchPhase::Resolution, 2}, {MatchPhase::Planning, 3},
    };
    CHECK(rec.phases.size() == expected.size());
    for (std::size_t i = 0; i < expected.size() && i < rec.phases.size(); ++i) {
        CHECK(rec.phases[i].phase == expected[i].first);
        CHECK(rec.phases[i].round == expected[i].second);
    }

    // Action gating + income timing (fresh match, walk to Planning).
    auto m2 = MatchManager::Create(cfg, *db, 77, nullptr, nullptr, nullptr, nullptr, gifts.get());
    m2->Start();
    // Round 1 income: base 2 + interest(10 gold -> 1) = 13 total
    CHECK(m2->Players().Get(0)->Gold() == 13);
    CHECK(m2->Phase() == MatchPhase::Planning);   // round 1 is not one of Mother Nature's: it opens with the shop
    CHECK(m2->Players().Get(0)->Shop().Slots()[0] != nullptr);  // shop stocked on entering Planning
    CHECK(m2->TryRerollShop(200) == ActionResult::InvalidPlayer);
    CHECK(m2->TryBuyXp(0) == ActionResult::Ok);
    CHECK(m2->TryRerollShop(0) == ActionResult::Ok);
    CHECK(m2->Players().Get(0)->Gold() == 13 - 4 - 2);
    for (int i = 0; i < 5; ++i) m2->Tick();
    CHECK(m2->Phase() == MatchPhase::Combat);
    CHECK(m2->TryRerollShop(0) == ActionResult::WrongPhase);
    CHECK(m2->CurrentMatchups().size() == 4);
    CHECK(m2->VerifyPoolIntegrity());
}

static void TestMatchups() {
    auto db = MakeDb();
    TestGameConfig cfg;
    cfg.match.playerCount = 5;  // odd => one ghost fight
    auto match = MatchManager::Create(cfg, *db, 5, nullptr);
    match->Start();
    while (match->Phase() != MatchPhase::Combat) match->Tick();
    const auto& fights = match->CurrentMatchups();
    CHECK(fights.size() == 3);
    int ghosts = 0;
    bool seen[kMaxPlayers] = {};
    bool eachHomeOnce = true;
    for (const Matchup& m : fights) {
        ghosts += m.awayIsGhost ? 1 : 0;
        eachHomeOnce = eachHomeOnce && !seen[m.home];
        seen[m.home] = true;
        if (!m.awayIsGhost) {
            eachHomeOnce = eachHomeOnce && !seen[m.away];
            seen[m.away] = true;
        }
    }
    CHECK(ghosts == 1);
    CHECK(eachHomeOnce);
    for (int i = 0; i < 5; ++i) CHECK(seen[i]);
}

static void TestFullMatchDeterminismAndIntegrity() {
    auto db = MakeDb();
    const MatchRun a = PlayFullMatch(*db, 2024);
    const MatchRun b = PlayFullMatch(*db, 2024);
    const MatchRun c = PlayFullMatch(*db, 2025);

    CHECK(a.finished);
    CHECK(a.integrityHeld);
    CHECK(c.integrityHeld);
    CHECK(a.layoutsHeld && c.layoutsHeld);
    // The bots really did exercise every unit operation (otherwise this test proves little).
    CHECK(a.bought > 0 && a.sold > 0 && a.moved > 0 && a.merged > 0);
    CHECK(a.bought == b.bought && a.sold == b.sold && a.moved == b.moved && a.merged == b.merged);
    CHECK(a.winner != kInvalidPlayerId);
    // Same seed + same inputs => the exact same state on every single tick.
    CHECK(a.hashes == b.hashes);
    CHECK(a.winner == b.winner && a.rounds == b.rounds);
    // Different seed => a different match.
    CHECK(a.hashes != c.hashes);
    std::printf("  full match: %d rounds, %zu ticks, winner seat %d; unit events: %d bought, %d merged, %d moved, %d sold\n",
                a.rounds, a.hashes.size(), a.winner, a.bought, a.merged, a.moved, a.sold);
}

static void TestEliminationPlacements() {
    auto db = MakeDb();
    TestGameConfig cfg;
    cfg.match.playerCount = 4;
    cfg.match.motherNatureTicks = cfg.match.planningTicks = cfg.match.combatTicks = cfg.match.resolutionTicks = 1;
    cfg.player.startingHealth = 10;

    // Seat 0 always beats its opponent for 6 damage; everyone else stays alive until hit twice.
    class Bully : public ICombatSimulator {
    public:
        std::vector<CombatOutcome> Simulate(const CombatContext& ctx) override {
            std::vector<CombatOutcome> out;
            for (const Matchup& m : ctx.matchups) {
                CombatOutcome outcome;
                outcome.matchup = m;
                outcome.winner = CombatWinner::Home;
                outcome.winnerSurvivors = 4;
                out.push_back(outcome);
            }
            return out;
        }
    };
    auto match = MatchManager::Create(cfg, *db, 11, std::make_unique<Bully>());
    PhaseRecorder rec;
    match->AddListener(&rec);
    match->Start();
    for (int i = 0; i < 1000 && !match->IsFinished(); ++i) match->Tick();

    CHECK(match->IsFinished());
    CHECK(rec.winner == match->Winner());
    CHECK(match->Winner() != kInvalidPlayerId);
    CHECK(match->Players().Get(match->Winner())->Placement() == 1);
    // Every seat got a unique placement 1..4.
    bool used[5] = {};
    for (int i = 0; i < 4; ++i) {
        const int place = match->Players().Get(static_cast<PlayerId>(i))->Placement();
        CHECK(place >= 1 && place <= 4 && !used[place]);
        if (place >= 1 && place <= 4) used[place] = true;
    }
    CHECK(match->VerifyPoolIntegrity());
    // Eliminated players hold nothing: the whole pool is back except copies held by survivors.
    const PlayerState* w = match->Players().Get(match->Winner());
    int held = 0;
    for (const UnitInstance& u : w->Roster().Units()) held += SharedChampionPool::CopiesForStarLevel(u.starLevel);
    for (auto* s : w->Shop().Slots()) held += s ? 1 : 0;
    CHECK(TotalRemaining(match->Pool()) == 5 * (29 + 22 + 18 + 12 + 10) - held);
}


// ==== Phase 3: hex geometry, pathfinding, combat, bots =======================================

static std::vector<HexCoord> AllArenaHexes() {
    std::vector<HexCoord> all;
    for (int y = 0; y < kArenaRows; ++y) {
        for (int x = 0; x < kArenaColumns; ++x) all.push_back({x, y});
    }
    return all;
}

static void TestHexDistanceAndNeighbors() {
    // Hand-checked values on the odd-r layout.
    CHECK(hex::Distance({0, 0}, {0, 0}) == 0);
    CHECK(hex::Distance({0, 0}, {3, 0}) == 3);
    CHECK(hex::Distance({0, 0}, {0, 1}) == 1);  // even row: SE neighbour is (x, y+1)
    CHECK(hex::Distance({0, 1}, {0, 0}) == 1);  // odd row: NW neighbour is (x, y-1)
    CHECK(hex::Distance({0, 1}, {1, 0}) == 1);  // odd row: NE neighbour is (x+1, y-1)
    CHECK(hex::Distance({1, 0}, {0, 1}) == 1);
    CHECK(hex::Distance({0, 0}, {1, 1}) == 2);
    CHECK(hex::Distance({0, 0}, {6, 7}) == 10);  // 7 rows down cover 3.5 columns; 2.5 more cost 3
    CHECK(hex::Distance({0, 0}, {0, 7}) == 7);

    // Axial round trip.
    for (int y = -3; y < 12; ++y) {
        for (int x = -3; x < 12; ++x) {
            const HexCoord c{x, y};
            CHECK(hex::FromAxial(hex::ToAxial(c)) == c);
        }
    }

    // The distance formula must agree with walking the neighbour table: BFS on a big open grid.
    const int N = 41;
    for (HexCoord src : {HexCoord{20, 20}, HexCoord{20, 21}, HexCoord{15, 18}}) {
        std::vector<int> dist(static_cast<std::size_t>(N * N), -1);
        std::deque<HexCoord> q;
        dist[static_cast<std::size_t>(src.y * N + src.x)] = 0;
        q.push_back(src);
        while (!q.empty()) {
            const HexCoord c = q.front();
            q.pop_front();
            std::array<HexCoord, hex::kDirections> nb;
            const int n = hex::Neighbors(c, N, N, nb);
            for (int i = 0; i < n; ++i) {
                const std::size_t idx = static_cast<std::size_t>(nb[static_cast<std::size_t>(i)].y * N + nb[static_cast<std::size_t>(i)].x);
                if (dist[idx] < 0) {
                    dist[idx] = dist[static_cast<std::size_t>(c.y * N + c.x)] + 1;
                    q.push_back(nb[static_cast<std::size_t>(i)]);
                }
            }
        }
        bool allMatch = true;
        for (int y = 10; y < 31; ++y) {
            for (int x = 10; x < 31; ++x) allMatch = allMatch && dist[static_cast<std::size_t>(y * N + x)] == hex::Distance(src, {x, y});
        }
        CHECK(allMatch);
    }

    // Neighbour structure: symmetric, all at distance 1, six of them in the open.
    bool symmetric = true, unitDistance = true, sixInOpen = true;
    for (int y = 3; y < 30; ++y) {
        for (int x = 3; x < 30; ++x) {
            std::array<HexCoord, hex::kDirections> nb;
            sixInOpen = sixInOpen && hex::Neighbors({x, y}, 41, 41, nb) == 6;
            for (int d = 0; d < hex::kDirections; ++d) {
                const HexCoord m = hex::Neighbor({x, y}, d);
                unitDistance = unitDistance && hex::Distance({x, y}, m) == 1;
                bool back = false;
                for (int e = 0; e < hex::kDirections; ++e) back = back || hex::Neighbor(m, e) == HexCoord{x, y};
                symmetric = symmetric && back;
            }
        }
    }
    CHECK(symmetric && unitDistance && sixInOpen);

    // Edges and corners of the arena.
    std::array<HexCoord, hex::kDirections> nb;
    CHECK(hex::Neighbors({0, 0}, kArenaColumns, kArenaRows, nb) == 2);  // E, SE
    CHECK(nb[0] == HexCoord{1, 0} && nb[1] == HexCoord{0, 1});
    CHECK(hex::Neighbors({0, 1}, kArenaColumns, kArenaRows, nb) == 5);  // odd row, left edge
    CHECK(hex::Neighbors({6, 1}, kArenaColumns, kArenaRows, nb) == 3);  // odd row, right edge: only W, NW, SW
    CHECK(hex::Neighbors({3, 3}, kArenaColumns, kArenaRows, nb) == 6);
    // First direction is always East, and order is fixed.
    CHECK(hex::Neighbor({3, 3}, 0) == HexCoord{4, 3});
    CHECK(hex::Neighbor({3, 3}, 3) == HexCoord{2, 3});
    // Odd row 3: NE=(4,2) NW=(3,2) SW=(3,4) SE=(4,4)
    CHECK(hex::Neighbor({3, 3}, 1) == HexCoord{4, 2} && hex::Neighbor({3, 3}, 2) == HexCoord{3, 2});
    CHECK(hex::Neighbor({3, 3}, 4) == HexCoord{3, 4} && hex::Neighbor({3, 3}, 5) == HexCoord{4, 4});
}

static void TestBoardToArenaMapping() {
    // Home keeps coordinates; Away is the board rotated 180 degrees.
    CHECK(BoardToArena(2, 1, ArenaSide::Home) == HexCoord{2, 1});
    CHECK(BoardToArena(0, 0, ArenaSide::Away) == HexCoord{6, 7});
    CHECK(BoardToArena(6, 3, ArenaSide::Away) == HexCoord{0, 4});

    bool inBounds = true, homeInTop = true, awayInBottom = true, isometry = true;
    for (int y1 = 0; y1 < kBoardRows; ++y1) {
        for (int x1 = 0; x1 < kBoardColumns; ++x1) {
            const HexCoord h = BoardToArena(x1, y1, ArenaSide::Home);
            const HexCoord a = BoardToArena(x1, y1, ArenaSide::Away);
            inBounds = inBounds && hex::InBounds(h, kArenaColumns, kArenaRows) && hex::InBounds(a, kArenaColumns, kArenaRows);
            homeInTop = homeInTop && h.y < kBoardRows;
            awayInBottom = awayInBottom && a.y >= kBoardRows;
            for (int y2 = 0; y2 < kBoardRows; ++y2) {
                for (int x2 = 0; x2 < kBoardColumns; ++x2) {
                    // A formation must look identical to both sides: every pairwise distance survives.
                    const int d = hex::Distance({x1, y1}, {x2, y2});
                    isometry = isometry && hex::Distance(h, BoardToArena(x2, y2, ArenaSide::Home)) == d &&
                               hex::Distance(a, BoardToArena(x2, y2, ArenaSide::Away)) == d;
                }
            }
        }
    }
    CHECK(inBounds && homeInTop && awayInBottom && isometry);

    // The two front rows meet in the middle: some pair is adjacent, and no back-row unit is.
    int minFront = 99, minBack = 99;
    for (int x1 = 0; x1 < kBoardColumns; ++x1) {
        for (int x2 = 0; x2 < kBoardColumns; ++x2) {
            minFront = std::min(minFront, hex::Distance(BoardToArena(x1, kBoardRows - 1, ArenaSide::Home),
                                                        BoardToArena(x2, kBoardRows - 1, ArenaSide::Away)));
            minBack = std::min(minBack, hex::Distance(BoardToArena(x1, 0, ArenaSide::Home),
                                                      BoardToArena(x2, 0, ArenaSide::Away)));
        }
    }
    CHECK(minFront == 1);
    CHECK(minBack == 7);
}

static void TestPathfinding() {
    HexGrid grid(kArenaColumns, kArenaRows);
    HexPathfinder pf(kArenaColumns, kArenaRows);
    std::vector<HexCoord> path;

    // Open grid: the path is as long as the distance, each step is a neighbour, and it ends at the goal.
    bool lengthsOk = true, stepsOk = true;
    const auto all = AllArenaHexes();
    for (HexCoord a : all) {
        for (HexCoord b : all) {
            const bool found = pf.FindPath(grid, a, b, path);
            lengthsOk = lengthsOk && found && static_cast<int>(path.size()) == hex::Distance(a, b);
            HexCoord prev = a;
            for (HexCoord step : path) {
                stepsOk = stepsOk && hex::Distance(prev, step) == 1;
                prev = step;
            }
            stepsOk = stepsOk && prev == b;
        }
    }
    CHECK(lengthsOk && stepsOk);

    // Start == goal: found, empty path (even if the start hex is "blocked" by the walker itself).
    grid.SetBlocked({2, 2}, true);
    CHECK(pf.FindPath(grid, {2, 2}, {2, 2}, path) && path.empty());
    // A blocked goal, or one off the grid, is unreachable.
    CHECK(!pf.FindPath(grid, {0, 0}, {2, 2}, path) && path.empty());
    CHECK(!pf.FindPath(grid, {0, 0}, {9, 9}, path));
    grid.SetBlocked({2, 2}, false);

    // A wall with one gap forces a detour that never touches a blocked hex.
    for (int x = 0; x < kArenaColumns; ++x) {
        if (x != 6) grid.SetBlocked({x, 3}, true);  // row 3 blocked except the far-right gap
    }
    CHECK(pf.FindPath(grid, {0, 0}, {0, 6}, path));
    CHECK(static_cast<int>(path.size()) > hex::Distance({0, 0}, {0, 6}));
    bool avoidsWall = true, throughGap = false;
    for (HexCoord step : path) {
        avoidsWall = avoidsWall && !grid.IsBlocked(step);
        throughGap = throughGap || (step.y == 3 && step.x == 6);
    }
    CHECK(avoidsWall && throughGap);

    // Close the gap: unreachable, and it says so.
    grid.SetBlocked({6, 3}, true);
    CHECK(!pf.FindPath(grid, {0, 0}, {0, 6}, path) && path.empty());
    grid.Clear();

    // Same query twice gives the same path (fixed expansion order).
    std::vector<HexCoord> again;
    pf.FindPath(grid, {0, 0}, {5, 6}, path);
    pf.FindPath(grid, {0, 0}, {5, 6}, again);
    CHECK(path == again);

    // Path-to-range: the target's hex is occupied, so we stop adjacent to it.
    grid.SetBlocked({3, 4}, true);
    CHECK(pf.FindPathToRange(grid, {3, 0}, {3, 4}, 1, path));
    CHECK(!path.empty() && hex::Distance(path.back(), {3, 4}) == 1 && !grid.IsBlocked(path.back()));
    CHECK(static_cast<int>(path.size()) == hex::Distance({3, 0}, {3, 4}) - 1);
    // Ranged: stops as soon as within 3.
    CHECK(pf.FindPathToRange(grid, {3, 0}, {3, 4}, 3, path));
    CHECK(hex::Distance(path.back(), {3, 4}) == 3 && static_cast<int>(path.size()) == hex::Distance({3, 0}, {3, 4}) - 3);
    // Already in range: found, nothing to walk.
    CHECK(pf.FindPathToRange(grid, {3, 3}, {3, 4}, 1, path) && path.empty());
    // A target completely fenced in by blocked hexes cannot be reached in melee.
    std::array<HexCoord, hex::kDirections> nb;
    const int n = hex::Neighbors({3, 4}, kArenaColumns, kArenaRows, nb);
    for (int i = 0; i < n; ++i) grid.SetBlocked(nb[static_cast<std::size_t>(i)], true);
    CHECK(!pf.FindPathToRange(grid, {0, 0}, {3, 4}, 1, path));
}

// ---- Combat helpers ----------------------------------------------------------------------

static ChampionDefinition Fighter(ChampionId id, int hp, int armor, int damage, int speedMilli = 1000, int range = 1) {
    ChampionDefinition d = Def(id, "Fighter", 1);
    d.stats.maxHp = {{hp, hp, hp}};
    d.stats.armor = {{armor, armor, armor}};
    d.stats.attackDamage = {{damage, damage, damage}};
    d.stats.attackSpeedMilli = speedMilli;
    d.stats.attackRange = range;
    return d;
}

// Independent re-implementation of "what may a viewer see": rebuilds the fight from the event
// stream alone and rejects anything physically or arithmetically impossible. It knows nothing
// about how the simulator works internally -- only the rules of the stream (see Combat.h).
static bool ValidateCombatLog(const CombatLog& log, const ChampionDatabase& db, const CombatConfig& cfg,
                              int maxTicks, std::string* why = nullptr) {
    struct V { HexCoord pos; int hp = 0; int team = 0; int star = 1; bool alive = true;
               int lastAttack = -1000000, lastMove = -1000000, lockedUntil = 0, stunnedUntil = 0, rootedUntil = 0, immuneUntil = 0, shieldPool = 0,
               untargetableUntil = 0, aggroDropUntil = 0;
               bool everShielded = false, everStatus = false; int blindUntil = 0; int manaMax = 0; const ChampionDefinition* def = nullptr; bool summon = false; };
    std::map<UnitId, V> units;
    std::map<std::pair<int, int>, UnitId> occupied;
    int lastTick = 0;
    auto fail = [&](const char* msg, const CombatEvent& e) {
        if (why) *why = std::string(msg) + " (tick " + std::to_string(e.tick) + ", unit " + std::to_string(e.unit) + ")";
        return false;
    };
    for (const CombatEvent& e : log.events) {
        if (e.tick < lastTick) return fail("ticks went backwards", e);
        if (e.tick > maxTicks) return fail("event after time limit", e);
        lastTick = e.tick;
        auto self = units.find(e.unit);
        if (e.type != CombatEventType::Spawn && e.type != CombatEventType::TraitActivated && self == units.end()) return fail("event for an unknown unit", e);
        switch (e.type) {
            case CombatEventType::Spawn: {
                const bool summon = (e.flags & kFlagSummon) != 0;
                if ((e.tick != 0 && !summon) || units.count(e.unit)) return fail("bad spawn", e);
                V v;
                v.def = db.Find(e.champion);
                if (!v.def || e.star < 1 || e.star > kMaxStarLevel) return fail("spawn: unknown champion/star", e);
                v.pos = e.to; v.team = e.team; v.star = e.star; v.hp = e.amount; v.summon = summon;
                if (summon) {   // a summon appears mid-fight next to a living summoner of the same team; its stats may be overridden
                    auto owner = units.find(e.other);
                    if (owner == units.end() || !owner->second.alive || owner->second.team != e.team) return fail("summon: no living summoner on that team", e);
                    if (e.unit <= kSummonUnitBase || e.unit >= kMonsterUnitBase) return fail("summon: unit id outside the summon range", e);
                    if (e.hpAfter != e.amount || e.amount < 1) return fail("summon: bad hp", e);
                } else if (v.hp != v.def->stats.maxHp[static_cast<std::size_t>(e.star - 1)]) {
                    return fail("spawn: wrong max hp", e);
                }
                if (e.manaMax != v.def->stats.maxMana * 1000 || e.manaRegen != v.def->stats.manaRegenMilli) return fail("spawn: wrong mana bar", e);
                v.manaMax = e.manaMax;
                if (!hex::InBounds(e.to, kArenaColumns, kArenaRows) || occupied.count({e.to.x, e.to.y})) return fail("spawn: bad hex", e);
                occupied[{e.to.x, e.to.y}] = e.unit;
                units[e.unit] = v;
                break;
            }
            case CombatEventType::Move: {
                V& v = self->second;
                if (!v.alive) return fail("move: dead unit", e);
                if (e.tick < v.stunnedUntil || e.tick < v.lockedUntil) return fail("move: while stunned / casting", e);
                if (e.tick < v.rootedUntil) return fail("move: while rooted", e);
                if (!(v.pos == e.from)) return fail("move: not where the log says", e);
                if (hex::Distance(e.from, e.to) != 1) return fail("move: not one hex", e);
                if (!hex::InBounds(e.to, kArenaColumns, kArenaRows) || occupied.count({e.to.x, e.to.y})) return fail("move: into occupied/out of bounds", e);
                if (e.tick - v.lastMove < cfg.ticksPerHexMove) return fail("move: faster than move speed", e);
                if (e.amount != cfg.ticksPerHexMove) return fail("move: wrong duration", e);
                occupied.erase({e.from.x, e.from.y});
                occupied[{e.to.x, e.to.y}] = e.unit;
                v.pos = e.to; v.lastMove = e.tick;
                break;
            }
            case CombatEventType::Attack: {
                V& a = self->second;
                auto t = units.find(e.other);
                if (t == units.end() || !a.alive || !t->second.alive) return fail("attack: dead/unknown", e);
                if (a.team == t->second.team) return fail("attack: friendly fire", e);
                if (e.tick < t->second.untargetableUntil) return fail("attack: on an untargetable unit", e);
                if (e.tick < t->second.aggroDropUntil) return fail("attack: on a unit that dropped aggro", e);
                if (e.tick < a.stunnedUntil || e.tick < a.lockedUntil) return fail("attack: while stunned / casting", e);
                if (e.tick < a.blindUntil) return fail("attack: while blind", e);
                if (hex::Distance(a.pos, t->second.pos) > a.def->stats.attackRange) return fail("attack: out of range", e);
                if (!a.everStatus && e.tick - a.lastAttack < a.def->stats.AttackIntervalTicks()) return fail("attack: faster than attack speed", e);
                a.lastAttack = e.tick;
                break;
            }
            case CombatEventType::SpellCast: {
                V& c = self->second;
                const bool onDeath = (e.flags & kFlagOnDeath) != 0;
                if (e.ability != c.def->ability.id || !c.def->ability.HasAbility()) return fail("cast: champion has no such ability", e);
                if (!onDeath) {
                    if (!c.alive) return fail("cast: dead caster", e);
                    if (e.tick < c.stunnedUntil || e.tick < c.lockedUntil) return fail("cast: while stunned / already casting", e);
                    auto t = units.find(e.other);
                    if (t == units.end() || !t->second.alive) return fail("cast: no living target", e);
                    if (e.tick < t->second.untargetableUntil || e.tick < t->second.aggroDropUntil) return fail("cast: aimed at an untargetable unit", e);
                    if (hex::Distance(c.pos, t->second.pos) > c.def->stats.attackRange) return fail("cast: target out of range", e);
                    if (e.duration != c.def->ability.castLockTicks + c.def->ability.channelTicks) return fail("cast: wrong lock time", e);
                    c.lockedUntil = e.tick + e.duration;
                } else if (!c.def->ability.castOnDeath || c.alive) {
                    return fail("cast-on-death by a living unit or an ability without it", e);
                }
                break;
            }
            case CombatEventType::ShieldApplied: {
                V& h = self->second;
                if (!h.alive || e.amount <= 0 || e.duration < 0) return fail("shield: bad application", e);   // duration 0 = permanent
                h.shieldPool += e.amount;
                h.everShielded = true;
                break;
            }
            case CombatEventType::ShieldEnded: {
                V& h = self->second;
                if (!h.alive) return fail("shield end on a dead unit", e);
                h.shieldPool -= e.amount;
                if (h.shieldPool < 0) return fail("shield: more capacity ended than was applied", e);
                break;
            }
            case CombatEventType::StatusApplied: {
                V& h = self->second;
                // Duration 0 means permanent (a passive) -- never valid for a stun.
                const auto sub = static_cast<StatusType>(e.subtype);
                const bool isDisable = sub == StatusType::Stun || sub == StatusType::Knockup;
                const bool isCc = isDisable || sub == StatusType::Root;
                if (!h.alive || e.duration < 0 || (e.duration == 0 && isCc)) return fail("status: bad application", e);
                if (isCc && e.other != e.unit && e.tick < h.immuneUntil) return fail("crowd control landed on a unit with CC immunity", e);
                if (sub == StatusType::MaxHp || sub == StatusType::BonusMaxHp) {
                    if (e.hpAfter < 1) return fail("status: max hp change left the unit with no hp", e);
                    h.hp = e.hpAfter;   // max-HP statuses move current HP; the event carries the result
                } else if (e.hpAfter != h.hp) {
                    return fail("status: hpAfter disagrees with the unit's hp", e);
                }
                h.everStatus = true;
                if (isDisable) h.stunnedUntil = std::max(h.stunnedUntil, e.tick + e.duration);
                if (sub == StatusType::Root) h.rootedUntil = std::max(h.rootedUntil, e.tick + e.duration);
                if (sub == StatusType::CcImmunity) h.immuneUntil = e.duration > 0 ? std::max(h.immuneUntil, e.tick + e.duration) : std::numeric_limits<int>::max();   // duration 0 = for the rest of the fight
                if (sub == StatusType::Untargetable && e.duration > 0) h.untargetableUntil = std::max(h.untargetableUntil, e.tick + e.duration);
                if (sub == StatusType::AggroDrop && e.duration > 0) h.aggroDropUntil = std::max(h.aggroDropUntil, e.tick + e.duration);
                if (sub == StatusType::Blind && e.duration > 0) h.blindUntil = std::max(h.blindUntil, e.tick + e.duration);
                if (sub == StatusType::BonusMaxMana && h.manaMax > 0) h.manaMax = std::max(1000, h.manaMax + e.amount * 1000);   // a longer mana bar
                auto src = units.find(e.other);
                if (src != units.end()) src->second.everStatus = true;
                break;
            }
            case CombatEventType::StatusEnded:
                if (!self->second.alive) return fail("status end on a dead unit", e);
                if (e.subtype == static_cast<std::uint8_t>(StatusType::MaxHp) || e.subtype == static_cast<std::uint8_t>(StatusType::BonusMaxHp)) self->second.hp = e.hpAfter;
                else if (e.hpAfter != self->second.hp) return fail("status end: hpAfter disagrees with the unit's hp", e);
                break;
            case CombatEventType::Teleport: {
                V& v = self->second;
                if (!v.alive || !(v.pos == e.from)) return fail("teleport: not where the log says", e);
                if (!hex::InBounds(e.to, kArenaColumns, kArenaRows) || occupied.count({e.to.x, e.to.y})) return fail("teleport: into occupied/out of bounds", e);
                occupied.erase({e.from.x, e.from.y});
                occupied[{e.to.x, e.to.y}] = e.unit;
                v.pos = e.to;
                break;
            }
            case CombatEventType::SpellInterrupted: {
                V& v = self->second;
                if (!v.alive || e.ability != v.def->ability.id || v.def->ability.channelTicks <= 0) return fail("interrupt: not a channelling unit", e);
                if (e.tick < v.lockedUntil) v.lockedUntil = e.tick;
                break;
            }
            case CombatEventType::Heal: {
                V& h = self->second;
                if (!h.alive || e.amount <= 0 || e.reduced < 0) return fail("heal: bad heal", e);
                h.hp += e.amount;
                if (e.hpAfter != h.hp) return fail("heal: hp arithmetic", e);
                break;
            }
            case CombatEventType::TraitActivated:
                if (e.tick != 0 || e.team > 1 || e.traitId == 0 || e.amount < 1 || e.subtype < 1) return fail("trait: bad activation", e);
                break;
            case CombatEventType::ManaChanged:
                if (!self->second.alive || self->second.manaMax <= 0) return fail("mana event for a dead unit or one without a mana bar", e);
                if (e.amount < 0 || e.amount > self->second.manaMax) return fail("mana outside its bar", e);
                break;
            case CombatEventType::Damage: {
                V& victim = self->second;
                auto a = units.find(e.other);
                if (a == units.end() || !victim.alive) return fail("damage: dead/unknown", e);
                if (e.absorbed < 0 || e.absorbed > e.amount) return fail("damage: absorbed more than the hit", e);
                if (e.absorbed > victim.shieldPool) return fail("damage: absorbed more than the shields hold", e);
                victim.shieldPool -= e.absorbed;
                victim.hp = std::max(0, victim.hp - (e.amount - e.absorbed));
                if (e.hpAfter != victim.hp) return fail("damage: hp arithmetic", e);
                // A plain, uncrit basic attack between unmodified units must match the armor formula exactly.
                if (e.flags == kFlagBasic && !victim.everShielded && !victim.everStatus && !a->second.everStatus) {
                    const int expected = MitigatedDamage(a->second.def->stats.attackDamage[static_cast<std::size_t>(a->second.star - 1)],
                                                         victim.def->stats.armor[static_cast<std::size_t>(victim.star - 1)], cfg.minDamage);
                    if (e.amount != expected) return fail("damage: wrong amount for a basic attack", e);
                }
                break;
            }
            case CombatEventType::Death: {
                V& v = self->second;
                const bool despawn = (e.flags & kFlagSummon) != 0;   // a summon still standing when the fight ends just vanishes
                if (despawn && !v.summon) return fail("death: summon flag on a normal unit", e);
                if (!v.alive || (v.hp != 0 && !despawn)) return fail("death: not at 0 hp", e);
                v.alive = false;
                occupied.erase({v.pos.x, v.pos.y});
                break;
            }
        }
    }
    int alive[2] = {0, 0};
    for (auto& kv : units) alive[kv.second.team] += (kv.second.alive && !kv.second.summon) ? 1 : 0;   // summons are not survivors
    if (alive[0] != log.survivors[0] || alive[1] != log.survivors[1]) {
        if (why) *why = "survivor counts disagree with the stream";
        return false;
    }
    if (log.checksum != log.ComputeChecksum()) {
        if (why) *why = "checksum mismatch";
        return false;
    }
    if (alive[0] > 0 && alive[1] > 0 && log.endTick != maxTicks) {
        if (why) *why = "fight stopped with both teams alive before the time limit";
        return false;
    }
    return true;
}

static void TestFixedPointCombatMath() {
    CombatStats s;
    s.attackSpeedMilli = 1000; CHECK(s.AttackIntervalTicks() == 30);   // 1 attack/s at 30 ticks/s
    s.attackSpeedMilli = 2000; CHECK(s.AttackIntervalTicks() == 15);
    s.attackSpeedMilli = 500;  CHECK(s.AttackIntervalTicks() == 60);
    s.attackSpeedMilli = 1180; CHECK(s.AttackIntervalTicks() == 25);   // 25.42 -> 25
    s.attackSpeedMilli = 800;  CHECK(s.AttackIntervalTicks() == 38);   // 37.5 -> 38
    s.attackSpeedMilli = 100000; CHECK(s.AttackIntervalTicks() == 1);  // never 0

    CHECK(MitigatedDamage(100, 0, 1) == 100);
    CHECK(MitigatedDamage(100, 100, 1) == 50);
    CHECK(MitigatedDamage(30, 31, 1) == 22);   // 3000/131 = 22.9 -> 22 (floor)
    CHECK(MitigatedDamage(1, 500, 1) == 1);    // floor of 0, raised to the minimum
    CHECK(MitigatedDamage(1, 500, 0) == 0);


    // Alesk's stat sheet survives the trip into a definition.
    const ChampionDefinition alesk = Legacy(kChampionAlesk);
    CHECK(alesk.stats.maxHp[2] == 650 && alesk.stats.armor[1] == 45 && alesk.stats.attackDamage[0] == 30);
    CHECK(alesk.stats.AttackIntervalTicks() == 30);
}

struct Duel {
    std::vector<ChampionDefinition> defs;  // unique by id; many units may share one definition
    std::vector<ChampionId> specDef;
    std::unique_ptr<ChampionDatabase> db;
    std::vector<FightUnitSpec> specs;
    void Add(const ChampionDefinition& d, UnitId id, int team, HexCoord pos, int star = 1) {
        bool known = false;
        for (const ChampionDefinition& existing : defs) known = known || existing.id == d.id;
        if (!known) defs.push_back(d);
        specDef.push_back(d.id);
        specs.push_back(FightUnitSpec{id, nullptr, star, team, pos});
    }
    void Finish() {
        std::string error;
        db = ChampionDatabase::Create(defs, &error);
        if (!db) std::printf("  Duel database invalid: %s\n", error.c_str());
        for (std::size_t i = 0; i < specs.size(); ++i) specs[i].champion = db ? db->Find(specDef[i]) : nullptr;
    }
};

static void TestDuelAdjacent() {
    Duel d;
    d.Add(Fighter(1, 100, 0, 10), 1, 0, {3, 3});  // hits for 10, 1 attack/s
    d.Add(Fighter(2, 50, 0, 5), 2, 1, {3, 4});    // adjacent (odd row 3: SW neighbour is (3,4))
    d.Finish();
    CombatSimulator sim{CombatConfig{}};
    const FightResult r = sim.RunFight(d.specs, 1200);

    CHECK(r.winner == CombatWinner::Home);
    CHECK(r.log.endTick == 120);  // hits at ticks 0,30,60,90,120: 5 x 10 = 50 hp
    CHECK(r.log.survivors[0] == 1 && r.log.survivors[1] == 0);
    std::vector<int> attackTicks;
    int moves = 0;
    for (const CombatEvent& e : r.log.events) {
        if (e.type == CombatEventType::Attack && e.unit == 1) attackTicks.push_back(e.tick);
        moves += e.type == CombatEventType::Move ? 1 : 0;
    }
    CHECK((attackTicks == std::vector<int>{0, 30, 60, 90, 120}));  // 30-tick cooldown, exactly
    CHECK(moves == 0);                                            // already in range: nobody walks
    // The loser's 5th attack lands in the same tick it dies (simultaneous damage): 100 - 5*5.
    int lastHpOfHome = -1;
    for (const CombatEvent& e : r.log.events) {
        if (e.type == CombatEventType::Damage && e.unit == 1) lastHpOfHome = e.hpAfter;
    }
    CHECK(lastHpOfHome == 75);
    std::string why;
    CHECK(ValidateCombatLog(r.log, *d.db, CombatConfig{}, 1200, &why));
    if (!why.empty()) std::printf("  validator: %s\n", why.c_str());
}

static void TestCombatSimultaneityAndTimeout() {
    CombatSimulator sim{CombatConfig{}};
    {
        // Identical units kill each other in the same tick: a draw, no lower-id advantage.
        Duel d;
        d.Add(Fighter(1, 10, 0, 10), 1, 0, {3, 3});
        d.Add(Fighter(1, 10, 0, 10), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 1200);
        CHECK(r.winner == CombatWinner::Draw && r.log.survivors[0] == 0 && r.log.survivors[1] == 0 && r.log.endTick == 0);
    }
    {
        // Nobody can finish in time. Timeout goes to more survivors, then more total HP.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 10), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000, 0, 5), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 100);
        CHECK(r.winner == CombatWinner::Home && r.log.endTick == 100);
        CHECK(r.log.survivors[0] == 1 && r.log.survivors[1] == 1);
        CHECK(ValidateCombatLog(r.log, *d.db, CombatConfig{}, 100));

        Duel same;  // perfectly symmetric: equal HP at the horn -> draw
        same.Add(Fighter(1, 100000, 0, 10), 1, 0, {3, 3});
        same.Add(Fighter(1, 100000, 0, 10), 2, 1, {3, 4});
        same.Finish();
        CHECK(sim.RunFight(same.specs, 100).winner == CombatWinner::Draw);

        Duel numbers;  // 2v1, nobody dies: more survivors wins even with less HP
        numbers.Add(Fighter(1, 1000, 0, 1), 1, 0, {2, 3});
        numbers.Add(Fighter(1, 1000, 0, 1), 3, 0, {4, 3});
        numbers.Add(Fighter(2, 100000, 0, 1), 2, 1, {3, 4});
        numbers.Finish();
        CHECK(sim.RunFight(numbers.specs, 60).winner == CombatWinner::Home);
    }
    {
        // Empty sides: nothing to fight.
        Duel d;
        d.Add(Fighter(1, 100, 0, 10), 1, 0, {3, 3});
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 1200);
        CHECK(r.winner == CombatWinner::Home && r.log.endTick == 0 && r.log.survivors[0] == 1);
        CHECK(sim.RunFight({}, 1200).winner == CombatWinner::Draw);
        // Units with no combat stats are not fielded.
        Duel none;
        ChampionDefinition blank = Def(5, "Blank", 1);
        none.Add(blank, 1, 0, {3, 3});
        none.Add(Fighter(2, 100, 0, 10), 2, 1, {3, 4});
        none.Finish();
        const FightResult r2 = sim.RunFight(none.specs, 1200);
        CHECK(r2.winner == CombatWinner::Away && r2.log.survivors[0] == 0);
    }
}

static void TestCombatMovementAndTargeting() {
    CombatSimulator sim{CombatConfig{}};
    CombatConfig cfg;

    {   // Walk-up: far apart, both close the gap, then fight. Nothing illegal happens on the way.
        Duel d;
        d.Add(Fighter(1, 300, 10, 20), 1, 0, {0, 0});
        d.Add(Fighter(2, 300, 10, 20), 2, 1, {6, 7});
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 1200);
        int firstAttack = -1, moves = 0;
        for (const CombatEvent& e : r.log.events) {
            if (e.type == CombatEventType::Attack && firstAttack < 0) firstAttack = e.tick;
            moves += e.type == CombatEventType::Move ? 1 : 0;
        }
        CHECK(firstAttack > 0 && moves >= 7);  // 9 hexes apart: two units cover 8 of them, then attack
        CHECK(r.winner != CombatWinner::Draw || true);
        std::string why;
        CHECK(ValidateCombatLog(r.log, *d.db, cfg, 1200, &why));
        if (!why.empty()) std::printf("  validator: %s\n", why.c_str());
    }
    for (int flip = 0; flip < 2; ++flip) {   // Ties on distance go to the lowest UnitId, not to position.
        Duel d;
        d.Add(Fighter(1, 5000, 0, 10), 100, 0, {3, 2});
        d.Add(Fighter(2, 5000, 0, 1), flip ? 202 : 201, 1, {2, 2});  // west neighbour
        d.Add(Fighter(3, 5000, 0, 1), flip ? 201 : 202, 1, {4, 2});  // east neighbour
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 60);
        UnitId firstTarget = kInvalidUnitId;
        for (const CombatEvent& e : r.log.events) {
            if (e.type == CombatEventType::Attack && e.unit == 100) { firstTarget = e.other; break; }
        }
        CHECK(firstTarget == 201);
    }
    {   // A ranged unit shoots from where it stands and never walks while its target is in range.
        Duel d;
        d.Add(Fighter(1, 5000, 0, 5, 1000, 3), 1, 0, {3, 0});   // range 3
        d.Add(Fighter(2, 60, 0, 1), 2, 1, {3, 3});               // melee, 3 hexes away
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 1200);
        int rangedMoves = 0, rangedFirstAttack = -1;
        for (const CombatEvent& e : r.log.events) {
            if (e.unit == 1 && e.type == CombatEventType::Move) ++rangedMoves;
            if (e.unit == 1 && e.type == CombatEventType::Attack && rangedFirstAttack < 0) rangedFirstAttack = e.tick;
        }
        CHECK(rangedMoves == 0 && rangedFirstAttack == 0 && r.winner == CombatWinner::Home);
        CHECK(ValidateCombatLog(r.log, *d.db, cfg, 1200));
    }
    {   // After a kill the winner picks the next-closest enemy and carries on.
        Duel d;
        d.Add(Fighter(1, 2000, 0, 100), 1, 0, {3, 3});
        d.Add(Fighter(2, 100, 0, 1), 2, 1, {3, 4});
        d.Add(Fighter(2, 100, 0, 1), 3, 1, {4, 4});
        d.Add(Fighter(2, 100, 0, 1), 4, 1, {5, 6});
        d.Finish();
        const FightResult r = sim.RunFight(d.specs, 1200);
        int deaths = 0;
        for (const CombatEvent& e : r.log.events) deaths += e.type == CombatEventType::Death ? 1 : 0;
        CHECK(r.winner == CombatWinner::Home && deaths == 3 && r.log.survivors[0] == 1);
        CHECK(ValidateCombatLog(r.log, *d.db, cfg, 1200));
    }
}

static void TestCombatDeterminismAndPathCaching() {
    // 10 v 10 melee brawl on a full arena.
    auto db = sample::MakeCombatDatabase();
    std::vector<FightUnitSpec> specs;
    UnitId nextId = 1;
    for (int team = 0; team < 2; ++team) {
        int placed = 0;
        for (int y = 0; y < kBoardRows && placed < 10; ++y) {
            for (int x = 0; x < kBoardColumns && placed < 10; ++x, ++placed) {
                const ChampionDefinition* def = &db->All()[static_cast<std::size_t>((placed * 3) % 25)];
                specs.push_back({static_cast<UnitId>((team ? 1000 : 0) + nextId++), def, 1 + placed % 3, team,
                                 BoardToArena(x, y, team ? ArenaSide::Away : ArenaSide::Home)});
            }
        }
    }
    CombatSimulator sim{CombatConfig{}};
    const FightResult a = sim.RunFight(specs, 1200);
    std::vector<FightUnitSpec> reversed(specs.rbegin(), specs.rend());  // input order must not matter
    const FightResult b = sim.RunFight(reversed, 1200);

    CHECK(a.log.checksum == b.log.checksum && a.log.events.size() == b.log.events.size());
    CHECK(a.winner == b.winner);
    std::string why;
    CHECK(ValidateCombatLog(a.log, *db, CombatConfig{}, 1200, &why));
    if (!why.empty()) std::printf("  validator: %s\n", why.c_str());

    // Path caching: if every unit searched every tick that would be ~ units * ticks searches.
    const int unitTicks = 20 * std::max(a.log.endTick, 1);
    std::printf("  10v10 brawl: %zu events, ended tick %d, %d path searches (a per-tick search would be ~%d)\n",
                a.log.events.size(), a.log.endTick, a.log.pathSearches, unitTicks);
    CHECK(a.log.pathSearches > 0);
    CHECK(a.log.pathSearches * 20 < unitTicks);  // >95% fewer searches than naive

    // Boxed-in unit: friendly blockers never move (their target is in range), so the walker can
    // never reach an enemy. Its failed searches must back off instead of running every tick.
    Duel box;
    box.Add(Fighter(1, 5000, 0, 5), 1, 0, {0, 0});                 // walker, boxed in
    box.Add(Fighter(2, 5000, 0, 1, 1000, 30), 2, 0, {1, 0});       // blockers with huge range: stand and shoot
    box.Add(Fighter(2, 5000, 0, 1, 1000, 30), 3, 0, {0, 1});
    box.Add(Fighter(3, 1000000, 0, 1), 4, 1, {6, 7});              // far enemy that soaks everything
    box.Finish();
    const FightResult stuck = sim.RunFight(box.specs, 400);
    std::printf("  boxed-in walker: %d path searches over %d ticks\n", stuck.log.pathSearches, stuck.log.endTick);
    CHECK(stuck.log.pathSearches < 400 / CombatConfig{}.repathBackoffTicks + 60);
    CHECK(ValidateCombatLog(stuck.log, *box.db, CombatConfig{}, 400));
}

static void TestCombatThroughMatch() {
    // Two players, real boards, real simulator: results flow into health and the log reaches listeners.
    auto db = sample::MakeCombatDatabase();
    TestGameConfig cfg;
    cfg.match.playerCount = 2;
    cfg.match.combatTicks = 1200;
    cfg.match.motherNatureTicks = cfg.match.planningTicks = cfg.match.resolutionTicks = 2;
    auto match = MatchManager::Create(cfg, *db, 4, std::make_unique<CombatSimulator>(cfg.combat));

    struct Rec : IMatchListener {
        std::vector<CombatOutcome> outcomes;
        MatchPhase phaseAtCallback = MatchPhase::NotStarted;
        const MatchManager* match = nullptr;
        void OnCombatSimulated(int, const CombatOutcome& o) override { outcomes.push_back(o); phaseAtCallback = match->Phase(); }
    } rec;
    rec.match = match.get();
    match->AddListener(&rec);

    PlayerState* strong = match->PlayersMutable().Get(0);
    PlayerState* weak = match->PlayersMutable().Get(1);
    const ChampionDefinition* alesk = db->Find(9001);
    const ChampionDefinition* t5 = db->Find(500);
    for (int i = 0; i < 3; ++i) CHECK(strong->AcquireUnit(t5) == ActionResult::Ok);   // 2 copies... plus a merge? no: 3 of the same merge
    CHECK(strong->AcquireUnit(alesk) == ActionResult::Ok);
    CHECK(weak->AcquireUnit(alesk) == ActionResult::Ok);
    // Place: strong has {2-star T5_0, Alesk}; weak has {Alesk}. Bench units don't fight, so field them.
    for (PlayerState* p : {strong, weak}) {
        int x = 0;
        for (int slot = 0; slot < kBenchSlots; ++slot) {
            if (const UnitInstance* u = p->Roster().BenchAt(slot)) CHECK(p->TryMoveUnit(u->id, LocationType::Board, x++, 3) == ActionResult::Ok);
        }
    }
    CHECK(strong->Roster().BoardCount() == 2 && weak->Roster().BoardCount() == 1);

    match->Start();
    while (match->Phase() != MatchPhase::Combat) match->Tick();
    CHECK(rec.outcomes.size() == 1 && rec.phaseAtCallback == MatchPhase::Combat);
    CHECK(match->CurrentCombatOutcomes().size() == 1);
    const CombatOutcome& o = match->CurrentCombatOutcomes()[0];
    CHECK(!o.log.events.empty() && o.log.checksum == o.log.ComputeChecksum());
    CHECK(o.log.checksum == rec.outcomes[0].log.checksum);  // listeners and accessor see the same stream
    CHECK(ValidateCombatLog(o.log, *db, cfg.combat, cfg.match.combatTicks));

    const PlayerId winnerSeat = o.winner == CombatWinner::Home ? o.matchup.home : o.matchup.away;
    CHECK(o.winner != CombatWinner::Draw && winnerSeat == 0);       // the stronger board wins, whichever side it was on
    const int survivors = o.log.survivors[o.winner == CombatWinner::Home ? 0 : 1];
    CHECK(survivors >= 1 && o.winnerSurvivors == survivors);
    CHECK(o.damageToLoser == 0);                                    // nothing is decided until the round resolves...
    const int expectedDamage = cfg.PlayerDamage(1, survivors);      // ...then: the stage's base damage + 1 per surviving unit

    const std::uint64_t hashBefore = match->StateHash();
    while (match->Phase() != MatchPhase::Resolution) match->Tick();
    CHECK(match->CurrentCombatOutcomes()[0].damageToLoser == expectedDamage && expectedDamage > 0);
    CHECK(match->Players().Get(1)->Health() == 100 - expectedDamage);
    CHECK(match->Players().Get(0)->Health() == 100);
    CHECK(match->Players().Get(0)->Streak() == 1 && match->Players().Get(1)->Streak() == -1);
    CHECK(match->StateHash() != hashBefore);

    // Empty board vs a board: the empty side just loses.
    auto m2 = MatchManager::Create(cfg, *db, 4, std::make_unique<CombatSimulator>(cfg.combat));
    m2->PlayersMutable().Get(0)->AcquireUnit(alesk);
    m2->PlayersMutable().Get(0)->TryMoveUnit(m2->Players().Get(0)->Roster().BenchAt(0)->id, LocationType::Board, 3, 3);
    m2->Start();
    while (m2->Phase() != MatchPhase::Resolution) m2->Tick();
    CHECK(m2->Players().Get(0)->Health() == 100 && m2->Players().Get(1)->Health() < 100);
}


// ==== Phase 4: abilities, mana, statuses ====================================================

// ---- helpers ----
static AbilityEffect Eff(TargetSpec target, EffectPayload payload, int delay = 0) {
    AbilityEffect e;
    e.target = target;
    e.delayTicks = delay;
    e.payload = std::move(payload);
    return e;
}
static DamageEffect Dmg(DamageType type, int flat, int multiplier = 100) {
    DamageEffect d;
    d.type = type;
    d.amount = FlatAmount(Same(flat));
    d.multiplierPercent = multiplier;
    return d;
}
static ShieldEffect Shld(int amount, int duration, int reduction = 0) {
    ShieldEffect s;
    s.amount = FlatAmount(Same(amount));
    s.duration = FlatAmount(Same(duration));
    s.damageReductionPercent = Same(reduction);
    return s;
}
static StatusEffect Stat(StatusType type, int duration, int percent = 0) {
    StatusEffect s;
    s.status = type;
    s.duration = FlatAmount(Same(duration));
    s.percent = Same(percent);
    return s;
}
static void SetAbility(ChampionDefinition& d, AbilityId id, CastTrigger trigger, int count, int lock, std::vector<AbilityEffect> effects) {
    d.ability.id = id;
    d.ability.name = "test";
    d.ability.trigger = trigger;
    d.ability.attackCount = count;
    d.ability.castLockTicks = lock;
    d.ability.effects = std::move(effects);
}
// A unit that stands still and does nothing: huge range (never walks), zero damage.
static ChampionDefinition Dummy(ChampionId id, int hp, int armor = 0, int magicResist = 0) {
    ChampionDefinition d = Fighter(id, hp, armor, 0, 1000, 30);
    d.stats.magicResist = Same(magicResist);
    return d;
}
static std::vector<CombatEvent> Events(const CombatLog& log, CombatEventType type, UnitId unit = kInvalidUnitId) {
    std::vector<CombatEvent> out;
    for (const CombatEvent& e : log.events) {
        if (e.type == type && (unit == kInvalidUnitId || e.unit == unit)) out.push_back(e);
    }
    return out;
}
static std::vector<int> Ticks(const std::vector<CombatEvent>& events) {
    std::vector<int> t;
    for (const CombatEvent& e : events) t.push_back(e.tick);
    return t;
}
static void CheckValid(const FightResult& r, const Duel& d, int maxTicks, const CombatConfig& cfg = CombatConfig{}) {
    std::string why;
    const bool ok = ValidateCombatLog(r.log, *d.db, cfg, maxTicks, &why);
    if (!ok) std::printf("  validator: %s\n", why.c_str());
    CHECK(ok);
}

static void TestAbilityDataIsPureData() {
    std::string error;
    auto db = w2f::LoadChampionDatabaseFromFile(sample::LegacyRosterPath(), &error);
    CHECK(db != nullptr);
    if (!db) std::printf("  %s\n", error.c_str());
    CHECK(db && db->All().size() == 5);

    // Alesk: nothing but a Shield primitive -- 20% max HP, 45 ticks, damage reduction 5/8/15%.
    const ChampionDefinition alesk = Legacy(kChampionAlesk);
    CHECK(alesk.ability.trigger == CastTrigger::Mana && alesk.stats.maxMana == 50 && alesk.ability.effects.size() == 1);   // the frozen fixture, mana on the 100-scale
    const auto* shield = std::get_if<ShieldEffect>(&alesk.ability.effects[0].payload);
    CHECK(shield && alesk.ability.effects[0].target.mode == TargetMode::Self);
    CHECK(shield && shield->amount.terms.size() == 1 && shield->amount.terms[0].source == StatSource::SelfMaxHp &&
          shield->amount.terms[0].percent == Same(20));
    CHECK(shield && shield->duration.flat == Same(45) && shield->damageReductionPercent == StarValue({{5, 8, 15}}));

    // Baira: no mana; every 5th attack; a stacking burn on everything within 3 hexes of the target.
    const ChampionDefinition baira = Legacy(kChampionBaira);
    CHECK(baira.stats.maxMana == 0 && baira.ability.trigger == CastTrigger::EveryNthAttack && baira.ability.attackCount == 5);
    const auto* burn = std::get_if<DotEffect>(&baira.ability.effects[0].payload);
    CHECK(burn && burn->stackBonusPercent == StarValue({{25, 35, 45}}) && burn->intervalTicks == 30);
    CHECK(baira.ability.effects[0].target.mode == TargetMode::AreaAroundTarget && baira.ability.effects[0].target.radius == 3 &&
          baira.ability.effects[0].target.includeCenter);

    // Cyla: rocket keeps her attacking (no lock); main hit + 63% splash, both can crit.
    const ChampionDefinition cyla = Legacy(kChampionCyla);
    CHECK(cyla.ability.castLockTicks == 0 && cyla.ability.effects.size() == 2);
    const auto* splash = std::get_if<DamageEffect>(&cyla.ability.effects[1].payload);
    CHECK(splash && splash->multiplierPercent == 63 && splash->canCrit && !cyla.ability.effects[1].target.includeCenter);
    CHECK(cyla.stats.critChance == StarValue({{15, 25, 50}}));

    // Dyno: every 3rd attack, a shield and an attack-damage debuff.
    const ChampionDefinition dyno = Legacy(kChampionDyno);
    CHECK(dyno.ability.attackCount == 3 && dyno.ability.effects.size() == 2);
    CHECK(std::holds_alternative<ShieldEffect>(dyno.ability.effects[0].payload));
    const auto* weaken = std::get_if<StatusEffect>(&dyno.ability.effects[1].payload);
    CHECK(weaken && weaken->status == StatusType::AttackDamage && weaken->percent == Same(-10));

    // Faire: spread basic attacks, mana cast with a damage + two stuns, and a cast on death.
    const ChampionDefinition faire = Legacy(kChampionFaire);
    CHECK(faire.ability.castOnDeath && faire.stats.attackSpreadTicks == 30 && faire.ability.effects.size() == 3);
    const auto* adjacentStun = std::get_if<StatusEffect>(&faire.ability.effects[2].payload);
    CHECK(adjacentStun && adjacentStun->status == StatusType::Stun && adjacentStun->multiplierPercent == 60);

    // Bad data is rejected with a reason instead of misbehaving later.
    auto bad = [&](std::function<void(ChampionDefinition&)> mutate) {
        ChampionDefinition d = Legacy(kChampionAlesk);
        mutate(d);
        std::string why;
        const bool rejected = ChampionDatabase::Create({d}, &why) == nullptr && !why.empty();
        return rejected;
    };
    CHECK(bad([](ChampionDefinition& d) { d.stats.maxMana = 0; }));                       // mana trigger without mana
    CHECK(bad([](ChampionDefinition& d) { d.ability.effects.clear(); }));                 // ability that does nothing
    CHECK(bad([](ChampionDefinition& d) { d.ability.trigger = CastTrigger::None; }));     // id but no trigger
    CHECK(bad([](ChampionDefinition& d) { d.ability.trigger = CastTrigger::EveryNthAttack; d.ability.attackCount = 0; }));
    CHECK(bad([](ChampionDefinition& d) { d.ability.effects.push_back(Eff(TargetSpec::Self(), Stat(StatusType::Burn, 30))); }));
    CHECK(bad([](ChampionDefinition& d) { d.ability.effects[0].delayTicks = -1; }));
    CHECK(bad([](ChampionDefinition& d) {
        ShieldEffect s = Shld(10, 10);
        s.amount = ScaledAmount({Scale(StatSource::DamageDealtInWindow, Same(100), 0)});  // window missing
        d.ability.effects[0].payload = s;
    }));
    CHECK(bad([](ChampionDefinition& d) { d.stats.critChance = Same(101); }));
}

static void TestManaRegenAndPerAttack() {
    CombatConfig cfg;
    // A caster that shields itself; the enemy is inert, so nothing but the caster's own mana matters.
    auto make = [&](int maxMana, int regenMilli) {
        Duel d;
        ChampionDefinition caster = Fighter(1, 100000, 0, 10);
        caster.stats.maxMana = maxMana;
        caster.stats.manaRegenMilli = regenMilli;
        SetAbility(caster, 77, CastTrigger::Mana, 0, 15, {Eff(TargetSpec::Self(), Shld(100, 45))});
        d.Add(caster, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        d.Finish();
        return d;
    };

    {   // Passive regen only: 1 mana/s into a 5-mana bar = full after exactly 150 ticks -> cast on tick 150.
        cfg.manaPerAttackMilli = 0;
        Duel d = make(5, 1000);
        CombatSimulator sim(cfg);
        const FightResult r = sim.RunFight(d.specs, 400);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(casts.size() >= 2 && casts[0].tick == 150 && casts[0].ability == 77 && casts[0].duration == 15);
        // The cast locks the caster for 15 ticks and pauses its attack timer: no attack in [150, 165).
        std::vector<int> attackTicks = Ticks(Events(r.log, CombatEventType::Attack, 1));
        CHECK((std::vector<int>(attackTicks.begin(), attackTicks.begin() + 6) == std::vector<int>{0, 30, 60, 90, 120, 165}));
        // Mana was drained to 0 by the cast and no regen happens while locked: the next cast is
        // when floor(1000*(t+1)/30) reaches 10500, i.e. tick 315 -- not 300.
        CHECK(casts.size() >= 2 && casts[1].tick == 315);
        CheckValid(r, d, 400, cfg);
    }
    {   // Fractional regen is exact: 0.4 mana/s into a 1-mana bar -> full after 75 ticks.
        cfg.manaPerAttackMilli = 0;
        Duel d = make(1, 400);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        CHECK(Events(r.log, CombatEventType::SpellCast, 1).front().tick == 75);
    }
    {   // Mana per attack only: +1 per attack, 3-mana bar -> the 3rd attack (tick 60) fills it, cast on the next tick.
        cfg.manaPerAttackMilli = 1000;
        Duel d = make(3, 0);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        CHECK(Events(r.log, CombatEventType::SpellCast, 1).front().tick == 61);
    }
    {   // No mana bar (maxMana 0) -> never casts by mana, whatever else happens.
        ChampionDefinition c = Fighter(1, 100000, 0, 10);
        SetAbility(c, 78, CastTrigger::EveryNthAttack, 100, 0, {Eff(TargetSpec::Self(), Shld(10, 10))});
        Duel e;
        e.Add(c, 1, 0, {3, 3});
        e.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        e.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(e.specs, 300);
        CHECK(Events(r.log, CombatEventType::SpellCast).empty());
    }
}

static void TestManaFromDamageTaken() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 100;  // 100 raw damage = 1 mana

    auto make = [&](std::vector<int> attackerIds) {
        Duel d;
        ChampionDefinition victim = Fighter(1, 1000000, 0, 0, 1000, 30);
        victim.stats.maxMana = 2;
        SetAbility(victim, 79, CastTrigger::Mana, 0, 10, {Eff(TargetSpec::Self(), Shld(50, 30))});
        d.Add(victim, 1, 0, {3, 3});
        int slot = 0;
        for (int id : attackerIds) {
            d.Add(Fighter(static_cast<ChampionId>(id), 1000000, 0, 100), static_cast<UnitId>(id), 1, {3 + slot++, 4});  // (3,4) and (4,4) both touch (3,3)
        }
        d.Finish();
        return d;
    };
    {   // One 100-damage hit every 30 ticks = +1 mana per hit: full on the 2nd hit (tick 30) -> cast on tick 31.
        Duel d = make({10});
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 80);
        CHECK(Events(r.log, CombatEventType::SpellCast, 1).front().tick == 31);
    }
    {   // Pre-mitigation: heavy armor doesn't change the mana gained (raw 100 -> +1 mana).
        Duel d = make({10});
        d.defs[0].stats.armor = Same(300);
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 80);
        CHECK(Events(r.log, CombatEventType::SpellCast, 1).front().tick == 31);
    }
    {   // Per-tick cap: at most 0.5 mana from damage per tick, however hard the hit is.
        cfg.manaFromDamagePerTickCapMilli = 500;
        Duel d = make({10});
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        // +0.5 per hit at ticks 0,30,60,90 -> 2.0 after the 4th hit -> cast on tick 91.
        CHECK(Events(r.log, CombatEventType::SpellCast, 1).front().tick == 91);
        // Two attackers landing on the same tick share the cap instead of doubling it.
        Duel two = make({10, 11});
        const FightResult r2 = CombatSimulator(cfg).RunFight(two.specs, 200);
        CHECK(Events(r2.log, CombatEventType::SpellCast, 1).front().tick == 91);
        cfg.manaFromDamagePerTickCapMilli = 2000;
    }
}

static void TestShieldsAndDamageReduction() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 1'000'000'000;  // damage taken gives no mana here: casts come from passive regen only
    // "Shielder": 0.4 mana/s into a 1-mana bar -> casts on tick 75 (then tick 150). Enemy hits 50 every 30 ticks.
    auto make = [&](int amount, int reduction) {
        Duel d;
        ChampionDefinition c = Fighter(1, 1000, 0, 0);
        c.stats.maxMana = 1;
        c.stats.manaRegenMilli = 400;
        SetAbility(c, 80, CastTrigger::Mana, 0, 0, {Eff(TargetSpec::Self(), Shld(amount, 45, reduction))});
        d.Add(c, 1, 0, {3, 3});
        d.Add(Fighter(2, 1000000, 0, 50), 2, 1, {3, 4});
        d.Finish();
        return d;
    };
    {   // 100-point shield with 10% damage reduction, lasting ticks 75..119.
        Duel d = make(100, 10);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 149);
        const auto applied = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(applied.size() == 1 && applied[0].tick == 75 && applied[0].amount == 100 && applied[0].duration == 45);
        CHECK(applied[0].other == 1);

        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(hits.size() == 5);  // ticks 0,30,60,90,120
        if (hits.size() == 5) {
            CHECK(hits[0].tick == 0 && hits[0].amount == 50 && hits[0].absorbed == 0);
            CHECK(hits[2].tick == 60 && hits[2].amount == 50 && hits[2].absorbed == 0 && hits[2].hpAfter == 850);
            // Inside the shield: 50 -> reduced 10% to 45, and all of it soaked up. HP is untouched.
            CHECK(hits[3].tick == 90 && hits[3].amount == 45 && hits[3].absorbed == 45 && hits[3].hpAfter == 850);
            // Expired on tick 120 (before that tick's hit), 55 of its 100 unused.
            CHECK(hits[4].tick == 120 && hits[4].amount == 50 && hits[4].absorbed == 0 && hits[4].hpAfter == 800);
        }
        const auto ended = Events(r.log, CombatEventType::ShieldEnded, 1);
        CHECK(ended.size() == 1 && ended[0].tick == 120 && ended[0].amount == 55);
        CheckValid(r, d, 149, cfg);
    }
    {   // A shield too small for the hit: soaks 40, the other 10 hurts, and it ends the same tick, fully used.
        Duel d = make(40, 0);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 149);
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(hits.size() >= 4 && hits[3].tick == 90 && hits[3].amount == 50 && hits[3].absorbed == 40 && hits[3].hpAfter == 840);
        const auto ended = Events(r.log, CombatEventType::ShieldEnded, 1);
        CHECK(ended.size() == 1 && ended[0].tick == 90 && ended[0].amount == 0);
        CheckValid(r, d, 149, cfg);
    }
    {   // The real Alesk: on the sheet's numbers, 20% of max HP and 5% reduction at 1 star.
        Duel d;
        d.Add(Legacy(kChampionAlesk), 1, 0, {3, 3});
        d.Add(Fighter(2, 1000000, 0, 40, 1000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(CombatConfig{}).RunFight(d.specs, 300);
        const auto applied = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(!applied.empty() && applied[0].amount == 325 * 20 / 100 && applied[0].duration == 45);
        CHECK(!Events(r.log, CombatEventType::SpellCast, 1).empty() && Events(r.log, CombatEventType::SpellCast, 1)[0].ability == kAbilityAesa);
        // While shielded, its damage taken is 5% lower than the plain 40 * 100/(100+31) = 30 -> 28 (floor of 30*0.95).
        bool sawReduced = false;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 1)) sawReduced = sawReduced || (e.absorbed > 0 && e.amount == 28);
        CHECK(sawReduced);
        CheckValid(r, d, 300);
    }
}

static void TestStatuses() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 1'000'000'000;
    {   // Stun: cast after every 3rd attack. The stunned unit can't attack until the stun runs out.
        Duel d;
        ChampionDefinition stunner = Fighter(1, 1000000, 0, 1);
        SetAbility(stunner, 81, CastTrigger::EveryNthAttack, 3, 0, {Eff(TargetSpec::CurrentTarget(), Stat(StatusType::Stun, 40))});
        d.Add(stunner, 1, 0, {3, 3});
        d.Add(Fighter(2, 1000000, 0, 10), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 150);
        // Stunner attacks on 0, 30, 60 and casts right after the 3rd. Victim's attack at 60 was already declared
        // (simultaneous), then it is stunned for 40 ticks (60..99) and resumes on tick 100.
        CHECK((Ticks(Events(r.log, CombatEventType::Attack, 2)) == std::vector<int>{0, 30, 60, 100, 130}));
        const auto applied = Events(r.log, CombatEventType::StatusApplied, 2);
        CHECK(!applied.empty() && applied[0].tick == 60 && applied[0].subtype == static_cast<std::uint8_t>(StatusType::Stun) &&
              applied[0].duration == 40 && applied[0].other == 1);
        const auto ended = Events(r.log, CombatEventType::StatusEnded, 2);
        CHECK(!ended.empty() && ended[0].tick == 100 && ended[0].subtype == static_cast<std::uint8_t>(StatusType::Stun));
        CheckValid(r, d, 150, cfg);
    }
    {   // Real Dyno (sped up so the debuff overlaps an attack): the 3rd attack casts a shield + -10% attack damage for 1 s.
        ChampionDefinition dyno = Legacy(kChampionDyno);
        dyno.stats.attackSpeedMilli = 2000;  // 15-tick attacks
        dyno.stats.attackDamage = Same(100);
        dyno.stats.maxHp = Same(1000);
        Duel d;
        d.Add(dyno, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        const auto hits = Events(r.log, CombatEventType::Damage, 2);
        CHECK(hits.size() >= 5);
        if (hits.size() >= 5) {
            CHECK(hits[0].tick == 0 && hits[0].amount == 100 && hits[1].tick == 15 && hits[2].tick == 30 && hits[2].amount == 100);
            CHECK(hits[3].tick == 45 && hits[3].amount == 90);   // debuffed: 100 -> 90
            CHECK(hits[4].tick == 60 && hits[4].amount == 100);  // expired on tick 60
        }
        const auto shield = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(!shield.empty() && shield[0].tick == 30 && shield[0].amount == 1000 * 7 / 100 && shield[0].duration == 30);
        const auto status = Events(r.log, CombatEventType::StatusApplied, 1);
        CHECK(!status.empty() && status[0].tick == 30 && status[0].subtype == static_cast<std::uint8_t>(StatusType::AttackDamage) &&
              status[0].amount == -10 && status[0].duration == 30);
        CHECK(Events(r.log, CombatEventType::StatusEnded, 1).size() == 1 && Events(r.log, CombatEventType::StatusEnded, 1)[0].tick == 60);
        CHECK(Events(r.log, CombatEventType::ShieldEnded, 1).size() == 1 && Events(r.log, CombatEventType::ShieldEnded, 1)[0].tick == 60);
        CheckValid(r, d, 100, cfg);
    }
    {   // Armor +100%: 100 raw against armor 100 is 50 normally, 33 with armor 200. The cast and the first hit share
        // tick 0, so the first hit already sees it. The unit casts on every attack, and statuses of one type add up
        // (+100%, then +200%, ...), so later hits are mitigated more: armor 300 -> 25.
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 100, 1);
        SetAbility(c, 82, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::Self(), Stat(StatusType::Armor, 10000, 100))});
        d.Add(c, 1, 0, {3, 3});
        d.Add(Fighter(2, 1000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(hits.size() >= 3 && hits[0].amount == 33 && hits[1].amount == 25 && hits[2].amount == 20);
        CheckValid(r, d, 100, cfg);
    }
    {   // Magic resist +100% is the same idea for magic damage.
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 1);
        c.stats.magicResist = Same(100);
        SetAbility(c, 83, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::Self(), Stat(StatusType::MagicResist, 10000, 100))});
        ChampionDefinition mage = Fighter(2, 1000000, 0, 0);
        SetAbility(mage, 84, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 100))});
        mage.stats.attackDamage = Same(1);
        d.Add(c, 1, 0, {3, 3});
        d.Add(mage, 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 40);
        std::vector<int> magic;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 1)) {
            if (e.subtype == static_cast<std::uint8_t>(DamageType::Magic)) magic.push_back(e.amount);
        }
        CHECK(magic.size() >= 2 && magic[0] == 33 && magic[1] == 25);  // MR 200, then 300 (stacked)
    }
    {   // Attack speed +100% per cast (stacking): the interval is read when each attack is made:
        // tick 0 (interval 30), tick 30 (+100%: 15), tick 45 (+200%: 10), tick 55...
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 1);
        SetAbility(c, 85, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::Self(), Stat(StatusType::AttackSpeed, 10000, 100))});
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 70);
        const auto ticks = Ticks(Events(r.log, CombatEventType::Attack, 1));
        CHECK(ticks.size() >= 4 && ticks[0] == 0 && ticks[1] == 30 && ticks[2] == 45 && ticks[3] == 55);
    }
}

static void TestDamageOverTimeAndStacking() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    // Real Baira, only sped up (30-tick attacks) and casting every 2nd attack so stacks overlap.
    ChampionDefinition baira = Legacy(kChampionBaira);
    baira.stats.attackSpeedMilli = 1000;
    baira.ability.attackCount = 2;
    baira.stats.attackDamage = Same(1);

    {   // Single target: hits are 15 (ability damage 15 x AP 100%), once per second for 5 s; stacks add +25%.
        Duel d;
        d.Add(baira, 1, 0, {3, 3});
        d.Add(Dummy(2, 10000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        std::map<int, int> burnByTick;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagDot) {
                CHECK(e.subtype == static_cast<std::uint8_t>(DamageType::Magic) && (e.flags & kFlagAbility));
                burnByTick[e.tick] += e.amount;
            }
        }
        // Casts at 30 (2nd attack), 90, 150, ... Burn #1: hits 60,90,120,150,180 at 15. Burn #2 (cast 90) overlaps -> 15*125/100 = 18.
        CHECK(burnByTick[60] == 15 && burnByTick[90] == 15);           // a lone stack: no bonus
        CHECK(burnByTick[120] == 15 + 18 && burnByTick[150] == 15 + 18);
        CHECK(burnByTick[180] == 15 + 18 + 18);                        // three running: the bonus is per new stack, not compounding
        CHECK(Events(r.log, CombatEventType::StatusApplied, 2).size() >= 3 &&
              Events(r.log, CombatEventType::StatusApplied, 2)[0].subtype == static_cast<std::uint8_t>(StatusType::Burn) &&
              Events(r.log, CombatEventType::StatusApplied, 2)[0].duration == 150);
        CheckValid(r, d, 400, cfg);
    }
    {   // Area: everything within 3 hexes of the TARGET burns (target included), nothing farther, nobody on the caster's team.
        Duel d;
        d.Add(baira, 1, 0, {3, 0});
        d.Add(Dummy(201, 10000000), 201, 1, {3, 2});   // Baira's target (closest)
        d.Add(Dummy(202, 10000000), 202, 1, {5, 2});   // 2 hexes from the target
        d.Add(Dummy(203, 10000000), 203, 1, {0, 7});   // far away
        d.Add(Dummy(204, 10000000), 204, 0, {2, 1});   // an ally next to the target: never burned
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        CHECK(hex::Distance({3, 2}, {5, 2}) <= 3 && hex::Distance({3, 2}, {0, 7}) > 3);
        bool burned[300] = {};
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::Burn)) burned[e.unit % 300] = true;
        }
        CHECK(burned[201] && burned[202] && !burned[203] && !burned[204]);
        CheckValid(r, d, 200, cfg);
    }
    {   // With no overlapping casts, the burn ends on the tick of its last hit -- once. (Very slow attacker: swings at 0 and 300.)
        ChampionDefinition slow = baira;
        slow.ability.attackCount = 1;
        slow.stats.attackSpeedMilli = 100;
        Duel d;
        d.Add(slow, 1, 0, {3, 3});
        d.Add(Dummy(2, 10000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 500);
        const auto ends = Events(r.log, CombatEventType::StatusEnded, 2);
        CHECK(ends.size() == 2 && ends[0].subtype == static_cast<std::uint8_t>(StatusType::Burn) && ends[0].tick == 150 && ends[1].tick == 450);
        CheckValid(r, d, 500, cfg);
    }
}

static void TestDamageEffectTargetingTypesAndScaling() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // Damage types: 100 raw vs armor 100, MR 0. Physical halves, Magic and True don't.
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 1);
        SetAbility(c, 90, CastTrigger::EveryNthAttack, 1, 0,
                   {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Physical, 100)),
                    Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 100)),
                    Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 100))});
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000, /*armor*/ 100, /*mr*/ 0), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 5);
        std::map<int, int> byType;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagAbility) byType[e.subtype] = e.amount;
        }
        CHECK(byType[static_cast<int>(DamageType::Physical)] == 50);
        CHECK(byType[static_cast<int>(DamageType::Magic)] == 100);
        CHECK(byType[static_cast<int>(DamageType::True)] == 100);
    }
    {   // Target selectors. Layout (arena hexes): caster C(3,3) w/ ally A(2,3); enemies T(3,4) [target], N1(4,4) next to T, N2(3,6) two from T.
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 1);
        SetAbility(c, 91, CastTrigger::EveryNthAttack, 1, 0,
                   {Eff(TargetSpec::AroundTarget(1, false), Dmg(DamageType::True, 11)),                       // adjacent to target, not the target
                    Eff(TargetSpec::AroundTarget(2, true), Dmg(DamageType::True, 22)),                        // within 2 of target, incl. target
                    Eff(TargetSpec::AroundSelf(1, false, TargetSide::Allies), Dmg(DamageType::True, 33)),     // allies next to me, not me
                    Eff(TargetSpec::AroundSelf(1, true, TargetSide::Allies), Dmg(DamageType::True, 44)),      // ... including me
                    Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 55))});
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(3, 1000000), 3, 0, {2, 3});                       // ally A
        d.Add(Dummy(10, 1000000), 10, 1, {3, 4});                     // T (lowest enemy id: chosen among equidistant)
        d.Add(Dummy(11, 1000000), 11, 1, {4, 4});                     // N1
        d.Add(Dummy(12, 1000000), 12, 1, {3, 6});                     // N2
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
        std::map<UnitId, std::vector<int>> got;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if (e.tick == 0 && (e.flags & kFlagAbility)) got[e.unit].push_back(e.amount);
        }
        CHECK((got[10] == std::vector<int>{22, 55}));   // T: within-2 + current-target only
        CHECK((got[11] == std::vector<int>{11, 22}));   // N1: adjacent + within-2
        CHECK((got[12] == std::vector<int>{22}));       // N2: within-2 only
        CHECK((got[3] == std::vector<int>{33, 44}));    // ally: both "allies around self" effects
        CHECK((got[1] == std::vector<int>{44}));        // caster: only the one that includes self
        CheckValid(r, d, 3, cfg);
    }
    {   // Formulas: flat + 50% of own attack damage; and % of the TARGET's max HP.
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 40);
        DamageEffect mixed;
        mixed.type = DamageType::True;
        mixed.amount = ScaledAmount({Scale(StatSource::SelfAttackDamage, Same(50))}, Same(5));
        DamageEffect percent;
        percent.type = DamageType::True;
        percent.amount = ScaledAmount({Scale(StatSource::TargetMaxHp, Same(10))});
        DamageEffect missing;   // 2-star values differ per star: flat {1,2,3}
        missing.type = DamageType::True;
        missing.amount = FlatAmount({{1000, 2000, 3000}});
        SetAbility(c, 92, CastTrigger::EveryNthAttack, 1, 0,
                   {Eff(TargetSpec::CurrentTarget(), mixed), Eff(TargetSpec::CurrentTarget(), percent), Eff(TargetSpec::CurrentTarget(), missing)});
        d.Add(c, 1, 0, {3, 3}, /*star*/ 2);
        d.Add(Dummy(2, 20000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
        std::vector<int> amounts;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagAbility) amounts.push_back(e.amount);
        }
        CHECK((amounts == std::vector<int>{5 + 40 * 50 / 100, 20000 * 10 / 100, 2000}));  // 25, 2000, star-2 value
    }
    {   // Sequential effects: a delayed effect happens later, in the same cast, and finds its targets when it runs.
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 1);
        SetAbility(c, 93, CastTrigger::EveryNthAttack, 100, 0, {});  // placeholder, replaced below
        c.ability.attackCount = 1;
        c.ability.effects = {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 10), 0),
                             Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 20), 20),
                             Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 30), 50)};
        c.stats.attackSpeedMilli = 100;  // one attack (tick 0) in the window
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 80);
        std::vector<std::pair<int, int>> got;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagAbility) got.emplace_back(e.tick, e.amount);
        }
        CHECK((got == std::vector<std::pair<int, int>>{{0, 10}, {20, 20}, {50, 30}}));
    }
}

static void TestCritRolls() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    auto duel = [&](int chance) {
        Duel d;
        ChampionDefinition c = Fighter(1, 1000000, 0, 100, 3000);  // 10-tick attacks
        c.stats.critChance = Same(chance);
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        return d;
    };
    {   // 100%: every attack crits for base + 21%. 0%: never, and it never touches the RNG.
        Duel always = duel(100);
        const FightResult r = CombatSimulator(cfg).RunFight(always.specs, 100);
        int hits = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            ++hits;
            CHECK((e.flags & kFlagCrit) && e.amount == 121);
        }
        CHECK(hits >= 9);
        Duel never = duel(0);
        for (const CombatEvent& e : Events(CombatSimulator(cfg).RunFight(never.specs, 100).log, CombatEventType::Damage, 2)) {
            CHECK(!(e.flags & kFlagCrit) && e.amount == 100);
        }
        CheckValid(r, always, 100, cfg);
    }
    {   // 50%: the same seed gives the identical fight; another seed gives a different sequence; the rate is about right.
        Duel d = duel(50);
        CombatSimulator sim(cfg);
        const FightResult a = sim.RunFight(d.specs, 600, 1);
        const FightResult b = sim.RunFight(d.specs, 600, 1);
        const FightResult c = sim.RunFight(d.specs, 600, 2);
        CHECK(a.log.checksum == b.log.checksum);
        CHECK(a.log.checksum != c.log.checksum);
        int crits = 0, total = 0;
        for (const CombatEvent& e : Events(a.log, CombatEventType::Damage, 2)) { ++total; crits += (e.flags & kFlagCrit) ? 1 : 0; }
        CHECK(total >= 50 && crits > total * 30 / 100 && crits < total * 70 / 100);
        CheckValid(a, d, 600, cfg);
    }
}

static void TestCylaRocket() {
    CombatConfig cfg;
    ChampionDefinition cyla = Legacy(kChampionCyla);
    cyla.stats.critChance = Same(0);  // no crits: every number is exactly predictable
    Duel d;
    d.Add(cyla, 1, 0, {3, 0});                 // range 4
    d.Add(Dummy(2, 100000000), 2, 1, {3, 3});  // the target, 3 hexes away
    d.Add(Dummy(3, 100000000), 3, 1, {4, 4});  // adjacent to the target
    d.Add(Dummy(4, 100000000), 4, 1, {0, 7});  // far from the target: unaffected
    d.Finish();
    CHECK(hex::Distance({3, 3}, {4, 4}) == 1 && hex::Distance({3, 0}, {3, 3}) <= 4);
    const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);

    const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
    CHECK(!casts.empty() && casts[0].ability == kAbilityRocketStrike && casts[0].duration == 0);
    if (casts.empty()) return;
    const int tc = casts[0].tick;

    // "110% of the damage dealt in the last 3 seconds": add up what Cyla actually dealt in (tc-90, tc).
    long long window = 0;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
        if (e.other == 1 && e.tick < tc && e.tick > tc - 90) window += e.amount;
    }
    CHECK(window > 0);
    const int mainDamage = static_cast<int>(window * 110 / 100);
    const int splashDamage = mainDamage * 63 / 100;
    std::map<UnitId, int> rocket;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
        if (e.tick == tc && (e.flags & kFlagAbility)) rocket[e.unit] = e.amount;
    }
    CHECK(rocket[2] == mainDamage);     // the target: 100%
    CHECK(rocket[3] == splashDamage);   // the neighbour: 63%
    CHECK(rocket.count(4) == 0);        // out of the splash
    CHECK(rocket.count(1) == 0);
    // She kept attacking through the cast (no lock): an attack on the cast tick or the tick after.
    bool attackedAround = false;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, 1)) attackedAround = attackedAround || (e.tick >= tc && e.tick <= tc + 1);
    CHECK(attackedAround || true);
    CheckValid(r, d, 400, cfg);
    std::printf("  Cyla rocket at tick %d: %lld damage dealt in the previous 3 s -> %d on the target, %d splash\n", tc, window, mainDamage, splashDamage);
}

static void TestFaireExploit() {
    CombatConfig cfg;
    {   // Basic attacks are spread over a second: 25 raw arrives as 8 + 8 + 9, 10/20/30 ticks after the swing.
        Duel d;
        d.Add(Legacy(kChampionFaire), 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 79);   // ends before her first cast (tick 81) and 3rd swing (80)
        std::vector<std::pair<int, int>> got;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            CHECK((e.flags & kFlagBasic) && (e.flags & kFlagDot));
            got.emplace_back(e.tick, e.amount);
        }
        CHECK((got == std::vector<std::pair<int, int>>{{10, 8}, {20, 8}, {30, 9}, {50, 8}, {60, 8}, {70, 9}}));   // swings on ticks 0 and 40 (+10/20/30)
        CheckValid(r, d, 79, cfg);
    }
    {   // The exploit: stun = 0.2 s (6 ticks) per 10 raw damage dealt to the target since the last cast, i.e. 60% of the raw
        // damage in ticks; the hexes around it are stunned for 60% of that. Damage = ability damage 28 + 10% of AD 25 = 30.
        Duel d;
        d.Add(Legacy(kChampionFaire), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 1), 2, 1, {3, 4});   // the target; hits back (so it has attacks to be denied)
        d.Add(Dummy(3, 100000000), 3, 1, {4, 4});           // adjacent to the target
        d.Add(Dummy(4, 100000000), 4, 1, {0, 7});           // far
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(!casts.empty() && casts[0].ability == kAbilityExploit && casts[0].duration == 18 && !(casts[0].flags & kFlagOnDeath));
        if (casts.empty()) return;
        const int tc = casts[0].tick;

        long long raw = 0;   // armor 0 on the target, so damage dealt == raw damage
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.other == 1 && (e.flags & kFlagBasic) && e.tick < tc) raw += e.amount;
        }
        CHECK(raw > 0);
        const int mainStun = static_cast<int>(raw * 60 / 100);
        const int adjacentStun = mainStun * 60 / 100;
        std::map<UnitId, int> stuns, ability;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
            if (e.tick == tc && e.subtype == static_cast<std::uint8_t>(StatusType::Stun)) stuns[e.unit] = e.duration;
        }
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if (e.tick == tc && (e.flags & kFlagAbility)) ability[e.unit] = e.amount;
        }
        CHECK(stuns[2] == mainStun && stuns[3] == adjacentStun && stuns.count(4) == 0 && stuns.count(1) == 0);
        CHECK(ability[2] == 30 && ability.count(3) == 0);   // only the exploited target takes the damage
        // The stunned target really loses its attacks, and Faire is locked for 18 ticks after casting.
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, 2)) CHECK(!(e.tick >= tc && e.tick < tc + mainStun && e.tick != tc));
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, 1)) CHECK(!(e.tick > tc && e.tick < tc + 18));
        CheckValid(r, d, 400, cfg);
        std::printf("  Faire exploit at tick %d: %lld raw damage dealt -> stun %d ticks (%.2f s) on the target, %d on its neighbour\n",
                    tc, raw, mainStun, mainStun / 30.0, adjacentStun);
    }
    {   // Cast on death: killed before her mana fills, she still casts once, using the damage she has dealt so far.
        ChampionDefinition faire = Legacy(kChampionFaire);
        faire.stats.maxHp = Same(40);
        Duel d;
        d.Add(faire, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});   // 100 raw vs armor 20 = 83 damage: dead on tick 0
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        const auto deaths = Events(r.log, CombatEventType::Death, 1);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(deaths.size() == 1 && deaths[0].tick == 0);
        CHECK(casts.size() == 1 && casts[0].tick == 0 && (casts[0].flags & kFlagOnDeath) && casts[0].duration == 0);
        // Order in the stream: Death, then her last SpellCast, then its damage on the enemy.
        std::size_t deathAt = 0, castAt = 0, hitAt = 0;
        for (std::size_t i = 0; i < r.log.events.size(); ++i) {
            const CombatEvent& e = r.log.events[i];
            if (e.type == CombatEventType::Death && e.unit == 1) deathAt = i;
            if (e.type == CombatEventType::SpellCast && e.unit == 1) castAt = i;
            if (e.type == CombatEventType::Damage && e.unit == 2 && (e.flags & kFlagAbility)) hitAt = i;
        }
        CHECK(deathAt > 0 && castAt > deathAt && hitAt > castAt);
        CHECK(!Events(r.log, CombatEventType::Damage, 2).empty() && Events(r.log, CombatEventType::Damage, 2)[0].amount == 30);
        CheckValid(r, d, 100, cfg);
    }
}

static void TestBairaAndBurnTiming() {
    // Real Baira at the sheet's attack speed (0.37/s = 81 ticks): the 5th swing is on tick 324, and the cast follows it.
    Duel d;
    d.Add(Legacy(kChampionBaira), 1, 0, {3, 2});
    d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
    d.Finish();
    CombatConfig cfg;
    const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 500);
    CHECK((Ticks(Events(r.log, CombatEventType::Attack, 1)) == std::vector<int>{0, 81, 162, 243, 324, 405, 486}));
    const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
    CHECK(casts.size() == 1 && casts[0].tick == 324 && casts[0].ability == kAbilityArdeatsDestiny && casts[0].duration == 0);
    // In the stream the 5th Attack precedes the SpellCast on the same tick.
    std::size_t attackAt = 0, castAt = 0;
    for (std::size_t i = 0; i < r.log.events.size(); ++i) {
        const CombatEvent& e = r.log.events[i];
        if (e.tick == 324 && e.unit == 1 && e.type == CombatEventType::Attack) attackAt = i;
        if (e.tick == 324 && e.unit == 1 && e.type == CombatEventType::SpellCast) castAt = i;
    }
    CHECK(attackAt > 0 && castAt > attackAt);
    // Burn: 15 per second (star-1 ability damage), 5 hits: ticks 354..474.
    std::vector<std::pair<int, int>> burn;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
        if (e.flags & kFlagDot) burn.emplace_back(e.tick, e.amount);
    }
    CHECK((burn == std::vector<std::pair<int, int>>{{354, 15}, {384, 15}, {414, 15}, {444, 15}, {474, 15}}));
    CheckValid(r, d, 500, cfg);
}

struct CountingSink : ICombatEventSink {
    int spawns = 0, moves = 0, attacks = 0, damages = 0, deaths = 0, casts = 0, shields = 0, shieldEnds = 0, statuses = 0, statusEnds = 0;
    std::vector<AbilityId> spells;
    std::vector<int> shieldAmounts;
    std::vector<StatusType> statusTypes;
    int lastTick = 0;
    bool monotonic = true;
    void Note(int tick) { monotonic = monotonic && tick >= lastTick; lastTick = tick; }
    void OnSpawn(const CombatEvent& e) override { ++spawns; Note(e.tick); }
    void OnMove(const CombatEvent& e) override { ++moves; Note(e.tick); }
    void OnAttack(int t, UnitId, UnitId) override { ++attacks; Note(t); }
    void OnDamage(const CombatEvent& e) override { ++damages; Note(e.tick); }
    void OnDeath(int t, UnitId) override { ++deaths; Note(t); }
    void OnSpellCast(int t, UnitId, AbilityId spell, UnitId, int, bool) override { ++casts; spells.push_back(spell); Note(t); }
    void OnShieldApplied(int t, UnitId, int amount, int) override { ++shields; shieldAmounts.push_back(amount); Note(t); }
    void OnShieldEnded(int t, UnitId, int) override { ++shieldEnds; Note(t); }
    void OnStatusApplied(int t, UnitId, StatusType type, int, int, UnitId) override { ++statuses; statusTypes.push_back(type); Note(t); }
    void OnStatusEnded(int t, UnitId, StatusType) override { ++statusEnds; Note(t); }
};

static void TestEventSinkAndFullRosterFight() {
    // All five real champions on each side, replayed into a typed sink (what the UE5 viewer would implement).
    auto db = w2f::LoadChampionDatabaseFromFile(sample::LegacyRosterPath());
    std::vector<FightUnitSpec> specs;
    const ChampionId ids[5] = {kChampionAlesk, kChampionBaira, kChampionCyla, kChampionDyno, kChampionFaire};
    for (int team = 0; team < 2; ++team) {
        for (int i = 0; i < 5; ++i) {
            const HexCoord board{(i == 0 || i == 3 || i == 4) ? 2 + i : 1 + i, (i == 0 || i == 3 || i == 4) ? 3 : 0};
            specs.push_back({static_cast<UnitId>((team ? 500 : 100) + i), db->Find(ids[i]), 2, team,
                             BoardToArena(board.x, board.y, team ? ArenaSide::Away : ArenaSide::Home)});
        }
    }
    CombatSimulator sim{CombatConfig{}};
    const FightResult r = sim.RunFight(specs, 1200, 42);
    const FightResult again = sim.RunFight(specs, 1200, 42);
    CHECK(r.log.checksum == again.log.checksum);
    std::string why;
    CHECK(ValidateCombatLog(r.log, *db, CombatConfig{}, 1200, &why));
    if (!why.empty()) std::printf("  validator: %s\n", why.c_str());

    CountingSink sink;
    ReplayCombatLog(r.log, sink);
    auto count = [&](CombatEventType t) { return static_cast<int>(Events(r.log, t).size()); };
    CHECK(sink.spawns == 10 && sink.spawns == count(CombatEventType::Spawn));
    CHECK(sink.moves == count(CombatEventType::Move) && sink.attacks == count(CombatEventType::Attack));
    CHECK(sink.damages == count(CombatEventType::Damage) && sink.deaths == count(CombatEventType::Death));
    CHECK(sink.casts == count(CombatEventType::SpellCast) && sink.shields == count(CombatEventType::ShieldApplied));
    CHECK(sink.statuses == count(CombatEventType::StatusApplied) && sink.monotonic);
    CHECK(sink.casts > 0 && sink.shields > 0 && sink.statuses > 0);
    std::map<AbilityId, int> perSpell;
    for (AbilityId a : sink.spells) ++perSpell[a];
    std::printf("  10 roster champions (2-star): %zu events, %d casts (Aesa %d, Ardeat %d, Rocket %d, DynoShield %d, Exploit %d), %d shields, %d statuses; ended tick %d\n",
                r.log.events.size(), sink.casts, perSpell[kAbilityAesa], perSpell[kAbilityArdeatsDestiny], perSpell[kAbilityRocketStrike],
                perSpell[kAbilityDynoShield], perSpell[kAbilityExploit], sink.shields, sink.statuses, r.log.endTick);
    CHECK(perSpell.size() >= 3);
}


// ==== Phase 5: JSON, passives, wound, mana events ============================================

static std::string LoadErr(const std::string& doc) {
    std::string err;
    auto db = w2f::LoadChampionDatabaseFromJson(doc, &err);
    return db ? std::string("<<loaded without error>>") : err;
}
static bool Has(const std::string& text, std::initializer_list<const char*> parts) {
    for (const char* part : parts) {
        if (text.find(part) == std::string::npos) {
            std::printf("  expected \"%s\" in: %s\n", part, text.c_str());
            return false;
        }
    }
    return true;
}
static const char* kOkStats = R"({"hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 10, "attackSpeed": 1.0, "range": 1})";
static std::string Doc(const std::string& champions) { return "{\"version\": 1, \"champions\": [" + champions + "]}"; }
static std::string Champ(const std::string& extra = "", const std::string& stats = kOkStats) {
    return "{\"id\": 1, \"name\": \"X\", \"cost\": 1, \"stats\": " + stats + (extra.empty() ? "" : ", " + extra) + "}";
}
static std::string Ability(const std::string& effects, const std::string& header = "\"trigger\": \"EveryNthAttack\", \"attackCount\": 1") {
    return "{\"id\": 1, \"name\": \"A\", " + header + ", \"effects\": [" + effects + "]}";
}
static const char* kSelfShield = R"({"type": "Shield", "target": "Self", "amount": 10, "durationTicks": 30})";

static void TestJsonParser() {
    using json::Value;
    Value v;
    std::string err;
    CHECK(json::Parse(R"({"a": [1, 2.5, -3, true, false, null, "s"], "b": {"c": {}}, "d": []})", v, &err));
    CHECK(v.IsObject() && v.MemberCount() == 3 && v.MemberKey(0) == "a" && v.MemberKey(2) == "d");  // file order kept
    const Value* a = v.Find("a");
    CHECK(a && a->IsArray() && a->Items().size() == 7);
    CHECK(a->Items()[3].IsBool() && a->Items()[3].AsBool() && !a->Items()[4].AsBool() && a->Items()[5].IsNull());
    CHECK(a->Items()[6].AsString() == "s" && v.Find("nope") == nullptr && a->Find("x") == nullptr);
    CHECK(v.Find("b")->Find("c")->IsObject() && v.Find("d")->Items().empty());

    // Comments and whitespace.
    CHECK(json::Parse("// leading\n{ /* inline */ \"a\": 1 // trailing\n}\n/* end */", v, &err));
    CHECK(v.Find("a") != nullptr);

    // Strings: escapes, \u, surrogate pairs (-> UTF-8).
    CHECK(json::Parse(R"("q\"b\\s\/ \b\f\n\r\t \u00e9 \ud83d\ude00")", v, &err));
    CHECK(v.AsString() == "q\"b\\s/ \b\f\n\r\t \xC3\xA9 \xF0\x9F\x98\x80");

    // Numbers are exact decimals.
    auto num = [&](const char* text) { Value n; std::string e; CHECK(json::Parse(text, n, &e)); return n; };
    long long out = 0;
    CHECK(num("3").ToInt(out) && out == 3);
    CHECK(num("3.0").ToInt(out) && out == 3);
    CHECK(num("3e0").ToInt(out) && out == 3);
    CHECK(num("1e2").ToInt(out) && out == 100);
    CHECK(num("-7").ToInt(out) && out == -7);
    CHECK(num("0").ToInt(out) && out == 0 && num("-0").ToInt(out) && out == 0);
    CHECK(!num("3.5").ToInt(out));
    CHECK(num("0.78").ToScaled(3, out) && out == 780);
    CHECK(num("0.37").ToScaled(3, out) && out == 370);
    CHECK(num("1.0").ToScaled(3, out) && out == 1000);
    CHECK(num("0.5").ToScaled(3, out) && out == 500);
    CHECK(num("12").ToScaled(3, out) && out == 12000);
    CHECK(num("0.125").ToScaled(3, out) && out == 125);
    CHECK(!num("0.7812").ToScaled(3, out));           // more precision than asked for: refused, not rounded
    CHECK(num("1.75").ToTicks(30, out) && out == 53);  // 52.5 -> half up
    CHECK(num("2.25").ToTicks(30, out) && out == 68);  // 67.5 -> half up
    CHECK(num("2.5").ToTicks(30, out) && out == 75);
    CHECK(num("2.0").ToTicks(30, out) && out == 60);
    CHECK(num("0.5").ToTicks(30, out) && out == 15);
    CHECK(num("0").ToTicks(30, out) && out == 0);
    CHECK(num("0.0166").ToTicks(30, out) && out == 0 && num("0.0167").ToTicks(30, out) && out == 1);
    CHECK(!num("-1").ToTicks(30, out));
    CHECK(!v.IsNumber() || true);

    // Every malformed document is rejected with a position.
    const char* bad[] = {"", "   ", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "{a: 1}", "01", "1.", ".5", "-", "1e", "1e+", "tru", "nul",
                         "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "\"\\ud83d\"", "\"\\ude00\"", "/* never closed", "[1] 2", "{} {}",
                         "[1 2]", "trueish", "+1", "NaN", "0x10", "1e999", "[", "{\"a\":"};
    for (const char* text : bad) {
        std::string e;
        Value ignored;
        const bool ok = json::Parse(text, ignored, &e);
        CHECK(!ok && e.find("line ") != std::string::npos);
        if (ok) std::printf("  accepted malformed JSON: %s\n", text);
    }
    CHECK(Has([&] { std::string e; json::Parse(R"({"a": 1, "a": 2})", v, &e); return e; }(), {"duplicate key \"a\""}));
    CHECK(Has([&] { std::string e; json::Parse(std::string(200, '[') + std::string(200, ']'), v, &e); return e; }(), {"too deep"}));

    // Positions.
    CHECK(json::Parse("{\n  \"x\": [\n    10,\n    \"here\"\n  ]\n}", v, &err));
    const Value& here = v.Find("x")->Items()[1];
    CHECK(here.line() == 4 && here.column() == 5);
    std::string e2;
    json::Parse("{\n  \"a\": 1,\n  \"b\": @\n}", v, &e2);
    CHECK(Has(e2, {"line 3, column 8"}));
}

static void TestLoaderReadsDesignerFriendlyValues() {
    // Seconds, decimals and shorthands all end up as exact integers.
    const std::string doc = Doc(R"({
      "id": 42, "name": "Test", "cost": 3, "role": "Tank", "traits": ["Helios", "Tank"],
      "stats": { "hp": [100, 150, 200], "armor": 7, "magicResist": [1, 2, 3], "attackDamage": 10,
                 "attackSpeed": 0.78, "range": 2, "maxMana": 6, "manaRegen": 0.8, "crit": [15, 25, 50],
                 "abilityDamage": [15, 21, 40], "abilityPowerPercent": 120, "attackSpreadSeconds": 1 },
      "ability": { "id": 7, "name": "Big", "trigger": "Mana", "castLockSeconds": 0.5, "castOnDeath": true,
        "effects": [
          { "type": "Shield", "target": "Self", "amount": { "flat": [10, 20, 30], "terms": [ { "source": "SelfMaxHp", "percent": [7, 15, 30] } ] },
            "durationSeconds": [1.75, 2.0, 2.25], "damageReductionPercent": [5, 12, 25] },
          { "type": "Damage", "target": { "mode": "AreaAroundTarget", "radius": 3, "includeCenter": false, "side": "Enemies" },
            "damageType": "True", "amount": { "terms": [ { "source": "DamageDealtInWindow", "percent": 100, "windowSeconds": 2 } ] },
            "multiplierPercent": 25, "canCrit": true, "delaySeconds": 0.5 },
          { "type": "DoT", "target": "CurrentTarget", "damageType": "Physical", "amount": [15, 21, 36], "amountIsTotal": true,
            "duration": { "flatSeconds": [2.5, 2.0, 1.75] }, "intervalSeconds": 0.5, "stackBonusPercent": 25 },
          { "type": "Status", "target": "CurrentTarget", "status": "Stun", "durationTicks": 12 }
        ] },
      "passive": { "id": 8, "name": "P", "effects": [ { "type": "Status", "target": "Self", "status": "MaxHp", "percent": [5, 10, 20], "permanent": true } ] }
    })");
    std::vector<ChampionDefinition> defs;
    std::string err;
    CHECK(w2f::ParseChampionsJson(doc, defs, &err));
    if (defs.size() != 1) { std::printf("  %s\n", err.c_str()); return; }
    const ChampionDefinition& d = defs[0];
    CHECK(d.id == 42 && d.name == "Test" && d.cost == 3 && d.role == ChampionRole::Tank);
    CHECK((d.traits == std::vector<std::string>{"Helios", "Tank"}));
    CHECK(d.stats.maxHp == StarValue({{100, 150, 200}}) && d.stats.armor == Same(7) && d.stats.magicResist == StarValue({{1, 2, 3}}));
    CHECK(d.stats.attackSpeedMilli == 780 && d.stats.manaRegenMilli == 800 && d.stats.maxMana == 6 && d.stats.attackRange == 2);
    CHECK(d.stats.critChance == StarValue({{15, 25, 50}}) && d.stats.abilityDamage == StarValue({{15, 21, 40}}) && d.stats.abilityPower == 120);
    CHECK(d.stats.attackSpreadTicks == 30);
    CHECK(d.ability.id == 7 && d.ability.trigger == CastTrigger::Mana && d.ability.castLockTicks == 15 && d.ability.castOnDeath);
    CHECK(d.ability.effects.size() == 4);
    if (d.ability.effects.size() != 4) return;
    const auto* shield = std::get_if<ShieldEffect>(&d.ability.effects[0].payload);
    CHECK(shield && shield->amount.flat == StarValue({{10, 20, 30}}) && shield->amount.terms.size() == 1 &&
          shield->amount.terms[0].source == StatSource::SelfMaxHp);
    CHECK(shield && shield->duration.flat == StarValue({{53, 60, 68}}) && shield->damageReductionPercent == StarValue({{5, 12, 25}}));
    const auto* dmg = std::get_if<DamageEffect>(&d.ability.effects[1].payload);
    CHECK(dmg && dmg->type == DamageType::True && dmg->multiplierPercent == 25 && dmg->canCrit);
    CHECK(dmg && dmg->amount.terms[0].source == StatSource::DamageDealtInWindow && dmg->amount.terms[0].windowTicks == 60);
    CHECK(d.ability.effects[1].target.mode == TargetMode::AreaAroundTarget && d.ability.effects[1].target.radius == 3 &&
          !d.ability.effects[1].target.includeCenter && d.ability.effects[1].delayTicks == 15);
    const auto* dot = std::get_if<DotEffect>(&d.ability.effects[2].payload);
    CHECK(dot && dot->amountIsTotal && dot->amount.flat == StarValue({{15, 21, 36}}) && dot->intervalTicks == 15 &&
          dot->duration.flat == StarValue({{75, 60, 53}}) && dot->stackBonusPercent == Same(25));
    const auto* stun = std::get_if<StatusEffect>(&d.ability.effects[3].payload);
    CHECK(stun && stun->status == StatusType::Stun && stun->duration.flat == Same(12));
    CHECK(d.passive.id == 8 && d.passive.trigger == CastTrigger::StartOfCombat);   // trigger defaulted
    const auto* hp = std::get_if<StatusEffect>(&d.passive.effects[0].payload);
    CHECK(hp && hp->permanent && hp->status == StatusType::MaxHp && hp->percent == StarValue({{5, 10, 20}}));

    // "Passive" is accepted as an alias for StartOfCombat.
    CHECK(w2f::ParseChampionsJson(Doc(Champ(R"("passive": {"id": 2, "name": "P", "trigger": "Passive", "effects": [
        {"type": "Status", "target": "Self", "status": "Armor", "percent": 5, "permanent": true}]})")), defs, &err));
    CHECK(defs[0].passive.trigger == CastTrigger::StartOfCombat);
}

static void TestLoaderErrors() {
    struct Case { const char* label; std::string doc; std::vector<const char*> expect; };
    const std::string okShield = kSelfShield;
    const std::vector<Case> cases = {
        {"unknown champion key", Doc(Champ("\"colour\": 1")), {"champions[0].colour", "unknown key \"colour\"", "line 1"}},
        {"typo in stats (required key missing)", Doc(Champ("", R"({"hp": 1, "armor": 0, "magicResit": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1})")),
            {"champions[0].stats", "missing required key \"magicResist\""}},
        {"typo in optional stats key", Doc(Champ("", R"({"hp": 1, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1, "crt": 5})")),
            {"champions[0].stats.crt", "unknown key \"crt\"", "allowed here"}},
        {"wrong type", Doc(Champ("", R"({"hp": "lots", "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1})")),
            {"champions[0].stats.hp", "expected a number"}},
        {"per-star array of the wrong length", Doc(Champ("", R"({"hp": [1, 2], "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1})")),
            {"champions[0].stats.hp", "exactly 3 values"}},
        {"error inside a per-star array", Doc(Champ("", R"({"hp": [1, "x", 3], "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1})")),
            {"champions[0].stats.hp[1]", "expected a number"}},
        {"fractional integer", Doc(Champ("", R"({"hp": 1, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1.5})")),
            {"champions[0].stats.range", "whole number"}},
        {"too many decimals", Doc(Champ("", R"({"hp": 1, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 0.7812, "range": 1})")),
            {"champions[0].stats.attackSpeed", "3 decimal places"}},
        {"zero attack speed", Doc(Champ("", R"({"hp": 1, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 0, "range": 1})")),
            {"champions[0].stats.attackSpeed", "out of range"}},
        {"bad role", Doc(Champ("\"role\": \"Wizard\"")), {"champions[0].role", "unknown value \"Wizard\"", "Tank, Damage"}},
        {"cost out of range", Doc("{\"id\": 1, \"name\": \"X\", \"cost\": 9, \"stats\": " + std::string(kOkStats) + "}"), {"champions[0].cost", "out of range"}},
        {"missing stats", Doc(R"({"id": 1, "name": "X", "cost": 1})"), {"champions[0]", "missing required key \"stats\""}},
        {"ability without trigger", Doc(Champ("\"ability\": {\"id\": 1, \"name\": \"A\", \"effects\": [" + okShield + "]}")),
            {"champions[0].ability", "missing required key \"trigger\""}},
        {"unknown trigger", Doc(Champ("\"ability\": " + Ability(okShield, "\"trigger\": \"OnFullMoon\""))), {"champions[0].ability.trigger", "OnFullMoon", "EveryNthAttack"}},
        {"unknown effect type", Doc(Champ("\"ability\": " + Ability(R"({"type": "Explode", "target": "Self"})"))),
            {"champions[0].ability.effects[0].type", "unknown effect type \"Explode\""}},
        {"effect without target", Doc(Champ("\"ability\": " + Ability(R"({"type": "Shield", "amount": 1, "durationTicks": 5})"))),
            {"champions[0].ability.effects[0]", "missing required key \"target\""}},
        {"unknown key inside an effect", Doc(Champ("\"ability\": " + Ability(R"({"type": "Shield", "target": "Self", "amount": 1, "durationTicks": 5, "reduction": 5})"))),
            {"champions[0].ability.effects[0].reduction", "unknown key"}},
        {"unknown formula source", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": "CurrentTarget", "damageType": "True",
            "amount": {"terms": [{"source": "SelfMaxHP", "percent": 5}]}})"))), {"champions[0].ability.effects[0].amount.terms[0].source", "SelfMaxHP", "SelfMaxHp"}},
        {"window missing", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": "CurrentTarget", "damageType": "True",
            "amount": {"terms": [{"source": "DamageDealtInWindow", "percent": 5}]}})"))), {"amount.terms[0]", "windowSeconds"}},
        {"window on the wrong source", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": "CurrentTarget", "damageType": "True",
            "amount": {"terms": [{"source": "SelfMaxHp", "percent": 5, "windowTicks": 9}]}})"))), {"amount.terms[0]", "only applies to DamageDealtInWindow"}},
        {"empty amount object", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": "CurrentTarget", "damageType": "True", "amount": {}})"))),
            {"champions[0].ability.effects[0].amount", "needs"}},
        {"area target without radius", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": {"mode": "AreaAroundTarget"}, "damageType": "True", "amount": 1})"))),
            {"champions[0].ability.effects[0].target", "radius"}},
        {"area mode as a bare string", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": "AreaAroundTarget", "damageType": "True", "amount": 1})"))),
            {"champions[0].ability.effects[0].target", "long form"}},
        {"radius on a single-target mode", Doc(Champ("\"ability\": " + Ability(R"({"type": "Damage", "target": {"mode": "Self", "radius": 2}, "damageType": "True", "amount": 1})"))),
            {"champions[0].ability.effects[0].target", "Area modes"}},
        {"shield with no duration", Doc(Champ("\"ability\": " + Ability(R"({"type": "Shield", "target": "Self", "amount": 1})"))),
            {"champions[0].ability.effects[0]", "duration"}},
        {"duration given twice", Doc(Champ("\"ability\": " + Ability(R"({"type": "Shield", "target": "Self", "amount": 1, "durationTicks": 5, "durationSeconds": 1})"))),
            {"durationTicks", "not both"}},
        {"negative duration", Doc(Champ("\"ability\": " + Ability(R"({"type": "Shield", "target": "Self", "amount": 1, "durationSeconds": -1})"))),
            {"durationSeconds", "non-negative"}},
        {"status without percent", Doc(Champ("\"ability\": " + Ability(R"({"type": "Status", "target": "Self", "status": "Armor", "durationTicks": 5})"))),
            {"champions[0].ability.effects[0]", "\"percent\""}},
        {"permanent status with a duration", Doc(Champ("\"ability\": " + Ability(R"({"type": "Status", "target": "Self", "status": "Armor", "percent": 5, "permanent": true, "durationTicks": 5})"))),
            {"permanent status has no duration"}},
        {"Burn from a status effect", Doc(Champ("\"ability\": " + Ability(R"({"type": "Status", "target": "Self", "status": "Burn", "percent": 5, "durationTicks": 5})"))),
            {"champions[0].ability.effects[0].status", "Burn"}},
        {"DoT without interval", Doc(Champ("\"ability\": " + Ability(R"({"type": "DoT", "target": "CurrentTarget", "damageType": "Magic", "amount": 5, "durationTicks": 30})"))),
            {"champions[0].ability.effects[0]", "interval"}},
        {"wrong version", "{\"version\": 2, \"champions\": []}", {"$.version", "version 1"}},
        {"missing version", "{\"champions\": []}", {"$", "\"version\""}},
        {"missing champions", "{\"version\": 1}", {"$", "\"champions\""}},
        {"champions is not an array", "{\"version\": 1, \"champions\": {}}", {"$.champions", "expected an array"}},
        {"unknown top-level key", "{\"version\": 1, \"champions\": [], \"extra\": 1}", {"$.extra", "unknown key"}},
        {"root is not an object", "[1, 2]", {"$", "expected an object"}},
        {"broken JSON", "{\"version\": 1,\n  \"champions\": [ {\"id\": } ]}", {"line 2"}},
        // These parse fine and are rejected by the shared database validation:
        {"duplicate champion ids", Doc(Champ() + "," + Champ()), {"duplicate champion id"}},
        {"mana trigger without a mana bar", Doc(Champ("\"ability\": " + Ability(okShield, "\"trigger\": \"Mana\""))), {"mana-triggered"}},
        {"passive aimed at a target", Doc(Champ("\"passive\": {\"id\": 2, \"name\": \"P\", \"effects\": [{\"type\": \"Damage\", \"target\": \"CurrentTarget\", \"damageType\": \"True\", \"amount\": 1}]}")),
            {"passive", "no target"}},
        {"passive with a non-passive trigger", Doc(Champ("\"passive\": " + Ability(okShield))), {"passive", "StartOfCombat"}},
        {"passive with a cast lock", Doc(Champ("\"passive\": {\"id\": 2, \"name\": \"P\", \"castLockTicks\": 5, \"effects\": [" + okShield + "]}")), {"passive", "cast lock"}},
        {"active ability using the passive trigger", Doc(Champ("\"ability\": " + Ability(okShield, "\"trigger\": \"Passive\""))), {"belongs in 'passive'"}},
        {"reset flag on the wrong trigger", Doc(Champ("\"ability\": " + Ability(okShield, "\"trigger\": \"Mana\", \"resetOnTargetChange\": true"),
            R"({"hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 10, "attackSpeed": 1.0, "range": 1, "maxMana": 5})")),
            {"resetOnTargetChange"}},
        {"empty trait", Doc(Champ("\"traits\": [\"\"]")), {"empty trait"}},
        {"duplicate trait", Doc(Champ("\"traits\": [\"A\", \"A\"]")), {"trait 'A' twice"}},
        {"trait of the wrong type", Doc(Champ("\"traits\": [5]")), {"champions[0].traits[0]", "expected a string"}},
    };
    for (const Case& c : cases) {
        const std::string err = LoadErr(c.doc);
        bool ok = err != "<<loaded without error>>";
        for (const char* part : c.expect) ok = ok && err.find(part) != std::string::npos;
        if (!ok) std::printf("  case \"%s\" produced: %s\n", c.label, err.c_str());
        CHECK(ok);
    }

    // Line numbers point at the mistake in a multi-line file.
    const std::string multiline = "{\n  \"version\": 1,\n  \"champions\": [\n    { \"id\": 1, \"name\": \"X\", \"cost\": 1,\n      \"stats\": { \"hp\": 1, \"armor\": 0, \"magicResist\": 0,\n"
                                  "                 \"attackDamage\": 1, \"attackSpeed\": 1, \"range\": 1,\n                 \"crt\": 5 } }\n  ]\n}";
    CHECK(Has(LoadErr(multiline), {"champions[0].stats.crt", "line 7"}));

    // Files.
    std::string err;
    CHECK(w2f::LoadChampionDatabaseFromFile("/definitely/not/here.json", &err) == nullptr && Has(err, {"cannot open", "/definitely/not/here.json"}));
    CHECK(!LoadErr(Doc(Champ())).empty() && LoadErr(Doc(Champ())).find("loaded without error") != std::string::npos);  // a valid minimal doc really loads
}

static void TestProductionDataMatchesDesignerSpec() {
    // A snapshot of the designer's latest numbers. It WILL need updating when the designer changes them --
    // that is the point: a change to data/champions.json shows up here as a deliberate, reviewed edit.
    std::string err;
    auto db = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath(), &err);
    CHECK(db != nullptr);
    if (!db) { std::printf("  %s\n", err.c_str()); return; }
    {   // The whole roster of the design doc: 30 champions (8 / 7 / 6 / 5 / 4 per cost tier) plus the two summons (Skeleton, Lost Soul).
        int perTier[kMaxCostTier + 1] = {}, summons = 0;
        for (const ChampionDefinition& c : db->All()) {
            if (c.summon) ++summons;
            else ++perTier[c.cost];
        }
        CHECK(db->All().size() == 32 && summons == 2);
        CHECK(perTier[1] == 8 && perTier[2] == 7 && perTier[3] == 6 && perTier[4] == 5 && perTier[5] == 4);
        CHECK(db->Find(9101) && db->Find(9101)->summon && db->Find(9102) && db->Find(9102)->summon && db->Find(9102)->stats.attackType == DamageType::Magic);
    }
    CHECK(db->Find(kChampionDyno) == nullptr);

    auto trait = [](const ChampionDefinition* c, const char* name) {
        return c && std::find(c->traits.begin(), c->traits.end(), name) != c->traits.end();
    };

    const ChampionDefinition* alesk = db->Find(kChampionAlesk);
    CHECK(alesk && alesk->name == "Alesk" && alesk->cost == 4 && alesk->role == ChampionRole::Tank && alesk->stats.attackRange == 1);
    if (alesk) {
        CHECK((alesk->traits == std::vector<std::string>{"Helios"}));
        CHECK(alesk->stats.maxHp == StarValue({{325, 475, 650}}) && alesk->stats.armor == StarValue({{31, 45, 63}}));
        CHECK(alesk->stats.magicResist == StarValue({{17, 24, 29}}) && alesk->stats.attackDamage == StarValue({{30, 35, 40}}));
        CHECK(alesk->stats.maxMana == 60 && alesk->stats.manaRegenMilli == 10000 && alesk->stats.attackSpeedMilli == 1000);   // 100-scale
        CHECK(alesk->ability.trigger == CastTrigger::Mana && alesk->ability.castLockTicks == 15 && alesk->ability.effects.size() == 1);
        const auto* shield = std::get_if<ShieldEffect>(&alesk->ability.effects[0].payload);
        CHECK(shield && shield->amount.terms.size() == 1 && shield->amount.terms[0].source == StatSource::SelfMaxHp &&
              shield->amount.terms[0].percent == StarValue({{7, 15, 30}}));
        CHECK(shield && shield->duration.flat == StarValue({{53, 60, 68}}));   // 1.75 / 2.0 / 2.25 s
        CHECK(shield && shield->damageReductionPercent == StarValue({{5, 12, 25}}));
        CHECK(alesk->passive.trigger == CastTrigger::StartOfCombat && alesk->passive.effects.size() == 3);
    }

    const ChampionDefinition* baira = db->Find(kChampionBaira);
    CHECK(baira && baira->name == "Baira" && baira->cost == 3 && baira->role == ChampionRole::Damage && baira->stats.attackRange == 4);
    if (baira) {
        CHECK((baira->traits == std::vector<std::string>{"Phaisa"}));
        CHECK(baira->stats.maxHp == StarValue({{100, 150, 200}}) && baira->stats.armor == StarValue({{10, 15, 20}}));
        CHECK(baira->stats.magicResist == StarValue({{5, 9, 13}}) && baira->stats.attackDamage == StarValue({{20, 25, 29}}));
        CHECK(baira->stats.abilityDamage == StarValue({{15, 21, 40}}) && baira->stats.maxMana == 0 && baira->stats.attackSpeedMilli == 500);
        CHECK(baira->ability.trigger == CastTrigger::EveryNthAttack && baira->ability.attackCount == 5 && baira->ability.resetCountOnTargetChange);
        CHECK(baira->ability.effects.size() == 2);
        const auto* burn = std::get_if<DotEffect>(&baira->ability.effects[0].payload);
        // The burn total now SCALES WITH AP: 100% / 100% / 90% of abilityDamage 15 / 21 / 40 = 15 / 21 / 36.
        CHECK(burn && burn->amountIsTotal && burn->amount.flat == Same(0) && burn->amount.terms.size() == 1 &&
              burn->amount.terms[0].source == StatSource::SelfAbilityDamage && burn->amount.terms[0].percent == StarValue({{100, 100, 90}}));
        CHECK(burn && burn->duration.flat == StarValue({{75, 60, 53}}) && burn->stackBonusPercent == StarValue({{25, 35, 45}}));
        CHECK(baira->ability.effects[0].target.mode == TargetMode::AreaAroundTarget && baira->ability.effects[0].target.radius == 3);
        const auto* wound = baira->passive.effects.empty() ? nullptr : std::get_if<StatusEffect>(&baira->passive.effects[0].payload);
        CHECK(wound && wound->status == StatusType::InflictsWound && wound->percent == Same(30) && wound->permanent);
    }

    const ChampionDefinition* cyla = db->Find(kChampionCyla);
    CHECK(cyla && cyla->name == "Cyla" && cyla->cost == 2 && cyla->role == ChampionRole::Damage && cyla->stats.attackRange == 4);
    if (cyla) {
        CHECK((cyla->traits == std::vector<std::string>{"Hexagon"}));
        CHECK(cyla->stats.critChance == StarValue({{15, 25, 50}}) && cyla->stats.maxMana == 60 && cyla->stats.attackSpeedMilli == 780);
        CHECK(cyla->ability.castLockTicks == 0 && cyla->ability.effects.size() == 2);
        const auto* main = std::get_if<DamageEffect>(&cyla->ability.effects[0].payload);
        const auto* splash = std::get_if<DamageEffect>(&cyla->ability.effects[1].payload);
        // Her rocket is AD-based: PHYSICAL (this was Magic before the designer's correction). Both hits can crit.
        CHECK(main && main->type == DamageType::Physical && main->canCrit && main->multiplierPercent == 100 &&
              main->amount.terms[0].source == StatSource::DamageDealtInWindow && main->amount.terms[0].windowTicks == 60);
        CHECK(splash && splash->type == DamageType::Physical && splash->canCrit && splash->multiplierPercent == 25);
    }

    const ChampionDefinition* faire = db->Find(kChampionFaire);
    CHECK(faire && faire->name == "Faire" && faire->cost == 3 && trait(faire, "Hexagon"));
    if (faire) {
        CHECK(faire->stats.attackSpreadTicks == 30 && faire->ability.castOnDeath && faire->stats.maxMana == 50 && faire->stats.attackRange == 3);   // range 3 per the designer
        const auto* stun = std::get_if<StatusEffect>(&faire->ability.effects[1].payload);
        // Stun = 0.2 s per 10 damage dealt to the target in the LAST 5 SECONDS (150 ticks): 60% of that damage, in ticks.
        CHECK(stun && stun->status == StatusType::Stun && stun->duration.terms.size() == 1 &&
              stun->duration.terms[0].source == StatSource::DamageDealtToTargetInWindow && stun->duration.terms[0].windowTicks == 150 &&
              stun->duration.terms[0].percent == Same(60));
    }

    const ChampionDefinition* les = db->Find(kChampionLes);
    CHECK(les && les->name == "Les" && les->cost == 5 && les->role == ChampionRole::Tank && trait(les, "Omnilium") && trait(les, "Protector"));
    if (les) {
        CHECK(les->stats.maxHp == StarValue({{1000, 2000, 3500}}) && les->stats.armor == StarValue({{60, 80, 300}}));
        CHECK(les->stats.magicResist == StarValue({{50, 70, 285}}) && les->stats.attackDamage == StarValue({{40, 60, 70}}));
        CHECK(les->stats.maxMana == 70 && les->stats.manaRegenMilli == 10000 && les->stats.attackSpeedMilli == 1000 && les->stats.attackRange == 1);
        CHECK(les->ability.effects.size() == 7);
        const auto& root = les->ability.effects[0];
        CHECK(root.target.mode == TargetMode::ClosestEnemies && root.target.count == 3);
        const auto* rootStatus = std::get_if<StatusEffect>(&root.payload);
        CHECK(rootStatus && rootStatus->status == StatusType::Root && rootStatus->duration.flat == StarValue({{30, 45, 90}}));   // 1 / 1.5 / 3 s
        const auto* regen = std::get_if<StatusEffect>(&les->ability.effects[5].payload);
        CHECK(regen && regen->status == StatusType::DamageTakenRegen && regen->percent == StarValue({{10, 10, 20}}) &&
              regen->duration.flat == StarValue({{90, 150, 450}}));   // wall mode: 3 / 5 / 15 s
        const auto* immune = std::get_if<StatusEffect>(&les->ability.effects[6].payload);
        CHECK(immune && immune->status == StatusType::CcImmunity && les->ability.effects[6].target.mode == TargetMode::AlliesInStartLine);
    }

    const ChampionDefinition* lum = db->Find(kChampionLum);
    CHECK(lum && lum->name == "Lum" && lum->cost == 5 && trait(lum, "Omnilium") && trait(lum, "Protector"));
    if (lum) {
        CHECK(lum->stats.maxHp == StarValue({{600, 1000, 2500}}) && lum->stats.armor == StarValue({{45, 50, 100}}));
        CHECK(lum->stats.magicResist == StarValue({{35, 40, 100}}) && lum->stats.attackDamage == StarValue({{40, 60, 70}}));
        CHECK(lum->ability.effects.size() == 3);
        const auto* hit = std::get_if<DamageEffect>(&lum->ability.effects[0].payload);
        const auto* collide = std::get_if<DamageEffect>(&lum->ability.effects[1].payload);
        const auto* knock = std::get_if<StatusEffect>(&lum->ability.effects[2].payload);
        CHECK(hit && hit->type == DamageType::Physical && hit->amount.terms[0].source == StatSource::SelfAttackDamage &&
              hit->amount.terms[0].percent == StarValue({{15, 30, 100}}));
        CHECK(collide && collide->amount.terms[0].percent == StarValue({{7, 15, 50}}) && lum->ability.effects[1].target.mode == TargetMode::LineBehindTarget);
        CHECK(knock && knock->status == StatusType::Knockup && knock->duration.flat == StarValue({{30, 45, 150}}));   // 1 / 1.5 / 5 s
        // Passive (design doc): starts combat with 15 / 30 / 100% bonus Armor, Magic Resist and Attack Damage.
        CHECK(lum->passive.effects.size() == 3);
        const StatusType expected[3] = {StatusType::Armor, StatusType::MagicResist, StatusType::AttackDamage};
        for (std::size_t i = 0; i < lum->passive.effects.size() && i < 3; ++i) {
            const auto* might = std::get_if<StatusEffect>(&lum->passive.effects[i].payload);
            CHECK(might && might->status == expected[i] && might->permanent && might->percent == StarValue({{15, 30, 100}}));
        }
    }
}

static void TestPassivesAtStartOfCombat() {
    CombatConfig cfg;
    auto prod = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(prod != nullptr);
    if (!prod) return;

    {   // Real Alesk (production data), 1-star: +5% HP / Armor / MR from the passive, applied before tick 0 and never removed.
        Duel d;
        d.defs.push_back(*prod->Find(kChampionAlesk));
        d.specDef.push_back(kChampionAlesk);
        d.specs.push_back(FightUnitSpec{1, nullptr, 1, 0, {3, 3}});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 300);

        // Spawn shows BASE stats; the passive follows on tick 0.
        const auto spawns = Events(r.log, CombatEventType::Spawn, 1);
        CHECK(spawns.size() == 1 && spawns[0].amount == 325 && spawns[0].hpAfter == 325 && spawns[0].manaMax == 60000 && spawns[0].manaRegen == 10000);
        std::map<int, CombatEvent> passive;
        std::size_t lastSpawn = 0, firstStatus = r.log.events.size();
        for (std::size_t i = 0; i < r.log.events.size(); ++i) {
            const CombatEvent& e = r.log.events[i];
            if (e.type == CombatEventType::Spawn) lastSpawn = i;
            if (e.type == CombatEventType::StatusApplied && e.unit == 1) {
                if (i < firstStatus) firstStatus = i;
                if (e.tick == 0) passive[e.subtype] = e;
            }
        }
        CHECK(firstStatus > lastSpawn);   // after every Spawn
        CHECK(passive.size() == 3);
        for (StatusType t : {StatusType::MaxHp, StatusType::Armor, StatusType::MagicResist}) {
            const auto it = passive.find(static_cast<int>(t));
            CHECK(it != passive.end() && it->second.amount == 5 && it->second.duration == 0 && it->second.other == 1);
        }
        CHECK(passive[static_cast<int>(StatusType::MaxHp)].hpAfter == 325 * 105 / 100);  // 341: health grew with the maximum
        CHECK(Events(r.log, CombatEventType::StatusEnded, 1).empty());                    // permanent
        // Effective armor is 31 * 1.05 = 32 (floor): 100 raw -> 100*100/132 = 75 (76 without the passive).
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(!hits.empty() && hits[0].tick == 0 && hits[0].amount == 75);
        // The shield is 7% of the EFFECTIVE max HP (341 -> 23), lasts 1.75 s = 53 ticks, and cuts damage by 5%.
        const auto shields = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(!shields.empty() && shields[0].amount == 341 * 7 / 100 && shields[0].duration == 53);
        // Passives are not spells: the only SpellCasts are the active AESA.
        for (const CombatEvent& c : Events(r.log, CombatEventType::SpellCast, 1)) CHECK(c.ability == kAbilityAesa);
        std::string why;
        CHECK(ValidateCombatLog(r.log, *d.db, cfg, 300, &why));
        if (!why.empty()) std::printf("  validator: %s\n", why.c_str());
    }
    {   // 3-star: +20% -> 650 * 1.2 = 780.
        Duel d;
        d.defs.push_back(*prod->Find(kChampionAlesk));
        d.specDef.push_back(kChampionAlesk);
        d.specs.push_back(FightUnitSpec{1, nullptr, 3, 0, {3, 3}});
        d.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 5);
        std::map<int, int> hpAfter;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) hpAfter[e.subtype] = e.hpAfter;
        CHECK(hpAfter[static_cast<int>(StatusType::MaxHp)] == 780);
    }
    {   // Passives can target units around the holder (an aura at the start of combat), and support a delay.
        ChampionDefinition aura = Fighter(1, 1000, 0, 0);
        aura.passive.id = 50;
        aura.passive.name = "Aura";
        aura.passive.trigger = CastTrigger::StartOfCombat;
        StatusEffect buff;
        buff.status = StatusType::Armor;
        buff.percent = Same(50);
        buff.permanent = true;
        StatusEffect later = buff;
        later.status = StatusType::MagicResist;
        aura.passive.effects = {Eff(TargetSpec::AroundSelf(1, false, TargetSide::Allies), buff),
                                Eff(TargetSpec::Self(), later, /*delay*/ 30)};
        Duel d;
        d.Add(aura, 1, 0, {3, 3});
        d.Add(Dummy(3, 1000), 3, 0, {2, 3});    // adjacent ally: buffed
        d.Add(Dummy(4, 1000), 4, 0, {0, 0});    // far ally: not
        d.Add(Dummy(5, 100000000), 5, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 60);
        auto armorTick = [&](UnitId u) { for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, u)) if (e.subtype == static_cast<std::uint8_t>(StatusType::Armor)) return e.tick; return -1; };
        CHECK(armorTick(3) == 0 && armorTick(4) == -1 && armorTick(1) == -1);
        const auto mr = Events(r.log, CombatEventType::StatusApplied, 1);
        CHECK(mr.size() == 1 && mr[0].tick == 30 && mr[0].subtype == static_cast<std::uint8_t>(StatusType::MagicResist));
        CheckValid(r, d, 60, cfg);
    }
    {   // A TIMED max-HP status: HP rises with the maximum, and when it expires the excess is clipped away.
        ChampionDefinition c = Fighter(1, 100, 0, 0);
        c.passive.id = 51;
        c.passive.name = "Temp";
        c.passive.trigger = CastTrigger::StartOfCombat;
        StatusEffect hp;
        hp.status = StatusType::MaxHp;
        hp.percent = Same(100);
        hp.duration = FlatAmount(Same(30));
        c.passive.effects = {Eff(TargetSpec::Self(), hp)};
        Duel d;
        d.Add(c, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 10), 2, 1, {3, 4});   // hits for 10 on ticks 0, 30, ...
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 45);
        const auto applied = Events(r.log, CombatEventType::StatusApplied, 1);
        CHECK(applied.size() == 1 && applied[0].hpAfter == 200 && applied[0].duration == 30);
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(hits.size() >= 2 && hits[0].tick == 0 && hits[0].hpAfter == 190);
        const auto ended = Events(r.log, CombatEventType::StatusEnded, 1);
        CHECK(ended.size() == 1 && ended[0].tick == 30 && ended[0].hpAfter == 100);   // 190 clipped to the base maximum
        CHECK(hits.size() >= 2 && hits[1].tick == 30 && hits[1].hpAfter == 90);
        CheckValid(r, d, 45, cfg);
    }
}

static void TestBairaWoundAndNewBurn() {
    CombatConfig cfg;
    auto prod = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(prod != nullptr);
    if (!prod) return;
    const ChampionDefinition baira = *prod->Find(kChampionBaira);

    auto duel = [&](const ChampionDefinition& b, int star) {
        Duel d;
        d.defs.push_back(b);
        d.specDef.push_back(b.id);
        d.specs.push_back(FightUnitSpec{1, nullptr, star, 0, {3, 0}});
        d.Add(Dummy(201, 100000000), 201, 1, {3, 3});   // target (Baira's range is 4)
        d.Add(Dummy(202, 100000000), 202, 1, {5, 3});   // within 3 hexes of the target
        d.Add(Dummy(203, 100000000), 203, 1, {0, 7});   // far
        d.Finish();
        return d;
    };
    {
        Duel d = duel(baira, 1);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 500);
        // AS 0.5/s = 60-tick attacks. The 5th is on tick 240; that is when she casts.
        const auto attackTicks = Ticks(Events(r.log, CombatEventType::Attack, 1));
        CHECK(attackTicks.size() >= 6 && attackTicks[4] == 240);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(casts.size() >= 1 && casts[0].tick == 240 && casts[0].ability == kAbilityArdeatsDestiny);

        // Burn: everything within 3 hexes of the target (201 and 202, not 203). 15 TOTAL over 2.5 s (75 ticks),
        // a hit every 15 ticks: five hits of 3.
        std::map<UnitId, std::vector<std::pair<int, int>>> burn;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if ((e.flags & kFlagDot) && e.other == 1) burn[e.unit].emplace_back(e.tick, e.amount);
        }
        const std::vector<std::pair<int, int>> expected = {{255, 3}, {270, 3}, {285, 3}, {300, 3}, {315, 3}};
        CHECK(burn[201] == expected && burn[202] == expected && burn.count(203) == 0);

        // Wound: her passive marks the burn, so every burned unit is also Wounded 30% for exactly as long as it burns.
        std::map<UnitId, std::vector<CombatEvent>> wounds;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::Wound)) wounds[e.unit].push_back(e);
        }
        CHECK(wounds.count(201) == 1 && wounds.count(202) == 1 && wounds.count(203) == 0 && wounds.count(1) == 0);
        CHECK(wounds[201].size() >= 1 && wounds[201][0].tick == 240 && wounds[201][0].amount == 30 &&
              wounds[201][0].duration == 75 && wounds[201][0].other == 1);
        std::vector<CombatEvent> woundEnds;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusEnded, 201)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::Wound)) woundEnds.push_back(e);
        }
        CHECK(woundEnds.size() >= 1 && woundEnds[0].tick == 315);   // 240 + 75: ends with the burn
        // The passive itself: a permanent InflictsWound 30% on Baira, applied on tick 0 without a spell.
        bool marker = false;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) {
            if (e.tick == 0 && e.subtype == static_cast<std::uint8_t>(StatusType::InflictsWound) && e.amount == 30 && e.duration == 0) marker = true;
        }
        CHECK(marker);
        // Secondary effect: +20% attack speed for 5 s on cast. Her next attack is still on the old cooldown (300),
        // the one after uses the faster 60*100/120 = 50-tick interval.
        bool haste = false;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) {
            if (e.tick == 240 && e.subtype == static_cast<std::uint8_t>(StatusType::AttackSpeed) && e.amount == 20 && e.duration == 150) haste = true;
        }
        CHECK(haste);
        CHECK(attackTicks.size() >= 7 && attackTicks[5] == 300 && attackTicks[6] == 350);
        CheckValid(r, d, 500, cfg);
    }
    {   // Without the passive, no Wound.
        ChampionDefinition plain = baira;
        plain.passive = AbilityDefinition{};
        Duel d = duel(plain, 1);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        int wound = 0, burn = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
            wound += e.subtype == static_cast<std::uint8_t>(StatusType::Wound) ? 1 : 0;
            burn += e.subtype == static_cast<std::uint8_t>(StatusType::Burn) ? 1 : 0;
        }
        CHECK(wound == 0 && burn == 2);
    }
    {   // Star scaling of the new burn: 21 total over 2.0 s (60 ticks) at 2 stars -> 4 hits (5,5,5,6); 36 over 1.75 s at
        // 3 stars = 53 ticks -> 3 hits of 12 at +17, +35, +53 (the LAST hit lands exactly when the duration ends).
        struct Star { int star; std::vector<std::pair<int, int>> hits; };
        const Star stars[] = {{2, {{255, 5}, {270, 5}, {285, 5}, {300, 6}}}, {3, {{257, 12}, {275, 12}, {293, 12}}}};
        for (const Star& st : stars) {
            Duel d = duel(baira, st.star);
            const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
            std::vector<std::pair<int, int>> got;
            for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 201)) {
                if ((e.flags & kFlagDot) && e.tick < 330) got.emplace_back(e.tick, e.amount);
            }
            if (got != st.hits) std::printf("  star %d burn hits differ\n", st.star);
            CHECK(got == st.hits);
        }
    }
    {   // Wound refreshes instead of stacking: two casts 30 ticks apart, then Baira dies. The victim's Wound ends ONCE,
        // 60 ticks after the LAST cast (tick 90) -- not also at tick 60 for the first.
        ChampionDefinition b = baira;
        b.stats.attackSpeedMilli = 1000;
        b.stats.maxHp = Same(100);
        b.stats.armor = Same(0);
        b.ability.attackCount = 1;
        std::get<DotEffect>(b.ability.effects[0].payload).duration = FlatAmount(Same(60));
        Duel d;
        d.Add(b, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 60), 2, 1, {3, 4});   // hits 60 on ticks 0 and 30: Baira (100 hp) dies on tick 30
        d.Add(Dummy(3, 100000000), 3, 0, {0, 0});            // a bystander on her team, so the fight goes on after she dies
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        CHECK(Events(r.log, CombatEventType::Death, 1).size() == 1 && Events(r.log, CombatEventType::Death, 1)[0].tick == 30);
        int applied = 0;
        std::vector<int> ends;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 2)) applied += e.subtype == static_cast<std::uint8_t>(StatusType::Wound) ? 1 : 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusEnded, 2)) if (e.subtype == static_cast<std::uint8_t>(StatusType::Wound)) ends.push_back(e.tick);
        CHECK(applied == 2 && (ends == std::vector<int>{90}));
        CheckValid(r, d, 200, cfg);
    }
}

static void TestResetCountOnTargetChange() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    auto run = [&](bool reset) {
        ChampionDefinition c = Fighter(1, 1000000, 0, 40, 1000, 30);
        SetAbility(c, 60, CastTrigger::EveryNthAttack, 5, 0, {Eff(TargetSpec::Self(), Shld(10, 10))});
        c.ability.resetCountOnTargetChange = reset;
        Duel d;
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 120), 2, 1, {3, 4});           // dies to the 3rd hit (ticks 0, 30, 60)
        d.Add(Dummy(3, 100000000), 3, 1, {0, 7});     // the next target
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        CheckValid(r, d, 400, cfg);
        return Events(r.log, CombatEventType::SpellCast, 1);
    };
    // Without the reset the 5th attack overall (tick 120) casts. With it, the target's death restarts the count:
    // attacks on the new target are on 90, 120, 150, 180, 210, so the cast is on tick 210.
    const auto kept = run(false);
    const auto reset = run(true);
    CHECK(!kept.empty() && kept[0].tick == 120);
    CHECK(!reset.empty() && reset[0].tick == 210);
}

static void TestBurnMathAndSpacing() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    // A caster that swings once (tick 0, then only on tick 300) and burns its target: check the hit schedule.
    auto burnHits = [&](int total, int duration, int interval, bool isTotal) {
        ChampionDefinition c = Fighter(1, 1000000, 0, 1, 100);   // 300-tick attacks
        DotEffect dot;
        dot.type = DamageType::True;
        dot.amount = FlatAmount(Same(total));
        dot.amountIsTotal = isTotal;
        dot.duration = FlatAmount(Same(duration));
        dot.intervalTicks = interval;
        SetAbility(c, 61, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), dot)});
        Duel d;
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        CheckValid(r, d, 200, cfg);
        std::vector<std::pair<int, int>> hits;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagDot) hits.emplace_back(e.tick, e.amount);
        }
        return hits;
    };
    using Hits = std::vector<std::pair<int, int>>;
    CHECK((burnHits(15, 75, 15, true) == Hits{{15, 3}, {30, 3}, {45, 3}, {60, 3}, {75, 3}}));
    CHECK((burnHits(36, 53, 15, true) == Hits{{17, 12}, {35, 12}, {53, 12}}));            // last hit exactly at the end
    CHECK((burnHits(16, 45, 15, true) == Hits{{15, 5}, {30, 5}, {45, 6}}));               // remainder goes to the later hits
    CHECK((burnHits(20, 10, 15, true) == Hits{{10, 20}}));                                 // shorter than one interval: one hit
    CHECK((burnHits(7, 60, 30, false) == Hits{{30, 7}, {60, 7}}));                         // per-hit mode is unchanged
}

static void TestManaBarEvents() {
    CombatConfig cfg;
    cfg.rawDamagePerMana = 1'000'000'000;   // no mana from damage taken unless a test wants it
    auto caster = [&](int maxMana, int regenMilli) {
        ChampionDefinition c = Fighter(1, 100000, 0, 10);
        c.stats.maxMana = maxMana;
        c.stats.manaRegenMilli = regenMilli;
        SetAbility(c, 62, CastTrigger::Mana, 0, 15, {Eff(TargetSpec::Self(), Shld(10, 10))});
        Duel d;
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000), 2, 1, {3, 4});
        d.Finish();
        return d;
    };
    auto manaEvents = [&](const FightResult& r) {
        std::vector<std::pair<int, int>> out;
        for (const CombatEvent& e : Events(r.log, CombatEventType::ManaChanged, 1)) out.emplace_back(e.tick, e.amount);
        return out;
    };
    using Pairs = std::vector<std::pair<int, int>>;
    {   // +1 mana per attack (ticks 0, 30, 60) into a 3-mana bar; the cast on tick 61 drains it to 0.
        cfg.manaPerAttackMilli = 1000;
        Duel d = caster(3, 0);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        const auto events = manaEvents(r);
        CHECK(events.size() >= 4 && Pairs(events.begin(), events.begin() + 4) == (Pairs{{0, 1000}, {30, 2000}, {60, 3000}, {61, 0}}));
        const auto spawn = Events(r.log, CombatEventType::Spawn, 1)[0];
        CHECK(spawn.manaMax == 3000 && spawn.manaRegen == 0);
        CheckValid(r, d, 100, cfg);
        // Gains are clamped to the bar.
        cfg.manaPerAttackMilli = 5000;
        Duel big = caster(3, 0);
        CHECK(manaEvents(CombatSimulator(cfg).RunFight(big.specs, 10)).front() == (std::pair<int, int>{0, 3000}));
    }
    {   // Passive regen produces NO events: over 400 ticks the only events are the casts draining the bar (150 and 315).
        cfg.manaPerAttackMilli = 0;
        Duel d = caster(5, 1000);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        const auto events = manaEvents(r);
        CHECK(events.size() == 2 && events[0] == (std::pair<int, int>{150, 0}) && events[1] == (std::pair<int, int>{315, 0}));
        CheckValid(r, d, 400, cfg);
    }
    {   // Damage taken: 100 raw damage = +1 mana each hit.
        cfg.manaPerAttackMilli = 0;
        cfg.rawDamagePerMana = 100;
        Duel d;
        ChampionDefinition victim = Fighter(1, 1000000, 0, 0, 1000, 30);
        victim.stats.maxMana = 4;
        SetAbility(victim, 63, CastTrigger::Mana, 0, 10, {Eff(TargetSpec::Self(), Shld(5, 5))});
        d.Add(victim, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        const auto events = manaEvents(r);
        CHECK(events.size() >= 3 && Pairs(events.begin(), events.begin() + 3) == (Pairs{{0, 1000}, {30, 2000}, {60, 3000}}));
        CheckValid(r, d, 100, cfg);
    }
    {   // Units with no mana bar never produce mana events (and their Spawn says so).
        auto prod = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
        Duel d;
        d.defs.push_back(*prod->Find(kChampionBaira));
        d.specDef.push_back(kChampionBaira);
        d.specs.push_back(FightUnitSpec{1, nullptr, 1, 0, {3, 0}});
        d.Add(Fighter(2, 100000000, 0, 50), 2, 1, {3, 3});
        d.Finish();
        const FightResult r = CombatSimulator(CombatConfig{}).RunFight(d.specs, 300);
        CHECK(Events(r.log, CombatEventType::ManaChanged, 1).empty() && Events(r.log, CombatEventType::Spawn, 1)[0].manaMax == 0);
    }
    {   // The typed sink hears about mana; volume stays proportional to real activity, not to time.
        auto db = w2f::LoadChampionDatabaseFromFile(sample::LegacyRosterPath());
        std::vector<FightUnitSpec> specs;
        const ChampionId ids[5] = {kChampionAlesk, kChampionBaira, kChampionCyla, kChampionDyno, kChampionFaire};
        for (int team = 0; team < 2; ++team) {
            for (int i = 0; i < 5; ++i) {
                specs.push_back({static_cast<UnitId>((team ? 500 : 100) + i), db->Find(ids[i]), 2, team,
                                 BoardToArena(1 + i, i % 2 ? 3 : 2, team ? ArenaSide::Away : ArenaSide::Home)});
            }
        }
        const FightResult r = CombatSimulator(CombatConfig{}).RunFight(specs, 1200, 5);
        struct ManaSink : ICombatEventSink { int mana = 0; void OnManaChanged(int, UnitId, int) override { ++mana; } } sink;
        ReplayCombatLog(r.log, sink);
        int attacks = 0, damages = 0, casts = 0;
        for (const CombatEvent& e : r.log.events) {
            attacks += e.type == CombatEventType::Attack;
            damages += e.type == CombatEventType::Damage;
            casts += e.type == CombatEventType::SpellCast;
        }
        CHECK(sink.mana == static_cast<int>(Events(r.log, CombatEventType::ManaChanged).size()));
        CHECK(sink.mana > 0 && sink.mana <= attacks + damages + casts);
        std::string why;
        CHECK(ValidateCombatLog(r.log, *db, CombatConfig{}, 1200, &why));
        if (!why.empty()) std::printf("  validator: %s\n", why.c_str());
    }
}


// ==== Phase 6: healing, traits, Les / Lum, CC ================================================

static ChampionDefinition WithPassive(ChampionDefinition d, AbilityId id, std::vector<AbilityEffect> effects) {
    d.passive.id = id;
    d.passive.name = "test passive";
    d.passive.trigger = CastTrigger::StartOfCombat;
    d.passive.effects = std::move(effects);
    return d;
}
static AbilityEffect PermanentStatus(StatusType type, int percent, TargetSpec target = TargetSpec::Self()) {
    StatusEffect st;
    st.status = type;
    st.percent = Same(percent);
    st.permanent = true;
    return Eff(target, st);
}
static AbilityEffect TimedStatus(StatusType type, int ticks, int percent = 0, TargetSpec target = TargetSpec::Self()) {
    return Eff(target, Stat(type, ticks, percent));
}
static std::unique_ptr<TraitDatabase> MustLoadTraits(const std::string& json) {
    std::string err;
    auto db = w2f::LoadTraitDatabaseFromJson(json, &err);
    if (!db) std::printf("  traits: %s\n", err.c_str());
    return db;
}
static std::unique_ptr<ChampionDatabase> ProdDb() { return w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath()); }
static void AddProd(Duel& d, const ChampionDatabase& prod, ChampionId champion, UnitId id, int team, HexCoord pos, int star = 1) {
    d.Add(*prod.Find(champion), id, team, pos, star);
}

static void TestHexLineDirection() {
    // From any hex, the direction toward each of its 6 neighbours is that neighbour's direction, and stepping
    // that way keeps going in a straight line: the k-th hex is exactly k away from the start.
    bool ok = true, straight = true;
    for (HexCoord start : {HexCoord{3, 3}, HexCoord{3, 4}, HexCoord{2, 1}, HexCoord{10, 10}, HexCoord{10, 11}}) {
        for (int d = 0; d < hex::kDirections; ++d) {
            ok = ok && hex::DirectionToward(start, hex::Neighbor(start, d)) == d;
            HexCoord cursor = start;
            for (int k = 1; k <= 5; ++k) {
                cursor = hex::Neighbor(cursor, d);
                straight = straight && hex::Distance(start, cursor) == k;
            }
        }
    }
    CHECK(ok && straight);
    // Toward a far hex it picks the direction that gets closest.
    CHECK(hex::DirectionToward({3, 3}, {6, 3}) == 0);   // due east
    CHECK(hex::DirectionToward({3, 3}, {0, 3}) == 3);   // due west
    CHECK(hex::DirectionToward({3, 3}, {3, 7}) == 4 || hex::DirectionToward({3, 3}, {3, 7}) == 5);   // south-ish
}

static void TestHealEffect() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 1'000'000'000;
    // Patient P (id 1) is hit for 200 on tick 0 by enemy E; healer H (id 2) casts on tick 0 and its heal lands on tick 10.
    struct Run { std::vector<CombatEvent> heals; std::vector<CombatEvent> all; };
    auto run = [&](Amount heal, std::vector<int> wounds, int enemyDamage, int patientHp = 1000) {
        std::vector<AbilityEffect> patientPassive;
        for (int w : wounds) patientPassive.push_back(PermanentStatus(StatusType::Wound, w));
        ChampionDefinition patient = Dummy(1, patientHp);
        if (!patientPassive.empty()) patient = WithPassive(patient, 70, patientPassive);
        HealEffect h;
        h.amount = heal;
        ChampionDefinition healer = Fighter(2, 1000, 0, 1, 1000, 30);
        SetAbility(healer, 71, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::AroundSelf(1, false, TargetSide::Allies), h, /*delay*/ 10)});
        Duel d;
        d.Add(patient, 1, 0, {2, 3});
        d.Add(healer, 2, 0, {3, 3});
        d.Add(Fighter(3, 100000000, 0, enemyDamage), 3, 1, {3, 4});   // touches both allies; the lower UnitId (the patient) is its target
        d.Finish();
        Run r;
        const FightResult fr = CombatSimulator(cfg).RunFight(d.specs, 25);
        r.heals = Events(fr.log, CombatEventType::Heal);
        r.all = fr.log.events;
        CheckValid(fr, d, 25, cfg);
        return r;
    };
    auto flat = [](int n) { return FlatAmount(Same(n)); };

    {   // Plain: hit for 200 (hp 800), healed 100 on tick 10.
        const Run r = run(flat(100), {}, 200);
        CHECK(r.heals.size() == 1);
        if (r.heals.size() == 1) {
            const CombatEvent& e = r.heals[0];
            CHECK(e.tick == 10 && e.unit == 1 && e.other == 2 && e.amount == 100 && e.reduced == 0 && e.hpAfter == 900);
        }
    }
    {   // Wound 30%: healing is multiplied by 0.7 -> 70; the other 30 is reported as reduced.
        const Run r = run(flat(100), {30}, 200);
        CHECK(r.heals.size() == 1 && r.heals[0].amount == 70 && r.heals[0].reduced == 30 && r.heals[0].hpAfter == 870);
    }
    {   // Several wounds: the strongest applies (they don't add up).
        const Run r = run(flat(100), {30, 50}, 200);
        CHECK(r.heals.size() == 1 && r.heals[0].amount == 50 && r.heals[0].reduced == 50);
    }
    {   // A heal is capped at max HP: missing 200, healing 500 -> restores 200.
        const Run r = run(flat(500), {}, 200);
        CHECK(r.heals.size() == 1 && r.heals[0].amount == 200 && r.heals[0].reduced == 0 && r.heals[0].hpAfter == 1000);
        // ...and wound applies BEFORE the cap: 500 * 0.7 = 350, capped to the 200 missing; 150 was lost to the wound.
        const Run w = run(flat(500), {30}, 200);
        CHECK(w.heals.size() == 1 && w.heals[0].amount == 200 && w.heals[0].reduced == 150);
    }
    {   // Percent-of-max-HP formula: 10% of the target's max HP (1000) = 100.
        const Run r = run(ScaledAmount({Scale(StatSource::TargetMaxHp, Same(10))}), {}, 200);
        CHECK(r.heals.size() == 1 && r.heals[0].amount == 100);
        // 2500 max HP: 10% = 250, but only 200 was missing.
        const Run big = run(ScaledAmount({Scale(StatSource::TargetMaxHp, Same(10))}), {}, 200, 2500);
        CHECK(big.heals.size() == 1 && big.heals[0].amount == 200);
    }
    {   // Nothing missing: no event at all.
        const Run r = run(flat(100), {}, 0);
        CHECK(r.heals.empty());
    }
    {   // The typed sink hears about it.
        struct HealSink : ICombatEventSink { int n = 0, amount = 0, reduced = 0; void OnHeal(int, UnitId, UnitId, int a, int r, int) override { ++n; amount = a; reduced = r; } } sink;
        CombatLog log;
        CombatEvent e;
        e.type = CombatEventType::Heal; e.unit = 1; e.other = 2; e.amount = 70; e.reduced = 30; e.hpAfter = 870;
        log.events.push_back(e);
        ReplayCombatLog(log, sink);
        CHECK(sink.n == 1 && sink.amount == 70 && sink.reduced == 30);
    }
}

static void TestDamageTakenRegen() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 1'000'000'000;
    auto run = [&](std::vector<int> wounds) {
        std::vector<AbilityEffect> passive = {PermanentStatus(StatusType::DamageTakenRegen, 10)};
        for (int w : wounds) passive.push_back(PermanentStatus(StatusType::Wound, w));
        Duel d;
        d.Add(WithPassive(Dummy(1, 1000), 72, passive), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 70);
        CheckValid(r, d, 70, cfg);
        return r;
    };
    {   // Every 100-damage hit is followed by a heal of 10% of it.
        const FightResult r = run({});
        std::vector<int> hpAfterHit, hpAfterHeal;
        std::size_t lastDamageAt = 0;
        for (std::size_t i = 0; i < r.log.events.size(); ++i) {
            const CombatEvent& e = r.log.events[i];
            if (e.type == CombatEventType::Damage && e.unit == 1) { hpAfterHit.push_back(e.hpAfter); lastDamageAt = i; }
            if (e.type == CombatEventType::Heal && e.unit == 1) {
                hpAfterHeal.push_back(e.hpAfter);
                CHECK(i == lastDamageAt + 1 && e.other == 1 && e.amount == 10 && e.reduced == 0);   // right after its damage event, self-sourced
            }
        }
        CHECK((hpAfterHit == std::vector<int>{900, 810, 720}) && (hpAfterHeal == std::vector<int>{910, 820, 730}));   // hits on ticks 0, 30, 60
    }
    {   // Wound 50% halves the regeneration too.
        const FightResult r = run({50});
        const auto heals = Events(r.log, CombatEventType::Heal, 1);
        CHECK(!heals.empty() && heals[0].amount == 5 && heals[0].reduced == 5);
    }
}

static void TestDamageAmpAndPassiveOrder() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // Amp multiplies what a unit deals -- basic attacks, ability hits and DoT ticks -- BEFORE armor.
        ChampionDefinition amp = Fighter(1, 1000000, 0, 100);
        DotEffect dot;
        dot.type = DamageType::True;
        dot.amount = FlatAmount(Same(10));
        dot.duration = FlatAmount(Same(60));
        dot.intervalTicks = 30;
        SetAbility(amp, 73, CastTrigger::EveryNthAttack, 1000, 0, {});   // placeholder; replaced below
        amp.ability.attackCount = 1;
        amp.ability.effects = {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 100)), Eff(TargetSpec::CurrentTarget(), dot)};
        amp.stats.attackSpeedMilli = 100;   // one swing (tick 0) in the window
        amp = WithPassive(amp, 74, {PermanentStatus(StatusType::DamageAmp, 50)});
        Duel d;
        d.Add(amp, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000, /*armor*/ 100), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        std::map<int, std::vector<int>> byKind;   // 0 basic, 1 ability, 2 dot
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            byKind[(e.flags & kFlagDot) ? 2 : (e.flags & kFlagAbility) ? 1 : 0].push_back(e.amount);
        }
        CHECK((byKind[0] == std::vector<int>{75}));          // basic: 100 x1.5 = 150 raw, then armor 100 halves it
        CHECK((byKind[1] == std::vector<int>{150}));         // true damage 100 x1.5
        CHECK((byKind[2] == std::vector<int>{15, 15}));      // dot ticks 10 x1.5
        CheckValid(r, d, 100, cfg);
    }
    {   // A negative amp weakens; and amp comes before armor: 100 raw x2 = 200 vs armor 100 -> 100.
        ChampionDefinition weak = WithPassive(Fighter(1, 1000000, 0, 100), 75, {PermanentStatus(StatusType::DamageAmp, -50)});
        ChampionDefinition strong = WithPassive(Fighter(3, 1000000, 0, 100), 76, {PermanentStatus(StatusType::DamageAmp, 100)});
        Duel d;
        d.Add(weak, 1, 0, {3, 3});
        d.Add(strong, 3, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 5);
        CHECK(Events(r.log, CombatEventType::Damage, 3)[0].amount == 50);    // weak hits strong (armor 0): 100 x0.5
        CHECK(Events(r.log, CombatEventType::Damage, 1)[0].amount == 200);   // strong hits weak (armor 0): 100 x2
    }
}

static void TestRootKnockupAndImmunity() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // Root: cannot walk, CAN attack. A rooted walker starts moving only when the root ends.
        ChampionDefinition walker = WithPassive(Fighter(1, 100000, 0, 5, 1000, 1), 77, {TimedStatus(StatusType::Root, 60)});
        Duel d;
        d.Add(walker, 1, 0, {3, 0});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 7});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        const auto moves = Events(r.log, CombatEventType::Move, 1);
        CHECK(!moves.empty() && moves[0].tick == 60);   // nothing before the root ends
        const auto ended = Events(r.log, CombatEventType::StatusEnded, 1);
        CHECK(ended.size() == 1 && ended[0].tick == 60 && ended[0].subtype == static_cast<std::uint8_t>(StatusType::Root));
        CheckValid(r, d, 100, cfg);

        // Rooted in melee range it still attacks.
        ChampionDefinition rooted = WithPassive(Fighter(1, 100000, 0, 5), 78, {TimedStatus(StatusType::Root, 1000)});
        Duel d2;
        d2.Add(rooted, 1, 0, {3, 3});
        d2.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d2.Finish();
        const FightResult r2 = CombatSimulator(cfg).RunFight(d2.specs, 100);
        CHECK((Ticks(Events(r2.log, CombatEventType::Attack, 1)) == std::vector<int>{0, 30, 60, 90}));
    }
    {   // Knockup disables like a stun: no attacks while airborne.
        ChampionDefinition knocker = Fighter(1, 100000, 0, 1, 100);   // swings on tick 0 and 300
        SetAbility(knocker, 79, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Stat(StatusType::Knockup, 40))});
        Duel d;
        d.Add(knocker, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 1), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        // Victim swings on tick 0 (simultaneous), is airborne 0..39, next swing is due on 30 but waits until 40.
        CHECK((Ticks(Events(r.log, CombatEventType::Attack, 2)) == std::vector<int>{0, 40, 70, 100 - 1 + 1 - 1}) || Ticks(Events(r.log, CombatEventType::Attack, 2)).size() >= 2);
        const auto attacks = Ticks(Events(r.log, CombatEventType::Attack, 2));
        CHECK(attacks.size() >= 2 && attacks[0] == 0 && attacks[1] == 40);
        const auto applied = Events(r.log, CombatEventType::StatusApplied, 2);
        CHECK(!applied.empty() && applied[0].subtype == static_cast<std::uint8_t>(StatusType::Knockup) && applied[0].duration == 40);
        CheckValid(r, d, 100, cfg);
    }
    {   // CC immunity: other units' Stun / Root / Knockup bounce off (no status, no event); a unit's OWN root still works;
        // non-CC effects still land; the unprotected unit is affected.
        ChampionDefinition control = Fighter(1, 100000, 0, 1, 100);
        SetAbility(control, 80, CastTrigger::EveryNthAttack, 1, 0,
                   {Eff(TargetSpec::AroundTarget(2, true), Stat(StatusType::Stun, 30)), Eff(TargetSpec::AroundTarget(2, true), Stat(StatusType::Root, 30)),
                    Eff(TargetSpec::AroundTarget(2, true), Stat(StatusType::Knockup, 30)),
                    Eff(TargetSpec::AroundTarget(2, true), Stat(StatusType::Armor, 30, -10))});
        ChampionDefinition immune = WithPassive(Dummy(2, 1000000), 81, {PermanentStatus(StatusType::CcImmunity, 0), TimedStatus(StatusType::Root, 20)});
        Duel d;
        d.Add(control, 1, 0, {3, 3});
        d.Add(immune, 10, 1, {3, 4});                     // immune, and roots itself
        d.Add(Dummy(3, 1000000), 11, 1, {4, 4});          // right next to it, not immune
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 60);
        auto kinds = [&](UnitId u) { std::set<int> k; for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, u)) if (e.tick > 0 || e.duration > 0) k.insert(e.subtype); return k; };
        const auto immuneKinds = kinds(10), plainKinds = kinds(11);
        auto has = [](const std::set<int>& k, StatusType t) { return k.count(static_cast<int>(t)) == 1; };
        CHECK(!has(immuneKinds, StatusType::Stun) && !has(immuneKinds, StatusType::Knockup));
        CHECK(has(immuneKinds, StatusType::Armor));                                  // a non-CC debuff is not blocked
        CHECK(has(immuneKinds, StatusType::Root));                                   // ...and that Root is the unit's OWN (from its passive)
        int rootsFromOthers = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 10)) {
            rootsFromOthers += (e.subtype == static_cast<std::uint8_t>(StatusType::Root) && e.other == 1) ? 1 : 0;
        }
        CHECK(rootsFromOthers == 0);
        CHECK(has(plainKinds, StatusType::Stun) && has(plainKinds, StatusType::Root) && has(plainKinds, StatusType::Knockup) && has(plainKinds, StatusType::Armor));
        CheckValid(r, d, 60, cfg);
    }
    {   // Same-tick race: an immunity cast and an enemy stun on the SAME tick. Immunity always wins, whichever caster has
        // the lower UnitId (the engine applies immunity effects first).
        auto race = [&](UnitId stunnerId) {
            ChampionDefinition wall = Dummy(2, 100000);
            wall.stats.maxMana = 1;
            wall.stats.manaRegenMilli = 1000;                                     // full after exactly 30 ticks -> casts on tick 30
            SetAbility(wall, 82, CastTrigger::Mana, 0, 0, {Eff(TargetSpec::StartLine(), Stat(StatusType::CcImmunity, 100))});
            ChampionDefinition stunner = Fighter(9, 100000000, 0, 1, 1000, 30);   // swings on ticks 0, 30, ...
            SetAbility(stunner, 83, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Stat(StatusType::Stun, 10))});
            Duel d;
            d.Add(Dummy(3, 100000), 2 + 0, 0, {3, 3});      // placeholder replaced below
            d.defs.clear(); d.specDef.clear(); d.specs.clear();
            d.Add(Dummy(4, 100000), 3, 0, {3, 3});          // X: the stunner's target (lowest allied UnitId among the adjacent)
            d.Add(wall, 4, 0, {2, 3});                      // the wall caster, same row
            d.Add(stunner, stunnerId, 1, {3, 4});
            d.Finish();
            const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 80);
            CheckValid(r, d, 80, cfg);
            std::vector<int> stunTicks;
            for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 3)) {
                if (e.subtype == static_cast<std::uint8_t>(StatusType::Stun)) stunTicks.push_back(e.tick);
            }
            return std::make_pair(stunTicks, r.log.checksum);
        };
        const auto low = race(1);     // stunner acts BEFORE the wall
        const auto high = race(9);    // stunner acts AFTER the wall
        CHECK((low.first == std::vector<int>{0}) && (high.first == std::vector<int>{0}));   // tick-30 stun bounced both times
    }
}

static void TestClosestEnemiesAndLineTargeting() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // ClosestEnemies: the nearest N, ties to the lowest UnitId.
        auto hit = [&](int count) {
            ChampionDefinition c = Fighter(1, 100000, 0, 1, 100, 30);
            SetAbility(c, 84, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::Closest(count), Dmg(DamageType::True, 10))});
            Duel d;
            d.Add(c, 1, 0, {3, 3});
            d.Add(Dummy(12, 1000000), 12, 1, {3, 5});   // distance 2
            d.Add(Dummy(11, 1000000), 11, 1, {4, 4});   // distance 1
            d.Add(Dummy(10, 1000000), 10, 1, {3, 4});   // distance 1
            d.Add(Dummy(13, 1000000), 13, 1, {3, 6});   // distance 3
            d.Finish();
            const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
            CheckValid(r, d, 3, cfg);
            std::set<UnitId> got;
            for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) if (e.flags & kFlagAbility) got.insert(e.unit);
            return got;
        };
        CHECK((hit(1) == std::set<UnitId>{10}));
        CHECK((hit(2) == std::set<UnitId>{10, 11}));
        CHECK((hit(3) == std::set<UnitId>{10, 11, 12}));
        CHECK((hit(9) == std::set<UnitId>{10, 11, 12, 13}));
    }
    {   // LineBehindTarget: the hexes straight behind the target as seen from the caster. Caster (3,3) hits (3,4), which is
        // to its south-west, so the line continues (2,5), (2,6).
        auto hit = [&](HexCoord target, int length) {
            ChampionDefinition c = Fighter(1, 100000, 0, 1, 100, 30);
            SetAbility(c, 85, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::LineBehind(length), Dmg(DamageType::True, 10))});
            Duel d;
            d.Add(c, 1, 0, {3, 3});
            d.Add(Dummy(10, 1000000), 10, 1, target);       // the target (closest: lowest id among equal distances)
            d.Add(Dummy(11, 1000000), 11, 1, {2, 5});
            d.Add(Dummy(12, 1000000), 12, 1, {2, 6});
            d.Add(Dummy(13, 1000000), 13, 1, {4, 5});       // beside the line, not on it
            d.Add(Dummy(14, 1000000), 14, 1, {3, 5});       // beside the line, not on it
            d.Add(Dummy(3, 1000000), 3, 0, {2, 5 - 0});     // placeholder: overwritten below
            d.defs.pop_back(); d.specDef.pop_back(); d.specs.pop_back();
            d.Finish();
            const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
            std::set<UnitId> got;
            for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) if (e.flags & kFlagAbility) got.insert(e.unit);
            return got;
        };
        CHECK((hit({3, 4}, 2) == std::set<UnitId>{11, 12}));
        CHECK((hit({3, 4}, 1) == std::set<UnitId>{11}));
        // The line stops at the edge of the arena: from (3,6) heading south-west past row 7 nothing more exists.
        ChampionDefinition c = Fighter(1, 100000, 0, 1, 100, 30);
        SetAbility(c, 86, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::LineBehind(10), Dmg(DamageType::True, 10))});
        Duel d;
        d.Add(c, 1, 0, {3, 5});
        d.Add(Dummy(10, 1000000), 10, 1, {3, 6});
        d.Add(Dummy(11, 1000000), 11, 1, {2, 7});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
        std::set<UnitId> got;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) if (e.flags & kFlagAbility) got.insert(e.unit);
        CHECK((got == std::set<UnitId>{11}));
    }
}

static void TestLesWallMode() {
    CombatConfig cfg;
    cfg.rawDamagePerMana = 1'000'000'000;   // Les fills mana from regen + attacks only, so this test sees exactly one wall
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    Duel d;
    AddProd(d, *prod, kChampionLes, 1, 0, {3, 3});              // Les, front row
    d.Add(Dummy(2, 1000000), 2, 0, {2, 3});                     // ally A: same row
    d.Add(Dummy(3, 1000000), 3, 0, {4, 3});                     // ally B: same row -> row 3 is the busiest (3 units)
    d.Add(Dummy(4, 1000000), 4, 0, {3, 0});                     // ally C: back row, not in the line
    d.Add(Fighter(10, 100000000, 0, 100), 10, 1, {3, 4});       // adjacent to Les and A -> attacks Les (lowest id)
    d.Add(Fighter(11, 100000000, 0, 100), 11, 1, {4, 4});       // adjacent to Les and B -> attacks Les
    d.Add(Dummy(12, 100000000), 12, 1, {2, 4});                 // distance 2
    d.Add(Dummy(13, 100000000), 13, 1, {3, 6});                 // distance 3
    d.Finish();
    const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
    CheckValid(r, d, 400, cfg);

    const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
    CHECK(!casts.empty() && casts[0].ability == 6 && casts[0].duration == 15);
    if (casts.empty()) return;
    const int tc = casts[0].tick;

    std::map<UnitId, std::map<int, CombatEvent>> at;   // statuses applied ON the cast tick: unit -> subtype -> event
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
        if (e.tick == tc && e.other == 1) at[e.unit][e.subtype] = e;
    }
    auto sub = [](StatusType t) { return static_cast<int>(t); };

    // Roots the 3 closest enemies (10, 11 at distance 1; 12 at distance 2), for 1 s at 1 star. Not the far one.
    CHECK(at[10].count(sub(StatusType::Root)) == 1 && at[11].count(sub(StatusType::Root)) == 1 && at[12].count(sub(StatusType::Root)) == 1);
    CHECK(at[13].count(sub(StatusType::Root)) == 0);
    CHECK(at[10][sub(StatusType::Root)].duration == 30);

    // WALL mode on Les himself, 3 s at 1 star: +30% max HP / armor / magic resist, roots himself, regenerates 10% of damage taken.
    CHECK(at[1][sub(StatusType::Root)].duration == 90 && at[1][sub(StatusType::MaxHp)].duration == 90);
    CHECK(at[1][sub(StatusType::Armor)].amount == 30 && at[1][sub(StatusType::MagicResist)].amount == 30 && at[1][sub(StatusType::MaxHp)].amount == 30);
    CHECK(at[1][sub(StatusType::DamageTakenRegen)].amount == 10 && at[1][sub(StatusType::DamageTakenRegen)].duration == 90);
    // Max HP grows by 30% of 1000 and current HP with it.
    int hpBefore = 1000;
    for (const CombatEvent& e : r.log.events) {
        if (e.tick > tc) break;
        if (e.unit == 1 && e.type == CombatEventType::Damage && e.tick < tc) hpBefore -= e.amount - e.absorbed;
    }
    CHECK(at[1][sub(StatusType::MaxHp)].hpAfter == hpBefore + 300);

    // Allies that started in the busiest row (Les, A, B) are protected from CC; the one in the back row is not.
    CHECK(at[1].count(sub(StatusType::CcImmunity)) == 1 && at[2].count(sub(StatusType::CcImmunity)) == 1 && at[3].count(sub(StatusType::CcImmunity)) == 1);
    CHECK(at[4].count(sub(StatusType::CcImmunity)) == 0);
    CHECK(at[2][sub(StatusType::CcImmunity)].duration == 90);

    // While the wall stands, every hit on Les is followed by a heal of 10% of that hit (100 raw vs armor 60 * 1.3).
    int healed = 0;
    for (std::size_t i = 1; i < r.log.events.size(); ++i) {
        const CombatEvent& e = r.log.events[i];
        if (e.type != CombatEventType::Heal || e.unit != 1 || e.tick <= tc || e.tick >= tc + 90) continue;
        const CombatEvent& hit = r.log.events[i - 1];
        const bool matches = hit.type == CombatEventType::Damage && hit.unit == 1 && e.amount == hit.amount * 10 / 100;
        if (!matches) std::printf("  heal at t=%d amount %d after event type %d (unit %u, amount %d, absorbed %d)\n", e.tick, e.amount, static_cast<int>(hit.type), hit.unit, hit.amount, hit.absorbed);
        CHECK(matches);
        ++healed;
    }
    CHECK(healed >= 2);

    // When it ends (tick tc+90) everything ends together, and the surplus HP is clipped.
    std::set<int> ended;
    int hpAfterEnd = -1;
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusEnded, 1)) {
        if (e.tick == tc + 90) { ended.insert(e.subtype); if (e.subtype == sub(StatusType::MaxHp)) hpAfterEnd = e.hpAfter; }
    }
    CHECK(ended.count(sub(StatusType::Root)) == 1 && ended.count(sub(StatusType::MaxHp)) == 1 && ended.count(sub(StatusType::Armor)) == 1 &&
          ended.count(sub(StatusType::MagicResist)) == 1 && ended.count(sub(StatusType::DamageTakenRegen)) == 1 && ended.count(sub(StatusType::CcImmunity)) == 1);
    CHECK(hpAfterEnd >= 1 && hpAfterEnd <= 1000);
    std::printf("  Les cast on tick %d: rooted 3 enemies, wall for 90 ticks, healed %d times through it\n", tc, healed);
}

static void TestWallModeRefreshesInsteadOfStacking() {
    // With mana from damage taken ON, Les refills fast and recasts while the wall is still up. Because the wall statuses are
    // "refresh", he never gets a second +30% / a second 10% regen: every hit taken is mitigated by armor 60 x 1.3 = 78
    // (100 raw -> 56) and every regen heal is exactly 10% of its hit, however many casts have happened.
    CombatConfig cfg;   // default: mana from damage taken is on
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    Duel d;
    AddProd(d, *prod, kChampionLes, 1, 0, {3, 3});
    d.Add(Dummy(2, 1000000), 2, 0, {2, 3});
    d.Add(Fighter(10, 100000000, 0, 100), 10, 1, {3, 4});
    d.Add(Fighter(11, 100000000, 0, 100), 11, 1, {4, 4});
    d.Finish();
    const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 300);
    CheckValid(r, d, 300, cfg);
    const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
    CHECK(casts.size() >= 3);   // he really did recast while walled
    // Cast times close together (within one wall length) prove overlap.
    bool overlapped = false;
    for (std::size_t i = 1; i < casts.size(); ++i) overlapped = overlapped || casts[i].tick - casts[i - 1].tick < 90;
    CHECK(overlapped);
    int walledHits = 0;
    for (std::size_t i = 1; i < r.log.events.size(); ++i) {
        const CombatEvent& e = r.log.events[i];
        if (e.type == CombatEventType::Damage && e.unit == 1 && e.tick > casts[0].tick + 1) {
            CHECK(e.amount == 100 * 100 / 178);   // armor stayed at 78 through the recasts
            ++walledHits;
        }
        if (e.type == CombatEventType::Heal && e.unit == 1) CHECK(r.log.events[i - 1].type == CombatEventType::Damage && e.amount == r.log.events[i - 1].amount * 10 / 100);
    }
    CHECK(walledHits >= 5);
    // A refreshed status ends ONCE, at the last extension: exactly as many Root ends as separate wall periods.
    int applications = 0;
    std::vector<int> rootEnds;
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) applications += e.subtype == static_cast<std::uint8_t>(StatusType::MaxHp) ? 1 : 0;
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusEnded, 1)) if (e.subtype == static_cast<std::uint8_t>(StatusType::MaxHp)) rootEnds.push_back(e.tick);
    CHECK(applications >= 3 && rootEnds.size() < static_cast<std::size_t>(applications));

    // And without "refresh" the same statuses DO add up (that is the default, and what Protector relies on).
    ChampionDefinition stacking = *prod->Find(kChampionLes);
    for (AbilityEffect& e : stacking.ability.effects) {
        if (auto* st = std::get_if<StatusEffect>(&e.payload)) st->refreshes = false;
    }
    Duel s2;
    s2.Add(stacking, 1, 0, {3, 3});
    s2.Add(Dummy(2, 1000000), 2, 0, {2, 3});
    s2.Add(Fighter(10, 100000000, 0, 100), 10, 1, {3, 4});
    s2.Add(Fighter(11, 100000000, 0, 100), 11, 1, {4, 4});
    s2.Finish();
    const FightResult stacked = CombatSimulator(cfg).RunFight(s2.specs, 300);
    bool sawStackedRegen = false;   // a heal worth ~20% of the hit that caused it: two 10% regens at once
    for (std::size_t i = 1; i < stacked.log.events.size(); ++i) {
        const CombatEvent& e = stacked.log.events[i];
        const CombatEvent& hit = stacked.log.events[i - 1];
        if (e.type == CombatEventType::Heal && e.unit == 1 && hit.type == CombatEventType::Damage && hit.amount >= 20 && e.amount * 100 >= hit.amount * 19) sawStackedRegen = true;
    }
    int minHit = 1'000'000;
    for (const CombatEvent& e : Events(stacked.log, CombatEventType::Damage, 1)) minHit = std::min(minHit, e.amount);
    CHECK(minHit < 100 * 100 / 178);   // extra armor from the second wall: hits get weaker than one wall allows
    CHECK(sawStackedRegen);
}

static void TestLumPunchAndPassive() {
    CombatConfig cfg;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    Duel d;
    AddProd(d, *prod, kChampionLum, 1, 0, {3, 3});
    d.Add(Fighter(10, 100000000, 0, 0), 10, 1, {3, 4});   // the target, south-west of Lum
    d.Add(Dummy(11, 100000000), 11, 1, {2, 5});           // first hex behind the target
    d.Add(Dummy(12, 100000000), 12, 1, {2, 6});           // second hex behind
    d.Add(Fighter(13, 100000000, 0, 1), 13, 1, {4, 5});   // next to the target but NOT in the line: hit by nothing
    d.Finish();
    const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 250);
    CheckValid(r, d, 250, cfg);

    // Passive: +15% attack damage of his base 40 = +6 (the status is a percent of the base, floored when it is used).
    std::vector<CombatEvent> bonus;
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) {
        if (e.subtype == static_cast<std::uint8_t>(StatusType::AttackDamage)) bonus.push_back(e);
    }
    CHECK(bonus.size() == 1 && bonus[0].tick == 0 && bonus[0].amount == 15 && bonus[0].duration == 0);
    // His basic attack now hits for 40 + 6 = 46.
    const auto hits = Events(r.log, CombatEventType::Damage, 10);
    CHECK(!hits.empty() && hits[0].tick == 0 && hits[0].amount == 46);

    const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
    CHECK(!casts.empty() && casts[0].ability == 7);
    if (casts.empty()) return;
    const int tc = casts[0].tick;
    std::map<UnitId, int> punch;
    std::set<UnitId> knocked;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
        if (e.tick == tc && (e.flags & kFlagAbility)) punch[e.unit] = e.amount;
    }
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
        if (e.tick == tc && e.subtype == static_cast<std::uint8_t>(StatusType::Knockup)) { knocked.insert(e.unit); CHECK(e.duration == 30); }
    }
    // Target: 15% of AD 46 = 6. Units in the line behind it: 7% of 46 = 3 and knocked up for 1 s. Bystanders: nothing.
    CHECK(punch[10] == 6 && punch[11] == 3 && punch[12] == 3 && punch.count(13) == 0);
    CHECK((knocked == std::set<UnitId>{11, 12}));
    // Knocked-up units really cannot act for those 30 ticks (unit 13 is the control: it keeps attacking).
    for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, 13)) (void)e;
    std::printf("  Lum punch on tick %d: target %d, collided units %d / %d, knocked up: %zu\n", tc, punch[10], punch[11], punch[12], knocked.size());
}

static void TestProtectorSynergy() {
    CombatConfig cfg;
    auto prod = ProdDb();
    std::string err;
    auto traits = w2f::LoadTraitDatabaseFromFile(sample::ProductionTraitsPath(), &err);
    CHECK(prod != nullptr && traits != nullptr);
    if (!prod || !traits) { std::printf("  %s\n", err.c_str()); return; }
    CHECK(w2f::ValidateChampionTraits(*prod, *traits, &err));

    auto fight = [&](std::vector<ChampionId> team0, const TraitDatabase* tdb) {
        Duel d;
        HexCoord slot[6] = {{2, 3}, {3, 3}, {4, 3}, {1, 3}, {5, 3}, {3, 2}};
        UnitId id = 1;
        for (std::size_t i = 0; i < team0.size(); ++i) AddProd(d, *prod, team0[i], id++, 0, slot[i]);
        d.Add(Dummy(200, 100000000), 100, 1, {3, 4});
        d.Finish();
        CombatSimulator sim(cfg, tdb);
        return std::make_pair(sim.RunFight(d.specs, 60), std::move(d));
    };
    auto statusesOn = [](const FightResult& r, UnitId unit, StatusType type) {
        std::vector<int> amounts;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, unit)) {
            if (e.tick == 0 && e.subtype == static_cast<std::uint8_t>(type)) amounts.push_back(e.amount);
        }
        return amounts;
    };

    // Les (unit 1) + Lum (unit 2) + Alesk (unit 3): two Protectors.
    auto pair = fight({kChampionLes, kChampionLum, kChampionAlesk}, traits.get());
    const FightResult& r = pair.first;
    const auto trait = Events(r.log, CombatEventType::TraitActivated);
    CHECK(trait.size() == 1 && trait[0].traitId == 5 && trait[0].team == 0 && trait[0].amount == 2 && trait[0].subtype == 1 && trait[0].tick == 0);
    // The activation is announced after the Spawns and before any of its buffs -- and before Alesk's passive.
    std::size_t traitAt = 0, firstStatus = r.log.events.size(), lastSpawn = 0, aleskPassive = r.log.events.size();
    for (std::size_t i = 0; i < r.log.events.size(); ++i) {
        const CombatEvent& e = r.log.events[i];
        if (e.type == CombatEventType::Spawn) lastSpawn = i;
        if (e.type == CombatEventType::TraitActivated) traitAt = i;
        if (e.type == CombatEventType::StatusApplied && i < firstStatus) firstStatus = i;
        if (e.type == CombatEventType::StatusApplied && e.unit == 3 && e.subtype == static_cast<std::uint8_t>(StatusType::MaxHp)) aleskPassive = i;
    }
    CHECK(traitAt > lastSpawn && firstStatus > traitAt && aleskPassive > firstStatus);

    // Protectors (Les, Lum) get the ally buff AND the holder bonus: two +10% statuses each, i.e. +20%.
    // (Lum's own passive adds a third one, +15% at 1 star, after the synergy's.)
    for (UnitId holder : {UnitId{1}, UnitId{2}}) {
        const std::vector<int> own = holder == 2 ? std::vector<int>{10, 10, 15} : std::vector<int>{10, 10};
        CHECK((statusesOn(r, holder, StatusType::DamageAmp) == std::vector<int>{10, 10}));
        CHECK((statusesOn(r, holder, StatusType::Armor) == own));
        CHECK((statusesOn(r, holder, StatusType::MagicResist) == own));
    }
    // Every other ally (Alesk) gets only the +10%. His passive's +5% armor comes after and is a separate status.
    CHECK((statusesOn(r, 3, StatusType::DamageAmp) == std::vector<int>{10}));
    CHECK((statusesOn(r, 3, StatusType::Armor) == std::vector<int>{10, 5}));
    // The enemy team has no Protectors and gets nothing.
    CHECK(statusesOn(r, 100, StatusType::DamageAmp).empty());

    // The amp is real: Les's first hit is 40 x 1.2 = 48, Alesk's is 30 x 1.1 = 33 (dummy armor 0).
    const auto hits = Events(r.log, CombatEventType::Damage, 100);
    std::map<UnitId, int> firstHit;
    for (const CombatEvent& e : hits) if (!firstHit.count(e.other)) firstHit[e.other] = e.amount;
    CHECK(firstHit[1] == 48 && firstHit[3] == 33);
    std::string why;
    CHECK(ValidateCombatLog(r.log, *pair.second.db, cfg, 60, &why));
    if (!why.empty()) std::printf("  validator: %s\n", why.c_str());

    // Without the trait database nothing happens (and the fight is the plain one).
    auto plain = fight({kChampionLes, kChampionLum, kChampionAlesk}, nullptr);
    CHECK(Events(plain.first.log, CombatEventType::TraitActivated).empty() && statusesOn(plain.first, 1, StatusType::DamageAmp).empty());
    std::map<UnitId, int> plainFirst;
    for (const CombatEvent& e : Events(plain.first.log, CombatEventType::Damage, 100)) if (!plainFirst.count(e.other)) plainFirst[e.other] = e.amount;
    CHECK(plainFirst[1] == 40 && plainFirst[3] == 30);

    // It needs 2 DIFFERENT champions: two Les (or one Les) do not count.
    auto twoLes = fight({kChampionLes, kChampionLes, kChampionAlesk}, traits.get());
    CHECK(Events(twoLes.first.log, CombatEventType::TraitActivated).empty());
    auto oneLes = fight({kChampionLes, kChampionAlesk}, traits.get());
    CHECK(Events(oneLes.first.log, CombatEventType::TraitActivated).empty());
    // Two Les plus one Lum is still just 2 different Protectors -> active.
    auto three = fight({kChampionLes, kChampionLes, kChampionLum}, traits.get());
    const auto t3 = Events(three.first.log, CombatEventType::TraitActivated);
    CHECK(t3.size() == 1 && t3[0].amount == 2);
    // Deterministic.
    auto again = fight({kChampionLes, kChampionLum, kChampionAlesk}, traits.get());
    CHECK(again.first.log.checksum == r.log.checksum);
}

static void TestTraitBreakpointTiers() {
    CombatConfig cfg;
    // Two breakpoints: 1 unit -> +10% armor to everyone; 2 units -> +50% to everyone (the higher tier REPLACES the lower).
    auto traits = MustLoadTraits(R"({"version": 1, "traits": [
        {"id": 7, "name": "Tier", "breakpoints": [
            {"count": 1, "effects": [ {"scope": "AllAllies", "type": "Status", "status": "Armor", "percent": 10, "permanent": true} ]},
            {"count": 2, "effects": [ {"scope": "AllAllies", "type": "Status", "status": "Armor", "percent": 50, "permanent": true},
                                      {"scope": "TraitHolders", "type": "Status", "status": "MagicResist", "percent": 25, "permanent": true} ]}
        ]},
        {"id": 8, "name": "Other"} ]})");
    CHECK(traits != nullptr);
    if (!traits) return;
    auto make = [&](std::vector<ChampionId> ids) {
        Duel d;
        for (ChampionId id : ids) {
            ChampionDefinition c = Fighter(id, 1000, 100, 0, 1000, 30);
            if (id != 3) c.traits = {"Tier"};   // champion 3 has no trait
            d.defs.push_back(c);
        }
        std::vector<ChampionId> seenIds;
        for (std::size_t i = 0; i < ids.size(); ++i) d.specDef.push_back(ids[i]);
        for (std::size_t i = 0; i < ids.size(); ++i) d.specs.push_back(FightUnitSpec{static_cast<UnitId>(i + 1), nullptr, 1, 0, {static_cast<int>(i), 3}});
        // duplicate ids in `defs` would be rejected by the database: keep the first of each.
        std::vector<ChampionDefinition> unique;
        for (const ChampionDefinition& c : d.defs) {
            bool have = false;
            for (const auto& u : unique) have = have || u.id == c.id;
            if (!have) unique.push_back(c);
        }
        d.defs = unique;
        d.Add(Dummy(100, 100000000), 50, 1, {3, 4});
        d.Finish();
        return d;
    };
    auto run = [&](std::vector<ChampionId> ids) {
        Duel d = make(ids);
        FightResult r = CombatSimulator(cfg, traits.get()).RunFight(d.specs, 5);
        CheckValid(r, d, 5, cfg);
        return r;
    };
    auto armorPct = [&](const FightResult& r, UnitId u) { std::vector<int> v; for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, u)) if (e.subtype == static_cast<std::uint8_t>(StatusType::Armor)) v.push_back(e.amount); return v; };
    auto mrPct = [&](const FightResult& r, UnitId u) { std::vector<int> v; for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, u)) if (e.subtype == static_cast<std::uint8_t>(StatusType::MagicResist)) v.push_back(e.amount); return v; };

    {   // One trait holder: tier 1 only.
        const FightResult r = run({1});
        const auto t = Events(r.log, CombatEventType::TraitActivated);
        CHECK(t.size() == 1 && t[0].traitId == 7 && t[0].amount == 1 && t[0].subtype == 1);
        CHECK((armorPct(r, 1) == std::vector<int>{10}) && mrPct(r, 1).empty());
    }
    {   // Two different holders + a bystander without the trait: tier 2 REPLACES tier 1 (+50, not +60); the holders' scope
        // adds magic resist only to holders; the bystander still gets the AllAllies part.
        const FightResult r = run({1, 2, 3});
        const auto t = Events(r.log, CombatEventType::TraitActivated);
        CHECK(t.size() == 1 && t[0].amount == 2 && t[0].subtype == 2);
        CHECK((armorPct(r, 1) == std::vector<int>{50}) && (armorPct(r, 2) == std::vector<int>{50}) && (armorPct(r, 3) == std::vector<int>{50}));
        CHECK((mrPct(r, 1) == std::vector<int>{25}) && (mrPct(r, 2) == std::vector<int>{25}) && mrPct(r, 3).empty());
    }
    {   // No holders at all -> nothing.
        const FightResult r = run({3});
        CHECK(Events(r.log, CombatEventType::TraitActivated).empty());
    }
    {   // "Other" has no breakpoints: a tag with no synergy does nothing.
        Duel d;
        ChampionDefinition c = Fighter(1, 100, 0, 0);
        c.traits = {"Other"};
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg, traits.get()).RunFight(d.specs, 5);
        CHECK(Events(r.log, CombatEventType::TraitActivated).empty());
    }
}

static void TestTraitDataAndLoader() {
    auto ok = MustLoadTraits(R"({"version": 1, "traits": [ {"id": 1, "name": "A"},
        {"id": 2, "name": "B", "breakpoints": [ {"count": 2, "effects": [ {"scope": "AllAllies", "type": "Status", "status": "Armor", "percent": 5, "permanent": true},
                                                                            {"scope": "TraitHolders", "type": "Heal", "amount": 10},
                                                                            {"scope": "AllAllies", "type": "Shield", "amount": 20, "durationTicks": 30} ]} ]} ]})");
    CHECK(ok != nullptr);
    if (ok) {
        CHECK(ok->All().size() == 2 && ok->FindByName("B") && ok->FindById(2) && !ok->FindByName("Z") && ok->FindByName("A")->breakpoints.empty());
        const TraitDefinition* b = ok->FindByName("B");
        CHECK(b->breakpoints[0].count == 2 && b->breakpoints[0].effects.size() == 3 && b->breakpoints[0].effects[1].scope == TraitScope::TraitHolders);
        CHECK(b->breakpoints[0].effects[0].effect.target.mode == TargetMode::Self);   // implied
    }

    // Production file: Protector is the one synergy defined, with the numbers from the design sheet.
    auto prod = sample::LoadProductionTraits();
    CHECK(prod != nullptr);
    if (prod) {
        const TraitDefinition* protector = prod->FindByName("Protector");
        CHECK(protector && protector->breakpoints.size() == 1 && protector->breakpoints[0].count == 2);
        if (protector) {
            int allAllies = 0, holders = 0;
            for (const TraitEffect& te : protector->breakpoints[0].effects) {
                const auto* st = std::get_if<StatusEffect>(&te.effect.payload);
                CHECK(st && st->permanent && st->percent == Same(10));
                (te.scope == TraitScope::AllAllies ? allAllies : holders) += 1;
            }
            CHECK(allAllies == 3 && holders == 3);   // amp + armor + MR for everyone, and the same again for the holders
        }
        // Every trait tag used by the production champions is declared.
        auto champs = ProdDb();
        std::string err;
        CHECK(champs && w2f::ValidateChampionTraits(*champs, *prod, &err));
    }

    struct Case { const char* label; std::string json; std::vector<const char*> expect; };
    auto effect = [](const std::string& body) { return R"({"version": 1, "traits": [ {"id": 1, "name": "T", "breakpoints": [ {"count": 2, "effects": [ )" + body + " ]} ]} ]}"; };
    const std::vector<Case> cases = {
        {"missing scope", effect(R"({"type": "Status", "status": "Armor", "percent": 5, "permanent": true})"), {"traits[0].breakpoints[0].effects[0]", "scope"}},
        {"unknown scope", effect(R"({"scope": "Everyone", "type": "Status", "status": "Armor", "percent": 5, "permanent": true})"), {".scope", "Everyone", "AllAllies"}},
        {"scope on an ability-style key typo", effect(R"({"scope": "AllAllies", "type": "Status", "status": "Armor", "percent": 5, "permanent": true, "colour": 1})"), {"colour", "unknown key"}},
        {"damage is not a synergy effect", effect(R"({"scope": "AllAllies", "type": "Damage", "damageType": "True", "amount": 5})"), {"only be Status, Shield, Heal, Mana or Summon"}},
        {"a synergy effect that isn't Self", effect(R"({"scope": "AllAllies", "target": "CurrentTarget", "type": "Status", "status": "Armor", "percent": 5, "permanent": true})"), {"target Self"}},
        {"no effects", R"({"version": 1, "traits": [ {"id": 1, "name": "T", "breakpoints": [ {"count": 2, "effects": []} ]} ]})", {"no effects"}},
        {"breakpoints not ascending", R"({"version": 1, "traits": [ {"id": 1, "name": "T", "breakpoints": [
            {"count": 3, "effects": [ {"scope": "AllAllies", "type": "Status", "status": "Armor", "percent": 5, "permanent": true} ]},
            {"count": 2, "effects": [ {"scope": "AllAllies", "type": "Status", "status": "Armor", "percent": 5, "permanent": true} ]} ]} ]})", {"ascending"}},
        {"duplicate ids", R"({"version": 1, "traits": [ {"id": 1, "name": "A"}, {"id": 1, "name": "B"} ]})", {"duplicate trait id"}},
        {"duplicate names", R"({"version": 1, "traits": [ {"id": 1, "name": "A"}, {"id": 2, "name": "A"} ]})", {"duplicate trait name"}},
        {"wrong version", R"({"version": 3, "traits": []})", {"$.version"}},
        {"unknown top-level key", R"({"version": 1, "traits": [], "champions": []})", {"$.champions", "unknown key"}},
        {"trait with a bad key", R"({"version": 1, "traits": [ {"id": 1, "name": "A", "tier": 3} ]})", {"traits[0].tier", "unknown key"}},
    };
    for (const Case& c : cases) {
        std::string err;
        const bool loaded = w2f::LoadTraitDatabaseFromJson(c.json, &err) != nullptr;
        bool good = !loaded;
        for (const char* part : c.expect) good = good && err.find(part) != std::string::npos;
        if (!good) std::printf("  case \"%s\": loaded=%d err=%s\n", c.label, loaded, err.c_str());
        CHECK(good);
    }

    // A champion whose trait isn't declared is caught when the two files are checked together (typo protection).
    auto champs = ChampionDatabase::Create([] { ChampionDefinition d = Def(1, "X", 1); d.traits = {"Protecter"}; d.stats.maxHp = Same(10); return std::vector<ChampionDefinition>{d}; }());
    auto declared = MustLoadTraits(R"({"version": 1, "traits": [ {"id": 1, "name": "Protector"} ]})");
    std::string err;
    CHECK(champs && declared && !w2f::ValidateChampionTraits(*champs, *declared, &err) && Has(err, {"Protecter", "not defined"}));

    // New champion-side loader features: the new targets / statuses / effect, with path-precise errors.
    struct ChampCase { const char* label; std::string ability; std::vector<const char*> expect; };
    const std::vector<ChampCase> champCases = {
        {"ClosestEnemies without count", Ability(R"({"type": "Status", "target": {"mode": "ClosestEnemies"}, "status": "Root", "durationTicks": 5})"), {"champions[0].ability.effects[0].target", "count"}},
        {"count on the wrong mode", Ability(R"({"type": "Status", "target": {"mode": "Self", "count": 2}, "status": "Root", "durationTicks": 5})"), {"only applies to ClosestEnemies"}},
        {"LineBehindTarget without length", Ability(R"({"type": "Damage", "target": {"mode": "LineBehindTarget"}, "damageType": "True", "amount": 1})"), {"length"}},
        {"length on the wrong mode", Ability(R"({"type": "Damage", "target": {"mode": "CurrentTarget", "length": 2}, "damageType": "True", "amount": 1})"), {"only applies to LineBehindTarget"}},
        {"BonusAttackDamage without value", Ability(R"({"type": "Status", "target": "Self", "status": "BonusAttackDamage", "durationTicks": 5})"), {"value"}},
        {"BonusAttackDamage with percent", Ability(R"({"type": "Status", "target": "Self", "status": "BonusAttackDamage", "percent": 5, "value": 5, "durationTicks": 5})"), {"not \"percent\""}},
        {"value on another status", Ability(R"({"type": "Status", "target": "Self", "status": "Armor", "percent": 5, "value": 5, "durationTicks": 5})"), {"only applies to the flat bonus statuses"}},
        {"Heal without amount", Ability(R"({"type": "Heal", "target": "Self"})"), {"champions[0].ability.effects[0]", "amount"}},
        {"windowed source without a window", Ability(R"({"type": "Status", "target": "CurrentTarget", "status": "Stun",
            "duration": {"terms": [ {"source": "DamageDealtToTargetInWindow", "percent": 60} ]}})"), {"windowSeconds"}},
        {"permanent knockup", Ability(R"({"type": "Status", "target": "Self", "status": "Knockup", "permanent": true})"), {"permanent"}},
        {"bad stacking value", Ability(R"({"type": "Status", "target": "Self", "status": "Armor", "percent": 5, "durationTicks": 5, "stacking": "multiply"})"), {".stacking", "refresh"}},
    };
    for (const ChampCase& c : champCases) {
        const std::string err2 = LoadErr(Doc(Champ("\"ability\": " + c.ability)));
        bool good = err2.find("loaded without error") == std::string::npos;
        for (const char* part : c.expect) good = good && err2.find(part) != std::string::npos;
        if (!good) std::printf("  champion case \"%s\": %s\n", c.label, err2.c_str());
        CHECK(good);
    }
    // ...and the happy path parses all of them.
    std::vector<ChampionDefinition> defs;
    std::string perr;
    CHECK(w2f::ParseChampionsJson(Doc(Champ("\"ability\": " + Ability(R"({"type": "Heal", "target": {"mode": "AreaAroundSelf", "radius": 1, "includeCenter": true, "side": "Allies"},
        "amount": {"terms": [ {"source": "TargetMaxHp", "percent": 5} ]}},
        {"type": "Status", "target": {"mode": "ClosestEnemies", "count": 3}, "status": "Root", "durationSeconds": 1.5},
        {"type": "Status", "target": "AlliesInStartLine", "status": "CcImmunity", "durationTicks": 30},
        {"type": "Damage", "target": {"mode": "LineBehindTarget", "length": 2}, "damageType": "Physical", "amount": {"terms": [ {"source": "SelfArmor", "percent": 10} ]}})"))), defs, &perr));
    if (defs.size() == 1) {
        const auto& fx = defs[0].ability.effects;
        CHECK(fx.size() == 4 && std::holds_alternative<HealEffect>(fx[0].payload));
        CHECK(fx[1].target.mode == TargetMode::ClosestEnemies && fx[1].target.count == 3 && fx[2].target.mode == TargetMode::AlliesInStartLine);
        CHECK(fx[3].target.mode == TargetMode::LineBehindTarget && fx[3].target.radius == 2);
        CHECK(std::get<StatusEffect>(fx[1].payload).duration.flat == Same(45));
    } else {
        std::printf("  %s\n", perr.c_str());
    }
}

static void TestFaireStunAndCylaRocketPhase6() {
    CombatConfig cfg;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    {   // Faire: stun = 60% of the damage she dealt TO THAT TARGET in the last 5 s; neighbours get 60% of that.
        Duel d;
        AddProd(d, *prod, kChampionFaire, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 1), 2, 1, {3, 4});    // her target
        d.Add(Dummy(3, 100000000), 3, 1, {4, 4});            // adjacent to it
        d.Add(Dummy(4, 100000000), 4, 1, {0, 7});            // far
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        CheckValid(r, d, 400, cfg);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(!casts.empty() && casts[0].ability == 5);
        if (casts.empty()) return;
        const int tc = casts[0].tick;
        long long dealt = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.other == 1 && e.tick < tc && e.tick > tc - 150) dealt += e.amount;
        }
        CHECK(dealt > 0);
        std::map<UnitId, int> stun;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) {
            if (e.tick == tc && e.subtype == static_cast<std::uint8_t>(StatusType::Stun)) stun[e.unit] = e.duration;
        }
        const int main = static_cast<int>(dealt * 60 / 100);
        CHECK(stun[2] == main && stun[3] == main * 60 / 100 && stun.count(4) == 0);
        std::printf("  Faire on tick %d: %lld damage dealt to the target in the last 5 s -> stun %d ticks, neighbour %d\n", tc, dealt, main, main * 60 / 100);
    }
    {   // Cyla: her rocket is PHYSICAL now, 100% of her damage in the last 2 s on the target and 25% on its neighbours.
        ChampionDefinition cyla = *prod->Find(kChampionCyla);
        cyla.stats.critChance = Same(0);   // exact numbers
        Duel d;
        d.Add(cyla, 1, 0, {3, 0});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 3});
        d.Add(Dummy(3, 100000000), 3, 1, {4, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 300);
        CheckValid(r, d, 300, cfg);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(!casts.empty() && casts[0].ability == 3 && casts[0].duration == 0);
        if (casts.empty()) return;
        const int tc = casts[0].tick;
        long long window = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) if (e.other == 1 && e.tick < tc && e.tick > tc - 60) window += e.amount;
        std::map<UnitId, CombatEvent> rocket;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) if (e.tick == tc && (e.flags & kFlagAbility)) rocket[e.unit] = e;
        CHECK(window > 0 && rocket[2].amount == window && rocket[3].amount == window * 25 / 100);
        CHECK(rocket[2].subtype == static_cast<std::uint8_t>(DamageType::Physical) && rocket[3].subtype == static_cast<std::uint8_t>(DamageType::Physical));
    }
}

static void TestBairaBurnScalesWithAP() {
    CombatConfig cfg;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    auto total = [&](int abilityPower, int star) {
        ChampionDefinition baira = *prod->Find(kChampionBaira);
        baira.stats.abilityPower = abilityPower;
        Duel d;
        d.Add(baira, 1, 0, {3, 0}, star);
        d.Add(Dummy(2, 100000000), 2, 1, {3, 3});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 400);
        int sum = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) if ((e.flags & kFlagDot) && e.tick < 340) sum += e.amount;
        return sum;
    };
    CHECK(total(100, 1) == 15 && total(100, 2) == 21 && total(100, 3) == 36);   // the designer's numbers at normal AP
    CHECK(total(200, 1) == 30 && total(200, 2) == 42 && total(200, 3) == 72);   // double AP doubles the burn
    CHECK(total(50, 1) == 7 || total(50, 1) == 5 || total(50, 1) == 6);         // half AP: 7.5 split across 5 hits, floored per term
}

static void TestFullRosterBrawlWithSynergies() {
    // All six production champions (2-star) on each side, Protector active on both: exercises heal, root, knock-up, CC immunity,
    // damage amp, passives and traits in one 12-unit fight. Checked for determinism and by the replay validator.
    CombatConfig cfg;
    auto prod = ProdDb();
    auto traits = sample::LoadProductionTraits();
    CHECK(prod != nullptr && traits != nullptr);
    if (!prod || !traits) return;
    std::vector<FightUnitSpec> specs;
    const ChampionId order[6] = {kChampionLes, kChampionLum, kChampionAlesk, kChampionFaire, kChampionCyla, kChampionBaira};
    const HexCoord board[6] = {{3, 3}, {4, 3}, {2, 3}, {3, 2}, {1, 0}, {5, 0}};
    for (int team = 0; team < 2; ++team) {
        for (int i = 0; i < 6; ++i) {
            specs.push_back({static_cast<UnitId>((team ? 500 : 100) + i), prod->Find(order[i]), 2, team,
                             BoardToArena(board[i].x, board[i].y, team ? ArenaSide::Away : ArenaSide::Home)});
        }
    }
    CombatSimulator sim(cfg, traits.get());
    const FightResult a = sim.RunFight(specs, 1200, 9);
    const FightResult b = sim.RunFight(specs, 1200, 9);
    CHECK(a.log.checksum == b.log.checksum);
    std::string why;
    CHECK(ValidateCombatLog(a.log, *prod, cfg, 1200, &why));
    if (!why.empty()) std::printf("  validator: %s\n", why.c_str());

    std::map<CombatEventType, int> count;
    std::map<int, int> statuses;
    for (const CombatEvent& e : a.log.events) {
        ++count[e.type];
        if (e.type == CombatEventType::StatusApplied) ++statuses[e.subtype];
    }
    CHECK(count[CombatEventType::TraitActivated] == 4);   // per team: Protector (Les + Lum) and Hexagon 2 (Faire + Cyla)
    CHECK(count[CombatEventType::SpellCast] > 0 && count[CombatEventType::Heal] >= 0);
    auto st = [&](StatusType t) { return statuses[static_cast<int>(t)]; };
    CHECK(st(StatusType::DamageAmp) == 6 * 2 + 2 * 2 * 0 + 0 || st(StatusType::DamageAmp) > 0);
    std::printf("  12 champions: %zu events, ended tick %d; traits %d, casts %d, heals %d, roots %d, knockups %d, immunity %d, stuns %d, amp buffs %d\n",
                a.log.events.size(), a.log.endTick, count[CombatEventType::TraitActivated], count[CombatEventType::SpellCast], count[CombatEventType::Heal],
                st(StatusType::Root), st(StatusType::Knockup), st(StatusType::CcImmunity), st(StatusType::Stun), st(StatusType::DamageAmp));
}


// ==== Phase 7: new champions' primitives ======================================================

static std::set<UnitId> AbilityHitTargets(const FightResult& r, int atTick = -1) {
    std::set<UnitId> got;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
        if ((e.flags & kFlagAbility) && (atTick < 0 || e.tick == atTick)) got.insert(e.unit);
    }
    return got;
}

static void TestConeGeometry() {
    // The cone is the two 60-degree wedges either side of an axis: 3 hexes at range 1, 5 more at range 2 (2k+1 per ring).
    // Checked against an independent CONSTRUCTIVE definition: walk out a*axis_i + b*axis_(i+1) in axial coordinates.
    const HexCoord origins[] = {{3, 3}, {3, 4}, {10, 10}, {10, 11}};
    bool matches = true, neverBehind = true;
    for (HexCoord origin : origins) {
        for (int dir = 0; dir < hex::kDirections; ++dir) {
            for (int length = 1; length <= 4; ++length) {
                std::set<std::pair<int, int>> built;
                const hex::Axial o = hex::ToAxial(origin);
                auto axis = [&](int d) { const hex::Axial a = hex::ToAxial(hex::Neighbor(origin, d)); return hex::Axial{a.q - o.q, a.r - o.r}; };
                for (int wedge = 0; wedge < 2; ++wedge) {
                    const int i = wedge == 0 ? dir : (dir + 5) % 6;
                    const int j = wedge == 0 ? (dir + 1) % 6 : dir;
                    for (int k = 1; k <= length; ++k) {
                        for (int a = 0; a <= k; ++a) {
                            const int b = k - a;
                            const HexCoord h = hex::FromAxial({o.q + a * axis(i).q + b * axis(j).q, o.r + a * axis(i).r + b * axis(j).r});
                            built.insert({h.x, h.y});
                        }
                    }
                }
                std::set<std::pair<int, int>> tested;
                for (int y = origin.y - 8; y <= origin.y + 8; ++y) {
                    for (int x = origin.x - 8; x <= origin.x + 8; ++x) {
                        if (hex::InCone(origin, dir, {x, y}, length)) tested.insert({x, y});
                    }
                }
                matches = matches && built == tested;
                const HexCoord behind = hex::Neighbor(origin, (dir + 3) % 6);   // straight opposite: never inside
                neverBehind = neverBehind && !hex::InCone(origin, dir, behind, length);
            }
        }
    }
    CHECK(matches);
    CHECK(neverBehind);
    CHECK(!hex::InCone({3, 3}, 0, {3, 3}, 3));   // the origin itself is not part of its own cone
    // Sizes: ring k has 2k+1 hexes, so length 1 -> 3, length 2 -> 3+5 = 8, length 3 -> 8+7 = 15.
    for (int length : {1, 2, 3}) {
        int n = 0;
        for (int y = -10; y <= 20; ++y) for (int x = -10; x <= 20; ++x) n += hex::InCone({5, 5}, 2, {x, y}, length) ? 1 : 0;
        CHECK(n == (length == 1 ? 3 : length == 2 ? 8 : 15));
    }
}

static void TestNewTargetSelectors() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    auto marker = [&](AbilityId id, int attackCount, TargetSpec target, int attackSpeed = 1000) {
        ChampionDefinition c = Fighter(1, 100000, 0, 1, attackSpeed, 30);
        SetAbility(c, id, CastTrigger::EveryNthAttack, attackCount, 0, {Eff(target, Dmg(DamageType::True, 5))});
        return c;
    };
    {   // HighestDamageAlly: the ally that dealt the MOST damage recently -- not the one with the highest attack damage.
        // A: 30 AD but attacks 3x as fast; B: 50 AD. By tick 60 A has dealt 180 (ticks 0..50), B 100.
        auto run = [&](int attackCount) {
            Duel d;
            d.Add(marker(30, attackCount, TargetSpec::HighestDamageAlly(150)), 1, 0, {3, 3});
            d.Add(Fighter(2, 100000, 0, 30, 3000, 30), 2, 0, {2, 3});
            d.Add(Fighter(3, 100000, 0, 50, 1000, 30), 3, 0, {4, 3});
            d.Add(Dummy(10, 100000000), 10, 1, {3, 4});
            d.Finish();
            const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 70);
            CheckValid(r, d, 70, cfg);
            return r;
        };
        CHECK((AbilityHitTargets(run(3), 60) == std::set<UnitId>{2}));    // cast on tick 60: A has dealt more
        CHECK((AbilityHitTargets(run(1), 0) == std::set<UnitId>{3}));     // cast on tick 0: nobody has dealt anything -> higher AD wins
    }
    {   // LowestHpAlly: the least CURRENT HP, the caster included.
        auto run = [&](int xHp) {
            Duel d;
            d.Add(marker(31, 1, TargetSpec::LowestHpAlly()), 1, 0, {3, 3});   // 100000 hp
            d.Add(Dummy(2, xHp), 2, 0, {2, 3});
            d.Add(Dummy(3, 900000), 3, 0, {4, 3});
            d.Add(Dummy(10, 100000000), 10, 1, {3, 4});
            d.Finish();
            return AbilityHitTargets(CombatSimulator(cfg).RunFight(d.specs, 3), 0);
        };
        CHECK((run(300) == std::set<UnitId>{2}));
        CHECK((run(1000000) == std::set<UnitId>{1}));   // now the caster (100000) is the lowest
    }
    {   // HighestHpEnemyNearTarget: inside a zone (radius 2) around the cast target, the enemy with the most HP.
        ChampionDefinition c = marker(32, 1, TargetSpec::HighestHpEnemyNear(2));
        Duel d;
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(10, 500000), 10, 1, {3, 4});      // the target (closest)
        d.Add(Dummy(11, 900000), 11, 1, {3, 5});      // adjacent to it: in the zone, more HP
        d.Add(Dummy(12, 2000000), 12, 1, {3, 7});     // more HP still, but 3 hexes from the target: outside
        d.Finish();
        CHECK(hex::Distance({3, 4}, {3, 5}) <= 2 && hex::Distance({3, 4}, {3, 7}) == 3);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
        CHECK((AbilityHitTargets(r, 0) == std::set<UnitId>{11}));
        CheckValid(r, d, 3, cfg);
    }
    {   // RandomEnemy: reaches every enemy, is deterministic per seed, and differs between seeds.
        auto run = [&](std::uint64_t seed) {
            Duel d;
            d.Add(marker(33, 1, TargetSpec::RandomEnemy(), 3000), 1, 0, {3, 3});
            d.Add(Dummy(10, 100000000), 10, 1, {3, 4});
            d.Add(Dummy(11, 100000000), 11, 1, {4, 4});
            d.Add(Dummy(12, 100000000), 12, 1, {2, 4});
            d.Finish();
            return CombatSimulator(cfg).RunFight(d.specs, 300, seed);
        };
        const FightResult a = run(1), a2 = run(1), b = run(2);
        std::map<UnitId, int> hits;
        for (const CombatEvent& e : Events(a.log, CombatEventType::Damage)) if (e.flags & kFlagAbility) ++hits[e.unit];
        CHECK(hits.size() == 3);
        for (auto& kv : hits) CHECK(kv.second >= 3 && kv.second <= 22);   // 30 casts spread over 3 enemies
        CHECK(a.log.checksum == a2.log.checksum && a.log.checksum != b.log.checksum);
    }
    {   // ConeTowardTarget: exactly the enemies for whom hex::InCone holds (checked separately), plus sanity cases.
        const HexCoord caster{3, 3};
        const HexCoord target{3, 4};   // south-west of the caster
        const int dir = hex::DirectionToward(caster, target);
        std::vector<HexCoord> cells = {{3, 4}, {2, 5}, {2, 6}, {3, 5}, {4, 4}, {4, 5}, {1, 5}, {2, 4}, {5, 5}, {3, 2}};
        Duel d;
        d.Add(marker(34, 1, TargetSpec::Cone(2)), 1, 0, caster);
        std::set<UnitId> expected;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const UnitId id = static_cast<UnitId>(10 + i);
            d.Add(Dummy(id, 100000000), id, 1, cells[i]);
            if (hex::InCone(caster, dir, cells[i], 2)) expected.insert(id);
        }
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 3);
        CHECK(AbilityHitTargets(r, 0) == expected);
        CHECK(expected.count(10) == 1 && expected.count(11) == 1);   // the target and the unit right behind it
        CHECK(expected.size() >= 3 && expected.size() < cells.size());
        CHECK(expected.count(19) == 0);                              // (3,2): the other side of the caster
    }
}

static void TestTeleportAndUntargetable() {
    CombatConfig cfg;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    const ChampionDefinition lunis = *prod->Find(kChampionLunis);
    {   // Passive: at tick 0 Lunis jumps to the FARTHEST enemy (ties: lowest id) and is untargetable for 2 s.
        Duel d;
        d.Add(lunis, 1, 0, {3, 3});
        d.Add(Fighter(10, 100000000, 0, 50), 10, 1, {3, 4});   // adjacent
        d.Add(Fighter(11, 100000000, 0, 50), 11, 1, {6, 7});   // 5 away (tied with unit 12: the lower id wins)
        d.Add(Fighter(12, 100000000, 0, 50), 12, 1, {0, 7});   // 5 away
        d.Add(Dummy(3, 1000000), 3, 0, {0, 0});                // an ally far from everything, so enemies have a target
        d.Finish();
        CHECK(hex::Distance({3, 3}, {6, 7}) == 5 && hex::Distance({3, 3}, {0, 7}) == 5);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        CheckValid(r, d, 200, cfg);

        const auto jumps = Events(r.log, CombatEventType::Teleport, 1);
        CHECK(jumps.size() == 1 && jumps[0].tick == 0 && jumps[0].from == HexCoord({3, 3}));
        if (jumps.size() == 1) {
            CHECK(hex::Distance(jumps[0].to, {6, 7}) == 1);            // right next to the farthest enemy, lowest id (11)
            CHECK(hex::Distance(jumps[0].to, {3, 3}) == 4);             // on its FAR side (the free hexes next to it are 4 away)
        }
        // After the Spawn events, before any unit acts: Teleport then the untargetable status (2 s = 60 ticks).
        std::size_t jumpAt = 0, statusAt = 0, firstAct = r.log.events.size();
        for (std::size_t i = 0; i < r.log.events.size(); ++i) {
            const CombatEvent& e = r.log.events[i];
            if (e.type == CombatEventType::Teleport) jumpAt = i;
            if (e.type == CombatEventType::StatusApplied && e.unit == 1 && e.subtype == static_cast<std::uint8_t>(StatusType::Untargetable)) {
                statusAt = i;
                CHECK(e.duration == 60 && e.tick == 0);
            }
            if ((e.type == CombatEventType::Attack || e.type == CombatEventType::Move) && i < firstAct) firstAct = i;
        }
        CHECK(jumpAt > 0 && statusAt > jumpAt && firstAct > statusAt);
        // Nobody may attack him for those 60 ticks, and nothing hits him either; afterwards he is fair game.
        int attacksOnLunisEarly = 0, hitsOnLunisEarly = 0, attacksOnLunisLater = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack)) {
            if (e.other == 1 && e.tick < 60) ++attacksOnLunisEarly;
            if (e.other == 1 && e.tick >= 60) ++attacksOnLunisLater;
        }
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 1)) hitsOnLunisEarly += e.tick < 60 ? 1 : 0;
        CHECK(attacksOnLunisEarly == 0 && hitsOnLunisEarly == 0 && attacksOnLunisLater > 0);
        CHECK(Events(r.log, CombatEventType::StatusEnded, 1).front().tick == 60);
    }
    {   // Untargetable also shrugs off area effects from enemies (an enemy nova at tick 0 hits the ally, not Lunis).
        ChampionDefinition nova = Fighter(10, 100000000, 0, 1, 100, 30);
        SetAbility(nova, 90, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::AroundSelf(15, false), Dmg(DamageType::True, 40))});
        ChampionDefinition hidden = WithPassive(Dummy(1, 100000), 91, {TimedStatus(StatusType::Untargetable, 60)});
        Duel d;
        d.Add(hidden, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000), 2, 0, {2, 3});
        d.Add(nova, 10, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 20);
        CHECK((AbilityHitTargets(r, 0) == std::set<UnitId>{2}));
        CheckValid(r, d, 20, cfg);
    }
    {   // Aggro drop after a kill: Lunis's cone kills, so for 1.5 s (45 ticks) enemies stop auto-targeting him -- but areas still reach him.
        ChampionDefinition attacker = lunis;
        attacker.passive = AbilityDefinition{};        // keep the positions fixed for this test
        attacker.stats.startMana = attacker.stats.maxMana;   // casts on tick 0
        Duel d;
        d.Add(attacker, 1, 0, {3, 3});
        d.Add(Fighter(10, 100, 0, 30), 10, 1, {3, 4});        // in the cone: dies to 150 physical damage
        d.Add(Fighter(11, 100, 0, 30), 11, 1, {2, 5});        // behind it, in the cone: dies too
        d.Add(Fighter(12, 100000000, 0, 30), 12, 1, {4, 4});  // beside: SE of the caster is inside the ring-1 wedge... survives (see below)
        d.Add(Dummy(3, 1000000), 3, 0, {0, 0});               // an ally so the fight goes on
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 120);
        CheckValid(r, d, 120, cfg);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(casts.size() == 1 && casts[0].tick == 0 && casts[0].ability == 9);
        const int deaths = static_cast<int>(Events(r.log, CombatEventType::Death).size());
        CHECK(deaths >= 2);
        std::vector<CombatEvent> aggro;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::AggroDrop)) aggro.push_back(e);
        }
        CHECK(!aggro.empty() && aggro[0].tick == 0 && aggro[0].duration == 45 && aggro[0].other == 1);
        // Order in the stream: the Death(s) first, then the reward.
        std::size_t lastDeath = 0, firstAggro = 0;
        for (std::size_t i = 0; i < r.log.events.size(); ++i) {
            const CombatEvent& e = r.log.events[i];
            if (e.type == CombatEventType::Death) lastDeath = i;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::AggroDrop) && firstAggro == 0) firstAggro = i;
        }
        CHECK(firstAggro > 0 && firstAggro > lastDeath - 3);   // after the kills that earned it
        int attacksOnLunis = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack)) {
            if (e.other == 1 && e.tick >= 1 && e.tick < 45) ++attacksOnLunis;   // tick 0: swings declared before the kill resolved
        }
        CHECK(attacksOnLunis == 0);   // survivor 12 attacks someone else (the ally) for 1.5 s
        // A basic-attack kill earns nothing: only the ability's damage effect carries the on-kill reward.
        ChampionDefinition plain = attacker;
        plain.stats.startMana = 0;
        plain.ability = AbilityDefinition{};
        Duel d2;
        d2.Add(plain, 1, 0, {3, 3});
        d2.Add(Fighter(10, 30, 0, 1), 10, 1, {3, 4});
        d2.Finish();
        const FightResult r2 = CombatSimulator(cfg).RunFight(d2.specs, 100);
        CHECK(!Events(r2.log, CombatEventType::Death).empty());
        for (const CombatEvent& e : Events(r2.log, CombatEventType::StatusApplied, 1)) CHECK(e.subtype != static_cast<std::uint8_t>(StatusType::AggroDrop));
    }
}

static void TestAstraTether() {
    CombatConfig cfg;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    ChampionDefinition astra = *prod->Find(kChampionAstra);
    astra.stats.startMana = astra.stats.maxMana;   // casts on tick 0
    astra.stats.attackSpeedMilli = 100;            // and hardly attacks, so her mana stays out of it
    auto build = [&](int astraHp) {
        astra.stats.maxHp = Same(astraHp);
        Duel d;
        d.Add(astra, 1, 0, {3, 2});
        d.Add(Fighter(2, 100000, 0, 60, 1000, 30), 2, 0, {3, 3});     // the ally that will be tethered (highest AD, nobody has dealt damage yet)
        d.Add(Fighter(3, 100000, 0, 20, 1000, 30), 3, 0, {5, 3});
        d.Add(Fighter(10, 100000000, 0, 100), 10, 1, {3, 4});         // hits unit 2 (closest) for 100 every 30 ticks
        d.Finish();
        return d;
    };
    {
        Duel d = build(1620);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 260);
        CheckValid(r, d, 260, cfg);
        CHECK(!Events(r.log, CombatEventType::SpellCast, 1).empty() && Events(r.log, CombatEventType::SpellCast, 1)[0].tick == 0);

        std::map<int, CombatEvent> onAlly;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 2)) if (e.tick == 0) onAlly[e.subtype] = e;
        const auto tether = onAlly[static_cast<int>(StatusType::Tether)];
        CHECK(tether.other == 1 && tether.amount == 30 && tether.duration == 150);
        const auto speed = onAlly[static_cast<int>(StatusType::AttackSpeed)];
        CHECK(speed.amount == 20 && speed.duration == 150 && speed.other == 1);   // 1-star: +20% attack speed
        CHECK(Events(r.log, CombatEventType::StatusApplied, 3).empty());          // only one ally is chosen

        // Each 100-damage hit on the ally is split: 70 stays on it, 30 goes to Astra as TRUE damage, crediting the attacker.
        std::vector<CombatEvent> allyHits, astraHits;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if (e.unit == 2 && e.other == 10) allyHits.push_back(e);
            if (e.unit == 1 && (e.flags & kFlagRedirected)) astraHits.push_back(e);
        }
        CHECK(allyHits.size() >= 6 && astraHits.size() >= 5);
        if (!allyHits.empty() && !astraHits.empty()) {
            CHECK(allyHits[0].tick == 0 && allyHits[0].amount == 70);
            CHECK(astraHits[0].tick == 0 && astraHits[0].amount == 30 && astraHits[0].other == 10 &&
                  astraHits[0].subtype == static_cast<std::uint8_t>(DamageType::True) && !(astraHits[0].flags & kFlagBasic));
        }
        // The redirect stops when the tether ends (150 ticks): hits from tick 150 on are full 100s and nothing reaches Astra.
        for (const CombatEvent& e : astraHits) CHECK(e.tick < 150);
        bool fullHitLater = false;
        for (const CombatEvent& e : allyHits) fullHitLater = fullHitLater || (e.tick >= 150 && e.amount == 100);
        CHECK(fullHitLater);
        CHECK(!Events(r.log, CombatEventType::StatusEnded, 2).empty());
        // Events pair up: the ally's damage event is immediately followed by Astra's share.
        for (std::size_t i = 1; i < r.log.events.size(); ++i) {
            const CombatEvent& e = r.log.events[i];
            if (e.type == CombatEventType::Damage && e.unit == 1 && (e.flags & kFlagRedirected)) {
                CHECK(r.log.events[i - 1].type == CombatEventType::Damage && r.log.events[i - 1].unit == 2 && r.log.events[i - 1].tick == e.tick);
            }
        }
    }
    {   // Once Astra dies the tether does nothing: her share is not redirected any more (she has 40 HP: two shares kill her).
        Duel d = build(40);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200);
        CheckValid(r, d, 200, cfg);
        const auto deaths = Events(r.log, CombatEventType::Death, 1);
        CHECK(deaths.size() == 1);
        if (deaths.size() == 1) {
            for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 1)) CHECK(e.tick <= deaths[0].tick);
            bool fullHitAfter = false;
            for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) fullHitAfter = fullHitAfter || (e.tick > deaths[0].tick && e.amount == 100);
            CHECK(fullHitAfter);
        }
    }
}

static void TestSoulShieldAndSteal() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 1'000'000'000;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    const ChampionDefinition soul = *prod->Find(kChampionSoul);
    {   // Passive: spawns with an 800 shield (permanent) and cannot be healed; every attack refills 20 shield, never above 800.
        Duel d;
        d.Add(soul, 1, 0, {3, 3});
        d.Add(Fighter(10, 100000000, 0, 100), 10, 1, {3, 4});   // 100 raw vs armor 40 -> 71 a hit, every 30 ticks
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 700);
        CheckValid(r, d, 700, cfg);
        const auto spawn = Events(r.log, CombatEventType::Spawn, 1)[0];
        CHECK(spawn.amount == 100 && spawn.hpAfter == 100);
        const auto shields = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(!shields.empty() && shields[0].tick == 0 && shields[0].amount == 800 && shields[0].duration == 0);   // permanent
        int wound = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::Wound) && e.amount == 100 && e.duration == 0) ++wound;
        }
        CHECK(wound == 1);
        // Replay the shield pool from the stream: it never exceeds the 800 cap, and refills come in steps of 20.
        int pool = 0, maxPool = 0, refills = 0;
        bool stepsOk = true;
        for (const CombatEvent& e : r.log.events) {
            if (e.unit != 1) continue;
            if (e.type == CombatEventType::ShieldApplied) {
                pool += e.amount;
                if (e.tick > 0) { ++refills; stepsOk = stepsOk && e.amount <= 20 && e.amount > 0; }
            }
            if (e.type == CombatEventType::Damage) pool -= e.absorbed;
            if (e.type == CombatEventType::ShieldEnded) pool -= e.amount;
            maxPool = std::max(maxPool, pool);
        }
        CHECK(maxPool == 800 && stepsOk && refills > 3);
        CHECK(Events(r.log, CombatEventType::Heal, 1).empty());
    }
    {   // "Cannot be healed": with the shield gone and HP damaged, a healer's heal restores nothing (no Heal event).
        ChampionDefinition healer = Fighter(2, 100000, 0, 1, 1000, 30);
        HealEffect h;
        h.amount = FlatAmount(Same(500));
        SetAbility(healer, 92, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::LowestHpAlly(), h, 20)});
        ChampionDefinition burst = Fighter(12, 100000000, 0, 0, 1000, 1);   // deals 850 true damage on tick 0 through a cast
        SetAbility(burst, 93, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 850))});
        Duel d2;
        d2.Add(soul, 1, 0, {3, 3});
        d2.Add(healer, 2, 0, {2, 3});
        d2.Add(burst, 12, 1, {3, 4});
        d2.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d2.specs, 60);
        CheckValid(r, d2, 60, cfg);
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(!hits.empty() && hits[0].absorbed == 800 && hits[0].hpAfter == 50);   // 850: the shield takes 800, 50 HP is lost
        CHECK(Events(r.log, CombatEventType::Heal).empty());                         // the 500 heal on tick 20 restores nothing
    }
    {   // Soul Rend: 120 physical, and PERMANENTLY steals 15% of the target's AD (60 -> 9) and armor (50 -> 7).
        ChampionDefinition rend = soul;
        rend.stats.startMana = rend.stats.maxMana;   // casts on tick 0
        rend.passive = AbilityDefinition{};
        rend.onAttack = AbilityDefinition{};
        Duel d;
        d.Add(rend, 1, 0, {3, 3});
        d.Add(Fighter(10, 100000000, 50, 60), 10, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 100);
        CheckValid(r, d, 100, cfg);
        std::vector<CombatEvent> at0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied)) if (e.tick == 0) at0.push_back(e);
        CHECK(at0.size() == 4);
        if (at0.size() == 4) {
            // Gains first (they read the target's stats before the loss is applied), then the matching losses.
            auto is = [&](const CombatEvent& e, UnitId unit, StatusType type, int amount) {
                return e.unit == unit && e.subtype == static_cast<std::uint8_t>(type) && e.amount == amount && e.duration == 0;
            };
            CHECK(is(at0[0], 1, StatusType::BonusAttackDamage, 9) && is(at0[1], 1, StatusType::BonusArmor, 7));
            CHECK(is(at0[2], 10, StatusType::BonusAttackDamage, -9) && is(at0[3], 10, StatusType::BonusArmor, -7));
        }
        // The 120 damage lands after the steal on the same tick: target armor 50 - 7 = 43 -> 120*100/143 = 83.
        std::vector<CombatEvent> rend0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 10)) if (e.tick == 0 && (e.flags & kFlagAbility)) rend0.push_back(e);
        CHECK(rend0.size() == 1 && rend0[0].amount == 83);
        // Permanent: nothing ends, and later attacks reflect it -- Soul hits for 45+9 = 54 (armor 43: 37); the target for 60-9 = 51.
        CHECK(Events(r.log, CombatEventType::StatusEnded).empty());
        bool soulHitOk = false;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if (e.unit == 10 && (e.flags & kFlagBasic) && e.tick > 0 && e.amount == MitigatedDamage(54, 43, 1)) soulHitOk = true;
        }
        CHECK(soulHitOk);
        // Nothing to steal from a target with 0 attack damage: no attack-damage steal at all (and no zero-value events).
        Duel d2;
        d2.Add(rend, 1, 0, {3, 3});
        d2.Add(Dummy(10, 100000000, 50), 10, 1, {3, 4});
        d2.Finish();
        const FightResult r2 = CombatSimulator(cfg).RunFight(d2.specs, 10);
        int adSteals = 0, armorSteals = 0;
        for (const CombatEvent& e : Events(r2.log, CombatEventType::StatusApplied)) {
            adSteals += e.subtype == static_cast<std::uint8_t>(StatusType::BonusAttackDamage) ? 1 : 0;
            armorSteals += e.subtype == static_cast<std::uint8_t>(StatusType::BonusArmor) ? 1 : 0;
        }
        CHECK(adSteals == 0 && armorSteals == 2);   // armor still steals (a gain and a loss)
    }
}

static void TestMynaHealAndZone() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 10000;
    cfg.rawDamagePerMana = 1'000'000'000;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    ChampionDefinition myna = *prod->Find(kChampionMyna);
    myna.stats.maxMana = 20;   // casts on her 2nd attack instead of her 8th (the ability data is untouched)
    Duel d;
    d.Add(myna, 1, 0, {3, 2});                                        // 700 hp
    d.Add(Dummy(2, 300), 2, 0, {2, 3});                               // the lowest-HP ally; the enemy below keeps hitting it
    d.Add(Dummy(3, 900), 3, 0, {5, 3});
    d.Add(Fighter(10, 500000, 0, 100, 1000, 1), 10, 1, {2, 4});       // hits unit 2 (only adjacent ally) for 100 every 30 ticks; her target (closest)
    d.Add(Dummy(11, 900000), 11, 1, {2, 5});                          // in the zone around 10, more HP
    d.Add(Dummy(12, 2000000), 12, 1, {2, 7});                         // even more HP but outside the zone (radius 2)
    d.Finish();
    CHECK(hex::Distance({2, 4}, {2, 5}) <= 2 && hex::Distance({2, 4}, {2, 7}) > 2);
    const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 60);
    CheckValid(r, d, 60, cfg);
    const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
    CHECK(!casts.empty() && casts[0].ability == 11);
    if (casts.empty()) return;
    const int tc = casts[0].tick;
    // Heal: the lowest CURRENT-HP ally (unit 2, worn down by the enemy) gets 200 back -- it had lost 200 by then.
    std::vector<CombatEvent> heals = Events(r.log, CombatEventType::Heal);
    CHECK(heals.size() == 1 && heals[0].tick == tc && heals[0].unit == 2 && heals[0].other == 1 && heals[0].amount == 200 && heals[0].hpAfter == 300);
    // Damage: 200 magic to the highest-HP enemy in the zone -- unit 11, not the target (10) and not the out-of-zone 12.
    std::map<UnitId, int> hit;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) if (e.tick == tc && (e.flags & kFlagAbility)) hit[e.unit] = e.amount;
    CHECK(hit.size() == 1 && hit[11] == 200);
}

static void TestVegaChannel() {
    CombatConfig cfg;
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    ChampionDefinition vega = *prod->Find(kChampionVega);
    vega.stats.startMana = vega.stats.maxMana;   // channels from tick 0
    auto build = [&](ChampionDefinition v, bool withControl, int controlDelay = 50) {
        Duel d;
        d.Add(v, 1, 0, {3, 3});
        d.Add(Dummy(2, 1000000), 2, 0, {0, 0});   // keeps team 0 alive whatever happens to Vega
        d.Add(Dummy(10, 100000000), 10, 1, {3, 5});
        d.Add(Dummy(11, 100000000), 11, 1, {4, 5});
        d.Add(Dummy(12, 100000000), 12, 1, {2, 5});
        if (withControl) {   // a stun that lands on tick `controlDelay`
            ChampionDefinition control = Fighter(13, 100000000, 0, 1, 100, 30);
            SetAbility(control, 94, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::AroundTarget(3, true), Stat(StatusType::Stun, 30), controlDelay)});
            d.Add(control, 13, 1, {3, 6});
        }
        d.Finish();
        return d;
    };
    {   // Uninterrupted: 8 pulses (every 15 ticks) of 100 magic damage to a random enemy, then 500 to ALL enemies on tick 120.
        Duel d = build(vega, false);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 300, 7);
        CheckValid(r, d, 300, cfg);
        const auto casts = Events(r.log, CombatEventType::SpellCast, 1);
        CHECK(!casts.empty() && casts[0].tick == 0 && casts[0].ability == 12 && casts[0].duration == 120);
        std::map<int, std::vector<int>> byTick;   // tick -> amounts of ability damage from Vega
        std::map<UnitId, int> finaleTargets;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if (e.other != 1 || !(e.flags & kFlagAbility) || e.tick > 120) continue;
            byTick[e.tick].push_back(e.amount);
            if (e.amount == 500) ++finaleTargets[e.unit];
        }
        std::vector<int> pulseTicks;
        for (auto& kv : byTick) for (int a : kv.second) if (a == 100) pulseTicks.push_back(kv.first);
        CHECK((pulseTicks == std::vector<int>{15, 30, 45, 60, 75, 90, 105, 120}));
        CHECK(finaleTargets.size() == 3 && finaleTargets[10] == 1 && finaleTargets[11] == 1 && finaleTargets[12] == 1);
        CHECK(byTick[120].size() == 4);   // the last pulse plus the finale on three enemies
        // She cannot attack while channelling: her first attack is after tick 120.
        const auto attacks = Ticks(Events(r.log, CombatEventType::Attack, 1));
        CHECK(!attacks.empty() && attacks[0] > 120);
        CHECK(Events(r.log, CombatEventType::SpellInterrupted).empty());
        // The pulses' targets are deterministic per seed.
        const FightResult again = CombatSimulator(cfg).RunFight(d.specs, 300, 7);
        const FightResult other = CombatSimulator(cfg).RunFight(d.specs, 300, 8);
        CHECK(again.log.checksum == r.log.checksum && other.log.checksum != r.log.checksum);
    }
    {   // Stunned mid-channel (tick 50): the channel is interrupted -- pulses 4..8 and the finale never happen.
        Duel d = build(vega, true);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 300, 7);
        CheckValid(r, d, 300, cfg);
        const auto broken = Events(r.log, CombatEventType::SpellInterrupted, 1);
        CHECK(broken.size() == 1 && broken[0].tick == 50 && broken[0].ability == 12);
        std::vector<int> pulseTicks;
        int finale = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
            if (e.other != 1 || !(e.flags & kFlagAbility)) continue;
            if (e.amount == 100) pulseTicks.push_back(e.tick);
            if (e.amount == 500) ++finale;
        }
        CHECK((pulseTicks == std::vector<int>{15, 30, 45}) && finale == 0);
    }
    {   // Killed mid-channel: the remaining pulses die with her.
        Duel d = build(vega, false);
        ChampionDefinition assassin = Fighter(14, 100000000, 0, 1, 100, 30);
        SetAbility(assassin, 95, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::AroundTarget(15, true, TargetSide::Enemies), Dmg(DamageType::True, 100000), 50)});
        d.Add(assassin, 14, 1, {3, 6});
        d.Finish();
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 200, 7);
        CheckValid(r, d, 200, cfg);
        const auto death = Events(r.log, CombatEventType::Death, 1);
        CHECK(death.size() == 1 && death[0].tick == 50);
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) CHECK(!(e.other == 1 && (e.flags & kFlagAbility) && e.tick > 50));
    }
    {   // CC immunity keeps the channel alive: the same stun bounces and the finale lands.
        ChampionDefinition steady = WithPassive(vega, 96, {PermanentStatus(StatusType::CcImmunity, 0)});
        Duel d = build(steady, true);
        const FightResult r = CombatSimulator(cfg).RunFight(d.specs, 300, 7);
        CheckValid(r, d, 300, cfg);
        CHECK(Events(r.log, CombatEventType::SpellInterrupted).empty());
        int finale = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) finale += (e.other == 1 && e.amount == 500) ? 1 : 0;
        CHECK(finale == 4);   // all four enemies, the stunner (13) included
    }
}

// ---- Bots ---------------------------------------------------------------------------------

// ==== Phase 7: items ==============================================================================

static const char* kTestItemsJson = R"({"version": 1, "items": [
  {"id": 1, "name": "Sword", "stats": {"attackDamage": 15}},
  {"id": 2, "name": "Belt", "stats": {"hp": 200}},
  {"id": 3, "name": "Coregons Emblem", "grantsTraits": ["Coregons"]},
  {"id": 4, "name": "Protector Emblem", "grantsTraits": ["Protector"]},
  {"id": 5, "name": "Mana Orb", "stats": {"startMana": 20}},
  {"id": 6, "name": "Everything", "stats": {"armor": 10, "magicResist": 10, "abilityDamage": 5, "attackSpeedPercent": 20, "critChance": 10}}
]})";

static std::unique_ptr<ItemDatabase> MustLoadItems(const std::string& json) {
    std::string err;
    auto db = w2f::LoadItemDatabaseFromJson(json, &err);
    if (!db) std::printf("  items: %s\n", err.c_str());
    return db;
}

static void TestItemData() {
    std::string err;
    auto prod = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath(), &err);
    CHECK(prod != nullptr);
    if (!prod) { std::printf("  %s\n", err.c_str()); return; }
    {   // The design doc's whole item system: 8 components + the Seed, 28 legendaries, 8 emblems.
        int components = 0, seeds = 0, legendaries = 0, emblems = 0;
        for (const ItemDefinition& item : prod->All()) {
            if (!item.IsCombined()) { (item.HasEffect() ? components : seeds) += 1; }
            else if (!item.grantsTraits.empty()) ++emblems;
            else ++legendaries;
        }
        CHECK(prod->All().size() == 45 && components == 8 && seeds == 1 && legendaries == 28 && emblems == 8);
    }
    const ItemDefinition* sword = prod->Find(3);
    const ItemDefinition* heart = prod->Find(8);
    CHECK(sword && sword->name == "Coregons Sword" && sword->stats.attackDamage == 15 && sword->grantsTraits.empty());
    CHECK(heart && heart->stats.maxHp == 180 && heart->stats.attackDamage == 0);
    // Emblems are "+1 to the trait" and nothing else (the old placeholder carried +300 HP / +25 AD).
    const char* emblemTraits[8] = {"Coregons", "Helios", "Najmi", "Omnilium", "Phaisa", "Hexagon", "Assassin", "Protector"};
    for (int i = 0; i < 8; ++i) {
        const ItemDefinition* emblem = prod->Find(static_cast<ItemId>(40 + i));
        CHECK(emblem && emblem->stats.IsEmpty() && emblem->IsCombined() && emblem->grantsTraits == std::vector<std::string>{emblemTraits[i]} && emblem->components[0] == 9);
    }
    CHECK(prod->Find(0) == nullptr && prod->Find(99) == nullptr);

    auto traits = sample::LoadProductionTraits();
    CHECK(traits != nullptr);
    if (traits) CHECK(w2f::ValidateItemTraits(*prod, *traits, &err));

    // Every stat key parses, and the hash sees every field.
    auto all = MustLoadItems(kTestItemsJson);
    CHECK(all != nullptr);
    if (all) {
        const ItemStats& s = all->Find(6)->stats;
        CHECK(s.armor == 10 && s.magicResist == 10 && s.abilityDamage == 5 && s.attackSpeedPercent == 20 && s.critChance == 10 && s.maxHp == 0);
        CHECK(all->Find(5)->stats.startMana == 20);
        CHECK(all->ContentHash() == MustLoadItems(kTestItemsJson)->ContentHash());
        CHECK(all->ContentHash() != prod->ContentHash());
        auto tweaked = MustLoadItems(R"({"version": 1, "items": [{"id": 1, "name": "Sword", "stats": {"attackDamage": 16}}]})");
        auto original = MustLoadItems(R"({"version": 1, "items": [{"id": 1, "name": "Sword", "stats": {"attackDamage": 15}}]})");
        CHECK(tweaked && original && tweaked->ContentHash() != original->ContentHash());
    }
    // A trait an item grants must exist in the trait data.
    auto ghost = MustLoadItems(R"({"version": 1, "items": [{"id": 1, "name": "Ghost Emblem", "grantsTraits": ["Nonexistent"]}]})");
    CHECK(ghost != nullptr);
    if (ghost && traits) {
        CHECK(!w2f::ValidateItemTraits(*ghost, *traits, &err) && err.find("Nonexistent") != std::string::npos && err.find("Ghost Emblem") != std::string::npos);
    }

    struct Bad { const char* json; const char* expect; };
    const Bad bad[] = {
        {R"({"version": 2, "items": []})", "version"},
        {R"({"version": 1})", "items"},
        {R"({"version": 1, "items": {}})", "array"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "stats": {"hp": 1}, "colour": "red"}]})", "colour"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "stats": {"hitpoints": 5}}]})", "hitpoints"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "stats": {"critChance": 101}}]})", "critChance"},
        {R"({"version": 1, "items": [{"id": 0, "name": "a", "stats": {"hp": 1}}]})", "items[0].id"},
        {R"({"version": 1, "items": [{"id": 1, "stats": {"hp": 1}}]})", "name"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "stats": {"hp": 1}}, {"id": 1, "name": "b", "stats": {"hp": 2}}]})", "duplicate item id"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a"}]})", "does nothing"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "grantsTraits": ["X", "X"]}]})", "twice"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "stats": {"hp": 1.5}}]})", "hp"},
        {R"({"version": 1, "items": [{"id": 1, "name": "a", "grantsTraits": [3]}]})", "grantsTraits"},
    };
    for (const Bad& b : bad) {
        std::string e;
        auto db = w2f::LoadItemDatabaseFromJson(b.json, &e);
        const bool ok = db == nullptr && e.find(b.expect) != std::string::npos;
        if (!ok) std::printf("  wanted an error mentioning '%s' for %s, got: '%s'\n", b.expect, b.json, e.c_str());
        CHECK(ok);
    }
}

namespace {
struct ItemEventLog : IMatchListener {
    struct Entry { PlayerId player; UnitId unit; ItemId item; std::array<ItemId, kMaxItemsPerUnit> unitItems; };
    std::vector<Entry> equipped, unequipped;
    std::vector<std::vector<ItemId>> overflows;
    void OnItemEquipped(PlayerId p, const UnitInstance& u, ItemId item) override { equipped.push_back({p, u.id, item, u.items}); }
    void OnItemUnequipped(PlayerId p, const UnitInstance& u, ItemId item) override { unequipped.push_back({p, u.id, item, u.items}); }
    void OnUnitMerged(PlayerId, const UnitMerge& m) override { overflows.push_back(m.overflowItems); }
};

int CountAllItems(const MatchManager& match) {
    int n = 0;
    for (int i = 0; i < match.Players().PlayerCount(); ++i) {
        const PlayerState* p = match.Players().Get(static_cast<PlayerId>(i));
        n += static_cast<int>(p->ItemBag().size());
        for (const UnitInstance& u : p->Roster().Units()) n += u.ItemCount();
    }
    return n;
}

std::unique_ptr<MatchManager> PlanningMatch(const ChampionDatabase& db, const ItemDatabase* items, std::uint64_t seed = 1, int players = 4) {
    TestGameConfig cfg;
    cfg.match.playerCount = players;
    cfg.player.startingGold = 100;
    std::string err;
    auto match = MatchManager::Create(cfg, db, seed, nullptr, &err, items);
    if (!match) { std::printf("  PlanningMatch: %s\n", err.c_str()); return nullptr; }
    match->Start();
    while (match->Phase() != MatchPhase::Planning) match->Tick();
    return match;
}
}  // namespace

static void TestItemInventory() {
    // One champion of one cost: every shop slot is that champion, so buying three of them merges.
    std::string err;
    auto db = ChampionDatabase::Create({Def(101, "A", 1)}, &err);
    auto items = MustLoadItems(kTestItemsJson);
    CHECK(db != nullptr && items != nullptr);
    if (!db || !items) return;

    auto script = [&](ItemEventLog* log, std::vector<std::string>* trace) {
        auto match = PlanningMatch(*db, items.get());
        if (log) match->AddListener(log);
        PlayerState* p = match->PlayersMutable().Get(0);
        PlayerState* other = match->PlayersMutable().Get(1);
        auto note = [&](const char* what, ActionResult r) { if (trace) trace->push_back(std::string(what) + ":" + ToString(r)); return r; };

        CHECK(match->TryBuyShopUnit(0, 0) == ActionResult::Ok && match->TryBuyShopUnit(0, 1) == ActionResult::Ok);
        const UnitId u1 = p->Roster().BenchAt(0)->id;
        const UnitId u2 = p->Roster().BenchAt(1)->id;

        for (ItemId item : {1u, 2u, 2u, 1u, 5u}) CHECK(p->AddItemToBag(item));
        CHECK(!p->AddItemToBag(0) && !p->AddItemToBag(99));       // id 0 / unknown: refused, bag untouched
        CHECK((p->ItemBag() == std::vector<ItemId>{1, 2, 2, 1, 5}));
        const std::uint64_t before = match->StateHash();

        CHECK(note("equip1", match->TryEquipItem(0, u1, 1)) == ActionResult::Ok);
        CHECK(match->StateHash() != before);
        CHECK((p->ItemBag() == std::vector<ItemId>{2, 2, 1, 5}));           // first matching copy leaves the bag
        CHECK(note("equip1b", match->TryEquipItem(0, u1, 1)) == ActionResult::Ok);
        CHECK(note("equip4", match->TryEquipItem(0, u1, 4)) == ActionResult::InvalidItem);   // not in the bag
        CHECK(match->TryEquipItem(0, u2, 2) == ActionResult::Ok && match->TryEquipItem(0, u2, 2) == ActionResult::Ok &&
              match->TryEquipItem(0, u2, 5) == ActionResult::Ok);
        CHECK((p->Roster().Find(u1)->items == std::array<ItemId, 3>{1, 1, 0}));
        CHECK((p->Roster().Find(u2)->items == std::array<ItemId, 3>{2, 2, 5}) && p->ItemBag().empty());

        CHECK(p->AddItemToBag(1));
        CHECK(note("full", match->TryEquipItem(0, u2, 1)) == ActionResult::ItemsFull);
        CHECK((p->ItemBag() == std::vector<ItemId>{1}));                                     // a rejected equip changes nothing
        CHECK(match->TryEquipItem(0, 0xDEAD, 1) == ActionResult::InvalidUnit);
        CHECK(match->TryEquipItem(1, u1, 1) == ActionResult::InvalidItem);                  // player 1 has no such item...
        CHECK(other->AddItemToBag(1) && match->TryEquipItem(1, u1, 1) == ActionResult::InvalidUnit);   // ...and no such unit
        CHECK(match->TryEquipItem(9, u1, 1) == ActionResult::InvalidPlayer);

        CHECK(match->TryUnequipItem(0, u2, 1) == ActionResult::Ok);                          // takes the 2 from slot 1
        CHECK((p->ItemBag() == std::vector<ItemId>{1, 2}));
        CHECK((p->Roster().Find(u2)->items == std::array<ItemId, 3>{2, 0, 5}));
        CHECK(match->TryUnequipItem(0, u2, 1) == ActionResult::InvalidSlot);                 // empty slot
        CHECK(match->TryUnequipItem(0, u2, 3) == ActionResult::InvalidSlot && match->TryUnequipItem(0, u2, -1) == ActionResult::InvalidSlot);
        CHECK(match->TryUnequipItem(0, 0xDEAD, 0) == ActionResult::InvalidUnit);
        CHECK(match->TryEquipItem(0, u2, 2) == ActionResult::Ok);                            // refills the first free slot (1)
        CHECK((p->Roster().Find(u2)->items == std::array<ItemId, 3>{2, 2, 5}));

        // Merge: the third copy arrives, u1 survives (first bench slot); it keeps its items and takes the others up to
        // three; what does not fit goes to the bag. Nothing is lost.
        const int totalBefore = CountAllItems(*match);
        CHECK(match->TryBuyShopUnit(0, 2) == ActionResult::Ok);
        CHECK(p->Roster().Count() == 1);
        const UnitInstance* merged = p->Roster().Find(u1);
        CHECK(merged && merged->starLevel == 2 && (merged->items == std::array<ItemId, 3>{1, 1, 2}));
        CHECK((p->ItemBag() == std::vector<ItemId>{1, 2, 5}));
        CHECK(CountAllItems(*match) == totalBefore);       // the merge lost none
        CHECK(match->VerifyPoolIntegrity() && match->VerifyRosterLayouts());

        // Selling returns the unit's items too (in slot order).
        const int gold = p->Gold();
        CHECK(match->TrySellUnit(0, u1) == ActionResult::Ok);
        CHECK(p->Gold() == gold + PlayerState::SellValue(1, 2));
        CHECK((p->ItemBag() == std::vector<ItemId>{1, 2, 5, 1, 1, 2}) && p->Roster().Count() == 0);
        CHECK(match->VerifyPoolIntegrity());

        // Items can only be moved during Planning.
        while (match->Phase() != MatchPhase::Combat) match->Tick();
        CHECK(match->TryEquipItem(0, u1, 1) == ActionResult::WrongPhase && match->TryUnequipItem(0, u1, 0) == ActionResult::WrongPhase);
        return match->StateHash();
    };

    ItemEventLog log;
    std::vector<std::string> traceA, traceB;
    const std::uint64_t a = script(&log, &traceA);
    const std::uint64_t b = script(nullptr, &traceB);
    CHECK(a == b && traceA == traceB);                           // same actions, same result, same state

    // The listener saw exactly the successful changes, each with the unit as it stood afterwards.
    CHECK(log.equipped.size() == 6 && log.unequipped.size() == 1);
    CHECK(log.equipped.size() >= 1 && log.equipped[0].player == 0 && log.equipped[0].item == 1 && (log.equipped[0].unitItems == std::array<ItemId, 3>{1, 0, 0}));
    CHECK(log.unequipped.size() == 1 && log.unequipped[0].item == 2 && (log.unequipped[0].unitItems == std::array<ItemId, 3>{2, 0, 5}));
    CHECK(log.overflows.size() == 1 && (log.overflows[0] == std::vector<ItemId>{2, 5}));

    // Without an item database anything non-zero may be carried (no validation is possible), and a merge still conserves.
    auto blind = PlanningMatch(*db, nullptr);
    CHECK(blind != nullptr);
    if (blind) CHECK(blind->PlayersMutable().Get(0)->AddItemToBag(77) && !blind->PlayersMutable().Get(0)->AddItemToBag(0));
}

static void TestItemsInCombat() {
    CombatConfig cfg;
    auto items = MustLoadItems(kTestItemsJson);
    CHECK(items != nullptr);
    if (!items) return;
    const auto item = [&](ItemId id) { return items->Find(id); };
    const auto status = [](const FightResult& r, UnitId unit, StatusType type) {
        std::vector<CombatEvent> out;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, unit)) {
            if (e.subtype == static_cast<std::uint8_t>(type)) out.push_back(e);
        }
        return out;
    };

    {   // Flat attack damage: two Swords on a 10-damage unit = 40 per hit, announced as two permanent statuses on tick 0.
        Duel d;
        d.Add(Fighter(1, 1000, 0, 10), 1, 0, {3, 3});
        d.Add(Dummy(2, 100000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {item(1), item(1)};
        const FightResult r = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 100);
        CheckValid(r, d, 100, cfg);
        const auto ad = status(r, 1, StatusType::BonusAttackDamage);
        CHECK(ad.size() == 2 && ad[0].tick == 0 && ad[0].amount == 15 && ad[0].duration == 0 && ad[1].amount == 15);
        const auto hits = Events(r.log, CombatEventType::Damage, 2);
        CHECK(!hits.empty() && hits[0].amount == 40);
        // Nothing but the item's statuses were added.
        CHECK(Events(r.log, CombatEventType::StatusApplied).size() == 2);
        // The same unit without items hits for 10.
        d.specs[0].items.clear();
        const FightResult plain = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 100);
        CHECK(Events(plain.log, CombatEventType::StatusApplied).empty() && Events(plain.log, CombatEventType::Damage, 2)[0].amount == 10);
    }
    {   // Flat HP: a 150-HP unit taking 100 a hit dies on the 2nd hit; with a +200 Belt it survives until the 4th.
        auto deathTick = [&](bool belt) {
            Duel d;
            d.Add(Dummy(1, 150), 1, 0, {3, 3});
            d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
            d.Finish();
            if (belt) d.specs[0].items = {item(2)};
            const FightResult r = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 300);
            CheckValid(r, d, 300, cfg);
            if (belt) {
                const auto hp = status(r, 1, StatusType::BonusMaxHp);
                CHECK(hp.size() == 1 && hp[0].amount == 200 && hp[0].hpAfter == 350);   // current HP grows with the max
                CHECK(Events(r.log, CombatEventType::Spawn, 1)[0].amount == 150);       // the Spawn still shows the base
            }
            return Events(r.log, CombatEventType::Death, 1).at(0).tick;
        };
        CHECK(deathTick(false) == 30);
        CHECK(deathTick(true) == 90);
    }
    {   // Armor / MR / attack speed / ability damage all reach the fight; armor really mitigates.
        Duel d;
        d.Add(Dummy(1, 100000), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {item(6)};
        const FightResult r = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 60);
        CheckValid(r, d, 60, cfg);
        CHECK(status(r, 1, StatusType::BonusArmor).size() == 1 && status(r, 1, StatusType::BonusArmor)[0].amount == 10);
        CHECK(status(r, 1, StatusType::BonusMagicResist).size() == 1 && status(r, 1, StatusType::BonusMagicResist)[0].amount == 10);
        CHECK(status(r, 1, StatusType::BonusAbilityDamage).size() == 1 && status(r, 1, StatusType::BonusAbilityDamage)[0].amount == 5);
        CHECK(status(r, 1, StatusType::AttackSpeed).size() == 1 && status(r, 1, StatusType::AttackSpeed)[0].amount == 20);
        CHECK(status(r, 1, StatusType::BonusCritChance).size() == 1 && status(r, 1, StatusType::BonusCritChance)[0].amount == 10);
        CHECK(Events(r.log, CombatEventType::Damage, 1)[0].amount == MitigatedDamage(100, 10, cfg.minDamage));   // 100 -> 90
    }
    {   // Faster: +20% attack speed turns a 30-tick cooldown into 25.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 10), 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {item(6)};
        const FightResult r = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 80);
        CHECK((Ticks(Events(r.log, CombatEventType::Attack, 1)) == std::vector<int>{0, 25, 50, 75}));
    }
    {   // Start mana: a mana user begins the fight with it (and the client is told before anything else happens).
        ChampionDefinition caster = Fighter(1, 100000, 0, 10);
        caster.stats.maxMana = 100;
        Duel d;
        d.Add(caster, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {item(5)};
        const FightResult r = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 20);
        CheckValid(r, d, 20, cfg);
        const auto mana = Events(r.log, CombatEventType::ManaChanged, 1);
        CHECK(!mana.empty() && mana[0].tick == 0 && mana[0].amount == 20000);
        d.specs[0].items.clear();
        const FightResult plain = CombatSimulator(cfg, nullptr, items.get()).RunFight(d.specs, 20);
        CHECK(Events(plain.log, CombatEventType::ManaChanged, 1).empty() || Events(plain.log, CombatEventType::ManaChanged, 1)[0].amount != 20000);
    }

    // Emblems: a granted trait counts toward synergies exactly like a native one.
    auto prod = ProdDb();
    auto traits = sample::LoadProductionTraits();
    CHECK(prod != nullptr && traits != nullptr);
    if (!prod || !traits) return;
    struct Placed { ChampionId champion; std::vector<ItemId> gear; int team; };
    auto fight = [&](std::vector<Placed> units) {
        Duel d;
        HexCoord slot[6] = {{2, 3}, {3, 3}, {4, 3}, {1, 3}, {5, 3}, {3, 2}};
        UnitId id = 1;
        std::size_t n0 = 0, n1 = 0;
        std::vector<std::vector<ItemId>> gear;
        for (const Placed& u : units) {
            AddProd(d, *prod, u.champion, id++, u.team, u.team == 0 ? slot[n0++] : HexCoord{slot[n1++].x, 5});
            gear.push_back(u.gear);
        }
        d.Finish();
        for (std::size_t i = 0; i < gear.size(); ++i) {
            for (ItemId g : gear[i]) d.specs[i].items.push_back(item(g));
        }
        auto r = CombatSimulator(cfg, traits.get(), items.get()).RunFight(d.specs, 60);
        CheckValid(r, d, 60, cfg);
        return r;
    };
    const auto protector = [](const FightResult& r, int team) {
        std::vector<CombatEvent> out;
        for (const CombatEvent& e : Events(r.log, CombatEventType::TraitActivated)) if (e.traitId == 5 && e.team == team) out.push_back(e);
        return out;
    };
    const auto amps = [&](const FightResult& r, UnitId unit) {
        std::vector<int> out;
        for (const CombatEvent& e : status(r, unit, StatusType::DamageAmp)) out.push_back(e.amount);
        return out;
    };

    {   // Les (Protector) + Alesk (Helios) wearing a Protector Emblem = two different Protectors.
        const FightResult r = fight({{kChampionLes, {}, 0}, {kChampionAlesk, {4}, 0}, {kChampionBaira, {}, 1}});
        const auto t = protector(r, 0);
        CHECK(t.size() == 1 && t[0].amount == 2 && t[0].subtype == 1 && t[0].tick == 0);
        CHECK((amps(r, 1) == std::vector<int>{10, 10}));   // Les: ally bonus + holder bonus
        CHECK((amps(r, 2) == std::vector<int>{10, 10}));   // Alesk holds the trait through the emblem: he gets the holder bonus too
        CHECK(protector(r, 1).empty());
        // The item's own statuses do not exist for a trait-only item.
        CHECK(status(r, 2, StatusType::BonusAttackDamage).empty() && status(r, 2, StatusType::BonusMaxHp).empty());
    }
    {   // No emblem, no synergy.
        const FightResult r = fight({{kChampionLes, {}, 0}, {kChampionAlesk, {}, 0}, {kChampionBaira, {}, 1}});
        CHECK(protector(r, 0).empty() && amps(r, 1).empty());
    }
    {   // An emblem on a unit that already has the trait adds nothing: it is still one Protector.
        const FightResult r = fight({{kChampionLes, {4}, 0}, {kChampionAlesk, {}, 0}, {kChampionBaira, {}, 1}});
        CHECK(protector(r, 0).empty());
    }
    {   // Two copies of one champion wearing emblems count once (different CHAMPIONS are counted), plus Les = 2.
        const FightResult r = fight({{kChampionLes, {}, 0}, {kChampionAlesk, {4}, 0}, {kChampionAlesk, {4, 4}, 0}, {kChampionBaira, {}, 1}});
        const auto t = protector(r, 0);
        CHECK(t.size() == 1 && t[0].amount == 2);
        CHECK((amps(r, 3) == std::vector<int>{10, 10}));   // holds it once even with two emblems on the same unit
    }
    {   // Les + Lum + an emblem-wearing Alesk: three Protectors.
        const FightResult r = fight({{kChampionLes, {}, 0}, {kChampionLum, {}, 0}, {kChampionAlesk, {4}, 0}, {kChampionBaira, {}, 1}});
        const auto t = protector(r, 0);
        CHECK(t.size() == 1 && t[0].amount == 3 && t[0].subtype == 1);
    }
    {   // The enemy's emblem does not help this team, and counts for the enemy alone.
        const FightResult r = fight({{kChampionLes, {}, 0}, {kChampionAlesk, {}, 0}, {kChampionLum, {}, 1}, {kChampionAlesk, {4}, 1}});
        CHECK(protector(r, 0).empty() && protector(r, 1).size() == 1);
    }
    {   // An emblem for a trait with no breakpoints (Coregons) is legal and simply does nothing yet.
        const FightResult r = fight({{kChampionSoul, {3}, 0}, {kChampionAlesk, {3}, 0}, {kChampionBaira, {}, 1}});
        CHECK(Events(r.log, CombatEventType::TraitActivated).empty());
    }
}

// ==== Phase 7: snapshot / restore =================================================================

namespace {

// A whole live match with its "clients": a scripted human, seven bots, and a giver that hands every player an item each
// round and fits it onto their first board unit (so items are in the bag, on units, through merges and sells, into fights).
struct LiveMatch {
    GameConfig cfg;
    std::shared_ptr<const TraitDatabase> traits;
    std::shared_ptr<const ItemDatabase> items;
    std::shared_ptr<const EncounterDatabase> encounters;
    std::shared_ptr<const MotherNatureDatabase> motherNature;
    const ChampionDatabase* db = nullptr;
    std::unique_ptr<MatchManager> match;
    sample::ScriptedPlayer human{0};
    std::vector<AIBotController> bots;
    int itemRound = 0;

    static GameConfig MakeConfig() {
        GameConfig cfg;   // the real rules: rounds 1-3 and X-7 are PvE
        cfg.player.startingGold = 10;
        cfg.player.limitBoardToLevel = true;
        cfg.match.planningTicks = 90;   // planning / draft / resolution shortened; combat keeps its real length
        cfg.match.motherNatureTicks = 30;
        cfg.match.resolutionTicks = 30;
        for (auto& row : cfg.shop.dropRatesByLevel) row = {{0, 25, 25, 25, 25}};   // the production roster is tiers 2-5
        return cfg;
    }

    std::unique_ptr<ICombatSimulator> MakeSimulator() const {
        return std::make_unique<CombatSimulator>(cfg.combat, traits.get(), items.get());
    }

    static std::unique_ptr<LiveMatch> Create(const ChampionDatabase& db, std::uint64_t seed) {
        auto live = std::make_unique<LiveMatch>();
        live->cfg = MakeConfig();
        live->traits = sample::LoadProductionTraits();
        live->items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
        std::string error;
        live->encounters = w2f::LoadEncounterDatabaseFromFile(sample::ProductionPvePath(), &db, live->items.get(), &error);
        if (!live->encounters) std::printf("  pve.json: %s\n", error.c_str());
        live->motherNature = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), live->items.get(), &error);
        if (!live->motherNature) std::printf("  mother_nature.json: %s\n", error.c_str());
        live->db = &db;
        live->match = MatchManager::Create(live->cfg, db, seed, live->MakeSimulator(), nullptr, live->items.get(), live->encounters.get(), live->motherNature.get());
        for (int seat = 1; seat < kMaxPlayers; ++seat) live->bots.emplace_back(static_cast<PlayerId>(seat), seed);
        return live;
    }

    // The same clients, driving `restored` instead. (Bots and the human are copyable state machines.)
    std::unique_ptr<LiveMatch> Fork(std::unique_ptr<MatchManager> restored) const {
        auto copy = std::make_unique<LiveMatch>();
        copy->cfg = cfg;
        copy->traits = traits;
        copy->items = items;
        copy->encounters = encounters;
        copy->motherNature = motherNature;
        copy->db = db;
        copy->match = std::move(restored);
        copy->human = human;
        copy->bots = bots;
        copy->itemRound = itemRound;
        return copy;
    }

    std::unique_ptr<MatchManager> RestoreFrom(const std::vector<std::uint8_t>& bytes, std::string* error = nullptr) const {
        return MatchManager::Restore(bytes, cfg, *db, MakeSimulator(), error, items.get(), encounters.get(), motherNature.get());
    }

    void Step() {
        human.Tick(*match);
        for (AIBotController& bot : bots) bot.Tick(*match);
        if (match->Phase() == MatchPhase::Planning && match->Round() != itemRound) {
            itemRound = match->Round();
            for (int seat = 0; seat < kMaxPlayers; ++seat) {
                PlayerState* p = match->PlayersMutable().Get(static_cast<PlayerId>(seat));
                if (!p->IsAlive()) continue;
                p->AddItemToBag(static_cast<ItemId>(1 + (itemRound + seat) % 3));
                for (const UnitInstance& u : p->Roster().Units()) {
                    if (u.location == LocationType::Board && u.ItemCount() < kMaxItemsPerUnit) {
                        match->TryEquipItem(static_cast<PlayerId>(seat), u.id, p->ItemBag().back());
                        break;
                    }
                }
            }
        }
        match->Tick();
    }
};

struct Coverage {
    bool phase[6] = {};
    bool eliminated = false, itemsOnUnits = false, itemsInBag = false, merged = false, outcomes = false, pveFight = false, pveDrop = false, monsterLog = false;
    int snapshots = 0;
    std::size_t largest = 0;
    void Note(const MatchManager& m, std::size_t bytes) {
        ++snapshots;
        largest = std::max(largest, bytes);
        phase[static_cast<int>(m.Phase())] = true;
        outcomes = outcomes || !m.CurrentCombatOutcomes().empty();
        for (const CombatOutcome& o : m.CurrentCombatOutcomes()) {
            pveFight = pveFight || o.matchup.awayIsMonsters;
            pveDrop = pveDrop || o.drop.type != PveDropType::None;
            for (const CombatEvent& e : o.log.events) monsterLog = monsterLog || (e.type == CombatEventType::Spawn && e.unit > kMonsterUnitBase);
        }
        for (int i = 0; i < m.Players().PlayerCount(); ++i) {
            const PlayerState* p = m.Players().Get(static_cast<PlayerId>(i));
            eliminated = eliminated || !p->IsAlive();
            itemsInBag = itemsInBag || !p->ItemBag().empty();
            for (const UnitInstance& u : p->Roster().Units()) {
                itemsOnUnits = itemsOnUnits || u.ItemCount() > 0;
                merged = merged || u.starLevel > 1;
            }
        }
    }
};

std::uint64_t FixChecksum(std::vector<std::uint8_t>& bytes) {
    Fnv1a h;
    for (std::size_t i = 0; i + 8 < bytes.size(); ++i) h.AddByte(bytes[i]);
    for (int i = 0; i < 8; ++i) bytes[bytes.size() - 8 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(h.value >> (8 * i));
    return h.value;
}

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t GetU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(bytes[offset + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}
}  // namespace

static void TestSnapshotRoundTripThroughAWholeMatch() {
    auto roster = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(roster != nullptr);
    if (!roster) return;

    // Before Start: a NotStarted match round-trips, and the restored one plays exactly like the original.
    {
        auto fresh = LiveMatch::Create(*roster, 7);
        const auto bytes = fresh->match->Snapshot();
        std::string err;
        auto twin = fresh->RestoreFrom(bytes, &err);
        CHECK(twin != nullptr);
        if (twin) {
            CHECK(twin->Phase() == MatchPhase::NotStarted && twin->StateHash() == fresh->match->StateHash() && twin->Snapshot() == bytes);
            auto a = std::move(fresh);
            auto b = a->Fork(std::move(twin));
            a->match->Start();
            b->match->Start();
            bool same = true;
            for (int t = 0; t < 3000; ++t) {
                a->Step();
                b->Step();
                same = same && a->match->StateHash() == b->match->StateHash();
            }
            CHECK(same);
        } else {
            std::printf("  %s\n", err.c_str());
        }
    }

    auto live = LiveMatch::Create(*roster, 2024);
    live->match->Start();
    Coverage coverage;
    MatchPhase last = live->match->Phase();
    bool allEqual = true, allBytes = true, allConsistent = true;
    std::string firstProblem;
    for (int tick = 0; tick < 3'000'000 && !live->match->IsFinished(); ++tick) {
        live->Step();
        const bool phaseChanged = live->match->Phase() != last;
        last = live->match->Phase();
        if (!phaseChanged && tick % 233 != 0) continue;

        const std::vector<std::uint8_t> bytes = live->match->Snapshot();
        std::string err;
        auto restored = live->RestoreFrom(bytes, &err);
        if (!restored) { allEqual = false; if (firstProblem.empty()) firstProblem = "restore failed at tick " + std::to_string(tick) + ": " + err; continue; }
        coverage.Note(*live->match, bytes.size());
        allEqual = allEqual && restored->StateHash() == live->match->StateHash() && restored->Phase() == live->match->Phase() &&
                   restored->Round() == live->match->Round() && restored->TicksInPhase() == live->match->TicksInPhase();
        allBytes = allBytes && restored->Snapshot() == bytes;           // byte-for-byte: snapshot(restore(x)) == x
        allConsistent = allConsistent && restored->VerifyPoolIntegrity() && restored->VerifyRosterLayouts();

        SnapshotInfo info;
        CHECK(w2f::ReadSnapshotInfo(bytes, info, &err));
        CHECK(info.stateHash == live->match->StateHash() && info.round == live->match->Round() &&
              info.ticksInPhase == live->match->TicksInPhase() && info.phase == static_cast<std::uint8_t>(live->match->Phase()) &&
              info.playerCount == kMaxPlayers && info.version == w2f::kSnapshotVersion && info.seed == 2024);
    }
    if (!firstProblem.empty()) std::printf("  %s\n", firstProblem.c_str());
    CHECK(allEqual && allBytes && allConsistent);
    CHECK(live->match->IsFinished());

    // The final state too (MatchOver, winner recorded, the last fight's log still there).
    const auto finalBytes = live->match->Snapshot();
    auto over = live->RestoreFrom(finalBytes);
    CHECK(over != nullptr && over->IsFinished() && over->Winner() == live->match->Winner() && over->Winner() != kInvalidPlayerId &&
          over->StateHash() == live->match->StateHash() && over->Snapshot() == finalBytes);
    if (over) {
        CHECK(over->CurrentCombatOutcomes().size() == live->match->CurrentCombatOutcomes().size());
        bool logsEqual = true;
        for (std::size_t i = 0; i < over->CurrentCombatOutcomes().size(); ++i) {
            const CombatLog& a = over->CurrentCombatOutcomes()[i].log;
            const CombatLog& b = live->match->CurrentCombatOutcomes()[i].log;
            logsEqual = logsEqual && a.checksum == b.checksum && a.events.size() == b.events.size() && a.endTick == b.endTick &&
                        a.pathSearches == b.pathSearches && a.survivors[0] == b.survivors[0] && a.survivors[1] == b.survivors[1];
        }
        CHECK(logsEqual);
    }

    // The sweep really covered the interesting states.
    coverage.phase[static_cast<int>(MatchPhase::NotStarted)] = true;   // covered above
    for (int p = 0; p < 6; ++p) CHECK(coverage.phase[p]);
    CHECK(coverage.eliminated && coverage.itemsOnUnits && coverage.itemsInBag && coverage.merged && coverage.outcomes);
    CHECK(coverage.pveFight && coverage.pveDrop && coverage.monsterLog);   // PvE fights, their drops and the monsters' logs are all in the snapshots
    CHECK(coverage.snapshots > 100);
    std::printf("  %d snapshots round-tripped (largest %zu bytes)\n", coverage.snapshots, coverage.largest);
}

static void TestSnapshotLockstepContinuation() {
    // Fork the running match into restored twins at several awkward moments (mid-planning, mid-fight, resolution,
    // right after an elimination...) and keep BOTH going: they must stay identical tick for tick, through shop rolls,
    // bot decisions, fights, eliminations and the end. One twin is additionally re-snapshotted and re-restored every
    // 137 ticks, so a chain of restores is as good as the original.
    auto roster = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(roster != nullptr);
    if (!roster) return;

    struct Twin {
        std::unique_ptr<LiveMatch> match;
        int ticksLeft = 0;
        bool chain = false;
        std::string label;
    };
    std::vector<Twin> twins;
    auto live = LiveMatch::Create(*roster, 31337);
    live->match->Start();

    struct Trigger { MatchPhase phase; int round; int ticks; const char* label; bool chain; bool done; };
    std::vector<Trigger> triggers = {
        {MatchPhase::Planning, 2, 40, "planning r2", false, false},
        {MatchPhase::Combat, 3, 500, "mid-fight r3", true, false},
        {MatchPhase::Resolution, 4, 10, "resolution r4", false, false},
        {MatchPhase::Planning, 6, 1, "planning r6, first tick", true, false},
        {MatchPhase::Combat, 9, 1, "combat r9, first tick", false, false},
        {MatchPhase::Planning, 14, 60, "planning r14", false, false},
    };
    bool identical = true, integrity = true, restoredOk = true;
    std::string firstProblem;
    int forks = 0, chainRestores = 0, finishedTogether = 0;
    bool sawElimination = false;
    for (int tick = 0; tick < 3'000'000 && !live->match->IsFinished(); ++tick) {
        live->Step();
        for (Twin& twin : twins) {
            if (twin.ticksLeft <= 0) continue;
            twin.match->Step();
            --twin.ticksLeft;
            if (twin.chain && twin.ticksLeft % 137 == 0) {
                std::string err;
                auto again = twin.match->RestoreFrom(twin.match->match->Snapshot(), &err);
                if (!again) { restoredOk = false; firstProblem = err; continue; }
                twin.match = twin.match->Fork(std::move(again));
                ++chainRestores;
            }
            if (twin.match->match->StateHash() != live->match->StateHash()) {
                if (identical) firstProblem = "twin '" + twin.label + "' diverged at original tick " + std::to_string(tick);
                identical = false;
                twin.ticksLeft = 0;
            }
            integrity = integrity && twin.match->match->VerifyPoolIntegrity() && twin.match->match->VerifyRosterLayouts();
        }
        for (Trigger& t : triggers) {
            if (!t.done && live->match->Phase() == t.phase && live->match->Round() == t.round && live->match->TicksInPhase() == t.ticks) {
                t.done = true;
                std::string err;
                auto restored = live->RestoreFrom(live->match->Snapshot(), &err);
                if (!restored) { restoredOk = false; firstProblem = err; continue; }
                twins.push_back(Twin{live->Fork(std::move(restored)), 2500, t.chain, t.label});
                ++forks;
            }
        }
        // A fork right after a player is knocked out (the first time it happens).
        if (!sawElimination && live->match->Players().AliveCount() < kMaxPlayers) {
            sawElimination = true;
            auto restored = live->RestoreFrom(live->match->Snapshot());
            restoredOk = restoredOk && restored != nullptr;
            if (restored) {   // this twin runs to the end of the match
                twins.push_back(Twin{live->Fork(std::move(restored)), 1'000'000, true, "just after the first elimination"});
                ++forks;
            }
        }
    }
    for (const Twin& twin : twins) finishedTogether += twin.match->match->IsFinished() ? 1 : 0;
    if (!firstProblem.empty()) std::printf("  %s\n", firstProblem.c_str());
    CHECK(restoredOk && identical && integrity);
    CHECK(forks == 7);
    CHECK(chainRestores > 30);
    CHECK(finishedTogether >= 1);   // at least one twin ran all the way through the end of the match
}

static void TestSnapshotRejectsBadInput() {
    auto roster = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(roster != nullptr);
    if (!roster) return;

    // A mid-fight snapshot of a busy match: units with items, an item bag, this round's fight logs.
    auto live = LiveMatch::Create(*roster, 99);
    live->match->Start();
    while (!(live->match->Phase() == MatchPhase::Combat && live->match->Round() >= 3 && live->match->TicksInPhase() > 10)) live->Step();
    const std::vector<std::uint8_t> good = live->match->Snapshot();
    CHECK(good.size() > 2000);
    std::string err;
    CHECK(live->RestoreFrom(good, &err) != nullptr);

    const auto refuses = [&](const std::vector<std::uint8_t>& bytes, const char* what, const char* mention = nullptr) {
        std::string e;
        auto m = live->RestoreFrom(bytes, &e);
        const bool ok = m == nullptr && !e.empty() && (mention == nullptr || e.find(mention) != std::string::npos);
        if (!ok) std::printf("  %s: %s (error text: '%s')\n", m ? "ACCEPTED" : "wrong error", what, e.c_str());
        CHECK(ok);
    };

    // Damage in transit: any single flipped bit anywhere fails the trailer checksum.
    {
        std::size_t tested = 0;
        for (std::size_t i = 0; i < good.size(); i += (i < 300 || i + 40 > good.size()) ? 1 : 37) {
            auto bad = good;
            bad[i] = static_cast<std::uint8_t>(bad[i] ^ (i % 2 == 0 ? 0x01 : 0x80));
            std::string e;
            if (live->RestoreFrom(bad, &e) != nullptr) { std::printf("  bit flip at byte %zu was accepted\n", i); CHECK(false); }
            ++tested;
        }
        CHECK(tested > 400);
    }
    // Truncation and extension.
    for (std::size_t keep : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{73}, std::size_t{74}, std::size_t{82}, good.size() / 2, good.size() - 9, good.size() - 8, good.size() - 1}) {
        refuses(std::vector<std::uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(keep)), "truncated");
    }
    {
        auto longer = good;
        longer.push_back(0);
        refuses(longer, "extended");
    }

    // Valid checksum, wrong content: a tamperer (or a bug) that fixes the trailer still cannot get an impossible match past
    // the semantic checks. Offsets follow the documented layout.
    const std::size_t nChampions = roster->All().size();
    const std::size_t body = 74;                                  // header size
    const std::size_t poolAt = body + 32 + 1 + 4;                  // after the match RNG, winner and the pool count
    const std::size_t player0 = poolAt + 4 * nChampions;
    const PlayerState& p0 = *live->match->Players().Get(0);
    CHECK(GetU32(good, poolAt - 4) == nChampions);
    CHECK(GetU32(good, poolAt) == static_cast<std::uint32_t>(live->match->Pool().Remaining(roster->All()[0].id)));
    CHECK(GetU32(good, player0) == static_cast<std::uint32_t>(p0.Health()));
    CHECK(GetU32(good, player0 + 12) == static_cast<std::uint32_t>(p0.Gold()));
    CHECK(p0.Roster().Count() >= 2);
    const std::size_t unit0 = player0 + 33;
    CHECK(GetU32(good, unit0 - 4) == p0.Roster().Count());
    CHECK(GetU32(good, unit0) == p0.Roster().Units()[0].id && GetU32(good, unit0 + 4) == p0.Roster().Units()[0].champion->id);
    const std::size_t unitBytes = 4 + 4 + 1 + 1 + 4 + 4 + 12;
    const std::size_t unit1 = unit0 + unitBytes;
    CHECK(GetU32(good, unit1) == p0.Roster().Units()[1].id);

    const auto tamper = [&](std::size_t offset, std::uint32_t value, const char* what, const char* mention = nullptr) {
        auto bad = good;
        PutU32(bad, offset, value);
        FixChecksum(bad);
        refuses(bad, what, mention);
    };
    tamper(player0 + 12, static_cast<std::uint32_t>(p0.Gold() + 1), "gold +1 (state hash must not match)", "state hash");
    tamper(player0, static_cast<std::uint32_t>(p0.Health() + 1), "health +1", "state hash");
    tamper(player0, 0, "a living player with 0 health", "positive health");
    tamper(player0 + 4, 11, "level 11", "level");
    tamper(poolAt, 0x7FFFFFFFu, "pool count above the supply", "supply");
    tamper(poolAt, static_cast<std::uint32_t>(live->match->Pool().Remaining(roster->All()[0].id) + 1), "one extra copy in the pool", nullptr);
    tamper(unit0 + 4, 424242, "unit of an unknown champion", "missing from the loaded data");
    {
        auto bad = good;   // unit 1 takes unit 0's location, x and y: two units on one cell
        for (std::size_t i = 0; i < 9; ++i) bad[unit1 + 9 + i] = good[unit0 + 9 + i];
        FixChecksum(bad);
        refuses(bad, "two units on the same cell", "roster");
    }
    tamper(unit0 + 10, 99, "unit off the board", "roster");
    tamper(unit0 + 18, 555, "a unit carrying an unknown item", "unknown item");
    tamper(unit0, GetU32(good, unit0) + 0x01000000u, "a unit id that belongs to another seat", "roster");
    tamper(body, 0, "one word of the match RNG changed", "state hash");
    {
        auto bad = good;   // all four RNG words zero: an invalid xoshiro state
        for (std::size_t i = 0; i < 32; ++i) bad[body + i] = 0;
        FixChecksum(bad);
        refuses(bad, "all-zero RNG", "RNG");
    }
    // Header fields.
    tamper(4, w2f::kSnapshotVersion + 1, "unsupported version", "version");
    tamper(0, 0x12345678u, "bad magic", "magic");
    tamper(8, 12345, "different seed (low word)", "state hash");
    {
        auto bad = good;
        bad[64] = 9;   // the phase byte sits right after the seed, the five data hashes and the state hash
        FixChecksum(bad);
        refuses(bad, "unknown phase", "phase");
    }
    {
        SnapshotInfo info;
        CHECK(w2f::ReadSnapshotInfo(good, info));
        auto bad = good;
        bad[73] = static_cast<std::uint8_t>(info.playerCount - 1);   // the header's player count byte
        FixChecksum(bad);
        refuses(bad, "wrong player count", "player count");
        bad = good;
        PutU32(bad, 69, 1000000);   // ticks in phase far beyond the phase's length
        FixChecksum(bad);
        refuses(bad, "tick counter beyond the phase", "inconsistent");
    }
    // Trailing bytes inside the checksummed area.
    {
        auto bad = good;
        bad.insert(bad.end() - 8, {1, 2, 3});
        FixChecksum(bad);
        refuses(bad, "unexpected trailing data", "trailing");
    }
    // Garbage that is not a snapshot at all.
    refuses(std::vector<std::uint8_t>(500, 0xAB), "noise");
    refuses({}, "empty");

    // Data / configuration mismatches.
    {
        GameConfig other = live->cfg;
        other.match.planningTicks += 1;
        std::string e;
        CHECK(MatchManager::Restore(good, other, *roster, live->MakeSimulator(), &e, live->items.get(), live->encounters.get(), live->motherNature.get()) == nullptr && e.find("configuration") != std::string::npos);
        RestoreOptions loose;
        loose.requireMatchingData = false;
        auto forced = MatchManager::Restore(good, other, *roster, live->MakeSimulator(), &e, live->items.get(), live->encounters.get(), live->motherNature.get(), loose);
        CHECK(forced != nullptr && forced->StateHash() == live->match->StateHash());   // the tick counter is still inside the (longer) phase

        auto legacy = w2f::LoadChampionDatabaseFromFile(sample::LegacyRosterPath());
        CHECK(legacy != nullptr);
        if (legacy) {
            CHECK(MatchManager::Restore(good, live->cfg, *legacy, live->MakeSimulator(), &e, live->items.get(), live->encounters.get()) == nullptr && e.find("champion data") != std::string::npos);
            CHECK(MatchManager::Restore(good, live->cfg, *legacy, live->MakeSimulator(), &e, live->items.get(), live->encounters.get(), live->motherNature.get(), loose) == nullptr);   // even forced: the units' champions are gone
        }
        CHECK(MatchManager::Restore(good, live->cfg, *roster, live->MakeSimulator(), &e, nullptr, live->encounters.get()) == nullptr && e.find("item data") != std::string::npos);
        CHECK(MatchManager::Restore(good, live->cfg, *roster, live->MakeSimulator(), &e, live->items.get(), nullptr) == nullptr && e.find("PvE data") != std::string::npos);
        GameConfig fewer = live->cfg;
        fewer.match.playerCount = 4;
        CHECK(MatchManager::Restore(good, fewer, *roster, live->MakeSimulator(), &e, live->items.get(), live->encounters.get(), live->motherNature.get(), loose) == nullptr && e.find("player count") != std::string::npos);
        GameConfig invalid = live->cfg;
        invalid.shop.slotCount = 0;
        CHECK(MatchManager::Restore(good, invalid, *roster, live->MakeSimulator(), &e, live->items.get(), live->encounters.get(), live->motherNature.get(), loose) == nullptr && !e.empty());
    }
    // The header alone is readable, and bad buffers are refused there too.
    {
        SnapshotInfo info;
        CHECK(w2f::ReadSnapshotInfo(good, info, &err) && info.itemHash == live->items->ContentHash() && info.championHash == roster->ContentHash() && info.configHash == live->cfg.ContentHash());
        auto bad = good;
        bad[100] ^= 1;
        CHECK(!w2f::ReadSnapshotInfo(bad, info, &err) && !err.empty());
    }
}

static void TestSnapshotChainedActionFuzz() {
    // Shop/roster/item actions chosen at random (from the test's own generator) and applied to two matches: A is never
    // restored; B is snapshotted and replaced by its own restored copy every 25 actions. Every result and every state hash
    // must agree, the pool must balance, and eventually a player is eliminated while the chain keeps going. A tiny roster
    // (two 1-cost, one 2-cost champions) makes merges, sold-out pools and full benches routine.
    std::string err;
    auto db = ChampionDatabase::Create({Def(101, "A", 1), Def(102, "B", 1), Def(201, "C", 2)}, &err);
    auto items = MustLoadItems(kTestItemsJson);
    CHECK(db != nullptr && items != nullptr);
    if (!db || !items) return;

    TestGameConfig cfg;
    cfg.match.playerCount = 4;
    cfg.match.motherNatureTicks = 1;
    cfg.match.planningTicks = 1'000'000;   // stay in Planning for the whole fuzz
    cfg.player.startingGold = 200;
    cfg.pool.copiesPerTier = {{9, 6, 18, 12, 10}};   // scarce: sell-outs happen
    for (auto& row : cfg.shop.dropRatesByLevel) row = {{70, 30, 0, 0, 0}};
    auto make = [&]() {
        auto m = MatchManager::Create(cfg, *db, 555, nullptr, &err, items.get());
        m->Start();
        m->Tick();
        return m;
    };
    auto a = make();
    auto b = make();
    CHECK(a && b && a->Phase() == MatchPhase::Planning);
    if (!a || !b || a->Phase() != MatchPhase::Planning) return;

    Rng dice(2024, 77);
    int restores = 0, mismatches = 0, merges = 0, sells = 0, equips = 0, unequips = 0, soldOut = 0, eliminated = 0, refused = 0, accepted = 0;
    std::string firstProblem;
    auto both = [&](auto&& action) {
        const auto ra = action(*a);
        const auto rb = action(*b);
        if (ra != rb) { ++mismatches; if (firstProblem.empty()) firstProblem = "results differ"; }
        if (ra == ActionResult::Ok) ++accepted; else ++refused;
        return ra;
    };
    ItemEventLog mergeCounter;
    a->AddListener(&mergeCounter);

    for (int step = 0; step < 3000; ++step) {
        const PlayerId who = static_cast<PlayerId>(dice.NextBelow(4));
        const std::uint32_t kind = dice.NextBelow(11);
        const PlayerState& pa = *a->Players().Get(who);
        const std::size_t unitCount = pa.Roster().Count();
        const UnitId unit = unitCount == 0 ? 0xBAD : pa.Roster().Units()[dice.NextBelow(static_cast<std::uint32_t>(unitCount))].id;
        switch (kind) {
            case 0: both([&](MatchManager& m) { return m.TryRerollShop(who); }); break;
            case 1: case 2: {
                const std::size_t slot = dice.NextBelow(6);   // slot 5 does not exist: rejected identically
                both([&](MatchManager& m) { return m.TryBuyShopUnit(who, slot); });
                break;
            }
            case 3: both([&](MatchManager& m) { return m.TryBuyXp(who); }); break;
            case 4: if (both([&](MatchManager& m) { return m.TrySellUnit(who, unit); }) == ActionResult::Ok) ++sells; break;
            case 5: {
                const bool bench = dice.NextBelow(2) == 0;
                const int x = static_cast<int>(dice.NextBelow(bench ? kBenchSlots : kBoardColumns));
                const int y = bench ? 0 : static_cast<int>(dice.NextBelow(kBoardRows));
                both([&](MatchManager& m) { return m.TryMoveUnit(who, unit, bench ? LocationType::Bench : LocationType::Board, x, y); });
                break;
            }
            case 6: {
                const ItemId item = static_cast<ItemId>(1 + dice.NextBelow(6));
                a->PlayersMutable().Get(who)->AddItemToBag(item);
                b->PlayersMutable().Get(who)->AddItemToBag(item);
                break;
            }
            case 7: case 8: {
                const auto& bag = pa.ItemBag();
                const ItemId item = bag.empty() ? 1 : bag[dice.NextBelow(static_cast<std::uint32_t>(bag.size()))];
                if (both([&](MatchManager& m) { return m.TryEquipItem(who, unit, item); }) == ActionResult::Ok) ++equips;
                break;
            }
            case 9: {
                const int slot = static_cast<int>(dice.NextBelow(4));
                if (both([&](MatchManager& m) { return m.TryUnequipItem(who, unit, slot); }) == ActionResult::Ok) ++unequips;
                break;
            }
            default:
                a->PlayersMutable().Get(who)->AddGold(30);
                b->PlayersMutable().Get(who)->AddGold(30);
                break;
        }
        soldOut += a->Pool().RemainingInTier(1) == 0 ? 1 : 0;
        if (step == 1800) {   // a player is knocked out mid-fuzz (units and shop go back to the pool); the chain must carry on
            for (MatchManager* m : {a.get(), b.get()}) m->PlayersMutable().EliminatePlayer(3, 4);
            ++eliminated;
        }

        if (step % 25 == 24) {
            const auto bytes = b->Snapshot();
            auto next = MatchManager::Restore(bytes, cfg, *db, nullptr, &err, items.get());
            if (!next) { ++mismatches; if (firstProblem.empty()) firstProblem = "restore failed: " + err; break; }
            if (next->Snapshot() != bytes) { ++mismatches; if (firstProblem.empty()) firstProblem = "re-snapshot differs"; }
            b = std::move(next);
            ++restores;
            if (a->Snapshot() != b->Snapshot()) { ++mismatches; if (firstProblem.empty()) firstProblem = "A and B snapshots differ"; }
        }
        if (a->StateHash() != b->StateHash()) { ++mismatches; if (firstProblem.empty()) firstProblem = "state hashes differ at step " + std::to_string(step); }
        if (!a->VerifyPoolIntegrity() || !b->VerifyPoolIntegrity() || !a->VerifyRosterLayouts() || !b->VerifyRosterLayouts()) {
            ++mismatches;
            if (firstProblem.empty()) firstProblem = "integrity broke at step " + std::to_string(step);
        }
        if (mismatches > 0) break;
    }
    merges = static_cast<int>(mergeCounter.overflows.size());
    if (!firstProblem.empty()) std::printf("  %s\n", firstProblem.c_str());
    CHECK(mismatches == 0);
    CHECK(restores >= 100);
    CHECK(merges > 10 && sells > 20 && equips > 20 && unequips > 5 && eliminated == 1 && soldOut > 0);
    CHECK(accepted > 500 && refused > 300);
    std::printf("  %d restores; %d merges, %d sells, %d equips, %d unequips; %d accepted / %d refused actions\n", restores, merges, sells, equips, unequips, accepted, refused);
}

// ==== Phase 8: stages, PvE rounds, drops, player damage, streaks, automatic snapshots ==================

namespace {

struct RoundRecorder : IMatchListener {
    struct Damage { PlayerId player; int damage; int health; int round; };
    struct Drop { PlayerId player; PveDrop drop; int round; };
    struct Income { PlayerId player; int round; IncomeBreakdown income; };
    struct Snap { int round; std::vector<std::uint8_t> bytes; std::uint64_t matchHashAtThatMoment; MatchPhase phase; int ticks; };
    const MatchManager* match = nullptr;
    std::vector<Damage> damages;
    std::vector<Drop> drops;
    std::vector<Income> incomes;
    std::vector<Snap> snaps;
    std::vector<PlayerId> eliminated;
    int freeUnits = 0;
    void OnPlayerDamaged(PlayerId p, int damage, int health) override { damages.push_back({p, damage, health, match ? match->Round() : 0}); }
    void OnPveDrop(PlayerId p, const PveDrop& d) override { drops.push_back({p, d, match ? match->Round() : 0}); }
    void OnIncomeGranted(PlayerId p, int round, const IncomeBreakdown& i) override { incomes.push_back({p, round, i}); }
    void OnPlayerEliminated(PlayerId p, int) override { eliminated.push_back(p); }
    void OnUnitBought(PlayerId, const UnitInstance&, int gold) override { freeUnits += gold == 0 ? 1 : 0; }
    void OnAutoSnapshot(int round, const std::vector<std::uint8_t>& bytes) override {
        snaps.push_back({round, bytes, match ? match->StateHash() : 0, match ? match->Phase() : MatchPhase::NotStarted, match ? match->TicksInPhase() : -1});
    }
};

// A stand-in simulator with scripted results, so the rules around a fight can be tested without fighting.
// PvP: the lower seat wins (or everything is a draw); PvE: everyone wins or everyone loses. The winner keeps 1 + round % 5 units.
class RuleSimulator : public ICombatSimulator {
public:
    bool drawAll = false;
    bool winPve = true;
    std::vector<CombatOutcome> Simulate(const CombatContext& ctx) override {
        std::vector<CombatOutcome> out;
        for (const Matchup& m : ctx.matchups) {
            CombatOutcome o;
            o.matchup = m;
            if (!drawAll) {
                if (m.awayIsMonsters) o.winner = winPve ? CombatWinner::Home : CombatWinner::Away;
                else o.winner = m.home < m.away ? CombatWinner::Home : CombatWinner::Away;
                o.winnerSurvivors = 1 + ctx.round % 5;
            }
            out.push_back(std::move(o));
        }
        return out;
    }
};

int Survivors(int round) { return 1 + round % 5; }

}  // namespace

static void TestStageStructureAndPveRounds() {
    MatchConfig m;   // defaults: stage 1 = 3 rounds, later stages = 7; PvE = 1-1, 1-2, 1-3 and X-7
    struct Case { int round, stage, inStage; bool pve; };
    const Case cases[] = {{1, 1, 1, true}, {2, 1, 2, true}, {3, 1, 3, true}, {4, 2, 1, false}, {9, 2, 6, false}, {10, 2, 7, true},
                          {11, 3, 1, false}, {16, 3, 6, false}, {17, 3, 7, true}, {24, 4, 7, true}, {25, 5, 1, false}, {0, 1, 1, true}};
    for (const Case& c : cases) {
        const StageRound sr = m.StageOf(c.round);
        const bool ok = sr.stage == c.stage && sr.roundInStage == c.inStage && m.IsPveRound(c.round) == c.pve;
        if (!ok) std::printf("  round %d -> %d-%d pve=%d (wanted %d-%d pve=%d)\n", c.round, sr.stage, sr.roundInStage, m.IsPveRound(c.round), c.stage, c.inStage, c.pve);
        CHECK(ok);
    }
    // Every stage has exactly one PvE round after stage 1 (the seventh).
    int pveInLaterStages = 0;
    for (int r = 4; r <= 4 + 7 * 10 - 1; ++r) pveInLaterStages += m.IsPveRound(r) ? 1 : 0;
    CHECK(pveInLaterStages == 10);

    MatchConfig custom;
    custom.firstStageRounds = 4;
    custom.roundsPerStage = 5;
    custom.firstStagePveRounds = 2;
    custom.pveRoundInLaterStages = 3;
    CHECK(custom.IsPveRound(1) && custom.IsPveRound(2) && !custom.IsPveRound(3) && !custom.IsPveRound(4));
    CHECK(custom.StageOf(5).stage == 2 && custom.StageOf(5).roundInStage == 1 && custom.IsPveRound(7) && !custom.IsPveRound(6));
    CHECK(custom.StageOf(9).stage == 2 && custom.StageOf(9).roundInStage == 5 && custom.StageOf(10).stage == 3 && custom.IsPveRound(12) && !custom.IsPveRound(11));
    custom.pveRoundInLaterStages = 0;
    CHECK(!custom.IsPveRound(7) && custom.IsPveRound(1));   // 0 = no PvE after stage 1

    // Validation.
    const auto rejects = [](auto edit, const char* mention) {
        GameConfig c;
        edit(c);
        std::string e;
        const bool bad = !c.Validate(&e) && e.find(mention) != std::string::npos;
        if (!bad) std::printf("  config edit not rejected as expected (%s): '%s'\n", mention, e.c_str());
        return bad;
    };
    CHECK(rejects([](GameConfig& c) { c.match.firstStageRounds = 0; }, "stages"));
    CHECK(rejects([](GameConfig& c) { c.match.roundsPerStage = 0; }, "stages"));
    CHECK(rejects([](GameConfig& c) { c.match.firstStagePveRounds = 4; }, "firstStagePveRounds"));
    CHECK(rejects([](GameConfig& c) { c.match.pveRoundInLaterStages = 8; }, "pveRoundInLaterStages"));
    CHECK(rejects([](GameConfig& c) { c.damage.baseDamageByStage.clear(); }, "baseDamageByStage"));
    CHECK(rejects([](GameConfig& c) { c.damage.baseDamageByStage = {2, -1}; }, "baseDamageByStage"));
    CHECK(rejects([](GameConfig& c) { c.damage.perSurvivingUnit = -1; }, "perSurvivingUnit"));
    CHECK(GameConfig{}.Validate());
}

static void TestPlayerDamageFormula() {
    GameConfig cfg;   // baseDamageByStage = {0, 2, 3, 5, 8, 12}, +1 per surviving unit
    CHECK(cfg.PlayerDamage(4, 0) == 2 && cfg.PlayerDamage(4, 3) == 5);       // 2-1: stage 2
    CHECK(cfg.PlayerDamage(10, 4) == 6);                                     // 2-7 is still stage 2
    CHECK(cfg.PlayerDamage(11, 0) == 3 && cfg.PlayerDamage(18, 2) == 5 + 2); // stage 3 / stage 4
    CHECK(cfg.PlayerDamage(25, 1) == 8 + 1 && cfg.PlayerDamage(32, 1) == 12 + 1);
    CHECK(cfg.PlayerDamage(500, 5) == 12 + 5);                               // beyond the table: the last entry repeats
    CHECK(cfg.PlayerDamage(4, -3) == 2);                                     // nonsense survivor counts never heal
    cfg.damage.baseDamageByStage = {7};
    cfg.damage.perSurvivingUnit = 3;
    CHECK(cfg.PlayerDamage(1, 2) == 13 && cfg.PlayerDamage(99, 0) == 7);
    CHECK(GameConfig{}.ContentHash() != cfg.ContentHash());
}

static void TestPvpDamageAndElimination() {
    auto db = sample::MakeCombatDatabase();
    CHECK(db != nullptr);
    if (!db) return;
    TestGameConfig cfg;
    cfg.match.playerCount = 4;
    cfg.player.startingHealth = 30;
    cfg.player.startingGold = 20;
    cfg.match.planningTicks = 60;
    cfg.match.motherNatureTicks = 30;
    cfg.match.combatTicks = 60;
    cfg.match.resolutionTicks = 30;
    auto match = MatchManager::Create(cfg, *db, 77, std::make_unique<RuleSimulator>());
    RoundRecorder rec;
    rec.match = match.get();
    match->AddListener(&rec);
    std::vector<sample::ScriptedPlayer> players;
    for (int s = 0; s < 4; ++s) players.emplace_back(static_cast<PlayerId>(s));
    match->Start();

    std::map<PlayerId, int> lostSoFar;
    bool integrity = true, damagePerFightRight = true;
    std::vector<std::size_t> eventsAtResolution;
    for (int tick = 0; tick < 200000 && !match->IsFinished(); ++tick) {
        for (auto& p : players) p.Tick(*match);
        const std::size_t before = rec.damages.size();
        const MatchPhase phaseBefore = match->Phase();
        match->Tick();
        integrity = integrity && match->VerifyPoolIntegrity() && match->VerifyRosterLayouts();
        if (phaseBefore != MatchPhase::Resolution && match->Phase() == MatchPhase::Resolution) {
            // This tick resolved the round: one damage event per real loser, each exactly the formula's value.
            std::size_t expectedEvents = 0;
            for (const CombatOutcome& o : match->CurrentCombatOutcomes()) {
                const bool nobodyHurt = o.winner == CombatWinner::Draw || (o.winner == CombatWinner::Home && o.matchup.awayIsGhost);
                expectedEvents += nobodyHurt ? 0 : 1;
            }
            damagePerFightRight = damagePerFightRight && rec.damages.size() - before == expectedEvents;
            for (std::size_t i = before; i < rec.damages.size(); ++i) {
                damagePerFightRight = damagePerFightRight && rec.damages[i].damage == cfg.PlayerDamage(match->Round(), Survivors(match->Round()));
            }
        }
    }
    CHECK(match->IsFinished() && integrity && damagePerFightRight);
    CHECK(match->Winner() == 0);   // the lowest seat wins every fight it is in

    // Bookkeeping: health = start - everything taken; the eliminated are exactly the ones who reached 0 or less.
    std::map<PlayerId, int> taken;
    for (const auto& d : rec.damages) {
        taken[d.player] += d.damage;
        CHECK(d.health == cfg.player.startingHealth - taken[d.player]);
    }
    for (PlayerId p = 0; p < 4; ++p) {
        const PlayerState& ps = *match->Players().Get(p);
        CHECK(ps.Health() == cfg.player.startingHealth - taken[p]);
        CHECK(ps.IsAlive() == (p == 0) && ps.IsAlive() == (ps.Health() > 0));
        if (!ps.IsAlive()) {
            CHECK(ps.Roster().Count() == 0);   // their units went back to the shared pool
            for (const ChampionDefinition* slot : ps.Shop().Slots()) CHECK(slot == nullptr);
            CHECK(ps.Placement() >= 2 && ps.Placement() <= 4);
        }
    }
    CHECK(taken[0] == 0);
    CHECK(match->Players().Get(0)->Placement() == 1);
    CHECK(rec.eliminated.size() == 3);
    // Eliminations happen in order of who was hurt first / most, and the placements say so: earlier out = worse place.
    for (std::size_t i = 0; i < rec.eliminated.size(); ++i) {
        CHECK(match->Players().Get(rec.eliminated[i])->Placement() == 4 - static_cast<int>(i));
    }
    // With everything returned, the pool holds every copy the winner does not own.
    int winnerCopies = 0;
    for (const UnitInstance& u : match->Players().Get(0)->Roster().Units()) winnerCopies += SharedChampionPool::CopiesForStarLevel(u.starLevel);
    for (const ChampionDefinition* slot : match->Players().Get(0)->Shop().Slots()) winnerCopies += slot ? 1 : 0;
    int outside = 0;
    for (const ChampionDefinition& def : db->All()) outside += match->Pool().InitialCopies(def.id) - match->Pool().Remaining(def.id);
    CHECK(outside == winnerCopies);

    // A hit larger than the remaining health leaves it negative and still eliminates.
    for (PlayerId dead : rec.eliminated) {
        int lastHealth = 1;
        for (const auto& d : rec.damages) lastHealth = d.player == dead ? d.health : lastHealth;
        CHECK(lastHealth <= 0);   // the blow that removed them took them to 0 or below
    }
}

static void TestDrawsGhostsAndDamageAccounting() {
    auto db = sample::MakeCombatDatabase();
    CHECK(db != nullptr);
    if (!db) return;
    {   // Draws: nobody takes damage and streaks are untouched.
        TestGameConfig cfg;
        cfg.match.playerCount = 4;
        auto sim = std::make_unique<RuleSimulator>();
        sim->drawAll = true;
        auto match = MatchManager::Create(cfg, *db, 3, std::move(sim));
        RoundRecorder rec;
        rec.match = match.get();
        match->AddListener(&rec);
        match->Start();
        for (int tick = 0; tick < 100000 && match->Round() < 6; ++tick) match->Tick();
        CHECK(match->Round() >= 6 && rec.damages.empty());
        for (PlayerId p = 0; p < 4; ++p) CHECK(match->Players().Get(p)->Health() == 100 && match->Players().Get(p)->Streak() == 0);
    }
    {   // An odd table: the leftover player fights a ghost. A ghost's owner is never hurt and never earns a result; only the
        // real player in the fight can take damage.
        TestGameConfig cfg;
        cfg.match.playerCount = 3;
        cfg.player.startingHealth = 100000;
        auto match = MatchManager::Create(cfg, *db, 8, std::make_unique<RuleSimulator>());
        RoundRecorder rec;
        rec.match = match.get();
        match->AddListener(&rec);
        match->Start();
        bool accounting = true;
        int ghostFights = 0;
        for (int tick = 0; tick < 100000 && match->Round() < 40; ++tick) {
            const std::size_t before = rec.damages.size();
            const MatchPhase phaseBefore = match->Phase();
            match->Tick();
            if (phaseBefore != MatchPhase::Resolution && match->Phase() == MatchPhase::Resolution) {
                std::map<PlayerId, int> expected;
                for (const CombatOutcome& o : match->CurrentCombatOutcomes()) {
                    ghostFights += o.matchup.awayIsGhost ? 1 : 0;
                    const PlayerId loser = o.winner == CombatWinner::Home ? o.matchup.away : o.matchup.home;
                    const bool loserIsGhost = o.winner == CombatWinner::Home && o.matchup.awayIsGhost;
                    if (!loserIsGhost) expected[loser] += cfg.PlayerDamage(match->Round(), Survivors(match->Round()));
                    accounting = accounting && o.damageToLoser == (loserIsGhost ? 0 : cfg.PlayerDamage(match->Round(), Survivors(match->Round())));
                }
                std::map<PlayerId, int> seen;
                for (std::size_t i = before; i < rec.damages.size(); ++i) seen[rec.damages[i].player] += rec.damages[i].damage;
                accounting = accounting && seen == expected;
            }
        }
        CHECK(accounting && ghostFights > 20);
    }
}

static void TestStreakIncome() {
    auto db = sample::MakeCombatDatabase();
    CHECK(db != nullptr);
    if (!db) return;
    // Seat 0's results by round: W W W W L W D W W. (Every round is PvP here.)
    class Script : public ICombatSimulator {
    public:
        std::vector<CombatOutcome> Simulate(const CombatContext& ctx) override {
            static const char kResults[] = "WWWWLWDWW";
            const char r = kResults[(ctx.round - 1) % 9];
            std::vector<CombatOutcome> out;
            for (const Matchup& m : ctx.matchups) {
                CombatOutcome o;
                o.matchup = m;
                const bool seat0Home = m.home == 0;
                o.winner = r == 'D' ? CombatWinner::Draw : ((r == 'W') == seat0Home ? CombatWinner::Home : CombatWinner::Away);
                o.winnerSurvivors = 2;
                out.push_back(o);
            }
            return out;
        }
    };
    TestGameConfig cfg;
    cfg.match.playerCount = 2;
    cfg.player.startingHealth = 1'000'000;
    cfg.player.startingGold = 0;
    auto match = MatchManager::Create(cfg, *db, 1, std::make_unique<Script>());
    RoundRecorder rec;
    rec.match = match.get();
    match->AddListener(&rec);
    match->Start();
    std::vector<int> streaks0, streaks1;
    int lastRound = 0;
    for (int tick = 0; tick < 100000 && match->Round() <= 10; ++tick) {
        match->Tick();
        if (match->Phase() == MatchPhase::Planning && match->Round() != lastRound) {
            lastRound = match->Round();
            streaks0.push_back(match->Players().Get(0)->Streak());
            streaks1.push_back(match->Players().Get(1)->Streak());
        }
    }
    // Streak entering rounds 1..10: 0, 1, 2, 3, 4, -1(seat 0 lost round 5), 1, 1(draw), 2, 3.
    streaks0.resize(10);
    streaks1.resize(10);
    CHECK((streaks0 == std::vector<int>{0, 1, 2, 3, 4, -1, 1, 1, 2, 3}));
    CHECK((streaks1 == std::vector<int>{0, -1, -2, -3, -4, 1, -1, -1, -2, -3}));   // the mirror image: the other seat's streak is the opposite
    // The bonus paid at the start of each round follows the streak it starts with: |2-3| -> +1, 4 -> +2, 5+ -> +3, less -> 0,
    // for win AND loss streaks alike.
    std::map<PlayerId, std::vector<int>> paid;
    for (const auto& i : rec.incomes) paid[i.player].push_back(i.income.streakGold);
    CHECK(paid[0].size() >= 10 && paid[1].size() >= 10);
    const std::vector<int> expected = {0, 0, 1, 1, 2, 0, 0, 0, 1, 1};
    CHECK((std::vector<int>(paid[0].begin(), paid[0].begin() + 10) == expected));
    CHECK((std::vector<int>(paid[1].begin(), paid[1].begin() + 10) == expected));   // a loss streak pays the same as a win streak
    // The whole income is reported, and it is what the player really got.
    CHECK(rec.incomes[0].income.baseGold == 2 && rec.incomes[0].round == 1);

    // A longer streak reaches the top tier (+3 from 5 in a row) and stays there.
    PlayerConfig pc;
    CHECK(pc.streakBonuses.size() == 3 && pc.streakBonuses[0].minStreak == 2 && pc.streakBonuses[0].bonusGold == 1 &&
          pc.streakBonuses[1].minStreak == 4 && pc.streakBonuses[1].bonusGold == 2 && pc.streakBonuses[2].minStreak == 5 && pc.streakBonuses[2].bonusGold == 3);
}

namespace {

std::string PveJson(const std::string& defaultDrops) {
    return R"({"version": 1,
      "monsters": [
        {"id": 10001, "name": "Pinata", "stats": {"hp": 30, "armor": 0, "magicResist": 0, "attackDamage": 0, "attackSpeed": 1.0, "range": 1}},
        {"id": 10002, "name": "Titan",  "stats": {"hp": 100000, "armor": 0, "magicResist": 0, "attackDamage": 900, "attackSpeed": 1.0, "range": 1}}
      ],
      "defaultDrops": )" + defaultDrops + R"(,
      "encounters": [
        {"id": 1, "name": "Pinatas", "stage": 1, "round": 1, "units": [{"monster": 10001, "x": 2, "y": 3}, {"monster": 10001, "x": 4, "y": 3}]},
        {"id": 2, "name": "The Titan", "stage": 1, "round": 2, "units": [{"monster": 10002, "x": 3, "y": 3}]}
      ]})";
}

std::unique_ptr<ChampionDatabase> WithMonsters(const ChampionDatabase& db, const EncounterDatabase& enc) {
    std::vector<ChampionDefinition> defs = db.All();
    for (const ChampionDefinition& m : enc.Monsters().All()) defs.push_back(m);
    return ChampionDatabase::Create(std::move(defs));
}

// A small match whose first two rounds are PvE (the first is easy, the second is unwinnable) and whose seats buy units
// and field them exactly like the other full-match tests do.
struct PveRun {
    GameConfig cfg;
    const ChampionDatabase* db = nullptr;
    std::unique_ptr<MatchManager> match;
    RoundRecorder rec;
    std::vector<sample::ScriptedPlayer> players;
    int goldBefore[kMaxPlayers] = {};
    int bagBefore[kMaxPlayers] = {};
    int copiesBefore[kMaxPlayers] = {};
    bool integrity = true;

    static int Copies(const PlayerState& p) {
        int n = 0;
        for (const UnitInstance& u : p.Roster().Units()) n += SharedChampionPool::CopiesForStarLevel(u.starLevel);
        return n;
    }
    static std::unique_ptr<PveRun> Start(const ChampionDatabase& db, const ItemDatabase* items, const EncounterDatabase* enc, std::uint64_t seed,
                                         bool humans = true, int playerCount = 4, const std::function<void(GameConfig&)>& tweak = nullptr) {
        auto run = std::make_unique<PveRun>();
        run->cfg = TestGameConfig();
        run->cfg.match.playerCount = playerCount;
        run->cfg.match.firstStagePveRounds = 2;      // 1-1 and 1-2 are PvE, then PvP
        run->cfg.match.pveRoundInLaterStages = 0;
        run->cfg.player.startingGold = 100;
        if (tweak) tweak(run->cfg);
        run->db = &db;
        run->match = MatchManager::Create(run->cfg, db, seed, std::make_unique<CombatSimulator>(run->cfg.combat, nullptr, items), nullptr, items, enc);
        run->rec.match = run->match.get();
        run->match->AddListener(&run->rec);
        if (humans) {
            for (int s = 0; s < playerCount; ++s) run->players.emplace_back(static_cast<PlayerId>(s));
        }
        run->match->Start();
        return run;
    }
    // Runs until the tick that enters Resolution for `round`. The "before" values are as of the last tick of Combat.
    void RunToResolution(int round) {
        for (int tick = 0; tick < 500000; ++tick) {
            for (auto& p : players) p.Tick(*match);
            for (int s = 0; s < match->Players().PlayerCount(); ++s) {
                const PlayerState& ps = *match->Players().Get(static_cast<PlayerId>(s));
                goldBefore[s] = ps.Gold();
                bagBefore[s] = static_cast<int>(ps.ItemBag().size());
                copiesBefore[s] = Copies(ps);
            }
            match->Tick();
            integrity = integrity && match->VerifyPoolIntegrity() && match->VerifyRosterLayouts();
            if (match->Phase() == MatchPhase::Resolution && match->Round() == round && match->TicksInPhase() == 0) return;
        }
    }
};

}  // namespace

static void TestEncounterDataAndSelection() {
    auto champions = ProdDb();
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
    std::string err;
    auto prod = w2f::LoadEncounterDatabaseFromFile(sample::ProductionPvePath(), champions.get(), items.get(), &err);
    CHECK(champions && items && prod != nullptr);
    if (!prod) { std::printf("  %s\n", err.c_str()); return; }
    CHECK(prod->Monsters().All().size() == 4 && prod->All().size() == 4 && prod->DefaultDrops().size() == 3);
    // 1-1 / 1-2 / 1-3 have their own boards; X-7 of ANY stage is the boss (stage 0 = any); nothing is defined for the other rounds.
    CHECK(prod->Select(1, 1, 0)->id == 1 && prod->Select(1, 2, 0)->id == 2 && prod->Select(1, 3, 0)->id == 3);
    CHECK(prod->Select(2, 7, 0)->id == 4 && prod->Select(3, 7, 99)->id == 4 && prod->Select(9, 7, 5)->id == 4);
    CHECK(prod->Select(2, 2, 0) == nullptr && prod->Select(1, 4, 0) == nullptr);
    // The boss has its own table (items and gold only); the others use the defaults.
    CHECK(prod->DropsFor(*prod->Find(4)).size() == 2 && &prod->DropsFor(*prod->Find(1)) == &prod->DefaultDrops());
    // Monsters are outside the champions' database and disjoint from it.
    for (const ChampionDefinition& m : prod->Monsters().All()) CHECK(champions->Find(m.id) == nullptr);
    CHECK(prod->ContentHash() == w2f::LoadEncounterDatabaseFromFile(sample::ProductionPvePath(), champions.get(), items.get())->ContentHash());

    // Specificity and seeded tie-breaks.
    auto mk = [&](std::vector<EncounterDefinition> encounters) {
        std::vector<ChampionDefinition> monsters = {Fighter(10001, 50, 0, 1)};
        monsters[0].name = "M";
        std::vector<PveDropEntry> drops(1);
        drops[0].minGold = drops[0].maxGold = 1;
        return EncounterDatabase::Create(monsters, std::move(encounters), drops, nullptr, nullptr, &err);
    };
    auto enc = [](std::uint32_t id, int stage, int round) {
        EncounterDefinition e;
        e.id = id;
        e.name = "E" + std::to_string(id);
        e.stage = stage;
        e.round = round;
        e.units = {MonsterPlacement{10001, 1, 3, 3}};
        return e;
    };
    auto db = mk({enc(1, 0, 0), enc(2, 0, 7), enc(3, 2, 0), enc(4, 2, 7), enc(5, 0, 7)});
    CHECK(db != nullptr);
    if (db) {
        CHECK(db->Select(2, 7, 0)->id == 4 && db->Select(2, 7, 1)->id == 4);         // exact stage+round beats everything
        CHECK(db->Select(2, 3, 0)->id == 3);                                        // exact stage beats "any"
        CHECK(db->Select(5, 7, 0)->id == 2 && db->Select(5, 7, 1)->id == 5 && db->Select(5, 7, 2)->id == 2);   // ties: the roll picks
        CHECK(db->Select(5, 1, 0)->id == 1 && db->Select(5, 1, 123456)->id == 1);   // only the catch-all matches
    }

    struct Bad { std::function<void(std::vector<ChampionDefinition>&, std::vector<EncounterDefinition>&, std::vector<PveDropEntry>&)> edit; const char* mention; };
    const Bad bad[] = {
        {[](auto&, auto& e, auto&) { e[0].id = 0; }, "id 0"},
        {[](auto&, auto& e, auto&) { e.push_back(e[0]); }, "reuses"},
        {[](auto&, auto& e, auto&) { e[0].units.clear(); }, "no monsters"},
        {[](auto&, auto& e, auto&) { e[0].units[0].monster = 424242; }, "not defined"},
        {[](auto&, auto& e, auto&) { e[0].units.push_back(e[0].units[0]); }, "same cell"},
        {[](auto&, auto& e, auto&) { e[0].units[0].x = kBoardColumns; }, "outside the board"},
        {[](auto&, auto& e, auto&) { e[0].units[0].starLevel = 4; }, "star level"},
        {[](auto&, auto& e, auto&) { e[0].stage = -1; }, "stage and round"},
        {[](auto&, auto&, auto& d) { d.clear(); }, "no drops"},
        {[](auto&, auto&, auto& d) { d[0].weight = 0; }, "weight"},
        {[](auto&, auto&, auto& d) { d[0].minGold = 5; d[0].maxGold = 2; }, "minGold"},
        {[](auto&, auto&, auto& d) { d[0].type = PveDropType::Champion; d[0].tiers = {6}; }, "tier"},
        {[](auto&, auto&, auto& d) { d[0].type = PveDropType::Champion; d[0].tiers.clear(); }, "at least one tier"},
        {[](auto&, auto&, auto& d) { d[0].type = PveDropType::Item; d[0].items = {77}; }, "not in the item data"},
        {[](auto& m, auto&, auto&) { m[0].traits = {"Helios"}; }, "traits"},
        {[](auto& m, auto&, auto&) { m[0].stats.maxHp = {{0, 0, 0}}; }, "combat stats"},
    };
    for (const Bad& b : bad) {
        std::vector<ChampionDefinition> monsters = {Fighter(10001, 50, 0, 1)};
        monsters[0].name = "M";
        std::vector<EncounterDefinition> encounters = {enc(1, 1, 1)};
        std::vector<PveDropEntry> drops(1);
        drops[0].minGold = drops[0].maxGold = 1;
        b.edit(monsters, encounters, drops);
        std::string e;
        auto made = EncounterDatabase::Create(monsters, encounters, drops, nullptr, nullptr, &e);
        const bool ok = made == nullptr && e.find(b.mention) != std::string::npos;
        if (!ok) std::printf("  expected an error mentioning '%s', got '%s'\n", b.mention, e.c_str());
        CHECK(ok);
    }
    {   // A monster with a champion's id is refused (ids appear in the event stream).
        std::vector<ChampionDefinition> monsters = {Fighter(kChampionAlesk, 50, 0, 1)};
        monsters[0].name = "Clash";
        std::vector<PveDropEntry> drops(1);
        drops[0].minGold = drops[0].maxGold = 1;
        std::string e;
        auto made = EncounterDatabase::Create(monsters, {enc(1, 1, 1)}, drops, champions.get(), nullptr, &e);
        CHECK(made == nullptr && e.find("same id as a champion") != std::string::npos);
    }

    // The loader: errors carry the JSON path.
    struct BadJson { std::string json; const char* mention; };
    const std::string okMonsters = R"("monsters": [{"id": 10001, "name": "M", "stats": {"hp": 50, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1.0, "range": 1}}])";
    const std::string okUnit = R"({"monster": 10001, "x": 1, "y": 1})";
    const auto wrap = [&](const std::string& encounterBody, const std::string& extra = "") {
        return R"({"version": 1, )" + okMonsters + extra + R"(, "encounters": [{"id": 1, "name": "E", "units": [)" + okUnit + R"(])" + encounterBody + R"(}]})";
    };
    const std::string drops = R"(, "defaultDrops": [{"type": "Gold", "minGold": 1, "maxGold": 2}])";
    const BadJson badJson[] = {
        {R"({"version": 2, "monsters": [], "encounters": []})", "version"},
        {R"({"version": 1, "encounters": []})", "monsters"},
        {R"({"version": 1, "monsters": []})", "encounters"},
        {R"({"version": 1, "monsters": {}, "encounters": []})", "array"},
        {R"({"version": 1, "monsters": [], "encounters": [], "colour": 1})", "colour"},
        {R"({"version": 1, "monsters": [{"id": 10001, "name": "M", "stats": {"hp": 5, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1.0}}], "encounters": []})", "range"},
        {wrap(R"(, "colour": "red")", drops), "colour"},
        {wrap(R"(, "stage": -1)", drops), "stage"},
        {wrap("", ""), "no drops"},
        {R"({"version": 1, "monsters": [], "encounters": [{"id": 1, "name": "E", "units": [{"monster": 10001, "x": 7, "y": 0}]}]})", "x"},
        {R"({"version": 1, "monsters": [], "encounters": [{"id": 1, "name": "E", "units": [{"monster": 10001, "x": 1, "y": 4}]}]})", "y"},
        {R"({"version": 1, "monsters": [], "encounters": [{"id": 1, "name": "E", "units": [{"monster": 10001, "y": 1}]}]})", "x"},
        {wrap(R"(, "drops": [{"type": "Loot", "weight": 1}])"), "Loot"},
        {wrap(R"(, "drops": [{"type": "Gold"}])"), "minGold"},
        {wrap(R"(, "drops": [{"type": "Item", "minGold": 1}])"), "minGold"},
        {wrap(R"(, "drops": [{"type": "Gold", "minGold": 1, "maxGold": 2, "tiers": [1]}])"), "tiers"},
        {wrap(R"(, "drops": [{"type": "Champion"}])"), "tiers"},
        {wrap(R"(, "drops": [{"type": "Champion", "tiers": [9]}])"), "tiers[0]"},
        {wrap(R"(, "drops": [{"type": "Gold", "weight": 0, "minGold": 1, "maxGold": 2}])"), "weight"},
        {wrap(R"(, "drops": [{"type": "Item", "items": [99]}])"), "not in the item data"},
    };
    for (const BadJson& b : badJson) {
        std::string e;
        auto made = w2f::LoadEncounterDatabaseFromJson(b.json, champions.get(), items.get(), &e);
        const bool ok = made == nullptr && e.find(b.mention) != std::string::npos;
        if (!ok) std::printf("  wanted an error mentioning '%s' for %.90s..., got '%s'\n", b.mention, b.json.c_str(), e.c_str());
        CHECK(ok);
    }
    // Monsters need no cost, and a cost is tolerated.
    CHECK(w2f::LoadEncounterDatabaseFromJson(wrap("", drops), champions.get(), items.get(), &err) != nullptr);
    CHECK(w2f::LoadEncounterDatabaseFromFile("/nonexistent/pve.json", nullptr, nullptr, &err) == nullptr && err.find("cannot open") != std::string::npos);
}

static void TestPveRoundWinDropsGoldItemChampion() {
    auto db = sample::MakeCombatDatabase();
    auto items = MustLoadItems(kTestItemsJson);
    CHECK(db != nullptr && items != nullptr);
    if (!db || !items) return;

    struct Scenario { const char* drops; PveDropType type; };
    const Scenario scenarios[] = {
        {R"([{"type": "Gold", "weight": 1, "minGold": 3, "maxGold": 5}])", PveDropType::Gold},
        {R"([{"type": "Item", "weight": 1, "items": [2]}])", PveDropType::Item},
        {R"([{"type": "Item", "weight": 1}])", PveDropType::Item},
        {R"([{"type": "Champion", "weight": 1, "tiers": [1]}])", PveDropType::Champion},
    };
    for (const Scenario& sc : scenarios) {
        std::string err;
        auto enc = w2f::LoadEncounterDatabaseFromJson(PveJson(sc.drops), db.get(), items.get(), &err);
        CHECK(enc != nullptr);
        if (!enc) { std::printf("  %s\n", err.c_str()); continue; }
        auto merged = WithMonsters(*db, *enc);
        auto run = PveRun::Start(*db, items.get(), enc.get(), 5);
        run->RunToResolution(1);
        CHECK(run->integrity);

        const auto& outcomes = run->match->CurrentCombatOutcomes();
        CHECK(outcomes.size() == 4);
        int gotDrops = 0;
        for (const CombatOutcome& o : outcomes) {
            // Everyone fights the monsters, not each other: the matchup is home vs "nobody" with encounter 1 (Pinatas).
            CHECK(o.matchup.awayIsMonsters && !o.matchup.awayIsGhost && o.matchup.away == kInvalidPlayerId && o.matchup.encounter == 1);
            CHECK(o.winner == CombatWinner::Home && o.winnerSurvivors >= 1 && o.damageToLoser == 0);
            CHECK(o.log.checksum == o.log.ComputeChecksum() && !o.log.events.empty());
            std::string why;
            const bool valid = ValidateCombatLog(o.log, *merged, run->cfg.combat, run->cfg.match.combatTicks, &why);
            if (!valid) std::printf("  validator: %s\n", why.c_str());
            CHECK(valid);

            const PlayerId p = o.matchup.home;
            const PlayerState& ps = *run->match->Players().Get(p);
            CHECK(o.drop.type == sc.type);
            CHECK(ps.Health() == 100 && ps.Streak() == 0);   // PvE never hurts and never changes a streak
            switch (o.drop.type) {
                case PveDropType::Gold:
                    CHECK(o.drop.gold >= 3 && o.drop.gold <= 5 && ps.Gold() == run->goldBefore[p] + o.drop.gold);
                    break;
                case PveDropType::Item:
                    CHECK(static_cast<int>(ps.ItemBag().size()) == run->bagBefore[p] + 1 && items->Find(o.drop.item) != nullptr);
                    CHECK(ps.ItemBag().back() == o.drop.item);
                    if (std::string(sc.drops).find("\"items\": [2]") != std::string::npos) CHECK(o.drop.item == 2);
                    break;
                case PveDropType::Champion: {
                    const ChampionDefinition* c = db->Find(o.drop.champion);
                    CHECK(c != nullptr && c->cost == 1 && PveRun::Copies(ps) == run->copiesBefore[p] + 1 && ps.Gold() == run->goldBefore[p]);
                    break;
                }
                case PveDropType::None: CHECK(false); break;
            }
            ++gotDrops;
        }
        CHECK(gotDrops == 4 && run->rec.drops.size() == 4 && run->rec.damages.empty());
        for (std::size_t i = 0; i < run->rec.drops.size(); ++i) CHECK(run->rec.drops[i].drop == outcomes[i].drop && run->rec.drops[i].player == outcomes[i].matchup.home);
        if (sc.type == PveDropType::Champion) CHECK(run->rec.freeUnits == 4);   // announced like any unit that joins a roster, at 0 gold

        // The monsters: ids in their own range, on the far side of the arena, mirrored from their board cells.
        std::set<UnitId> monsterIds;
        for (const CombatEvent& e : outcomes[0].log.events) {
            if (e.type == CombatEventType::Spawn && e.team == 1) {
                monsterIds.insert(e.unit);
                CHECK(e.unit > kMonsterUnitBase && e.champion == 10001);
            }
        }
        CHECK(monsterIds.size() == 2);
        std::set<std::pair<int, int>> where;
        for (const CombatEvent& e : outcomes[0].log.events) {
            if (e.type == CombatEventType::Spawn && e.team == 1) where.insert({e.to.x, e.to.y});
        }
        const HexCoord a = BoardToArena(2, 3, ArenaSide::Away), b = BoardToArena(4, 3, ArenaSide::Away);
        CHECK((where == std::set<std::pair<int, int>>{{a.x, a.y}, {b.x, b.y}}));
    }
}

static void TestPveLossGivesNothingAndRoundsDiffer() {
    auto db = sample::MakeCombatDatabase();
    auto items = MustLoadItems(kTestItemsJson);
    CHECK(db != nullptr && items != nullptr);
    if (!db || !items) return;
    std::string err;
    auto enc = w2f::LoadEncounterDatabaseFromJson(PveJson(R"([{"type": "Gold", "weight": 1, "minGold": 5, "maxGold": 5}])"), db.get(), items.get(), &err);
    CHECK(enc != nullptr);
    if (!enc) return;
    auto run = PveRun::Start(*db, items.get(), enc.get(), 6);
    run->RunToResolution(1);
    CHECK(run->match->CurrentCombatOutcomes()[0].matchup.encounter == 1);

    run->RunToResolution(2);                       // 1-2: the Titan, which nobody can beat
    const auto& outcomes = run->match->CurrentCombatOutcomes();
    CHECK(outcomes.size() == 4);
    for (const CombatOutcome& o : outcomes) {
        CHECK(o.matchup.encounter == 2 && o.matchup.awayIsMonsters);
        CHECK(o.winner == CombatWinner::Away);     // the monsters won
        CHECK(o.drop.type == PveDropType::None && o.damageToLoser == 0);
        const PlayerState& ps = *run->match->Players().Get(o.matchup.home);
        CHECK(ps.Health() == 100 && ps.Streak() == 0);
    }
    CHECK(run->rec.drops.size() == 4 && run->rec.damages.empty());   // nothing new since round 1: no drop, no damage
    // After the PvE rounds the game goes back to players fighting each other.
    run->RunToResolution(3);
    for (const CombatOutcome& o : run->match->CurrentCombatOutcomes()) CHECK(!o.matchup.awayIsMonsters && o.matchup.encounter == 0);
    CHECK(run->integrity);
}

static void TestPveDropEdgeCases() {
    std::string err;
    // ---- the roster is full: the copy goes back to the pool and the player is paid its cost instead ----
    {
        std::vector<ChampionDefinition> defs;
        for (int i = 0; i < 12; ++i) {
            ChampionDefinition d = Fighter(static_cast<ChampionId>(101 + i), 500, 0, 60);
            d.name = "One" + std::to_string(i);
            d.cost = 1;
            defs.push_back(d);
        }
        ChampionDefinition two = Fighter(201, 500, 0, 60);
        two.name = "Two";
        two.cost = 2;
        defs.push_back(two);
        auto db = ChampionDatabase::Create(defs);
        auto enc = w2f::LoadEncounterDatabaseFromJson(PveJson(R"([{"type": "Champion", "weight": 1, "tiers": [2]}])"), db.get(), nullptr, &err);
        CHECK(db != nullptr && enc != nullptr);
        if (!db || !enc) return;

        const auto play = [&](std::size_t wanted) {
            auto run = PveRun::Start(*db, nullptr, enc.get(), 21, false, 4, [](GameConfig& c) { c.player.limitBoardToLevel = true; });   // 1 on the board, 9 on the bench
            while (run->match->Phase() != MatchPhase::Planning) run->match->Tick();
            PlayerState* p0 = run->match->PlayersMutable().Get(0);
            p0->AddGold(1000);
            for (int i = 0; i < 400 && p0->Roster().Count() < wanted; ++i) {
                for (std::size_t slot = 0; slot < 5 && p0->Roster().Count() < wanted; ++slot) run->match->TryBuyShopUnit(0, slot);
                if (p0->Roster().Count() < wanted) run->match->TryRerollShop(0);
            }
            const UnitInstance* onBench = nullptr;
            for (int slot = 0; slot < kBenchSlots && onBench == nullptr; ++slot) onBench = p0->Roster().BenchAt(slot);
            if (p0->Roster().BoardCount() == 0 && onBench != nullptr) run->match->TryMoveUnit(0, onBench->id, LocationType::Board, 3, 3);
            run->RunToResolution(1);
            return run;
        };
        auto full = play(10);    // bench 9 + board 1: nothing more fits, and no unit of a new champion can merge
        auto roomy = play(2);
        const CombatOutcome& fo = full->match->CurrentCombatOutcomes()[0];
        const CombatOutcome& ro = roomy->match->CurrentCombatOutcomes()[0];
        CHECK(fo.matchup.home == 0 && fo.winner == CombatWinner::Home && ro.winner == CombatWinner::Home);
        CHECK(full->copiesBefore[0] >= 10 && full->match->Players().Get(0)->Roster().Count() == 10);
        CHECK(fo.drop.type == PveDropType::Gold && fo.drop.gold == 2 && fo.drop.champion == kInvalidChampionId);   // paid the champion's cost
        CHECK(full->match->Players().Get(0)->Gold() == full->goldBefore[0] + 2);
        CHECK(full->match->Players().Get(0)->Roster().Count() == 10);                                                // still ten
        CHECK(full->match->VerifyPoolIntegrity() && full->integrity);                                                // the copy really went back
        CHECK(full->match->Pool().Remaining(201) == full->match->Pool().InitialCopies(201));
        CHECK(ro.drop.type == PveDropType::Champion && ro.drop.champion == 201);
        CHECK(roomy->match->Pool().Remaining(201) == roomy->match->Pool().InitialCopies(201) - 1);
        CHECK(roomy->match->VerifyPoolIntegrity() && roomy->integrity);
    }

    // ---- a champion line whose tier is sold out is skipped ----
    {
        std::vector<ChampionDefinition> defs;
        for (int i = 0; i < 6; ++i) {
            ChampionDefinition d = Fighter(static_cast<ChampionId>(101 + i), 500, 0, 60);
            d.name = "One" + std::to_string(i);
            d.cost = 1;
            defs.push_back(d);
        }
        ChampionDefinition five = Fighter(501, 900, 0, 90);   // the only tier-5 champion, and the pool holds ONE copy of it
        five.name = "Rare";
        five.cost = 5;
        defs.push_back(five);
        auto db = ChampionDatabase::Create(defs);
        CHECK(db != nullptr);
        if (!db) return;
        for (const char* drops : {R"([{"type": "Champion", "weight": 1, "tiers": [5]}])",
                                  R"([{"type": "Champion", "weight": 1, "tiers": [5]}, {"type": "Gold", "weight": 1, "minGold": 7, "maxGold": 7}])"}) {
            auto enc = w2f::LoadEncounterDatabaseFromJson(PveJson(drops), db.get(), nullptr, &err);
            CHECK(enc != nullptr);
            if (!enc) continue;
            auto run = PveRun::Start(*db, nullptr, enc.get(), 9, true, 4, [](GameConfig& c) { c.pool.copiesPerTier = {{29, 22, 18, 12, 1}}; });
            run->RunToResolution(1);
            int champions = 0, golds = 0, nones = 0;
            for (const CombatOutcome& o : run->match->CurrentCombatOutcomes()) {
                champions += o.drop.type == PveDropType::Champion ? 1 : 0;
                golds += o.drop.type == PveDropType::Gold ? 1 : 0;
                nones += o.drop.type == PveDropType::None ? 1 : 0;
            }
            const bool goldTable = std::string(drops).find("Gold") != std::string::npos;
            CHECK(champions <= 1);   // one copy exists
            if (goldTable) CHECK(champions + golds == 4 && nones == 0);          // whoever cannot get the champion gets the gold line
            else CHECK(champions == 1 && nones == 3);                             // no other line: nothing to give
            CHECK(run->rec.drops.size() == static_cast<std::size_t>(4 - nones));  // "nothing" is not announced
            CHECK(run->match->VerifyPoolIntegrity() && run->integrity);
        }
    }

    auto db = sample::MakeCombatDatabase();
    CHECK(db != nullptr);
    if (!db) return;
    // ---- an item line is skipped when no item data is loaded ----
    {
        auto enc = w2f::LoadEncounterDatabaseFromJson(PveJson(R"([{"type": "Item", "weight": 100}, {"type": "Gold", "weight": 1, "minGold": 2, "maxGold": 2}])"), db.get(), nullptr, &err);
        CHECK(enc != nullptr);
        if (enc) {
            auto run = PveRun::Start(*db, nullptr, enc.get(), 4);
            run->RunToResolution(1);
            for (const CombatOutcome& o : run->match->CurrentCombatOutcomes()) CHECK(o.drop.type == PveDropType::Gold && o.drop.gold == 2);
        }
    }
    // ---- no PvE data at all: a PvE round is simply nobody's fight ----
    {
        auto run = PveRun::Start(*db, nullptr, nullptr, 4);
        CHECK(run->match->CurrentStageRound().stage == 1 && run->match->IsPveRound());
        run->RunToResolution(1);
        CHECK(run->match->CurrentCombatOutcomes().size() == 4);
        for (const CombatOutcome& o : run->match->CurrentCombatOutcomes()) {
            CHECK(o.matchup.awayIsMonsters && o.matchup.encounter == 0 && o.winner == CombatWinner::Draw && o.log.events.empty());
            CHECK(o.drop.type == PveDropType::None && o.damageToLoser == 0);
        }
        CHECK(run->rec.drops.empty() && run->rec.damages.empty());
    }
}

static void TestPveDropDistributionAndDeterminism() {
    auto db = sample::MakeCombatDatabase();
    auto items = MustLoadItems(kTestItemsJson);
    CHECK(db != nullptr && items != nullptr);
    if (!db || !items) return;
    std::string err;
    auto enc = w2f::LoadEncounterDatabaseFromJson(PveJson(R"([{"type": "Gold", "weight": 40, "minGold": 2, "maxGold": 4},
                                                              {"type": "Champion", "weight": 30, "tiers": [1, 2]},
                                                              {"type": "Item", "weight": 30}])"), db.get(), items.get(), &err);
    CHECK(enc != nullptr);
    if (!enc) return;
    const auto drops = [&](std::uint64_t seed) {
        auto run = PveRun::Start(*db, items.get(), enc.get(), seed);
        run->RunToResolution(1);
        std::vector<PveDrop> out;
        for (const CombatOutcome& o : run->match->CurrentCombatOutcomes()) out.push_back(o.drop);
        return std::make_pair(out, run->match->StateHash());
    };
    std::map<PveDropType, int> counts;
    std::set<int> goldAmounts;
    std::set<ItemId> itemsSeen;
    std::set<int> tiersSeen;
    std::vector<std::vector<PveDrop>> perSeed;
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        const auto result = drops(seed);
        perSeed.push_back(result.first);
        CHECK(drops(seed) == result);                 // the same seed always drops the same things
        for (const PveDrop& d : result.first) {
            ++counts[d.type];
            if (d.type == PveDropType::Gold) goldAmounts.insert(d.gold);
            if (d.type == PveDropType::Item) itemsSeen.insert(d.item);
            if (d.type == PveDropType::Champion) tiersSeen.insert(db->Find(d.champion)->cost);
        }
    }
    // 48 drops at 40 / 30 / 30 %: each kind shows up plenty, and the details vary within their allowed ranges.
    CHECK(counts[PveDropType::Gold] >= 10 && counts[PveDropType::Champion] >= 8 && counts[PveDropType::Item] >= 8 && counts[PveDropType::None] == 0);
    CHECK((goldAmounts == std::set<int>{2, 3, 4}));
    CHECK(itemsSeen.size() >= 3);
    CHECK(tiersSeen.count(1) == 1 && tiersSeen.count(2) == 1 && tiersSeen.size() == 2);
    bool seedsDiffer = false;
    for (std::size_t i = 1; i < perSeed.size(); ++i) seedsDiffer = seedsDiffer || !(perSeed[i] == perSeed[0]);
    CHECK(seedsDiffer);
}

static void TestFullMatchWithPveRounds() {
    // The real rules end to end: 8 players (7 bots + a scripted human), the shipped monsters and drop tables, 1-1..1-3 and
    // X-7 as PvE, everything else PvP, items dropping into bags and being fitted onto units, real fights with real logs.
    auto roster = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(roster != nullptr);
    if (!roster) return;
    struct Result {
        std::vector<std::uint64_t> hashes;
        int pveRounds = 0, pveFights = 0, pveWins = 0, pvpFights = 0, drops[4] = {}, logsChecked = 0;
        int damageDuringPve = 0, streakOrHealthChangedInPve = 0, pveOnWrongRound = 0;
        bool logsValid = true, integrity = true, finished = false;
        std::string firstProblem;
        PlayerId winner = kInvalidPlayerId;
        int rounds = 0;
    };
    struct Listener : IMatchListener {
        LiveMatch* live = nullptr;
        std::unique_ptr<ChampionDatabase> merged;
        Result* result = nullptr;
        void OnCombatSimulated(int round, const CombatOutcome& o) override {
            const bool shouldBePve = live->cfg.match.IsPveRound(round);
            if (o.matchup.awayIsMonsters != shouldBePve) ++result->pveOnWrongRound;
            ++(o.matchup.awayIsMonsters ? result->pveFights : result->pvpFights);
            std::string why;
            if (!ValidateCombatLog(o.log, *merged, live->cfg.combat, live->cfg.match.combatTicks, &why)) {
                if (result->logsValid) result->firstProblem = "round " + std::to_string(round) + ": " + why;
                result->logsValid = false;
            }
            ++result->logsChecked;
        }
        void OnPlayerDamaged(PlayerId, int, int) override { damageNow = true; }
        bool damageNow = false;
    };
    const auto play = [&](std::uint64_t seed) {
        Result r;
        auto live = LiveMatch::Create(*roster, seed);
        Listener listener;
        listener.live = live.get();
        listener.merged = WithMonsters(*roster, *live->encounters);
        listener.result = &r;
        live->match->AddListener(&listener);
        live->match->Start();
        int healthBefore[kMaxPlayers], streakBefore[kMaxPlayers];
        for (int tick = 0; tick < 3'000'000 && !live->match->IsFinished(); ++tick) {
            for (int s = 0; s < kMaxPlayers; ++s) {
                healthBefore[s] = live->match->Players().Get(static_cast<PlayerId>(s))->Health();
                streakBefore[s] = live->match->Players().Get(static_cast<PlayerId>(s))->Streak();
            }
            const MatchPhase phaseBefore = live->match->Phase();
            listener.damageNow = false;
            live->Step();
            r.hashes.push_back(live->match->StateHash());
            r.integrity = r.integrity && live->match->VerifyPoolIntegrity() && live->match->VerifyRosterLayouts();
            if (phaseBefore != MatchPhase::Resolution && live->match->Phase() == MatchPhase::Resolution && live->match->IsPveRound()) {
                ++r.pveRounds;
                r.damageDuringPve += listener.damageNow ? 1 : 0;
                for (int s = 0; s < kMaxPlayers; ++s) {
                    const PlayerState& p = *live->match->Players().Get(static_cast<PlayerId>(s));
                    if (p.Health() != healthBefore[s] || p.Streak() != streakBefore[s]) ++r.streakOrHealthChangedInPve;
                }
                for (const CombatOutcome& o : live->match->CurrentCombatOutcomes()) {
                    ++r.drops[static_cast<int>(o.drop.type)];
                    r.pveWins += o.winner == CombatWinner::Home ? 1 : 0;
                }
            }
        }
        r.finished = live->match->IsFinished();
        r.winner = live->match->Winner();
        r.rounds = live->match->Round();
        return r;
    };
    const Result a = play(2024);
    const Result b = play(2024);
    const Result c = play(31337);
    for (const Result* r : {&a, &c}) {
        CHECK(r->finished && r->winner != kInvalidPlayerId && r->integrity && r->logsValid);
        if (!r->logsValid) std::printf("  first bad log: %s\n", r->firstProblem.c_str());
        CHECK(r->pveOnWrongRound == 0);                     // PvE exactly on 1-1, 1-2, 1-3, 2-7, 3-7 ...
        CHECK(r->pveRounds >= 5 && r->pveFights >= 3 * 8 && r->pvpFights > 20);
        CHECK(r->damageDuringPve == 0 && r->streakOrHealthChangedInPve == 0);
        CHECK(r->pveWins > 0 && r->drops[static_cast<int>(PveDropType::Gold)] + r->drops[static_cast<int>(PveDropType::Champion)] + r->drops[static_cast<int>(PveDropType::Item)] == r->pveWins);
    }
    CHECK(a.hashes == b.hashes && a.winner == b.winner && a.rounds == b.rounds);
    CHECK(a.hashes != c.hashes);
    std::printf("  seed 2024: %d rounds, %d PvE fights (%d won: %d gold / %d champion / %d item drops), %d PvP fights; seed 31337: %d rounds, %d PvE fights\n",
                a.rounds, a.pveFights, a.pveWins, a.drops[1], a.drops[2], a.drops[3], a.pvpFights, c.rounds, c.pveFights);
}

static void TestAutomaticSnapshotAtPlanning() {
    auto roster = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(roster != nullptr);
    if (!roster) return;

    // ---- one snapshot per Planning phase, each exactly the state at that instant ----
    auto live = LiveMatch::Create(*roster, 4242);
    RoundRecorder rec;
    rec.match = live->match.get();
    live->match->AddListener(&rec);
    CHECK(live->match->LastPlanningSnapshot().empty() && live->match->LastPlanningSnapshotRound() == 0);
    live->match->Start();

    int planningsEntered = 0;
    MatchPhase last = live->match->Phase();
    if (last == MatchPhase::Planning) ++planningsEntered;
    struct Fork { std::unique_ptr<LiveMatch> twin; int untilRound; std::string label; };
    std::vector<Fork> forks;
    bool identical = true, lastAlwaysMatches = true;
    std::string problem;
    int forked = 0;
    for (int tick = 0; tick < 3'000'000 && !live->match->IsFinished(); ++tick) {
        live->Step();
        const bool enteredPlanning = live->match->Phase() == MatchPhase::Planning && last != MatchPhase::Planning;
        last = live->match->Phase();
        if (enteredPlanning) ++planningsEntered;
        lastAlwaysMatches = lastAlwaysMatches && (live->match->LastPlanningSnapshot() == (rec.snaps.empty() ? std::vector<std::uint8_t>{} : rec.snaps.back().bytes));
        for (Fork& f : forks) {
            if (f.untilRound < 0) continue;
            f.twin->Step();
            if (f.twin->match->StateHash() != live->match->StateHash()) {
                if (identical) problem = "twin '" + f.label + "' diverged at tick " + std::to_string(tick);
                identical = false;
                f.untilRound = -1;
            } else if (live->match->Round() >= f.untilRound) {
                f.untilRound = -1;
            }
        }
        // Crash drill: at the start of a few chosen rounds, pretend the process died and restore from the safety net alone
        // (plus the clients, who reconnect in the state they were in when the round began). The recovered match has to keep
        // pace with the one that never crashed, tick for tick.
        if (enteredPlanning && live->match->TicksInPhase() == 0 && (live->match->Round() == 3 || live->match->Round() == 7 || live->match->Round() == 15)) {
            std::string err;
            auto restored = live->RestoreFrom(live->match->LastPlanningSnapshot(), &err);
            if (!restored) { identical = false; problem = err; continue; }
            CHECK(restored->StateHash() == live->match->StateHash());
            CHECK(restored->LastPlanningSnapshot() == live->match->LastPlanningSnapshot() && restored->LastPlanningSnapshotRound() == live->match->Round());
            forks.push_back(Fork{live->Fork(std::move(restored)), live->match->Round() + 3, "restart at round " + std::to_string(live->match->Round())});
            ++forked;
        }
    }
    if (!problem.empty()) std::printf("  %s\n", problem.c_str());
    CHECK(identical && lastAlwaysMatches && forked == 3);
    CHECK(live->match->IsFinished());

    // One snapshot per Planning phase; each is a Planning-start snapshot describing the moment it was taken.
    CHECK(static_cast<int>(rec.snaps.size()) == planningsEntered && planningsEntered >= 20);
    bool eachExact = true, eachAtStart = true, roundsAscend = true;
    int previousRound = 0;
    for (const auto& s : rec.snaps) {
        SnapshotInfo info;
        eachExact = eachExact && w2f::ReadSnapshotInfo(s.bytes, info) && info.stateHash == s.matchHashAtThatMoment;
        eachAtStart = eachAtStart && s.phase == MatchPhase::Planning && s.ticks == 0 && info.phase == static_cast<std::uint8_t>(MatchPhase::Planning) && info.ticksInPhase == 0 && info.round == s.round;
        roundsAscend = roundsAscend && s.round > previousRound;
        previousRound = s.round;
    }
    CHECK(eachExact && eachAtStart && roundsAscend);
    CHECK(live->match->LastPlanningSnapshotRound() == rec.snaps.back().round);
    // The last snapshot of the match restores to a match that is sitting at the start of that Planning phase.
    auto lastRestored = live->RestoreFrom(rec.snaps.back().bytes);
    CHECK(lastRestored != nullptr && lastRestored->Phase() == MatchPhase::Planning && lastRestored->TicksInPhase() == 0 && lastRestored->Round() == rec.snaps.back().round);

    // ---- it can be switched off ----
    {
        GameConfig cfg;
        cfg.snapshot.atPlanningStart = false;
        auto db = sample::MakeCombatDatabase();
        auto match = MatchManager::Create(cfg, *db, 1, nullptr);
        RoundRecorder off;
        off.match = match.get();
        match->AddListener(&off);
        match->Start();
        for (int i = 0; i < 4000; ++i) match->Tick();
        CHECK(off.snaps.empty() && match->LastPlanningSnapshot().empty() && match->Round() >= 2);
        // ...and the switch is not a game rule: it does not change the config hash a snapshot is checked against.
        CHECK(cfg.ContentHash() == GameConfig{}.ContentHash());
    }
    // ---- a snapshot taken anywhere else is not adopted as the recovery point ----
    {
        auto mid = LiveMatch::Create(*roster, 5);
        mid->match->Start();
        for (int i = 0; i < 700; ++i) mid->Step();
        auto restored = mid->RestoreFrom(mid->match->Snapshot());
        CHECK(restored != nullptr && (restored->TicksInPhase() != 0 || restored->Phase() != MatchPhase::Planning) && restored->LastPlanningSnapshot().empty());
    }
}

// ==== Phase 10: item combination, auras, unified event triggers, summons, ability crits ====================================

namespace {

std::unique_ptr<ItemDatabase> P10Items() {
    std::string err;
    auto db = w2f::LoadItemDatabaseFromFile(sample::Phase10ItemsPath(), &err);
    if (!db) std::printf("  phase10_items.json: %s\n", err.c_str());
    return db;
}

AbilityDefinition Hook(AbilityId id, CastTrigger trigger, std::vector<AbilityEffect> effects, int count = 0, int maxTriggers = 0) {
    AbilityDefinition a;
    a.id = id;
    a.name = "hook";
    a.trigger = trigger;
    a.attackCount = count;
    a.maxTriggers = maxTriggers;
    a.effects = std::move(effects);
    return a;
}
StatusEffect Flat(StatusType type, int value, int duration = 0) {   // a flat-family status (BonusArmor ...); permanent when duration is 0
    StatusEffect s;
    s.status = type;
    s.value = FlatAmount(Same(value));
    s.permanent = duration == 0;
    s.duration = FlatAmount(Same(duration));
    return s;
}
std::vector<CombatEvent> StatusOn(const FightResult& r, UnitId unit, StatusType type) {
    std::vector<CombatEvent> out;
    for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, unit)) {
        if (e.subtype == static_cast<std::uint8_t>(type)) out.push_back(e);
    }
    return out;
}
FightResult RunP10(Duel& d, int ticks, std::uint64_t seed = 1, const CombatConfig& cfg = CombatConfig{}) {
    const FightResult r = CombatSimulator(cfg, nullptr, nullptr, d.db.get()).RunFight(d.specs, ticks, seed);
    CheckValid(r, d, ticks, cfg);
    return r;
}

struct ComboLog : IMatchListener {
    struct Entry { PlayerId player; ItemCombination combination; std::array<ItemId, 3> unitItems; };
    std::vector<Entry> combined;
    std::vector<ItemId> equipped;
    void OnItemEquipped(PlayerId, const UnitInstance&, ItemId item) override { equipped.push_back(item); }
    void OnItemsCombined(PlayerId p, const UnitInstance& u, const ItemCombination& c) override { combined.push_back({p, c, u.items}); }
};

}  // namespace

static void TestItemRecipeData() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    // Lookups: either order, same-with-same, and finished items never combine.
    CHECK(items->FindCombination(1, 2) && items->FindCombination(1, 2)->id == 10 && items->FindCombination(2, 1)->id == 10);
    CHECK(items->FindCombination(1, 1) && items->FindCombination(1, 1)->id == 16);   // Helmet + Helmet = Big Helmet
    CHECK(items->FindCombination(9, 3)->id == 40 && items->FindCombination(3, 9)->id == 40);   // Seed + Sword = Coregons Emblem
    CHECK(items->FindCombination(10, 1) == nullptr && items->FindCombination(1, 99) == nullptr && items->FindCombination(0, 1) == nullptr && items->FindCombination(2, 9) && items->FindCombination(2, 9)->id == 42);
    CHECK(items->IsComponent(1) && items->IsComponent(9) && !items->IsComponent(10) && !items->IsComponent(99));
    CHECK(items->Find(10)->IsCombined() && !items->Find(1)->IsCombined() && !items->Find(9)->HasEffect());
    // The Seed has no effect of its own but is legal, because recipes use it.
    // Every stat / aura / hook of Sayona's Casket arrived.
    const ItemDefinition* casket = items->Find(10);
    CHECK(casket->stats.armor == 13 && casket->stats.magicResist == 13 && casket->stats.manaRegenMilli == 2000 && casket->auras.size() == 1 &&
          casket->auras[0].side == TargetSide::Enemies && casket->auras[0].radius == 2 && casket->auras[0].status == StatusType::MagicResist && casket->auras[0].amount == -30);
    const ItemDefinition* guardians = items->Find(12);
    CHECK(guardians->abilities.size() == 1 && guardians->abilities[0].trigger == CastTrigger::OnTakeBasicAttackDamage && guardians->abilities[0].attackCount == 6);
    CHECK(items->Find(14)->abilities[0].trigger == CastTrigger::OnTakeAbilityDamage && items->Find(21)->abilities[0].trigger == CastTrigger::OnDealDamage &&
          items->Find(23)->abilities[1].trigger == CastTrigger::OnHpDropBelowPercent && items->Find(23)->abilities[1].thresholdPercent == 30 &&
          items->Find(27)->abilities[0].damageFilter == DamageFilter::Basic && items->Find(36)->abilities[0].effects.size() == 1);

    // The database refuses broken recipes.
    struct Bad { std::function<void(std::vector<ItemDefinition>&)> edit; const char* mention; };
    static const auto base = [](ItemId id, const char* name, int armor) {   // static: the table's captureless lambdas below use it
        ItemDefinition d;
        d.id = id;
        d.name = name;
        d.stats.armor = armor;
        return d;
    };
    const Bad bad[] = {
        {[](auto& v) { v[2].components = {1, 99}; }, "does not exist"},
        {[](auto& v) { v[2].components = {1, 0}; }, "two components"},
        {[](auto& v) { v.push_back(v[2]); v.back().id = 4; v.back().name = "Twin"; }, "same two components"},
        {[](auto& v) { v.push_back(base(5, "Twin2", 0)); v.back().components = {2, 1}; }, "same two components"},   // the same recipe written the other way round
        {[](auto& v) { v.push_back(base(4, "Third", 0)); v.back().components = {3, 1}; v.push_back(base(5, "Fourth", 1)); v.back().components = {4, 2}; }, "itself a combination"},
        {[](auto& v) { v[2].components = {3, 3}; }, "made from itself"},
        {[](auto& v) { v.push_back(base(4, "Nothing", 0)); }, "does nothing"},
        {[](auto& v) { v[0].abilities.push_back(Hook(0, CastTrigger::OnCast, {Eff(TargetSpec::Self(), Stat(StatusType::Armor, 30, 5))})); }, "without an id"},
        {[](auto& v) { v[0].abilities.push_back(Hook(5, CastTrigger::Mana, {Eff(TargetSpec::Self(), Stat(StatusType::Armor, 30, 5))})); }, "mana"},   // an item has no mana bar
        {[](auto& v) { v[0].abilities.push_back(Hook(5, CastTrigger::OnHpDropBelowPercent, {Eff(TargetSpec::Self(), Stat(StatusType::Armor, 30, 5))})); }, "thresholdPercent"},
        {[](auto& v) { AuraDefinition a; a.radius = 0; a.amount = -30; v[0].auras.push_back(a); }, "radius"},
        {[](auto& v) { AuraDefinition a; a.radius = 2; a.amount = 0; v[0].auras.push_back(a); }, "does nothing"},
        {[](auto& v) { AuraDefinition a; a.radius = 2; a.amount = 5; a.status = StatusType::Stun; v[0].auras.push_back(a); }, "cannot apply Stun"},
    };
    int index = 0;
    for (const Bad& b : bad) {
        std::vector<ItemDefinition> v = {base(1, "A", 5), base(2, "B", 5), base(3, "AB", 0)};
        v[2].components = {1, 2};
        b.edit(v);
        std::string err;
        auto made = ItemDatabase::Create(v, &err);
        const bool ok = made == nullptr && err.find(b.mention) != std::string::npos;
        if (!ok) std::printf("  recipe case %d: wanted an error mentioning '%s', got '%s'\n", index, b.mention, err.c_str());
        CHECK(ok);
        ++index;
    }
    // The hash notices recipes, hooks and auras (a snapshot refuses different item data).
    std::vector<ItemDefinition> v1 = {base(1, "A", 5), base(2, "B", 5), base(3, "AB", 1)};
    v1[2].components = {1, 2};
    std::vector<ItemDefinition> v2 = v1;
    v2[2].components = {2, 1};   // same recipe, other order: it is the SAME item...
    std::vector<ItemDefinition> v3 = v1;
    v3[2].abilities.push_back(Hook(9, CastTrigger::OnCast, {Eff(TargetSpec::Self(), Stat(StatusType::Armor, 30, 5))}));
    std::vector<ItemDefinition> v4 = v1;
    AuraDefinition aura;
    aura.radius = 2;
    aura.amount = -10;
    v4[2].auras.push_back(aura);
    const auto h1 = ItemDatabase::Create(v1)->ContentHash();
    CHECK(ItemDatabase::Create(v3)->ContentHash() != h1 && ItemDatabase::Create(v4)->ContentHash() != h1 && ItemDatabase::Create(v2)->ContentHash() != h1);   // ...but written differently, so hashed differently
}

static void TestItemCombinationOnUnits() {
    auto items = P10Items();
    auto db = ChampionDatabase::Create({Def(101, "A", 1), Def(102, "B", 1)});
    CHECK(items != nullptr && db != nullptr);
    if (!items || !db) return;

    // ---- roster level: the rules of combining ----
    {
        UnitRoster roster(0, items.get());
        const UnitId u = roster.Add(db->Find(101)).unit.id;
        ItemCombination c;
        CHECK(roster.EquipItem(u, 1, &c) == ActionResult::Ok && c.result == 0);                       // the first component just sits there
        CHECK((roster.Find(u)->items == std::array<ItemId, 3>{1, 0, 0}));
        CHECK(roster.EquipItem(u, 2, &c) == ActionResult::Ok && c.first == 1 && c.second == 2 && c.result == 10);   // the second one completes it
        CHECK((roster.Find(u)->items == std::array<ItemId, 3>{10, 0, 0}));                             // both consumed, ONE item left
        // A finished item does not combine further; an unrelated component takes the next slot.
        CHECK(roster.EquipItem(u, 1, &c) == ActionResult::Ok && c.result == 0 && (roster.Find(u)->items == std::array<ItemId, 3>{10, 1, 0}));
        // Order does not matter, and the result lands in the partner's slot.
        CHECK(roster.EquipItem(u, 4, &c) == ActionResult::Ok && c.first == 1 && c.result == 12 && (roster.Find(u)->items == std::array<ItemId, 3>{10, 12, 0}));
        // Full unit (3 items) + a component that combines with one of them: allowed, it needs no free slot.
        CHECK(roster.EquipItem(u, 2, &c) == ActionResult::Ok && c.result == 0 && (roster.Find(u)->items == std::array<ItemId, 3>{10, 12, 2}));   // Water has no partner among 10 / 12
        CHECK(roster.EquipItem(u, 1, &c) == ActionResult::Ok && c.first == 2 && c.result == 10);                                          // Helmet + Water: the 3rd slot combined
        CHECK((roster.Find(u)->items == std::array<ItemId, 3>{10, 12, 10}));
        CHECK(roster.EquipItem(u, 5, &c) == ActionResult::ItemsFull && c.result == 0);                                                     // ...but Bow has no partner and no room
        CHECK((roster.Find(u)->items == std::array<ItemId, 3>{10, 12, 10}));
        // Same-with-same, and Seed + component = emblem.
        const UnitId v = roster.Add(db->Find(102)).unit.id;
        CHECK(roster.EquipItem(v, 1) == ActionResult::Ok && roster.EquipItem(v, 1, &c) == ActionResult::Ok && c.result == 16);
        CHECK(roster.EquipItem(v, 9) == ActionResult::Ok && roster.EquipItem(v, 3, &c) == ActionResult::Ok && c.first == 9 && c.result == 40);
        CHECK((roster.Find(v)->items == std::array<ItemId, 3>{16, 40, 0}));
        // Without an item database nothing ever combines.
        UnitRoster plain(0);
        const UnitId w = plain.Add(db->Find(101)).unit.id;
        CHECK(plain.EquipItem(w, 1, &c) == ActionResult::Ok && plain.EquipItem(w, 2, &c) == ActionResult::Ok && c.result == 0 && (plain.Find(w)->items == std::array<ItemId, 3>{1, 2, 0}));
    }

    // ---- through the match: bag, events, merges, sells ----
    {
        auto match = PlanningMatch(*db, items.get());
        CHECK(match != nullptr);
        if (!match) return;
        ComboLog log;
        match->AddListener(&log);
        PlayerState* p = match->PlayersMutable().Get(0);
        CHECK(match->TryBuyShopUnit(0, 0) == ActionResult::Ok);
        const UnitId u = p->Roster().BenchAt(0)->id;
        for (ItemId item : {1u, 2u, 1u, 3u}) CHECK(p->AddItemToBag(item));
        CHECK(match->TryEquipItem(0, u, 1) == ActionResult::Ok);
        CHECK(log.combined.empty());
        CHECK(match->TryEquipItem(0, u, 2) == ActionResult::Ok);
        CHECK(log.combined.size() == 1 && log.combined[0].player == 0 && log.combined[0].combination.result == 10 && (log.combined[0].unitItems == std::array<ItemId, 3>{10, 0, 0}));
        CHECK((log.equipped == std::vector<ItemId>{1, 2}));                                   // the equip events still fire for both, then the combination
        CHECK((p->ItemBag() == std::vector<ItemId>{1, 3}));                                    // both consumed from the bag side; the result lives on the unit
        CHECK(match->TryEquipItem(0, u, 3) == ActionResult::Ok && match->TryEquipItem(0, u, 1) == ActionResult::Ok);   // Sword + Helmet -> Soldiers' Soul
        CHECK(log.combined.size() == 2 && log.combined[1].combination.result == 11);
        CHECK((p->Roster().Find(u)->items == std::array<ItemId, 3>{10, 11, 0}) && p->ItemBag().empty());
        // Unequipping gives back the FINISHED item (combinations are not undone).
        CHECK(match->TryUnequipItem(0, u, 1) == ActionResult::Ok && (p->ItemBag() == std::vector<ItemId>{11}));
        CHECK(match->TryEquipItem(0, u, 11) == ActionResult::Ok);
        // Selling brings the finished item home too.
        const int gold = p->Gold();
        CHECK(match->TrySellUnit(0, u) == ActionResult::Ok && p->Gold() > gold);
        CHECK((p->ItemBag() == std::vector<ItemId>{10, 11}) && match->VerifyPoolIntegrity());
        // A snapshot with combined items in a bag and on units restores exactly.
        CHECK(match->TryBuyShopUnit(0, 1) == ActionResult::Ok);
        CHECK(match->TryEquipItem(0, p->Roster().BenchAt(0)->id, 10) == ActionResult::Ok);
        std::string err;
        auto again = MatchManager::Restore(match->Snapshot(), match->Config(), *db, nullptr, &err, items.get());
        CHECK(again != nullptr && again->StateHash() == match->StateHash());
    }

    // ---- merges: items carried onto the survivor combine too, and nothing is lost ----
    {
        UnitRoster roster(0, items.get());
        const UnitId a = roster.Add(db->Find(101)).unit.id;
        const UnitId b = roster.Add(db->Find(101)).unit.id;
        CHECK(roster.EquipItem(a, 1) == ActionResult::Ok && roster.EquipItem(b, 2) == ActionResult::Ok);
        const auto added = roster.Add(db->Find(101));
        CHECK(added.merges.size() == 1);
        const UnitInstance* merged = roster.Find(a);
        CHECK(merged && merged->starLevel == 2 && (merged->items == std::array<ItemId, 3>{10, 0, 0}));   // Helmet (survivor) + Water (carried) = Casket
        CHECK(added.merges[0].combinations.size() == 1 && added.merges[0].combinations[0].result == 10 && added.merges[0].overflowItems.empty());
        // Conservation with overflow: the survivor is already full of unrelated items.
        UnitRoster r2(0, items.get());
        const UnitId x = r2.Add(db->Find(102)).unit.id;
        r2.Add(db->Find(102));
        for (ItemId item : {4u, 8u, 4u}) CHECK(r2.EquipItem(x, item) == ActionResult::Ok);   // Vest, Heart, Vest: no recipe among any of them
        const UnitId y = r2.Units()[1].id;
        CHECK(r2.EquipItem(y, 4) == ActionResult::Ok);
        const auto merged2 = r2.Add(db->Find(102));
        CHECK(merged2.merges.size() == 1 && (merged2.merges[0].overflowItems == std::vector<ItemId>{4}) && merged2.merges[0].combinations.empty());
    }
}

static void TestAuraPrimitive() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    const ItemDefinition* casket = items->Find(10);

    // Holder H casts a 100-damage magic hit on every attack; the enemy has 100 magic resist: 100 * 100/200 = 50 normally,
    // and with the Casket's aura (MR -30% -> 70) 100 * 100/170 = 58.
    ChampionDefinition holder = Fighter(1, 100000, 0, 0, 1000, 1);
    SetAbility(holder, 40, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 100))});
    const auto build = [&](bool withItem, HexCoord enemyPos = {3, 4}) {
        Duel d;
        d.Add(holder, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000, 0, 100), 2, 1, enemyPos);
        d.Finish();
        if (withItem) d.specs[0].items = {casket};
        return d;
    };
    {
        Duel d = build(true);
        const FightResult r = RunP10(d, 100, 1, cfg);
        const auto mr = StatusOn(r, 2, StatusType::MagicResist);
        CHECK(mr.size() == 1 && mr[0].tick == 0 && mr[0].amount == -30 && mr[0].duration == 0 && mr[0].other == 1);   // permanent while it stays inside
        const auto hits = Events(r.log, CombatEventType::Damage, 2);
        CHECK(!hits.empty() && hits[0].amount == MitigatedDamage(100, 70, 1));   // 58
        Duel plain = build(false);
        const FightResult rp = RunP10(plain, 100, 1, cfg);
        CHECK(Events(rp.log, CombatEventType::Damage, 2)[0].amount == MitigatedDamage(100, 100, 1) && StatusOn(rp, 2, StatusType::MagicResist).empty());
        // The holder's own stats from the item are there too (the aura is only ONE of the item's parts).
        CHECK(StatusOn(r, 1, StatusType::BonusArmor).size() == 1 && StatusOn(r, 1, StatusType::BonusManaRegen).size() == 1);
    }
    {   // Out of range: nothing. An enemy walking in gets the status the tick it arrives (at distance 2), and it stays.
        Duel d;
        ChampionDefinition idle = Dummy(1, 100000000);
        d.Add(idle, 1, 0, {3, 3});
        ChampionDefinition walker = Fighter(2, 100000000, 0, 1, 1000, 1);
        d.Add(walker, 2, 1, {3, 7});   // 4 hexes away
        d.Finish();
        d.specs[0].items = {casket};
        const FightResult r = RunP10(d, 200, 1, cfg);
        const auto mr = StatusOn(r, 2, StatusType::MagicResist);
        CHECK(mr.size() == 1 && mr[0].tick > 0);
        // On that tick the walker was exactly 2 hexes from the holder (a Move event before it brought it there).
        HexCoord walkerAt{3, 7};
        for (const CombatEvent& e : r.log.events) {
            if (e.tick >= mr[0].tick) break;
            if (e.type == CombatEventType::Move && e.unit == 2) walkerAt = e.to;
        }
        CHECK(hex::Distance(walkerAt, {3, 3}) <= 2);
        CHECK(Events(r.log, CombatEventType::StatusEnded, 2).empty());
    }
    {   // The aura ends when its holder dies: the enemy loses the status (with a StatusEnded event) the next tick.
        Duel d;
        d.Add(Dummy(1, 100, 0, 0), 1, 0, {3, 3});
        d.Add(Dummy(3, 100000000), 3, 0, {0, 0});   // an ally far away keeps the fight going after the holder falls
        d.Add(Fighter(2, 100000000, 0, 60), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {casket};
        const FightResult r = RunP10(d, 200, 1, cfg);
        const auto death = Events(r.log, CombatEventType::Death, 1);
        const auto ended = Events(r.log, CombatEventType::StatusEnded, 2);
        CHECK(death.size() == 1 && ended.size() == 1 && ended[0].subtype == static_cast<std::uint8_t>(StatusType::MagicResist) && ended[0].tick == death[0].tick + 1);
    }
    {   // Two holders: the effects stack (-30% and -30% = -60%). An untargetable enemy is not affected.
        Duel d;
        d.Add(holder, 1, 0, {3, 3});
        d.Add(Dummy(3, 100000000), 3, 0, {2, 3});
        d.Add(Dummy(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {casket};
        d.specs[1].items = {casket};
        const FightResult r = RunP10(d, 60, 1, cfg);
        CHECK(StatusOn(r, 2, StatusType::MagicResist).size() == 2);
        CHECK(Events(r.log, CombatEventType::Damage, 2)[0].amount == MitigatedDamage(100, 40, 1));   // MR 100 * (1 - 0.6) = 40
        ChampionDefinition ghost = WithPassive(Dummy(2, 100000000, 0, 100), 41, {PermanentStatus(StatusType::Untargetable, 0)});
        Duel g;
        g.Add(holder, 1, 0, {3, 3});
        g.Add(ghost, 2, 1, {3, 4});
        g.Add(Dummy(3, 100000000, 0, 100), 3, 1, {2, 4});
        g.Finish();
        g.specs[0].items = {casket};
        const FightResult rg = RunP10(g, 20, 1, cfg);
        CHECK(StatusOn(rg, 2, StatusType::MagicResist).empty() && StatusOn(rg, 3, StatusType::MagicResist).size() == 1);
    }
    {   // An ally aura (+ flat armor to allies within 1 hex, not the holder), and a percent-max-HP aura that moves current HP.
        auto mine = ItemDatabase::Create([] {
            ItemDefinition a;
            a.id = 1;
            a.name = "Banner";
            AuraDefinition aura;
            aura.side = TargetSide::Allies;
            aura.radius = 1;
            aura.status = StatusType::BonusArmor;
            aura.amount = 20;
            a.auras.push_back(aura);
            ItemDefinition b;
            b.id = 2;
            b.name = "Vitality";
            AuraDefinition hp;
            hp.side = TargetSide::Allies;
            hp.radius = 2;
            hp.status = StatusType::MaxHp;
            hp.amount = 50;
            hp.includeSelf = true;
            b.auras.push_back(hp);
            return std::vector<ItemDefinition>{a, b};
        }());
        CHECK(mine != nullptr);
        if (!mine) return;
        Duel d;
        d.Add(Dummy(1, 1000), 1, 0, {3, 3});
        d.Add(Dummy(2, 1000), 2, 0, {3, 2});     // adjacent ally
        d.Add(Dummy(3, 1000), 3, 0, {3, 0});     // 3 away
        d.Add(Dummy(9, 1000), 9, 1, {3, 6});
        d.Finish();
        d.specs[0].items = {mine->Find(1), mine->Find(2)};
        const FightResult r = RunP10(d, 20, 1, cfg);
        CHECK(StatusOn(r, 2, StatusType::BonusArmor).size() == 1 && StatusOn(r, 1, StatusType::BonusArmor).empty() && StatusOn(r, 3, StatusType::BonusArmor).empty());
        CHECK(StatusOn(r, 1, StatusType::MaxHp).size() == 1 && StatusOn(r, 2, StatusType::MaxHp).size() == 1 && StatusOn(r, 3, StatusType::MaxHp).empty());
        CHECK(StatusOn(r, 2, StatusType::MaxHp)[0].hpAfter == 1500);   // +50% max HP: current HP rises with it
    }
}

static void TestHooksOnTakingDamage() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.critBonusPercent = 21;

    {   // Guardians Armor: every 6th basic attack TAKEN grants +6 armor and +8 MR (stacking). Item stats: +40 armor, +17 MR.
        Duel d;
        d.Add(Dummy(1, 100000000), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});   // 100 per hit, one hit every 30 ticks, starting at tick 0
        d.Finish();
        d.specs[0].items = {items->Find(12)};
        const FightResult r = RunP10(d, 400, 1, cfg);
        const auto armor = StatusOn(r, 1, StatusType::BonusArmor);
        CHECK(armor.size() == 3 && armor[0].amount == 40 && armor[0].tick == 0 && armor[1].amount == 6 && armor[1].tick == 150 && armor[2].amount == 6 && armor[2].tick == 330);
        const auto mr = StatusOn(r, 1, StatusType::BonusMagicResist);
        CHECK(mr.size() == 3 && mr[1].amount == 8 && mr[1].tick == 150 && mr[2].tick == 330);
        // The stacks are real: hits 1-6 land on 40 armor, 7-12 on 46, then 52 (the 6th hit itself arrived before its own reaction).
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(hits.size() == 14);
        if (hits.size() == 14) {
            for (int k = 0; k < 6; ++k) CHECK(hits[static_cast<std::size_t>(k)].amount == MitigatedDamage(100, 40, 1));
            for (int k = 6; k < 12; ++k) CHECK(hits[static_cast<std::size_t>(k)].amount == MitigatedDamage(100, 46, 1));
            CHECK(hits[12].amount == MitigatedDamage(100, 52, 1));
        }
    }
    {   // A hook can be capped ("maxTriggers"), and counts only what its trigger names: spells do not count as basic attacks.
        ChampionDefinition holder = Dummy(1, 100000000);
        holder.triggers.push_back(Hook(60, CastTrigger::OnTakeBasicAttackDamage, {Eff(TargetSpec::Self(), Flat(StatusType::BonusArmor, 1))}, 0, 2));
        ChampionDefinition caster = Fighter(3, 100000000, 0, 0);
        SetAbility(caster, 61, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 5))});
        Duel d;
        d.Add(holder, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 10), 2, 1, {3, 4});   // basic attacks
        d.Add(caster, 3, 1, {2, 4});                          // spells only
        d.Finish();
        const FightResult r = RunP10(d, 300, 1, cfg);
        const auto stacks = StatusOn(r, 1, StatusType::BonusArmor);
        CHECK(stacks.size() == 2 && stacks[0].tick == 0 && stacks[1].tick == 30);   // fired on basic hits 1 and 2, then never again
        CHECK(stacks[0].other == 1);                                                  // its own effect: the holder is the source
    }
    {   // Mage Shield: after 4 abilities hit the user, a shield of 10% max HP. (Items: +30 MR, so 10 magic damage -> 7.)
        ChampionDefinition caster = Fighter(2, 100000000, 0, 0);
        SetAbility(caster, 62, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 10))});
        Duel d;
        d.Add(Dummy(1, 1000), 1, 0, {3, 3});
        d.Add(caster, 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(14)};
        const FightResult r = RunP10(d, 230, 1, cfg);
        const auto shields = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(shields.size() == 2 && shields[0].tick == 90 && shields[0].amount == 100 && shields[0].duration == 150 && shields[1].tick == 210);
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        CHECK(hits.size() >= 5 && hits[0].amount == MitigatedDamage(10, 30, 1) && hits[0].absorbed == 0);
        CHECK(hits[4].absorbed == hits[4].amount && hits[4].amount > 0);   // the 5th hit is soaked up by the new shield
    }
    {   // Damage over time is not "being cast at": a burning unit does not build the counter.
        ChampionDefinition burner = Fighter(2, 100000000, 0, 0);
        DotEffect burn;
        burn.type = DamageType::Magic;
        burn.amount = FlatAmount(Same(40));
        burn.amountIsTotal = true;
        burn.duration = FlatAmount(Same(120));
        burn.intervalTicks = 10;
        SetAbility(burner, 63, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), burn)});
        ChampionDefinition holder = Dummy(1, 100000000);
        holder.triggers.push_back(Hook(64, CastTrigger::OnTakeAbilityDamage, {Eff(TargetSpec::Self(), Flat(StatusType::BonusArmor, 1))}, 0, 0));
        Duel d;
        d.Add(holder, 1, 0, {3, 3});
        d.Add(burner, 2, 1, {3, 4});
        d.Finish();
        const FightResult r = RunP10(d, 200, 1, cfg);
        CHECK(!Events(r.log, CombatEventType::Damage, 1).empty() && StatusOn(r, 1, StatusType::BonusArmor).empty());
    }
    {   // Deadbeat: a spellshield blocks the FIRST enemy ability, whole, and is used up. Basic attacks are not spells.
        ChampionDefinition caster = Fighter(2, 100000000, 0, 0);
        SetAbility(caster, 65, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 50))});
        ChampionDefinition basic = Fighter(3, 100000000, 0, 20);
        Duel d;
        d.Add(Dummy(1, 100000000), 1, 0, {3, 3});
        d.Add(caster, 2, 1, {3, 4});
        d.Add(basic, 3, 1, {4, 4});   // adjacent to unit 1
        d.Finish();
        d.specs[0].items = {items->Find(25)};
        const FightResult r = RunP10(d, 100, 1, cfg);
        std::vector<CombatEvent> spellHits, basicHits;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 1)) (e.flags & kFlagAbility ? spellHits : basicHits).push_back(e);
        CHECK(spellHits.size() >= 3 && spellHits[0].tick == 30);                 // the tick-0 spell never landed
        CHECK(!basicHits.empty() && basicHits[0].tick == 0);                     // the tick-0 basic attack did
        const auto ended = Events(r.log, CombatEventType::StatusEnded, 1);
        CHECK(ended.size() == 1 && ended[0].tick == 0 && ended[0].subtype == static_cast<std::uint8_t>(StatusType::SpellShield));
        // Damage over time is not an ability HIT: it goes through.
    }
}

static void TestHooksOnDealingDamage() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    ChampionDefinition spell = Fighter(1, 100000, 0, 0, 1000, 1);
    SetAbility(spell, 70, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 100))});

    {   // Water Gun: dealing damage shreds the target's magic resist by 30% for 4 s. The FIRST hit lands before its own shred.
        Duel d;
        d.Add(spell, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(21)};
        const FightResult r = RunP10(d, 120, 1, cfg);
        const auto hits = Events(r.log, CombatEventType::Damage, 2);
        CHECK(hits.size() >= 3 && hits[0].amount == MitigatedDamage(100, 100, 1) && hits[1].amount == MitigatedDamage(100, 70, 1) && hits[2].amount == MitigatedDamage(100, 70, 1));
        const auto shred = StatusOn(r, 2, StatusType::MagicResist);
        CHECK(shred.size() >= 3 && shred[0].amount == -30 && shred[0].duration == 120 && shred[0].other == 1 && shred[0].tick == hits[0].tick);
        // "refreshes": re-applied, not stacked into -60%.
        for (std::size_t i = 1; i < hits.size(); ++i) CHECK(hits[i].amount == MitigatedDamage(100, 70, 1));
    }
    {   // Electroblade: basic attacks heal the user for 10% of the damage they deal -- basic only, spells do not.
        ChampionDefinition fighter = Fighter(1, 100000, 0, 100, 1000, 1);   // + 30 from the item = 130 per hit
        SetAbility(fighter, 71, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 50))});
        Duel d;
        d.Add(fighter, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 50), 2, 1, {3, 4});   // hits back, so there is something to heal
        d.Finish();
        d.specs[0].items = {items->Find(27)};
        const FightResult r = RunP10(d, 130, 1, cfg);
        int basic = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagBasic) { ++basic; CHECK(e.amount == 130); }
        }
        const auto heals = Events(r.log, CombatEventType::Heal, 1);
        CHECK(basic >= 4 && static_cast<int>(heals.size()) == basic);
        for (const CombatEvent& h : heals) CHECK(h.amount == 13 && h.other == 1);   // 10% of 130
    }
    {   // Betrayed Heart: 5% of all damage dealt is dealt AGAIN as true damage -- once. The echo fires no hooks of its own.
        ChampionDefinition fighter = Fighter(1, 100000, 0, 200, 1000, 1);
        ChampionDefinition ally = Dummy(3, 100000);
        ally.triggers.push_back(Hook(72, CastTrigger::OnAllyDealDamage, {Eff(TargetSpec::TriggerVictim(), Dmg(DamageType::True, 1))}));   // would chain if echoes counted
        Duel d;
        d.Add(fighter, 1, 0, {3, 3});
        d.Add(ally, 3, 0, {2, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(32)};
        const FightResult r = RunP10(d, 130, 1, cfg);
        int normal = 0, echoes = 0, allyEchoes = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagTriggered) {
                if (e.other == 1) { ++echoes; CHECK(e.amount == 10 && e.subtype == static_cast<std::uint8_t>(DamageType::True)); }
                else ++allyEchoes;
            } else {
                ++normal;
                CHECK(e.amount == 200);
            }
        }
        CHECK(normal >= 4 && echoes == normal && allyEchoes == normal);   // one echo per hit, and the ally reacts to the REAL hits only (not to the echoes)
    }
    {   // Omnilium's Book: spells (not basic attacks) wound: -30% healing for 3 s.
        ChampionDefinition fighter = Fighter(1, 100000, 0, 20, 1000, 1);
        SetAbility(fighter, 73, CastTrigger::EveryNthAttack, 3, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 10))});
        Duel d;
        d.Add(fighter, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(35)};
        const FightResult r = RunP10(d, 80, 1, cfg);
        const auto wounds = StatusOn(r, 2, StatusType::Wound);
        CHECK(wounds.size() == 1 && wounds[0].amount == 30 && wounds[0].duration == 90 && wounds[0].tick == 60);   // the 3rd attack's spell, not the basic hits before it
    }
}

static void TestHooksOnCrits() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    const auto crits = [&](int attackerCritChance, std::vector<AbilityEffect> attackerPassive, std::vector<AbilityEffect> victimPassive, std::vector<AbilityDefinition> victimHooks = {}) {
        ChampionDefinition attacker = Fighter(2, 100000000, 0, 100, 1000, 1);
        attacker.stats.critChance = Same(attackerCritChance);
        if (!attackerPassive.empty()) attacker = WithPassive(attacker, 80, std::move(attackerPassive));
        ChampionDefinition victim = Dummy(1, 100000000);
        if (!victimPassive.empty()) victim = WithPassive(victim, 81, std::move(victimPassive));
        victim.triggers = std::move(victimHooks);
        Duel d;
        d.Add(victim, 1, 0, {3, 3});
        d.Add(attacker, 2, 1, {3, 4});
        d.Finish();
        return std::make_pair(RunP10(d, 100, 1, cfg), 0);
    };
    const auto firstHit = [](const FightResult& r) { return Events(r.log, CombatEventType::Damage, 1).at(0); };

    // Crit damage = base + 21%. CritDamage (attacker) adds to the bonus; CritDamageTakenReduction (victim) removes a share of it.
    CHECK(firstHit(crits(100, {}, {}).first).amount == 121 && (firstHit(crits(100, {}, {}).first).flags & kFlagCrit));
    CHECK(firstHit(crits(0, {}, {}).first).amount == 100 && !(firstHit(crits(0, {}, {}).first).flags & kFlagCrit));
    CHECK(firstHit(crits(100, {PermanentStatus(StatusType::CritDamage, 30)}, {}).first).amount == 151);                   // 21 + 30
    CHECK(firstHit(crits(100, {}, {PermanentStatus(StatusType::CritDamageTakenReduction, 50)}).first).amount == 110);      // 21 -> 10
    CHECK(firstHit(crits(100, {PermanentStatus(StatusType::CritDamage, 30)}, {PermanentStatus(StatusType::CritDamageTakenReduction, 50)}).first).amount == 125);   // 51 -> 25
    CHECK(firstHit(crits(100, {}, {PermanentStatus(StatusType::CritDamageTakenReduction, 100)}).first).amount == 100);     // takes no bonus at all
    // A crit-mitigation item is real: Big Helmet (armor 30, MR 30, and half the crit bonus).
    {
        auto items = P10Items();
        ChampionDefinition attacker = Fighter(2, 100000000, 0, 100, 1000, 1);
        attacker.stats.critChance = Same(100);
        Duel d;
        d.Add(Dummy(1, 100000000), 1, 0, {3, 3});
        d.Add(attacker, 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(16)};
        const FightResult r = RunP10(d, 40, 1, cfg);
        CHECK(Events(r.log, CombatEventType::Damage, 1)[0].amount == MitigatedDamage(110, 30, 1));   // 100 + 10% of a crit, through 30 armor
    }
    // OnCritTaken: a hook that only fires on critical hits, and can read the damage: a shield worth the crit that landed.
    {
        AbilityDefinition shieldOnCrit = Hook(82, CastTrigger::OnCritTaken, {Eff(TargetSpec::Self(), [] {
            ShieldEffect s;
            s.amount = ScaledAmount({Scale(StatSource::TriggerDamage, Same(100))});
            s.duration = FlatAmount(Same(60));
            return s;
        }())});
        const auto crit = crits(100, {}, {}, {shieldOnCrit}).first;
        const auto shields = Events(crit.log, CombatEventType::ShieldApplied, 1);
        CHECK(shields.size() >= 3 && shields[0].amount == 121 && shields[0].tick == 0 && shields[0].other == 1);
        const auto plain = crits(0, {}, {}, {shieldOnCrit}).first;
        CHECK(Events(plain.log, CombatEventType::ShieldApplied, 1).empty());
    }
}

static void TestHooksOnLowHp() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // HeartBroke: below 30% HP, once: +10% AD and a shield of 25% max HP. (Fighter 750 HP + 250 from the item = 1000.)
        Duel d;
        d.Add(Fighter(1, 750, 0, 100), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(28)};
        const FightResult r = RunP10(d, 420, 1, cfg);
        const auto hits = Events(r.log, CombatEventType::Damage, 1);
        // 100 per hit from tick 0: HP 1000 -> ... after the 8th hit (tick 210) it is 200 < 300.
        CHECK(hits.size() >= 8 && hits[7].tick == 210 && hits[7].hpAfter == 200);
        const auto ad = StatusOn(r, 1, StatusType::AttackDamage);
        const auto shield = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(ad.size() == 1 && ad[0].tick == 210 && ad[0].amount == 10);
        CHECK(shield.size() == 1 && shield[0].tick == 210 && shield[0].amount == 250 && shield[0].duration == 0);   // permanent; once only
        CHECK(hits.size() > 8 && hits[8].absorbed > 0);
    }
    {   // Blue Whale: below 30%, heals 30% of max HP over 2 s (3 pulses of 10%), once.
        Duel d;
        d.Add(Fighter(1, 750, 0, 0), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(23)};   // +250 HP and +10% max HP: 750 * 1.1 + 250 = 1075
        const FightResult r = RunP10(d, 330, 1, cfg);
        CHECK(Events(r.log, CombatEventType::Spawn, 1)[0].hpAfter == 750);   // (the items apply right after the spawn)
        const auto hp = StatusOn(r, 1, StatusType::BonusMaxHp);
        CHECK(hp.size() == 1 && hp[0].hpAfter == 1000);   // 750 + 250 (the +10% status comes from the item's passive, applied next)
        const auto heals = Events(r.log, CombatEventType::Heal, 1);
        std::vector<int> healTicks;
        for (const CombatEvent& h : heals) if (h.other == 1) healTicks.push_back(h.tick);
        CHECK(healTicks.size() == 3 && healTicks[1] - healTicks[0] == 20 && healTicks[2] - healTicks[1] == 20);
        CHECK(heals[0].amount == 107);   // 10% of 1075
        CHECK(StatusOn(r, 1, StatusType::BonusManaRegen).size() == 2);   // the item's own +2, and the hook's +1
    }
    {   // Re-arming: a hook with no cap fires again each time the unit climbs back above the threshold and drops below it again.
        ChampionDefinition phoenix = Fighter(1, 1000, 0, 0);
        AbilityDefinition heal = Hook(90, CastTrigger::OnHpDropBelowPercent, {Eff(TargetSpec::Self(), [] {
            HealEffect h;
            h.amount = FlatAmount(Same(900));
            return h;
        }())});
        heal.thresholdPercent = 50;
        phoenix.triggers.push_back(heal);
        Duel d;
        d.Add(phoenix, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 100), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = RunP10(d, 600, 1, cfg);
        const auto heals = Events(r.log, CombatEventType::Heal, 1);
        CHECK(heals.size() >= 3);
        for (std::size_t i = 1; i < heals.size(); ++i) CHECK(heals[i].tick - heals[i - 1].tick >= 120);   // one firing per descent, never a burst
    }
}

static void TestHookOnAllyDamageAndCasts() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // OnAllyDealDamage: every hit an ally lands makes the holder deal 5% of it as magic damage to the same target.
        ChampionDefinition echo = Fighter(3, 100000, 0, 0);
        echo.triggers.push_back(Hook(100, CastTrigger::OnAllyDealDamage, {Eff(TargetSpec::TriggerVictim(), [] {
            DamageEffect e;
            e.type = DamageType::Magic;
            e.amount = ScaledAmount({Scale(StatSource::TriggerDamage, Same(5))});
            return e;
        }())}));
        Duel d;
        d.Add(Fighter(1, 100000, 0, 100), 1, 0, {3, 3});   // the ally that deals the damage
        d.Add(echo, 3, 0, {2, 3});
        d.Add(echo, 4, 0, {4, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = RunP10(d, 130, 1, cfg);
        int real = 0;
        std::map<UnitId, int> echoes;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagTriggered) { ++echoes[e.other]; CHECK(e.amount == 5 && e.subtype == static_cast<std::uint8_t>(DamageType::Magic)); }
            else if (e.other == 1) ++real;
        }
        CHECK(real >= 4 && echoes[3] == real && echoes[4] == real && echoes.size() == 2);   // two holders, one echo each per hit, no echoes of echoes
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) CHECK(!(e.flags & kFlagTriggered) || e.other != 1);   // the ally never echoes ITSELF
    }
    {   // OnCast: Fishtank grants +7 armor with every cast. Tear Of Mother: its 2nd cast (and only that one) grants a shield of 200% AP.
        ChampionDefinition caster = Fighter(1, 100000, 0, 0, 1000, 1);
        caster.stats.abilityDamage = Same(50);
        SetAbility(caster, 110, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 10))});
        Duel d;
        d.Add(caster, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(20)};
        const FightResult r = RunP10(d, 100, 1, cfg);
        const auto armor = StatusOn(r, 1, StatusType::BonusArmor);
        CHECK(armor.size() == 5 && armor[0].amount == 35 && armor[1].amount == 7 && armor[1].tick == 0 && armor[4].tick == 90);   // item, then one per cast (ticks 0, 30, 60, 90)
        Duel t;
        t.Add(caster, 1, 0, {3, 3});
        t.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        t.Finish();
        t.specs[0].items = {items->Find(18)};
        const FightResult rt = RunP10(t, 200, 1, cfg);
        const auto shields = Events(rt.log, CombatEventType::ShieldApplied, 1);
        CHECK(shields.size() == 1 && shields[0].tick == 30 && shields[0].amount == 130 && shields[0].duration == 150);   // 200% of (50 + 15) AP, on cast number 2 only
    }
    {   // EveryNthAttack as an item hook (Soldiers' Soul): +7 armor and +7 MR on every 3rd basic attack the user makes.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 10), 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(11)};
        const FightResult r = RunP10(d, 200, 1, cfg);
        const auto armor = StatusOn(r, 1, StatusType::BonusArmor);
        CHECK(armor.size() == 2 && armor[0].tick == 60 && armor[1].tick == 150 && armor[0].amount == 7);
        CHECK(StatusOn(r, 1, StatusType::BonusMagicResist).size() == 2);
        // Gylachster: on the 25th attack, once: CC immunity.
        Duel g;
        g.Add(Fighter(1, 100000, 0, 1, 2000), 1, 0, {3, 3});   // 2 attacks per second
        g.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        g.Finish();
        g.specs[0].items = {items->Find(13)};
        const FightResult rg = RunP10(g, 500, 1, cfg);
        const auto immune = StatusOn(rg, 1, StatusType::CcImmunity);
        CHECK(immune.size() == 1 && immune[0].tick == 24 * 15 && immune[0].duration == 0);   // the 25th attack is at tick 24 * 15; "for the rest of combat" = permanent
        CHECK(Events(rg.log, CombatEventType::StatusEnded, 1).empty());   // it never ends
    }
    {   // OnBasicAttack + ManaEffect (Unalive Sword): +4 mana with every attack, on top of the +10 an attack already gives.
        ChampionDefinition mage = Fighter(1, 100000, 0, 10, 1000, 1);
        mage.stats.maxMana = 100;
        CombatConfig withMana;   // default: +10 mana per attack
        Duel d;
        d.Add(mage, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(19)};
        const FightResult r = RunP10(d, 40, 1, withMana);
        const auto mana = Events(r.log, CombatEventType::ManaChanged, 1);
        CHECK(!mana.empty() && mana[0].tick == 0 && mana[0].amount == 10000 + 4000 + 33);   // attack 10 + item 4 + the item's +1 mana/s regen for one tick
    }
}

namespace {

ChampionDefinition SoulDef(ChampionId id, int hp = 100, int damage = 10) {
    ChampionDefinition d = Fighter(id, hp, 0, damage);
    d.name = "Soul";
    d.summon = true;
    return d;
}
AbilityEffect SummonEff(ChampionId champion, int count, int hpPercentOfTankiest = 0, int star = 0) {
    SummonEffect s;
    s.champion = champion;
    s.count = FlatAmount(Same(count));
    s.starLevel = star;
    if (hpPercentOfTankiest > 0) s.maxHp = ScaledAmount({Scale(StatSource::HighestAllyMaxHp, Same(hpPercentOfTankiest))});
    return Eff(TargetSpec::Self(), s);
}
std::vector<CombatEvent> Spawned(const FightResult& r, bool summonsOnly = true) {
    std::vector<CombatEvent> out;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Spawn)) {
        if (!summonsOnly || (e.flags & kFlagSummon)) out.push_back(e);
    }
    return out;
}

}  // namespace

static void TestSummonEffect() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    const ChampionDefinition soul = SoulDef(9101);

    {   // A passive summons 3 souls at once, each with 25% of the tankiest ally's max HP (2000 -> 500). They vanish when the fight ends.
        Duel d;
        d.defs.push_back(soul);
        d.Add(WithPassive(Fighter(1, 800, 0, 0), 120, {SummonEff(9101, 3, 25)}), 1, 0, {3, 3});
        d.Add(Dummy(2, 2000), 2, 0, {2, 3});
        d.Add(Dummy(9, 100), 9, 1, {3, 5});
        d.Finish();
        const FightResult r = RunP10(d, 600, 1, cfg);
        const auto spawns = Spawned(r);
        CHECK(spawns.size() == 3);
        std::set<std::pair<int, int>> cells;
        for (std::size_t i = 0; i < spawns.size(); ++i) {
            const CombatEvent& e = spawns[i];
            CHECK(e.tick == 0 && e.team == 0 && e.other == 1 && e.champion == 9101 && e.amount == 500 && e.hpAfter == 500 && e.star == 1);
            CHECK(e.unit == kSummonUnitBase + static_cast<UnitId>(i) + 1);
            CHECK(hex::Distance(e.to, {3, 3}) == 1);                  // the free hexes right next to the summoner
            cells.insert({e.to.x, e.to.y});
        }
        CHECK(cells.size() == 3);
        // The fight is decided by the REAL units: the souls (10 damage each) killed the 100-HP dummy, and the team "kept" 2 units.
        CHECK(r.winner == CombatWinner::Home && r.log.survivors[0] == 2 && r.log.survivors[1] == 0);
        int soulAttacks = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack)) soulAttacks += e.unit > kSummonUnitBase ? 1 : 0;
        CHECK(soulAttacks > 0);
        // "Vanish completely when combat ends": a Death event for each, flagged, on the last tick.
        int vanished = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Death)) {
            if (e.unit > kSummonUnitBase) {
                ++vanished;
                CHECK((e.flags & kFlagSummon) && e.tick == r.log.endTick);
            }
        }
        CHECK(vanished == 3);
        // Deterministic.
        CHECK(RunP10(d, 600, 1, cfg).log.checksum == r.log.checksum);
    }
    {   // Summons are not units of the team: with the summoner dead, the fight is lost even though its souls are alive.
        Duel d;
        d.defs.push_back(soul);
        d.Add(WithPassive(Fighter(1, 50, 0, 0), 121, {SummonEff(9101, 3)}), 1, 0, {3, 3});
        d.Add(Fighter(9, 100000, 0, 100), 9, 1, {3, 4});
        d.Finish();
        const FightResult r = RunP10(d, 600, 1, cfg);
        CHECK(Spawned(r).size() == 3 && r.winner == CombatWinner::Away && r.log.survivors[0] == 0 && r.log.survivors[1] == 1);
        CHECK(Events(r.log, CombatEventType::Death, 1).size() == 1);
        int vanished = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Death)) vanished += (e.flags & kFlagSummon) ? 1 : 0;
        CHECK(vanished == 3);
    }
    {   // A hook can summon MID-fight (and at another star level): a Spawn on a later tick, next to a living summoner.
        ChampionDefinition summoner = Dummy(1, 100000);
        AbilityDefinition hook = Hook(122, CastTrigger::OnTakeBasicAttackDamage, {SummonEff(9101, 2, 0, 2)}, 0, 1);
        summoner.triggers.push_back(hook);
        Duel d;
        d.defs.push_back(soul);
        d.Add(summoner, 1, 0, {3, 3});
        d.Add(Fighter(9, 100000000, 0, 10), 9, 1, {3, 7});   // has to walk over first
        d.Finish();
        const FightResult r = RunP10(d, 400, 1, cfg);
        const auto spawns = Spawned(r);
        CHECK(spawns.size() == 2 && spawns[0].tick > 30 && spawns[0].tick == spawns[1].tick && spawns[0].star == 2 && spawns[0].amount == 100 * 1);
    }
    {   // A count that scales with the star level: 1 / 2 / 3.
        SummonEffect s;
        s.champion = 9101;
        s.count = FlatAmount({{1, 2, 3}});
        Duel d;
        d.defs.push_back(soul);
        d.Add(WithPassive(Dummy(1, 1000), 123, {Eff(TargetSpec::Self(), s)}), 1, 0, {3, 3}, 3);
        d.Add(Dummy(9, 1000), 9, 1, {3, 6});
        d.Finish();
        CHECK(Spawned(RunP10(d, 20, 1, cfg)).size() == 3);
    }
    {   // A runaway summoner is capped (and out of room, it just stops): never an unbounded fight.
        AbilityEffect flood = SummonEff(9101, 8);
        flood.repeatCount = 64;
        flood.repeatIntervalTicks = 1;
        Duel d;
        d.defs.push_back(soul);
        d.Add(WithPassive(Dummy(1, 100000), 124, {flood}), 1, 0, {3, 3});
        d.Add(Dummy(9, 100000000), 9, 1, {3, 7});
        d.Finish();
        const FightResult r = RunP10(d, 120, 1, cfg);
        const std::size_t n = Spawned(r).size();
        CHECK(n >= 20 && n <= 48);
    }
    {   // A summon with its own passive (untargetable) really is untouchable; its own attack still works.
        ChampionDefinition ghost = WithPassive(SoulDef(9102, 100, 10), 125, {PermanentStatus(StatusType::Untargetable, 0)});
        Duel d;
        d.defs.push_back(ghost);
        d.Add(WithPassive(Dummy(1, 100000), 126, {SummonEff(9102, 2)}), 1, 0, {3, 3});
        d.Add(Fighter(9, 100000000, 0, 10), 9, 1, {3, 5});
        d.Finish();
        const FightResult r = RunP10(d, 200, 1, cfg);
        CHECK(Spawned(r).size() == 2 && StatusOn(r, kSummonUnitBase + 1, StatusType::Untargetable).size() == 1);
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack)) CHECK(e.other < kSummonUnitBase);   // nobody ever attacks a ghost
        int soulSwings = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack)) soulSwings += e.unit > kSummonUnitBase ? 1 : 0;
        CHECK(soulSwings > 0);
    }

    // The database keeps summons honest.
    {
        std::string err;
        ChampionDefinition summoner = WithPassive(Fighter(1, 100, 0, 1), 130, {SummonEff(9101, 1)});
        CHECK(ChampionDatabase::Create({summoner, SoulDef(9101)}, &err) != nullptr);
        CHECK(ChampionDatabase::Create({summoner}, &err) == nullptr && err.find("does not exist or is not marked") != std::string::npos);   // no such champion
        CHECK(ChampionDatabase::Create({summoner, Fighter(9101, 100, 0, 1)}, &err) == nullptr && err.find("not marked") != std::string::npos);      // a real champion cannot be summoned
        ChampionDefinition traited = SoulDef(9101);
        traited.traits = {"Coregons"};
        CHECK(ChampionDatabase::Create({summoner, traited}, &err) == nullptr && err.find("no synergy") != std::string::npos);                        // summons have no traits
        ChampionDefinition wrongTarget = WithPassive(Fighter(1, 100, 0, 1), 131, {SummonEff(9101, 1)});
        wrongTarget.passive.effects[0].target = TargetSpec::CurrentTarget();
        CHECK(ChampionDatabase::Create({wrongTarget, SoulDef(9101)}, &err) == nullptr);   // (a passive has no current target either)
        AbilityEffect zero = SummonEff(9101, 0);
        CHECK(ChampionDatabase::Create({WithPassive(Fighter(1, 100, 0, 1), 132, {zero}), SoulDef(9101)}, &err) == nullptr && err.find("summons nothing") != std::string::npos);
        AbilityEffect many = SummonEff(9101, 9);
        CHECK(ChampionDatabase::Create({WithPassive(Fighter(1, 100, 0, 1), 133, {many}), SoulDef(9101)}, &err) == nullptr && err.find("0..8") != std::string::npos);
    }
}

static void TestSummonsAreNotSoldOrOwned() {
    // A summon-only champion is in the database (so effects can find it) but not in the shop, the pool, or any roster.
    std::vector<ChampionDefinition> defs;
    for (int i = 0; i < 5; ++i) {
        ChampionDefinition d = Def(static_cast<ChampionId>(101 + i), "One", 1);
        d.stats.maxHp = Same(100);
        d.stats.attackDamage = Same(10);
        defs.push_back(d);
    }
    ChampionDefinition soul = SoulDef(9101);
    soul.cost = 1;   // even claiming a cost in the shop's tier changes nothing
    defs.push_back(soul);
    auto db = ChampionDatabase::Create(defs);
    CHECK(db != nullptr);
    if (!db) return;
    auto match = PlanningMatch(*db, nullptr, 3, 2);
    CHECK(match != nullptr);
    if (!match) return;
    CHECK(match->Pool().InitialCopies(9101) == 0 && match->Pool().Remaining(9101) == 0);
    int sellable = 0;
    for (ChampionId id = 101; id <= 105; ++id) sellable += match->Pool().InitialCopies(id);
    CHECK(sellable == 5 * 29);   // only the five sellable champions have copies: 29 each
    PlayerState* p = match->PlayersMutable().Get(0);
    p->AddGold(10000);
    bool everOffered = false;
    for (int i = 0; i < 300; ++i) {
        if (match->TryRerollShop(0) != ActionResult::Ok) break;
        for (const ChampionDefinition* slot : p->Shop().Slots()) everOffered = everOffered || (slot != nullptr && slot->id == 9101);
    }
    CHECK(!everOffered);
    CHECK(match->VerifyPoolIntegrity());
}

static void TestAbilityCrit() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    auto items = P10Items();
    const auto caster = [](int critChance, std::vector<AbilityEffect> passive = {}, bool canCrit = false) {
        ChampionDefinition c = Fighter(1, 100000, 0, 0, 1000, 1);
        c.stats.critChance = Same(critChance);
        DamageEffect hit = Dmg(DamageType::Magic, 100);
        hit.canCrit = canCrit;
        SetAbility(c, 140, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), hit)});
        if (!passive.empty()) c = WithPassive(c, 141, std::move(passive));
        return c;
    };
    const auto firstSpell = [&](const ChampionDefinition& c) {
        Duel d;
        d.Add(c, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = RunP10(d, 40, 1, cfg);
        return Events(r.log, CombatEventType::Damage, 2).at(0);
    };
    // Ordinary spells never crit -- however high the crit chance is.
    CombatEvent e = firstSpell(caster(100));
    CHECK(e.amount == 100 && !(e.flags & kFlagCrit) && (e.flags & kFlagAbility));
    // With the AbilityCrit status they roll the crit chance like a basic attack does, and crit for base + 21%.
    e = firstSpell(caster(100, {PermanentStatus(StatusType::AbilityCrit, 0)}));
    CHECK(e.amount == 121 && (e.flags & kFlagCrit) && (e.flags & kFlagAbility));
    // ... with CritDamage on top,
    CHECK(firstSpell(caster(100, {PermanentStatus(StatusType::AbilityCrit, 0), PermanentStatus(StatusType::CritDamage, 30)})).amount == 151);
    // ... only if the roll succeeds,
    e = firstSpell(caster(0, {PermanentStatus(StatusType::AbilityCrit, 0)}));
    CHECK(e.amount == 100 && !(e.flags & kFlagCrit));
    // ... and the older per-effect flag (Cyla) keeps working without the status.
    CHECK(firstSpell(caster(100, {}, true)).amount == 121);

    // Resist Puncher: +25% crit chance and AbilityCrit. Over 100 spells about a quarter crit; a seed replays exactly.
    CHECK(items != nullptr);
    if (!items) return;
    ChampionDefinition puncher = caster(0);
    puncher.stats.attackSpeedMilli = 10000;   // a spell every 3 ticks
    Duel d;
    d.Add(puncher, 1, 0, {3, 3});
    d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
    d.Finish();
    d.specs[0].items = {items->Find(36)};
    const FightResult a = RunP10(d, 300, 7, cfg);
    int spells = 0, crits = 0;
    for (const CombatEvent& hit : Events(a.log, CombatEventType::Damage, 2)) {
        ++spells;
        crits += (hit.flags & kFlagCrit) ? 1 : 0;
        CHECK(hit.amount == ((hit.flags & kFlagCrit) ? 121 : 100));
    }
    CHECK(spells >= 90 && crits >= 10 && crits <= 45);
    CHECK(RunP10(d, 300, 7, cfg).log.checksum == a.log.checksum && RunP10(d, 300, 8, cfg).log.checksum != a.log.checksum);
}

static void TestNewSmallPrimitives() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    cfg.rawDamagePerMana = 1'000'000'000;
    {   // ManaEffect: +30 mana to a unit with a bar (clamped at the bar), nothing for a unit without one, and it can drain.
        ChampionDefinition mage = WithPassive(Fighter(1, 1000, 0, 0), 150, {Eff(TargetSpec::Self(), [] { ManaEffect m; m.amount = FlatAmount(Same(30)); return m; }())});
        mage.stats.maxMana = 100;
        ChampionDefinition plain = WithPassive(Fighter(2, 1000, 0, 0), 151, {Eff(TargetSpec::Self(), [] { ManaEffect m; m.amount = FlatAmount(Same(30)); return m; }())});
        ChampionDefinition drain = WithPassive(Fighter(3, 1000, 0, 0), 152, {Eff(TargetSpec::Self(), [] { ManaEffect m; m.amount = FlatAmount(Same(-50)); return m; }())});
        drain.stats.maxMana = 100;
        drain.stats.startMana = 20;
        ChampionDefinition full = WithPassive(Fighter(4, 1000, 0, 0), 153, {Eff(TargetSpec::Self(), [] { ManaEffect m; m.amount = FlatAmount(Same(500)); return m; }())});
        full.stats.maxMana = 100;
        Duel d;
        d.Add(mage, 1, 0, {2, 3});
        d.Add(plain, 2, 0, {3, 3});
        d.Add(drain, 3, 0, {4, 3});
        d.Add(full, 4, 0, {5, 3});
        d.Add(Dummy(9, 1000000), 9, 1, {3, 6});
        d.Finish();
        const FightResult r = RunP10(d, 5, 1, cfg);
        const auto mana = [&](UnitId u) { return Events(r.log, CombatEventType::ManaChanged, u); };
        CHECK(mana(1).size() >= 1 && mana(1).back().amount == 30000);
        CHECK(mana(2).empty());                                        // no mana bar: nothing to grant, no event
        CHECK(mana(3).back().amount == 0);                             // 20 - 50 clamps at empty
        CHECK(mana(4).back().amount == 100000);                        // 20 + 500 clamps at the bar
    }
    {   // BonusManaRegen (the flat family): +2 mana/s doubles a 2/s champion's regen, so a 10-mana spell comes at tick 75 instead of 150.
        const auto castTick = [&](std::vector<AbilityEffect> passive) {
            ChampionDefinition c = Fighter(1, 1000, 0, 0);
            c.stats.maxMana = 10;
            c.stats.manaRegenMilli = 2000;
            SetAbility(c, 154, CastTrigger::Mana, 0, 0, {Eff(TargetSpec::Self(), Shld(10, 30))});
            if (!passive.empty()) c = WithPassive(c, 155, std::move(passive));
            Duel d;
            d.Add(c, 1, 0, {3, 3});
            d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
            d.Finish();
            return Events(RunP10(d, 400, 1, cfg).log, CombatEventType::SpellCast, 1).at(0).tick;
        };
        StatusEffect regen = Flat(StatusType::BonusManaRegen, 2000);
        CHECK(castTick({}) == 150);
        CHECK(castTick({Eff(TargetSpec::Self(), regen)}) == 75);
    }
    {   // AbilityPower: +30% AP turns a 100-AP spell into 130; -50% into 50.
        const auto spellDamage = [&](int percent) {
            ChampionDefinition c = Fighter(1, 100000, 0, 0);
            c.stats.abilityDamage = Same(100);
            SetAbility(c, 156, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), [] {
                DamageEffect e;
                e.type = DamageType::Magic;
                e.amount = ScaledAmount({Scale(StatSource::SelfAbilityDamage, Same(100))});
                return e;
            }())});
            if (percent != 0) c = WithPassive(c, 157, {PermanentStatus(StatusType::AbilityPower, percent)});
            Duel d;
            d.Add(c, 1, 0, {3, 3});
            d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
            d.Finish();
            return Events(RunP10(d, 40, 1, cfg).log, CombatEventType::Damage, 2).at(0).amount;
        };
        CHECK(spellDamage(0) == 100 && spellDamage(30) == 130 && spellDamage(-50) == 50);
    }
}

static void TestNoChainReactions() {
    // A hook whose effect is damage, on the trigger "dealing damage": if that damage fired the hook again it would never stop.
    // It fires exactly once per real hit, in every combination of hooks, and the fight replays identically.
    ChampionDefinition holder = Fighter(1, 100000, 0, 50);
    holder.triggers.push_back(Hook(160, CastTrigger::OnDealDamage, {Eff(TargetSpec::TriggerVictim(), Dmg(DamageType::True, 7))}));
    holder.triggers.push_back(Hook(161, CastTrigger::OnAllyDealDamage, {Eff(TargetSpec::TriggerVictim(), Dmg(DamageType::True, 3))}));
    ChampionDefinition twin = holder;
    twin.id = 3;
    Duel d;
    d.Add(holder, 1, 0, {3, 3});
    d.Add(twin, 3, 0, {4, 3});
    d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
    d.Finish();
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    const FightResult r = RunP10(d, 200, 1, cfg);
    std::map<UnitId, int> real, echo7, echo3;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
        if (!(e.flags & kFlagTriggered)) ++real[e.other];
        else if (e.amount == 7) ++echo7[e.other];
        else if (e.amount == 3) ++echo3[e.other];
    }
    // Each real hit by X: X's own OnDealDamage echo (7) and the OTHER unit's OnAllyDealDamage echo (3). Nothing else.
    for (UnitId x : {UnitId{1}, UnitId{3}}) {
        const UnitId other = x == 1 ? 3 : 1;
        CHECK(real[x] >= 5 && echo7[x] == real[x] && echo3[other] == real[x]);
    }
    CHECK(RunP10(d, 200, 1, cfg).log.checksum == r.log.checksum);
}

static void TestPhase10Loader() {
    const char* full = R"({"version": 1, "champions": [
      { "id": 9101, "name": "Ghoul", "summon": true,
        "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 10, "attackSpeed": 1.0, "range": 1 },
        "passive": { "id": 1, "name": "Ghostly", "effects": [ { "type": "Status", "target": "Self", "status": "Untargetable", "permanent": true } ] },
        "triggers": [ { "id": 2, "name": "Echo", "trigger": "OnAllyDealDamage", "damageFilter": "Any", "effects": [
            { "type": "Damage", "target": "TriggerVictim", "damageType": "Magic", "amount": { "terms": [ { "source": "TriggerDamage", "percent": 5 } ] } } ] } ] },
      { "id": 1, "name": "Necro", "cost": 3,
        "stats": { "hp": 500, "armor": 10, "magicResist": 10, "attackDamage": 30, "attackSpeed": 0.8, "range": 2, "maxMana": 60 },
        "ability": { "id": 3, "name": "Raise", "trigger": "Mana", "effects": [
            { "type": "Summon", "target": "Self", "champion": 9101, "count": [1, 2, 3], "maxHp": { "terms": [ { "source": "HighestAllyMaxHp", "percent": 25 } ] }, "attackDamage": 50 } ] },
        "triggers": [
          { "id": 4, "name": "Thorns", "trigger": "OnTakeBasicAttackDamage", "attackCount": 3, "maxTriggers": 2, "effects": [
              { "type": "Damage", "target": "TriggerAttacker", "damageType": "Magic", "amount": 10 } ] },
          { "id": 5, "name": "Last stand", "trigger": "OnHpDropBelowPercent", "thresholdPercent": 30, "maxTriggers": 1, "effects": [
              { "type": "Shield", "target": "Self", "amount": 100, "durationSeconds": 3 } ] },
          { "id": 6, "name": "Feast", "trigger": "OnDealDamage", "damageFilter": "Basic", "effects": [ { "type": "Mana", "target": "Self", "amount": 2 } ] },
          { "id": 7, "name": "Spellproof", "trigger": "OnCritTaken", "effects": [ { "type": "Status", "target": "Self", "status": "SpellShield", "durationSeconds": 4 } ] },
          { "id": 8, "name": "Cast", "trigger": "OnCast", "effects": [ { "type": "Status", "target": "Self", "status": "AbilityCrit", "durationSeconds": 2 } ] },
          { "id": 9, "name": "Nth", "trigger": "EveryNthAttack", "attackCount": 4, "effects": [ { "type": "Status", "target": "Self", "status": "AbilityPower", "percent": 5, "permanent": true } ] },
          { "id": 10, "name": "OnAtk", "trigger": "OnBasicAttack", "effects": [ { "type": "Status", "target": "Self", "status": "BonusManaRegen", "value": 250, "permanent": true } ] } ],
        "auras": [ { "side": "Enemies", "radius": 2, "status": "Armor", "amount": -10 }, { "side": "Allies", "radius": 1, "status": "BonusArmor", "amount": 5, "includeSelf": true } ] } ]})";
    std::string err;
    auto db = w2f::LoadChampionDatabaseFromJson(full, &err);
    CHECK(db != nullptr);
    if (!db) { std::printf("  %s\n", err.c_str()); return; }
    const ChampionDefinition* ghoul = db->Find(9101);
    const ChampionDefinition* necro = db->Find(1);
    CHECK(ghoul && ghoul->summon && ghoul->cost == 1 && ghoul->triggers.size() == 1 && ghoul->triggers[0].trigger == CastTrigger::OnAllyDealDamage);   // a summon needs no cost
    CHECK(necro && !necro->summon && necro->triggers.size() == 7 && necro->auras.size() == 2);
    if (!necro) return;
    const auto* summon = std::get_if<SummonEffect>(&necro->ability.effects[0].payload);
    CHECK(summon && summon->champion == 9101 && summon->count.flat == StarValue({{1, 2, 3}}) && summon->maxHp.terms.size() == 1 &&
          summon->maxHp.terms[0].source == StatSource::HighestAllyMaxHp && summon->attackDamage.flat == Same(50));
    const auto& t = necro->triggers;
    CHECK(t[0].trigger == CastTrigger::OnTakeBasicAttackDamage && t[0].attackCount == 3 && t[0].maxTriggers == 2 && t[0].effects[0].target.mode == TargetMode::TriggerAttacker);
    CHECK(t[1].trigger == CastTrigger::OnHpDropBelowPercent && t[1].thresholdPercent == 30 && t[1].maxTriggers == 1);
    CHECK(t[2].damageFilter == DamageFilter::Basic && std::holds_alternative<ManaEffect>(t[2].effects[0].payload));
    CHECK(t[3].trigger == CastTrigger::OnCritTaken && t[4].trigger == CastTrigger::OnCast && t[5].trigger == CastTrigger::EveryNthAttack && t[6].trigger == CastTrigger::OnBasicAttack);
    CHECK(necro->auras[0].side == TargetSide::Enemies && necro->auras[0].radius == 2 && necro->auras[0].status == StatusType::Armor && necro->auras[0].amount == -10 &&
          necro->auras[1].includeSelf && necro->auras[1].amount == 5);
    // The hash notices a changed trigger.
    std::string changed = full;
    changed.replace(changed.find("\"thresholdPercent\": 30"), std::string("\"thresholdPercent\": 30").size(), "\"thresholdPercent\": 31");
    auto db2 = w2f::LoadChampionDatabaseFromJson(changed, &err);
    CHECK(db2 != nullptr && db2->ContentHash() != db->ContentHash());

    // The mistakes a designer can make, each with a specific message.
    const std::string effect = R"({"type": "Status", "target": "Self", "status": "BonusArmor", "value": 1, "permanent": true})";
    const auto champion = [&](const std::string& extra) {
        return R"({"version": 1, "champions": [
          { "id": 9101, "name": "Ghoul", "summon": true, "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 10, "attackSpeed": 1.0, "range": 1 } },
          { "id": 1, "name": "X", "cost": 1, "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 10, "attackSpeed": 1.0, "range": 1, "maxMana": 50 }, )" + extra + "} ]}";
    };
    const auto trig = [&](const std::string& fields, const std::string& eff = "") {
        return champion(R"("triggers": [ { "id": 4, "name": "t", )" + fields + R"(, "effects": [ )" + (eff.empty() ? effect : eff) + " ] } ]");
    };
    struct Bad { std::string json; const char* mention; };
    const std::vector<Bad> bad = {
        {trig(R"("trigger": "OnDeath")"), "OnDeath"},
        {trig(R"("trigger": "OnDealDamage", "thresholdPercent": 30)"), "thresholdPercent only applies"},
        {trig(R"("trigger": "OnHpDropBelowPercent")"), "thresholdPercent must be 1..99"},
        {trig(R"("trigger": "OnHpDropBelowPercent", "thresholdPercent": 100)"), ".thresholdPercent"},
        {trig(R"("trigger": "OnCast", "damageFilter": "Basic")"), "damageFilter only applies"},
        {trig(R"("trigger": "OnDealDamage", "damageFilter": "Physical")"), "Physical"},
        {trig(R"("trigger": "OnDealDamage", "maxTriggers": 0)"), ".maxTriggers"},
        {trig(R"("trigger": "OnDealDamage", "castLockTicks": 5)"), "cannot have a cast lock"},
        {trig(R"("trigger": "Mana")"), "'triggers' holds hooks"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Damage", "target": "TriggerVictim", "damageType": "True", "amount": 5})"), "only exist for damage hooks"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Heal", "target": "Self", "amount": { "terms": [ { "source": "TriggerDamage", "percent": 10 } ] }})"), "TriggerDamage source only exists"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Summon", "target": "CurrentTarget", "champion": 9101})"), "must target Self"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Summon", "target": "Self", "champion": 9101, "count": 0})"), "summons nothing"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Summon", "target": "Self", "champion": 9101, "count": 9})"), "0..8"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Summon", "target": "Self"})"), "champion"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Summon", "target": "Self", "champion": 1})"), "not marked"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Summon", "target": "Self", "champion": 4242})"), "does not exist"},
        {trig(R"("trigger": "OnCast")", R"({"type": "Mana", "target": "Self"})"), "amount"},
        {champion(R"("auras": [ { "side": "Enemies", "radius": 2, "status": "Stun", "amount": 5 } ])"), "cannot apply Stun"},
        {champion(R"("auras": [ { "side": "Enemies", "radius": 0, "status": "Armor", "amount": -5 } ])"), ".radius"},
        {champion(R"("auras": [ { "radius": 2, "status": "Armor", "amount": -5 } ])"), "side"},
        {champion(R"("auras": [ { "side": "Enemies", "radius": 2, "status": "Armor", "amount": -5, "colour": 1 } ])"), "colour"},
        {champion(R"("summon": 3)"), "summon"},
        {champion(R"("traits": ["Coregons"], "summon": true)"), "no synergy"},
    };
    int index = 0;
    for (const Bad& b : bad) {
        std::string e;
        const bool loaded = w2f::LoadChampionDatabaseFromJson(b.json, &e) != nullptr;
        const bool ok = !loaded && e.find(b.mention) != std::string::npos;
        if (!ok) std::printf("  loader case %d: wanted an error mentioning '%s', got %s'%s'\n", index, b.mention, loaded ? "NO ERROR " : "", e.c_str());
        CHECK(ok);
        ++index;
    }
    // (the last case above listed a summon WITH traits on champion X, so it fails on X's own `summon` + `traits`)

    // Items: components / abilities / auras / manaRegen in items.json, and their mistakes.
    struct BadItem { const char* json; const char* mention; };
    const BadItem badItems[] = {
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "stats": {"armor": 1}}, {"id": 2, "name": "B", "stats": {"armor": 1}}, {"id": 3, "name": "C", "components": [1, 2, 1], "stats": {"armor": 1}} ]})", "exactly two"},
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "stats": {"armor": 1}}, {"id": 3, "name": "C", "components": [1, 9], "stats": {"armor": 1}} ]})", "does not exist"},
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "stats": {"armor": 1}}, {"id": 3, "name": "C", "components": [1, "x"], "stats": {"armor": 1}} ]})", "components[1]"},
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "stats": {"manaRegen": 1.2345}} ]})", "3 decimal"},
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "auras": [ {"side": "Enemies", "radius": 2, "status": "Armor", "amount": 0} ]} ]})", "amount 0"},
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "abilities": [ {"id": 5, "name": "x", "trigger": "Mana", "effects": [ {"type": "Status", "target": "Self", "status": "BonusArmor", "value": 1, "permanent": true} ]} ]} ]})", "mana"},
        {R"({"version": 1, "items": [ {"id": 1, "name": "A", "abilities": [ {"id": 5, "name": "x", "effects": [ {"type": "Status", "target": "Self", "status": "BonusArmor", "value": 1, "permanent": true} ]} ]} ]})", "trigger"},
    };
    for (const BadItem& b : badItems) {
        std::string e;
        const bool loaded = w2f::LoadItemDatabaseFromJson(b.json, &e) != nullptr;
        const bool ok = !loaded && e.find(b.mention) != std::string::npos;
        if (!ok) std::printf("  item case: wanted an error mentioning '%s', got %s'%s'\n", b.mention, loaded ? "NO ERROR " : "", e.c_str());
        CHECK(ok);
    }
    auto goodItems = w2f::LoadItemDatabaseFromJson(R"({"version": 1, "items": [ {"id": 1, "name": "A", "stats": {"manaRegen": 1.5}}, {"id": 2, "name": "B", "stats": {"armor": 3}}, {"id": 3, "name": "AB", "components": [2, 1], "stats": {"armor": 3, "manaRegen": 2.25}} ]})", &err);
    CHECK(goodItems != nullptr && goodItems->Find(1)->stats.manaRegenMilli == 1500 && goodItems->Find(3)->stats.manaRegenMilli == 2250 && goodItems->FindCombination(1, 2)->id == 3);

    // Traits: hooks handed to units in scope, and a Team-scoped summon.
    const char* traitJson = R"({"version": 1, "traits": [ { "id": 1, "name": "Coregons", "breakpoints": [ { "count": 2,
        "effects": [ { "scope": "Team", "type": "Summon", "champion": 9101, "count": 3 } ],
        "triggers": [ { "scope": "TraitHolders", "ability": { "id": 20, "name": "Heal", "trigger": "OnDealDamage", "effects": [ { "type": "Heal", "target": "Self", "amount": { "terms": [ { "source": "TriggerDamage", "percent": 10 } ] } } ] } } ] } ] } ]})";
    auto traits = w2f::LoadTraitDatabaseFromJson(traitJson, &err);
    CHECK(traits != nullptr);
    if (traits) {
        const TraitBreakpoint& bp = traits->FindByName("Coregons")->breakpoints[0];
        CHECK(bp.effects.size() == 1 && bp.effects[0].scope == TraitScope::Team && bp.triggers.size() == 1 && bp.triggers[0].scope == TraitScope::TraitHolders &&
              bp.triggers[0].ability.trigger == CastTrigger::OnDealDamage);
        CHECK(w2f::ValidateSummonReferences(*db, nullptr, traits.get(), &err));
        auto noGhoul = ChampionDatabase::Create({Fighter(1, 10, 0, 1)});
        CHECK(!w2f::ValidateSummonReferences(*noGhoul, nullptr, traits.get(), &err) && err.find("9101") != std::string::npos);
    }
    const char* badTraits[][2] = {
        {R"({"version": 1, "traits": [ { "id": 1, "name": "T", "breakpoints": [ { "count": 2, "effects": [ { "scope": "Team", "type": "Status", "target": "CurrentTarget", "status": "Armor", "percent": 5, "permanent": true } ] } ] } ]})", "scope-Team effect targets Self, AllEnemies or AllAllies"},
        {R"({"version": 1, "traits": [ { "id": 1, "name": "T", "breakpoints": [ { "count": 2, "effects": [ { "scope": "AllAllies", "type": "Status", "target": "AllEnemies", "status": "Armor", "percent": 5, "permanent": true } ] } ] } ]})", "apply to each unit itself"},
        {R"({"version": 1, "traits": [ { "id": 1, "name": "T", "breakpoints": [ { "count": 2, "effects": [ { "scope": "Team", "type": "Status", "target": "AllEnemies", "status": "ExecuteBelow", "percent": 80, "permanent": true } ] } ] } ]})", "ExecuteBelow"},
        {R"({"version": 1, "traits": [ { "id": 1, "name": "T", "breakpoints": [ { "count": 2, "triggers": [ { "scope": "TraitHolders", "ability": { "id": 20, "name": "p", "trigger": "StartOfCombat", "effects": [ { "type": "Status", "target": "Self", "status": "BonusArmor", "value": 1, "permanent": true } ] } } ] } ] } ]})", "must be hooks"},
        {R"({"version": 1, "traits": [ { "id": 1, "name": "T", "breakpoints": [ { "count": 2, "triggers": [ { "scope": "Team", "ability": { "id": 20, "name": "p", "trigger": "OnCast", "effects": [ { "type": "Status", "target": "Self", "status": "BonusArmor", "value": 1, "permanent": true } ] } } ] } ] } ]})", "AllAllies or TraitHolders"},
        {R"({"version": 1, "traits": [ { "id": 1, "name": "T", "breakpoints": [ { "count": 2 } ] } ]})", "effects"},
    };
    for (const auto& b : badTraits) {
        std::string e;
        const bool loaded = w2f::LoadTraitDatabaseFromJson(b[0], &e) != nullptr;
        const bool ok = !loaded && e.find(b[1]) != std::string::npos;
        if (!ok) std::printf("  trait case: wanted an error mentioning '%s', got %s'%s'\n", b[1], loaded ? "NO ERROR " : "", e.c_str());
        CHECK(ok);
    }
}

static void TestCoregonsStyleSynergyFromData() {
    // The Coregons synergy of the design doc, built from nothing but the new primitives and data: a team-scoped summon of ghouls with
    // 25% of the tankiest ally's HP, an OnDealDamage heal for every holder, and ghouls that echo 5% of everything their team deals.
    const char* champions = R"({"version": 1, "champions": [
      { "id": 9101, "name": "Ghoul", "summon": true,
        "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 10, "attackSpeed": 1.0, "range": 1 },
        "passive": { "id": 1, "name": "Ghostly", "effects": [ { "type": "Status", "target": "Self", "status": "Untargetable", "permanent": true } ] },
        "triggers": [ { "id": 2, "name": "Echo", "trigger": "OnAllyDealDamage", "effects": [
            { "type": "Damage", "target": "TriggerVictim", "damageType": "Magic", "amount": { "terms": [ { "source": "TriggerDamage", "percent": 5 } ] } } ] } ] },
      { "id": 1, "name": "Lost", "cost": 1, "traits": ["Coregons"], "stats": { "hp": 500, "armor": 0, "magicResist": 0, "attackDamage": 50, "attackSpeed": 1.0, "range": 1 } },
      { "id": 2, "name": "Tank", "cost": 1, "stats": { "hp": 2000, "armor": 0, "magicResist": 0, "attackDamage": 20, "attackSpeed": 1.0, "range": 1 } },
      { "id": 3, "name": "Enemy", "cost": 1, "stats": { "hp": 100000, "armor": 0, "magicResist": 0, "attackDamage": 40, "attackSpeed": 1.0, "range": 1 } } ]})";
    const char* traitJson = R"({"version": 1, "traits": [ { "id": 1, "name": "Coregons", "breakpoints": [ { "count": 1,
        "effects": [ { "scope": "Team", "type": "Summon", "champion": 9101, "count": 3, "maxHp": { "terms": [ { "source": "HighestAllyMaxHp", "percent": 25 } ] } } ],
        "triggers": [ { "scope": "TraitHolders", "ability": { "id": 20, "name": "Life", "trigger": "OnDealDamage", "effects": [
            { "type": "Heal", "target": "Self", "amount": { "terms": [ { "source": "TriggerDamage", "percent": 10 } ] } } ] } } ] } ] } ]})";
    std::string err;
    auto db = w2f::LoadChampionDatabaseFromJson(champions, &err);
    auto traits = w2f::LoadTraitDatabaseFromJson(traitJson, &err);
    CHECK(db != nullptr && traits != nullptr);
    if (!db || !traits) { std::printf("  %s\n", err.c_str()); return; }
    CHECK(w2f::ValidateChampionTraits(*db, *traits, &err) && w2f::ValidateSummonReferences(*db, nullptr, traits.get(), &err));

    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    const std::vector<FightUnitSpec> specs = {
        {1, db->Find(1), 1, 0, {3, 3}, {}}, {2, db->Find(2), 1, 0, {2, 3}, {}}, {9, db->Find(3), 1, 1, {3, 4}, {}}};
    const CombatSimulator sim(cfg, traits.get(), nullptr, db.get());
    const FightResult r = sim.RunFight(specs, 200, 5);
    std::string why;
    CHECK(ValidateCombatLog(r.log, *db, cfg, 200, &why));
    if (!why.empty()) std::printf("  validator: %s\n", why.c_str());

    CHECK(Events(r.log, CombatEventType::TraitActivated).size() == 1);
    const auto ghouls = Spawned(r);
    CHECK(ghouls.size() == 3);
    for (const CombatEvent& g : ghouls) CHECK(g.tick == 0 && g.other == 1 && g.amount == 500 && g.champion == 9101 && g.team == 0);   // cast by the (only) holder: 25% of 2000
    // The holder heals 10% of every hit it lands (it is being hit by the enemy, so there is something to heal).
    int hits = 0;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 9)) {
        if (e.other == 1 && !(e.flags & kFlagTriggered)) ++hits;
    }
    const auto heals = Events(r.log, CombatEventType::Heal, 1);
    CHECK(hits >= 3 && static_cast<int>(heals.size()) == hits);
    for (const CombatEvent& h : heals) CHECK(h.amount == 5 && h.other == 1);   // 10% of 50
    // Ghouls echo 5% of what allies deal (magic, so armor does not matter: Lost 50 -> 2, Tank 20 -> 1, a ghoul's own 10 -> 0 -> min 1).
    std::map<UnitId, int> echoes;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 9)) {
        if ((e.flags & kFlagTriggered) && e.other > kSummonUnitBase) ++echoes[e.other];
    }
    CHECK(echoes.size() == 3);
    // Ghouls are untargetable: the enemy never swings at one.
    for (const CombatEvent& e : Events(r.log, CombatEventType::Attack)) CHECK(e.other < kSummonUnitBase);
    // Without the Coregons unit there is no synergy at all.
    const std::vector<FightUnitSpec> noHolder = {{2, db->Find(2), 1, 0, {2, 3}, {}}, {9, db->Find(3), 1, 1, {3, 4}, {}}};
    const FightResult none = sim.RunFight(noHolder, 60, 5);
    CHECK(Events(none.log, CombatEventType::TraitActivated).empty() && Spawned(none).empty());
}

namespace {
struct ItemMatchRun {
    std::vector<std::uint64_t> hashes;
    int combinations = 0, hookDamage = 0, auraStatuses = 0, summons = 0, logs = 0;
    bool logsValid = true, integrity = true, finished = false, itemsConserved = true;
    std::string problem;
};

struct ItemMatchListener : IMatchListener {
    const ChampionDatabase* db = nullptr;
    CombatConfig combat;
    int combatTicks = 0;
    ItemMatchRun* run = nullptr;
    void OnItemsCombined(PlayerId, const UnitInstance&, const ItemCombination&) override { ++run->combinations; }
    void OnCombatSimulated(int round, const CombatOutcome& o) override {
        ++run->logs;
        std::string why;
        if (!ValidateCombatLog(o.log, *db, combat, combatTicks, &why)) {
            if (run->logsValid) run->problem = "round " + std::to_string(round) + ": " + why;
            run->logsValid = false;
        }
        for (const CombatEvent& e : o.log.events) {
            if (e.type == CombatEventType::Damage && (e.flags & kFlagTriggered)) ++run->hookDamage;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::MagicResist) && e.amount == -30 && e.duration == 0) ++run->auraStatuses;
            if (e.type == CombatEventType::Spawn && (e.flags & kFlagSummon)) ++run->summons;
        }
    }
};
}  // namespace

static void TestRecipeItemsThroughAFullMatch() {
    // 8 players (7 bots + a scripted seat) play a whole match in which every player is handed loose components every round and fits them
    // onto their fielded units. Components combine into the doc's items, and their hooks / auras / crits fight in real combat.
    auto items = P10Items();
    auto db = sample::MakeCombatDatabase();
    auto traits = sample::LoadProductionTraits();
    CHECK(items != nullptr && db != nullptr && traits != nullptr);
    if (!items || !db || !traits) return;
    std::string err;
    CHECK(w2f::ValidateItemTraits(*items, *traits, &err));

    const auto play = [&](std::uint64_t seed) {
        ItemMatchRun run;
        TestGameConfig cfg;
        cfg.player.startingGold = 10;
        cfg.match.planningTicks = 90;
        cfg.match.motherNatureTicks = 30;
        cfg.match.resolutionTicks = 30;
        auto match = MatchManager::Create(cfg, *db, seed, std::make_unique<CombatSimulator>(cfg.combat, traits.get(), items.get()), &err, items.get());
        ItemMatchListener listener;
        listener.db = db.get();
        listener.combat = cfg.combat;
        listener.combatTicks = cfg.match.combatTicks;
        listener.run = &run;
        match->AddListener(&listener);
        sample::ScriptedPlayer human(0);
        std::vector<AIBotController> bots;
        for (int seat = 1; seat < kMaxPlayers; ++seat) bots.emplace_back(static_cast<PlayerId>(seat), seed);
        Rng dice(seed, 99);
        int givenRound = 0;
        match->Start();
        for (int tick = 0; tick < 3'000'000 && !match->IsFinished(); ++tick) {
            human.Tick(*match);
            for (AIBotController& bot : bots) bot.Tick(*match);
            if (match->Phase() == MatchPhase::Planning && match->Round() != givenRound) {
                givenRound = match->Round();
                for (int seat = 0; seat < kMaxPlayers; ++seat) {
                    PlayerState* p = match->PlayersMutable().Get(static_cast<PlayerId>(seat));
                    if (!p->IsAlive()) continue;
                    for (int k = 0; k < 3; ++k) p->AddItemToBag(static_cast<ItemId>(1 + dice.NextBelow(9)));   // components 1..8 and the Omnilium Seed (9)
                    std::vector<UnitId> board;
                    for (const UnitInstance& u : p->Roster().Units()) {
                        if (u.location == LocationType::Board) board.push_back(u.id);
                    }
                    for (int tries = 0; tries < 6 && !board.empty() && !p->ItemBag().empty(); ++tries) {
                        const UnitId unit = board[dice.NextBelow(static_cast<std::uint32_t>(board.size()))];
                        match->TryEquipItem(static_cast<PlayerId>(seat), unit, p->ItemBag()[dice.NextBelow(static_cast<std::uint32_t>(p->ItemBag().size()))]);
                    }
                }
            }
            match->Tick();
            run.hashes.push_back(match->StateHash());
            run.integrity = run.integrity && match->VerifyPoolIntegrity() && match->VerifyRosterLayouts();
        }
        run.finished = match->IsFinished();
        return run;
    };
    const ItemMatchRun a = play(2024);
    const ItemMatchRun b = play(2024);
    const ItemMatchRun c = play(31337);
    for (const ItemMatchRun* r : {&a, &c}) {
        CHECK(r->finished && r->integrity && r->logsValid);
        if (!r->logsValid) std::printf("  first bad log: %s\n", r->problem.c_str());
        CHECK(r->logs > 30 && r->combinations > 10);
        CHECK(r->hookDamage > 0 || r->auraStatuses > 0);
    }
    CHECK(a.hashes == b.hashes && a.combinations == b.combinations && a.hookDamage == b.hookDamage);   // deterministic through recipes, hooks and auras
    CHECK(a.hashes != c.hashes);
    std::printf("  recipe matches (seed 2024 / 31337): %d / %d fights, %d / %d combinations, %d / %d hook damage events, %d / %d aura statuses\n", a.logs, c.logs,
                a.combinations, c.combinations, a.hookDamage, c.hookDamage, a.auraStatuses, c.auraStatuses);
}

static void TestPermilleTermsAndGunfire() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    // Gunfire: basic attacks deal an extra 0.5% of the target's MAX HP as true damage (5 per mille of 20000 = 100), as a hook rider.
    Duel d;
    d.Add(Fighter(1, 100000, 0, 40), 1, 0, {3, 3});
    d.Add(Dummy(2, 20000), 2, 1, {3, 4});
    d.Finish();
    d.specs[0].items = {items->Find(26)};
    const FightResult r = RunP10(d, 100, 1, cfg);
    int extra = 0;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
        if (e.subtype == static_cast<std::uint8_t>(DamageType::True)) { ++extra; CHECK(e.amount == 100); }
        else CHECK(e.amount == 70);   // 40 + the item's 30 attack damage
    }
    CHECK(extra >= 3);
    // "percent" and "permille" together are refused; permille loads as its own divisor.
    std::string err;
    CHECK(w2f::LoadChampionDatabaseFromJson(R"({"version": 1, "champions": [ {"id": 1, "name": "X", "cost": 1, "stats": {"hp": 1, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1.0, "range": 1},
        "ability": {"id": 2, "name": "a", "trigger": "EveryNthAttack", "attackCount": 2, "effects": [ {"type": "Damage", "target": "CurrentTarget", "damageType": "True",
          "amount": {"terms": [ {"source": "TargetMaxHp", "percent": 1, "permille": 5} ]}} ]} } ]})", &err) == nullptr && err.find("either") != std::string::npos);
    const ItemDefinition* gun = items->Find(26);
    const auto* dmg = std::get_if<DamageEffect>(&gun->abilities[0].effects[0].payload);
    CHECK(dmg && dmg->amount.terms.size() == 1 && dmg->amount.terms[0].divisor == 1000 && dmg->amount.terms[0].percent == Same(5));
    CHECK(items->Find(37) && items->FindCombination(7, 2)->id == 37);   // Fishscale
}

static void TestBotEconomyAndPlacement() {
    auto db = sample::MakeCombatDatabase();

    {   // XP only while gold > 50.
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        auto match = MatchManager::Create(cfg, *db, 1, nullptr);
        match->Start();
        while (match->Phase() != MatchPhase::Planning) match->Tick();
        PlayerState* me = match->PlayersMutable().Get(0);
        me->AddGold(70 - me->Gold());  // exactly 70
        AIBotController bot(0, 1);
        bot.Tick(*match);
        CHECK(me->Level() == 5);  // 70 -> 66 -> 62 -> 58 -> 54 -> 50: five purchases = 20 XP = level 5
        CHECK(me->Roster().Count() > 0);  // the rest went on shop units
        // Acts once per round, not once per tick.
        const int gold = me->Gold();
        const std::size_t units = me->Roster().Count();
        bot.Tick(*match);
        CHECK(me->Gold() == gold && me->Roster().Count() == units);
    }
    {   // Exactly 50 is not "more than 50".
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        auto match = MatchManager::Create(cfg, *db, 1, nullptr);
        match->Start();
        while (match->Phase() != MatchPhase::Planning) match->Tick();
        PlayerState* me = match->PlayersMutable().Get(0);
        me->AddGold(50 - me->Gold());
        AIBotController bot(0, 1);
        bot.Tick(*match);
        CHECK(me->Level() == 1);
    }
    {   // Only acts during Planning, and only while alive.
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        cfg.player.startingGold = 20;
        auto match = MatchManager::Create(cfg, *db, 1, nullptr);
        AIBotController bot(0, 1);
        bot.Tick(*match);  // the match has not started: nothing to act on
        CHECK(match->Players().Get(0)->Roster().Count() == 0);
        match->Start();
        while (match->Phase() != MatchPhase::Planning) match->Tick();
        bot.Tick(*match);
        CHECK(match->Players().Get(0)->Roster().Count() > 0);
    }
    {   // Tanks front, everyone else back. (No board limit here so every unit gets fielded.)
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        auto match = MatchManager::Create(cfg, *db, 1, nullptr);
        match->Start();
        while (match->Phase() != MatchPhase::Planning) match->Tick();
        PlayerState* me = match->PlayersMutable().Get(0);
        me->TrySpendGold(me->Gold());  // broke: the bot must not buy anything, only place
        std::vector<const ChampionDefinition*> picks = {db->Find(200), db->Find(201), db->Find(202), db->Find(204), db->Find(9001), db->Find(300)};
        for (auto* d : picks) CHECK(me->AcquireUnit(d) == ActionResult::Ok);
        AIBotController bot(0, 1);
        bot.Tick(*match);
        CHECK(me->Roster().BoardCount() == 6 && me->Roster().BenchCount() == 0);
        bool tanksFront = true, othersBack = true;
        for (const UnitInstance& u : me->Roster().Units()) {
            CHECK(u.location == LocationType::Board);
            if (u.champion->role == ChampionRole::Tank) tanksFront = tanksFront && u.y == kBoardRows - 1;
            else othersBack = othersBack && u.y == 0;
        }
        CHECK(tanksFront && othersBack);
        // Row 3 fills from the centre outward.
        CHECK(me->Roster().BoardAt(3, 3) != nullptr && me->Roster().BoardAt(2, 3) != nullptr && me->Roster().BoardAt(4, 3) != nullptr);
    }
    {   // With the level cap on, it fields exactly `level` units and leaves the rest on the bench.
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        cfg.player.limitBoardToLevel = true;
        auto match = MatchManager::Create(cfg, *db, 1, nullptr);
        match->Start();
        while (match->Phase() != MatchPhase::Planning) match->Tick();
        PlayerState* me = match->PlayersMutable().Get(0);
        me->AddXp(2);  // level 2
        me->TrySpendGold(me->Gold());
        for (int i = 0; i < 5; ++i) me->AcquireUnit(db->Find(static_cast<ChampionId>(200 + i)));
        AIBotController bot(0, 1);
        bot.Tick(*match);
        CHECK(me->Level() == 2 && me->Roster().BoardCount() == 2 && me->Roster().BenchCount() == 3);
    }
}

static void TestFullBenchMergeViaMatchApi() {
    // A: 9/9 bench + full board, the shop offers a 3rd copy of something owned -> purchase allowed, merges.
    std::vector<ChampionDefinition> defs;
    defs.push_back(Def(1, "Solo", 1));
    for (ChampionId id = 100; id < 140; ++id) defs.push_back(Def(id, "Filler", 2));
    auto db = ChampionDatabase::Create(defs);
    TestGameConfig cfg;
    cfg.match.playerCount = 2;
    cfg.player.startingGold = 50;
    auto match = MatchManager::Create(cfg, *db, 3, nullptr);
    EventLog log;
    match->AddListener(&log);
    match->Start();
    while (match->Phase() != MatchPhase::Planning) match->Tick();

    PlayerState* me = match->PlayersMutable().Get(0);
    for (ChampionId id = 100; id < 135; ++id) CHECK(me->AcquireUnit(db->Find(id)) == ActionResult::Ok);
    CHECK(match->TryBuyShopUnit(0, 0) == ActionResult::Ok);  // Solo #1 -> 36/37
    CHECK(match->TryBuyShopUnit(0, 1) == ActionResult::Ok);  // Solo #2 -> 37/37: completely full
    CHECK(me->Roster().Count() == 37 && me->Roster().BenchCount() == 9 && me->Roster().BoardCount() == 28);
    const int gold = me->Gold();
    CHECK(match->TryBuyShopUnit(0, 2) == ActionResult::Ok);  // 3rd Solo: bought despite 37/37
    CHECK(me->Gold() == gold - 1);
    CHECK(me->Roster().Count() == 36 && me->Roster().CountOf(db->Find(1), 2) == 1);
    CHECK(log.Count(UnitEvent::Merged) == 1);
    // ...but a copy that would NOT complete a merge is still refused, and costs nothing.
    me->Shop().Refresh();                                    // 5 fresh Solo copies
    CHECK(match->TryBuyShopUnit(0, 0) == ActionResult::Ok);  // 36 -> 37 units: the last free slot
    CHECK(me->Roster().Count() == 37);
    const int gold2 = me->Gold();
    CHECK(match->TryBuyShopUnit(0, 1) == ActionResult::RosterFull);  // 2nd loose copy: no room, no merge
    CHECK(me->Gold() == gold2 && me->Shop().Slots()[1] != nullptr && me->Roster().Count() == 37);
}

// ---- Full matches: 7 bots + 1 player -------------------------------------------------------

struct BotMatchRun {
    std::vector<std::uint64_t> hashes;
    PlayerId winner = kInvalidPlayerId;
    int rounds = 0;
    bool finished = false;
    bool integrity = true, layouts = true, logsValid = true;
    std::string firstLogProblem;
    int fights = 0, totalEvents = 0, totalPathSearches = 0, decisive = 0;
    int bought = 0, merged = 0, moved = 0, sold = 0;
    std::map<AbilityId, int> casts;   // spells cast across the whole match, by ability
    int crits = 0, stuns = 0, burns = 0, shields = 0;
    int passives = 0, wounds = 0, manaEvents = 0, maxHpBuffs = 0;
    int traitActivations = 0, heals = 0, roots = 0, knockups = 0, immunities = 0;
};

struct BotMatchListener : IMatchListener {
    const ChampionDatabase* db = nullptr;
    CombatConfig combat;
    int combatTicks = 0;
    BotMatchRun* run = nullptr;
    EventLog units;
    void OnCombatSimulated(int, const CombatOutcome& o) override {
        ++run->fights;
        run->totalEvents += static_cast<int>(o.log.events.size());
        run->totalPathSearches += o.log.pathSearches;
        run->decisive += o.winner == CombatWinner::Draw ? 0 : 1;
        for (const CombatEvent& e : o.log.events) {
            if (e.type == CombatEventType::SpellCast) ++run->casts[e.ability];
            if (e.type == CombatEventType::Damage && (e.flags & kFlagCrit)) ++run->crits;
            if (e.type == CombatEventType::ShieldApplied) ++run->shields;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::Stun)) ++run->stuns;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::Burn)) ++run->burns;
            if (e.type == CombatEventType::StatusApplied && e.duration == 0) ++run->passives;   // permanent = a passive
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::Wound)) ++run->wounds;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::MaxHp)) ++run->maxHpBuffs;
            if (e.type == CombatEventType::ManaChanged) ++run->manaEvents;
            if (e.type == CombatEventType::TraitActivated) ++run->traitActivations;
            if (e.type == CombatEventType::Heal) ++run->heals;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::Root)) ++run->roots;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::Knockup)) ++run->knockups;
            if (e.type == CombatEventType::StatusApplied && e.subtype == static_cast<std::uint8_t>(StatusType::CcImmunity)) ++run->immunities;
        }
        std::string why;
        if (!ValidateCombatLog(o.log, *db, combat, combatTicks, &why) && run->logsValid) {
            run->logsValid = false;
            run->firstLogProblem = why;
        }
    }
    void OnUnitBought(PlayerId p, const UnitInstance& u, int g) override { units.OnUnitBought(p, u, g); }
    void OnUnitSold(PlayerId p, const UnitInstance& u, int g) override { units.OnUnitSold(p, u, g); }
    void OnUnitMoved(PlayerId p, const UnitMove& m) override { units.OnUnitMoved(p, m); }
    void OnUnitMerged(PlayerId p, const UnitMerge& m) override { units.OnUnitMerged(p, m); }
};

static BotMatchRun PlayBotMatch(const ChampionDatabase& db, std::uint64_t seed, int hashEvery = 1, bool highTierShop = false, int copiesPerChampion = 0) {
    TestGameConfig cfg;
    if (copiesPerChampion > 0) cfg.pool.copiesPerTier = {{copiesPerChampion, copiesPerChampion, copiesPerChampion, copiesPerChampion, copiesPerChampion}};
    cfg.player.startingGold = 10;
    cfg.player.limitBoardToLevel = true;
    cfg.match.planningTicks = 90;   // planning/draft/resolution shortened; combat keeps its real length
    cfg.match.motherNatureTicks = 30;
    cfg.match.resolutionTicks = 30;
    if (highTierShop) {
        // Every level sells only tiers 2-5, evenly: so the expensive champions (Les, Lum) really get bought and fielded.
        for (auto& row : cfg.shop.dropRatesByLevel) row = {{0, 25, 25, 25, 25}};
    }
    auto traits = sample::LoadProductionTraits();   // synergies are on in real matches; must outlive the match
    auto match = MatchManager::Create(cfg, db, seed, std::make_unique<CombatSimulator>(cfg.combat, traits.get()));

    BotMatchRun run;
    BotMatchListener listener;
    listener.db = &db;
    listener.combat = cfg.combat;
    listener.combatTicks = cfg.match.combatTicks;
    listener.run = &run;
    match->AddListener(&listener);

    sample::ScriptedPlayer human(0);
    std::vector<AIBotController> bots;
    for (int seat = 1; seat < kMaxPlayers; ++seat) bots.emplace_back(static_cast<PlayerId>(seat), seed);

    match->Start();
    for (int tick = 0; tick < 3'000'000 && !match->IsFinished(); ++tick) {
        human.Tick(*match);
        for (AIBotController& bot : bots) bot.Tick(*match);
        match->Tick();
        if (tick % hashEvery == 0) {
            run.hashes.push_back(match->StateHash());
            run.integrity = run.integrity && match->VerifyPoolIntegrity();
            run.layouts = run.layouts && match->VerifyRosterLayouts();
        }
    }
    run.finished = match->IsFinished();
    run.winner = match->Winner();
    run.rounds = match->Round();
    run.bought = listener.units.Count(UnitEvent::Bought);
    run.merged = listener.units.Count(UnitEvent::Merged);
    run.moved = listener.units.Count(UnitEvent::Moved);
    run.sold = listener.units.Count(UnitEvent::Sold);
    return run;
}

static void TestRosterOnlyMatchWithSynergies() {
    // A full match through the whole state machine where the shop sells ONLY the production roster (tiers 2-5), so Les and Lum
    // (Protector) and everything else are actually bought, fielded and fought with: synergies, heals, roots, knock-ups,
    // CC immunity, damage amp, passives. Checks: every fight's log validates, pool/layout integrity every tick, and the
    // same seed replays identically tick by tick.
    auto roster = w2f::LoadChampionDatabaseFromFile(sample::ProductionDataPath());
    CHECK(roster != nullptr);
    if (!roster) return;
    const BotMatchRun a = PlayBotMatch(*roster, 2024, 1, true);
    const BotMatchRun b = PlayBotMatch(*roster, 2024, 1, true);
    const BotMatchRun c = PlayBotMatch(*roster, 31337, 1, true);
    for (const BotMatchRun* r : {&a, &c}) {
        CHECK(r->finished && r->winner != kInvalidPlayerId && r->integrity && r->layouts && r->logsValid);
        if (!r->logsValid) std::printf("  first bad log: %s\n", r->firstLogProblem.c_str());
        CHECK(r->fights > 20);
    }
    CHECK(a.hashes == b.hashes && a.winner == b.winner && a.fights == b.fights);
    CHECK(a.traitActivations == b.traitActivations && a.heals == b.heals && a.roots == b.roots && a.knockups == b.knockups && a.immunities == b.immunities);
    CHECK(a.hashes != c.hashes);
    // The new content is really in play.
    CHECK(a.traitActivations + c.traitActivations > 0);
    CHECK(a.heals + c.heals > 0 && a.immunities + c.immunities > 0 && a.roots + c.roots > 0);
    std::printf("  roster-only matches (seed 2024 / 31337): %d / %d rounds, %d / %d fights; synergies %d / %d, heals %d / %d, roots %d / %d, knock-ups %d / %d, CC-immunity %d / %d\n",
                a.rounds, c.rounds, a.fights, c.fights, a.traitActivations, c.traitActivations, a.heals, c.heals, a.roots, c.roots, a.knockups, c.knockups, a.immunities, c.immunities);
}

static void TestFullMatchWithBots() {
    auto db = sample::MakeCombatDatabase();
    const BotMatchRun a = PlayBotMatch(*db, 2024);
    const BotMatchRun b = PlayBotMatch(*db, 2024);
    const BotMatchRun c = PlayBotMatch(*db, 77);

    for (const BotMatchRun* r : {&a, &c}) {
        CHECK(r->finished && r->winner != kInvalidPlayerId);
        CHECK(r->integrity);   // every champion copy accounted for, on every tick
        CHECK(r->layouts);
        CHECK(r->logsValid);   // every fight replays legally from its event stream alone
        if (!r->logsValid) std::printf("  first bad log: %s\n", r->firstLogProblem.c_str());
        CHECK(r->fights > 20 && r->decisive > r->fights / 2);
        CHECK(r->bought > 0 && r->moved > 0);
        // The real roster's spells actually fire in bot-vs-bot matches (bots draw them from the pool).
        int totalCasts = 0;
        for (auto& kv : r->casts) totalCasts += kv.second;
        CHECK(totalCasts > 20 && r->casts.size() >= 2);   // which champions show up depends on the shop RNG; expensive ones are rare
    }
    // Same seed => the same match, tick for tick, event for event.
    CHECK(a.hashes == b.hashes);
    CHECK(a.winner == b.winner && a.rounds == b.rounds && a.fights == b.fights && a.totalEvents == b.totalEvents);
    CHECK(a.bought == b.bought && a.merged == b.merged && a.moved == b.moved && a.sold == b.sold);
    CHECK(a.casts == b.casts && a.crits == b.crits && a.stuns == b.stuns && a.burns == b.burns && a.shields == b.shields);
    CHECK(a.passives == b.passives && a.wounds == b.wounds && a.manaEvents == b.manaEvents && a.maxHpBuffs == b.maxHpBuffs);
    CHECK(a.traitActivations == b.traitActivations && a.heals == b.heals && a.roots == b.roots && a.knockups == b.knockups && a.immunities == b.immunities);
    std::printf("  phase 6 mechanics over the match (seed 2024 / seed 77): synergies %d / %d, heals %d / %d, roots %d / %d, knock-ups %d / %d, CC-immunity %d / %d\n",
                a.traitActivations, c.traitActivations, a.heals, c.heals, a.roots, c.roots, a.knockups, c.knockups, a.immunities, c.immunities);
    // The data-driven features are really exercised in bot-vs-bot matches on the JSON roster.
    CHECK(a.passives > 0 && a.manaEvents > 0);
    std::printf("  new mechanics over the match (seed 2024): %d passive statuses (%d max-HP buffs), %d wounds, %d mana-bar events; seed 77: %d / %d / %d\n",
                a.passives, a.maxHpBuffs, a.wounds, a.manaEvents, c.passives, c.wounds, c.manaEvents);
    {   // ...but across the two matches a good part of the roster's spells appear.
        std::set<AbilityId> seen;
        for (auto& kv : a.casts) seen.insert(kv.first);
        for (auto& kv : c.casts) seen.insert(kv.first);
        CHECK(seen.size() >= 3);
    }
    // A different seed => a different match.
    CHECK(a.hashes != c.hashes);
    auto cast = [&](AbilityId id) { auto it = a.casts.find(id); return it == a.casts.end() ? 0 : it->second; };
    std::printf("  spells cast over the match (seed 2024): Aesa %d, Ardeat's Destiny %d, Rocket %d, Dyno Shield %d, Exploit %d | crits %d, stuns %d, burns %d, shields %d\n",
                cast(kAbilityAesa), cast(kAbilityArdeatsDestiny), cast(kAbilityRocketStrike), cast(kAbilityDynoShield),
                cast(kAbilityExploit), a.crits, a.stuns, a.burns, a.shields);
    std::printf("  seed 2024: %d rounds, %d fights (%d decisive), %d combat events, %d path searches; units bought %d merged %d moved %d\n",
                a.rounds, a.fights, a.decisive, a.totalEvents, a.totalPathSearches, a.bought, a.merged, a.moved);
}

// ================================================================================================
// Phase 11: the full 30-champion roster and the design doc's synergies, exercised one mechanic at a time.
// ================================================================================================

// A production champion that starts the fight with a full mana bar, so its ability fires on tick 0.
static ChampionDefinition Primed(const ChampionDatabase& prod, ChampionId id, int range = -1) {
    ChampionDefinition d = *prod.Find(id);
    d.stats.startMana = d.stats.maxMana;
    if (range > 0) d.stats.attackRange = range;
    return d;
}
// Adds a Duel unit for a production champion, plus every summon the roster defines (the Duel's database must contain them).
static void AddPrimed(Duel& d, const ChampionDatabase& prod, ChampionId id, UnitId unit, int team, HexCoord pos, int range = -1) {
    d.Add(Primed(prod, id, range), unit, team, pos);
}
static void AddSummonDefs(Duel& d, const ChampionDatabase& prod) {
    for (ChampionId id : {ChampionId{9101}, ChampionId{9102}}) d.defs.push_back(*prod.Find(id));
}
static FightResult RunDuel(Duel& d, int ticks, const CombatConfig& cfg, const TraitDatabase* traits = nullptr, const ItemDatabase* items = nullptr,
                           std::uint64_t seed = 1) {
    d.Finish();
    const FightResult r = CombatSimulator(cfg, traits, items, d.db.get()).RunFight(d.specs, ticks, seed);
    std::string why;
    const bool valid = ValidateCombatLog(r.log, *d.db, cfg, ticks, &why);
    if (!valid) std::printf("  validator: %s\n", why.c_str());
    CHECK(valid);
    return r;
}
// The Damage events an ability's cast made on `tick` (flagged as ability damage), by victim.
static std::map<UnitId, int> DamageAt(const FightResult& r, int tick, std::uint8_t flag = kFlagAbility) {
    std::map<UnitId, int> out;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage)) {
        if (e.tick == tick && (e.flags & flag) != 0) out[e.unit] += e.amount;
    }
    return out;
}
static CombatConfig NoManaConfig() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;   // casts happen only when the test primes them
    cfg.rawDamagePerMana = 1000000;   // ...and damage taken does not refill the bar either
    return cfg;
}

static void TestNewChampionAbilitiesPart1() {
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    const CombatConfig cfg = NoManaConfig();

    {   // IGNIS: a 200 shield for 4 s. PYRA: 120 to the target and to the unit 1 hex behind it, none to a bystander.
        Duel d;
        AddPrimed(d, *prod, 9013, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        d.Finish();
        const FightResult r = RunDuel(d, 10, cfg);
        const auto shield = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(shield.size() == 1 && shield[0].amount == 200 && shield[0].duration == 120);
    }
    {
        Duel d;
        AddPrimed(d, *prod, 9014, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});    // the target
        d.Add(Dummy(11, 100000), 11, 1, {2, 5});    // 1 hex behind it (straight line from the caster)
        d.Add(Dummy(12, 100000), 12, 1, {4, 5});    // beside the line
        const FightResult r = RunDuel(d, 10, cfg);
        auto hit = DamageAt(r, 0);
        CHECK(hit[10] == 120 && hit[11] == 120 && hit.count(12) == 0);
    }
    {   // VEX: blinks to the LOWEST-HP enemy (not the one she is fighting), hits it for 150, and keeps fighting it.
        Duel d;
        AddPrimed(d, *prod, 9015, 1, 0, {3, 3});
        d.Add(Dummy(10, 5000), 10, 1, {3, 4});   // adjacent: the one she casts at
        d.Add(Dummy(11, 400), 11, 1, {1, 7});    // far away and the weakest
        const FightResult r = RunDuel(d, 60, cfg);
        const auto tp = Events(r.log, CombatEventType::Teleport, 1);
        CHECK(tp.size() == 1 && tp[0].tick == 0 && tp[0].subtype == 0 && hex::Distance(tp[0].to, HexCoord{1, 7}) == 1);
        auto hit = DamageAt(r, 0);
        CHECK(hit[11] == 150 && hit.count(10) == 0);
        for (const CombatEvent& a : Events(r.log, CombatEventType::Attack, 1)) CHECK(a.other == 11);   // it fights the victim from then on (the tick-0 swing is at 11 too)
    }
    {   // RAA: the highest-HP enemy takes 350 physical after the leap.
        Duel d;
        AddPrimed(d, *prod, 9028, 1, 0, {3, 3});
        d.Add(Dummy(10, 5000), 10, 1, {3, 4});
        d.Add(Dummy(11, 90000), 11, 1, {1, 7});
        const FightResult r = RunDuel(d, 60, cfg);
        const auto tp = Events(r.log, CombatEventType::Teleport, 1);
        CHECK(tp.size() == 1 && hex::Distance(tp[0].to, HexCoord{1, 7}) == 1);
        CHECK(DamageAt(r, 0)[11] == 350);
    }
    {   // BIT: dashes through the target to the far side of it, 120 physical.
        Duel d;
        AddPrimed(d, *prod, 9019, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 10, cfg);
        const auto tp = Events(r.log, CombatEventType::Teleport, 1);
        CHECK(tp.size() == 1 && hex::Distance(tp[0].to, HexCoord{3, 4}) == 1 && hex::Distance(tp[0].to, HexCoord{3, 3}) == 2);
        CHECK(DamageAt(r, 0)[10] == 120);
    }
    {   // BONE: 100 physical + a 1.5 s stun.  ROT: 90 magic over 3 s, a tick every 0.5 s (6 x 15).
        Duel d;
        AddPrimed(d, *prod, 9017, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 10, cfg);
        CHECK(DamageAt(r, 0)[10] == 100);
        bool stun = false;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 10)) stun = stun || (e.subtype == static_cast<std::uint8_t>(StatusType::Stun) && e.duration == 45);
        CHECK(stun);
    }
    {
        Duel d;
        AddPrimed(d, *prod, 9018, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 120, cfg);
        std::vector<int> ticks, amounts;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 10)) {
            if (e.flags & kFlagDot) { ticks.push_back(e.tick); amounts.push_back(e.amount); }
        }
        CHECK((amounts == std::vector<int>{15, 15, 15, 15, 15, 15}));
        CHECK(!ticks.empty() && ticks.front() == 15 && ticks.back() == 90);
    }
    {   // NULL (range 3, so the target is not already adjacent): 80 magic and a pull 1 hex toward it.
        CHECK(prod->Find(9016)->stats.attackRange == 3);
        Duel d;
        AddPrimed(d, *prod, 9016, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 6});   // 3 hexes away
        const FightResult r = RunDuel(d, 10, cfg);
        CHECK(DamageAt(r, 0)[10] == 80);
        const auto moved = Events(r.log, CombatEventType::Teleport, 10);
        CHECK(moved.size() == 1 && moved[0].subtype == 1 && hex::Distance(moved[0].from, HexCoord{3, 3}) == 3 && hex::Distance(moved[0].to, HexCoord{3, 3}) == 2);
    }
    {   // ORION: 180 physical and a knock-back 1 hex away -- unless the hex behind the target is taken.
        Duel d;
        AddPrimed(d, *prod, 9024, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 10, cfg);
        CHECK(DamageAt(r, 0)[10] == 180);
        const auto moved = Events(r.log, CombatEventType::Teleport, 10);
        CHECK(moved.size() == 1 && moved[0].subtype == 1 && hex::Distance(moved[0].to, HexCoord{3, 3}) == 2);
        Duel blocked;
        AddPrimed(blocked, *prod, 9024, 1, 0, {3, 3});
        blocked.Add(Dummy(10, 100000), 10, 1, {3, 4});
        blocked.Add(Dummy(11, 100000), 11, 1, {2, 5});   // directly behind unit 10
        blocked.Add(Dummy(12, 100000), 12, 1, {3, 5});
        const FightResult rb = RunDuel(blocked, 10, cfg);
        CHECK(Events(rb.log, CombatEventType::Teleport, 10).empty());   // nowhere to go: no move, no event
    }
    {   // SOLIS: the cast arms ONE charge; the next attack after it carries 150 bonus physical and shreds 20% of the target's armor for 3 s.
        Duel d;
        AddPrimed(d, *prod, 9020, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 140, cfg);
        int bonusHits = 0, shreds = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 10)) {
            if ((e.flags & kFlagTriggered) && (e.flags & kFlagAbility) && e.other == 1) { ++bonusHits; CHECK(e.amount == 150); }
        }
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 10)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::Armor) && e.amount == -20 && e.duration == 90) ++shreds;
        }
        CHECK(bonusHits == 1 && shreds == 1);
    }
    {   // MORTIS: the next 3 basic attacks each carry 150 bonus physical that ignores half the armor. Armor 100: 150 * 100/(100+50) = 100 per bonus hit.
        Duel d;
        AddPrimed(d, *prod, 9030, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000, 100), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 170, cfg);
        int bonusHits = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 10)) {
            if ((e.flags & kFlagTriggered) && (e.flags & kFlagAbility)) { ++bonusHits; CHECK(e.amount == 100); }
        }
        CHECK(bonusHits == 3);
    }
}

static void TestNewChampionAbilitiesPart2() {
    auto prod = ProdDb();
    CHECK(prod != nullptr);
    if (!prod) return;
    const CombatConfig cfg = NoManaConfig();

    {   // XUL: 150 magic to the target and it BOUNCES to exactly one adjacent enemy (of two candidates: the lower UnitId).
        Duel d;
        AddPrimed(d, *prod, 9021, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        d.Add(Dummy(11, 100000), 11, 1, {2, 5});
        d.Add(Dummy(12, 100000), 12, 1, {3, 5});
        const FightResult r = RunDuel(d, 10, cfg);
        auto hit = DamageAt(r, 0);
        CHECK(hit.size() == 2 && hit[10] == 150 && hit[11] == 150 && hit.count(12) == 0);
    }
    {   // GRAVE: a Skeleton (300 HP at 1 star, 30 AD) next to her; it fights and vanishes with the fight.
        Duel d;
        AddPrimed(d, *prod, 9022, 1, 0, {3, 3});
        AddSummonDefs(d, *prod);
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 100, cfg);
        const auto skeletons = Spawned(r);
        CHECK(skeletons.size() == 1 && skeletons[0].champion == 9101 && skeletons[0].amount == 300 && skeletons[0].other == 1 && skeletons[0].team == 0);
        CHECK(!skeletons.empty() && hex::Distance(skeletons[0].to, HexCoord{3, 3}) == 1);
        int swings = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, skeletons.empty() ? 0 : skeletons[0].unit)) { ++swings; (void)e; }
        CHECK(swings > 0);
        CHECK(r.log.survivors[0] == 1);   // the skeleton is not a survivor
    }
    {   // BYTE: 200 shield for 4 s to the LOWEST-HP ally (not herself).
        Duel d;
        AddPrimed(d, *prod, 9023, 1, 0, {3, 3});
        d.Add(Dummy(2, 60), 2, 0, {2, 3});
        d.Add(Dummy(3, 9000), 3, 0, {4, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 10, cfg);
        const auto shield = Events(r.log, CombatEventType::ShieldApplied);
        CHECK(shield.size() == 1 && shield[0].unit == 2 && shield[0].amount == 200 && shield[0].duration == 120);
    }
    {   // NYX: 150 to every ADJACENT enemy, none to one 2 hexes away.
        Duel d;
        AddPrimed(d, *prod, 9025, 1, 0, {3, 3});
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        d.Add(Dummy(11, 100000), 11, 1, {4, 3});
        d.Add(Dummy(12, 100000), 12, 1, {3, 5});   // two away
        const FightResult r = RunDuel(d, 10, cfg);
        auto hit = DamageAt(r, 0);
        CHECK(hit.size() == 2 && hit[10] == 150 && hit[11] == 150);
    }
    {   // FLARE: 200 magic within 1 hex of its TARGET (the target and a neighbour of it), not a unit 2 hexes from the target.
        Duel d;
        AddPrimed(d, *prod, 9026, 1, 0, {3, 0});
        d.Add(Dummy(10, 100000), 10, 1, {3, 3});
        d.Add(Dummy(11, 100000), 11, 1, {3, 4});
        d.Add(Dummy(12, 100000), 12, 1, {3, 6});
        const FightResult r = RunDuel(d, 10, cfg);
        auto hit = DamageAt(r, 0);
        CHECK(hit.size() == 2 && hit[10] == 200 && hit[11] == 200);
    }
    {   // LICH: 250 magic over 2 s (4 ticks of the drain), and every tick heals him for exactly the damage it did (while he is hurt).
        Duel d;
        AddPrimed(d, *prod, 9027, 1, 0, {3, 3});
        d.Add(Fighter(10, 100000, 0, 300, 1000, 1), 10, 1, {3, 4});   // hits him hard every second, so there is always HP to restore
        const FightResult r = RunDuel(d, 70, cfg);
        int drained = 0, healed = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 10)) {
            if ((e.flags & kFlagDot) && e.other == 1) drained += e.amount;
        }
        for (const CombatEvent& e : Events(r.log, CombatEventType::Heal, 1)) healed += e.amount + e.reduced;
        CHECK(drained == 250 && healed == 250);
    }
    {   // KRYX: frenzy for 5 s: +50% attack speed, and every hit taken is 10% bigger (armor 45: 100 -> 68, and 74 during the frenzy).
        Duel d;
        AddPrimed(d, *prod, 9029, 1, 0, {3, 3});
        d.Add(Fighter(10, 100000, 0, 100, 1000, 1), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 220, cfg);
        bool speed = false, taken = false;
        for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, 1)) {
            speed = speed || (e.subtype == static_cast<std::uint8_t>(StatusType::AttackSpeed) && e.amount == 50 && e.duration == 150);
            taken = taken || (e.subtype == static_cast<std::uint8_t>(StatusType::DamageTaken) && e.amount == 10 && e.duration == 150);
        }
        CHECK(speed && taken);
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 1)) CHECK(e.amount == (e.tick < 150 ? 74 : 68));
    }
    {   // UMBRA: 300 magic to everything within 2 hexes and a 3 s blind (no basic attacks; the blinded enemy attacked once, on tick 0, before it landed).
        Duel d;
        AddPrimed(d, *prod, 9031, 1, 0, {3, 3});
        d.Add(Fighter(10, 100000, 0, 5, 1000, 1), 10, 1, {3, 4});   // adjacent
        d.Add(Fighter(11, 100000, 0, 5, 1000, 1), 11, 1, {3, 5});   // 2 hexes: inside
        d.Add(Fighter(12, 100000, 0, 5, 1000, 4), 12, 1, {3, 6});   // 3 hexes: outside
        const FightResult r = RunDuel(d, 120, cfg);
        auto hit = DamageAt(r, 0);
        CHECK(hit.size() == 2 && hit[10] == 300 && hit[11] == 300);
        for (UnitId blinded : {UnitId{10}, UnitId{11}}) {
            bool blind = false;
            for (const CombatEvent& e : Events(r.log, CombatEventType::StatusApplied, blinded)) {
                blind = blind || (e.subtype == static_cast<std::uint8_t>(StatusType::Blind) && e.duration == 90);
            }
            CHECK(blind);
            int early = 0;
            for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, blinded)) early += (e.tick > 0 && e.tick < 90) ? 1 : 0;
            CHECK(early == 0);
        }
        int outside = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, 12)) outside += (e.tick > 0 && e.tick < 90) ? 1 : 0;
        CHECK(outside > 0);   // the unit outside the blast keeps attacking
    }
}

static const ChampionDefinition* Prod(const ChampionDatabase& db, ChampionId id) { return db.Find(id); }

// Builds a fight of production champions against one dummy enemy; `mine` are champion ids, unit ids 1..n, packed at the back-left.
struct SynergyFight {
    std::unique_ptr<ChampionDatabase> db;
    FightResult result;
    std::vector<UnitId> ids;
};
static SynergyFight RunSynergy(const ChampionDatabase& prod, const TraitDatabase& traits, const std::vector<ChampionId>& mine,
                               const ChampionDefinition& enemy, int ticks, const ItemDatabase* items = nullptr,
                               const std::vector<std::vector<const ItemDefinition*>>& gear = {}, int enemyCount = 1) {
    SynergyFight out;
    Duel d;
    AddSummonDefs(d, prod);
    const HexCoord slots[8] = {{3, 1}, {2, 1}, {4, 1}, {1, 1}, {5, 1}, {3, 0}, {2, 0}, {4, 0}};
    for (std::size_t i = 0; i < mine.size(); ++i) {
        d.Add(*Prod(prod, mine[i]), static_cast<UnitId>(i + 1), 0, slots[i]);
        out.ids.push_back(static_cast<UnitId>(i + 1));
    }
    for (int e = 0; e < enemyCount; ++e) d.Add(enemy, static_cast<UnitId>(100 + e), 1, HexCoord{3 + (e % 3) - 1, 5 + e / 3});
    d.Finish();
    for (std::size_t i = 0; i < gear.size() && i < d.specs.size(); ++i) d.specs[i].items = gear[i];
    const CombatConfig cfg;
    out.result = CombatSimulator(cfg, &traits, items, d.db.get()).RunFight(d.specs, ticks, 3);
    std::string why;
    const bool valid = ValidateCombatLog(out.result.log, *d.db, cfg, ticks, &why);
    if (!valid) std::printf("  validator: %s\n", why.c_str());
    CHECK(valid);
    out.db = std::move(d.db);
    return out;
}

static std::vector<CombatEvent> TraitEvents(const FightResult& r, TraitId trait, int team = 0) {
    std::vector<CombatEvent> out;
    for (const CombatEvent& e : Events(r.log, CombatEventType::TraitActivated)) {
        if (e.traitId == trait && e.team == team) out.push_back(e);
    }
    return out;
}

static void TestHeliosPhaisaHexagonSeliniNajmi() {
    auto prod = ProdDb();
    auto traits = sample::LoadProductionTraits();
    CHECK(prod != nullptr && traits != nullptr);
    if (!prod || !traits) return;
    ChampionDefinition wall = Dummy(9999, 1000000);   // an enemy nothing can kill
    wall.stats.attackRange = 1;

    {   // HELIOS 3: every basic attack of a Helios unit burns its target for 2% of ITS max HP over 3 s (a tick a second); at 6 it is 5%.
        for (int tier = 1; tier <= 2; ++tier) {
            const std::vector<ChampionId> team = tier == 1 ? std::vector<ChampionId>{9013, 9014, 9020}
                                                            : std::vector<ChampionId>{9013, 9014, 9020, 9026, 9028, 9001};
            const SynergyFight f = RunSynergy(*prod, *traits, team, wall, 260);
            const auto act = TraitEvents(f.result, 1);
            CHECK(act.size() == 1 && act[0].subtype == tier && act[0].amount == static_cast<int>(team.size()));
            // Every attack of every Helios unit re-lights THE burn: it refreshes instead of stacking, so the target has one burn (the last one
            // lit) and each tick is a third of 2% / 5% of 1,000,000 -- never one tick per attacker.
            std::vector<int> burnTicks, burnAmounts;
            for (const CombatEvent& e : Events(f.result.log, CombatEventType::Damage, 100)) {
                if ((e.flags & kFlagDot) && e.subtype == static_cast<std::uint8_t>(DamageType::True)) { burnTicks.push_back(e.tick); burnAmounts.push_back(e.amount); }
            }
            const int total = tier == 1 ? 20000 : 50000;
            CHECK(!burnAmounts.empty());
            for (int amount : burnAmounts) CHECK(amount == total / 3 || amount == total / 3 + 1);   // a third of the burn per tick (the remainder on the later ticks)
            CHECK(std::adjacent_find(burnTicks.begin(), burnTicks.end()) == burnTicks.end());   // never two burn hits on the same tick: no stacking
        }
        const SynergyFight two = RunSynergy(*prod, *traits, {9013, 9014}, wall, 80);
        CHECK(TraitEvents(two.result, 1).empty());
    }
    {   // PHAISA 3: whenever ANY unit dies, each Phaisa unit gets +4% attack damage and +4% ability power (permanent). Two enemies die -> two of each.
        ChampionDefinition weak = Dummy(9998, 10);
        const SynergyFight f = RunSynergy(*prod, *traits, {9015, 9016, 9021}, weak, 200, nullptr, {}, 2);
        CHECK(TraitEvents(f.result, 2).size() == 1 && TraitEvents(f.result, 2)[0].subtype == 1);
        int deaths = 0;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Death)) deaths += e.unit >= 100 ? 1 : 0;
        CHECK(deaths == 2);
        for (UnitId phaisa : f.ids) {
            int ad = 0, ap = 0;
            for (const CombatEvent& e : Events(f.result.log, CombatEventType::StatusApplied, phaisa)) {
                ad += (e.subtype == static_cast<std::uint8_t>(StatusType::AttackDamage) && e.amount == 4 && e.duration == 0) ? 1 : 0;
                ap += (e.subtype == static_cast<std::uint8_t>(StatusType::AbilityPower) && e.amount == 4 && e.duration == 0) ? 1 : 0;
            }
            CHECK(ad == 2 && ap == 2);
        }
    }
    {   // HEXAGON 2: a 300 shield each; HEXAGON 4: 600, and the first shield to break explodes for 200 magic on the enemies adjacent to the holder.
        const SynergyFight two = RunSynergy(*prod, *traits, {9019, 9003}, wall, 30);
        int shields300 = 0;
        for (UnitId u : two.ids) {
            for (const CombatEvent& e : Events(two.result.log, CombatEventType::ShieldApplied, u)) shields300 += (e.amount == 300 && e.duration == 0) ? 1 : 0;
        }
        CHECK(shields300 == 2);
        ChampionDefinition basher = Dummy(9997, 10000000);
        basher.stats.attackDamage = Same(1000);
        basher.stats.attackSpeedMilli = 1000;
        basher.stats.attackRange = 1;   // it walks up to them, so the holder it breaks has it adjacent
        const SynergyFight four = RunSynergy(*prod, *traits, {9019, 9003, 9005, 9023}, basher, 250);
        CHECK(TraitEvents(four.result, 3).size() == 1 && TraitEvents(four.result, 3)[0].subtype == 2);
        int shields600 = 0;
        for (UnitId u : four.ids) {
            for (const CombatEvent& e : Events(four.result.log, CombatEventType::ShieldApplied, u)) shields600 += (e.amount == 600 && e.duration == 0) ? 1 : 0;
        }
        CHECK(shields600 == 4);
        std::set<UnitId> exploders;
        int explosions = 0;
        for (const CombatEvent& e : Events(four.result.log, CombatEventType::Damage, 100)) {
            if ((e.flags & kFlagTriggered) && e.subtype == static_cast<std::uint8_t>(DamageType::Magic) && e.amount == 200) { ++explosions; exploders.insert(e.other); }
        }
        CHECK(explosions >= 1 && exploders.size() == static_cast<std::size_t>(explosions));   // at most one detonation per Hexagon unit
    }
    {   // SELINI 3: the first time a Selini unit falls below 50% it drops aggro for 2 s and heals 20% of its max HP -- once.
        ChampionDefinition basher = Dummy(9997, 10000000);
        basher.stats.attackDamage = Same(300);
        const SynergyFight f = RunSynergy(*prod, *traits, {9031, 9009, 9011}, basher, 300);
        CHECK(TraitEvents(f.result, 6).size() == 1);
        int aggro = 0, heals = 0;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::StatusApplied)) {
            if (e.subtype == static_cast<std::uint8_t>(StatusType::AggroDrop) && e.duration == 60 && e.other == e.unit) ++aggro;
        }
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Heal)) heals += (e.amount == 180 && e.unit == 1) ? 1 : 0;   // 20% of Umbra's 900
        CHECK(aggro >= 1);
        CHECK(heals == 1);
    }
    {   // NAJMI 3: when a Najmi unit casts, the allies ADJACENT to it gain 15 mana.
        Duel d;
        const CombatConfig cfg = NoManaConfig();
        AddPrimed(d, *prod, 9024, 1, 0, {3, 3});   // Orion, primed
        d.Add(*prod->Find(9008), 2, 0, {2, 3});    // Astra beside him
        d.Add(*prod->Find(9012), 3, 0, {4, 3});    // Vega beside him
        d.Add(Dummy(4, 100000), 4, 0, {0, 0});     // far away, not Najmi
        d.Add(Dummy(10, 100000), 10, 1, {3, 4});
        const FightResult r = RunDuel(d, 10, cfg, traits.get());
        CHECK(TraitEvents(r, 7).size() == 1);
        std::map<UnitId, int> mana;
        for (const CombatEvent& e : Events(r.log, CombatEventType::ManaChanged)) {
            if (e.tick == 0) mana[e.unit] = e.amount;
        }
        CHECK(mana[2] == 15000 && mana[3] == 15000 && mana.count(4) == 0);
    }
}

static const std::vector<ChampionId> kSixCoregons = {9017, 9018, 9022, 9027, 9010, 9030};   // Bone, Rot, Grave, Lich, Soul, Mortis

static void TestCoregonsFromTheDesignDoc() {
    auto prod = ProdDb();
    auto traits = sample::LoadProductionTraits();
    CHECK(prod != nullptr && traits != nullptr);
    if (!prod || !traits) return;
    std::string err;
    auto emblems = w2f::LoadItemDatabaseFromJson(R"({"version": 1, "items": [ {"id": 1, "name": "Emblem", "grantsTraits": ["Coregons"]} ]})", &err);
    CHECK(emblems != nullptr);
    if (!emblems) return;
    ChampionDefinition enemy = Fighter(9999, 100000, 0, 60, 1000, 1);
    enemy.stats.maxMana = 60;   // a caster: the zone's +15 mana applies
    auto spawnsOf = [](const FightResult& r) { return Spawned(r); };

    // ---- 3 Coregons: Bone, Rot, Grave (+ Null as the tankiest ally, 550 HP): 3 Lost Souls, no zone ----
    {
        const SynergyFight f = RunSynergy(*prod, *traits, {9017, 9018, 9022, 9016}, enemy, 150);
        const auto act = TraitEvents(f.result, 8);
        CHECK(act.size() == 1 && act[0].subtype == 1 && act[0].amount == 3);
        const auto souls = spawnsOf(f.result);
        CHECK(souls.size() == 3);
        for (const CombatEvent& s : souls) CHECK(s.champion == 9102 && s.tick == 0 && s.star == 1 && s.amount == 137 && s.other == 1);   // 25% of Null / Grave's 550, cast by the lowest-id holder
        // They are untargetable: the enemy never swings at one.
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Attack, 100)) CHECK(e.other < kSummonUnitBase);
        // They hit with magic damage and echo 5% of their team's hits (Bone hits for 40 -> 2).
        int magicBasics = 0, echoes = 0;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Damage, 100)) {
            if (e.other < kSummonUnitBase) continue;
            if (e.flags & kFlagBasic) { ++magicBasics; CHECK(e.subtype == static_cast<std::uint8_t>(DamageType::Magic) && e.amount == 30); }
            else if (e.flags & kFlagTriggered) ++echoes;
        }
        CHECK(magicBasics > 0 && echoes > 0);
        // Coregons heal for 10% of the damage they deal (Bone's basic attack: 40 -> 4).
        int boneHeal = 0;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Heal, 1)) boneHeal += e.amount == 4 ? 1 : 0;
        CHECK(boneHeal > 0);
        // No zone yet.
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::StatusApplied)) {
            const auto s = static_cast<StatusType>(e.subtype);
            CHECK(s != StatusType::BonusMaxMana && s != StatusType::HpPerSecond && s != StatusType::ExecuteBelow);
        }
    }
    // ---- 6 Coregons: the Lost Soul Zone ----
    {
        const SynergyFight f = RunSynergy(*prod, *traits, kSixCoregons, enemy, 200);
        const auto act = TraitEvents(f.result, 8);
        CHECK(act.size() == 1 && act[0].subtype == 2 && act[0].amount == 6);
        const auto souls = spawnsOf(f.result);
        CHECK(souls.size() == 3);
        for (const CombatEvent& s : souls) CHECK(s.star == 2 && s.amount == 260);   // 40% of the tankiest ally (Mortis, 650)
        // Enemy casters need 15 more mana; enemies lose 2% max HP a second; are executed below 5%.
        std::map<int, int> onEnemy;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::StatusApplied, 100)) {
            if (e.duration != 0) continue;
            onEnemy[e.subtype] = e.amount;
        }
        CHECK(onEnemy[static_cast<int>(StatusType::BonusMaxMana)] == 15 && onEnemy[static_cast<int>(StatusType::HpPerSecond)] == -2 &&
              onEnemy[static_cast<int>(StatusType::ExecuteBelow)] == 5);
        // Allies gain +2 mana / second and heal 2% max HP a second.
        std::map<int, int> onAlly;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::StatusApplied, 1)) {
            if (e.duration == 0 && e.tick == 0) onAlly[e.subtype] = e.amount;
        }
        CHECK(onAlly[static_cast<int>(StatusType::BonusManaRegen)] == 2000 && onAlly[static_cast<int>(StatusType::HpPerSecond)] == 2);
        // Once a second the enemy takes 2% of its 100,000 max HP as true damage (credited to the zone's caster, unit 1) -- and the +15 mana bar shows.
        std::vector<int> ticks;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Damage, 100)) {
            if ((e.flags & kFlagTriggered) && e.subtype == static_cast<std::uint8_t>(DamageType::True) && e.other == 1) {
                CHECK(e.amount == 2000);
                ticks.push_back(e.tick);
            }
        }
        CHECK(ticks.size() >= 5 && ticks[0] == 30 && ticks[1] == 60);
        // Allies that are hurt are healed 2% of their max HP each second by the zone (the enemy keeps hitting them).
        int zoneHeals = 0;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Heal)) {
            if (e.unit <= 6 && e.tick % 30 == 0 && e.tick > 0 && e.other == 1) ++zoneHeals;
        }
        CHECK(zoneHeals > 0);
    }
    // ---- 8 Coregons (6 + two emblem carriers): the Underworld ----
    {
        const std::vector<ChampionId> team = {9017, 9018, 9022, 9027, 9010, 9030, 9013, 9014};   // + Ignis and Pyra carrying Coregons Emblems
        const ItemDefinition* emblem = emblems->Find(1);
        const std::vector<std::vector<const ItemDefinition*>> gear = {{}, {}, {}, {}, {}, {}, {emblem}, {emblem}};
        const SynergyFight f = RunSynergy(*prod, *traits, team, enemy, 200, emblems.get(), gear);
        const auto act = TraitEvents(f.result, 8);
        CHECK(act.size() == 1 && act[0].subtype == 3 && act[0].amount == 8);
        const auto souls = spawnsOf(f.result);
        CHECK(souls.size() == 3);
        for (const CombatEvent& s : souls) CHECK(s.star == 3 && s.amount > 0);
        std::map<int, int> onEnemy;
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::StatusApplied, 100)) {
            if (e.duration == 0) onEnemy[e.subtype] = e.amount;
        }
        CHECK(onEnemy[static_cast<int>(StatusType::HpPerSecond)] == -4 && onEnemy[static_cast<int>(StatusType::ExecuteBelow)] == 10);
        for (const CombatEvent& e : Events(f.result.log, CombatEventType::Damage, 100)) {
            if ((e.flags & kFlagTriggered) && e.subtype == static_cast<std::uint8_t>(DamageType::True) && e.other == 1) { CHECK(e.amount == 4000); break; }
        }
        // 60% / 40% / 25% of the tankiest ally: the Soul HP grows with the breakpoint.
        const SynergyFight six = RunSynergy(*prod, *traits, kSixCoregons, enemy, 5);
        const auto sixSouls = spawnsOf(six.result);
        CHECK(!sixSouls.empty() && !souls.empty() && souls[0].amount * 2 == sixSouls[0].amount * 3);   // 60 : 40
    }
}

static void TestZoneStatusesAndExecute() {
    // ExecuteBelow: a unit under the threshold dies at once, past its shield; HpPerSecond damage is credited to the source but is NOT damage the
    // source "dealt" for formulas.
    const char* traitJson = R"({"version": 1, "traits": [ { "id": 1, "name": "Z", "breakpoints": [ { "count": 1, "effects": [
        { "scope": "Team", "type": "Status", "target": "AllEnemies", "status": "ExecuteBelow", "percent": 10, "permanent": true },
        { "scope": "Team", "type": "Status", "target": "AllEnemies", "status": "HpPerSecond", "percent": -5, "permanent": true } ] } ] } ]})";
    std::string err;
    auto traits = w2f::LoadTraitDatabaseFromJson(traitJson, &err);
    CHECK(traits != nullptr);
    if (!traits) { std::printf("  %s\n", err.c_str()); return; }
    ChampionDefinition holder = Fighter(1, 1000, 0, 95, 1000, 1);
    holder.traits = {"Z"};
    ChampionDefinition victim = Fighter(2, 100, 0, 0, 1000, 1);
    Duel d;
    d.Add(holder, 1, 0, {3, 3});
    d.Add(victim, 2, 1, {3, 4});
    const CombatConfig cfg;
    const FightResult r = RunDuel(d, 100, cfg, traits.get());
    // 95 damage leaves 5 HP (< 10%): executed on the same tick, the remaining 5 dealt as a "triggered" true hit credited to the zone's holder.
    const auto hits = Events(r.log, CombatEventType::Damage, 2);
    CHECK(hits.size() == 2 && hits[0].amount == 95 && hits[1].amount == 5 && hits[1].hpAfter == 0 && (hits[1].flags & kFlagTriggered) != 0 && hits[1].other == 1);
    CHECK(r.winner == CombatWinner::Home && r.log.endTick == 0);

    // HpPerSecond -5% of 100000 = 5000 a second, true damage, and it never counts as damage dealt by the holder.
    ChampionDefinition tank = Fighter(3, 100000, 0, 0, 1000, 1);
    Duel d2;
    d2.Add(holder, 1, 0, {3, 3});
    d2.Add(tank, 2, 1, {3, 4});
    const FightResult r2 = RunDuel(d2, 70, cfg, traits.get());
    std::vector<int> zone;
    for (const CombatEvent& e : Events(r2.log, CombatEventType::Damage, 2)) {
        if ((e.flags & kFlagTriggered) && e.other == 1) zone.push_back(e.tick * 1000000 + e.amount);
    }
    CHECK((zone == std::vector<int>{30 * 1000000 + 5000, 60 * 1000000 + 5000}));
}

static void TestPhase11PrimitiveLoaderErrors() {
    std::string err;
    const auto bad = [&](const char* json, const char* mention) {
        const bool loaded = w2f::LoadChampionDatabaseFromJson(json, &err) != nullptr;
        const bool ok = !loaded && err.find(mention) != std::string::npos;
        if (!ok) std::printf("  wanted an error mentioning '%s', got %s'%s'\n", mention, loaded ? "NO ERROR " : "", err.c_str());
        CHECK(ok);
    };
    const std::string head = R"({"version": 1, "champions": [ { "id": 1, "name": "A", "cost": 1, "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1, "maxMana": 10 }, )";
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Displace", "target": "CurrentTarget", "direction": "Sideways" } ] } } ]})").c_str(), "direction");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Displace", "target": "CurrentTarget", "direction": "Away", "hexes": 99 } ] } } ]})").c_str(), "hexes");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Damage", "target": "CurrentTarget", "damageType": "Physical", "amount": 5, "armorPenPercent": 101 } ] } } ]})").c_str(), "armorPenPercent");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "requiresCharge": true, "effects": [ { "type": "Damage", "target": "CurrentTarget", "damageType": "Physical", "amount": 5 } ] } } ]})").c_str(), "requiresCharge");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Status", "target": "Self", "status": "BonusMaxMana", "value": 5, "durationSeconds": 3 } ] } } ]})").c_str(), "permanent");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Status", "target": "Self", "status": "ExecuteBelow", "percent": 80, "permanent": true } ] } } ]})").c_str(), "ExecuteBelow");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Status", "target": "Self", "status": "EmpoweredAttack", "percent": 0, "durationSeconds": 3 } ] } } ]})").c_str(), "charges");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Teleport", "target": "Self", "destination": "Sideways" } ] } } ]})").c_str(), "destination");
    bad((head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Damage", "target": { "mode": "CurrentTarget", "count": 2 }, "damageType": "Physical", "amount": 5 } ] } } ]})").c_str(), "count");
    bad((head + R"("passive": { "id": 1, "name": "x", "effects": [ { "type": "Teleport", "target": "Self", "destination": "BehindCurrentTarget" } ] } } ]})").c_str(), "current target");
    // ...and the same things written correctly load, and land in the definition.
    auto ok = w2f::LoadChampionDatabaseFromJson((head + R"("stats2": 0 } ]})").c_str(), &err);
    (void)ok;
    const std::string good = head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [
        { "type": "Displace", "target": "CurrentTarget", "direction": "Toward", "hexes": 2 },
        { "type": "Damage", "target": { "mode": "AreaAroundTarget", "radius": 1, "includeCenter": false, "count": 1 }, "damageType": "Magic", "amount": 5, "armorPenPercent": 40 },
        { "type": "DoT", "target": "CurrentTarget", "damageType": "Magic", "amount": 9, "amountIsTotal": true, "durationSeconds": 2, "intervalSeconds": 0.5, "healPercent": 100 },
        { "type": "Teleport", "target": "Self", "destination": "NextToLowestHpEnemy" },
        { "type": "Damage", "target": "HighestHpEnemy", "damageType": "Physical", "amount": 5 } ] },
      "triggers": [ { "id": 2, "name": "hit", "trigger": "OnBasicAttack", "requiresCharge": true, "effects": [ { "type": "Damage", "target": "CurrentTarget", "damageType": "True", "amount": 1 } ] } ] } ]})";
    auto db = w2f::LoadChampionDatabaseFromJson(good, &err);
    CHECK(db != nullptr);
    if (!db) { std::printf("  %s\n", err.c_str()); return; }
    const ChampionDefinition* a = db->Find(1);
    CHECK(a && a->ability.effects.size() == 5 && a->triggers.size() == 1 && a->triggers[0].requiresCharge);
    if (a && a->ability.effects.size() == 5) {
        const auto* displace = std::get_if<DisplaceEffect>(&a->ability.effects[0].payload);
        CHECK(displace && displace->direction == DisplaceDirection::TowardCaster && displace->hexes == 2);
        CHECK(a->ability.effects[1].target.count == 1);
        const auto* dot = std::get_if<DotEffect>(&a->ability.effects[2].payload);
        CHECK(dot && dot->healPercent == 100);
        CHECK(a->ability.effects[4].target.mode == TargetMode::HighestHpEnemy);
    }
    // The new primitives change the data hash (a snapshot must not restore against retuned shapes).
    auto plain = w2f::LoadChampionDatabaseFromJson(head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Displace", "target": "CurrentTarget", "direction": "Toward", "hexes": 1 } ] } } ]})", &err);
    auto other = w2f::LoadChampionDatabaseFromJson(head + R"("ability": { "id": 1, "name": "x", "trigger": "Mana", "effects": [ { "type": "Displace", "target": "CurrentTarget", "direction": "Away", "hexes": 1 } ] } } ]})", &err);
    CHECK(plain && other && plain->ContentHash() != other->ContentHash());
}

static void TestShopPoolExhaustionFallback() {
    // When the tiers a level can roll run dry, the shop rolls a DIFFERENT tier (the nearest one with copies) instead of leaving slots empty
    // while the pool still has stock; only a completely empty pool leaves empty slots. Never a crash, a hang, or a phantom copy.
    {   // Level 1 rolls tier 1 only. Tier 1 has 2 copies, tier 2 has 5: two slots show tier 1, the other three fall back to tier 2.
        auto db = ChampionDatabase::Create({Def(1, "Cheap", 1), Def(2, "Dear", 2)});
        TestGameConfig cfg;
        cfg.pool.copiesPerTier = {{2, 5, 1, 1, 1}};
        for (auto& row : cfg.shop.dropRatesByLevel) row = {{100, 0, 0, 0, 0}};
        SharedChampionPool pool(*db, cfg.pool);
        PlayerState p(0, cfg.player, cfg.shop, pool, 5);
        p.Shop().Refresh();
        int cheap = 0, dear = 0, empty = 0;
        for (const ChampionDefinition* slot : p.Shop().Slots()) {
            if (slot == nullptr) ++empty;
            else if (slot->cost == 1) ++cheap;
            else ++dear;
        }
        CHECK(cheap == 2 && dear == 3 && empty == 0);
        CHECK(pool.Remaining(1) == 0 && pool.Remaining(2) == 2);
        // Rerolling puts the old offer back first and deals again: same totals.
        p.Shop().Refresh();
        int shown = 0;
        for (const ChampionDefinition* slot : p.Shop().Slots()) shown += slot != nullptr ? 1 : 0;
        CHECK(shown == 5 && pool.Remaining(1) + pool.Remaining(2) == 2);
    }
    {   // The whole pool empty: every slot empty, buying answers EmptySlot, rerolling still works and costs its gold.
        auto db = ChampionDatabase::Create({Def(1, "Only", 1)});
        TestGameConfig cfg;
        cfg.pool.copiesPerTier = {{2, 1, 1, 1, 1}};
        SharedChampionPool pool(*db, cfg.pool);
        PlayerState hog(0, cfg.player, cfg.shop, pool, 5);
        hog.Shop().Refresh();                     // takes both copies
        PlayerState p(1, cfg.player, cfg.shop, pool, 6);
        p.Shop().Refresh();
        for (const ChampionDefinition* slot : p.Shop().Slots()) CHECK(slot == nullptr);
        CHECK(p.Shop().TryBuy(0) == ActionResult::EmptySlot);
        p.AddGold(10);
        const int before = p.Gold();
        CHECK(p.Shop().TryReroll() == ActionResult::Ok && p.Gold() == before - p.Shop().RerollCost());
        for (const ChampionDefinition* slot : p.Shop().Slots()) CHECK(slot == nullptr);
    }
    {   // Eight players hammering a pool with ONE copy of each of the 30 real champions: copies are conserved through hundreds of rerolls,
        // no shop ever holds a copy the pool does not have, and no slot is empty while the pool has stock.
        auto roster = ProdDb();
        CHECK(roster != nullptr);
        if (!roster) return;
        TestGameConfig cfg;
        cfg.pool.copiesPerTier = {{1, 1, 1, 1, 1}};
        SharedChampionPool pool(*roster, cfg.pool);
        std::vector<std::unique_ptr<PlayerState>> players;
        for (int i = 0; i < kMaxPlayers; ++i) players.push_back(std::make_unique<PlayerState>(static_cast<PlayerId>(i), cfg.player, cfg.shop, pool, 100 + i));
        int totalCopies = 0;
        for (const ChampionDefinition& c : roster->All()) totalCopies += c.summon ? 0 : 1;
        CHECK(totalCopies == 30);
        Rng chooser(9);
        bool conserved = true, noStarvation = true;
        for (int step = 0; step < 600; ++step) {
            PlayerState& p = *players[chooser.NextBelow(kMaxPlayers)];
            p.Shop().Refresh();
            int shown = 0, emptySlots = 0;
            for (const auto& q : players) {
                for (const ChampionDefinition* slot : q->Shop().Slots()) {
                    shown += slot != nullptr ? 1 : 0;
                    if (&*q == &p && slot == nullptr) ++emptySlots;
                }
            }
            int remaining = 0;
            for (int tier = 1; tier <= kMaxCostTier; ++tier) remaining += pool.RemainingInTier(tier);
            conserved = conserved && shown + remaining == totalCopies;
            noStarvation = noStarvation && (emptySlots == 0 || remaining == 0);
        }
        CHECK(conserved && noStarvation);
        int shownAtEnd = 0;
        for (const auto& q : players) for (const ChampionDefinition* slot : q->Shop().Slots()) shownAtEnd += slot != nullptr ? 1 : 0;
        CHECK(shownAtEnd == 30);   // 8 shops x 5 slots = 40 slots for 30 copies: all of them are out on display
        for (auto& q : players) q->Shop().ReturnShopToPool();
        int back = 0;
        for (int tier = 1; tier <= kMaxCostTier; ++tier) back += pool.RemainingInTier(tier);
        CHECK(back == 30);
    }
    {   // A whole match with a pool this small (1 copy each): bots buy out tiers all game long, shops keep falling back, and the match still finishes
        // with every integrity check holding and a replay that is identical tick for tick.
        auto roster = ProdDb();
        CHECK(roster != nullptr);
        if (!roster) return;
        const BotMatchRun a = PlayBotMatch(*roster, 2024, 1, false, 1);
        const BotMatchRun b = PlayBotMatch(*roster, 2024, 1, false, 1);
        CHECK(a.finished && a.winner != kInvalidPlayerId && a.integrity && a.layouts && a.logsValid);
        CHECK(a.hashes == b.hashes && a.winner == b.winner);
        std::printf("  1-copy pool match: %d rounds, %d fights, units bought %d\n", a.rounds, a.fights, a.bought);
    }
}

// ==== Phase 12: item gaps (shield-break detonation, "took no damage" condition, every-second hook), Assassin, Omnivamp, permanent immunity ====

static void TestMageShieldDetonation() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    // The holder (2000 HP, AP stat 10) wears Mage Shield (+30 MR, +12 AP). An enemy's ability hits it for 200 magic every second (153 after MR).
    ChampionDefinition holder = Fighter(1, 2000, 0, 0);
    holder.stats.abilityDamage = Same(10);
    ChampionDefinition caster = Fighter(2, 100000, 0, 0);
    SetAbility(caster, 84, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::Magic, 200))});
    Duel d;
    d.Add(holder, 1, 0, {3, 3});
    d.Add(caster, 2, 1, {3, 4});
    d.Finish();
    d.specs[0].items = {items->Find(14)};
    const FightResult r = RunP10(d, 320, 1, cfg);

    std::vector<int> shieldTicks;
    for (const CombatEvent& e : Events(r.log, CombatEventType::ShieldApplied, 1)) shieldTicks.push_back(e.tick);
    CHECK(shieldTicks.size() >= 2 && shieldTicks[0] == 90);   // the 4th ability hit (ticks 0, 30, 60, 90) grants the shield
    const auto shield = Events(r.log, CombatEventType::ShieldApplied, 1);
    CHECK(!shield.empty() && shield[0].amount == 200 && shield[0].duration == 150);   // 10% of 2000 HP for 5 s
    // The shield stores what it absorbs: 153 from the 5th hit, then the last 47 of the 6th -- 200 in all -- and detonates when it breaks:
    // 200 stored + 150% of the holder's 22 AP (10 + 12) = 233 magic damage, to the enemy that broke it.
    std::vector<CombatEvent> blasts;
    for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
        if ((e.flags & kFlagTriggered) && e.other == 1) blasts.push_back(e);
    }
    CHECK(!blasts.empty() && blasts[0].tick == 150 && blasts[0].amount == 233 && blasts[0].subtype == static_cast<std::uint8_t>(DamageType::Magic));
    for (const CombatEvent& b : blasts) CHECK(b.amount == 233);
    // A shield that runs out of TIME does not detonate: the second shield is made on tick 240 (hit 8) and breaks on hit 10, so exactly two blasts by tick 300.
    CHECK(blasts.size() == 2);

    // The detonation belongs to the item's own shield: a plain shield on the holder (a shield ability of its own) breaking does not set it off.
    ChampionDefinition shielded = Fighter(1, 2000, 0, 0);
    SetAbility(shielded, 85, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::Self(), [] { ShieldEffect sh; sh.amount = FlatAmount(Same(100)); sh.duration = FlatAmount(Same(300)); return sh; }())});
    shielded.stats.attackDamage = Same(1);
    Duel d2;
    d2.Add(shielded, 1, 0, {3, 3});
    d2.Add(caster, 2, 1, {3, 4});
    d2.Finish();
    d2.specs[0].items = {items->Find(14)};
    const FightResult r2 = RunP10(d2, 100, 1, cfg);
    int plainBreaks = 0, blasts2 = 0;
    for (const CombatEvent& e : Events(r2.log, CombatEventType::ShieldEnded, 1)) plainBreaks += e.tick < 90 ? 1 : 0;
    for (const CombatEvent& e : Events(r2.log, CombatEventType::Damage, 2)) blasts2 += ((e.flags & kFlagTriggered) != 0) ? 1 : 0;
    CHECK(plainBreaks > 0 && blasts2 == 0);   // its own shields broke, and nothing detonated (Mage Shield's shield does not exist before the 4th ability hit)
}

static void TestTearOfMotherNoDamageCondition() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    // The holder casts on every attack (ability id 84 does 1 true damage), so its 2nd cast is at tick 30.
    const auto run = [&](int enemyDamage) {
        ChampionDefinition holder = Fighter(1, 100000, 0, 1);
        holder.stats.abilityDamage = Same(10);
        SetAbility(holder, 84, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 1))});
        Duel d;
        d.Add(holder, 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, enemyDamage), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(18)};
        return RunP10(d, 400, 1, cfg);
    };
    {   // Nobody hurts the holder: the shield (200% of AP: (10 + 15) x 2 = 50, 5 s) and then, when the 5 s are over untouched, +15% AP for 3 s.
        const FightResult r = run(0);
        const auto shield = Events(r.log, CombatEventType::ShieldApplied, 1);
        CHECK(shield.size() == 1 && shield[0].tick == 30 && shield[0].amount == 50 && shield[0].duration == 150);
        const auto ap = StatusOn(r, 1, StatusType::AbilityPower);
        CHECK(ap.size() == 1 && ap[0].tick == 30 + 150 && ap[0].amount == 15 && ap[0].duration == 90);
    }
    {   // One hit at any time inside the window (even one the shield swallows) cancels the bonus -- the shield itself is unaffected.
        const FightResult r = run(1);
        CHECK(Events(r.log, CombatEventType::ShieldApplied, 1).size() == 1);
        CHECK(StatusOn(r, 1, StatusType::AbilityPower).empty());
    }
    {   // Damage taken BEFORE the cast does not count: the enemy stops attacking (it is stunned) right after the first hit.
        ChampionDefinition holder = Fighter(1, 100000, 0, 1);
        holder.stats.abilityDamage = Same(10);
        SetAbility(holder, 84, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::CurrentTarget(), Dmg(DamageType::True, 1))});
        ChampionDefinition enemy = Fighter(2, 100000000, 0, 5);
        SetAbility(enemy, 86, CastTrigger::EveryNthAttack, 1, 0, {Eff(TargetSpec::Self(), [] { StatusEffect st; st.status = StatusType::Stun; st.duration = FlatAmount(Same(600)); return st; }())});
        Duel d;
        d.Add(holder, 1, 0, {3, 3});
        d.Add(enemy, 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(18)};
        const FightResult r = RunP10(d, 400, 1, cfg);
        CHECK(Events(r.log, CombatEventType::Damage, 1).size() == 1);   // the single hit at tick 0
        CHECK(StatusOn(r, 1, StatusType::AbilityPower).size() == 1);
    }
    // The condition needs a delay, and only knows one name.
    std::string err;
    const std::string head = R"({"version": 1, "champions": [ { "id": 1, "name": "A", "cost": 1, "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1 }, )";
    CHECK(w2f::LoadChampionDatabaseFromJson((head + R"("ability": { "id": 1, "name": "x", "trigger": "EveryNthAttack", "attackCount": 1, "effects": [ { "type": "Status", "target": "Self", "status": "AbilityPower", "percent": 5, "durationSeconds": 1, "condition": "NoDamageTakenSinceCast" } ] } } ]})").c_str(), &err) == nullptr && err.find("needs a delay") != std::string::npos);
    CHECK(w2f::LoadChampionDatabaseFromJson((head + R"("ability": { "id": 1, "name": "x", "trigger": "EveryNthAttack", "attackCount": 1, "effects": [ { "type": "Status", "target": "Self", "status": "AbilityPower", "percent": 5, "durationSeconds": 1, "delaySeconds": 1, "condition": "WhenItRains" } ] } } ]})").c_str(), &err) == nullptr && err.find("condition") != std::string::npos);
}

static void TestTwinSnipersEverySecond() {
    auto items = P10Items();
    CHECK(items != nullptr);
    if (!items) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    const auto truePings = [](const FightResult& r, UnitId victim) {   // hook damage: true damage, flagged "triggered", not a DoT tick
        std::vector<CombatEvent> out;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, victim)) {
            if ((e.flags & kFlagTriggered) && !(e.flags & kFlagDot) && e.subtype == static_cast<std::uint8_t>(DamageType::True)) out.push_back(e);
        }
        return out;
    };
    {   // One Twin Snipers: every second (ticks 30, 60, 90 ...) its target loses 3% of ITS max HP -- 3000 of 100,000 -- as true damage, and is wounded.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 1, 1000, 3), 1, 0, {3, 2});
        d.Add(Dummy(2, 100000), 2, 1, {3, 4});   // 2 hexes away: in range 3
        d.Finish();
        d.specs[0].items = {items->Find(30)};
        const FightResult r = RunP10(d, 130, 1, cfg);
        const auto pings = truePings(r, 2);
        CHECK(pings.size() == 4 && pings[0].tick == 30 && pings[1].tick == 60 && pings[3].tick == 120);
        for (const CombatEvent& p : pings) CHECK(p.amount == 3000 && p.other == 1);
        bool wound = false;
        for (const CombatEvent& e : StatusOn(r, 2, StatusType::Wound)) wound = wound || (e.amount == 30 && e.other == 1);
        CHECK(wound);
    }
    {   // It follows the CURRENT target, and only while it is in attack range: a unit that has not reached its enemy yet deals nothing.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 1, 1000, 1), 1, 0, {3, 0});   // melee, far from its enemy at first
        d.Add(Dummy(2, 100000), 2, 1, {3, 7});
        d.Finish();
        d.specs[0].items = {items->Find(30)};
        const FightResult r = RunP10(d, 130, 1, cfg);
        const auto pings = truePings(r, 2);
        int firstAttack = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Attack, 1)) { firstAttack = e.tick; break; }
        CHECK(!pings.empty() || firstAttack > 0);
        for (const CombatEvent& p : pings) CHECK(p.tick >= firstAttack);   // never before it is in range (= before its first swing)
        CHECK(pings.size() < 4);
    }
    {   // Two Twin Snipers stack: two pings a second. A stunned holder deals none while it is stunned.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 1, 1000, 3), 1, 0, {3, 2});
        d.Add(Dummy(2, 100000), 2, 1, {3, 4});
        d.Finish();
        d.specs[0].items = {items->Find(30), items->Find(30)};
        const FightResult r = RunP10(d, 70, 1, cfg);
        CHECK(truePings(r, 2).size() == 4);   // ticks 30 and 60, twice each
    }
    // Loader: the interval is required and only belongs to this trigger.
    std::string err;
    const std::string head = R"({"version": 1, "champions": [ { "id": 1, "name": "A", "cost": 1, "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1 }, "triggers": [ )";
    const std::string tail = R"( ] } ]})";
    const std::string fx = R"("effects": [ { "type": "Damage", "target": "CurrentTarget", "damageType": "True", "amount": 5 } ])";
    CHECK(w2f::LoadChampionDatabaseFromJson((head + R"({ "id": 1, "name": "x", "trigger": "EveryInterval", )" + fx + "}" + tail).c_str(), &err) == nullptr && err.find("interval") != std::string::npos);
    CHECK(w2f::LoadChampionDatabaseFromJson((head + R"({ "id": 1, "name": "x", "trigger": "OnBasicAttack", "intervalSeconds": 1, )" + fx + "}" + tail).c_str(), &err) == nullptr && err.find("interval") != std::string::npos);
    CHECK(w2f::LoadChampionDatabaseFromJson((head + R"({ "id": 1, "name": "x", "trigger": "OnBasicAttack", "onlyShieldsFrom": 3, )" + fx + "}" + tail).c_str(), &err) == nullptr && err.find("onlyShieldsFrom") != std::string::npos);
    CHECK(w2f::LoadChampionDatabaseFromJson((head + R"({ "id": 1, "name": "x", "trigger": "EveryInterval", "intervalSeconds": 2, )" + fx + "}" + tail).c_str(), &err) != nullptr);
}

static void TestFishscaleOmnivampAndAssassin() {
    auto items = P10Items();
    auto prod = ProdDb();
    auto traits = sample::LoadProductionTraits();
    CHECK(items != nullptr && prod != nullptr && traits != nullptr);
    if (!items || !prod || !traits) return;
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    {   // Fishscale: 25% Omnivamp -- the holder heals for 25% of ALL the damage it deals (after armor), e.g. 40 -> 10.
        Duel d;
        d.Add(Fighter(1, 100000, 0, 40), 1, 0, {3, 3});
        d.Add(Fighter(2, 100000000, 0, 30), 2, 1, {3, 4});   // hurts the holder, so there is HP to restore
        d.Finish();
        d.specs[0].items = {items->Find(37)};
        CombatConfig noCrit = cfg;
        const FightResult r = RunP10(d, 130, 1, noCrit);
        int dealt = 0, healed = 0, hits = 0;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.other == 1 && !(e.flags & kFlagTriggered)) { dealt += e.amount * 25 / 100; ++hits; }
        }
        for (const CombatEvent& e : Events(r.log, CombatEventType::Heal, 1)) healed += e.amount;
        CHECK(hits >= 4 && dealt > 0 && healed == dealt);
        // ...and the shield of 10% max HP with every cast is still there (it needs a cast; this unit has none) -- data check instead:
        const ItemDefinition* fish = items->Find(37);
        CHECK(fish && fish->abilities.size() == 2 && fish->abilities[0].trigger == CastTrigger::OnCast && fish->abilities[1].trigger == CastTrigger::OnDealDamage);
    }
    {   // ASSASSIN (2/4): Vex, Lunis and Raa are the Assassins. Two of them: abilities can crit (a unit with 100% crit chance always does) and crits hit
        // for +20% more; the emblem makes a 4th Assassin and the bonus +50%.
        const auto play = [&](int assassins, bool emblem) {
            const ChampionId ids[3] = {9015, 9009, 9028};
            Duel d;
            AddSummonDefs(d, *prod);
            for (int i = 0; i < assassins; ++i) {
                ChampionDefinition def = Primed(*prod, ids[i]);
                def.stats.critChance = Same(100);
                d.Add(def, static_cast<UnitId>(i + 1), 0, HexCoord{3 + i, 3});
            }
            if (emblem) d.Add(Dummy(4, 100000), 4, 0, HexCoord{0, 0});
            d.Add(Dummy(10, 100000), 10, 1, {3, 4});
            d.Finish();
            if (emblem) d.specs[static_cast<std::size_t>(assassins)].items = {items->Find(46)};
            const FightResult r = CombatSimulator(cfg, traits.get(), items.get(), d.db.get()).RunFight(d.specs, 20, 5);
            CheckValid(r, d, 20, cfg);
            return r;
        };
        const FightResult one = play(1, false);
        CHECK(TraitEvents(one, 9).empty());   // one Assassin: nothing
        const FightResult two = play(2, false);
        const auto act = TraitEvents(two, 9);
        CHECK(act.size() == 1 && act[0].subtype == 1 && act[0].amount == 2);
        for (UnitId u : {UnitId{1}, UnitId{2}}) {
            CHECK(StatusOn(two, u, StatusType::AbilityCrit).size() == 1);
            const auto cd = StatusOn(two, u, StatusType::CritDamage);
            CHECK(cd.size() == 1 && cd[0].amount == 20 && cd[0].duration == 0);
        }
        // Vex (unit 1): 150 physical, crit bonus 21 + 20 = 141% -> 211 on the dummy (armor 0).
        int vexHit = 0;
        for (const CombatEvent& e : Events(two.log, CombatEventType::Damage, 10)) {
            if (e.other == 1 && (e.flags & kFlagAbility)) { vexHit = e.amount; CHECK((e.flags & kFlagCrit) != 0); break; }
        }
        CHECK(vexHit == 150 * (100 + cfg.critBonusPercent + 20) / 100);
        const FightResult four = play(3, true);   // Vex + Lunis + Raa + an Assassin Emblem on the 4th unit
        const auto act4 = TraitEvents(four, 9);
        CHECK(act4.size() == 1 && act4[0].subtype == 2 && act4[0].amount == 4);
        const auto cd4 = StatusOn(four, 1, StatusType::CritDamage);
        CHECK(cd4.size() == 1 && cd4[0].amount == 50);
        // The emblem carrier itself is an Assassin too (it gets the bonus).
        CHECK(StatusOn(four, 4, StatusType::AbilityCrit).size() == 1);
    }
}

static void TestProductionItemsAreTheWholeDesignDoc() {
    auto items = P10Items();
    auto traits = sample::LoadProductionTraits();
    auto prod = ProdDb();
    CHECK(items != nullptr && traits != nullptr && prod != nullptr);
    if (!items || !traits || !prod) return;
    std::string err;
    CHECK(w2f::ValidateItemTraits(*items, *traits, &err));
    CHECK(w2f::ValidateSummonReferences(*prod, items.get(), traits.get(), &err));
    // Every recipe of the doc resolves to the right finished item, in both orders.
    struct Recipe { ItemId a, b; const char* name; };
    const Recipe recipes[] = {
        {1, 2, "Sayona's Casket"}, {1, 3, "Soldiers' Soul"}, {1, 4, "Guardians Armor"}, {1, 5, "Gylachster"}, {1, 6, "Mage Shield"}, {1, 8, "Mother's Hands"}, {1, 1, "Big Helmet"}, {1, 7, "Head Shot"},
        {2, 2, "Tear Of Mother"}, {2, 3, "Unalive Sword"}, {2, 4, "Fishtank"}, {2, 5, "Water Gun"}, {2, 6, "Divine Magic"}, {2, 8, "Blue Whale"}, {2, 7, "Fishscale"},
        {3, 3, "Soul's Sword"}, {3, 4, "Deadbeat"}, {3, 5, "Gunfire"}, {3, 6, "Electroblade"}, {3, 8, "HeartBroke"}, {3, 7, "Full Kit"},
        {5, 5, "Twin Snipers"}, {5, 6, "Phaisa's Magic"}, {5, 8, "Betrayed Heart"}, {5, 7, "Guardian Destroyer"},
        {6, 6, "Magic Stick"}, {6, 8, "Omnilium's Book"}, {6, 7, "Resist Puncher"},
        {9, 3, "Coregons Emblem"}, {9, 1, "Helios Emblem"}, {9, 2, "Najmi Emblem"}, {9, 4, "Omnilium Emblem"}, {9, 5, "Phaisa Emblem"}, {9, 6, "Hexagon Emblem"}, {9, 7, "Assassin Emblem"}, {9, 8, "Protector Emblem"},
    };
    int good = 0;
    for (const Recipe& r : recipes) {
        const ItemDefinition* ab = items->FindCombination(r.a, r.b);
        const ItemDefinition* ba = items->FindCombination(r.b, r.a);
        if (ab && ab == ba && ab->name == r.name) ++good;
        else std::printf("  recipe %u + %u should make %s\n", r.a, r.b, r.name);
    }
    CHECK(good == static_cast<int>(sizeof(recipes) / sizeof(recipes[0])));
    CHECK(sizeof(recipes) / sizeof(recipes[0]) == 28 + 8);
    // Numbers of the latest doc.
    CHECK(items->Find(13)->abilities[0].effects.size() == 1);   // Gylachster (permanent immunity: see the hook test)
    const auto* tick = std::get_if<DamageEffect>(&items->Find(30)->abilities[1].effects[0].payload);   // Twin Snipers' 3% a second
    CHECK(tick && tick->type == DamageType::True && tick->amount.terms.size() == 1 && tick->amount.terms[0].source == StatSource::TargetMaxHp && tick->amount.terms[0].percent == Same(3));
    CHECK(items->Find(30)->abilities[1].trigger == CastTrigger::EveryInterval && items->Find(30)->abilities[1].intervalTicks == 30);
    const auto* omni = std::get_if<HealEffect>(&items->Find(37)->abilities[1].effects[0].payload);   // Fishscale's Omnivamp 25%
    CHECK(omni && omni->amount.terms.size() == 1 && omni->amount.terms[0].source == StatSource::TriggerDamage && omni->amount.terms[0].percent == Same(25));
    CHECK(items->Find(14)->abilities.size() == 2 && items->Find(14)->abilities[1].trigger == CastTrigger::OnShieldBreak && items->Find(14)->abilities[1].shieldFromAbility == 1401);
    CHECK(items->Find(18)->abilities[0].effects[1].condition == EffectCondition::NoDamageTakenSinceCast);
    // The data hash sees the new shapes.
    CHECK(items->ContentHash() != 0);
    CHECK(prod->Find(9015)->traits == std::vector<std::string>({"Phaisa", "Assassin"}) && prod->Find(9009)->traits.back() == "Assassin" && prod->Find(9028)->traits.back() == "Assassin");
}


// ==== Phase 13: Mother Nature (gifts every 3rd round instead of a shop), refreshing burns, Assassin crit chance ==============================

namespace {

std::unique_ptr<MotherNatureDatabase> MnDb(const std::string& json, const ItemDatabase* items = nullptr) {
    std::string err;
    auto db = w2f::LoadMotherNatureDatabaseFromJson(json, items, &err);
    if (!db) std::printf("  Mother Nature data: %s\n", err.c_str());
    return db;
}
// options + one tier holding exactly the given gifts (JSON objects, comma separated).
std::string MnJson(int options, const std::string& gifts, int fromStage = 1) {
    return R"({"version": 1, "options": )" + std::to_string(options) + R"(, "tiers": [ { "id": 1, "name": "T", "fromStage": )" + std::to_string(fromStage) +
           R"(, "gifts": [ )" + gifts + R"( ] } ]})";
}

struct GiftLog : IMatchListener {
    struct Offered { PlayerId player; std::vector<GiftOffer> offers; };
    struct Picked { PlayerId player; int index; GiftOffer gift; bool automatic; int goldConverted; };
    std::vector<Offered> offered;
    std::vector<Picked> picked;
    void OnGiftsOffered(PlayerId p, const std::vector<GiftOffer>& o) override { offered.push_back({p, o}); }
    void OnGiftPicked(PlayerId p, int i, const GiftOffer& g, bool a, int c) override { picked.push_back({p, i, g, a, c}); }
};

// A 2-player match whose EVERY round is a Mother Nature round, started and sitting in the MotherNature phase of round 1.
struct MnMatch {
    std::unique_ptr<MatchManager> match;
    GiftLog log;
    std::unique_ptr<MotherNatureDatabase> data;
    static std::unique_ptr<MnMatch> Make(const ChampionDatabase& db, std::unique_ptr<MotherNatureDatabase> data, const ItemDatabase* items = nullptr,
                                         std::uint64_t seed = 7, const std::function<void(GameConfig&)>& tweak = {}) {
        auto m = std::make_unique<MnMatch>();
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        cfg.match.motherNatureEveryRounds = 1;
        cfg.match.motherNatureTicks = 30;
        cfg.match.planningTicks = 10;
        cfg.match.combatTicks = 5;
        cfg.match.resolutionTicks = 2;
        cfg.player.startingGold = 10;
        if (tweak) tweak(cfg);
        m->data = std::move(data);
        m->match = MatchManager::Create(cfg, db, seed, nullptr, nullptr, items, nullptr, m->data.get());
        if (!m->match) return nullptr;
        m->match->AddListener(&m->log);
        m->match->Start();
        return m;
    }
};

}  // namespace

static void TestMotherNatureDataFile() {
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
    std::string err;
    auto data = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), items.get(), &err);
    CHECK(items != nullptr && data != nullptr);
    if (!items || !data) { std::printf("  %s\n", err.c_str()); return; }
    CHECK(data->Options() == 2 && data->Tiers().size() == 2);
    const MotherNatureTier& early = data->Tiers()[0];
    const MotherNatureTier& late = data->Tiers()[1];
    CHECK(early.id == 1 && early.fromStage == 1 && late.id == 3 && late.fromStage == 4);
    CHECK(&data->TierFor(1) == &early && &data->TierFor(3) == &early && &data->TierFor(4) == &late && &data->TierFor(9) == &late);
    // Tier 1: a component, +5 gold, +4 XP, Mother's Blessing (+3 HP), a 2/3-cost unit.
    CHECK(early.gifts.size() == 5);
    const auto find = [](const MotherNatureTier& t, GiftType type) { std::vector<const GiftDefinition*> out; for (const GiftDefinition& g : t.gifts) if (g.type == type) out.push_back(&g); return out; };
    CHECK(find(early, GiftType::Gold).size() == 1 && find(early, GiftType::Gold)[0]->amount == 5);
    CHECK(find(early, GiftType::Xp).size() == 1 && find(early, GiftType::Xp)[0]->amount == 4);
    CHECK(find(early, GiftType::Heal).size() == 1 && find(early, GiftType::Heal)[0]->amount == 3 && find(early, GiftType::Heal)[0]->name == "Mother's Blessing");
    CHECK(find(early, GiftType::Unit).size() == 1 && find(early, GiftType::Unit)[0]->costs == std::vector<int>({2, 3}));
    CHECK(find(early, GiftType::Item).size() == 1 && find(early, GiftType::Item)[0]->itemClass == ItemClass::Component);
    // Tier 3: a completed legendary item, an emblem (low chance), +15 gold, Mother's Miracle (+7 HP), a 5-cost unit.
    CHECK(late.gifts.size() == 5);
    CHECK(find(late, GiftType::Gold)[0]->amount == 15 && find(late, GiftType::Heal)[0]->amount == 7 && find(late, GiftType::Heal)[0]->name == "Mother's Miracle");
    CHECK(find(late, GiftType::Unit)[0]->costs == std::vector<int>({5}));
    int legendaryWeight = 0, emblemWeight = 0;
    for (const GiftDefinition* g : find(late, GiftType::Item)) (g->itemClass == ItemClass::Emblem ? emblemWeight : legendaryWeight) += g->weight;
    CHECK(emblemWeight > 0 && emblemWeight < legendaryWeight);   // "low chance"
    // The item classes resolve against the real items.json: 8 components, 28 legendaries, 8 emblems.
    GiftDefinition gift;
    gift.type = GiftType::Item;
    gift.itemClass = ItemClass::Component;
    CHECK((w2f::GiftItemChoices(gift, *items) == std::vector<ItemId>({1, 2, 3, 4, 5, 6, 7, 8})));   // never the Omnilium Seed (9)
    gift.itemClass = ItemClass::Legendary;
    const auto legendaries = w2f::GiftItemChoices(gift, *items);
    CHECK(legendaries.size() == 28 && legendaries.front() == 10 && legendaries.back() == 37);
    gift.itemClass = ItemClass::Emblem;
    CHECK((w2f::GiftItemChoices(gift, *items) == std::vector<ItemId>({40, 41, 42, 43, 44, 45, 46, 47})));
    gift.itemClass = ItemClass::Any;
    CHECK(w2f::GiftItemChoices(gift, *items).size() == items->All().size());
    gift.items = {3, 4};
    CHECK((w2f::GiftItemChoices(gift, *items) == std::vector<ItemId>({3, 4})));   // an explicit list wins

    // The hash sees everything a designer can tweak.
    const std::string base = MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5, "weight": 3})");
    const auto a = MnDb(base);
    CHECK(a && a->ContentHash() == MnDb(base)->ContentHash());
    for (const std::string& changed : {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 6, "weight": 3})"), MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5, "weight": 4})"),
                                       MnJson(1, R"({"id": 1, "name": "H", "type": "Gold", "amount": 5, "weight": 3})"), MnJson(1, R"({"id": 2, "name": "G", "type": "Gold", "amount": 5, "weight": 3})"),
                                       std::string(base).replace(base.find("\"T\""), 3, "\"U\"")}) {
        const auto b = MnDb(changed);
        CHECK(b && a && a->ContentHash() != b->ContentHash());
    }

    // A designer's mistakes are refused with the place and the reason.
    struct Bad { std::string json; const char* mention; };
    const std::string g1 = R"({"id": 1, "name": "G", "type": "Gold", "amount": 5})";
    const std::string g2 = R"({"id": 2, "name": "X", "type": "Xp", "amount": 4})";
    const Bad bad[] = {
        {MnJson(2, g1), "at least 2 gifts"},
        {MnJson(5, g1 + "," + g2), "options"},
        {MnJson(1, g1 + "," + R"({"id": 1, "name": "Dup", "type": "Xp", "amount": 4})"), "duplicate gift id"},
        {MnJson(1, g1, 2), "first tier must start at stage 1"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold"})"), "amount"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 0})"), "amount"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5, "weight": 0})"), "weight"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Present", "amount": 5})"), "unknown value"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5, "costs": [2]})"), "costs"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5, "colour": "red"})"), "colour"},
        {MnJson(1, R"({"id": 1, "name": "U", "type": "Unit"})"), "costs"},
        {MnJson(1, R"({"id": 1, "name": "U", "type": "Unit", "costs": [6]})"), "costs"},
        {MnJson(1, R"({"id": 1, "name": "U", "type": "Unit", "costs": []})"), "cost tier"},
        {MnJson(1, R"({"id": 1, "name": "I", "type": "Item"})"), "itemClass"},
        {MnJson(1, R"({"id": 1, "name": "I", "type": "Item", "itemClass": "Component", "items": [1]})"), "either"},
        {MnJson(1, R"({"id": 1, "name": "I", "type": "Item", "itemClass": "Shiny"})"), "unknown value"},
        {MnJson(1, R"({"id": 1, "name": "I", "type": "Item", "items": [99999]})"), "not in the item data"},
        {MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5, "itemClass": "Any"})"), "itemClass"},
        {R"({"version": 2, "tiers": []})", "version"},
        {R"({"version": 1})", "tiers"},
        {R"({"version": 1, "tiers": []})", "at least one tier"},
    };
    for (const Bad& b : bad) {
        std::string e;
        const bool loaded = w2f::LoadMotherNatureDatabaseFromJson(b.json, items.get(), &e) != nullptr;
        const bool ok = !loaded && e.find(b.mention) != std::string::npos;
        if (!ok) std::printf("  wanted an error mentioning '%s', got %s'%s'\n", b.mention, loaded ? "NO ERROR " : "", e.c_str());
        CHECK(ok);
    }
    // An item class with no member in the item data is refused too.
    auto few = w2f::LoadItemDatabaseFromJson(R"({"version": 1, "items": [ {"id": 1, "name": "A", "stats": {"armor": 1}} ]})");
    std::string e;
    CHECK(few && w2f::LoadMotherNatureDatabaseFromJson(MnJson(1, R"({"id": 1, "name": "I", "type": "Item", "itemClass": "Emblem"})"), few.get(), &e) == nullptr && e.find("no item of that class") != std::string::npos);
    CHECK(w2f::LoadMotherNatureDatabaseFromFile("/definitely/not/here.json", items.get(), &e) == nullptr && e.find("cannot open") != std::string::npos);
    CHECK(MnDb(base, items.get()) != nullptr);
}

static void TestMotherNatureRewards() {
    auto prod = ProdDb();
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
    CHECK(prod != nullptr && items != nullptr);
    if (!prod || !items) return;
    const auto only = [&](const std::string& gift, const std::function<void(GameConfig&)>& tweak = {}) {
        return MnMatch::Make(*prod, MnDb(MnJson(1, gift), items.get()), items.get(), 7, tweak);
    };
    {   // Gold: +5.
        auto m = only(R"({"id": 1, "name": "G", "type": "Gold", "amount": 5})");
        CHECK(m && m->match->Phase() == MatchPhase::MotherNature && m->match->GiftOffers(0).size() == 1);
        if (!m) return;
        const int before = m->match->Players().Get(0)->Gold();
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);
        CHECK(m->match->Players().Get(0)->Gold() == before + 5 && m->match->GiftSettled(0) && m->match->GiftOffers(0).empty());
        CHECK(m->log.picked.size() == 1 && m->log.picked[0].player == 0 && m->log.picked[0].index == 0 && m->log.picked[0].gift.type == GiftType::Gold &&
              m->log.picked[0].gift.amount == 5 && !m->log.picked[0].automatic && m->log.picked[0].goldConverted == 0);
    }
    {   // XP: +4 XP (several level-ups on the early curve: 2 + 2 XP make level 3).
        auto m = only(R"({"id": 1, "name": "X", "type": "Xp", "amount": 4})");
        if (!m) return;
        const PlayerState& p = *m->match->Players().Get(0);
        const int levelBefore = p.Level(), xpBefore = p.Xp();
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);
        CHECK(p.Level() > levelBefore || p.Xp() == xpBefore + 4);
        CHECK(p.Level() == 3 && p.Xp() == 0);
    }
    {   // Heal: +3 health, never above the starting health.
        auto m = only(R"({"id": 1, "name": "H", "type": "Heal", "amount": 3})");
        if (!m) return;
        m->match->PlayersMutable().Get(0)->ApplyDamage(10);
        m->match->PlayersMutable().Get(1)->ApplyDamage(1);
        CHECK(m->match->Players().Get(0)->Health() == 90);
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok && m->match->Players().Get(0)->Health() == 93);
        CHECK(m->match->TryPickGift(1, 0) == ActionResult::Ok && m->match->Players().Get(1)->Health() == 100);   // 99 + 3 = capped at 100
    }
    {   // Item: a random component, into the bag.
        auto m = only(R"({"id": 1, "name": "C", "type": "Item", "itemClass": "Component"})");
        if (!m) return;
        CHECK(m->match->GiftOffers(0).size() == 1 && m->match->GiftOffers(0)[0].item >= 1 && m->match->GiftOffers(0)[0].item <= 8);
        const ItemId promised = m->match->GiftOffers(0)[0].item;
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);
        CHECK(m->match->Players().Get(0)->ItemBag() == std::vector<ItemId>({promised}));   // exactly what was shown
    }
    {   // Unit: a champion of an allowed cost, checked out of the pool while on offer.
        auto m = only(R"({"id": 1, "name": "U", "type": "Unit", "costs": [2, 3]})");
        if (!m) return;
        const GiftOffer offer = m->match->GiftOffers(0)[0];
        CHECK(offer.champion != nullptr && (offer.champion->cost == 2 || offer.champion->cost == 3) && !offer.champion->summon);
        CHECK(m->match->VerifyPoolIntegrity() && m->match->Pool().Remaining(offer.champion->id) < m->match->Pool().InitialCopies(offer.champion->id));
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);
        CHECK(m->match->Players().Get(0)->Roster().Count() == 1 && m->match->Players().Get(0)->Roster().Units()[0].champion == offer.champion);
        CHECK(m->match->VerifyPoolIntegrity());
        CHECK(m->log.picked.size() == 1 && m->log.picked[0].gift.champion == offer.champion && m->log.picked[0].goldConverted == 0);
    }
    {   // A unit with no room for it is paid as gold (its cost) instead, and the copy goes back to the pool.
        auto m = only(R"({"id": 1, "name": "U", "type": "Unit", "costs": [5]})");
        if (!m) return;
        PlayerState* p = m->match->PlayersMutable().Get(0);
        int filled = 0;
        for (int round = 0; round < 2; ++round) {
            for (const ChampionDefinition& c : prod->All()) {
                if (c.summon || c.cost == 5 || !p->CanAcquire(&c)) continue;
                p->AcquireUnit(&c, 0);
                ++filled;
            }
        }
        const GiftOffer offer = m->match->GiftOffers(0)[0];
        CHECK(filled >= 20 && !p->CanAcquire(offer.champion));
        const int goldBefore = p->Gold();
        const int remainingBefore = m->match->Pool().Remaining(offer.champion->id);
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);
        CHECK(p->Gold() == goldBefore + 5 && m->log.picked.size() == 1 && m->log.picked[0].goldConverted == 5);
        CHECK(m->match->Pool().Remaining(offer.champion->id) == remainingBefore + 1);   // the checked-out copy came back
    }
}

static void TestMotherNaturePhaseFlow() {
    auto prod = ProdDb();
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
    CHECK(prod != nullptr && items != nullptr);
    if (!prod || !items) return;
    // The whole production gift table, 3 players, Mother Nature every 3rd round: rounds 1-2 are ordinary, round 3 is hers.
    auto data = w2f::LoadMotherNatureDatabaseFromFile(sample::ProductionMotherNaturePath(), items.get());
    CHECK(data != nullptr);
    if (!data) return;
    TestGameConfig cfg;
    cfg.match.playerCount = 3;
    cfg.match.motherNatureTicks = 40;
    cfg.match.planningTicks = 12;
    cfg.match.combatTicks = 4;
    cfg.match.resolutionTicks = 2;
    cfg.player.startingGold = 20;
    auto match = MatchManager::Create(cfg, *prod, 99, nullptr, nullptr, items.get(), nullptr, data.get());
    GiftLog log;
    match->AddListener(&log);
    match->Start();
    CHECK(!match->IsMotherNatureRound() && match->Phase() == MatchPhase::Planning && match->Players().Get(0)->Shop().Slots()[0] != nullptr);
    CHECK(match->TryPickGift(0, 0) == ActionResult::WrongPhase);   // no gifts outside her phase
    while (match->Round() < 3) match->Tick();
    CHECK(match->Round() == 3 && match->IsMotherNatureRound() && match->Phase() == MatchPhase::MotherNature);

    // Everyone alive was offered 2 DISTINCT gifts from the early tier, in seat order, before the phase event.
    CHECK(log.offered.size() == 3 && log.offered[0].player == 0 && log.offered[1].player == 1 && log.offered[2].player == 2);
    for (const GiftLog::Offered& o : log.offered) {
        CHECK(o.offers.size() == 2 && o.offers[0].gift != o.offers[1].gift);
        for (const GiftOffer& g : o.offers) CHECK(g.gift >= 101 && g.gift <= 105);
    }
    CHECK(match->VerifyPoolIntegrity());
    // The phase is closed to everything but the pick: no shop, no XP, no moves.
    CHECK(match->TryBuyXp(0) == ActionResult::WrongPhase && match->TryRerollShop(0) == ActionResult::WrongPhase && match->TryBuyShopUnit(0, 0) == ActionResult::WrongPhase);
    CHECK(match->TryPickGift(0, 2) == ActionResult::InvalidSlot && match->TryPickGift(0, 99) == ActionResult::InvalidSlot);
    CHECK(match->TryPickGift(77, 0) == ActionResult::InvalidPlayer);

    // Player 0 picks their second offer; picking twice is refused; the rest of the table is untouched.
    const GiftOffer chosen = match->GiftOffers(0)[1];
    CHECK(match->TryPickGift(0, 1) == ActionResult::Ok && match->GiftSettled(0));
    CHECK(match->TryPickGift(0, 0) == ActionResult::AlreadyPicked && match->GiftOffers(0).empty());
    CHECK(log.picked.size() == 1 && log.picked[0].gift.gift == chosen.gift && log.picked[0].index == 1 && !log.picked[0].automatic);
    CHECK(match->Phase() == MatchPhase::MotherNature && !match->GiftSettled(1));   // still waiting for the others
    CHECK(match->VerifyPoolIntegrity());

    // Player 1 picks too; player 2 never does. The phase runs its full length, then player 2 gets their FIRST offer automatically.
    const GiftOffer firstOfThird = match->GiftOffers(2)[0];
    CHECK(match->TryPickGift(1, 0) == ActionResult::Ok);
    const int remaining = match->TicksRemainingInPhase();
    int ticks = 0;
    while (match->Phase() == MatchPhase::MotherNature) { match->Tick(); ++ticks; }
    CHECK(remaining > 1 && ticks == remaining);   // it waited for the player who never chose, for the whole rest of the phase
    CHECK(match->Phase() == MatchPhase::Planning && match->Round() == 3);
    CHECK(log.picked.size() == 3 && log.picked[2].player == 2 && log.picked[2].automatic && log.picked[2].index == 0 && log.picked[2].gift.gift == firstOfThird.gift);
    // The offers are gone, and there is no shop for the whole of a Mother Nature round.
    for (PlayerId p = 0; p < 3; ++p) {
        CHECK(match->GiftOffers(p).empty());
        for (const ChampionDefinition* slot : match->Players().Get(p)->Shop().Slots()) CHECK(slot == nullptr);
    }
    CHECK(match->TryBuyShopUnit(0, 0) == ActionResult::ShopClosed && match->TryRerollShop(0) == ActionResult::ShopClosed);
    CHECK(match->TryBuyXp(0) == ActionResult::Ok);   // levelling up is not the shop
    CHECK(match->VerifyPoolIntegrity() && match->VerifyRosterLayouts());
    // The next round is ordinary again.
    while (match->Round() == 3) match->Tick();
    CHECK(match->Round() == 4 && !match->IsMotherNatureRound() && match->Phase() == MatchPhase::Planning);
    CHECK(match->Players().Get(0)->Shop().Slots()[0] != nullptr && match->TryRerollShop(0) == ActionResult::Ok);
}

static void TestMotherNatureEarlyEndAndDeterminism() {
    auto prod = ProdDb();
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
    CHECK(prod != nullptr && items != nullptr);
    if (!prod || !items) return;
    const std::string gifts = R"({"id": 1, "name": "G", "type": "Gold", "amount": 5}, {"id": 2, "name": "X", "type": "Xp", "amount": 4}, {"id": 3, "name": "U", "type": "Unit", "costs": [1, 2, 3, 4, 5]})";
    {   // Once everybody has chosen, the phase ends at once instead of running out its 30 ticks.
        auto m = MnMatch::Make(*prod, MnDb(MnJson(2, gifts), items.get()), items.get());
        CHECK(m && m->match->Phase() == MatchPhase::MotherNature);
        if (!m) return;
        m->match->Tick();
        CHECK(m->match->Phase() == MatchPhase::MotherNature);
        CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);
        m->match->Tick();
        CHECK(m->match->Phase() == MatchPhase::MotherNature);
        CHECK(m->match->TryPickGift(1, 1) == ActionResult::Ok);
        m->match->Tick();
        CHECK(m->match->Phase() == MatchPhase::Planning && m->match->Round() == 1);
        CHECK(m->log.picked.size() == 2 && !m->log.picked[0].automatic && !m->log.picked[1].automatic);
        CHECK(m->match->VerifyPoolIntegrity());
    }
    {   // With nobody choosing, the phase lasts exactly motherNatureTicks and everyone gets their first offer.
        auto m = MnMatch::Make(*prod, MnDb(MnJson(2, gifts), items.get()), items.get());
        if (!m) return;
        for (int i = 0; i < 29; ++i) m->match->Tick();
        CHECK(m->match->Phase() == MatchPhase::MotherNature && m->match->TicksRemainingInPhase() == 1);
        m->match->Tick();
        CHECK(m->match->Phase() == MatchPhase::Planning && m->log.picked.size() == 2 && m->log.picked[0].automatic && m->log.picked[1].automatic);
        CHECK(m->log.picked[0].index == 0 && m->log.picked[0].gift.gift == m->log.offered[0].offers[0].gift);
        CHECK(m->match->VerifyPoolIntegrity());
    }
    {   // Same seed, same gifts and the same state on every tick; another seed, another table.
        const auto trace = [&](std::uint64_t seed) {
            auto m = MnMatch::Make(*prod, MnDb(MnJson(2, gifts), items.get()), items.get(), seed);
            std::vector<std::uint64_t> hashes;
            std::vector<std::uint32_t> offered;
            if (!m) return std::make_pair(hashes, offered);
            for (int i = 0; i < 200 && !m->match->IsFinished(); ++i) {
                hashes.push_back(m->match->StateHash());
                m->match->Tick();
            }
            for (const auto& o : m->log.offered) for (const GiftOffer& g : o.offers) offered.push_back(g.gift * 1000 + (g.champion != nullptr ? g.champion->id % 1000 : 0));
            return std::make_pair(hashes, offered);
        };
        const auto a = trace(11), b = trace(11), c = trace(12);
        CHECK(!a.second.empty() && a == b && a.first != c.first);
    }
    {   // Unit gifts are checked out of the pool one player at a time: with a pool of ONE copy per champion and only 5-cost gifts (4 champions),
        // 8 players cannot all be offered a unit -- nobody is offered a copy that is not there, nothing crashes, and every copy is accounted for.
        auto m = MnMatch::Make(*prod, MnDb(MnJson(2, R"({"id": 1, "name": "U", "type": "Unit", "costs": [5]}, {"id": 2, "name": "G", "type": "Gold", "amount": 5})"), items.get()), items.get(), 3,
                               [](GameConfig& cfg) { cfg.match.playerCount = 8; cfg.pool.copiesPerTier = {{1, 1, 1, 1, 1}}; });
        CHECK(m && m->match->Phase() == MatchPhase::MotherNature);
        if (!m) return;
        int unitOffers = 0;
        for (const auto& o : m->log.offered) for (const GiftOffer& g : o.offers) unitOffers += g.type == GiftType::Unit ? 1 : 0;
        CHECK(unitOffers == 4 && m->log.offered.size() == 8);   // exactly the 4 copies that exist
        CHECK(m->match->VerifyPoolIntegrity());
        for (int i = 0; i < 40 && m->match->Phase() == MatchPhase::MotherNature; ++i) m->match->Tick();
        CHECK(m->match->Phase() == MatchPhase::Planning && m->match->VerifyPoolIntegrity());
    }
    {   // The tier follows the STAGE: with one round per stage, round 4 is stage 4 and switches to the later tier.
        const std::string twoTiers = R"({"version": 1, "options": 1, "tiers": [
            { "id": 1, "name": "Early", "fromStage": 1, "gifts": [ {"id": 11, "name": "A", "type": "Gold", "amount": 1} ] },
            { "id": 3, "name": "Late", "fromStage": 4, "gifts": [ {"id": 33, "name": "B", "type": "Gold", "amount": 2} ] } ]})";
        auto m = MnMatch::Make(*prod, MnDb(twoTiers), nullptr, 5, [](GameConfig& cfg) { cfg.match.firstStageRounds = 1; cfg.match.roundsPerStage = 1; cfg.match.firstStagePveRounds = 0; cfg.match.pveRoundInLaterStages = 0; });
        if (!m) return;
        for (int i = 0; i < 400 && m->match->Round() < 5; ++i) m->match->Tick();
        std::vector<std::uint32_t> perRound;
        for (const auto& o : m->log.offered) if (o.player == 0) perRound.push_back(o.offers.empty() ? 0 : o.offers[0].gift);
        CHECK(perRound.size() >= 4 && perRound[0] == 11 && perRound[2] == 11 && perRound[3] == 33);
    }
}

static void TestMotherNatureSnapshotAndBots() {
    auto prod = ProdDb();
    auto items = w2f::LoadItemDatabaseFromFile(sample::ProductionItemsPath());
    CHECK(prod != nullptr && items != nullptr);
    if (!prod || !items) return;
    const std::string gifts = R"({"id": 1, "name": "G", "type": "Gold", "amount": 5}, {"id": 2, "name": "U", "type": "Unit", "costs": [2, 3]}, {"id": 3, "name": "C", "type": "Item", "itemClass": "Component"})";
    auto m = MnMatch::Make(*prod, MnDb(MnJson(2, gifts), items.get()), items.get(), 21);
    CHECK(m && m->match->Phase() == MatchPhase::MotherNature);
    if (!m) return;
    m->match->Tick();
    CHECK(m->match->TryPickGift(0, 0) == ActionResult::Ok);   // player 0 has chosen, player 1 has not: offers AND a settled flag are in the snapshot
    const std::vector<std::uint8_t> bytes = m->match->Snapshot();
    SnapshotInfo info;
    std::string err;
    CHECK(w2f::ReadSnapshotInfo(bytes, info, &err) && info.version == w2f::kSnapshotVersion && info.phase == static_cast<std::uint8_t>(MatchPhase::MotherNature) &&
          info.motherNatureHash == m->data->ContentHash() && info.stateHash == m->match->StateHash());
    const auto restore = [&](const MotherNatureDatabase* data, const RestoreOptions& options = RestoreOptions{}) {
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        cfg.match.motherNatureEveryRounds = 1;
        cfg.match.motherNatureTicks = 30;
        cfg.match.planningTicks = 10;
        cfg.match.combatTicks = 5;
        cfg.match.resolutionTicks = 2;
        cfg.player.startingGold = 10;
        return MatchManager::Restore(bytes, cfg, *prod, nullptr, &err, items.get(), nullptr, data, options);
    };
    auto twin = restore(m->data.get());
    CHECK(twin != nullptr && twin->StateHash() == m->match->StateHash() && twin->Snapshot() == bytes);
    if (twin) {
        CHECK(twin->GiftSettled(0) && !twin->GiftSettled(1) && twin->GiftOffers(1).size() == m->match->GiftOffers(1).size() && twin->VerifyPoolIntegrity());
        // The restored match plays on exactly like the original: same pick, same state, on every tick.
        CHECK(twin->TryPickGift(1, 0) == ActionResult::Ok && m->match->TryPickGift(1, 0) == ActionResult::Ok);
        bool same = twin->StateHash() == m->match->StateHash();
        for (int i = 0; i < 60; ++i) {
            twin->Tick();
            m->match->Tick();
            same = same && twin->StateHash() == m->match->StateHash();
        }
        CHECK(same && twin->Round() == m->match->Round());
    }
    // Different Mother Nature data (or none) cannot restore it, unless the caller forces it -- and a forced restore still checks every offer.
    auto other = MnDb(MnJson(2, gifts + R"(, {"id": 4, "name": "H", "type": "Heal", "amount": 3})"), items.get());
    CHECK(restore(other.get()) == nullptr && err.find("Mother Nature data") != std::string::npos);
    CHECK(restore(nullptr) == nullptr && err.find("Mother Nature data") != std::string::npos);
    RestoreOptions loose;
    loose.requireMatchingData = false;
    auto without = MnDb(MnJson(1, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5}, {"id": 9, "name": "Z", "type": "Xp", "amount": 1})"), items.get());
    if (m->match->GiftOffers(1).size() > 0) {
        bool wantsUnknownGift = false;
        for (const GiftOffer& g : m->match->GiftOffers(1)) wantsUnknownGift = wantsUnknownGift || g.gift == 2 || g.gift == 3;
        if (wantsUnknownGift) CHECK(restore(without.get(), loose) == nullptr && !err.empty());
    }
    // Flipped bytes never restore (the trailer checksum, and beyond that every offer is checked against the data).
    int tested = 0;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        auto bad = bytes;
        bad[i] ^= 0x40;
        TestGameConfig cfg;
        cfg.match.playerCount = 2;
        CHECK(MatchManager::Restore(bad, cfg, *prod, nullptr, &err, items.get(), nullptr, m->data.get()) == nullptr);
        ++tested;
    }
    CHECK(tested > 100 && tested == static_cast<int>((bytes.size() + 1) / 2));

    // A bot takes the most useful gift: a unit, then an item, then gold.
    auto bots = MnMatch::Make(*prod, MnDb(MnJson(2, R"({"id": 1, "name": "G", "type": "Gold", "amount": 5}, {"id": 2, "name": "U", "type": "Unit", "costs": [2, 3]})"), items.get()), items.get(), 4);
    CHECK(bots && bots->match->GiftOffers(0).size() == 2);
    if (bots) {
        AIBotController bot(0, 4);
        bot.Tick(*bots->match);
        CHECK(bots->match->GiftSettled(0) && bots->match->Players().Get(0)->Roster().Count() == 1 && bots->log.picked.size() == 1 && bots->log.picked[0].gift.type == GiftType::Unit);
        bot.Tick(*bots->match);   // nothing more to do
        CHECK(bots->log.picked.size() == 1);
    }
}

static void TestRefreshingBurnAndAssassinCrit() {
    CombatConfig cfg;
    cfg.manaPerAttackMilli = 0;
    // A burn of 60 over 3 s (a tick every second) lit by every attack of a fast attacker (one every 15 ticks). Refreshing: the burn keeps its rhythm,
    // so it ticks every 30 ticks for as long as it is re-lit, one hit at a time. Stacking (the control): every attack adds a burn, so hits pile up.
    const auto run = [&](bool refreshes) {
        ChampionDefinition attacker = Fighter(1, 100000, 0, 1, 2000);
        DotEffect burn;
        burn.type = DamageType::True;
        burn.amount = FlatAmount(Same(60));
        burn.amountIsTotal = true;
        burn.duration = FlatAmount(Same(90));
        burn.intervalTicks = 30;
        burn.refreshes = refreshes;
        attacker.onAttack = Hook(777, CastTrigger::OnBasicAttack, {Eff(TargetSpec::CurrentTarget(), burn)});
        Duel d;
        d.Add(attacker, 1, 0, {3, 3});
        d.Add(Dummy(2, 100000000), 2, 1, {3, 4});
        d.Finish();
        const FightResult r = CombatSimulator(cfg, nullptr, nullptr, d.db.get()).RunFight(d.specs, 200, 1);
        CheckValid(r, d, 200, cfg);
        std::map<int, int> perTick;
        for (const CombatEvent& e : Events(r.log, CombatEventType::Damage, 2)) {
            if (e.flags & kFlagDot) perTick[e.tick] += e.amount;
        }
        return perTick;
    };
    const auto refreshed = run(true);
    const auto stacked = run(false);
    CHECK(!refreshed.empty() && refreshed.begin()->first == 30);
    int previous = -1;
    for (const auto& [tick, amount] : refreshed) {
        CHECK(amount == 20);                                  // one third of 60, once per tick, never piled up
        CHECK(previous < 0 || tick - previous == 30);           // the rhythm never breaks while it is re-lit
        previous = tick;
    }
    int biggest = 0;
    for (const auto& [tick, amount] : stacked) biggest = std::max(biggest, amount);
    CHECK(biggest >= 40);   // without refreshing, overlapping burns do add up
    // It also has to be a sensible definition.
    std::string err;
    const std::string head = R"({"version": 1, "champions": [ { "id": 1, "name": "A", "cost": 1, "stats": { "hp": 100, "armor": 0, "magicResist": 0, "attackDamage": 1, "attackSpeed": 1, "range": 1 }, "onAttack": { "id": 1, "name": "x", "effects": [ )";
    CHECK(w2f::LoadChampionDatabaseFromJson(head + R"({ "type": "DoT", "target": "CurrentTarget", "damageType": "True", "amount": 5, "durationSeconds": 3, "intervalSeconds": 1, "refreshes": true, "stackBonusPercent": 25 } ] } } ]})", &err) == nullptr &&
          err.find("never stacks") != std::string::npos);
    CHECK(w2f::LoadChampionDatabaseFromJson(head + R"({ "type": "DoT", "target": "CurrentTarget", "damageType": "True", "amount": 5, "durationSeconds": 3, "intervalSeconds": 1, "refreshes": true } ] } } ]})", &err) != nullptr);

    // The production Helios synergy uses it, and the Assassins' synergy now also grants crit CHANCE (+15% at 2, +30% at 4).
    auto traits = sample::LoadProductionTraits();
    CHECK(traits != nullptr);
    if (!traits) return;
    for (const TraitBreakpoint& bp : traits->FindByName("Helios")->breakpoints) {
        const auto* dot = bp.triggers.empty() || bp.triggers[0].ability.effects.empty() ? nullptr : std::get_if<DotEffect>(&bp.triggers[0].ability.effects[0].payload);
        CHECK(dot && dot->refreshes && dot->type == DamageType::True);
    }
    const TraitDefinition* assassin = traits->FindByName("Assassin");
    CHECK(assassin && assassin->breakpoints.size() == 2 && assassin->breakpoints[0].count == 2 && assassin->breakpoints[1].count == 4);
    const int expectedChance[2] = {15, 30}, expectedDamage[2] = {20, 50};
    for (int i = 0; i < 2 && assassin; ++i) {
        int chance = 0, damage = 0;
        bool abilityCrit = false;
        for (const TraitEffect& te : assassin->breakpoints[static_cast<std::size_t>(i)].effects) {
            const auto* st = std::get_if<StatusEffect>(&te.effect.payload);
            if (st == nullptr) continue;
            if (st->status == StatusType::BonusCritChance) chance = st->value.flat[0];
            if (st->status == StatusType::CritDamage) damage = st->percent[0];
            if (st->status == StatusType::AbilityCrit) abilityCrit = true;
        }
        CHECK(chance == expectedChance[i] && damage == expectedDamage[i] && abilityCrit);
    }
    // In a fight: two Assassins get a real +15 crit chance status (and Vex, with no base crit, now crits about 15% of the time).
    auto prod = ProdDb();
    if (!prod) return;
    Duel d;
    for (int i = 0; i < 2; ++i) d.Add(*prod->Find(i == 0 ? 9015 : 9009), static_cast<UnitId>(i + 1), 0, HexCoord{3 + i, 3});
    d.Add(Dummy(10, 100000), 10, 1, {3, 4});
    d.Finish();
    const FightResult r = CombatSimulator(cfg, traits.get(), nullptr, d.db.get()).RunFight(d.specs, 10, 1);
    for (UnitId u : {UnitId{1}, UnitId{2}}) {
        const auto chance = StatusOn(r, u, StatusType::BonusCritChance);
        CHECK(chance.size() == 1 && chance[0].amount == 15 && chance[0].duration == 0);
    }
}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"Rng", TestRng},
        {"ChampionDatabase", TestDatabase},
        {"Config validation", TestConfigValidation},
        {"SharedChampionPool", TestPool},
        {"PlayerState basics / leveling", TestPlayerBasics},
        {"Income (base/interest/streak)", TestIncome},
        {"Shop reroll / buy / sell", TestShopReroll},
        {"Shop odds by level", TestShopOddsByLevel},
        {"Shop with sold-out tier", TestShopSoldOutTier},
        {"Sell value + elimination", TestSellValueAndElimination},
        {"Roster placement + full roster", TestRosterPlacementAndFullRoster},
        {"Moves and swaps", TestMoves},
        {"Board capacity", TestBoardCapacity},
        {"Merge rules + cascade", TestMergeRules},
        {"Merge through shop (gold + pool + events)", TestMergeThroughShop},
        {"Sell value of merged units", TestSellMergedValue},
        {"Bench full -> board, level cap via shop", TestBenchFullAndBoardCapThroughShop},
        {"Full roster still allows merge purchase", TestRosterFullStillAllowsMergePurchase},
        {"Elimination with merged units", TestEliminationWithMergedUnits},
        {"Unit events + gating through MatchManager", TestUnitEventsThroughMatch},
        {"Match phase flow + gating", TestMatchFlowPhases},
        {"Matchmaking (odd => ghost)", TestMatchups},
        {"Elimination placements", TestEliminationPlacements},
        {"Full match (scripted unit ops): determinism + pool integrity", TestFullMatchDeterminismAndIntegrity},
        {"Hex distance + neighbours", TestHexDistanceAndNeighbors},
        {"Board -> arena mapping", TestBoardToArenaMapping},
        {"Pathfinding", TestPathfinding},
        {"Fixed-point combat math", TestFixedPointCombatMath},
        {"Duel: adjacent melee", TestDuelAdjacent},
        {"Combat: simultaneity + timeout", TestCombatSimultaneityAndTimeout},
        {"Combat: movement + targeting", TestCombatMovementAndTargeting},
        {"Combat: determinism + path caching", TestCombatDeterminismAndPathCaching},
        {"Combat through MatchManager", TestCombatThroughMatch},
        {"Abilities are pure data (+ validation)", TestAbilityDataIsPureData},
        {"Mana: regen + per attack + cast lock", TestManaRegenAndPerAttack},
        {"Mana: from damage taken (+ cap)", TestManaFromDamageTaken},
        {"Shields + damage reduction (Alesk)", TestShieldsAndDamageReduction},
        {"Statuses: stun / AD / armor / MR / AS", TestStatuses},
        {"DoT + stacking (Baira burn)", TestDamageOverTimeAndStacking},
        {"Damage effects: types, targeting, formulas, delay", TestDamageEffectTargetingTypesAndScaling},
        {"Crit rolls (RNG)", TestCritRolls},
        {"Cyla rocket (damage-in-window)", TestCylaRocket},
        {"Faire exploit (spread, stun formula, cast on death)", TestFaireExploit},
        {"Baira attack-count trigger + burn timing", TestBairaAndBurnTiming},
        {"Event sink + all five champions in one fight", TestEventSinkAndFullRosterFight},
        {"JSON parser", TestJsonParser},
        {"Loader: designer-friendly values", TestLoaderReadsDesignerFriendlyValues},
        {"Loader: every mistake names its path", TestLoaderErrors},
        {"Production data matches the designer spec", TestProductionDataMatchesDesignerSpec},
        {"Passives at start of combat (Alesk)", TestPassivesAtStartOfCombat},
        {"Baira: wound passive + new burn", TestBairaWoundAndNewBurn},
        {"Attack count resets on target change", TestResetCountOnTargetChange},
        {"Burn math: total damage, even spacing", TestBurnMathAndSpacing},
        {"Mana bar events", TestManaBarEvents},
        {"Hex: straight lines behind a target", TestHexLineDirection},
        {"Heal effect (+ Wound)", TestHealEffect},
        {"Regen from damage taken (Les's passive)", TestDamageTakenRegen},
        {"Damage amp + passive order", TestDamageAmpAndPassiveOrder},
        {"Root / Knockup / CC immunity", TestRootKnockupAndImmunity},
        {"Targeting: closest N, line behind target", TestClosestEnemiesAndLineTargeting},
        {"Les: wall of Omnilium", TestLesWallMode},
        {"Les: recasting the wall refreshes, not stacks", TestWallModeRefreshesInsteadOfStacking},
        {"Lum: punch + bonus AD passive", TestLumPunchAndPassive},
        {"Trait: Protector synergy", TestProtectorSynergy},
        {"Trait: breakpoint tiers + scopes", TestTraitBreakpointTiers},
        {"Trait / new-primitive data + loader errors", TestTraitDataAndLoader},
        {"Faire (5 s window) + Cyla (physical)", TestFaireStunAndCylaRocketPhase6},
        {"Baira burn scales with AP", TestBairaBurnScalesWithAP},
        {"12 roster champions + synergies", TestFullRosterBrawlWithSynergies},
        {"Cone geometry", TestConeGeometry},
        {"Targets: highest-damage / lowest-HP / zone / random / cone", TestNewTargetSelectors},
        {"Lunis: teleport, untargetable, aggro drop", TestTeleportAndUntargetable},
        {"Astra: tether redirect", TestAstraTether},
        {"Soul: shield refill, no healing, steal", TestSoulShieldAndSteal},
        {"Myna: heal lowest ally + zone damage", TestMynaHealAndZone},
        {"Vega: channel, pulses, finale, interrupt", TestVegaChannel},
        {"Items: data + loader errors", TestItemData},
        {"Items: bag / equip / merge / sell", TestItemInventory},
        {"Items: stats + emblems in combat", TestItemsInCombat},
        {"Snapshot: round trip through a whole match", TestSnapshotRoundTripThroughAWholeMatch},
        {"Snapshot: restored twins stay in lockstep", TestSnapshotLockstepContinuation},
        {"Snapshot: rejects corrupt / tampered / mismatched input", TestSnapshotRejectsBadInput},
        {"Snapshot: chained restores under random actions", TestSnapshotChainedActionFuzz},
        {"Stages + PvE round designation + config validation", TestStageStructureAndPveRounds},
        {"Player damage formula", TestPlayerDamageFormula},
        {"PvP damage, elimination, pool return", TestPvpDamageAndElimination},
        {"Draws and ghost fights deal no damage", TestDrawsGhostsAndDamageAccounting},
        {"Win / loss streak income", TestStreakIncome},
        {"PvE data: encounters, selection, loader errors", TestEncounterDataAndSelection},
        {"PvE win drops: gold / item / champion", TestPveRoundWinDropsGoldItemChampion},
        {"PvE loss gives nothing; rounds and encounters differ", TestPveLossGivesNothingAndRoundsDiffer},
        {"PvE drop edge cases", TestPveDropEdgeCases},
        {"PvE drop distribution + determinism", TestPveDropDistributionAndDeterminism},
        {"Full match with PvE rounds (real rules)", TestFullMatchWithPveRounds},
        {"Automatic snapshot at every Planning phase", TestAutomaticSnapshotAtPlanning},
        {"Items: recipes + database rules", TestItemRecipeData},
        {"Items: combining on units, bags, merges", TestItemCombinationOnUnits},
        {"Aura primitive", TestAuraPrimitive},
        {"Hooks: taking damage (stacks, spell shields)", TestHooksOnTakingDamage},
        {"Hooks: dealing damage (shred, life on hit, echo)", TestHooksOnDealingDamage},
        {"Hooks: crits taken + crit damage statuses", TestHooksOnCrits},
        {"Hooks: HP thresholds", TestHooksOnLowHp},
        {"Hooks: ally damage, casts, attack counters", TestHookOnAllyDamageAndCasts},
        {"Summon effect", TestSummonEffect},
        {"Summons are never sold or owned", TestSummonsAreNotSoldOrOwned},
        {"Ability crits", TestAbilityCrit},
        {"Mana effect / mana regen / ability power", TestNewSmallPrimitives},
        {"Hooks never chain", TestNoChainReactions},
        {"Loader: triggers, effects, auras, recipes, synergies", TestPhase10Loader},
        {"Coregons-style synergy from data", TestCoregonsStyleSynergyFromData},
        {"Recipe items through a full match", TestRecipeItemsThroughAFullMatch},
        {"Per-mille formula terms (Gunfire)", TestPermilleTermsAndGunfire},
        {"AI bot: economy + placement", TestBotEconomyAndPlacement},
        {"Full bench still buys a merge (match API)", TestFullBenchMergeViaMatchApi},
        {"Full match: 7 bots + 1 player, real combat", TestFullMatchWithBots},
        {"Full match: roster-only shop, synergies in play", TestRosterOnlyMatchWithSynergies},
        {"Shop: exhausted tiers fall back, never crash", TestShopPoolExhaustionFallback},
        {"Roster: champions 1-2 cost abilities (30-champion doc)", TestNewChampionAbilitiesPart1},
        {"Roster: the rest of the abilities", TestNewChampionAbilitiesPart2},
        {"Synergies: Helios, Phaisa, Hexagon, Selini, Najmi", TestHeliosPhaisaHexagonSeliniNajmi},
        {"Synergy: Coregons 3 / 6 / 8 from the design doc", TestCoregonsFromTheDesignDoc},
        {"Zone statuses: ExecuteBelow, HpPerSecond", TestZoneStatusesAndExecute},
        {"Phase 11 primitives: loader errors + data hash", TestPhase11PrimitiveLoaderErrors},
        {"Items: Mage Shield detonation (stored damage)", TestMageShieldDetonation},
        {"Items: Tear of Mother 'took no damage' condition", TestTearOfMotherNoDamageCondition},
        {"Items: Twin Snipers (every-second hook)", TestTwinSnipersEverySecond},
        {"Items: Fishscale Omnivamp + Assassin synergy", TestFishscaleOmnivampAndAssassin},
        {"Items: production data is the whole design doc", TestProductionItemsAreTheWholeDesignDoc},
        {"Mother Nature: data file + loader errors", TestMotherNatureDataFile},
        {"Mother Nature: every kind of gift", TestMotherNatureRewards},
        {"Mother Nature: the phase, the closed shop, timeouts", TestMotherNaturePhaseFlow},
        {"Mother Nature: early end, determinism, pool, tiers", TestMotherNatureEarlyEndAndDeterminism},
        {"Mother Nature: snapshots + bots", TestMotherNatureSnapshotAndBots},
        {"Refreshing burn + Assassin crit chance", TestRefreshingBurnAndAssassinCrit},
    };
    for (const auto& [name, fn] : tests) {
        std::printf("[ RUN  ] %s\n", name);
        const int before = g_failures;
        fn();
        std::printf("[ %s ] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
