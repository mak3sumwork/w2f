#include "w2f/PlayerState.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>

#include "w2f/Rng.h"
#include "w2f/ShopManager.h"

namespace w2f {

PlayerState::PlayerState(PlayerId id, const PlayerConfig& playerConfig, const ShopConfig& shopConfig,
                         SharedChampionPool& pool, std::uint64_t matchSeed, IPlayerListener* listener, const ItemDatabase* items)
    : id_(id),
      config_(playerConfig),
      pool_(pool),
      listener_(listener),
      health_(playerConfig.startingHealth),
      gold_(playerConfig.startingGold),
      roster_(id, items),
      items_(items),
      shop_(std::make_unique<ShopManager>(*this, pool, shopConfig, Rng(matchSeed, kRngStreamShopBase + id))) {
    SyncBoardCapacity();
}

PlayerState::~PlayerState() = default;

void PlayerState::SyncBoardCapacity() {
    roster_.SetBoardCapacity(config_.limitBoardToLevel ? level_ : kBoardRows * kBoardColumns);
}

// ---- Leveling ----------------------------------------------------------------------------

int PlayerState::XpToNextLevel() const {
    if (IsMaxLevel()) return 0;
    return config_.xpToNextLevel[static_cast<std::size_t>(level_ - 1)];
}

ActionResult PlayerState::TryBuyXp() {
    if (!IsAlive()) return ActionResult::PlayerEliminated;
    if (IsMaxLevel()) return ActionResult::MaxLevel;
    if (!TrySpendGold(config_.buyXpCost)) return ActionResult::NotEnoughGold;
    AddXp(config_.buyXpAmount);
    return ActionResult::Ok;
}

void PlayerState::AddXp(int amount) {
    if (amount <= 0 || IsMaxLevel()) return;
    xp_ += amount;
    while (!IsMaxLevel() && xp_ >= XpToNextLevel()) {
        xp_ -= XpToNextLevel();
        ++level_;
    }
    if (IsMaxLevel()) xp_ = 0;  // Nothing left to earn; don't display a stale partial bar.
    SyncBoardCapacity();
}

// ---- Gold --------------------------------------------------------------------------------

void PlayerState::AddGold(int amount) {
    if (amount > 0) gold_ += amount;
}

bool PlayerState::TrySpendGold(int amount) {
    if (amount < 0 || gold_ < amount) return false;
    gold_ -= amount;
    return true;
}

IncomeBreakdown PlayerState::CalculateIncome(int round) const {
    IncomeBreakdown income;

    const std::size_t roundIndex = static_cast<std::size_t>(std::max(round, 1) - 1);
    income.baseGold = roundIndex < config_.baseIncomeByRound.size() ? config_.baseIncomeByRound[roundIndex]
                                                                    : config_.baseIncomeAfterTable;

    income.interestGold =
        std::min((gold_ / config_.interestStepGold) * config_.interestPerStep, config_.interestCap);

    const int streakLength = std::abs(streak_);
    for (const StreakBonus& bonus : config_.streakBonuses) {  // ascending: last match wins
        if (streakLength >= bonus.minStreak) income.streakGold = bonus.bonusGold;
    }

    if (round >= config_.firstPassiveXpRound && !IsMaxLevel()) income.passiveXp = config_.passiveXpPerRound;
    return income;
}

IncomeBreakdown PlayerState::GrantRoundIncome(int round) {
    if (!IsAlive()) return {};
    const IncomeBreakdown income = CalculateIncome(round);
    AddGold(income.TotalGold());
    AddXp(income.passiveXp);
    if (listener_) listener_->OnIncomeGranted(id_, round, income);
    return income;
}

// ---- Combat aftermath --------------------------------------------------------------------

void PlayerState::ApplyDamage(int amount) {
    if (amount > 0 && IsAlive()) health_ -= amount;
}

void PlayerState::RecordRoundResult(RoundResult result) {
    switch (result) {
        case RoundResult::Win: streak_ = streak_ > 0 ? streak_ + 1 : 1; break;
        case RoundResult::Loss: streak_ = streak_ < 0 ? streak_ - 1 : -1; break;
        case RoundResult::Draw: break;
    }
}

// ---- Units -------------------------------------------------------------------------------

bool PlayerState::CanAcquire(const ChampionDefinition* champion, int starLevel) const {
    return IsAlive() && roster_.CanAdd(champion, starLevel);
}

ActionResult PlayerState::AcquireUnit(const ChampionDefinition* champion, int goldSpent, int starLevel) {
    if (!IsAlive()) return ActionResult::PlayerEliminated;
    if (champion == nullptr || starLevel < 1 || starLevel > kMaxStarLevel) return ActionResult::InvalidUnit;
    if (!roster_.CanAdd(champion, starLevel)) return ActionResult::RosterFull;

    const UnitRoster::AddResult added = roster_.Add(champion, starLevel);
    assert(added.added);
    for (const UnitMerge& merge : added.merges) {
        for (ItemId item : merge.overflowItems) itemBag_.push_back(item);   // items that no longer fit on the merged unit
    }

    // Notify only after the roster is fully consistent again.
    if (listener_) {
        listener_->OnUnitBought(id_, added.unit, goldSpent);
        for (const UnitMerge& merge : added.merges) {
            listener_->OnUnitMerged(id_, merge);
            for (const ItemCombination& combo : merge.combinations) listener_->OnItemsCombined(id_, merge.upgraded, combo);
        }
    }
    return ActionResult::Ok;
}

ActionResult PlayerState::TryMoveUnit(UnitId unit, LocationType location, int x, int y) {
    if (!IsAlive()) return ActionResult::PlayerEliminated;
    std::vector<UnitMove> moves;
    const ActionResult result = roster_.Move(unit, location, x, y, moves);
    if (result == ActionResult::Ok && listener_) {
        for (const UnitMove& move : moves) listener_->OnUnitMoved(id_, move);
    }
    return result;
}

ActionResult PlayerState::SellUnit(UnitId unit) {
    if (!IsAlive()) return ActionResult::PlayerEliminated;

    UnitInstance sold;
    if (!roster_.Remove(unit, &sold)) return ActionResult::InvalidUnit;

    // A merged unit stands for 3^(star-1) copies; all of them go back.
    const bool returned = pool_.Return(sold.champion, SharedChampionPool::CopiesForStarLevel(sold.starLevel));
    assert(returned && "sold more copies than were ever in the pool");
    (void)returned;

    for (ItemId item : sold.items) {
        if (item != 0) itemBag_.push_back(item);   // a sold unit's items are not lost
    }
    const int value = SellValue(sold.champion->cost, sold.starLevel);
    gold_ += value;
    if (listener_) listener_->OnUnitSold(id_, sold, value);
    return ActionResult::Ok;
}

int PlayerState::SellValue(int cost, int starLevel) {
    // Matches TFT: 1-cost units sell for exactly the copies spent; others lose 1 gold when merged.
    const int copies = SharedChampionPool::CopiesForStarLevel(starLevel);
    return copies * cost - ((starLevel > 1 && cost > 1) ? 1 : 0);
}

// ---- Items -------------------------------------------------------------------------------

bool PlayerState::AddItemToBag(ItemId item) {
    if (item == 0 || (items_ != nullptr && items_->Find(item) == nullptr)) return false;
    itemBag_.push_back(item);
    return true;
}

ActionResult PlayerState::TryEquipItem(UnitId unit, ItemId item) {
    if (!IsAlive()) return ActionResult::PlayerEliminated;
    const auto inBag = std::find(itemBag_.begin(), itemBag_.end(), item);
    if (item == 0 || inBag == itemBag_.end()) return ActionResult::InvalidItem;
    if (items_ != nullptr && items_->Find(item) == nullptr) return ActionResult::InvalidItem;
    ItemCombination combination;
    const ActionResult result = roster_.EquipItem(unit, item, &combination);
    if (result != ActionResult::Ok) return result;
    itemBag_.erase(inBag);
    if (listener_) {
        listener_->OnItemEquipped(id_, *roster_.Find(unit), item);
        if (combination.result != 0) listener_->OnItemsCombined(id_, *roster_.Find(unit), combination);
    }
    return ActionResult::Ok;
}

ActionResult PlayerState::TryUnequipItem(UnitId unit, int slot) {
    if (!IsAlive()) return ActionResult::PlayerEliminated;
    ItemId item = 0;
    const ActionResult result = roster_.UnequipItem(unit, slot, &item);
    if (result != ActionResult::Ok) return result;
    itemBag_.push_back(item);
    if (listener_) listener_->OnItemUnequipped(id_, *roster_.Find(unit), item);
    return ActionResult::Ok;
}

// ---- Snapshot ----------------------------------------------------------------------------

bool PlayerState::RestoreState(const PlayerRestoreData& data, std::string* error) {
    auto fail = [error, this](const std::string& why) {
        if (error) *error = "player " + std::to_string(id_) + ": " + why;
        return false;
    };
    const auto knownItem = [this](ItemId item) { return item != 0 && (items_ == nullptr || items_->Find(item) != nullptr); };

    if (data.level < 1 || data.level > kMaxPlayerLevel) return fail("level out of range");
    if (data.gold < 0) return fail("negative gold");
    if (data.xp < 0) return fail("negative xp");
    if (data.level == kMaxPlayerLevel ? data.xp != 0 : data.xp >= config_.xpToNextLevel[static_cast<std::size_t>(data.level - 1)]) {
        return fail("xp inconsistent with level");
    }
    if (data.eliminated) {
        if (data.placement < 1) return fail("an eliminated player needs a placement");
        if (!data.units.empty()) return fail("an eliminated player cannot own units");
        for (const ChampionDefinition* slot : data.shopSlots) {
            if (slot != nullptr) return fail("an eliminated player cannot hold a shop offer");
        }
    } else {
        if (data.health <= 0) return fail("a living player needs positive health");
        if (data.placement != 0 && data.placement != 1) return fail("a living player's placement must be 0 or 1");
    }
    for (ItemId item : data.itemBag) {
        if (!knownItem(item)) return fail("item bag holds an unknown item");
    }
    for (const UnitInstance& unit : data.units) {
        if (unit.champion == nullptr || unit.starLevel < 1 || unit.starLevel > kMaxStarLevel) return fail("invalid unit");
        for (ItemId item : unit.items) {
            if (item != 0 && !knownItem(item)) return fail("a unit carries an unknown item");
        }
    }

    health_ = data.health;
    level_ = data.level;
    xp_ = data.xp;
    gold_ = data.gold;
    streak_ = data.streak;
    eliminated_ = data.eliminated;
    placement_ = data.placement;
    itemBag_ = data.itemBag;
    SyncBoardCapacity();
    if (!roster_.Restore(data.units, data.nextUnitSerial)) return fail("roster layout is inconsistent");
    if (roster_.BoardCount() > roster_.BoardCapacity()) return fail("more units on the board than the level allows");
    if (!shop_->RestoreState(data.shopSlots, data.shopRng)) return fail("shop state does not fit the configuration");
    return true;
}

// ---- Lifecycle ---------------------------------------------------------------------------

void PlayerState::Eliminate(int placement) {
    if (eliminated_) return;
    shop_->ReturnShopToPool();
    for (const UnitInstance& unit : roster_.Units()) {
        const bool returned = pool_.Return(unit.champion, SharedChampionPool::CopiesForStarLevel(unit.starLevel));
        assert(returned && "eliminated player owned more copies than were ever in the pool");
        (void)returned;
    }
    roster_.Clear();
    eliminated_ = true;
    placement_ = placement;
}

}  // namespace w2f
