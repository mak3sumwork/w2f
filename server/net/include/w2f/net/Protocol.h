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
constexpr int kProtocolRevision = 10;   // counts the ADDITIVE changes within a major version (new fields / messages / appended enum values): see docs/UE5-Integration.md, section 13
constexpr std::size_t kMaxCommandBytes = 4096;   // a genuine command is under 200 bytes

enum class CommandType : std::uint8_t {
    BuyUnit,      // shop_index
    RerollShop,
    PickGift,     // gift_index: Mother Nature's phase only -- take one of the offered gifts, free
    BuyXp,
    SetShopLock,  // locked (bool): a locked shop keeps its offer through the next refresh
    SellUnit,     // unit_id
    MoveUnit,     // unit_id, location ("bench" | "board"), x, [y]
    EquipItem,    // unit_id, item_id
    UnequipItem,  // unit_id, slot
    CombineItems, // first, second: two components in the item bag become the finished item
    GetState,     // re-send the private and public state
    GetFight,     // fight_index: the combat log of one of this round's fights (any player's: fights are public)
    Ping,         // answered with "pong" even before a match exists
    GetCatalog,   // answered with "catalog" (what every champion / item / trait id means), also before a match exists
    PickTraitChoice,  // index (revision 5): answer the pending trait choice (a Hexagon module, a Najmi prototype); -1 declines a prototype
    // Revision 7: matchmaking. Only a queue server (w2f_server --queue, QueueServer) acts on these; a single-lobby server answers `error` "no_queue".
    JoinQueue,    // mode ("bots" | "normal"): look for a match
    LeaveQueue,   // stop looking
    LeaveMatch,   // go back to the client from a match (the seat stays reserved for its token, the match goes on without you)
    // Revision 8 (demo 1.5): accounts, profile, friends. Only a queue server with accounts (w2f_server --queue --accounts FILE) acts on these.
    Register,       // username, password: create an account and log in
    Login,          // username, password
    ResumeSession,  // session: log in with a remembered session token
    Logout,
    GetProfile,     // [username]: a player's profile (rank, stats, match history); yours without a name
    GetFriends,
    FriendRequest,  // username
    FriendAccept,   // username
    FriendDecline,  // username: refuse an incoming request, or cancel your own
    FriendRemove,   // username
    // Revision 9 (demo 1.9): the ready check and profile icons (a queue server; set_icon needs accounts).
    AcceptMatch,    // answer `match_found` (ready_check true): ready
    DeclineMatch,   // answer it: not now (you leave the queue; the others go back into it)
    SetIcon,        // icon: a champion id, your profile icon
    // Revision 10 (demo 2.0, the League-style client): parties, chat, match details, the leaderboard, progression (store, missions), password change.
    PartyInvite,    // username: invite a friend into your party (makes one with you as the leader)
    PartyAccept,    // username: join the party of the friend who invited you
    PartyDecline,   // username: refuse their invite
    PartyLeave,
    PartyKick,      // username (leader only)
    PartyPromote,   // username (leader only): make them the leader
    PartyMode,      // mode (leader only): the queue the party's lobby shows ("bots" | "normal" | "ranked")
    Chat,           // to ("party" or a friend's username), text
    GetChat,        // username: the recent conversation with a friend
    GetMatch,       // match_id: every seat of a finished match (match history, the post-game screen)
    GetLadder,      // the Ranked leaderboard
    GetStore,       // the cosmetics store and what you own
    BuyItem,        // item: buy a store item with coins
    EquipCosmetic,  // item: wear an owned store item (a kind's default takes the other off)
    GetMissions,    // the daily and weekly missions
    ChangePassword, // password, new_password
};

enum class QueueMode : std::uint8_t { Bots, Normal, Ranked };   // Ranked: revision 8
constexpr const char* ToString(QueueMode m) { return m == QueueMode::Bots ? "bots" : (m == QueueMode::Normal ? "normal" : "ranked"); }

constexpr const char* ToString(CommandType t) {
    switch (t) {
        case CommandType::BuyUnit: return "buy_unit";
        case CommandType::RerollShop: return "reroll_shop";
        case CommandType::PickGift: return "pick_gift";
        case CommandType::BuyXp: return "buy_xp";
        case CommandType::SetShopLock: return "set_shop_lock";
        case CommandType::SellUnit: return "sell_unit";
        case CommandType::MoveUnit: return "move_unit";
        case CommandType::EquipItem: return "equip_item";
        case CommandType::UnequipItem: return "unequip_item";
        case CommandType::CombineItems: return "combine_items";
        case CommandType::GetState: return "get_state";
        case CommandType::GetFight: return "get_fight";
        case CommandType::Ping: return "ping";
        case CommandType::GetCatalog: return "get_catalog";
        case CommandType::PickTraitChoice: return "pick_trait_choice";
        case CommandType::JoinQueue: return "queue";
        case CommandType::LeaveQueue: return "leave_queue";
        case CommandType::LeaveMatch: return "leave_match";
        case CommandType::Register: return "register";
        case CommandType::Login: return "login";
        case CommandType::ResumeSession: return "resume_session";
        case CommandType::Logout: return "logout";
        case CommandType::GetProfile: return "get_profile";
        case CommandType::GetFriends: return "get_friends";
        case CommandType::FriendRequest: return "friend_request";
        case CommandType::FriendAccept: return "friend_accept";
        case CommandType::FriendDecline: return "friend_decline";
        case CommandType::FriendRemove: return "friend_remove";
        case CommandType::AcceptMatch: return "accept_match";
        case CommandType::DeclineMatch: return "decline_match";
        case CommandType::SetIcon: return "set_icon";
        case CommandType::PartyInvite: return "party_invite";
        case CommandType::PartyAccept: return "party_accept";
        case CommandType::PartyDecline: return "party_decline";
        case CommandType::PartyLeave: return "party_leave";
        case CommandType::PartyKick: return "party_kick";
        case CommandType::PartyPromote: return "party_promote";
        case CommandType::PartyMode: return "party_mode";
        case CommandType::Chat: return "chat";
        case CommandType::GetChat: return "get_chat";
        case CommandType::GetMatch: return "get_match";
        case CommandType::GetLadder: return "get_ladder";
        case CommandType::GetStore: return "get_store";
        case CommandType::BuyItem: return "buy_item";
        case CommandType::EquipCosmetic: return "equip_cosmetic";
        case CommandType::GetMissions: return "get_missions";
        case CommandType::ChangePassword: return "change_password";
    }
    return "?";
}

// Revision 8: the account / profile / friends commands (a QueueServer with accounts answers them; anything else says "no_accounts").
// Revision 10 adds the party, chat, match detail, leaderboard, store, missions and password commands (they need an account too).
constexpr bool IsAccountCommand(CommandType t) {
    return t == CommandType::Register || t == CommandType::Login || t == CommandType::ResumeSession || t == CommandType::Logout || t == CommandType::GetProfile ||
           t == CommandType::GetFriends || t == CommandType::FriendRequest || t == CommandType::FriendAccept || t == CommandType::FriendDecline || t == CommandType::FriendRemove || t == CommandType::SetIcon ||
           (t >= CommandType::PartyInvite && t <= CommandType::ChangePassword);
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
    bool locked = false;   // set_shop_lock
    int choiceIndex = 0;   // pick_trait_choice: -1..7
    QueueMode queueMode = QueueMode::Bots;   // queue
    bool readyCheck = false;                 // queue (revision 9): ask for a ready check (match_found -> accept_match / decline_match) before the match starts
    int icon = 0;                            // set_icon
    std::string username, password, session; // register / login / resume_session / get_profile / friend_* / party_* / get_chat / change_password
    std::string to, text;                    // chat (revision 10)
    std::string storeItem;                   // buy_item / equip_cosmetic (named so: `item` is equip_item's)
    std::string newPassword;                 // change_password
    std::uint64_t matchId = 0;               // get_match
};

constexpr std::size_t kMaxChatBytes = 300;   // (revision 10) a chat line

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
