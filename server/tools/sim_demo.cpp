// Headless demo: plays a full 8-player match (seat 0 = scripted "human", seats 1-7 = AI bots)
// with real combat, and prints how it went. The champion roster is read from JSON at start-up.
// Usage: w2f_demo [seed] [champions.json] [traits.json] [items.json] [pve.json] [mother_nature.json]
// The demo also drills crash recovery: at round 10 it throws the running match away and carries on from the automatic snapshot
// taken when that round's Planning phase began. Set W2F_NO_DRILL=1 to skip the drill; the final state hash must be the same
// either way (that is the whole point of a safety net). Set W2F_ROSTER_ONLY=1 to sell only the real roster (no generic fillers).

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "SampleData.h"
#include "ScriptedPlayer.h"
#include "w2f/AIBotController.h"
#include "w2f/CombatEventSink.h"
#include "w2f/ChampionLoader.h"
#include "w2f/CombatSimulator.h"
#include "w2f/Hash.h"
#include "w2f/MatchManager.h"
#include "w2f/Trait.h"

using namespace w2f;

namespace {
// A viewer-style sink: prints the "interesting" events of one fight, using the typed callbacks a
// UE5 wrapper would implement (each would queue an animation / particle effect for its tick).
struct PrintSink : ICombatEventSink {
    int printed = 0;
    static constexpr int kMax = 16;
    static const char* Status(StatusType t) {
        switch (t) {
            case StatusType::Stun: return "STUN";
            case StatusType::Burn: return "BURN";
            case StatusType::AttackDamage: return "AD%";
            case StatusType::AttackSpeed: return "AS%";
            case StatusType::Armor: return "ARMOR%";
            case StatusType::MagicResist: return "MR%";
            case StatusType::MaxHp: return "MAX-HP%";
            case StatusType::Wound: return "WOUND";
            case StatusType::InflictsWound: return "INFLICTS-WOUND";
            case StatusType::DamageAmp: return "DMG-AMP%";
            case StatusType::Root: return "ROOT";
            case StatusType::Knockup: return "KNOCKUP";
            case StatusType::CcImmunity: return "CC-IMMUNE";
            case StatusType::DamageTakenRegen: return "REGEN-ON-HIT%";
            case StatusType::BonusAttackDamage: return "BONUS-AD";
            case StatusType::Tether: return "TETHER%";
            case StatusType::Untargetable: return "UNTARGETABLE";
            case StatusType::AggroDrop: return "AGGRO-DROP";
            case StatusType::BonusArmor: return "BONUS-ARMOR";
            case StatusType::BonusMagicResist: return "BONUS-MR";
            case StatusType::BonusMaxHp: return "BONUS-HP";
            case StatusType::BonusAbilityDamage: return "BONUS-AP";
            case StatusType::BonusCritChance: return "BONUS-CRIT";
            case StatusType::AbilityCrit: return "ABILITY-CRIT";
            case StatusType::CritDamage: return "CRIT-DAMAGE";
            case StatusType::CritDamageTakenReduction: return "CRIT-REDUCTION";
            case StatusType::BonusManaRegen: return "BONUS-MANA-REGEN";
            case StatusType::AbilityPower: return "AP%";
            case StatusType::SpellShield: return "SPELL-SHIELD";
            case StatusType::Blind: return "BLIND";
            case StatusType::DamageTaken: return "DMG-TAKEN%";
            case StatusType::BonusMaxMana: return "BONUS-MAX-MANA";
            case StatusType::ExecuteBelow: return "EXECUTE-BELOW%";
            case StatusType::HpPerSecond: return "HP-PER-SECOND%";
            case StatusType::EmpoweredAttack: return "EMPOWERED-ATTACKS";
        }
        return "?";
    }
    bool Room() { return printed++ < kMax; }
    void OnSpellCast(int tick, UnitId caster, AbilityId spell, UnitId target, int lock, bool onDeath) override {
        if (Room()) std::printf("     t=%4d  unit %08x casts spell #%u on %08x (lock %d ticks)%s\n", tick, caster, spell, target, lock, onDeath ? "  [dying breath]" : "");
    }
    void OnShieldApplied(int tick, UnitId unit, int amount, int duration) override {
        if (Room()) std::printf("     t=%4d  unit %08x gains a %d-point shield for %d ticks\n", tick, unit, amount, duration);
    }
    void OnStatusApplied(int tick, UnitId unit, StatusType status, int duration, int percent, UnitId) override {
        if (!Room()) return;
        if (duration == 0) std::printf("     t=%4d  unit %08x has passive status %s %+d%% (permanent)\n", tick, unit, Status(status), percent);
        else std::printf("     t=%4d  unit %08x is %s for %d ticks%s\n", tick, unit, Status(status), duration, percent ? " (modifier)" : "");
    }
    void OnHeal(int tick, UnitId unit, UnitId source, int amount, int reduced, int hpAfter) override {
        if (Room()) std::printf("     t=%4d  unit %08x heals %d%s (hp %d) from %08x\n", tick, unit, amount, reduced ? " [wounded]" : "", hpAfter, source);
    }
    void OnTraitActivated(int tick, int team, std::uint32_t traitId, int count, int breakpoint) override {
        if (Room()) std::printf("     t=%4d  team %d: synergy #%u active (%d champions, breakpoint %d)\n", tick, team, traitId, count, breakpoint);
    }
    void OnManaChanged(int tick, UnitId unit, int manaMilli) override {
        if (manaShown++ < 4 && Room()) std::printf("     t=%4d  unit %08x mana bar -> %.1f\n", tick, unit, manaMilli / 1000.0);
    }
    int manaShown = 0;
    void OnDamage(const CombatEvent& e) override {
        if ((e.flags & kFlagCrit) && Room()) std::printf("     t=%4d  CRIT! %d damage to unit %08x\n", e.tick, e.amount, e.unit);
    }
};

struct Reporter : IMatchListener {
    int printedFights = 0;
    int casts = 0;
    int pveFights = 0, pveWins = 0, drops[4] = {};
    int autoSnapshots = 0;
    std::size_t largestSnapshot = 0;
    void OnPveDrop(PlayerId, const PveDrop& drop) override { ++drops[static_cast<int>(drop.type)]; }
    int gifts[5] = {};   // Mother Nature gifts taken, by GiftType
    int giftsAutomatic = 0, giftsConverted = 0;
    void OnGiftPicked(PlayerId, int, const GiftOffer& gift, bool automatic, int goldConverted) override {
        ++gifts[static_cast<int>(gift.type)];
        giftsAutomatic += automatic ? 1 : 0;
        giftsConverted += goldConverted > 0 ? 1 : 0;
    }
    void OnAutoSnapshot(int, const std::vector<std::uint8_t>& bytes) override {
        ++autoSnapshots;
        if (bytes.size() > largestSnapshot) largestSnapshot = bytes.size();
    }
    void OnPlayerEliminated(PlayerId p, int placement) override {
        std::printf("  seat %d eliminated, placement %d\n", p, placement);
    }
    void OnCombatSimulated(int round, const CombatOutcome& o) override {
        if (o.matchup.awayIsMonsters) {
            ++pveFights;
            pveWins += o.winner == CombatWinner::Home ? 1 : 0;
        }
        for (const CombatEvent& e : o.log.events) casts += e.type == CombatEventType::SpellCast ? 1 : 0;
        bool interesting = false;   // a synergy, a heal or a spell
        for (const CombatEvent& e : o.log.events) {
            interesting = interesting || e.type == CombatEventType::TraitActivated || e.type == CombatEventType::Heal || e.type == CombatEventType::SpellCast;
        }
        if (round < 8 || !interesting || printedFights++ != 0) return;  // show one fight's stream, once
        std::printf("\n  -- fight, round %d: seat %d vs seat %d, %zu events, winner %s, ends tick %d, checksum %016llx\n",
                    round, o.matchup.home, o.matchup.away, o.log.events.size(),
                    o.winner == CombatWinner::Home ? "home" : o.winner == CombatWinner::Away ? "away" : "draw",
                    o.log.endTick, static_cast<unsigned long long>(o.log.checksum));
        std::printf("     spawn-time passives, mana bar syncs, spells, shields, statuses and crits, replayed through ICombatEventSink:\n");
        PrintSink sink;
        ReplayCombatLog(o.log, sink);
        std::printf("     ...\n\n");
    }
};
}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2024;

    // Server start-up: load the champion data. A bad file is reported with its exact location and stops the server.
    const std::string dataPath = argc > 2 ? argv[2] : sample::ProductionDataPath();
    std::string loadError;
    auto roster = w2f::LoadChampionDatabaseFromFile(dataPath, &loadError);
    if (!roster) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }
    std::printf("Loaded %zu champions from %s:", roster->All().size(), dataPath.c_str());
    for (const ChampionDefinition& c : roster->All()) {
        std::printf(" %s", c.name.c_str());
        if (!c.traits.empty()) std::printf("[%s]", c.traits[0].c_str());
    }
    std::printf("\n");
    const std::string traitsPath = argc > 3 ? argv[3] : sample::ProductionTraitsPath();
    auto traits = w2f::LoadTraitDatabaseFromFile(traitsPath, &loadError);
    if (!traits || !w2f::ValidateChampionTraits(*roster, *traits, &loadError)) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }
    std::printf("Loaded %zu traits from %s\n", traits->All().size(), traitsPath.c_str());
    const std::string itemsPath = argc > 4 ? argv[4] : sample::ProductionItemsPath();
    auto items = w2f::LoadItemDatabaseFromFile(itemsPath, &loadError);
    if (!items || !w2f::ValidateItemTraits(*items, *traits, &loadError)) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }
    std::printf("Loaded %zu items from %s\n", items->All().size(), itemsPath.c_str());
    // W2F_ROSTER_ONLY=1: the shop sells only the real roster (all 30 champions, real synergies) instead of the roster plus 25 generic fillers.
    // The CI determinism check runs both.
    auto db = std::getenv("W2F_ROSTER_ONLY") != nullptr ? w2f::LoadChampionDatabaseFromFile(dataPath, &loadError) : sample::MakeCombatDatabase(dataPath);
    if (!db) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }
    const std::string pvePath = argc > 5 ? argv[5] : sample::ProductionPvePath();
    auto encounters = w2f::LoadEncounterDatabaseFromFile(pvePath, db.get(), items.get(), &loadError);
    if (!encounters) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }
    std::printf("Loaded %zu monsters and %zu encounters from %s\n", encounters->Monsters().All().size(), encounters->All().size(), pvePath.c_str());
    GameConfig cfg;
    cfg.player.startingGold = 10;
    cfg.player.limitBoardToLevel = true;  // TFT rule: fielded units <= level
    const auto makeSimulator = [&]() { return std::make_unique<CombatSimulator>(cfg.combat, traits.get(), items.get()); };
    const std::string naturePath = argc > 6 ? argv[6] : sample::ProductionMotherNaturePath();
    auto motherNature = w2f::LoadMotherNatureDatabaseFromFile(naturePath, items.get(), &loadError);
    if (!motherNature) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }
    std::printf("Loaded Mother Nature: %zu tiers, %d options a round, from %s\n", motherNature->Tiers().size(), motherNature->Options(), naturePath.c_str());
    auto match = MatchManager::Create(cfg, *db, seed, makeSimulator(), &loadError, items.get(), encounters.get(), motherNature.get());
    if (!match) {
        std::fprintf(stderr, "Cannot start: %s\n", loadError.c_str());
        return 2;
    }

    Reporter reporter;
    match->AddListener(&reporter);
    // The clients (seat 0 and the bots) are not part of a match snapshot, so the drill keeps copies of them as they were when
    // the snapshot was taken -- at that moment nobody had acted yet this round.
    struct Clients { sample::ScriptedPlayer human{0}; std::vector<AIBotController> bots; };
    sample::ScriptedPlayer human(0);
    std::vector<AIBotController> bots;
    for (int seat = 1; seat < kMaxPlayers; ++seat) bots.emplace_back(static_cast<PlayerId>(seat), seed, BotProfile{}, traits.get());
    Clients atSnapshot{human, bots};
    int snapshotRound = 0;
    bool drilled = false;
    const bool drill = std::getenv("W2F_NO_DRILL") == nullptr;

    std::printf("Match seed %llu: seat 0 scripted, seats 1-7 AI bots\n", static_cast<unsigned long long>(seed));
    match->Start();
    int lastRound = 0;
    int snapshotsChecked = 0;
    bool snapshotsOk = true;
    for (long tick = 0; !match->IsFinished() && tick < 5'000'000; ++tick) {
        human.Tick(*match);
        for (AIBotController& bot : bots) bot.Tick(*match);
        match->Tick();
        if (match->LastPlanningSnapshotRound() != snapshotRound) {   // a Planning phase just began: remember the clients as they were
            snapshotRound = match->LastPlanningSnapshotRound();
            atSnapshot = Clients{human, bots};
        }
        if (drill && !drilled && match->Round() == 10 && match->Phase() == MatchPhase::Combat) {
            // "The process crashed during round 10's fight." Rebuild everything from the safety net alone.
            drilled = true;
            const std::vector<std::uint8_t> safetyNet = match->LastPlanningSnapshot();
            auto recovered = MatchManager::Restore(safetyNet, cfg, *db, makeSimulator(), &loadError, items.get(), encounters.get(), motherNature.get());
            if (!recovered) {
                std::fprintf(stderr, "Crash drill FAILED: %s\n", loadError.c_str());
                return 3;
            }
            std::printf("Crash drill: match thrown away during round %d's fight; restored from the round-%d planning snapshot (%zu bytes) and carrying on\n",
                        match->Round(), recovered->Round(), safetyNet.size());
            recovered->AddListener(&reporter);
            match = std::move(recovered);
            human = atSnapshot.human;
            bots = atSnapshot.bots;
            lastRound = 0;
            continue;
        }

        // Every 2000 ticks: snapshot the whole match, restore it, and check the copy is the same match down to the byte.
        if (tick % 2000 == 0) {
            const std::vector<std::uint8_t> bytes = match->Snapshot();
            auto copy = MatchManager::Restore(bytes, cfg, *db, makeSimulator(), &loadError, items.get(), encounters.get(), motherNature.get());
            snapshotsOk = snapshotsOk && copy != nullptr && copy->StateHash() == match->StateHash() && copy->Snapshot() == bytes;
            ++snapshotsChecked;
        }

        if (match->Round() != lastRound && match->Phase() != MatchPhase::MatchOver) {
            lastRound = match->Round();
            if (lastRound % 5 == 0 || lastRound == 1) {
                std::printf("round %2d: alive %d |", lastRound, match->Players().AliveCount());
                for (int s = 0; s < kMaxPlayers; ++s) {
                    const PlayerState* p = match->Players().Get(static_cast<PlayerId>(s));
                    std::printf(" %s", p->IsAlive() ? "" : "x");
                    std::printf("%d/L%d/%dg", p->Health(), p->Level(), p->Gold());
                }
                std::printf("\n");
            }
        }
    }

    std::printf("\nFinished after round %d. Winner: seat %d. State hash %016llx\n", match->Round(), match->Winner(),
                static_cast<unsigned long long>(match->StateHash()));
    std::printf("Pool integrity: %s, roster layouts: %s\n", match->VerifyPoolIntegrity() ? "ok" : "BROKEN",
                match->VerifyRosterLayouts() ? "ok" : "BROKEN");
    std::printf("Mother Nature gifts taken: %d gold, %d xp, %d heal, %d item, %d unit (%d auto-picked, %d units paid as gold)\n", reporter.gifts[0], reporter.gifts[1],
                reporter.gifts[2], reporter.gifts[3], reporter.gifts[4], reporter.giftsAutomatic, reporter.giftsConverted);
    std::printf("PvE: %d fights, %d won; drops: %d gold, %d champion, %d item.  Automatic snapshots taken: %d (largest %zu bytes)\n",
                reporter.pveFights, reporter.pveWins, reporter.drops[1], reporter.drops[2], reporter.drops[3], reporter.autoSnapshots, reporter.largestSnapshot);
    std::printf("Snapshot / restore round trips checked during the match: %d, %s\n", snapshotsChecked, snapshotsOk ? "all identical" : "MISMATCH");
    {   // The final snapshot's fingerprint: identical across builds means snapshots are portable between them.
        const std::vector<std::uint8_t> bytes = match->Snapshot();
        Fnv1a h;
        for (std::uint8_t b : bytes) h.AddByte(b);
        std::printf("Final snapshot: %zu bytes, checksum %016llx\n", bytes.size(), static_cast<unsigned long long>(h.value));
    }
    std::printf("Spells cast over the whole match: %d\n", reporter.casts);
    std::printf("Final placements:");
    for (int place = 1; place <= kMaxPlayers; ++place) {
        for (int s = 0; s < kMaxPlayers; ++s) {
            if (match->Players().Get(static_cast<PlayerId>(s))->Placement() == place) std::printf("  #%d=seat %d", place, s);
        }
    }
    std::printf("\n");
    return match->IsFinished() && snapshotsOk ? 0 : 1;
}
