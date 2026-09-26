// Trait system v2 (September 2026): the half of the traits that lives OUTSIDE fights. The combat simulator applies a trait's stats and hooks;
// this file handles what a trait does to the match:
//   * the path a trait with paths takes (Selini: Enlightenment or Prosperity), chosen once per match from the seed;
//   * after-combat rewards: XP (Enlightenment), gold per takedowns (Prosperity), star dust (Najmi), Mining Drill gold (Hexagon), and units
//     given to the bench (Phaisa's Rift Herald);
//   * choices: Hexagon's modules (pick 1 of 3 on reaching a tier) and Najmi's prototypes (take the item, or decline and bank the dust);
//   * the Nature trait's plants, kept in step with the board;
//   * the Phaisa Queen, offered in the shop to a player who qualifies.
// Everything is deterministic: random picks use their own RNG streams (kRngStreamTraitPaths / kRngStreamTraitBase).

#include <algorithm>

#include "w2f/MatchManager.h"
#include "w2f/MotherNature.h"
#include "w2f/Trait.h"

namespace w2f {

namespace {
// Najmi's cash-outs (like TFT's Anima, FEEDBACK V1): offered only at every 100 star dust; the loot grows with the bank.
// tier = hundreds banked (1..6): {items to pick from, of which kind, bonus completed items, bonus Item Removers, gold}
struct CashOut { int choices; bool completed; int bonusCompleted; int bonusRemovers; int gold; };
constexpr int kStarDustPerTier = 100;
constexpr int kCashOutTiers = 6;
constexpr CashOut kCashOuts[kCashOutTiers] = {
    {1, false, 0, 0, 2},    // 100: a component
    {1, true, 0, 0, 2},     // 200: a completed item
    {3, true, 0, 1, 3},     // 300: a completed item of your choice + an Item Remover
    {3, true, 1, 0, 8},     // 400: a choice + another completed item
    {3, true, 1, 1, 15},    // 500: a choice + another completed item + an Item Remover
    {3, true, 2, 0, 20},    // 600+: a choice + two more completed items
};
constexpr int kModuleOptions = 3;
constexpr int kColumnsFromCentre[kBoardColumns] = {3, 2, 4, 1, 5, 0, 6};
}  // namespace

void MatchManager::ChooseTraitPaths() {
    traitPaths_.clear();
    if (traits_ == nullptr) return;
    Rng rng(seed_, kRngStreamTraitPaths);
    for (const TraitDefinition& trait : traits_->All()) {   // ascending trait id: one roll per trait with paths
        if (trait.paths.empty()) continue;
        traitPaths_.emplace_back(trait.id, static_cast<int>(rng.NextBelow(static_cast<std::uint32_t>(trait.paths.size()))));
    }
}

int MatchManager::TraitPath(std::uint32_t trait) const {
    for (const auto& p : traitPaths_) {
        if (p.first == trait) return p.second;
    }
    return 0;
}

const TraitChoice& MatchManager::PendingTraitChoice(PlayerId player) const {
    static const TraitChoice kNone;
    return player < choices_.size() ? choices_[player] : kNone;
}

std::vector<TraitCountUnit> MatchManager::BoardUnits(const PlayerState& player) const {
    return BoardTraitUnits(player.Roster().Units(), items_);
}

int MatchManager::TierFor(const PlayerState& player, const TraitDefinition& trait, int* countOut) const {
    const std::vector<TraitCountUnit> units = BoardUnits(player);
    const int count = CountTraitHolders(units, trait.name);
    if (countOut) *countOut = count;
    return ActiveTier(trait.Breakpoints(TraitPath(trait.id)), count, CountEmblemHolders(units, trait.name));
}

int MatchManager::TraitTier(PlayerId player, const TraitDefinition& trait, int* countOut) const {
    const PlayerState* p = players_.Get(player);
    if (p == nullptr) {
        if (countOut) *countOut = 0;
        return 0;
    }
    return TierFor(*p, trait, countOut);
}

void MatchManager::AfterRosterChange(PlayerId id) {
    PlayerState* p = players_.Get(id);
    if (p == nullptr || !p->IsAlive()) return;
    if (traits_ == nullptr) {
        CheckUnlocks(*p);
        return;
    }
    SyncPlants(*p);
    OfferModules(*p);
    OfferQueen(*p);
    CheckUnlocks(*p);
}

// Unlockable champions (TFT's T-Hex): once the star levels of the rule's trait on the board add up to enough while the player is at the level,
// the champion can appear in their shop for the rest of the match.
void MatchManager::CheckUnlocks(PlayerState& player) {
    for (const ChampionDefinition& def : database_.All()) {
        if (!def.unlock.Gated() || player.HasUnlocked(def.id) || player.Level() < def.unlock.playerLevel) continue;
        int stars = 0;
        for (const UnitInstance& u : player.Roster().Units()) {
            if (u.location != LocationType::Board || u.champion == nullptr || u.champion->summon || u.champion->plant) continue;
            if (std::find(u.champion->traits.begin(), u.champion->traits.end(), def.unlock.trait) != u.champion->traits.end()) stars += u.starLevel;
        }
        if (stars < def.unlock.starLevel) continue;
        player.TraitsMutable().unlocked.push_back(def.id);
        for (IMatchListener* listener : listeners_) listener->OnChampionUnlocked(player.Id(), def.id);
    }
}

// The Nature trait's plants follow the board: the breakpoint reached says which plants (and at what star) stand on it. Plants that should not
// be there go; missing ones grow on a free cell (tanks in the front row, the rest in the back, centre first). Only while the board may change.
void MatchManager::SyncPlants(PlayerState& player) {
    if (traits_ == nullptr) return;
    if (phase_ == MatchPhase::Combat || phase_ == MatchPhase::Resolution) return;   // the board is locked
    for (const TraitDefinition& trait : traits_->All()) {
        const std::vector<TraitBreakpoint>& breakpoints = trait.Breakpoints(TraitPath(trait.id));
        std::vector<ChampionId> kinds;   // every plant this trait can grow
        for (const TraitBreakpoint& bp : breakpoints) {
            for (const PlantGrant& g : bp.plants) {
                if (std::find(kinds.begin(), kinds.end(), g.champion) == kinds.end()) kinds.push_back(g.champion);
            }
        }
        if (kinds.empty()) continue;
        const int tier = TierFor(player, trait, nullptr);
        int star = 1;
        for (int k = 0; k < tier; ++k) star = std::max(star, breakpoints[static_cast<std::size_t>(k)].plantStar);

        for (ChampionId kind : kinds) {
            int wanted = 0;
            if (tier > 0) {
                for (const PlantGrant& g : breakpoints[static_cast<std::size_t>(tier - 1)].plants) wanted += g.champion == kind ? g.count : 0;
            }
            // Keep the first `wanted` plants of this kind; the rest go. A kept plant at the wrong star is regrown on its own cell.
            std::vector<UnitInstance> existing;
            for (const UnitInstance& u : player.Roster().Units()) {
                if (u.champion->id == kind) existing.push_back(u);
            }
            int kept = 0;
            for (const UnitInstance& u : existing) {
                if (kept < wanted) {
                    ++kept;
                    if (u.starLevel != star) {
                        player.RemovePlant(u.id);
                        player.GrantPlant(u.champion, star, u.x, u.y);
                    }
                } else {
                    player.RemovePlant(u.id);
                }
            }
            const ChampionDefinition* def = database_.Find(kind);
            for (; kept < wanted && def != nullptr; ++kept) {
                const bool front = def->role == ChampionRole::Tank;
                bool placed = false;
                for (int pass = 0; pass < kBoardRows && !placed; ++pass) {
                    const int y = front ? kBoardRows - 1 - pass : pass;
                    for (int x : kColumnsFromCentre) {
                        if (player.Roster().BoardAt(x, y) == nullptr && player.GrantPlant(def, star, x, y)) {
                            placed = true;
                            break;
                        }
                    }
                }
                if (!placed) break;   // a full board: it grows as soon as there is room
            }
        }
    }
}

// The Phaisa Queen: offered in the first shop slot while the player has enough different holders on the board, is at the level, and owns no Queen.
void MatchManager::OfferQueen(PlayerState& player) {
    if (traits_ == nullptr || IsShopClosed()) return;
    for (const TraitDefinition& trait : traits_->All()) {
        if (trait.queen.champion == 0) continue;
        const ChampionDefinition* queen = database_.Find(trait.queen.champion);
        if (queen == nullptr || player.Level() < trait.queen.level) continue;
        int count = 0;
        TierFor(player, trait, &count);
        if (count < trait.queen.uniqueHolders) continue;
        bool owned = false;
        for (const UnitInstance& u : player.Roster().Units()) owned = owned || u.champion == queen;
        if (!owned) player.Shop().OfferSpecial(queen);
    }
}

// Hexagon: reaching a breakpoint that unlocks a module tier offers 3 random modules of the next tier not offered yet (one tier at a time).
void MatchManager::OfferModules(PlayerState& player) {
    if (traits_ == nullptr) return;
    TraitChoice& choice = choices_[player.Id()];
    if (choice.Pending() && choice.kind != TraitChoiceKind::Prototype) return;   // a Najmi cash-out waits; a module pushes it aside (it comes back after)
    for (const TraitDefinition& trait : traits_->All()) {
        if (trait.modules.empty()) continue;
        const std::vector<TraitBreakpoint>& breakpoints = trait.Breakpoints(TraitPath(trait.id));
        const int tier = TierFor(player, trait, nullptr);
        int unlocked = 0;
        for (int k = 0; k < tier; ++k) unlocked = std::max(unlocked, breakpoints[static_cast<std::size_t>(k)].moduleTier);
        TraitProgress& progress = player.TraitsMutable();
        if (unlocked <= progress.moduleTiersOffered) continue;
        const int next = progress.moduleTiersOffered + 1;
        std::vector<std::uint32_t> pool;
        for (const TraitModule& m : trait.modules) {
            if (m.tier == next && std::find(progress.modules.begin(), progress.modules.end(), m.id) == progress.modules.end()) pool.push_back(m.id);
        }
        progress.moduleTiersOffered = next;
        if (pool.empty()) continue;
        Rng rng(seed_, kRngStreamTraitBase + static_cast<std::uint64_t>(round_) * 16u + player.Id());
        choice = TraitChoice{};
        choice.kind = TraitChoiceKind::Module;
        choice.trait = trait.id;
        choice.tier = next;
        while (!pool.empty() && static_cast<int>(choice.options.size()) < kModuleOptions) {
            const std::size_t pick = rng.NextBelow(static_cast<std::uint32_t>(pool.size()));
            choice.options.push_back(pool[pick]);
            pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pick));
        }
        for (IMatchListener* listener : listeners_) listener->OnTraitChoiceOffered(player.Id(), choice);
        return;
    }
}

// Najmi: a cash-out is offered each time the bank reaches another 100 star dust (100 .. 600). It stays on offer -- no pop-up, nothing to answer --
// until the player takes it (the bank is spent) or a bigger one replaces it.
void MatchManager::OfferPrototype(PlayerState& player) {
    if (items_ == nullptr || traits_ == nullptr) return;
    TraitChoice& choice = choices_[player.Id()];
    if (choice.Pending() && choice.kind != TraitChoiceKind::Prototype) return;   // a module decision first; the dust stays banked
    const int dust = player.Traits().starDust;
    const int tier = std::min(kCashOutTiers, dust / kStarDustPerTier);
    if (tier < 1) return;                                                           // nothing below 100
    if (choice.kind == TraitChoiceKind::Prototype && choice.tier >= tier) return;   // this cash-out is already on offer
    const CashOut& rule = kCashOuts[tier - 1];
    std::vector<std::uint32_t> components;
    std::vector<std::uint32_t> completed;
    std::uint32_t remover = 0;
    for (const ItemDefinition& def : items_->All()) {
        if (ItemIsOfClass(*items_, def, ItemClass::Component)) components.push_back(def.id);
        else if (ItemIsOfClass(*items_, def, ItemClass::Legendary)) completed.push_back(def.id);
        else if (def.IsConsumable() && remover == 0) remover = def.id;
    }
    std::vector<std::uint32_t>& source = rule.completed ? completed : components;
    if (source.empty()) return;
    std::uint32_t najmi = 0;
    for (const TraitDefinition& trait : traits_->All()) {
        for (const TraitBreakpoint& bp : trait.Breakpoints(TraitPath(trait.id))) najmi = bp.starDust.Any() ? trait.id : najmi;
    }
    Rng rng(seed_, kRngStreamTraitBase + 4096u + static_cast<std::uint64_t>(round_) * 16u + player.Id());
    choice = TraitChoice{};
    choice.kind = TraitChoiceKind::Prototype;
    choice.trait = najmi;
    choice.tier = tier;
    choice.bonusGold = rule.gold;
    while (!source.empty() && static_cast<int>(choice.options.size()) < rule.choices) {
        const std::size_t pick = rng.NextBelow(static_cast<std::uint32_t>(source.size()));
        choice.options.push_back(source[pick]);
        source.erase(source.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    for (int k = 0; k < rule.bonusCompleted && !completed.empty(); ++k) {
        const std::size_t pick = rng.NextBelow(static_cast<std::uint32_t>(completed.size()));
        choice.bonusItems.push_back(completed[pick]);
        completed.erase(completed.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    for (int k = 0; k < rule.bonusRemovers && remover != 0; ++k) choice.bonusItems.push_back(remover);
    for (IMatchListener* listener : listeners_) listener->OnTraitChoiceOffered(player.Id(), choice);
}

// After a fight: what the player's traits (the breakpoints their board reached) pay. `lost`: they lost a player-versus-player combat.
void MatchManager::GrantTraitRewards(PlayerState& player, const CombatOutcome& outcome, bool home, bool lost) {
    if (traits_ == nullptr || !player.IsAlive() || player.Health() <= 0) return;
    (void)home;
    const std::vector<TraitCountUnit> units = BoardUnits(player);
    TraitProgress& progress = player.TraitsMutable();
    TraitRewards rewards;
    for (const TraitDefinition& trait : traits_->All()) {
        const std::vector<TraitBreakpoint>& breakpoints = trait.Breakpoints(TraitPath(trait.id));
        const int tier = ActiveTier(breakpoints, CountTraitHolders(units, trait.name), CountEmblemHolders(units, trait.name));
        if (tier == 0) continue;
        const TraitBreakpoint& bp = breakpoints[static_cast<std::size_t>(tier - 1)];
        int takedowns = 0;   // by this player's units that carry the trait
        for (const auto& [unitId, count] : outcome.log.takedowns) {
            const UnitInstance* unit = player.Roster().Find(unitId);
            if (unit == nullptr) continue;
            TraitCountUnit view;
            view.champion = unit->champion;
            if (items_ != nullptr) {
                for (ItemId item : unit->items) {
                    if (const ItemDefinition* def = item != 0 ? items_->Find(item) : nullptr) {
                        for (const std::string& t : def->grantsTraits) view.extraTraits.push_back(t);
                    }
                }
            }
            if (UnitHasTrait(view, trait.name)) takedowns += count;
        }
        rewards.xp += bp.xpAfterCombat;
        if (bp.takedownsPerGold > 0) {
            progress.takedownCounter += takedowns;
            const int gold = progress.takedownCounter / bp.takedownsPerGold;
            progress.takedownCounter %= bp.takedownsPerGold;
            progress.traitGold += gold;
            rewards.gold += gold;
        }
        if (bp.starDust.Any()) {
            int dust = bp.starDust.perTakedown * takedowns;
            if (lost) dust += bp.starDust.onLoss + bp.starDust.perLossStreak * std::max(0, -player.Streak());
            if (!outcome.matchup.awayIsMonsters) dust += bp.starDust.perCombat;   // every player combat, won or lost
            dust *= bp.starDust.multiplier;
            progress.starDust += dust;
            rewards.starDust += dust;
        }
        if (!trait.modules.empty()) {
            int unlocked = 0;
            for (int k = 0; k < tier; ++k) unlocked = std::max(unlocked, breakpoints[static_cast<std::size_t>(k)].moduleTier);
            for (std::uint32_t id : progress.modules) {
                const TraitModule* m = trait.FindModule(id);
                if (m != nullptr && m->tier <= unlocked) rewards.gold += m->goldAfterCombat;
            }
        }
        if (bp.grantUnit.champion != 0 && !progress.unitGranted) {
            ++progress.grantCombats;
            const ChampionDefinition* unit = database_.Find(bp.grantUnit.champion);
            if (progress.grantCombats >= bp.grantUnit.afterCombats && unit != nullptr && player.CanAcquire(unit)) {
                player.AcquireUnit(unit, 0);
                progress.unitGranted = true;
                rewards.unit = unit->id;
            }
        }
    }
    if (rewards.xp > 0) player.AddXp(rewards.xp);
    if (rewards.gold > 0) player.AddGold(rewards.gold);
    if (rewards.starDust > 0) OfferPrototype(player);
    if (rewards.Any()) {
        for (IMatchListener* listener : listeners_) listener->OnTraitRewards(player.Id(), rewards);
    }
}

ActionResult MatchManager::ResolveTraitChoice(PlayerId id, int index, bool automatic) {
    PlayerState* p = players_.Get(id);
    if (p == nullptr) return ActionResult::InvalidPlayer;
    if (!p->IsAlive()) return ActionResult::PlayerEliminated;
    TraitChoice& open = choices_[id];
    if (!open.Pending()) return ActionResult::AlreadyPicked;
    if (index == -1 ? open.kind != TraitChoiceKind::Prototype : (index < 0 || static_cast<std::size_t>(index) >= open.options.size())) {
        return ActionResult::InvalidSlot;
    }
    const TraitChoice choice = open;
    open = TraitChoice{};
    if (index >= 0) {
        const std::uint32_t picked = choice.options[static_cast<std::size_t>(index)];
        if (choice.kind == TraitChoiceKind::Module) {
            p->TraitsMutable().modules.push_back(picked);
        } else {
            p->AddItemToBag(picked);
            for (std::uint32_t bonus : choice.bonusItems) p->AddItemToBag(bonus);
            if (choice.bonusGold > 0) p->AddGold(choice.bonusGold);
            p->TraitsMutable().starDust = 0;
        }
    }
    for (IMatchListener* listener : listeners_) listener->OnTraitChoiceResolved(id, choice, index, automatic);
    if (traits_ != nullptr) OfferModules(*p);   // the next tier may be waiting
    if (traits_ != nullptr && !choices_[id].Pending() && choice.kind == TraitChoiceKind::Module) OfferPrototype(*p);   // a cash-out the module pushed aside
    return ActionResult::Ok;
}

ActionResult MatchManager::TryPickTraitChoice(PlayerId player, int index) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p, ActorRule::Bench);
    if (check != ActionResult::Ok) return check;
    return ResolveTraitChoice(player, index, false);
}

void MatchManager::SettleTraitChoices() {
    for (PlayerId id : players_.AlivePlayerIds()) {
        // Modules are settled (the first option) when a fight starts; a Najmi cash-out stays on offer until the player takes it (FEEDBACK V1).
        for (int guard = 0; guard < 4 && choices_[id].Pending() && choices_[id].kind == TraitChoiceKind::Module; ++guard) {   // (settling a module may open the next tier's offer)
            ResolveTraitChoice(id, 0, true);
        }
    }
}

}  // namespace w2f
