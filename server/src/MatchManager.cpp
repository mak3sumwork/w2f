#include "w2f/MatchManager.h"

#include <algorithm>
#include <cassert>

#include "w2f/Hash.h"
#include "w2f/ShopManager.h"

namespace w2f {

std::unique_ptr<MatchManager> MatchManager::Create(const GameConfig& config, const ChampionDatabase& database,
                                                   std::uint64_t seed, std::unique_ptr<ICombatSimulator> simulator,
                                                   std::string* error, const ItemDatabase* items, const EncounterDatabase* encounters) {
    if (!config.Validate(error)) return nullptr;
    // Private ctor: can't use make_unique.
    return std::unique_ptr<MatchManager>(new MatchManager(config, database, seed, std::move(simulator), items, encounters));
}

MatchManager::MatchManager(const GameConfig& config, const ChampionDatabase& database, std::uint64_t seed,
                           std::unique_ptr<ICombatSimulator> simulator, const ItemDatabase* items, const EncounterDatabase* encounters)
    : config_(config),
      database_(database),
      items_(items),
      encounters_(encounters),
      seed_(seed),
      rng_(seed, kRngStreamMatch),
      pool_(database, config.pool),
      players_(config.match.playerCount, config.player, config.shop, pool_, seed, static_cast<IPlayerListener*>(this), items),
      simulator_(std::move(simulator)) {}

// ---- Listeners ---------------------------------------------------------------------------

void MatchManager::AddListener(IMatchListener* listener) {
    if (listener != nullptr) listeners_.push_back(listener);
}

void MatchManager::RemoveListener(IMatchListener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
}

// ---- State machine -----------------------------------------------------------------------

void MatchManager::Start() {
    if (phase_ != MatchPhase::NotStarted) return;
    round_ = 1;
    BeginRound();
}

void MatchManager::Tick() {
    if (phase_ == MatchPhase::NotStarted || phase_ == MatchPhase::MatchOver) return;
    ++ticksInPhase_;
    if (ticksInPhase_ >= PhaseDuration(phase_)) AdvancePhase();
}

int MatchManager::PhaseDuration(MatchPhase phase) const {
    switch (phase) {
        case MatchPhase::Draft: return config_.match.draftTicks;
        case MatchPhase::Planning: return config_.match.planningTicks;
        case MatchPhase::Combat: return config_.match.combatTicks;
        case MatchPhase::Resolution: return config_.match.resolutionTicks;
        case MatchPhase::NotStarted:
        case MatchPhase::MatchOver: break;
    }
    return 0;
}

int MatchManager::TicksRemainingInPhase() const {
    return std::max(PhaseDuration(phase_) - ticksInPhase_, 0);
}

void MatchManager::BeginRound() {
    // Income is paid when a round begins, from the gold/streak the player finished the last one with.
    players_.GrantRoundIncome(round_);
    EnterPhase(config_.match.IsDraftRound(round_) ? MatchPhase::Draft : MatchPhase::Planning);
}

void MatchManager::AdvancePhase() {
    switch (phase_) {
        case MatchPhase::Draft: EnterPhase(MatchPhase::Planning); break;
        case MatchPhase::Planning: EnterPhase(MatchPhase::Combat); break;
        case MatchPhase::Combat: EnterPhase(MatchPhase::Resolution); break;
        case MatchPhase::Resolution:
            if (players_.AliveCount() <= 1) {
                EnterPhase(MatchPhase::MatchOver);
            } else {
                ++round_;
                BeginRound();
            }
            break;
        case MatchPhase::NotStarted:
        case MatchPhase::MatchOver: break;
    }
}

void MatchManager::EnterPhase(MatchPhase next) {
    const MatchPhase previous = phase_;
    phase_ = next;
    ticksInPhase_ = 0;

    switch (next) {
        case MatchPhase::Draft:
            // Placeholder: shared carousel pick is not implemented yet; the phase only times.
            break;
        case MatchPhase::Planning:
            players_.RefreshAllShops();
            if (config_.snapshot.atPlanningStart) TakeAutoSnapshot();
            break;
        case MatchPhase::Combat:
            BuildMatchups();
            RunCombat();
            break;
        case MatchPhase::Resolution:
            ApplyCombatOutcomes();
            break;
        case MatchPhase::MatchOver:
            EndMatch();
            break;
        case MatchPhase::NotStarted:
            break;
    }

    for (IMatchListener* listener : listeners_) listener->OnPhaseChanged(previous, next, round_);
    if (next == MatchPhase::MatchOver) {
        for (IMatchListener* listener : listeners_) listener->OnMatchEnded(winner_);
    }
}

// ---- Combat flow (matchmaking + applying results; the fighting itself is external) --------

void MatchManager::BuildMatchups() {
    matchups_.clear();
    std::vector<PlayerId> ids = players_.AlivePlayerIds();

    if (config_.match.IsPveRound(round_)) {
        // Everyone fights the same monster board (each on their own copy of it). Its own random stream, so a PvE round never
        // disturbs the shuffle of the PvP rounds around it.
        std::uint32_t encounter = 0;
        if (encounters_ != nullptr) {
            const StageRound sr = config_.match.StageOf(round_);
            const std::uint32_t roll = Rng(seed_, kRngStreamPveBase + static_cast<std::uint64_t>(round_)).Next32();
            if (const EncounterDefinition* e = encounters_->Select(sr.stage, sr.roundInStage, roll)) encounter = e->id;
        }
        for (PlayerId id : ids) matchups_.push_back(Matchup{id, kInvalidPlayerId, false, true, encounter});
        return;
    }
    rng_.Shuffle(ids);

    for (std::size_t i = 0; i + 1 < ids.size(); i += 2) {
        matchups_.push_back(Matchup{ids[i], ids[i + 1], false});
    }
    if (ids.size() % 2 == 1) {
        // Odd one out fights a ghost of a random other living player.
        const PlayerId lonely = ids.back();
        const PlayerId ghostOf = ids[rng_.NextBelow(static_cast<std::uint32_t>(ids.size() - 1))];
        matchups_.push_back(Matchup{lonely, ghostOf, true});
    }
}

void MatchManager::RunCombat() {
    outcomes_.clear();
    if (simulator_) {
        const CombatContext context{round_, Rng(seed_, kRngStreamCombatBase + static_cast<std::uint64_t>(round_)).Next64(),
                                    players_, matchups_, config_.match.combatTicks, encounters_, &database_};
        outcomes_ = simulator_->Simulate(context);
    } else {
        for (const Matchup& m : matchups_) {
            CombatOutcome draw;
            draw.matchup = m;
            outcomes_.push_back(std::move(draw));
        }
    }
    for (const CombatOutcome& outcome : outcomes_) {
        for (IMatchListener* listener : listeners_) listener->OnCombatSimulated(round_, outcome);
    }
}

void MatchManager::ApplyCombatOutcomes() {
    Rng dropRng(seed_, kRngStreamPveDropBase + static_cast<std::uint64_t>(round_));
    for (CombatOutcome& outcome : outcomes_) {
        PlayerState* home = players_.Get(outcome.matchup.home);
        if (home == nullptr) continue;  // Defensive: bad data from a simulator.
        outcome.damageToLoser = 0;
        outcome.drop = PveDrop{};

        if (outcome.matchup.awayIsMonsters) {
            // PvE: a win pays one random drop; nothing else changes -- no damage, and streaks are left alone.
            if (outcome.winner == CombatWinner::Home) {
                outcome.drop = GrantPveDrop(*home, outcome.matchup.encounter, dropRng);
                if (outcome.drop.type != PveDropType::None) {
                    for (IMatchListener* listener : listeners_) listener->OnPveDrop(outcome.matchup.home, outcome.drop);
                }
            }
            continue;
        }

        PlayerState* away = players_.Get(outcome.matchup.away);
        if (away == nullptr) continue;
        const bool ghost = outcome.matchup.awayIsGhost;
        // The loser's damage: the stage's base plus a bit per unit the winner kept alive. Nobody takes it in a draw, or when
        // the loser is only a ghost.
        const int damage = config_.PlayerDamage(round_, outcome.winnerSurvivors);
        const auto hurt = [&](PlayerState& loser) {
            loser.ApplyDamage(damage);
            outcome.damageToLoser = damage;
            for (IMatchListener* listener : listeners_) listener->OnPlayerDamaged(loser.Id(), damage, loser.Health());
        };

        switch (outcome.winner) {
            case CombatWinner::Home:
                home->RecordRoundResult(RoundResult::Win);
                if (!ghost) {
                    away->RecordRoundResult(RoundResult::Loss);
                    hurt(*away);
                }
                break;
            case CombatWinner::Away:
                home->RecordRoundResult(RoundResult::Loss);
                hurt(*home);
                if (!ghost) away->RecordRoundResult(RoundResult::Win);
                break;
            case CombatWinner::Draw:
                home->RecordRoundResult(RoundResult::Draw);
                if (!ghost) away->RecordRoundResult(RoundResult::Draw);
                break;
        }
    }

    // Eliminate everyone who hit 0 this round. If several fall at once, the one who took the
    // most overkill (lowest health) places worst; equal health -> higher seat id places worse.
    std::vector<PlayerId> dying;
    for (PlayerId id : players_.AlivePlayerIds()) {
        if (players_.Get(id)->Health() <= 0) dying.push_back(id);
    }
    std::sort(dying.begin(), dying.end(), [this](PlayerId a, PlayerId b) {
        const int ha = players_.Get(a)->Health();
        const int hb = players_.Get(b)->Health();
        return ha != hb ? ha < hb : a > b;
    });

    int placement = players_.AliveCount();
    for (PlayerId id : dying) {
        players_.EliminatePlayer(id, placement);  // Returns their units + shop to the pool.
        for (IMatchListener* listener : listeners_) listener->OnPlayerEliminated(id, placement);
        --placement;
    }
}

PveDrop MatchManager::GrantPveDrop(PlayerState& player, std::uint32_t encounterId, Rng& rng) {
    PveDrop none;
    const EncounterDefinition* encounter = encounters_ != nullptr ? encounters_->Find(encounterId) : nullptr;
    if (encounter == nullptr) return none;
    const std::vector<PveDropEntry>& table = encounters_->DropsFor(*encounter);

    // Lines that cannot pay out right now (no items loaded, every listed tier sold out) are skipped, so a drop is never wasted
    // on a line that has nothing to give.
    const auto usableTiers = [&](const PveDropEntry& entry) {
        std::vector<int> tiers;
        for (int tier : entry.tiers) {
            if (pool_.RemainingInTier(tier) > 0 && std::find(tiers.begin(), tiers.end(), tier) == tiers.end()) tiers.push_back(tier);
        }
        return tiers;
    };
    const auto itemChoices = [&](const PveDropEntry& entry) {
        std::vector<ItemId> choices = entry.items;
        if (choices.empty() && items_ != nullptr) {
            for (const ItemDefinition& def : items_->All()) choices.push_back(def.id);
        }
        return choices;
    };
    std::vector<std::uint32_t> weights;
    std::uint32_t total = 0;
    for (const PveDropEntry& entry : table) {
        std::uint32_t w = static_cast<std::uint32_t>(entry.weight);
        if (entry.type == PveDropType::Champion && usableTiers(entry).empty()) w = 0;
        if (entry.type == PveDropType::Item && (items_ == nullptr || itemChoices(entry).empty())) w = 0;
        weights.push_back(w);
        total += w;
    }
    if (total == 0) return none;
    std::uint32_t roll = rng.NextBelow(total);
    std::size_t chosen = 0;
    while (roll >= weights[chosen]) roll -= weights[chosen++];
    const PveDropEntry& entry = table[chosen];

    PveDrop drop;
    switch (entry.type) {
        case PveDropType::Gold: {
            const int span = entry.maxGold - entry.minGold + 1;
            drop.type = PveDropType::Gold;
            drop.gold = entry.minGold + static_cast<int>(rng.NextBelow(static_cast<std::uint32_t>(span)));
            player.AddGold(drop.gold);
            break;
        }
        case PveDropType::Champion: {
            const std::vector<int> tiers = usableTiers(entry);
            const ChampionDefinition* champion = pool_.DrawFromTier(tiers[rng.NextBelow(static_cast<std::uint32_t>(tiers.size()))], rng);
            if (champion == nullptr) return none;
            if (player.CanAcquire(champion)) {
                player.AcquireUnit(champion, 0);
                drop.type = PveDropType::Champion;
                drop.champion = champion->id;
            } else {
                // No room on the bench or board and no merge to make: the copy goes back and the player is paid its cost instead.
                pool_.Return(champion, 1);
                player.AddGold(champion->cost);
                drop.type = PveDropType::Gold;
                drop.gold = champion->cost;
            }
            break;
        }
        case PveDropType::Item: {
            const std::vector<ItemId> choices = itemChoices(entry);
            const ItemId item = choices[rng.NextBelow(static_cast<std::uint32_t>(choices.size()))];
            if (!player.AddItemToBag(item)) return none;
            drop.type = PveDropType::Item;
            drop.item = item;
            break;
        }
        case PveDropType::None: return none;
    }
    return drop;
}

void MatchManager::TakeAutoSnapshot() {
    autoSnapshot_ = Snapshot();
    autoSnapshotRound_ = round_;
    for (IMatchListener* listener : listeners_) listener->OnAutoSnapshot(round_, autoSnapshot_);
}

void MatchManager::EndMatch() {
    const std::vector<PlayerId> alive = players_.AlivePlayerIds();
    if (alive.size() == 1) {
        winner_ = alive.front();
        players_.Get(winner_)->SetPlacement(1);
        return;
    }
    // Everyone fell in the same round: the one holding placement 1 (least overkill) wins.
    for (int i = 0; i < players_.PlayerCount(); ++i) {
        if (players_.Get(static_cast<PlayerId>(i))->Placement() == 1) winner_ = static_cast<PlayerId>(i);
    }
}

// ---- Player actions ----------------------------------------------------------------------

ActionResult MatchManager::ResolveActor(PlayerId id, PlayerState*& outPlayer) {
    outPlayer = nullptr;
    if (phase_ != MatchPhase::Planning) return ActionResult::WrongPhase;
    PlayerState* player = players_.Get(id);
    if (player == nullptr) return ActionResult::InvalidPlayer;
    if (!player->IsAlive()) return ActionResult::PlayerEliminated;
    outPlayer = player;
    return ActionResult::Ok;
}

ActionResult MatchManager::TryRerollShop(PlayerId player) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->Shop().TryReroll() : check;
}

ActionResult MatchManager::TryBuyShopUnit(PlayerId player, std::size_t shopSlot) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->Shop().TryBuy(shopSlot) : check;
}

ActionResult MatchManager::TryBuyXp(PlayerId player) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->TryBuyXp() : check;
}

ActionResult MatchManager::TrySellUnit(PlayerId player, UnitId unit) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->SellUnit(unit) : check;
}

ActionResult MatchManager::TryMoveUnit(PlayerId player, UnitId unit, LocationType location, int x, int y) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->TryMoveUnit(unit, location, x, y) : check;
}

ActionResult MatchManager::TryEquipItem(PlayerId player, UnitId unit, ItemId item) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->TryEquipItem(unit, item) : check;
}

ActionResult MatchManager::TryUnequipItem(PlayerId player, UnitId unit, int slot) {
    PlayerState* p = nullptr;
    const ActionResult check = ResolveActor(player, p);
    return check == ActionResult::Ok ? p->TryUnequipItem(unit, slot) : check;
}

// ---- Player event fan-out ----------------------------------------------------------------

void MatchManager::OnUnitBought(PlayerId player, const UnitInstance& unit, int goldSpent) {
    for (IMatchListener* l : listeners_) l->OnUnitBought(player, unit, goldSpent);
}
void MatchManager::OnUnitSold(PlayerId player, const UnitInstance& unit, int goldGained) {
    for (IMatchListener* l : listeners_) l->OnUnitSold(player, unit, goldGained);
}
void MatchManager::OnUnitMoved(PlayerId player, const UnitMove& move) {
    for (IMatchListener* l : listeners_) l->OnUnitMoved(player, move);
}
void MatchManager::OnUnitMerged(PlayerId player, const UnitMerge& merge) {
    for (IMatchListener* l : listeners_) l->OnUnitMerged(player, merge);
}
void MatchManager::OnItemEquipped(PlayerId player, const UnitInstance& unit, ItemId item) {
    for (IMatchListener* l : listeners_) l->OnItemEquipped(player, unit, item);
}
void MatchManager::OnItemUnequipped(PlayerId player, const UnitInstance& unit, ItemId item) {
    for (IMatchListener* l : listeners_) l->OnItemUnequipped(player, unit, item);
}
void MatchManager::OnItemsCombined(PlayerId player, const UnitInstance& unit, const ItemCombination& combination) {
    for (IMatchListener* l : listeners_) l->OnItemsCombined(player, unit, combination);
}
void MatchManager::OnIncomeGranted(PlayerId player, int round, const IncomeBreakdown& income) {
    for (IMatchListener* l : listeners_) l->OnIncomeGranted(player, round, income);
}

// ---- Diagnostics -------------------------------------------------------------------------

bool MatchManager::VerifyPoolIntegrity() const {
    const auto& all = database_.All();
    std::vector<int> outstanding(all.size(), 0);  // copies held outside the pool

    for (int i = 0; i < players_.PlayerCount(); ++i) {
        const PlayerState* p = players_.Get(static_cast<PlayerId>(i));
        for (const ChampionDefinition* slot : p->Shop().Slots()) {
            if (slot != nullptr) outstanding[database_.IndexOf(slot->id)] += 1;
        }
        for (const UnitInstance& unit : p->Roster().Units()) {
            outstanding[database_.IndexOf(unit.champion->id)] += SharedChampionPool::CopiesForStarLevel(unit.starLevel);
        }
    }
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (pool_.Remaining(all[i].id) + outstanding[i] != pool_.InitialCopies(all[i].id)) return false;
    }
    return true;
}

bool MatchManager::VerifyRosterLayouts() const {
    for (int i = 0; i < players_.PlayerCount(); ++i) {
        if (!players_.Get(static_cast<PlayerId>(i))->Roster().CheckInvariants()) return false;
    }
    return true;
}


std::uint64_t MatchManager::StateHash() const {
    Fnv1a h;
    h.Add(seed_);   // every future fight's seed derives from it
    h.Add(static_cast<std::uint64_t>(phase_));
    h.AddInt(round_);
    h.AddInt(ticksInPhase_);
    h.Add(winner_);
    // Hidden state matters as much as visible state: two matches that look identical but hold different generators
    // or id counters would diverge on the next roll / purchase.
    for (std::uint64_t word : rng_.GetState().words) h.Add(word);

    for (int i = 0; i < players_.PlayerCount(); ++i) {
        const PlayerState* p = players_.Get(static_cast<PlayerId>(i));
        h.AddInt(p->Health());
        h.AddInt(p->Level());
        h.AddInt(p->Xp());
        h.AddInt(p->Gold());
        h.AddInt(p->Streak());
        h.AddInt(p->Placement());
        h.Add(p->IsAlive() ? 1 : 0);
        for (const UnitInstance& unit : p->Roster().Units()) {
            h.Add(unit.id);
            h.Add(unit.champion->id);
            h.AddInt(unit.starLevel);
            h.Add(static_cast<std::uint64_t>(unit.location));
            h.AddInt(unit.x);
            h.AddInt(unit.y);
            for (ItemId item : unit.items) h.Add(item);
        }
        h.Add(0xFFFFFFFEull);  // separator
        for (ItemId item : p->ItemBag()) h.Add(item);
        h.Add(0xFFFFFFFFull);  // separator
        for (const ChampionDefinition* slot : p->Shop().Slots()) h.Add(slot ? slot->id : 0);
        h.Add(p->Roster().NextSerial());
        for (std::uint64_t word : p->Shop().GetRngState().words) h.Add(word);
    }
    for (const ChampionDefinition& def : database_.All()) h.AddInt(pool_.Remaining(def.id));
    for (const Matchup& m : matchups_) {
        h.Add(m.home);
        h.Add(m.away);
        h.Add(m.awayIsGhost ? 1 : 0);
        h.Add(m.awayIsMonsters ? 1 : 0);
        h.Add(m.encounter);
    }
    // The fights themselves: results and the full event stream (via its checksum).
    for (const CombatOutcome& o : outcomes_) {
        h.Add(o.matchup.home);
        h.Add(o.matchup.away);
        h.Add(o.matchup.awayIsGhost ? 1 : 0);
        h.Add(o.matchup.awayIsMonsters ? 1 : 0);
        h.Add(o.matchup.encounter);
        h.Add(static_cast<std::uint64_t>(o.winner));
        h.AddInt(o.winnerSurvivors);
        h.AddInt(o.damageToLoser);
        h.Add(static_cast<std::uint64_t>(o.drop.type));
        h.AddInt(o.drop.gold);
        h.Add(o.drop.champion);
        h.Add(o.drop.item);
        h.Add(o.log.checksum);
    }
    return h.value;
}

}  // namespace w2f
