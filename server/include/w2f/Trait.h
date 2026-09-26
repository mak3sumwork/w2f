#pragma once

// Traits and synergies.
//
// A champion lists trait tags ("Helios", "Protector", ...). At the start of a fight, for each team,
// the simulator counts how many DIFFERENT champions (two copies of one champion count once) on the
// board have each trait, and for every trait picks the highest breakpoint whose `count` is reached
// (like TFT tiers: a higher breakpoint replaces the lower ones, it does not add to them). The
// effects of that breakpoint are then applied to the team, BEFORE passives fire.
//
// The effects are ordinary ability effects, restricted to the ones that make sense as team buffs
// (Status, Shield, Heal). Each one is applied to each qualifying unit as if that unit had cast it on
// itself; `scope` says who qualifies:
//   AllAllies     every fielded unit on the team
//   TraitHolders  only units that themselves have this trait
// Statuses of the same type add up, so "allies +10%" plus "holders +10%" gives holders +20%.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "w2f/Ability.h"
#include "w2f/ChampionDatabase.h"
#include "w2f/Unit.h"

namespace w2f {

using TraitId = std::uint32_t;

// AllAllies / TraitHolders: applied to each qualifying unit. Team: applied ONCE for the whole team (cast by its lowest-UnitId trait holder),
// aimed by the effect's own `target` (Self, AllEnemies or AllAllies): "summon 3 souls" (not "3 souls per holder"), or a zone that changes the
// whole enemy side. The caster is the source of whatever the effect does (a status's source, damage credit).
enum class TraitScope : std::uint8_t { AllAllies, TraitHolders, Team };

struct TraitEffect {
    TraitScope scope = TraitScope::AllAllies;
    AbilityEffect effect;  // target must be Self, except for scope Team (see above)
};

// A hook (see EventTrigger in Ability.h) the synergy attaches to every unit in scope for the fight: "Coregons heal for 10% of the
// damage they deal" is an OnDealDamage hook on the trait holders; "souls echo the team's damage" an OnAllyDealDamage hook on a summon.
// Scope Team (a trigger): the hook is handed to every holder, but each event fires it ONCE for the whole team -- on the first living holder
// (lowest UnitId). Helios's Rally smite: one smite per Rally, not one per Helios unit.
struct TraitTrigger {
    TraitScope scope = TraitScope::TraitHolders;   // AllAllies, TraitHolders or Team
    AbilityDefinition ability;
};

// ---- Trait system v2 (September 2026): the match-level parts of a breakpoint. The combat simulator ignores them; the MatchManager reads them. ----

// Star dust (Najmi): gained after a LOST player combat (onLoss + perLossStreak x the loss streak) and per takedown by a holder; x multiplier.
struct StarDustRule {
    int onLoss = 0;
    int perLossStreak = 0;
    int perTakedown = 0;
    int multiplier = 1;
    bool Any() const { return onLoss != 0 || perLossStreak != 0 || perTakedown != 0; }
};

// A unit the trait gives the player (on the bench) after `afterCombats` player combats fought with this breakpoint (or a higher one) active.
// Once per match. The Rift Herald.
struct UnitGrant {
    ChampionId champion = 0;
    int afterCombats = 0;
};

// Plants the breakpoint puts on the player's board (Nature): `count` of `champion` in total (not on top of lower breakpoints'), at `plantStar`.
struct PlantGrant {
    ChampionId champion = 0;
    int count = 0;
};

struct TraitBreakpoint {
    int count = 0;  // unique champions with the trait needed to activate
    std::vector<TraitEffect> effects;
    std::vector<TraitTrigger> triggers;
    // ---- v2 ----
    int mutationSlots = 0;        // Phaisa: how many holders carry a mutation (see TraitDefinition::mutations); -1 = all of them
    bool supercharge = false;     // mutations use their `super*` lists
    int xpAfterCombat = 0;        // XP the player gains after every player combat fought with this breakpoint active (Selini, Enlightenment)
    int takedownsPerGold = 0;     // 1 gold per this many takedowns by holders, kept across rounds (Selini, Prosperity)
    StarDustRule starDust;        // Najmi
    UnitGrant grantUnit;          // Phaisa's Rift Herald
    std::vector<PlantGrant> plants;   // Nature
    int plantStar = 0;            // Nature: plants' star level at this breakpoint (0 = 1)
    int moduleTier = 0;           // Hexagon: reaching this breakpoint offers a module of this tier (1..3)
};

// One of a trait's alternative rule sets. With paths, the MATCH picks one at random when it starts, for everybody (Selini).
struct TraitPath {
    std::string name;
    std::vector<TraitBreakpoint> breakpoints;
};

// Phaisa's mutations: the unit that gets a mutation slot gets the first mutation whose `classes` it carries (the last one is the fallback when its
// `classes` are empty). Its effects target Self; its triggers are hooks of that unit only.
struct TraitMutation {
    std::string name;
    std::vector<std::string> classes;
    std::vector<AbilityEffect> effects;
    std::vector<AbilityDefinition> triggers;
    std::vector<AbilityEffect> superEffects;        // used instead when the breakpoint supercharges
    std::vector<AbilityDefinition> superTriggers;
};

// Hexagon's modules: the Invention (a summon, `TraitDefinition::invention`) fires the chosen modules' abilities 8 s into every fight.
struct TraitModule {
    std::uint32_t id = 0;
    std::string name;
    int tier = 1;                 // 1..3: offered on reaching the breakpoint with this moduleTier
    AbilityDefinition ability;    // run by the Invention as a passive (effects carry their own delay); may be empty for purely economic modules
    int goldAfterCombat = 0;      // Mining Drill
    bool echo = false;            // Echo Engine: the Invention repeats the other modules every `echoEveryTicks`
    int echoEveryTicks = 0;
};

// The Phaisa Queen: offered in the shop to a player with `uniqueHolders` different holders on the board at `level` or higher.
struct QueenRule {
    ChampionId champion = 0;
    int uniqueHolders = 0;
    int level = 0;
};

struct TraitDefinition {
    TraitId id = 0;  // non-zero, unique; appears in the event stream
    std::string name;
    std::vector<TraitBreakpoint> breakpoints;  // strictly ascending count; may be empty (a tag with no synergy yet). Empty when `paths` is used
    std::vector<TraitPath> paths;              // alternative rule sets (same counts in each); the match picks one
    std::vector<TraitMutation> mutations;
    std::vector<TraitModule> modules;
    ChampionId invention = 0;                  // the summon that carries the chosen modules (Hexagon)
    QueenRule queen;

    // The breakpoints in force for a match that picked path `path` (ignored when the trait has no paths).
    const std::vector<TraitBreakpoint>& Breakpoints(int path = 0) const {
        if (paths.empty()) return breakpoints;
        const std::size_t i = path >= 0 && static_cast<std::size_t>(path) < paths.size() ? static_cast<std::size_t>(path) : 0;
        return paths[i].breakpoints;
    }
    const TraitModule* FindModule(std::uint32_t moduleId) const {
        for (const TraitModule& m : modules) {
            if (m.id == moduleId) return &m;
        }
        return nullptr;
    }
};

// Which breakpoint `count` holders reach: 1-based tier, 0 = none.
int ActiveTier(const std::vector<TraitBreakpoint>& breakpoints, int count);

class TraitDatabase {
public:
    // Validates ids / names / breakpoints / effects. nullptr and *error on failure.
    static std::unique_ptr<TraitDatabase> Create(std::vector<TraitDefinition> definitions, std::string* error = nullptr);

    TraitDatabase(const TraitDatabase&) = delete;
    TraitDatabase& operator=(const TraitDatabase&) = delete;

    const std::vector<TraitDefinition>& All() const { return definitions_; }  // sorted by id
    const TraitDefinition* FindByName(const std::string& name) const;
    const TraitDefinition* FindById(TraitId id) const;

private:
    explicit TraitDatabase(std::vector<TraitDefinition> definitions) : definitions_(std::move(definitions)) {}
    std::vector<TraitDefinition> definitions_;
};

// Every trait tag a champion lists must be declared in the trait data, so a typo in one file
// ("Protecter") is caught at start-up instead of silently giving no synergy.
bool ValidateChampionTraits(const ChampionDatabase& champions, const TraitDatabase& traits, std::string* error = nullptr);

class ItemDatabase;

// One unit as trait counting sees it: its champion and the trait tags its items grant.
struct TraitCountUnit {
    const ChampionDefinition* champion = nullptr;
    int starLevel = 1;
    std::vector<std::string> extraTraits;
};
bool UnitHasTrait(const TraitCountUnit& unit, const std::string& trait);
// How many DIFFERENT champions among `units` carry `trait` (a champion counts if any of its copies does). Summons and plants never count.
int CountTraitHolders(const std::vector<TraitCountUnit>& units, const std::string& trait);
// The units' trait-count view of a roster's BOARD (items resolve through `items`, which may be null).
std::vector<TraitCountUnit> BoardTraitUnits(const std::vector<UnitInstance>& roster, const ItemDatabase* items);

// Every SummonEffect anywhere (items, synergies) must name a champion that exists and is marked `summon` (champions are checked by
// ChampionDatabase::Create itself). Either optional database may be null.
bool ValidateSummonReferences(const ChampionDatabase& champions, const ItemDatabase* items, const TraitDatabase* traits, std::string* error = nullptr);

}  // namespace w2f
