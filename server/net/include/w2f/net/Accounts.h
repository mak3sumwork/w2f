#pragma once

// Player accounts (demo 1.5): what the game client's Profile, Match history, Friends and Ranked screens show.
//
//   * a username (3-16 letters, digits, '_'; unique ignoring case) and a password (6-64 characters), kept as PBKDF2-HMAC-SHA-256 with a random salt;
//   * session tokens ("remember me": the client logs back in with one instead of the password), at most kMaxSessions per account;
//   * the RANK: ladder points from the Ranked queue (TFT's tiers Iron .. Diamond with divisions IV .. I of 100 LP, then Master, Grandmaster, Challenger);
//   * stats and the last kHistoryKept matches (mode, placement, level, round, LP change, the final board, the lobby);
//   * friends: requests one way, accepted both ways.
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

namespace w2f::net {

constexpr int kHistoryKept = 20;
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
    AccountStats stats;
    std::vector<HistoryEntry> history;   // newest first
    std::set<std::string> friends, incoming, outgoing;   // account KEYS (lower case)
    std::vector<std::string> sessions;   // newest last
};

class AccountStore {
public:
    enum class Result { Ok, InvalidName, InvalidPassword, NameTaken, BadCredentials, BadSession, UnknownUser, Self, AlreadyFriends, AlreadyRequested, NoRequest, NotFriends, TooManyFriends };
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
};

}  // namespace w2f::net
