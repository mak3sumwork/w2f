#include "w2f/MotherNature.h"

#include <algorithm>

#include "w2f/Hash.h"

namespace w2f {

bool ItemIsOfClass(const ItemDatabase& items, const ItemDefinition& item, ItemClass cls) {
    switch (cls) {
        case ItemClass::Any: return !item.IsConsumable();
        case ItemClass::Component: return !item.IsCombined() && item.HasEffect() && items.IsComponent(item.id);
        case ItemClass::Legendary: return item.IsCombined() && item.grantsTraits.empty();
        case ItemClass::Emblem: return item.IsCombined() && !item.grantsTraits.empty();
    }
    return false;
}

std::vector<ItemId> GiftItemChoices(const GiftDefinition& gift, const ItemDatabase& items) {
    if (!gift.items.empty()) return gift.items;
    std::vector<ItemId> out;
    for (const ItemDefinition& def : items.All()) {
        if (ItemIsOfClass(items, def, gift.itemClass)) out.push_back(def.id);
    }
    return out;
}

std::unique_ptr<MotherNatureDatabase> MotherNatureDatabase::Create(int options, std::vector<MotherNatureTier> tiers, const ItemDatabase* items,
                                                                    std::string* error) {
    const auto fail = [error](const std::string& message) -> std::unique_ptr<MotherNatureDatabase> {
        if (error) *error = message;
        return nullptr;
    };
    if (options < 1 || options > 4) return fail("options (how many gifts a player is shown) must be 1..4");
    if (tiers.empty()) return fail("Mother Nature needs at least one tier");
    std::sort(tiers.begin(), tiers.end(), [](const MotherNatureTier& a, const MotherNatureTier& b) { return a.fromStage < b.fromStage; });
    if (tiers.front().fromStage != 1) return fail("the first tier must start at stage 1 (otherwise the early rounds have no gifts)");

    std::vector<std::uint32_t> seenGifts;
    std::vector<int> seenTiers;
    for (std::size_t t = 0; t < tiers.size(); ++t) {
        const MotherNatureTier& tier = tiers[t];
        const std::string where = "tier " + std::to_string(tier.id) + " ('" + tier.name + "')";
        if (tier.id <= 0) return fail(where + ": tier ids must be > 0");
        if (std::find(seenTiers.begin(), seenTiers.end(), tier.id) != seenTiers.end()) return fail(where + ": duplicate tier id");
        seenTiers.push_back(tier.id);
        if (t > 0 && tiers[t - 1].fromStage == tier.fromStage) return fail(where + ": two tiers start at the same stage");
        if (tier.fromStage < 1) return fail(where + ": fromStage must be >= 1");
        if (static_cast<int>(tier.gifts.size()) < options) return fail(where + ": needs at least " + std::to_string(options) + " gifts to offer " + std::to_string(options) + " distinct options");
        for (const GiftDefinition& gift : tier.gifts) {
            const std::string what = where + ", gift " + std::to_string(gift.id) + " ('" + gift.name + "')";
            if (gift.id == 0) return fail(where + ": gift ids must be > 0");
            if (std::find(seenGifts.begin(), seenGifts.end(), gift.id) != seenGifts.end()) return fail(what + ": duplicate gift id (ids are unique across the whole file)");
            seenGifts.push_back(gift.id);
            if (gift.name.empty()) return fail(what + ": a gift needs a name");
            if (gift.weight < 1) return fail(what + ": weight must be >= 1");
            switch (gift.type) {
                case GiftType::Gold:
                case GiftType::Xp:
                case GiftType::Heal:
                    if (gift.amount < 1) return fail(what + ": amount must be >= 1");
                    if (!gift.items.empty() || !gift.costs.empty() || !gift.costsByStage.empty()) return fail(what + ": only Item gifts list items and only Unit gifts list costs");
                    break;
                case GiftType::Item:
                    if (gift.amount != 0 || !gift.costs.empty() || !gift.costsByStage.empty()) return fail(what + ": an Item gift has neither an amount nor costs");
                    for (ItemId id : gift.items) {
                        if (items != nullptr && items->Find(id) == nullptr) return fail(what + ": item " + std::to_string(id) + " is not in the item data");
                    }
                    if (items != nullptr && GiftItemChoices(gift, *items).empty()) return fail(what + ": no item of that class exists in the item data");
                    break;
                case GiftType::Unit:
                    if (gift.costs.empty() == gift.costsByStage.empty()) return fail(what + ": a Unit gift needs either \"costs\" or \"costsByStage\" (exactly one)");
                    if (gift.amount != 0 || !gift.items.empty()) return fail(what + ": a Unit gift has neither an amount nor items");
                    for (int cost : gift.costs) {
                        if (cost < 1 || cost > kMaxCostTier) return fail(what + ": unit costs must be 1.." + std::to_string(kMaxCostTier));
                    }
                    for (std::size_t k = 0; k < gift.costsByStage.size(); ++k) {
                        const StageCosts& entry = gift.costsByStage[k];
                        if (k == 0 ? entry.fromStage != 1 : entry.fromStage <= gift.costsByStage[k - 1].fromStage) {
                            return fail(what + ": costsByStage must start at fromStage 1 and ascend");
                        }
                        if (entry.costs.empty()) return fail(what + ": a costsByStage entry needs at least one cost");
                        for (int cost : entry.costs) {
                            if (cost < 1 || cost > kMaxCostTier) return fail(what + ": unit costs must be 1.." + std::to_string(kMaxCostTier));
                        }
                    }
                    break;
            }
        }
    }
    auto db = std::unique_ptr<MotherNatureDatabase>(new MotherNatureDatabase());
    db->options_ = options;
    db->tiers_ = std::move(tiers);
    return db;
}

const MotherNatureTier& MotherNatureDatabase::TierFor(int stage) const {
    const MotherNatureTier* best = &tiers_.front();
    for (const MotherNatureTier& tier : tiers_) {
        if (tier.fromStage <= stage) best = &tier;
    }
    return *best;
}

const GiftDefinition* MotherNatureDatabase::FindGift(std::uint32_t id) const {
    for (const MotherNatureTier& tier : tiers_) {
        for (const GiftDefinition& gift : tier.gifts) {
            if (gift.id == id) return &gift;
        }
    }
    return nullptr;
}

std::uint64_t MotherNatureDatabase::ContentHash() const {
    Fnv1a h;
    h.AddInt(options_);
    h.AddInt(static_cast<std::int64_t>(tiers_.size()));
    for (const MotherNatureTier& tier : tiers_) {
        h.AddInt(tier.id);
        h.AddString(tier.name);
        h.AddInt(tier.fromStage);
        h.AddInt(static_cast<std::int64_t>(tier.gifts.size()));
        for (const GiftDefinition& gift : tier.gifts) {
            h.Add(gift.id);
            h.AddString(gift.name);
            h.Add(static_cast<std::uint64_t>(gift.type));
            h.AddInt(gift.weight);
            h.AddInt(gift.amount);
            h.Add(static_cast<std::uint64_t>(gift.itemClass));
            h.AddInt(static_cast<std::int64_t>(gift.items.size()));
            for (ItemId item : gift.items) h.Add(item);
            h.AddInt(static_cast<std::int64_t>(gift.costs.size()));
            for (int cost : gift.costs) h.AddInt(cost);
            h.AddInt(static_cast<std::int64_t>(gift.costsByStage.size()));
            for (const StageCosts& entry : gift.costsByStage) {
                h.AddInt(entry.fromStage);
                h.AddInt(static_cast<std::int64_t>(entry.costs.size()));
                for (int cost : entry.costs) h.AddInt(cost);
            }
        }
    }
    return h.value;
}

}  // namespace w2f
