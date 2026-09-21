#pragma once

// Fundamental IDs, constants and result codes shared by every server system.
// Nothing in this codebase may depend on rendering / engine types (no AActor etc).

#include <cstddef>
#include <cstdint>

namespace w2f {

using PlayerId = std::uint8_t;
using ChampionId = std::uint32_t;
using ItemId = std::uint32_t;  // 0 = no item

constexpr PlayerId kInvalidPlayerId = 0xFF;
constexpr ChampionId kInvalidChampionId = 0;  // Champion ids in data must be non-zero.

constexpr int kMaxPlayers = 8;
constexpr int kMaxCostTier = 5;      // Champion cost / pool tier: 1..kMaxCostTier
constexpr int kMaxPlayerLevel = 10;  // Player level: 1..kMaxPlayerLevel
constexpr int kTicksPerSecond = 30;  // Fixed simulation rate. The server never reads wall-clock time.

// Per-player unit storage (see UnitRoster).
constexpr int kBenchSlots = 9;
constexpr int kBoardRows = 4;
constexpr int kBoardColumns = 7;
constexpr int kMaxStarLevel = 3;
constexpr int kMaxItemsPerUnit = 3;

constexpr int Seconds(int s) { return s * kTicksPerSecond; }

// Every player-initiated action reports exactly why it succeeded or failed,
// so the network layer can relay a precise rejection reason to the client.
enum class ActionResult : std::uint8_t {
    Ok,
    WrongPhase,
    InvalidPlayer,
    PlayerEliminated,
    NotEnoughGold,
    InvalidSlot,
    EmptySlot,
    RosterFull,
    BoardFull,
    MaxLevel,
    InvalidUnit,
    ItemsFull,     // the unit already carries kMaxItemsPerUnit items
    InvalidItem,   // unknown item id, or the player does not have it in their item bag
    ShopClosed,    // the opening round or a Mother Nature round: there is no shop (buy / reroll)
    AlreadyPicked, // the player already took their Mother Nature gift this round
    NoItemsToRemove, // an Item Remover was used on a unit that carries no items (the remover is kept)
    UnitInCombat,    // Combat / Resolution: the board is locked (a unit on it cannot be sold, moved or equipped, and a purchase may not merge into it)
};

constexpr const char* ToString(ActionResult r) {
    switch (r) {
        case ActionResult::Ok: return "Ok";
        case ActionResult::WrongPhase: return "WrongPhase";
        case ActionResult::InvalidPlayer: return "InvalidPlayer";
        case ActionResult::PlayerEliminated: return "PlayerEliminated";
        case ActionResult::NotEnoughGold: return "NotEnoughGold";
        case ActionResult::InvalidSlot: return "InvalidSlot";
        case ActionResult::EmptySlot: return "EmptySlot";
        case ActionResult::RosterFull: return "RosterFull";
        case ActionResult::BoardFull: return "BoardFull";
        case ActionResult::MaxLevel: return "MaxLevel";
        case ActionResult::InvalidUnit: return "InvalidUnit";
        case ActionResult::ItemsFull: return "ItemsFull";
        case ActionResult::InvalidItem: return "InvalidItem";
        case ActionResult::ShopClosed: return "ShopClosed";
        case ActionResult::AlreadyPicked: return "AlreadyPicked";
        case ActionResult::NoItemsToRemove: return "NoItemsToRemove";
        case ActionResult::UnitInCombat: return "UnitInCombat";
    }
    return "Unknown";
}

enum class RoundResult : std::uint8_t { Win, Loss, Draw };

}  // namespace w2f
