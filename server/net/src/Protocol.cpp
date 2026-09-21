#include "w2f/net/Protocol.h"

#include <vector>

#include "w2f/Json.h"

namespace w2f::net {

namespace {

constexpr long long kMaxId = 9007199254740991LL;   // 2^53 - 1: what a JSON number in a JavaScript / Lua client can hold exactly
constexpr int kMaxShopIndex = 63;
constexpr int kMaxGiftIndex = 3;   // Mother Nature offers 1..4 gifts

struct Reader {
    const json::Value& object;
    ParseResult& result;

    bool Fail(const char* code, const std::string& detail) {
        result.ok = false;
        result.error = {code, detail};
        return false;
    }

    const json::Value* Take(const char* key) { return object.Find(key); }
    bool Int(const char* key, long long lo, long long hi, bool required, long long& out, bool& present) {
        const json::Value* v = Take(key);
        present = v != nullptr;
        if (v == nullptr) return required ? Fail("missing_field", std::string("\"") + key + "\" is required") : true;
        if (!v->IsNumber()) return Fail("wrong_type", std::string("\"") + key + "\" must be a number");
        long long n = 0;
        if (!v->ToInt(n)) return Fail("wrong_type", std::string("\"") + key + "\" must be a whole number");
        if (n < lo || n > hi) return Fail("out_of_range", std::string("\"") + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi));
        out = n;
        return true;
    }
    bool Required(const char* key, long long lo, long long hi, long long& out) {
        bool present = false;
        return Int(key, lo, hi, true, out, present);
    }
};

}  // namespace

ParseResult ParseCommand(std::string_view text) {
    ParseResult r;
    auto fail = [&r](const char* code, const std::string& detail) {
        r.ok = false;
        r.error = {code, detail};
        return r;
    };
    if (text.size() > kMaxCommandBytes) return fail("too_large", "a command is at most " + std::to_string(kMaxCommandBytes) + " bytes");
    json::Value root;
    std::string parseError;
    if (!json::Parse(text, root, &parseError)) return fail("invalid_json", parseError);
    if (!root.IsObject()) return fail("not_an_object", "a command is a JSON object");

    Reader in{root, r};

    // "id" first, so that even a bad command's error can carry it.
    if (const json::Value* id = in.Take("id")) {
        long long n = 0;
        if (!id->IsNumber() || !id->ToInt(n) || n < 0 || n > kMaxId) return fail("wrong_type", "\"id\" must be a whole number between 0 and 2^53-1");
        r.hasId = true;
        r.id = n;
        r.command.hasId = true;
        r.command.id = n;
    }

    const json::Value* action = in.Take("action");
    if (action == nullptr) return fail("missing_action", "\"action\" is required");
    if (!action->IsString()) return fail("wrong_type", "\"action\" must be a string");
    const std::string& name = action->AsString();

    Command& c = r.command;
    long long v = 0;
    bool present = false;
    struct Known { const char* name; CommandType type; };
    static const Known kKnown[] = {{"buy_unit", CommandType::BuyUnit}, {"reroll_shop", CommandType::RerollShop}, {"pick_gift", CommandType::PickGift}, {"buy_xp", CommandType::BuyXp},
                                   {"sell_unit", CommandType::SellUnit}, {"move_unit", CommandType::MoveUnit}, {"equip_item", CommandType::EquipItem},
                                   {"unequip_item", CommandType::UnequipItem}, {"combine_items", CommandType::CombineItems}, {"get_state", CommandType::GetState}, {"get_fight", CommandType::GetFight},
                                   {"ping", CommandType::Ping}, {"get_catalog", CommandType::GetCatalog}};
    bool found = false;
    for (const Known& k : kKnown) {
        if (name == k.name) { c.type = k.type; found = true; }
    }
    if (!found) return fail("unknown_action", "unknown action \"" + (name.size() > 40 ? name.substr(0, 40) + "..." : name) + "\"");

    // Exactly the documented fields, nothing else: a typo such as "shop_idx" is an error, not a silently missing value.
    std::vector<const char*> allowed;   // a vector, NOT an initializer_list: that would dangle after its assignment
    switch (c.type) {
        case CommandType::BuyUnit: allowed = {"id", "action", "shop_index"}; break;
        case CommandType::PickGift: allowed = {"id", "action", "gift_index"}; break;
        case CommandType::SellUnit: allowed = {"id", "action", "unit_id"}; break;
        case CommandType::MoveUnit: allowed = {"id", "action", "unit_id", "location", "x", "y"}; break;
        case CommandType::EquipItem: allowed = {"id", "action", "unit_id", "item_id"}; break;
        case CommandType::UnequipItem: allowed = {"id", "action", "unit_id", "slot"}; break;
        case CommandType::CombineItems: allowed = {"id", "action", "first", "second"}; break;
        case CommandType::GetFight: allowed = {"id", "action", "fight_index"}; break;
        case CommandType::RerollShop:
        case CommandType::BuyXp:
        case CommandType::GetState:
        case CommandType::GetCatalog:
        case CommandType::Ping: allowed = {"id", "action"}; break;
    }
    for (std::size_t i = 0; i < root.MemberCount(); ++i) {
        const std::string& key = root.MemberKey(i);
        bool ok = false;
        for (const char* a : allowed) ok = ok || key == a;
        if (!ok) return fail("unknown_field", "\"" + (key.size() > 40 ? key.substr(0, 40) + "..." : key) + "\" is not a field of " + ToString(c.type));
    }

    bool ok = true;
    switch (c.type) {
        case CommandType::BuyUnit:
            ok = in.Required("shop_index", 0, kMaxShopIndex, v);
            c.shopIndex = static_cast<int>(v);
            break;
        case CommandType::PickGift:
            ok = in.Required("gift_index", 0, kMaxGiftIndex, v);
            c.giftIndex = static_cast<int>(v);
            break;
        case CommandType::SellUnit:
            ok = in.Required("unit_id", 1, 4294967295LL, v);
            c.unit = static_cast<UnitId>(v);
            break;
        case CommandType::MoveUnit: {
            ok = in.Required("unit_id", 1, 4294967295LL, v);
            c.unit = static_cast<UnitId>(v);
            if (!ok) break;
            const json::Value* loc = in.Take("location");
            if (loc == nullptr) { ok = in.Fail("missing_field", "\"location\" is required"); break; }
            if (!loc->IsString()) { ok = in.Fail("wrong_type", "\"location\" must be \"bench\" or \"board\""); break; }
            if (loc->AsString() == "bench") c.location = LocationType::Bench;
            else if (loc->AsString() == "board") c.location = LocationType::Board;
            else { ok = in.Fail("out_of_range", "\"location\" must be \"bench\" or \"board\""); break; }
            const bool bench = c.location == LocationType::Bench;
            ok = in.Required("x", 0, bench ? kBenchSlots - 1 : kBoardColumns - 1, v);
            c.x = static_cast<int>(v);
            if (!ok) break;
            if (bench) {
                // The bench has a single row; "y" may be given (as 0) for symmetry with the board.
                ok = in.Int("y", 0, 0, false, v, present);
                c.y = 0;
            } else {
                ok = in.Required("y", 0, kBoardRows - 1, v);
                c.y = static_cast<int>(v);
            }
            break;
        }
        case CommandType::EquipItem:
            ok = in.Required("unit_id", 1, 4294967295LL, v);
            c.unit = static_cast<UnitId>(v);
            if (!ok) break;
            ok = in.Required("item_id", 1, 4294967295LL, v);
            c.item = static_cast<ItemId>(v);
            break;
        case CommandType::UnequipItem:
            ok = in.Required("unit_id", 1, 4294967295LL, v);
            c.unit = static_cast<UnitId>(v);
            if (!ok) break;
            ok = in.Required("slot", 0, kMaxItemsPerUnit - 1, v);
            c.slot = static_cast<int>(v);
            break;
        case CommandType::CombineItems:
            ok = in.Required("first", 1, 4294967295LL, v);
            c.item = static_cast<ItemId>(v);
            if (!ok) break;
            ok = in.Required("second", 1, 4294967295LL, v);
            c.item2 = static_cast<ItemId>(v);
            break;
        case CommandType::GetFight:
            ok = in.Required("fight_index", 0, kMaxPlayers - 1, v);
            c.fightIndex = static_cast<int>(v);
            break;
        case CommandType::RerollShop:
        case CommandType::BuyXp:
        case CommandType::GetState:
        case CommandType::GetCatalog:
        case CommandType::Ping:
            break;
    }
    if (!ok) return r;   // the reader already recorded why

    r.ok = true;
    return r;
}

}  // namespace w2f::net
