#pragma once

// Player accounts (demo 1.5): what the game client's Profile, Match history, Friends and Ranked screens show.
//
//   * a username (3-16 letters, digits, '_'; unique ignoring case) and a password (6-64 characters), kept as PBKDF2-HMAC-SHA-256 with a random salt;
//   * session tokens ("remember me": the client logs back in with one instead of the password), at most kMaxSessions per account;
//   * the RANK: ladder points from the Ranked queue (TFT's tiers Iron .. Diamond with divisions IV .. I of 100 LP, then Master, Grandmaster, Challenger);
//   * stats and the last kHistoryKept matches (mode, placement, level, round, LP change, the final board, the lobby);
//   * friends: requests one way, accepted both ways;
//   * (demo 2.0, revision 10) the progression (Progression.h): coins, account XP / level, the cosmetics bought and worn (border, title, arena), the
//     mission progress of the current day and week -- and the full record of every recent match (all eight seats), which match history rows point to.
//
// Everything lives in ONE JSON file (w2f_server --queue --accounts FILE), rewritten after every change (write to FILE.tmp, then rename: a crash
// never leaves half a file). Like the rest of the network layer it knows nothing about sockets; the caller passes the wall clock where one is needed.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "w2f/net/Progression.h"

namespace w2f::net {

constexpr int kHistoryKept = 20;
constexpr int kMatchesKept = 400;     // (revision 10) full match records kept for get_match (the oldest go first)
constexpr int kMaxSessions = 5;
constexpr int kMaxFriends = 100;

// The ladder: `points` counts LP from Iron IV 0. Iron .. Diamond: 4 divisions of 100 LP each (2800 points); Master from 2800, Grandmaster from 3000, Challenger from 3300.
struct RankInfo {
    std::string tier;   // "Iron" .. "Challenger"
    int division = 4;   // 4 (IV) .. 1 (I); 0 for Master and above
    int lp = 0;         // LP inside the division (Master+: LP above Master's floor)
    int points = 0;
};
RankInfo RankOf(int points);
int LpForPlacement(int placement);   // TFT's Ranked: 1st +40, 2nd +30, 3rd +20, 4th +10, 5th -10, 6th -20, 7th -30, 8th -40

struct HistoryUnit {
    int champion = 0;
    int star = 1;
    std::vector<int> items;
};

struct HistoryEntry {
    std::uint64_t timeMs = 0;            // when it ended (unix ms)
    std::string mode;                    // "bots" | "normal" | "ranked"
    int placement = 0, level = 0, round = 0, lpChange = 0, pointsAfter = 0;
    std::vector<HistoryUnit> board;      // the board the player last fought with
    std::vector<std::string> players;    // the lobby, 1st place first
    std::uint64_t matchId = 0;           // (revision 10) the full record (get_match); 0 = none
    int coins = 0, xp = 0;               // (revision 10) what the match paid (missions not included)
};

// (revision 10) A finished match as everyone in it saw it: every seat, 1st place first.
struct MatchRecordSeat {
    std::string name;
    bool bot = false;
    int icon = 0;
    std::string border, title;
    int placement = 0, level = 0, round = 0;
    std::vector<HistoryUnit> board;
};
struct MatchRecord {
    std::uint64_t id = 0;
    std::uint64_t timeMs = 0;
    std::string mode;
    int rounds = 0;
    std::vector<MatchRecordSeat> seats;
};

struct AccountStats {
    int games = 0, wins = 0, top4 = 0, placementSum = 0;
    int rankedGames = 0, rankedWins = 0, rankedTop4 = 0, rankedPlacementSum = 0;
};

struct Account {
    std::string name;                    // as registered (shown to everyone)
    std::string salt, hash;              // hex
    int iterations = 0;
    std::uint64_t createdMs = 0;
    int points = 0, peakPoints = 0;
    int icon = 0;                        // (revision 9) the profile icon: a champion id, 0 = none chosen (the client shows a default)
    AccountStats stats;
    std::vector<HistoryEntry> history;   // newest first
    std::set<std::string> friends, incoming, outgoing;   // account KEYS (lower case)
    std::vector<std::string> sessions;   // newest last
    // (revision 10) progression: what Progression.h describes
    int coins = 0, xp = 0;               // xp = the account XP in total (the level is computed from it)
    std::set<std::string> owned;         // store items bought (the free defaults are owned by everyone and not listed)
    std::string border, title, arena;    // the equipped cosmetics ("" = the kind's default)
    long long missionDay = -1, missionWeek = -1;   // the day / week the progress below belongs to
    std::map<std::string, int> missionProgress;    // mission id -> qualifying matches so far
    std::set<std::string> missionsDone;            // rewarded already
};

class AccountStore {
public:
    enum class Result { Ok, InvalidName, InvalidPassword, NameTaken, BadCredentials, BadSession, UnknownUser, Self, AlreadyFriends, AlreadyRequested, NoRequest, NotFriends, TooManyFriends,
                        UnknownItem, AlreadyOwned, NotEnoughCoins, NotOwned, UnknownMatch };   // the last five: revision 10
    static const char* Code(Result r);     // the protocol's error code ("name_taken" ...)
    static const char* Describe(Result r); // a sentence for people

    // `path` empty = memory only (tests). `entropy` gives salts and session tokens. `iterations`: PBKDF2 rounds (tests use few).
    AccountStore(std::string path, std::function<std::uint64_t()> entropy, int iterations = 20000);

    bool Load(std::string* error);         // a missing file is an empty store
    bool Save(std::string* error) const;   // done automatically after every change; exposed for tests

    static std::string Key(std::string_view name);   // lower case
    static bool ValidName(std::string_view name);
    static bool ValidPassword(std::string_view password);

    Result Register(std::string_view name, std::string_view password, std::uint64_t nowMs, std::string& keyOut, std::string& sessionOut);
    Result Login(std::string_view name, std::string_view password, std::string& keyOut, std::string& sessionOut);
    Result Resume(std::string_view session, std::string& keyOut) const;
    void Logout(std::string_view key, std::string_view session);
    void SetIcon(std::string_view key, int icon);   // (revision 9) the caller checks that `icon` is a champion id
    // (revision 10) A new password: the old one must match; every other session is forgotten (the one in `keepSession` stays).
    Result ChangePassword(std::string_view key, std::string_view oldPassword, std::string_view newPassword, std::string_view keepSession);

    // ---- (revision 10) progression; `progression` is the loaded data/progression.json ----
    // A new account's coins (Register gives them when a progression is attached).
    void AttachProgression(const Progression* progression) { progression_ = progression; }
    const Progression* progression() const { return progression_; }
    bool Owns(const Account& account, const StoreItem& item) const { return item.isDefault || account.owned.count(item.id) != 0; }
    Result Buy(std::string_view key, std::string_view itemId);
    Result Equip(std::string_view key, std::string_view itemId);
    // The equipped item of a kind (the default when nothing is): "border_bronze" ...
    std::string Equipped(const Account& account, std::string_view kind) const;
    // Coins and XP in; returns the levels gained (each pays level_up_coins, added here too).
    int Grant(std::string_view key, int coins, int xp);
    // Moves the account's missions to the day / week of `nowMs` (a new period starts at 0).
    void RefreshMissions(std::string_view key, std::uint64_t nowMs);
    struct MissionUpdate { const MissionDef* mission = nullptr; int progress = 0; bool done = false, justDone = false; };
    // A finished match counts toward the current missions; finished ones pay at once. Returns every current mission with its new progress.
    std::vector<MissionUpdate> CountMissions(std::string_view key, std::uint64_t nowMs, const MissionMatch& match);
    std::vector<MissionUpdate> Missions(std::string_view key, std::uint64_t nowMs);
    int LevelOf(const Account& account) const;

    // (revision 10) Full match records: AddMatch gives the record its id.
    std::uint64_t AddMatch(MatchRecord record);
    const MatchRecord* FindMatch(std::uint64_t id) const;
    // The Ranked leaderboard: everyone who played Ranked, most points first (then name).
    std::vector<const Account*> Ladder() const;

    const Account* Find(std::string_view nameOrKey) const;

    // Friends: Request sends a request (if they already asked you, you become friends at once: `becameFriends`).
    Result Request(std::string_view key, std::string_view otherName, bool& becameFriends);
    Result Accept(std::string_view key, std::string_view otherName);
    Result Decline(std::string_view key, std::string_view otherName);   // an incoming request, or cancels your own outgoing one
    Result Unfriend(std::string_view key, std::string_view otherName);

    // A finished match: stats, history, and (ranked) the LP. `entry.lpChange` / `pointsAfter` are filled in here.
    void RecordMatch(std::string_view key, HistoryEntry entry);

    int size() const { return static_cast<int>(accounts_.size()); }

private:
    Account* Mutable(std::string_view nameOrKey);
    std::string NewHex(int bytes);
    std::string Hash(std::string_view password, std::string_view saltHex, int iterations) const;
    void Changed();

    std::string path_;
    std::function<std::uint64_t()> entropy_;
    int iterations_;
    std::map<std::string, Account> accounts_;   // by key; ordered: the file is written in a stable order
    const Progression* progression_ = nullptr;
    std::map<std::uint64_t, MatchRecord> matches_;   // (revision 10) by id; the oldest are dropped past kMatchesKept
    std::uint64_t nextMatchId_ = 1;
};

}  // namespace w2f::net
