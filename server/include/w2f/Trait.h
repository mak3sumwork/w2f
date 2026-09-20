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

namespace w2f {

using TraitId = std::uint32_t;

// AllAllies / TraitHolders: applied to each qualifying unit. Team: applied ONCE for the whole team (cast by its lowest-UnitId trait holder);
// only for Summon effects ("summon 3 souls", not "3 souls per holder").
enum class TraitScope : std::uint8_t { AllAllies, TraitHolders, Team };

struct TraitEffect {
    TraitScope scope = TraitScope::AllAllies;
    AbilityEffect effect;  // target must be Self
};

// A hook (see EventTrigger in Ability.h) the synergy attaches to every unit in scope for the fight: "Coregons heal for 10% of the
// damage they deal" is an OnDealDamage hook on the trait holders; "souls echo the team's damage" an OnAllyDealDamage hook on a summon.
struct TraitTrigger {
    TraitScope scope = TraitScope::TraitHolders;   // AllAllies or TraitHolders
    AbilityDefinition ability;
};

struct TraitBreakpoint {
    int count = 0;  // unique champions with the trait needed to activate
    std::vector<TraitEffect> effects;
    std::vector<TraitTrigger> triggers;
};

struct TraitDefinition {
    TraitId id = 0;  // non-zero, unique; appears in the event stream
    std::string name;
    std::vector<TraitBreakpoint> breakpoints;  // strictly ascending count; may be empty (a tag with no synergy yet)
};

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
// Every SummonEffect anywhere (items, synergies) must name a champion that exists and is marked `summon` (champions are checked by
// ChampionDatabase::Create itself). Either optional database may be null.
bool ValidateSummonReferences(const ChampionDatabase& champions, const ItemDatabase* items, const TraitDatabase* traits, std::string* error = nullptr);

}  // namespace w2f
