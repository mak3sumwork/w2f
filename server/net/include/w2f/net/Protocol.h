#pragma once

// The client -> server command format, and its validation. See docs/network-protocol.md for the full protocol.
//
// A command is one JSON object per WebSocket text message, with an "action" and that action's fields, plus an optional
// "id" (any integer 0..2^53-1) that the server echoes in its answer so a client can match answers to requests:
//
//     {"action": "buy_unit", "shop_index": 2, "id": 17}
//
// ParseCommand accepts exactly the documented shapes: unknown actions, unknown fields, missing fields, wrong types,
// non-integers and out-of-range numbers are each refused with a specific code. Nothing that fails to parse here ever
// reaches the game engine, and nothing that parses can make the engine misbehave (it still validates every action by itself).

#include <cstdint>
#include <string>
#include <string_view>

#include "w2f/Types.h"
#include "w2f/Unit.h"

namespace w2f::net {

constexpr int kProtocolVersion = 1;    // the MAJOR version: frozen. Only a breaking change would make it 2.
constexpr int kProtocolRevision = 2;   // counts the ADDITIVE changes within a major version (new fields / messages / appended enum values): see docs/UE5-Integration.md, section 13
constexpr std::size_t kMaxCommandBytes = 4096;   // a genuine command is under 200 bytes

enum class CommandType : std::uint8_t {
    BuyUnit,      // shop_index
    RerollShop,
    PickGift,     // gift_index: Mother Nature's phase only -- take one of the offered gifts, free
    BuyXp,
    SellUnit,     // unit_id
    MoveUnit,     // unit_id, location ("bench" | "board"), x, [y]
    EquipItem,    // unit_id, item_id
    UnequipItem,  // unit_id, slot
    CombineItems, // first, second: two components in the item bag become the finished item
    GetState,     // re-send the private and public state
    GetFight,     // fight_index: the combat log of one of this round's fights (any player's: fights are public)
    Ping,         // answered with "pong" even before a match exists
    GetCatalog,   // answered with "catalog" (what every champion / item / trait id means), also before a match exists
};

constexpr const char* ToString(CommandType t) {
    switch (t) {
        case CommandType::BuyUnit: return "buy_unit";
        case CommandType::RerollShop: return "reroll_shop";
        case CommandType::PickGift: return "pick_gift";
        case CommandType::BuyXp: return "buy_xp";
        case CommandType::SellUnit: return "sell_unit";
        case CommandType::MoveUnit: return "move_unit";
        case CommandType::EquipItem: return "equip_item";
        case CommandType::UnequipItem: return "unequip_item";
        case CommandType::CombineItems: return "combine_items";
        case CommandType::GetState: return "get_state";
        case CommandType::GetFight: return "get_fight";
        case CommandType::Ping: return "ping";
        case CommandType::GetCatalog: return "get_catalog";
    }
    return "?";
}

struct Command {
    CommandType type = CommandType::Ping;
    bool hasId = false;
    long long id = 0;
    int shopIndex = 0;
    int giftIndex = 0;
    UnitId unit = kInvalidUnitId;
    ItemId item = 0;
    ItemId item2 = 0;   // combine_items: the second component (`item` is the first)
    LocationType location = LocationType::None;
    int x = 0;
    int y = 0;
    int slot = 0;
    int fightIndex = 0;
};

struct ProtocolError {
    std::string code;     // stable, machine-readable: invalid_json, not_an_object, missing_action, unknown_action, missing_field,
                          //                            unknown_field, wrong_type, out_of_range, too_large
    std::string detail;   // for humans / logs
};

struct ParseResult {
    bool ok = false;
    Command command;
    ProtocolError error;
    bool hasId = false;   // set even on failure when the "id" itself was readable, so the error can be correlated
    long long id = 0;
};

ParseResult ParseCommand(std::string_view text);

}  // namespace w2f::net
