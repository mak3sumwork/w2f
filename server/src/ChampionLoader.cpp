#include "w2f/ChampionLoader.h"

#include <fstream>
#include <sstream>

#include "w2f/Json.h"
#include "w2f/Item.h"
#include "w2f/Pve.h"
#include "w2f/Trait.h"

namespace w2f {

namespace {

using json::Value;

template <typename E>
struct EnumName {
    const char* name;
    E value;
};

constexpr EnumName<ChampionRole> kRoles[] = {{"Tank", ChampionRole::Tank}, {"Damage", ChampionRole::Damage}};
constexpr EnumName<DamageType> kDamageTypes[] = {
    {"Physical", DamageType::Physical}, {"Magic", DamageType::Magic}, {"True", DamageType::True}};
// The visuals a damage-over-time may have (its `"visual"`). Burn, Poison, Bleed and Drain can only come from a DoT effect: they are not in kStatusTypes.
constexpr EnumName<StatusType> kDotVisuals[] = {
    {"Burn", StatusType::Burn}, {"Poison", StatusType::Poison}, {"Bleed", StatusType::Bleed}, {"Drain", StatusType::Drain}};
// Burn (and the other DoT visuals) are deliberately absent: they can only come from a DoT effect.
constexpr EnumName<StatusType> kStatusTypes[] = {
    {"Stun", StatusType::Stun},         {"AttackDamage", StatusType::AttackDamage},
    {"AttackSpeed", StatusType::AttackSpeed}, {"Armor", StatusType::Armor},
    {"MagicResist", StatusType::MagicResist}, {"MaxHp", StatusType::MaxHp},
    {"Wound", StatusType::Wound},       {"InflictsWound", StatusType::InflictsWound},
    {"DamageAmp", StatusType::DamageAmp}, {"Root", StatusType::Root},
    {"Knockup", StatusType::Knockup},   {"CcImmunity", StatusType::CcImmunity},
    {"DamageTakenRegen", StatusType::DamageTakenRegen}, {"BonusAttackDamage", StatusType::BonusAttackDamage},
    {"Tether", StatusType::Tether}, {"Untargetable", StatusType::Untargetable}, {"AggroDrop", StatusType::AggroDrop},
    {"BonusArmor", StatusType::BonusArmor}, {"BonusMagicResist", StatusType::BonusMagicResist}, {"BonusMaxHp", StatusType::BonusMaxHp},
    {"BonusAbilityDamage", StatusType::BonusAbilityDamage}, {"BonusCritChance", StatusType::BonusCritChance},
    {"AbilityCrit", StatusType::AbilityCrit}, {"CritDamage", StatusType::CritDamage}, {"CritDamageTakenReduction", StatusType::CritDamageTakenReduction},
    {"BonusManaRegen", StatusType::BonusManaRegen}, {"AbilityPower", StatusType::AbilityPower}, {"SpellShield", StatusType::SpellShield},
    {"Blind", StatusType::Blind}, {"DamageTaken", StatusType::DamageTaken}, {"BonusMaxMana", StatusType::BonusMaxMana},
    {"ExecuteBelow", StatusType::ExecuteBelow}, {"HpPerSecond", StatusType::HpPerSecond}, {"EmpoweredAttack", StatusType::EmpoweredAttack},
    {"ManaCost", StatusType::ManaCost}, {"HealingAmp", StatusType::HealingAmp}, {"Omnivamp", StatusType::Omnivamp}, {"Awakened", StatusType::Awakened}};
constexpr EnumName<StatSource> kSources[] = {
    {"SelfMaxHp", StatSource::SelfMaxHp},
    {"SelfCurrentHp", StatSource::SelfCurrentHp},
    {"SelfAttackDamage", StatSource::SelfAttackDamage},
    {"SelfAbilityDamage", StatSource::SelfAbilityDamage},
    {"SelfArmor", StatSource::SelfArmor},
    {"SelfMagicResist", StatSource::SelfMagicResist},
    {"DamageDealtToTargetInWindow", StatSource::DamageDealtToTargetInWindow},
    {"CastTargetAttackDamage", StatSource::CastTargetAttackDamage},
    {"CastTargetArmor", StatSource::CastTargetArmor},
    {"TriggerDamage", StatSource::TriggerDamage},
    {"HighestAllyMaxHp", StatSource::HighestAllyMaxHp},
    {"TargetMaxHp", StatSource::TargetMaxHp},
    {"TargetCurrentHp", StatSource::TargetCurrentHp},
    {"DamageDealtInWindow", StatSource::DamageDealtInWindow},
    {"RawDamageDealtToTarget", StatSource::RawDamageDealtToTarget},
    {"PlayerLevel", StatSource::PlayerLevel},
    {"TraitGold", StatSource::TraitGold},
    {"TraitStarLevel", StatSource::TraitStarLevel},
    {"TargetCastCount", StatSource::TargetCastCount}};
constexpr EnumName<TargetMode> kTargetModes[] = {{"Self", TargetMode::Self},
                                                 {"CurrentTarget", TargetMode::CurrentTarget},
                                                 {"AreaAroundTarget", TargetMode::AreaAroundTarget},
                                                 {"AreaAroundSelf", TargetMode::AreaAroundSelf},
                                                 {"ClosestEnemies", TargetMode::ClosestEnemies},
                                                 {"LineBehindTarget", TargetMode::LineBehindTarget},
                                                 {"AlliesInStartLine", TargetMode::AlliesInStartLine},
                                                 {"HighestDamageAlly", TargetMode::HighestDamageAlly},
                                                 {"LowestHpAlly", TargetMode::LowestHpAlly},
                                                 {"HighestHpEnemyNearTarget", TargetMode::HighestHpEnemyNearTarget},
                                                 {"AreaAroundDensestEnemy", TargetMode::AreaAroundDensestEnemy},
                                                 {"RandomEnemy", TargetMode::RandomEnemy},
                                                 {"ConeTowardTarget", TargetMode::ConeTowardTarget},
                                                 {"TriggerAttacker", TargetMode::TriggerAttacker},
                                                 {"TriggerVictim", TargetMode::TriggerVictim},
                                                 {"LowestHpEnemy", TargetMode::LowestHpEnemy},
                                                 {"HighestHpEnemy", TargetMode::HighestHpEnemy},
                                                 {"AllEnemies", TargetMode::AllEnemies},
                                                 {"AllAllies", TargetMode::AllAllies},
                                                 {"TopDamageEnemies", TargetMode::TopDamageEnemies}};
constexpr EnumName<TeleportDestination> kDestinations[] = {{"BehindFarthestEnemy", TeleportDestination::BehindFarthestEnemy},
                                                           {"BehindClosestEnemy", TeleportDestination::BehindClosestEnemy},
                                                           {"NextToLowestHpEnemy", TeleportDestination::NextToLowestHpEnemy},
                                                           {"NextToHighestHpEnemy", TeleportDestination::NextToHighestHpEnemy},
                                                           {"BehindCurrentTarget", TeleportDestination::BehindCurrentTarget}};
constexpr EnumName<DisplaceDirection> kDisplaceDirections[] = {{"Toward", DisplaceDirection::TowardCaster}, {"Away", DisplaceDirection::AwayFromCaster},
                                                                  {"TowardAreaCenter", DisplaceDirection::TowardAreaCenter}};
constexpr EnumName<GiftType> kGiftTypes[] = {{"Gold", GiftType::Gold}, {"Xp", GiftType::Xp}, {"Heal", GiftType::Heal}, {"Item", GiftType::Item}, {"Unit", GiftType::Unit}};
constexpr EnumName<ItemClass> kItemClasses[] = {{"Any", ItemClass::Any}, {"Component", ItemClass::Component}, {"Legendary", ItemClass::Legendary}, {"Emblem", ItemClass::Emblem}};
constexpr EnumName<PveDropType> kDropTypes[] = {{"Gold", PveDropType::Gold}, {"Champion", PveDropType::Champion}, {"Item", PveDropType::Item}};
constexpr EnumName<TraitScope> kScopes[] = {{"AllAllies", TraitScope::AllAllies}, {"TraitHolders", TraitScope::TraitHolders}, {"Team", TraitScope::Team}};
constexpr EnumName<DamageFilter> kDamageFilters[] = {{"Any", DamageFilter::Any}, {"Basic", DamageFilter::Basic}, {"Ability", DamageFilter::Ability}};
constexpr EnumName<TargetSide> kSides[] = {{"Enemies", TargetSide::Enemies}, {"Allies", TargetSide::Allies}, {"All", TargetSide::All}};
constexpr EnumName<CastTrigger> kTriggers[] = {{"Mana", CastTrigger::Mana},
                                               {"EveryNthAttack", CastTrigger::EveryNthAttack},
                                               {"StartOfCombat", CastTrigger::StartOfCombat},
                                               {"Passive", CastTrigger::StartOfCombat},  // alias
                                               {"OnBasicAttack", CastTrigger::OnBasicAttack},
                                               {"OnCast", CastTrigger::OnCast},
                                               {"OnTakeBasicAttackDamage", CastTrigger::OnTakeBasicAttackDamage},
                                               {"OnTakeAbilityDamage", CastTrigger::OnTakeAbilityDamage},
                                               {"OnDealDamage", CastTrigger::OnDealDamage},
                                               {"OnCritTaken", CastTrigger::OnCritTaken},
                                               {"OnHpDropBelowPercent", CastTrigger::OnHpDropBelowPercent},
                                               {"OnAllyDealDamage", CastTrigger::OnAllyDealDamage},
                                               {"OnAnyUnitDeath", CastTrigger::OnAnyUnitDeath},
                                               {"OnShieldBreak", CastTrigger::OnShieldBreak},
                                               {"EveryInterval", CastTrigger::EveryInterval},
                                               {"OnEnemyDeath", CastTrigger::OnEnemyDeath},
                                               {"OnTeamHpLoss", CastTrigger::OnTeamHpLoss},
                                               {"OnDeath", CastTrigger::OnDeath},
                                               {"OnAllyDeath", CastTrigger::OnAllyDeath}};
constexpr EnumName<EffectCondition> kConditions[] = {{"NoDamageTakenSinceCast", EffectCondition::NoDamageTakenSinceCast}};

constexpr long long kMaxStat = 10'000'000;
constexpr long long kMaxTicks = 1'000'000;
constexpr long long kMinPercent = -10'000;
constexpr long long kMaxPercent = 1'000'000;

class Loader {
public:
    bool Run(std::string_view text, std::vector<ChampionDefinition>& out, std::string* error) {
        Value root;
        std::string parseError;
        if (!json::Parse(text, root, &parseError)) return Finish(false, parseError, error);

        Obj top;
        if (!Open(root, "$", top)) return Finish(false, "", error);
        const Value* version = Take(top, "version");
        const Value* champions = Take(top, "champions");
        if (version == nullptr) { Missing(top, "version"); return Finish(false, "", error); }
        long long v = 0;
        if (!version->ToInt(v) || v != 1) { Fail("$.version", *version, "unsupported format version (this loader reads version 1)"); return Finish(false, "", error); }
        if (champions == nullptr) { Missing(top, "champions"); return Finish(false, "", error); }
        if (!RejectUnknown(top) || !champions->IsArray()) {
            if (champions->IsArray() == false) Fail("$.champions", *champions, std::string("expected an array, found ") + Value::TypeName(champions->type()));
            return Finish(false, "", error);
        }

        std::vector<ChampionDefinition> parsed;
        for (std::size_t i = 0; i < champions->Items().size(); ++i) {
            ChampionDefinition def;
            if (!ReadChampion(champions->Items()[i], "champions[" + std::to_string(i) + "]", def)) return Finish(false, "", error);
            parsed.push_back(std::move(def));
        }
        out = std::move(parsed);
        return Finish(true, "", error);
    }

    bool RunItems(std::string_view text, std::vector<ItemDefinition>& out, std::string* error) {
        Value root;
        std::string parseError;
        if (!json::Parse(text, root, &parseError)) return Finish(false, parseError, error);
        Obj top;
        if (!Open(root, "$", top)) return Finish(false, "", error);
        const Value* version = Take(top, "version");
        const Value* items = Take(top, "items");
        if (version == nullptr) { Missing(top, "version"); return Finish(false, "", error); }
        long long v = 0;
        if (!version->ToInt(v) || v != 1) { Fail("$.version", *version, "unsupported format version (this loader reads version 1)"); return Finish(false, "", error); }
        if (items == nullptr) { Missing(top, "items"); return Finish(false, "", error); }
        if (!RejectUnknown(top)) return Finish(false, "", error);
        if (!items->IsArray()) { Fail("$.items", *items, std::string("expected an array, found ") + Value::TypeName(items->type())); return Finish(false, "", error); }
        std::vector<ItemDefinition> parsed;
        for (std::size_t i = 0; i < items->Items().size(); ++i) {
            ItemDefinition def;
            if (!ReadItem(items->Items()[i], "items[" + std::to_string(i) + "]", def)) return Finish(false, "", error);
            parsed.push_back(std::move(def));
        }
        out = std::move(parsed);
        return Finish(true, "", error);
    }

    bool RunPve(std::string_view text, PveFile& out, std::string* error) {
        Value root;
        std::string parseError;
        if (!json::Parse(text, root, &parseError)) return Finish(false, parseError, error);
        Obj top;
        if (!Open(root, "$", top)) return Finish(false, "", error);
        const Value* version = Take(top, "version");
        const Value* monsters = Take(top, "monsters");
        const Value* encounters = Take(top, "encounters");
        const Value* defaultDrops = Take(top, "defaultDrops");
        if (version == nullptr) { Missing(top, "version"); return Finish(false, "", error); }
        long long v = 0;
        if (!version->ToInt(v) || v != 1) { Fail("$.version", *version, "unsupported format version (this loader reads version 1)"); return Finish(false, "", error); }
        if (monsters == nullptr) { Missing(top, "monsters"); return Finish(false, "", error); }
        if (encounters == nullptr) { Missing(top, "encounters"); return Finish(false, "", error); }
        if (!RejectUnknown(top)) return Finish(false, "", error);
        for (const auto& [key, value] : {std::pair<const char*, const Value*>{"monsters", monsters}, {"encounters", encounters}}) {
            if (!value->IsArray()) { Fail(std::string("$.") + key, *value, std::string("expected an array, found ") + Value::TypeName(value->type())); return Finish(false, "", error); }
        }

        PveFile parsed;
        monsterMode_ = true;   // a monster needs no shop cost
        for (std::size_t i = 0; i < monsters->Items().size(); ++i) {
            ChampionDefinition def;
            if (!ReadChampion(monsters->Items()[i], "monsters[" + std::to_string(i) + "]", def)) { monsterMode_ = false; return Finish(false, "", error); }
            parsed.monsters.push_back(std::move(def));
        }
        monsterMode_ = false;
        if (defaultDrops && !ReadDrops(*defaultDrops, "$.defaultDrops", parsed.defaultDrops)) return Finish(false, "", error);
        for (std::size_t i = 0; i < encounters->Items().size(); ++i) {
            EncounterDefinition def;
            if (!ReadEncounter(encounters->Items()[i], "encounters[" + std::to_string(i) + "]", def)) return Finish(false, "", error);
            parsed.encounters.push_back(std::move(def));
        }
        out = std::move(parsed);
        return Finish(true, "", error);
    }

    bool ReadGift(const Value& v, const std::string& path, GiftDefinition& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* id = nullptr;
        const Value* name = nullptr;
        const Value* type = nullptr;
        if (!Require(o, "id", id) || !Require(o, "name", name) || !Require(o, "type", type)) return false;
        const Value* weight = Take(o, "weight");
        const Value* amount = Take(o, "amount");
        const Value* itemClass = Take(o, "itemClass");
        const Value* items = Take(o, "items");
        const Value* costs = Take(o, "costs");
        const Value* costsByStage = Take(o, "costsByStage");
        if (!RejectUnknown(o)) return false;
        int idValue = 0;
        if (!ReadInt(*id, path + ".id", 1, 2'000'000'000, idValue)) return false;
        out.id = static_cast<std::uint32_t>(idValue);
        if (!ReadString(*name, path + ".name", out.name) || !ReadEnum(*type, path + ".type", kGiftTypes, out.type)) return false;
        if (weight && !ReadInt(*weight, path + ".weight", 1, 1'000'000, out.weight)) return false;
        if (amount && !ReadInt(*amount, path + ".amount", 1, 1'000'000, out.amount)) return false;
        if (out.type == GiftType::Gold || out.type == GiftType::Xp || out.type == GiftType::Heal) {
            if (!amount) return Missing(o, "amount");
        } else if (amount) {
            return Fail(path + ".amount", *amount, "\"amount\" only applies to Gold, Xp and Heal gifts");
        }
        if (itemClass) {
            if (out.type != GiftType::Item) return Fail(path + ".itemClass", *itemClass, "\"itemClass\" only applies to Item gifts");
            if (!ReadEnum(*itemClass, path + ".itemClass", kItemClasses, out.itemClass)) return false;
        }
        if (items) {
            if (out.type != GiftType::Item) return Fail(path + ".items", *items, "\"items\" only applies to Item gifts");
            if (itemClass) return Fail(path + ".items", *items, "give either \"items\" (an explicit list) or \"itemClass\", not both");
            if (!items->IsArray()) return Fail(path + ".items", *items, std::string("expected an array of item ids, found ") + Value::TypeName(items->type()));
            for (std::size_t i = 0; i < items->Items().size(); ++i) {
                int item = 0;
                if (!ReadInt(items->Items()[i], path + ".items[" + std::to_string(i) + "]", 1, 2'000'000'000, item)) return false;
                out.items.push_back(static_cast<ItemId>(item));
            }
        }
        if (out.type == GiftType::Item && !itemClass && !items) return Fail(path, v, "an Item gift needs \"itemClass\" (Any, Component, Legendary, Emblem) or an explicit \"items\" list");
        if (costs) {
            if (out.type != GiftType::Unit) return Fail(path + ".costs", *costs, "\"costs\" only applies to Unit gifts");
            if (!costs->IsArray()) return Fail(path + ".costs", *costs, std::string("expected an array of cost tiers, found ") + Value::TypeName(costs->type()));
            for (std::size_t i = 0; i < costs->Items().size(); ++i) {
                int cost = 0;
                if (!ReadInt(costs->Items()[i], path + ".costs[" + std::to_string(i) + "]", 1, kMaxCostTier, cost)) return false;
                out.costs.push_back(cost);
            }
        }
        if (costsByStage) {   // "costsByStage": [ { "fromStage": 1, "costs": [1, 2] }, { "fromStage": 3, "costs": [3, 4] } ]
            if (out.type != GiftType::Unit) return Fail(path + ".costsByStage", *costsByStage, "\"costsByStage\" only applies to Unit gifts");
            if (costs) return Fail(path + ".costsByStage", *costsByStage, "give either \"costs\" or \"costsByStage\", not both");
            if (!costsByStage->IsArray()) return Fail(path + ".costsByStage", *costsByStage, std::string("expected an array, found ") + Value::TypeName(costsByStage->type()));
            for (std::size_t i = 0; i < costsByStage->Items().size(); ++i) {
                const std::string entryPath = path + ".costsByStage[" + std::to_string(i) + "]";
                Obj e;
                if (!Open(costsByStage->Items()[i], entryPath, e)) return false;
                const Value* from = nullptr;
                const Value* list = nullptr;
                if (!Require(e, "fromStage", from) || !Require(e, "costs", list) || !RejectUnknown(e)) return false;
                StageCosts entry;
                if (!ReadInt(*from, entryPath + ".fromStage", 1, 1000, entry.fromStage)) return false;
                if (!list->IsArray()) return Fail(entryPath + ".costs", *list, std::string("expected an array of cost tiers, found ") + Value::TypeName(list->type()));
                for (std::size_t k = 0; k < list->Items().size(); ++k) {
                    int cost = 0;
                    if (!ReadInt(list->Items()[k], entryPath + ".costs[" + std::to_string(k) + "]", 1, kMaxCostTier, cost)) return false;
                    entry.costs.push_back(cost);
                }
                out.costsByStage.push_back(std::move(entry));
            }
        } else if (out.type == GiftType::Unit && !costs) {
            return Missing(o, "costs");
        }
        return true;
    }

    bool RunMotherNature(std::string_view text, MotherNatureFile& out, std::string* error) {
        Value root;
        std::string parseError;
        if (!json::Parse(text, root, &parseError)) return Finish(false, parseError, error);
        Obj top;
        if (!Open(root, "$", top)) return Finish(false, "", error);
        const Value* version = Take(top, "version");
        const Value* options = Take(top, "options");
        const Value* tiers = Take(top, "tiers");
        if (version == nullptr) { Missing(top, "version"); return Finish(false, "", error); }
        long long v = 0;
        if (!version->ToInt(v) || v != 1) { Fail("$.version", *version, "unsupported format version (this loader reads version 1)"); return Finish(false, "", error); }
        if (tiers == nullptr) { Missing(top, "tiers"); return Finish(false, "", error); }
        if (!RejectUnknown(top)) return Finish(false, "", error);
        if (!tiers->IsArray()) { Fail("$.tiers", *tiers, std::string("expected an array, found ") + Value::TypeName(tiers->type())); return Finish(false, "", error); }
        MotherNatureFile parsed;
        if (options && !ReadInt(*options, "$.options", 1, 4, parsed.options)) return Finish(false, "", error);
        for (std::size_t i = 0; i < tiers->Items().size(); ++i) {
            const std::string path = "tiers[" + std::to_string(i) + "]";
            Obj t;
            if (!Open(tiers->Items()[i], path, t)) return Finish(false, "", error);
            const Value* id = nullptr;
            const Value* name = nullptr;
            const Value* gifts = nullptr;
            if (!Require(t, "id", id) || !Require(t, "name", name) || !Require(t, "gifts", gifts)) return Finish(false, "", error);
            const Value* fromStage = Take(t, "fromStage");
            if (!RejectUnknown(t)) return Finish(false, "", error);
            MotherNatureTier tier;
            if (!ReadInt(*id, path + ".id", 1, 1'000'000, tier.id) || !ReadString(*name, path + ".name", tier.name)) return Finish(false, "", error);
            if (fromStage && !ReadInt(*fromStage, path + ".fromStage", 1, 1000, tier.fromStage)) return Finish(false, "", error);
            if (!gifts->IsArray()) { Fail(path + ".gifts", *gifts, std::string("expected an array, found ") + Value::TypeName(gifts->type())); return Finish(false, "", error); }
            for (std::size_t g = 0; g < gifts->Items().size(); ++g) {
                GiftDefinition gift;
                if (!ReadGift(gifts->Items()[g], path + ".gifts[" + std::to_string(g) + "]", gift)) return Finish(false, "", error);
                tier.gifts.push_back(std::move(gift));
            }
            parsed.tiers.push_back(std::move(tier));
        }
        out = std::move(parsed);
        return Finish(true, "", error);
    }

    bool RunTraits(std::string_view text, std::vector<TraitDefinition>& out, std::string* error) {
        Value root;
        std::string parseError;
        if (!json::Parse(text, root, &parseError)) return Finish(false, parseError, error);
        Obj top;
        if (!Open(root, "$", top)) return Finish(false, "", error);
        const Value* version = Take(top, "version");
        const Value* traits = Take(top, "traits");
        if (version == nullptr) { Missing(top, "version"); return Finish(false, "", error); }
        long long v = 0;
        if (!version->ToInt(v) || v != 1) { Fail("$.version", *version, "unsupported format version (this loader reads version 1)"); return Finish(false, "", error); }
        if (traits == nullptr) { Missing(top, "traits"); return Finish(false, "", error); }
        if (!RejectUnknown(top)) return Finish(false, "", error);
        if (!traits->IsArray()) { Fail("$.traits", *traits, std::string("expected an array, found ") + Value::TypeName(traits->type())); return Finish(false, "", error); }
        std::vector<TraitDefinition> parsed;
        for (std::size_t i = 0; i < traits->Items().size(); ++i) {
            TraitDefinition def;
            if (!ReadTrait(traits->Items()[i], "traits[" + std::to_string(i) + "]", def)) return Finish(false, "", error);
            parsed.push_back(std::move(def));
        }
        out = std::move(parsed);
        return Finish(true, "", error);
    }

private:
    struct Obj {
        const Value* value = nullptr;
        std::string path;
        std::vector<std::string> known;
    };

    bool Finish(bool ok, const std::string& message, std::string* error) {
        if (!ok && error) *error = message.empty() ? error_ : message;
        return ok;
    }

    // ---- errors ----
    bool Fail(const std::string& path, const Value& at, const std::string& message) {
        if (error_.empty()) {
            error_ = path + " (line " + std::to_string(at.line()) + ", column " + std::to_string(at.column()) + "): " + message;
        }
        return false;
    }
    bool Missing(const Obj& o, const std::string& key) {
        return Fail(o.path, *o.value, "missing required key \"" + key + "\"");
    }

    // ---- objects ----
    bool Open(const Value& v, const std::string& path, Obj& out) {
        if (!v.IsObject()) return Fail(path, v, std::string("expected an object, found ") + Value::TypeName(v.type()));
        out.value = &v;
        out.path = path;
        out.known.clear();
        return true;
    }
    const Value* Take(Obj& o, const std::string& key) {
        o.known.push_back(key);
        return o.value->Find(key);
    }
    bool RejectUnknown(const Obj& o) {
        for (std::size_t i = 0; i < o.value->MemberCount(); ++i) {
            const std::string& key = o.value->MemberKey(i);
            bool known = false;
            for (const std::string& k : o.known) known = known || k == key;
            if (!known) {
                std::string allowed;
                for (const std::string& k : o.known) allowed += (allowed.empty() ? "" : ", ") + k;
                return Fail(o.path + "." + key, o.value->MemberValue(i), "unknown key \"" + key + "\" (allowed here: " + allowed + ")");
            }
        }
        return true;
    }
    bool Require(Obj& o, const std::string& key, const Value*& out) {
        out = Take(o, key);
        return out != nullptr || Missing(o, key);
    }

    // ---- scalars ----
    bool ReadInt(const Value& v, const std::string& path, long long lo, long long hi, int& out) {
        long long n = 0;
        if (!v.IsNumber()) return Fail(path, v, std::string("expected a number, found ") + Value::TypeName(v.type()));
        if (!v.ToInt(n)) return Fail(path, v, "expected a whole number");
        if (n < lo || n > hi) return Fail(path, v, "value " + std::to_string(n) + " is out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
        out = static_cast<int>(n);
        return true;
    }
    bool ReadBool(const Value& v, const std::string& path, bool& out) {
        if (!v.IsBool()) return Fail(path, v, std::string("expected true or false, found ") + Value::TypeName(v.type()));
        out = v.AsBool();
        return true;
    }
    bool ReadString(const Value& v, const std::string& path, std::string& out) {
        if (!v.IsString()) return Fail(path, v, std::string("expected a string, found ") + Value::TypeName(v.type()));
        out = v.AsString();
        return true;
    }
    // A decimal with at most 3 decimals, returned in thousandths (0.78 -> 780).
    bool ReadMilli(const Value& v, const std::string& path, long long lo, long long hi, int& out) {
        long long n = 0;
        if (!v.IsNumber()) return Fail(path, v, std::string("expected a number, found ") + Value::TypeName(v.type()));
        if (!v.ToScaled(3, n)) return Fail(path, v, "expected at most 3 decimal places");
        if (n < lo || n > hi) return Fail(path, v, "value is out of range");
        out = static_cast<int>(n);
        return true;
    }
    bool ReadSecondsAsTicks(const Value& v, const std::string& path, int& out) {
        long long n = 0;
        if (!v.IsNumber()) return Fail(path, v, std::string("expected a number of seconds, found ") + Value::TypeName(v.type()));
        if (!v.ToTicks(kTicksPerSecond, n) || n > kMaxTicks) return Fail(path, v, "expected a non-negative number of seconds (not too large)");
        out = static_cast<int>(n);
        return true;
    }
    template <typename E, std::size_t N>
    bool ReadEnum(const Value& v, const std::string& path, const EnumName<E> (&table)[N], E& out) {
        if (!v.IsString()) return Fail(path, v, std::string("expected a string, found ") + Value::TypeName(v.type()));
        for (const auto& entry : table) {
            if (v.AsString() == entry.name) { out = entry.value; return true; }
        }
        std::string expected;
        for (const auto& entry : table) expected += (expected.empty() ? "" : ", ") + std::string(entry.name);
        return Fail(path, v, "unknown value \"" + v.AsString() + "\"; expected one of: " + expected);
    }

    // ---- per-star values: one number (all three stars) or an array of exactly three ----
    template <typename ReadOne>
    bool ReadStar(const Value& v, const std::string& path, StarValue& out, ReadOne readOne) {
        if (v.IsArray()) {
            if (v.Items().size() != static_cast<std::size_t>(kMaxStarLevel)) {
                return Fail(path, v, "expected exactly " + std::to_string(kMaxStarLevel) + " values (one per star level), found " + std::to_string(v.Items().size()));
            }
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (!readOne(v.Items()[i], path + "[" + std::to_string(i) + "]", out[i])) return false;
            }
            return true;
        }
        int single = 0;
        if (!readOne(v, path, single)) return false;
        out = Same(single);
        return true;
    }
    bool ReadStarInts(const Value& v, const std::string& path, long long lo, long long hi, StarValue& out) {
        return ReadStar(v, path, out, [&](const Value& item, const std::string& p, int& o) { return ReadInt(item, p, lo, hi, o); });
    }
    bool ReadStarSeconds(const Value& v, const std::string& path, StarValue& out) {
        return ReadStar(v, path, out, [&](const Value& item, const std::string& p, int& o) { return ReadSecondsAsTicks(item, p, o); });
    }

    // <base>Ticks or <base>Seconds (per star). Sets `found` if either is present.
    bool ReadStarTime(Obj& o, const std::string& base, StarValue& out, bool& found) {
        const Value* ticks = Take(o, base + "Ticks");
        const Value* seconds = Take(o, base + "Seconds");
        found = ticks != nullptr || seconds != nullptr;
        if (ticks && seconds) return Fail(o.path, *o.value, "give either \"" + base + "Ticks\" or \"" + base + "Seconds\", not both");
        if (ticks) return ReadStarInts(*ticks, o.path + "." + base + "Ticks", 0, kMaxTicks, out);
        if (seconds) return ReadStarSeconds(*seconds, o.path + "." + base + "Seconds", out);
        return true;
    }
    // Same, but a single value (not per star).
    bool ReadScalarTime(Obj& o, const std::string& base, int& out, bool& found) {
        const Value* ticks = Take(o, base + "Ticks");
        const Value* seconds = Take(o, base + "Seconds");
        found = ticks != nullptr || seconds != nullptr;
        if (ticks && seconds) return Fail(o.path, *o.value, "give either \"" + base + "Ticks\" or \"" + base + "Seconds\", not both");
        if (ticks) return ReadInt(*ticks, o.path + "." + base + "Ticks", 0, kMaxTicks, out);
        if (seconds) return ReadSecondsAsTicks(*seconds, o.path + "." + base + "Seconds", out);
        return true;
    }

    // ---- formulas ----
    bool ReadAmount(const Value& v, const std::string& path, Amount& out) {
        out = Amount{};
        if (v.IsNumber() || v.IsArray()) return ReadStarInts(v, path, -kMaxStat, kMaxStat, out.flat);  // shorthand: just a flat value
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* flat = Take(o, "flat");
        const Value* flatSeconds = Take(o, "flatSeconds");
        const Value* terms = Take(o, "terms");
        if (!RejectUnknown(o)) return false;
        if (flat && flatSeconds) return Fail(path, v, "give either \"flat\" or \"flatSeconds\", not both");
        if (flat && !ReadStarInts(*flat, path + ".flat", -kMaxStat, kMaxStat, out.flat)) return false;
        if (flatSeconds && !ReadStarSeconds(*flatSeconds, path + ".flatSeconds", out.flat)) return false;
        if (terms) {
            if (!terms->IsArray()) return Fail(path + ".terms", *terms, std::string("expected an array, found ") + Value::TypeName(terms->type()));
            for (std::size_t i = 0; i < terms->Items().size(); ++i) {
                ScalingTerm term;
                if (!ReadTerm(terms->Items()[i], path + ".terms[" + std::to_string(i) + "]", term)) return false;
                out.terms.push_back(term);
            }
        }
        if (!flat && !flatSeconds && !terms) return Fail(path, v, "an amount needs \"flat\", \"flatSeconds\" and/or \"terms\"");
        return true;
    }

    bool ReadTerm(const Value& v, const std::string& path, ScalingTerm& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* source = nullptr;
        const Value* percent = nullptr;
        if (!Require(o, "source", source)) return false;
        const Value* permille = Take(o, "permille");   // "percent" in thousandths: 5 = 0.5%
        if (permille && o.value->Find("percent") != nullptr) return Fail(path, v, "give either \"percent\" or \"permille\", not both");
        if (permille) percent = permille;
        else if (!Require(o, "percent", percent)) return false;
        const Value* trait = Take(o, "trait");
        int window = 0;
        bool haveWindow = false;
        if (!ReadScalarTime(o, "window", window, haveWindow)) return false;
        if (!RejectUnknown(o)) return false;
        if (!ReadEnum(*source, path + ".source", kSources, out.source)) return false;
        if ((out.source == StatSource::TraitStarLevel) != (trait != nullptr)) return Fail(path, v, "a TraitStarLevel term (and only that) names a \"trait\"");
        if (trait && !ReadString(*trait, path + ".trait", out.trait)) return false;
        if (!ReadStarInts(*percent, path + (permille ? ".permille" : ".percent"), kMinPercent * (permille ? 10 : 1), kMaxPercent * (permille ? 10 : 1), out.percent)) return false;
        if (permille) out.divisor = 1000;
        out.windowTicks = window;
        const bool windowed = out.source == StatSource::DamageDealtInWindow || out.source == StatSource::DamageDealtToTargetInWindow;
        if (windowed && !haveWindow) {
            return Fail(path, v, "a windowed source needs \"windowSeconds\" or \"windowTicks\"");
        }
        if (!windowed && haveWindow) {
            return Fail(path, v, "a window only applies to DamageDealtInWindow / DamageDealtToTargetInWindow");
        }
        return true;
    }

    bool ReadTarget(const Value& v, const std::string& path, TargetSpec& out) {
        out = TargetSpec{};
        if (v.IsString()) {  // shorthand for the modes that take no options
            if (!ReadEnum(v, path, kTargetModes, out.mode)) return false;
            switch (out.mode) {
                case TargetMode::Self: out = TargetSpec::Self(); return true;
                case TargetMode::CurrentTarget: return true;
                case TargetMode::AlliesInStartLine: out = TargetSpec::StartLine(); return true;
                case TargetMode::LowestHpAlly: out = TargetSpec::LowestHpAlly(); return true;
                case TargetMode::RandomEnemy: out = TargetSpec::RandomEnemy(); return true;
                case TargetMode::HighestDamageAlly: out = TargetSpec::HighestDamageAlly(); return true;
                case TargetMode::TriggerAttacker: out = TargetSpec::TriggerAttacker(); return true;
                case TargetMode::TriggerVictim: out = TargetSpec::TriggerVictim(); return true;
                case TargetMode::LowestHpEnemy:
                case TargetMode::HighestHpEnemy:
                case TargetMode::AllEnemies: out.side = TargetSide::Enemies; return true;
                case TargetMode::AllAllies: out.side = TargetSide::Allies; return true;
                default: break;
            }
            return Fail(path, v, "this target needs the long form, e.g. {\"mode\": \"AreaAroundTarget\", \"radius\": 3}");
        }
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* mode = nullptr;
        if (!Require(o, "mode", mode)) return false;
        const Value* radius = Take(o, "radius");
        const Value* includeCenter = Take(o, "includeCenter");
        const Value* side = Take(o, "side");
        const Value* count = Take(o, "count");
        const Value* length = Take(o, "length");
        const Value* traitFilter = Take(o, "trait");         // only units carrying this trait tag
        const Value* championFilter = Take(o, "champion");   // only units of this champion
        int window = 0;
        bool haveWindow = false;
        if (!ReadScalarTime(o, "window", window, haveWindow)) return false;
        if (!RejectUnknown(o) || !ReadEnum(*mode, path + ".mode", kTargetModes, out.mode)) return false;
        if (traitFilter && !ReadString(*traitFilter, path + ".trait", out.trait)) return false;
        if (championFilter) {
            int id = 0;
            if (!ReadInt(*championFilter, path + ".champion", 1, 2'000'000'000, id)) return false;
            out.champion = static_cast<ChampionId>(id);
        }
        const bool area = out.mode == TargetMode::AreaAroundTarget || out.mode == TargetMode::AreaAroundSelf || out.mode == TargetMode::AreaAroundDensestEnemy;
        const bool zone = area || out.mode == TargetMode::HighestHpEnemyNearTarget;
        switch (out.mode) {
            case TargetMode::Self: out.side = TargetSide::All; break;
            case TargetMode::AlliesInStartLine:
            case TargetMode::HighestDamageAlly:
            case TargetMode::AllAllies:
            case TargetMode::LowestHpAlly: out.side = TargetSide::Allies; break;
            default: out.side = TargetSide::Enemies; break;
        }
        if (zone && !radius) return Missing(o, "radius");
        if (!zone && radius) return Fail(path, v, "\"radius\" only applies to the Area modes and HighestHpEnemyNearTarget");
        if (includeCenter && !area) return Fail(path, v, "\"includeCenter\" only applies to the Area modes");
        if (out.mode == TargetMode::ClosestEnemies || out.mode == TargetMode::TopDamageEnemies) {
            if (!count) return Missing(o, "count");
        } else if (count && !area && out.mode != TargetMode::LowestHpAlly) {
            return Fail(path, v, "\"count\" only applies to ClosestEnemies, LowestHpAlly and the Area modes (at most that many, nearest first)");
        }
        const bool lineLike = out.mode == TargetMode::LineBehindTarget || out.mode == TargetMode::ConeTowardTarget;
        if (lineLike) {
            if (!length) return Missing(o, "length");
        } else if (length) {
            return Fail(path, v, "\"length\" only applies to LineBehindTarget and ConeTowardTarget");
        }
        if (haveWindow && out.mode != TargetMode::HighestDamageAlly) return Fail(path, v, "a window only applies to HighestDamageAlly");
        if (radius && !ReadInt(*radius, path + ".radius", 0, 32, out.radius)) return false;
        if (includeCenter && !ReadBool(*includeCenter, path + ".includeCenter", out.includeCenter)) return false;
        if (count && count->IsArray()) {   // per star: [2, 3, 3]
            if (!ReadStarInts(*count, path + ".count", 1, 16, out.countPerStar)) return false;
            out.count = out.countPerStar[0];
        } else if (count && !ReadInt(*count, path + ".count", 1, 16, out.count)) {
            return false;
        }
        if (length && !ReadInt(*length, path + ".length", 1, 16, out.radius)) return false;
        if (side && !ReadEnum(*side, path + ".side", kSides, out.side)) return false;
        out.windowTicks = window;
        return true;
    }

    // The keys of a StatusEffect, shared by "Status" effects and "onKill" entries. Takes what it needs from `o` and
    // rejects anything else in it.
    bool ReadStatusBody(Obj& o, const Value& v, const std::string& path, StatusEffect& s) {
        const Value* status = nullptr;
        if (!Require(o, "status", status)) return false;
        const Value* percent = Take(o, "percent");
        const Value* value = Take(o, "value");
        const Value* multiplier = Take(o, "multiplierPercent");
        const Value* permanent = Take(o, "permanent");
        const Value* stacking = Take(o, "stacking");
        // Duration is optional only for permanent statuses.
        const Value* formula = Take(o, "duration");
        StarValue flat{};
        bool haveFlat = false;
        if (!ReadStarTime(o, "duration", flat, haveFlat) || !RejectUnknown(o)) return false;
        if (!ReadEnum(*status, path + ".status", kStatusTypes, s.status)) return false;
        if (permanent && !ReadBool(*permanent, path + ".permanent", s.permanent)) return false;
        if (stacking) {
            if (!stacking->IsString() || (stacking->AsString() != "add" && stacking->AsString() != "refresh")) {
                return Fail(path + ".stacking", *stacking, "expected \"add\" (the default: same-type statuses add up) or \"refresh\"");
            }
            s.refreshes = stacking->AsString() == "refresh";
        }
        if (s.permanent) {
            if (formula || haveFlat) return Fail(path, v, "a permanent status has no duration");
        } else {
            if (formula && haveFlat) return Fail(path, v, "give one of \"duration\", \"durationTicks\", \"durationSeconds\"");
            if (!formula && !haveFlat) return Missing(o, "duration (or durationTicks / durationSeconds), or \"permanent\": true");
            if (formula) {
                if (!ReadAmount(*formula, path + ".duration", s.duration)) return false;
            } else {
                s.duration = FlatAmount(flat);
            }
        }
        if (percent && percent->IsObject() && !IsFlatBonusStatus(s.status)) {   // {"flat": 10, "terms": [...]}: a percent that is a formula
            if (!ReadAmount(*percent, path + ".percent", s.value)) return false;
            s.percentFromValue = true;
            percent = nullptr;
            if (value) return Fail(path, v, "a formula percent cannot also have a \"value\"");
        } else if (percent && !ReadStarInts(*percent, path + ".percent", kMinPercent, kMaxPercent, s.percent)) {
            return false;
        }
        const bool durationOnly = s.status == StatusType::Blind || s.status == StatusType::Stun || s.status == StatusType::Root || s.status == StatusType::Knockup ||
                                  s.status == StatusType::CcImmunity || s.status == StatusType::Untargetable || s.status == StatusType::AggroDrop ||
                                  s.status == StatusType::AbilityCrit || s.status == StatusType::SpellShield;
        if (IsFlatBonusStatus(s.status)) {
            if (!value) return Missing(o, "value");
            if (percent) return Fail(path, v, "a flat bonus status takes a \"value\" formula, not \"percent\"");
            if (!ReadAmount(*value, path + ".value", s.value)) return false;
        } else {
            if (value) return Fail(path, v, "\"value\" only applies to the flat bonus statuses (BonusAttackDamage, BonusArmor, ...)");
            if (!percent && !durationOnly && !s.percentFromValue && s.status != StatusType::Awakened) return Missing(o, "percent");
        }
        if (multiplier && !ReadInt(*multiplier, path + ".multiplierPercent", 0, kMaxPercent, s.multiplierPercent)) return false;
        return true;
    }

    // "duration" (a formula), "durationTicks" or "durationSeconds" (flat, per star): exactly one.
    bool ReadDuration(Obj& o, Amount& out) {
        const Value* formula = Take(o, "duration");
        StarValue flat{};
        bool haveFlat = false;
        if (!ReadStarTime(o, "duration", flat, haveFlat)) return false;
        if (formula && haveFlat) return Fail(o.path, *o.value, "give one of \"duration\", \"durationTicks\", \"durationSeconds\"");
        if (!formula && !haveFlat) return Missing(o, "duration (or durationTicks / durationSeconds)");
        if (formula) return ReadAmount(*formula, o.path + ".duration", out);
        out = FlatAmount(flat);
        return true;
    }

    // ---- effects ----
    bool ReadEffect(const Value& v, const std::string& path, AbilityEffect& out, TraitScope* scopeOut = nullptr) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* type = nullptr;
        if (!Require(o, "type", type)) return false;
        if (!type->IsString()) return Fail(path + ".type", *type, "expected a string");
        const std::string kind = type->AsString();

        const Value* target = Take(o, "target");
        const Value* scope = scopeOut ? Take(o, "scope") : nullptr;   // only trait effects have a scope
        const Value* repeat = Take(o, "repeat");
        const Value* condition = Take(o, "condition");
        int delay = 0;
        bool haveDelay = false;
        if (!ReadScalarTime(o, "delay", delay, haveDelay)) return false;
        out.delayTicks = delay;
        if (condition && !ReadEnum(*condition, path + ".condition", kConditions, out.condition)) return false;
        if (repeat) {   // {"count": 8, "everySeconds": 0.5}: runs `count` times, the first at the delay, then at that spacing
            Obj r;
            if (!Open(*repeat, path + ".repeat", r)) return false;
            const Value* count = nullptr;
            if (!Require(r, "count", count)) return false;
            int every = 0;
            bool haveEvery = false;
            if (!ReadScalarTime(r, "every", every, haveEvery) || !RejectUnknown(r)) return false;
            if (!ReadInt(*count, path + ".repeat.count", 1, 64, out.repeatCount)) return false;
            if (out.repeatCount > 1 && (!haveEvery || every < 1)) return Fail(path + ".repeat", *repeat, "a repeat of more than 1 needs everySeconds / everyTicks of at least 1 tick");
            out.repeatIntervalTicks = every;
        }
        if (target) {
            if (!ReadTarget(*target, path + ".target", out.target)) return false;
        } else if (scopeOut) {
            out.target = TargetSpec::Self();   // synergy effects always apply to each qualifying unit itself
        } else {
            return Missing(o, "target");
        }
        if (scopeOut) {
            if (!scope) return Missing(o, "scope");
            if (!ReadEnum(*scope, path + ".scope", kScopes, *scopeOut)) return false;
        }

        if (kind == "Damage") {
            DamageEffect d;
            const Value* damageType = Take(o, "damageType");
            const Value* amount = nullptr;
            if (!Require(o, "amount", amount)) return false;
            const Value* multiplier = Take(o, "multiplierPercent");
            const Value* canCrit = Take(o, "canCrit");
            const Value* armorPen = Take(o, "armorPenPercent");
            const Value* onKill = Take(o, "onKill");
            const Value* bounce = Take(o, "bounceOnCritPercent");
            if (!RejectUnknown(o)) return false;
            if (bounce && !ReadInt(*bounce, path + ".bounceOnCritPercent", 0, 1000, d.bounceOnCritPercent)) return false;
            if (!damageType) return Missing(o, "damageType");
            if (!ReadEnum(*damageType, path + ".damageType", kDamageTypes, d.type) || !ReadAmount(*amount, path + ".amount", d.amount)) return false;
            if (multiplier && !ReadInt(*multiplier, path + ".multiplierPercent", 0, kMaxPercent, d.multiplierPercent)) return false;
            if (canCrit && !ReadBool(*canCrit, path + ".canCrit", d.canCrit)) return false;
            if (armorPen && !ReadInt(*armorPen, path + ".armorPenPercent", 0, 100, d.armorPenPercent)) return false;
            if (onKill) {   // statuses the killer gains: [ {"status": "AggroDrop", "durationSeconds": 1.5}, ... ]
                if (!onKill->IsArray()) return Fail(path + ".onKill", *onKill, std::string("expected an array, found ") + Value::TypeName(onKill->type()));
                for (std::size_t i = 0; i < onKill->Items().size(); ++i) {
                    const std::string killPath = path + ".onKill[" + std::to_string(i) + "]";
                    Obj k;
                    if (!Open(onKill->Items()[i], killPath, k)) return false;
                    StatusEffect reward;
                    if (!ReadStatusBody(k, onKill->Items()[i], killPath, reward)) return false;
                    d.onKill.push_back(std::move(reward));
                }
            }
            out.payload = d;
        } else if (kind == "Shield") {
            ShieldEffect s;
            const Value* amount = nullptr;
            if (!Require(o, "amount", amount)) return false;
            const Value* reduction = Take(o, "damageReductionPercent");
            const Value* permanent = Take(o, "permanent");
            const Value* cap = Take(o, "cap");
            const Value* durationFormula = Take(o, "duration");
            StarValue durationFlat{};
            bool haveDurationFlat = false;
            if (!ReadStarTime(o, "duration", durationFlat, haveDurationFlat) || !RejectUnknown(o)) return false;
            if (permanent && !ReadBool(*permanent, path + ".permanent", s.permanent)) return false;
            if (s.permanent) {
                if (durationFormula || haveDurationFlat) return Fail(path, v, "a permanent shield has no duration");
            } else {
                if (durationFormula && haveDurationFlat) return Fail(path, v, "give one of \"duration\", \"durationTicks\", \"durationSeconds\"");
                if (!durationFormula && !haveDurationFlat) return Missing(o, "duration (or durationTicks / durationSeconds), or \"permanent\": true");
                if (durationFormula) {
                    if (!ReadAmount(*durationFormula, path + ".duration", s.duration)) return false;
                } else {
                    s.duration = FlatAmount(durationFlat);
                }
            }
            if (!ReadAmount(*amount, path + ".amount", s.amount)) return false;
            if (reduction && !ReadStarInts(*reduction, path + ".damageReductionPercent", 0, 100, s.damageReductionPercent)) return false;
            if (cap) {
                if (!ReadAmount(*cap, path + ".cap", s.cap)) return false;
                s.capped = true;
            }
            out.payload = s;
        } else if (kind == "Status") {
            StatusEffect st;
            if (!ReadStatusBody(o, v, path, st)) return false;
            out.payload = st;
        } else if (kind == "Teleport") {
            TeleportEffect tp;
            const Value* destination = Take(o, "destination");
            if (!RejectUnknown(o)) return false;
            if (destination && !ReadEnum(*destination, path + ".destination", kDestinations, tp.destination)) return false;
            out.payload = tp;
        } else if (kind == "Mana") {
            ManaEffect m;
            const Value* amount = nullptr;
            if (!Require(o, "amount", amount) || !RejectUnknown(o)) return false;
            if (!ReadAmount(*amount, path + ".amount", m.amount)) return false;
            out.payload = m;
        } else if (kind == "Summon") {
            SummonEffect sm;
            const Value* champion = nullptr;
            if (!Require(o, "champion", champion)) return false;
            const Value* count = Take(o, "count");
            const Value* star = Take(o, "star");
            const Value* maxHp = Take(o, "maxHp");
            const Value* attackDamage = Take(o, "attackDamage");
            if (!RejectUnknown(o)) return false;
            int championId = 0;
            if (!ReadInt(*champion, path + ".champion", 1, 2'000'000'000, championId)) return false;
            sm.champion = static_cast<ChampionId>(championId);
            if (count && !ReadAmount(*count, path + ".count", sm.count)) return false;
            if (star && !ReadInt(*star, path + ".star", 0, kMaxStarLevel, sm.starLevel)) return false;
            if (maxHp && !ReadAmount(*maxHp, path + ".maxHp", sm.maxHp)) return false;
            if (attackDamage && !ReadAmount(*attackDamage, path + ".attackDamage", sm.attackDamage)) return false;
            out.payload = sm;
        } else if (kind == "Displace") {
            DisplaceEffect dp;
            const Value* direction = nullptr;
            const Value* hexes = Take(o, "hexes");
            if (!Require(o, "direction", direction) || !RejectUnknown(o)) return false;
            if (!ReadEnum(*direction, path + ".direction", kDisplaceDirections, dp.direction)) return false;
            if (hexes && !ReadInt(*hexes, path + ".hexes", 1, 8, dp.hexes)) return false;
            out.payload = dp;
        } else if (kind == "Heal") {
            HealEffect h;
            const Value* amount = nullptr;
            if (!Require(o, "amount", amount) || !RejectUnknown(o)) return false;
            if (!ReadAmount(*amount, path + ".amount", h.amount)) return false;
            out.payload = h;
        } else if (kind == "DoT") {
            DotEffect d;
            const Value* damageType = Take(o, "damageType");
            const Value* amount = nullptr;
            if (!Require(o, "amount", amount)) return false;
            const Value* isTotal = Take(o, "amountIsTotal");
            const Value* stack = Take(o, "stackBonusPercent");
            const Value* drain = Take(o, "healPercent");
            const Value* refreshes = Take(o, "refreshes");
            const Value* visual = Take(o, "visual");   // "Burn" (default), "Poison", "Bleed" or "Drain": what the viewer shows
            int interval = 0;
            bool haveInterval = false;
            if (!ReadScalarTime(o, "interval", interval, haveInterval)) return false;
            if (!ReadDuration(o, d.duration) || !RejectUnknown(o)) return false;
            if (!damageType) return Missing(o, "damageType");
            if (!ReadEnum(*damageType, path + ".damageType", kDamageTypes, d.type) || !ReadAmount(*amount, path + ".amount", d.amount)) return false;
            if (isTotal && !ReadBool(*isTotal, path + ".amountIsTotal", d.amountIsTotal)) return false;
            if (stack && !ReadStarInts(*stack, path + ".stackBonusPercent", 0, kMaxPercent, d.stackBonusPercent)) return false;
            if (drain && !ReadInt(*drain, path + ".healPercent", 0, 1000, d.healPercent)) return false;
            if (refreshes && !ReadBool(*refreshes, path + ".refreshes", d.refreshes)) return false;
            if (visual && !ReadEnum(*visual, path + ".visual", kDotVisuals, d.visual)) return false;
            if (!haveInterval) return Missing(o, "intervalTicks (or intervalSeconds)");
            if (interval < 1) return Fail(path + ".interval", v, "the interval must be at least 1 tick");
            d.intervalTicks = interval;
            out.payload = d;
        } else if (kind == "AllyStrike") {
            AllyStrikeEffect as;
            const Value* ally = nullptr;
            const Value* percent = nullptr;
            if (!Require(o, "champion", ally) || !Require(o, "percentOfAllyAttackDamage", percent)) return false;
            const Value* damageType = Take(o, "damageType");
            const Value* canCrit = Take(o, "canCrit");
            if (!RejectUnknown(o)) return false;
            int allyId = 0;
            if (!ReadInt(*ally, path + ".champion", 1, 2'000'000'000, allyId)) return false;
            as.ally = static_cast<ChampionId>(allyId);
            if (!ReadStarInts(*percent, path + ".percentOfAllyAttackDamage", 0, 10000, as.percentOfAllyAttackDamage)) return false;
            if (damageType && !ReadEnum(*damageType, path + ".damageType", kDamageTypes, as.type)) return false;
            if (canCrit && !ReadBool(*canCrit, path + ".canCrit", as.canCrit)) return false;
            out.payload = as;
        } else if (kind == "Clone") {
            CloneEffect ce;
            const Value* trait = nullptr;
            if (!Require(o, "trait", trait)) return false;
            const Value* count = Take(o, "count");
            const Value* statPercent = Take(o, "statPercent");
            if (!RejectUnknown(o) || !ReadString(*trait, path + ".trait", ce.trait)) return false;
            if (count && !ReadInt(*count, path + ".count", 1, 4, ce.count)) return false;
            if (statPercent && !ReadInt(*statPercent, path + ".statPercent", 1, 200, ce.statPercent)) return false;
            out.payload = ce;
        } else {
            return Fail(path + ".type", *type, "unknown effect type \"" + kind + "\"; expected one of: Damage, Shield, Status, DoT, Heal, Teleport, Displace, Mana, Summon, AllyStrike, Clone");
        }
        return true;
    }

    bool ReadAbility(const Value& v, const std::string& path, bool isPassive, AbilityDefinition& out,
                     CastTrigger defaultTrigger = CastTrigger::None) {
        out = AbilityDefinition{};
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* id = nullptr;
        const Value* name = nullptr;
        const Value* effects = nullptr;
        if (!Require(o, "id", id) || !Require(o, "name", name) || !Require(o, "effects", effects)) return false;
        const Value* trigger = Take(o, "trigger");
        const Value* attackCount = Take(o, "attackCount");
        const Value* thresholdPercent = Take(o, "thresholdPercent");
        const Value* damageFilter = Take(o, "damageFilter");
        const Value* maxTriggers = Take(o, "maxTriggers");
        const Value* resetOnTargetChange = Take(o, "resetOnTargetChange");
        const Value* castOnDeath = Take(o, "castOnDeath");
        const Value* requiresCharge = Take(o, "requiresCharge");
        const Value* onlyShieldsFrom = Take(o, "onlyShieldsFrom");
        const Value* afterAttacks = Take(o, "afterAttacks");
        const Value* triggerTrait = Take(o, "triggerTrait");
        const Value* stopsOnDeath = Take(o, "stopsOnDeath");
        int interval = 0;
        bool haveInterval = false;
        int lock = 0;
        bool haveLock = false;
        int channel = 0;
        bool haveChannel = false;
        int windup = 0;
        bool haveWindup = false;
        if (!ReadScalarTime(o, "castLock", lock, haveLock) || !ReadScalarTime(o, "channel", channel, haveChannel) ||
            !ReadScalarTime(o, "windup", windup, haveWindup) ||
            !ReadScalarTime(o, "interval", interval, haveInterval) || !RejectUnknown(o)) return false;

        int idValue = 0;
        if (!ReadInt(*id, path + ".id", 1, 2'000'000'000, idValue)) return false;
        out.id = static_cast<AbilityId>(idValue);
        if (!ReadString(*name, path + ".name", out.name)) return false;
        if (trigger) {
            if (!ReadEnum(*trigger, path + ".trigger", kTriggers, out.trigger)) return false;
        } else if (isPassive) {
            out.trigger = CastTrigger::StartOfCombat;
        } else if (defaultTrigger != CastTrigger::None) {
            out.trigger = defaultTrigger;
        } else {
            return Missing(o, "trigger");
        }
        if (attackCount && !ReadInt(*attackCount, path + ".attackCount", 1, 1000, out.attackCount)) return false;
        if (thresholdPercent && !ReadInt(*thresholdPercent, path + ".thresholdPercent", 1, 99, out.thresholdPercent)) return false;
        if (damageFilter && !ReadEnum(*damageFilter, path + ".damageFilter", kDamageFilters, out.damageFilter)) return false;
        if (maxTriggers && !ReadInt(*maxTriggers, path + ".maxTriggers", 1, 1000, out.maxTriggers)) return false;
        if (resetOnTargetChange && !ReadBool(*resetOnTargetChange, path + ".resetOnTargetChange", out.resetCountOnTargetChange)) return false;
        if (castOnDeath && !ReadBool(*castOnDeath, path + ".castOnDeath", out.castOnDeath)) return false;
        if (requiresCharge && !ReadBool(*requiresCharge, path + ".requiresCharge", out.requiresCharge)) return false;
        if (afterAttacks && !ReadStarInts(*afterAttacks, path + ".afterAttacks", 0, 1000, out.afterAttacks)) return false;
        if (triggerTrait && !ReadString(*triggerTrait, path + ".triggerTrait", out.triggerTrait)) return false;
        if (stopsOnDeath && !ReadBool(*stopsOnDeath, path + ".stopsOnDeath", out.stopsOnDeath)) return false;
        if (onlyShieldsFrom) {
            int from = 0;
            if (!ReadInt(*onlyShieldsFrom, path + ".onlyShieldsFrom", 1, 2'000'000'000, from)) return false;
            out.shieldFromAbility = static_cast<AbilityId>(from);
        }
        out.intervalTicks = haveInterval ? interval : 0;
        out.castLockTicks = lock;
        if (haveWindup) out.windupTicks = windup;   // (presentation: the animation's lead time; see CombatEvent::windup)
        out.channelTicks = channel;

        if (!effects->IsArray()) return Fail(path + ".effects", *effects, std::string("expected an array, found ") + Value::TypeName(effects->type()));
        for (std::size_t i = 0; i < effects->Items().size(); ++i) {
            AbilityEffect effect;
            if (!ReadEffect(effects->Items()[i], path + ".effects[" + std::to_string(i) + "]", effect)) return false;
            out.effects.push_back(std::move(effect));
        }
        return true;
    }

    // ---- PvE ----
    bool ReadDrops(const Value& v, const std::string& path, std::vector<PveDropEntry>& out) {
        if (!v.IsArray()) return Fail(path, v, std::string("expected an array of drops, found ") + Value::TypeName(v.type()));
        for (std::size_t i = 0; i < v.Items().size(); ++i) {
            const std::string dropPath = path + "[" + std::to_string(i) + "]";
            Obj o;
            if (!Open(v.Items()[i], dropPath, o)) return false;
            const Value* type = nullptr;
            if (!Require(o, "type", type)) return false;
            const Value* weight = Take(o, "weight");
            const Value* minGold = Take(o, "minGold");
            const Value* maxGold = Take(o, "maxGold");
            const Value* tiers = Take(o, "tiers");
            const Value* items = Take(o, "items");
            const Value* count = Take(o, "count");
            if (!RejectUnknown(o)) return false;
            PveDropEntry entry;
            if (count && !ReadInt(*count, dropPath + ".count", 1, 20, entry.count)) return false;
            if (!ReadEnum(*type, dropPath + ".type", kDropTypes, entry.type)) return false;
            if (weight && !ReadInt(*weight, dropPath + ".weight", 1, 1'000'000, entry.weight)) return false;
            if (entry.type == PveDropType::Gold) {
                if (!minGold || !maxGold) return Fail(dropPath, v.Items()[i], "a Gold drop needs \"minGold\" and \"maxGold\"");
                if (!ReadInt(*minGold, dropPath + ".minGold", 0, 1'000'000, entry.minGold) || !ReadInt(*maxGold, dropPath + ".maxGold", 0, 1'000'000, entry.maxGold)) return false;
            } else if (minGold || maxGold) {
                return Fail(dropPath, v.Items()[i], "\"minGold\" / \"maxGold\" only apply to Gold drops");
            }
            if (entry.type == PveDropType::Champion) {
                if (!tiers) return Missing(o, "tiers");
                if (!tiers->IsArray()) return Fail(dropPath + ".tiers", *tiers, std::string("expected an array of cost tiers, found ") + Value::TypeName(tiers->type()));
                for (std::size_t t = 0; t < tiers->Items().size(); ++t) {
                    int tier = 0;
                    if (!ReadInt(tiers->Items()[t], dropPath + ".tiers[" + std::to_string(t) + "]", 1, kMaxCostTier, tier)) return false;
                    entry.tiers.push_back(tier);
                }
            } else if (tiers) {
                return Fail(dropPath, v.Items()[i], "\"tiers\" only applies to Champion drops");
            }
            if (items) {
                if (entry.type != PveDropType::Item) return Fail(dropPath, v.Items()[i], "\"items\" only applies to Item drops");
                if (!items->IsArray()) return Fail(dropPath + ".items", *items, std::string("expected an array of item ids, found ") + Value::TypeName(items->type()));
                for (std::size_t t = 0; t < items->Items().size(); ++t) {
                    int item = 0;
                    if (!ReadInt(items->Items()[t], dropPath + ".items[" + std::to_string(t) + "]", 1, 2'000'000'000, item)) return false;
                    entry.items.push_back(static_cast<ItemId>(item));
                }
            }
            out.push_back(std::move(entry));
        }
        return true;
    }

    bool ReadEncounter(const Value& v, const std::string& path, EncounterDefinition& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* id = nullptr;
        const Value* name = nullptr;
        const Value* units = nullptr;
        if (!Require(o, "id", id) || !Require(o, "name", name) || !Require(o, "units", units)) return false;
        const Value* stage = Take(o, "stage");
        const Value* round = Take(o, "round");
        const Value* drops = Take(o, "drops");
        const Value* guaranteed = Take(o, "guaranteedDrops");
        if (!RejectUnknown(o)) return false;
        int idValue = 0;
        if (!ReadInt(*id, path + ".id", 1, 2'000'000'000, idValue)) return false;
        out.id = static_cast<std::uint32_t>(idValue);
        if (!ReadString(*name, path + ".name", out.name)) return false;
        if (stage && !ReadInt(*stage, path + ".stage", 0, 1000, out.stage)) return false;
        if (round && !ReadInt(*round, path + ".round", 0, 1000, out.round)) return false;
        if (!units->IsArray()) return Fail(path + ".units", *units, std::string("expected an array, found ") + Value::TypeName(units->type()));
        for (std::size_t i = 0; i < units->Items().size(); ++i) {
            const std::string unitPath = path + ".units[" + std::to_string(i) + "]";
            Obj u;
            if (!Open(units->Items()[i], unitPath, u)) return false;
            const Value* monster = nullptr;
            const Value* x = nullptr;
            const Value* y = nullptr;
            if (!Require(u, "monster", monster) || !Require(u, "x", x) || !Require(u, "y", y)) return false;
            const Value* star = Take(u, "star");
            if (!RejectUnknown(u)) return false;
            MonsterPlacement placed;
            int monsterId = 0;
            if (!ReadInt(*monster, unitPath + ".monster", 1, 2'000'000'000, monsterId)) return false;
            placed.monster = static_cast<ChampionId>(monsterId);
            if (!ReadInt(*x, unitPath + ".x", 0, kBoardColumns - 1, placed.x) || !ReadInt(*y, unitPath + ".y", 0, kBoardRows - 1, placed.y)) return false;
            if (star && !ReadInt(*star, unitPath + ".star", 1, kMaxStarLevel, placed.starLevel)) return false;
            out.units.push_back(placed);
        }
        if (drops && !ReadDrops(*drops, path + ".drops", out.drops)) return false;
        if (guaranteed && !ReadDrops(*guaranteed, path + ".guaranteedDrops", out.guaranteed)) return false;
        return true;
    }

    // ---- items ----
    bool ReadItem(const Value& v, const std::string& path, ItemDefinition& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* id = nullptr;
        const Value* name = nullptr;
        if (!Require(o, "id", id) || !Require(o, "name", name)) return false;
        const Value* stats = Take(o, "stats");
        const Value* grants = Take(o, "grantsTraits");
        const Value* components = Take(o, "components");
        const Value* abilities = Take(o, "abilities");
        const Value* auras = Take(o, "auras");
        const Value* consumable = Take(o, "consumable");
        if (!RejectUnknown(o)) return false;
        int idValue = 0;
        if (!ReadInt(*id, path + ".id", 1, 2'000'000'000, idValue)) return false;
        out.id = static_cast<ItemId>(idValue);
        if (consumable) {   // "consumable": "RemoveAllItems"  (the Item Remover)
            std::string use;
            if (!ReadString(*consumable, path + ".consumable", use)) return false;
            if (use == "RemoveAllItems") out.use = ItemUse::RemoveAllItems;
            else return Fail(path + ".consumable", *consumable, "unknown consumable \"" + use + "\" (expected \"RemoveAllItems\")");
        }
        if (components) {   // "components": [ 4, 7 ]  the two BASE items this one is made from
            if (!components->IsArray() || components->Items().size() != 2) return Fail(path + ".components", *components, "expected an array of exactly two item ids");
            for (std::size_t i = 0; i < 2; ++i) {
                int c = 0;
                if (!ReadInt(components->Items()[i], path + ".components[" + std::to_string(i) + "]", 1, 2'000'000'000, c)) return false;
                out.components[i] = static_cast<ItemId>(c);
            }
        }
        if (abilities && !ReadAbilityList(*abilities, path + ".abilities", out.abilities)) return false;
        if (auras && !ReadAuras(*auras, path + ".auras", out.auras)) return false;
        if (!ReadString(*name, path + ".name", out.name)) return false;
        if (stats) {
            Obj st;
            const std::string statsPath = path + ".stats";
            if (!Open(*stats, statsPath, st)) return false;
            struct Field { const char* key; int* target; long long lo, hi; };
            ItemStats& is = out.stats;
            const Field fields[] = {{"hp", &is.maxHp, -100000, 100000},       {"armor", &is.armor, -10000, 10000},
                                    {"magicResist", &is.magicResist, -10000, 10000}, {"attackDamage", &is.attackDamage, -10000, 10000},
                                    {"abilityDamage", &is.abilityDamage, -10000, 10000}, {"attackSpeedPercent", &is.attackSpeedPercent, -90, 1000},
                                    {"critChance", &is.critChance, -100, 100},  {"startMana", &is.startMana, 0, 1000}};
            const Value* values[8] = {};
            for (std::size_t i = 0; i < 8; ++i) values[i] = Take(st, fields[i].key);
            const Value* manaRegen = Take(st, "manaRegen");   // mana per second, up to 3 decimals ("+1 Mana Regen" = 1)
            if (!RejectUnknown(st)) return false;
            for (std::size_t i = 0; i < 8; ++i) {
                if (values[i] && !ReadInt(*values[i], statsPath + "." + fields[i].key, fields[i].lo, fields[i].hi, *fields[i].target)) return false;
            }
            if (manaRegen && !ReadMilli(*manaRegen, statsPath + ".manaRegen", -10000, 20000, is.manaRegenMilli)) return false;
        }
        if (grants) {
            if (!grants->IsArray()) return Fail(path + ".grantsTraits", *grants, std::string("expected an array of strings, found ") + Value::TypeName(grants->type()));
            for (std::size_t i = 0; i < grants->Items().size(); ++i) {
                std::string trait;
                if (!ReadString(grants->Items()[i], path + ".grantsTraits[" + std::to_string(i) + "]", trait)) return false;
                out.grantsTraits.push_back(std::move(trait));
            }
        }
        return true;
    }

    // ---- traits ----
    // An array of abilities (a champion's `triggers`, an item's `abilities`): each needs its own "trigger".
    bool ReadAbilityList(const Value& v, const std::string& path, std::vector<AbilityDefinition>& out) {
        if (!v.IsArray()) return Fail(path, v, std::string("expected an array of abilities, found ") + Value::TypeName(v.type()));
        for (std::size_t i = 0; i < v.Items().size(); ++i) {
            AbilityDefinition a;
            if (!ReadAbility(v.Items()[i], path + "[" + std::to_string(i) + "]", false, a)) return false;
            out.push_back(std::move(a));
        }
        return true;
    }

    // Auras: [ { "side": "Enemies", "radius": 2, "status": "MagicResist", "amount": -30 } ]
    bool ReadAuras(const Value& v, const std::string& path, std::vector<AuraDefinition>& out) {
        if (!v.IsArray()) return Fail(path, v, std::string("expected an array of auras, found ") + Value::TypeName(v.type()));
        for (std::size_t i = 0; i < v.Items().size(); ++i) {
            const std::string auraPath = path + "[" + std::to_string(i) + "]";
            Obj o;
            if (!Open(v.Items()[i], auraPath, o)) return false;
            const Value* side = nullptr;
            const Value* radius = nullptr;
            const Value* status = nullptr;
            const Value* amount = nullptr;
            if (!Require(o, "side", side) || !Require(o, "radius", radius) || !Require(o, "status", status) || !Require(o, "amount", amount)) return false;
            const Value* includeSelf = Take(o, "includeSelf");
            if (!RejectUnknown(o)) return false;
            AuraDefinition a;
            if (!ReadEnum(*side, auraPath + ".side", kSides, a.side)) return false;
            if (!ReadInt(*radius, auraPath + ".radius", 1, 15, a.radius)) return false;
            if (!ReadEnum(*status, auraPath + ".status", kStatusTypes, a.status)) return false;
            if (!ReadInt(*amount, auraPath + ".amount", kMinPercent, kMaxPercent, a.amount)) return false;
            if (includeSelf && !ReadBool(*includeSelf, auraPath + ".includeSelf", a.includeSelf)) return false;
            out.push_back(a);
        }
        return true;
    }

    bool ReadTrait(const Value& v, const std::string& path, TraitDefinition& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* id = nullptr;
        const Value* name = nullptr;
        if (!Require(o, "id", id) || !Require(o, "name", name)) return false;
        const Value* breakpoints = Take(o, "breakpoints");
        const Value* paths = Take(o, "paths");
        const Value* mutations = Take(o, "mutations");
        const Value* modules = Take(o, "modules");
        const Value* invention = Take(o, "invention");
        const Value* queen = Take(o, "queen");
        if (!RejectUnknown(o)) return false;
        int idValue = 0;
        if (!ReadInt(*id, path + ".id", 1, 2'000'000'000, idValue)) return false;
        out.id = static_cast<TraitId>(idValue);
        if (!ReadString(*name, path + ".name", out.name)) return false;
        if (breakpoints && !ReadBreakpoints(*breakpoints, path + ".breakpoints", out.breakpoints)) return false;
        if (paths) {   // [ { "name": "Enlightenment", "breakpoints": [...] }, ... ]
            if (!paths->IsArray()) return Fail(path + ".paths", *paths, "expected an array");
            for (std::size_t i = 0; i < paths->Items().size(); ++i) {
                const std::string pp = path + ".paths[" + std::to_string(i) + "]";
                Obj p;
                if (!Open(paths->Items()[i], pp, p)) return false;
                const Value* pathName = nullptr;
                const Value* pathBreakpoints = nullptr;
                if (!Require(p, "name", pathName) || !Require(p, "breakpoints", pathBreakpoints) || !RejectUnknown(p)) return false;
                TraitPath tp;
                if (!ReadString(*pathName, pp + ".name", tp.name) || !ReadBreakpoints(*pathBreakpoints, pp + ".breakpoints", tp.breakpoints)) return false;
                out.paths.push_back(std::move(tp));
            }
        }
        if (mutations) {
            if (!mutations->IsArray()) return Fail(path + ".mutations", *mutations, "expected an array");
            for (std::size_t i = 0; i < mutations->Items().size(); ++i) {
                const std::string mp = path + ".mutations[" + std::to_string(i) + "]";
                Obj m;
                if (!Open(mutations->Items()[i], mp, m)) return false;
                const Value* mName = nullptr;
                if (!Require(m, "name", mName)) return false;
                const Value* classes = Take(m, "classes");
                const Value* effects = Take(m, "effects");
                const Value* triggers = Take(m, "triggers");
                const Value* superEffects = Take(m, "superEffects");
                const Value* superTriggers = Take(m, "superTriggers");
                if (!RejectUnknown(m)) return false;
                TraitMutation mutation;
                if (!ReadString(*mName, mp + ".name", mutation.name)) return false;
                if (classes) {
                    if (!classes->IsArray()) return Fail(mp + ".classes", *classes, "expected an array of trait names");
                    for (std::size_t c = 0; c < classes->Items().size(); ++c) {
                        std::string cls;
                        if (!ReadString(classes->Items()[c], mp + ".classes[" + std::to_string(c) + "]", cls)) return false;
                        mutation.classes.push_back(std::move(cls));
                    }
                }
                if (effects && !ReadEffectList(*effects, mp + ".effects", mutation.effects)) return false;
                if (superEffects && !ReadEffectList(*superEffects, mp + ".superEffects", mutation.superEffects)) return false;
                if (triggers && !ReadAbilityList(*triggers, mp + ".triggers", mutation.triggers)) return false;
                if (superTriggers && !ReadAbilityList(*superTriggers, mp + ".superTriggers", mutation.superTriggers)) return false;
                out.mutations.push_back(std::move(mutation));
            }
        }
        if (modules) {   // [ { "id": 301, "name": "Electrical Overload", "tier": 1, "effects": [...] , "goldAfterCombat": 0, "echoEverySeconds": 8 } ]
            if (!modules->IsArray()) return Fail(path + ".modules", *modules, "expected an array");
            for (std::size_t i = 0; i < modules->Items().size(); ++i) {
                const std::string mp = path + ".modules[" + std::to_string(i) + "]";
                Obj m;
                if (!Open(modules->Items()[i], mp, m)) return false;
                const Value* mId = nullptr;
                const Value* mName = nullptr;
                const Value* tier = nullptr;
                if (!Require(m, "id", mId) || !Require(m, "name", mName) || !Require(m, "tier", tier)) return false;
                const Value* effects = Take(m, "effects");
                const Value* gold = Take(m, "goldAfterCombat");
                int echo = 0;
                bool haveEcho = false;
                if (!ReadScalarTime(m, "echoEvery", echo, haveEcho) || !RejectUnknown(m)) return false;
                TraitModule module;
                int mid = 0;
                if (!ReadInt(*mId, mp + ".id", 1, 2'000'000'000, mid) || !ReadString(*mName, mp + ".name", module.name) ||
                    !ReadInt(*tier, mp + ".tier", 1, 3, module.tier)) return false;
                module.id = static_cast<std::uint32_t>(mid);
                if (gold && !ReadInt(*gold, mp + ".goldAfterCombat", 0, 50, module.goldAfterCombat)) return false;
                module.echo = haveEcho;
                module.echoEveryTicks = echo;
                if (effects) {
                    module.ability.id = module.id;
                    module.ability.name = module.name;
                    module.ability.trigger = CastTrigger::StartOfCombat;
                    if (!ReadEffectList(*effects, mp + ".effects", module.ability.effects)) return false;
                }
                out.modules.push_back(std::move(module));
            }
        }
        if (invention) {
            int inv = 0;
            if (!ReadInt(*invention, path + ".invention", 1, 2'000'000'000, inv)) return false;
            out.invention = static_cast<ChampionId>(inv);
        }
        if (queen) {   // { "champion": 9043, "uniqueHolders": 7, "level": 10 }
            Obj q;
            if (!Open(*queen, path + ".queen", q)) return false;
            const Value* c = nullptr;
            const Value* holders = nullptr;
            const Value* level = nullptr;
            if (!Require(q, "champion", c) || !Require(q, "uniqueHolders", holders) || !Require(q, "level", level) || !RejectUnknown(q)) return false;
            int cid = 0;
            if (!ReadInt(*c, path + ".queen.champion", 1, 2'000'000'000, cid) || !ReadInt(*holders, path + ".queen.uniqueHolders", 1, 20, out.queen.uniqueHolders) ||
                !ReadInt(*level, path + ".queen.level", 1, kMaxPlayerLevel, out.queen.level)) return false;
            out.queen.champion = static_cast<ChampionId>(cid);
        }
        return true;
    }

    bool ReadEffectList(const Value& v, const std::string& path, std::vector<AbilityEffect>& out) {
        if (!v.IsArray()) return Fail(path, v, std::string("expected an array, found ") + Value::TypeName(v.type()));
        for (std::size_t k = 0; k < v.Items().size(); ++k) {
            AbilityEffect effect;
            if (!ReadEffect(v.Items()[k], path + "[" + std::to_string(k) + "]", effect)) return false;
            out.push_back(std::move(effect));
        }
        return true;
    }

    bool ReadBreakpoints(const Value& v, const std::string& path, std::vector<TraitBreakpoint>& out) {
        if (!v.IsArray()) return Fail(path, v, std::string("expected an array, found ") + Value::TypeName(v.type()));
        for (std::size_t i = 0; i < v.Items().size(); ++i) {
            const std::string bpPath = path + "[" + std::to_string(i) + "]";
            Obj b;
            if (!Open(v.Items()[i], bpPath, b)) return false;
            const Value* count = nullptr;
            if (!Require(b, "count", count)) return false;
            const Value* effects = Take(b, "effects");
            const Value* triggers = Take(b, "triggers");
            const Value* mutationSlots = Take(b, "mutationSlots");
            const Value* supercharge = Take(b, "supercharge");
            const Value* xp = Take(b, "xpAfterCombat");
            const Value* takedowns = Take(b, "takedownsPerGold");
            const Value* starDust = Take(b, "starDust");
            const Value* grantUnit = Take(b, "grantUnit");
            const Value* plants = Take(b, "plants");
            const Value* plantStar = Take(b, "plantStar");
            const Value* moduleTier = Take(b, "moduleTier");
            if (!RejectUnknown(b)) return false;
            TraitBreakpoint bp;
            if (!ReadInt(*count, bpPath + ".count", 1, 100, bp.count)) return false;
            if (effects) {
                if (!effects->IsArray()) return Fail(bpPath + ".effects", *effects, std::string("expected an array, found ") + Value::TypeName(effects->type()));
                for (std::size_t k = 0; k < effects->Items().size(); ++k) {
                    TraitEffect te;
                    if (!ReadEffect(effects->Items()[k], bpPath + ".effects[" + std::to_string(k) + "]", te.effect, &te.scope)) return false;
                    bp.effects.push_back(std::move(te));
                }
            }
            if (triggers) {   // hooks handed to every unit in scope: [ { "scope": "TraitHolders", "ability": { ...an ability with a hook trigger... } } ]
                if (!triggers->IsArray()) return Fail(bpPath + ".triggers", *triggers, std::string("expected an array, found ") + Value::TypeName(triggers->type()));
                for (std::size_t k = 0; k < triggers->Items().size(); ++k) {
                    const std::string tPath = bpPath + ".triggers[" + std::to_string(k) + "]";
                    Obj t;
                    if (!Open(triggers->Items()[k], tPath, t)) return false;
                    const Value* scope = nullptr;
                    const Value* ability = nullptr;
                    if (!Require(t, "scope", scope) || !Require(t, "ability", ability) || !RejectUnknown(t)) return false;
                    TraitTrigger tt;
                    if (!ReadEnum(*scope, tPath + ".scope", kScopes, tt.scope)) return false;
                    if (!ReadAbility(*ability, tPath + ".ability", false, tt.ability)) return false;
                    bp.triggers.push_back(std::move(tt));
                }
            }
            if (mutationSlots && !ReadInt(*mutationSlots, bpPath + ".mutationSlots", -1, 20, bp.mutationSlots)) return false;
            if (supercharge && !ReadBool(*supercharge, bpPath + ".supercharge", bp.supercharge)) return false;
            if (xp && !ReadInt(*xp, bpPath + ".xpAfterCombat", 0, 100, bp.xpAfterCombat)) return false;
            if (takedowns && !ReadInt(*takedowns, bpPath + ".takedownsPerGold", 1, 100, bp.takedownsPerGold)) return false;
            if (starDust) {   // { "onLoss": 20, "perLossStreak": 5, "perTakedown": 2, "multiplier": 2 }
                Obj d;
                if (!Open(*starDust, bpPath + ".starDust", d)) return false;
                const Value* onLoss = Take(d, "onLoss");
                const Value* perStreak = Take(d, "perLossStreak");
                const Value* perTakedown = Take(d, "perTakedown");
                const Value* multiplier = Take(d, "multiplier");
                const Value* perCombat = Take(d, "perCombat");
                if (!RejectUnknown(d)) return false;
                if (perCombat && !ReadInt(*perCombat, bpPath + ".starDust.perCombat", 0, 1000, bp.starDust.perCombat)) return false;
                if (onLoss && !ReadInt(*onLoss, bpPath + ".starDust.onLoss", 0, 1000, bp.starDust.onLoss)) return false;
                if (perStreak && !ReadInt(*perStreak, bpPath + ".starDust.perLossStreak", 0, 1000, bp.starDust.perLossStreak)) return false;
                if (perTakedown && !ReadInt(*perTakedown, bpPath + ".starDust.perTakedown", 0, 1000, bp.starDust.perTakedown)) return false;
                if (multiplier && !ReadInt(*multiplier, bpPath + ".starDust.multiplier", 1, 10, bp.starDust.multiplier)) return false;
            }
            if (grantUnit) {   // { "champion": 9041, "afterCombats": 2 }
                Obj g;
                if (!Open(*grantUnit, bpPath + ".grantUnit", g)) return false;
                const Value* c = nullptr;
                const Value* after = Take(g, "afterCombats");
                if (!Require(g, "champion", c) || !RejectUnknown(g)) return false;
                int cid = 0;
                if (!ReadInt(*c, bpPath + ".grantUnit.champion", 1, 2'000'000'000, cid)) return false;
                bp.grantUnit.champion = static_cast<ChampionId>(cid);
                if (after && !ReadInt(*after, bpPath + ".grantUnit.afterCombats", 0, 50, bp.grantUnit.afterCombats)) return false;
            }
            if (plants) {   // [ { "champion": 9110, "count": 2 }, ... ]
                if (!plants->IsArray()) return Fail(bpPath + ".plants", *plants, "expected an array");
                for (std::size_t k = 0; k < plants->Items().size(); ++k) {
                    const std::string gp = bpPath + ".plants[" + std::to_string(k) + "]";
                    Obj g;
                    if (!Open(plants->Items()[k], gp, g)) return false;
                    const Value* c = nullptr;
                    const Value* n = nullptr;
                    if (!Require(g, "champion", c) || !Require(g, "count", n) || !RejectUnknown(g)) return false;
                    PlantGrant grant;
                    int cid = 0;
                    if (!ReadInt(*c, gp + ".champion", 1, 2'000'000'000, cid) || !ReadInt(*n, gp + ".count", 1, 4, grant.count)) return false;
                    grant.champion = static_cast<ChampionId>(cid);
                    bp.plants.push_back(grant);
                }
            }
            if (plantStar && !ReadInt(*plantStar, bpPath + ".plantStar", 1, kMaxStarLevel, bp.plantStar)) return false;
            if (moduleTier && !ReadInt(*moduleTier, bpPath + ".moduleTier", 1, 3, bp.moduleTier)) return false;
            out.push_back(std::move(bp));
        }
        return true;
    }

    // ---- champions ----
    bool ReadStats(const Value& v, const std::string& path, CombatStats& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* hp = nullptr;
        const Value* armor = nullptr;
        const Value* magicResist = nullptr;
        const Value* attackDamage = nullptr;
        const Value* attackSpeed = nullptr;
        const Value* range = nullptr;
        if (!Require(o, "hp", hp) || !Require(o, "armor", armor) || !Require(o, "magicResist", magicResist) ||
            !Require(o, "attackDamage", attackDamage) || !Require(o, "attackSpeed", attackSpeed) || !Require(o, "range", range)) {
            return false;
        }
        const Value* abilityDamage = Take(o, "abilityDamage");
        const Value* abilityPowerPercent = Take(o, "abilityPowerPercent");
        const Value* crit = Take(o, "crit");
        const Value* maxMana = Take(o, "maxMana");
        const Value* startMana = Take(o, "startMana");
        const Value* manaRegen = Take(o, "manaRegen");
        const Value* attackType = Take(o, "attackType");
        const Value* projectileSpeed = Take(o, "projectileSpeed");   // hexes per second (0 = a melee blow); default: ranged champions shoot at CombatConfig's speed
        int spread = 0;
        bool haveSpread = false;
        int windup = 0;
        bool haveWindup = false;
        if (!ReadScalarTime(o, "attackSpread", spread, haveSpread) || !ReadScalarTime(o, "attackWindup", windup, haveWindup) || !RejectUnknown(o)) return false;

        if (!ReadStarInts(*hp, path + ".hp", 1, kMaxStat, out.maxHp)) return false;
        if (!ReadStarInts(*armor, path + ".armor", 0, kMaxStat, out.armor)) return false;
        if (!ReadStarInts(*magicResist, path + ".magicResist", 0, kMaxStat, out.magicResist)) return false;
        if (!ReadStarInts(*attackDamage, path + ".attackDamage", 0, kMaxStat, out.attackDamage)) return false;
        if (!ReadMilli(*attackSpeed, path + ".attackSpeed", 1, 100'000, out.attackSpeedMilli)) return false;
        if (!ReadInt(*range, path + ".range", 1, 32, out.attackRange)) return false;
        if (abilityDamage && !ReadStarInts(*abilityDamage, path + ".abilityDamage", 0, kMaxStat, out.abilityDamage)) return false;
        if (abilityPowerPercent && !ReadInt(*abilityPowerPercent, path + ".abilityPowerPercent", 0, kMaxPercent, out.abilityPower)) return false;
        if (crit && !ReadStarInts(*crit, path + ".crit", 0, 100, out.critChance)) return false;
        if (maxMana && !ReadInt(*maxMana, path + ".maxMana", 0, 100'000, out.maxMana)) return false;
        if (startMana && !ReadInt(*startMana, path + ".startMana", 0, 100'000, out.startMana)) return false;
        if (manaRegen && !ReadMilli(*manaRegen, path + ".manaRegen", 0, 1'000'000, out.manaRegenMilli)) return false;
        if (attackType && !ReadEnum(*attackType, path + ".attackType", kDamageTypes, out.attackType)) return false;
        out.attackSpreadTicks = spread;
        if (haveWindup) out.attackWindupTicks = windup;   // (presentation only)
        if (projectileSpeed && !ReadMilli(*projectileSpeed, path + ".projectileSpeed", 0, 1'000'000, out.projectileSpeedMilli)) return false;
        return true;
    }

    // "pilot": { "hpPercent": 80, "bonuses": [ { "traits": ["Bastion", "Protector"], "effects": [ ... ] }, ... ] }
    bool ReadPilot(const Value& v, const std::string& path, PilotDefinition& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* hp = Take(o, "hpPercent");
        const Value* bonuses = Take(o, "bonuses");
        if (!RejectUnknown(o)) return false;
        out.enabled = true;
        if (hp && !ReadInt(*hp, path + ".hpPercent", 0, 500, out.hpPercent)) return false;
        if (bonuses) {
            if (!bonuses->IsArray()) return Fail(path + ".bonuses", *bonuses, "expected an array");
            for (std::size_t i = 0; i < bonuses->Items().size(); ++i) {
                const std::string bp = path + ".bonuses[" + std::to_string(i) + "]";
                Obj b;
                if (!Open(bonuses->Items()[i], bp, b)) return false;
                const Value* traits = nullptr;
                const Value* effects = nullptr;
                if (!Require(b, "traits", traits) || !Require(b, "effects", effects) || !RejectUnknown(b)) return false;
                PilotBonus bonus;
                if (!traits->IsArray() || !effects->IsArray()) return Fail(bp, bonuses->Items()[i], "\"traits\" and \"effects\" are arrays");
                for (std::size_t t = 0; t < traits->Items().size(); ++t) {
                    std::string name;
                    if (!ReadString(traits->Items()[t], bp + ".traits[" + std::to_string(t) + "]", name)) return false;
                    bonus.traits.push_back(std::move(name));
                }
                for (std::size_t e = 0; e < effects->Items().size(); ++e) {
                    AbilityEffect effect;
                    if (!ReadEffect(effects->Items()[e], bp + ".effects[" + std::to_string(e) + "]", effect)) return false;
                    bonus.effects.push_back(std::move(effect));
                }
                out.bonuses.push_back(std::move(bonus));
            }
        }
        return true;
    }

    bool ReadChampion(const Value& v, const std::string& path, ChampionDefinition& out) {
        Obj o;
        if (!Open(v, path, o)) return false;
        const Value* id = nullptr;
        const Value* name = nullptr;
        const Value* cost = nullptr;
        const Value* stats = nullptr;
        if (!Require(o, "id", id) || !Require(o, "name", name) || !Require(o, "stats", stats)) return false;
        const Value* summon = Take(o, "summon");
        bool isSummon = false;
        if (summon && !ReadBool(*summon, path + ".summon", isSummon)) return false;
        if (monsterMode_ || isSummon) {
            cost = Take(o, "cost");   // monsters and summons are never sold: a cost is accepted but not needed
        } else if (!Require(o, "cost", cost)) {
            return false;
        }
        out.summon = isSummon;
        const Value* triggersValue = Take(o, "triggers");
        const Value* aurasValue = Take(o, "auras");
        const Value* role = Take(o, "role");
        const Value* traits = Take(o, "traits");
        const Value* ability = Take(o, "ability");
        const Value* passive = Take(o, "passive");
        const Value* onAttack = Take(o, "onAttack");
        const Value* plant = Take(o, "plant");
        const Value* special = Take(o, "special");
        const Value* price = Take(o, "price");
        const Value* teamSlots = Take(o, "teamSlots");
        const Value* stationary = Take(o, "stationary");
        const Value* pilot = Take(o, "pilot");
        const Value* unlock = Take(o, "unlock");
        if (!RejectUnknown(o)) return false;
        if (unlock) {   // { "trait": "Hexagon", "starLevel": 7, "playerLevel": 8 }
            Obj u;
            if (!Open(*unlock, path + ".unlock", u)) return false;
            const Value* trait = nullptr;
            const Value* stars = nullptr;
            if (!Require(u, "trait", trait) || !Require(u, "starLevel", stars)) return false;
            const Value* level = Take(u, "playerLevel");
            if (!RejectUnknown(u)) return false;
            if (!ReadString(*trait, path + ".unlock.trait", out.unlock.trait)) return false;
            if (!ReadInt(*stars, path + ".unlock.starLevel", 1, 100, out.unlock.starLevel)) return false;
            if (level && !ReadInt(*level, path + ".unlock.playerLevel", 1, 20, out.unlock.playerLevel)) return false;
        }
        if (plant && !ReadBool(*plant, path + ".plant", out.plant)) return false;
        if (special && !ReadBool(*special, path + ".special", out.special)) return false;
        if (price && !ReadInt(*price, path + ".price", 1, 20, out.price)) return false;
        if (out.plant) out.teamSlots = 0;
        if (teamSlots && !ReadInt(*teamSlots, path + ".teamSlots", 0, 2, out.teamSlots)) return false;
        if (stationary && !ReadBool(*stationary, path + ".stationary", out.stationary)) return false;
        if (pilot && !ReadPilot(*pilot, path + ".pilot", out.pilot)) return false;

        int idValue = 0;
        if (!ReadInt(*id, path + ".id", 1, 2'000'000'000, idValue)) return false;
        out.id = static_cast<ChampionId>(idValue);
        if (!ReadString(*name, path + ".name", out.name)) return false;
        if (cost && !ReadInt(*cost, path + ".cost", 1, kMaxCostTier, out.cost)) return false;
        if (role && !ReadEnum(*role, path + ".role", kRoles, out.role)) return false;
        if (traits) {
            if (!traits->IsArray()) return Fail(path + ".traits", *traits, std::string("expected an array of strings, found ") + Value::TypeName(traits->type()));
            for (std::size_t i = 0; i < traits->Items().size(); ++i) {
                std::string trait;
                if (!ReadString(traits->Items()[i], path + ".traits[" + std::to_string(i) + "]", trait)) return false;
                out.traits.push_back(std::move(trait));
            }
        }
        if (!ReadStats(*stats, path + ".stats", out.stats)) return false;
        if (ability && !ReadAbility(*ability, path + ".ability", false, out.ability)) return false;
        if (passive && !ReadAbility(*passive, path + ".passive", true, out.passive)) return false;
        if (onAttack && !ReadAbility(*onAttack, path + ".onAttack", false, out.onAttack, CastTrigger::OnBasicAttack)) return false;
        if (triggersValue && !ReadAbilityList(*triggersValue, path + ".triggers", out.triggers)) return false;
        if (aurasValue && !ReadAuras(*aurasValue, path + ".auras", out.auras)) return false;
        return true;
    }

    bool monsterMode_ = false;
    std::string error_;
};

}  // namespace

bool ParseChampionsJson(std::string_view text, std::vector<ChampionDefinition>& out, std::string* error) {
    return Loader().Run(text, out, error);
}

std::unique_ptr<ChampionDatabase> LoadChampionDatabaseFromJson(std::string_view text, std::string* error) {
    std::vector<ChampionDefinition> definitions;
    if (!ParseChampionsJson(text, definitions, error)) return nullptr;
    std::string validationError;
    auto database = ChampionDatabase::Create(std::move(definitions), &validationError);
    if (!database && error) *error = "champion data is invalid: " + validationError;
    return database;
}

bool ParseItemsJson(std::string_view text, std::vector<ItemDefinition>& out, std::string* error) {
    return Loader().RunItems(text, out, error);
}

std::unique_ptr<ItemDatabase> LoadItemDatabaseFromJson(std::string_view text, std::string* error) {
    std::vector<ItemDefinition> definitions;
    if (!ParseItemsJson(text, definitions, error)) return nullptr;
    std::string validationError;
    auto database = ItemDatabase::Create(std::move(definitions), &validationError);
    if (!database && error) *error = "item data is invalid: " + validationError;
    return database;
}

std::unique_ptr<ItemDatabase> LoadItemDatabaseFromFile(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open item data file '" + path + "'";
        return nullptr;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string inner;
    auto database = LoadItemDatabaseFromJson(contents.str(), &inner);
    if (!database && error) *error = path + ": " + inner;
    return database;
}

bool ParsePveJson(std::string_view text, PveFile& out, std::string* error) {
    return Loader().RunPve(text, out, error);
}

std::unique_ptr<EncounterDatabase> LoadEncounterDatabaseFromJson(std::string_view text, const ChampionDatabase* champions,
                                                                  const ItemDatabase* items, std::string* error) {
    PveFile file;
    if (!ParsePveJson(text, file, error)) return nullptr;
    std::string validationError;
    auto database = EncounterDatabase::Create(std::move(file.monsters), std::move(file.encounters), std::move(file.defaultDrops), champions, items, &validationError);
    if (!database && error) *error = "PvE data is invalid: " + validationError;
    return database;
}

std::unique_ptr<EncounterDatabase> LoadEncounterDatabaseFromFile(const std::string& path, const ChampionDatabase* champions,
                                                                  const ItemDatabase* items, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open PvE data file '" + path + "'";
        return nullptr;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string inner;
    auto database = LoadEncounterDatabaseFromJson(contents.str(), champions, items, &inner);
    if (!database && error) *error = path + ": " + inner;
    return database;
}

bool ParseMotherNatureJson(std::string_view text, MotherNatureFile& out, std::string* error) {
    return Loader().RunMotherNature(text, out, error);
}

std::unique_ptr<MotherNatureDatabase> LoadMotherNatureDatabaseFromJson(std::string_view text, const ItemDatabase* items, std::string* error) {
    MotherNatureFile file;
    if (!ParseMotherNatureJson(text, file, error)) return nullptr;
    std::string validationError;
    auto database = MotherNatureDatabase::Create(file.options, std::move(file.tiers), items, &validationError);
    if (!database && error) *error = "Mother Nature data is invalid: " + validationError;
    return database;
}

std::unique_ptr<MotherNatureDatabase> LoadMotherNatureDatabaseFromFile(const std::string& path, const ItemDatabase* items, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open Mother Nature data file '" + path + "'";
        return nullptr;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string inner;
    auto database = LoadMotherNatureDatabaseFromJson(contents.str(), items, &inner);
    if (!database && error) *error = path + ": " + inner;
    return database;
}

bool ParseTraitsJson(std::string_view text, std::vector<TraitDefinition>& out, std::string* error) {
    return Loader().RunTraits(text, out, error);
}

std::unique_ptr<TraitDatabase> LoadTraitDatabaseFromJson(std::string_view text, std::string* error) {
    std::vector<TraitDefinition> definitions;
    if (!ParseTraitsJson(text, definitions, error)) return nullptr;
    std::string validationError;
    auto database = TraitDatabase::Create(std::move(definitions), &validationError);
    if (!database && error) *error = "trait data is invalid: " + validationError;
    return database;
}

std::unique_ptr<TraitDatabase> LoadTraitDatabaseFromFile(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open trait data file '" + path + "'";
        return nullptr;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string inner;
    auto database = LoadTraitDatabaseFromJson(contents.str(), &inner);
    if (!database && error) *error = path + ": " + inner;
    return database;
}

std::unique_ptr<ChampionDatabase> LoadChampionDatabaseFromFile(const std::string& path, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "cannot open champion data file '" + path + "'";
        return nullptr;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    std::string inner;
    auto database = LoadChampionDatabaseFromJson(contents.str(), &inner);
    if (!database && error) *error = path + ": " + inner;
    return database;
}

}  // namespace w2f
