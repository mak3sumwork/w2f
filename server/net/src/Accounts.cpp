#include "w2f/net/Accounts.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "w2f/Json.h"
#include "w2f/net/Encoding.h"
#include "w2f/net/JsonWriter.h"

namespace w2f::net {

namespace {

constexpr int kDivisionPoints = 100;
constexpr int kMasterPoints = 2800;
constexpr int kGrandmasterPoints = 3000;
constexpr int kChallengerPoints = 3300;
const char* const kTiers[] = {"Iron", "Bronze", "Silver", "Gold", "Platinum", "Emerald", "Diamond"};

std::string FromHex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto nib = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
        out.push_back(static_cast<char>(nib(hex[i]) * 16 + nib(hex[i + 1])));
    }
    return out;
}

int IntOf(const json::Value* v, int fallback = 0) {
    long long n = 0;
    return v != nullptr && v->IsNumber() && v->ToInt(n) ? static_cast<int>(n) : fallback;
}
std::uint64_t U64Of(const json::Value* v) {
    long long n = 0;
    return v != nullptr && v->IsNumber() && v->ToInt(n) && n > 0 ? static_cast<std::uint64_t>(n) : 0;
}
std::string StrOf(const json::Value* v) { return v != nullptr && v->IsString() ? v->AsString() : std::string(); }

}  // namespace

RankInfo RankOf(int points) {
    RankInfo r;
    r.points = std::max(0, points);
    if (r.points >= kChallengerPoints) { r.tier = "Challenger"; r.division = 0; r.lp = r.points - kMasterPoints; return r; }
    if (r.points >= kGrandmasterPoints) { r.tier = "Grandmaster"; r.division = 0; r.lp = r.points - kMasterPoints; return r; }
    if (r.points >= kMasterPoints) { r.tier = "Master"; r.division = 0; r.lp = r.points - kMasterPoints; return r; }
    const int step = r.points / kDivisionPoints;   // 0 .. 27
    r.tier = kTiers[step / 4];
    r.division = 4 - step % 4;
    r.lp = r.points % kDivisionPoints;
    return r;
}

int LpForPlacement(int placement) {
    static const int kLp[9] = {0, 40, 30, 20, 10, -10, -20, -30, -40};
    return placement >= 1 && placement <= 8 ? kLp[placement] : 0;
}

const char* AccountStore::Code(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::InvalidName: return "invalid_name";
        case Result::InvalidPassword: return "invalid_password";
        case Result::NameTaken: return "name_taken";
        case Result::BadCredentials: return "bad_credentials";
        case Result::BadSession: return "bad_session";
        case Result::UnknownUser: return "unknown_user";
        case Result::Self: return "self";
        case Result::AlreadyFriends: return "already_friends";
        case Result::AlreadyRequested: return "already_requested";
        case Result::NoRequest: return "no_request";
        case Result::NotFriends: return "not_friends";
        case Result::TooManyFriends: return "too_many_friends";
    }
    return "?";
}

const char* AccountStore::Describe(Result r) {
    switch (r) {
        case Result::Ok: return "Done.";
        case Result::InvalidName: return "A username is 3-16 letters, digits or '_'.";
        case Result::InvalidPassword: return "A password is 6-64 characters.";
        case Result::NameTaken: return "That username is taken.";
        case Result::BadCredentials: return "Wrong username or password.";
        case Result::BadSession: return "Your session has expired: log in again.";
        case Result::UnknownUser: return "There is no player with that name.";
        case Result::Self: return "That is you.";
        case Result::AlreadyFriends: return "You are already friends.";
        case Result::AlreadyRequested: return "You already sent them a request.";
        case Result::NoRequest: return "There is no such request.";
        case Result::NotFriends: return "You are not friends.";
        case Result::TooManyFriends: return "That friend list is full.";
    }
    return "";
}

AccountStore::AccountStore(std::string path, std::function<std::uint64_t()> entropy, int iterations)
    : path_(std::move(path)), entropy_(std::move(entropy)), iterations_(std::max(1, iterations)) {}

std::string AccountStore::Key(std::string_view name) {
    std::string k(name);
    for (char& c : k) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return k;
}

bool AccountStore::ValidName(std::string_view name) {
    if (name.size() < 3 || name.size() > 16) return false;
    for (char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool AccountStore::ValidPassword(std::string_view password) { return password.size() >= 6 && password.size() <= 64; }

std::string AccountStore::NewHex(int bytes) {
    std::string raw;
    while (static_cast<int>(raw.size()) < bytes) {
        const std::uint64_t v = entropy_();
        for (int j = 0; j < 8 && static_cast<int>(raw.size()) < bytes; ++j) raw.push_back(static_cast<char>((v >> (8 * j)) & 0xFFu));
    }
    return HexEncode(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
}

std::string AccountStore::Hash(std::string_view password, std::string_view saltHex, int iterations) const {
    const std::string dk = Pbkdf2Sha256(password, FromHex(saltHex), iterations, 32);
    return HexEncode(reinterpret_cast<const std::uint8_t*>(dk.data()), dk.size());
}

Account* AccountStore::Mutable(std::string_view nameOrKey) {
    auto it = accounts_.find(Key(nameOrKey));
    return it == accounts_.end() ? nullptr : &it->second;
}

const Account* AccountStore::Find(std::string_view nameOrKey) const {
    auto it = accounts_.find(Key(nameOrKey));
    return it == accounts_.end() ? nullptr : &it->second;
}

AccountStore::Result AccountStore::Register(std::string_view name, std::string_view password, std::uint64_t nowMs, std::string& keyOut, std::string& sessionOut) {
    if (!ValidName(name)) return Result::InvalidName;
    if (!ValidPassword(password)) return Result::InvalidPassword;
    const std::string key = Key(name);
    if (accounts_.count(key) != 0) return Result::NameTaken;
    Account a;
    a.name = std::string(name);
    a.salt = NewHex(16);
    a.iterations = iterations_;
    a.hash = Hash(password, a.salt, a.iterations);
    a.createdMs = nowMs;
    sessionOut = NewHex(16);
    a.sessions.push_back(sessionOut);
    accounts_[key] = std::move(a);
    keyOut = key;
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Login(std::string_view name, std::string_view password, std::string& keyOut, std::string& sessionOut) {
    Account* a = Mutable(name);
    if (a == nullptr || !ValidPassword(password) || !ConstantTimeEquals(Hash(password, a->salt, a->iterations), a->hash)) return Result::BadCredentials;
    sessionOut = NewHex(16);
    a->sessions.push_back(sessionOut);
    while (static_cast<int>(a->sessions.size()) > kMaxSessions) a->sessions.erase(a->sessions.begin());   // the oldest devices are forgotten
    keyOut = Key(name);
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Resume(std::string_view session, std::string& keyOut) const {
    if (session.size() != 32 || !IsLowerHex(session)) return Result::BadSession;
    for (const auto& [key, a] : accounts_) {
        for (const std::string& s : a.sessions) {
            if (ConstantTimeEquals(s, session)) { keyOut = key; return Result::Ok; }
        }
    }
    return Result::BadSession;
}

void AccountStore::Logout(std::string_view key, std::string_view session) {
    Account* a = Mutable(key);
    if (a == nullptr) return;
    a->sessions.erase(std::remove(a->sessions.begin(), a->sessions.end(), std::string(session)), a->sessions.end());
    Changed();
}

void AccountStore::SetIcon(std::string_view key, int icon) {
    Account* a = Mutable(key);
    if (a == nullptr || a->icon == icon) return;
    a->icon = icon;
    Changed();
}

AccountStore::Result AccountStore::Request(std::string_view key, std::string_view otherName, bool& becameFriends) {
    becameFriends = false;
    Account* me = Mutable(key);
    Account* other = Mutable(otherName);
    if (me == nullptr || other == nullptr) return Result::UnknownUser;
    const std::string myKey = Key(key), otherKey = Key(otherName);
    if (myKey == otherKey) return Result::Self;
    if (me->friends.count(otherKey) != 0) return Result::AlreadyFriends;
    if (me->outgoing.count(otherKey) != 0) return Result::AlreadyRequested;
    if (static_cast<int>(me->friends.size()) >= kMaxFriends || static_cast<int>(other->friends.size()) >= kMaxFriends) return Result::TooManyFriends;
    if (me->incoming.count(otherKey) != 0) {   // they asked first: that is a yes
        me->incoming.erase(otherKey);
        other->outgoing.erase(myKey);
        me->friends.insert(otherKey);
        other->friends.insert(myKey);
        becameFriends = true;
    } else {
        me->outgoing.insert(otherKey);
        other->incoming.insert(myKey);
    }
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Accept(std::string_view key, std::string_view otherName) {
    Account* me = Mutable(key);
    Account* other = Mutable(otherName);
    if (me == nullptr || other == nullptr) return Result::UnknownUser;
    const std::string myKey = Key(key), otherKey = Key(otherName);
    if (me->incoming.count(otherKey) == 0) return Result::NoRequest;
    if (static_cast<int>(me->friends.size()) >= kMaxFriends || static_cast<int>(other->friends.size()) >= kMaxFriends) return Result::TooManyFriends;
    me->incoming.erase(otherKey);
    other->outgoing.erase(myKey);
    me->friends.insert(otherKey);
    other->friends.insert(myKey);
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Decline(std::string_view key, std::string_view otherName) {
    Account* me = Mutable(key);
    Account* other = Mutable(otherName);
    if (me == nullptr || other == nullptr) return Result::UnknownUser;
    const std::string myKey = Key(key), otherKey = Key(otherName);
    const bool incoming = me->incoming.erase(otherKey) != 0;
    const bool outgoing = me->outgoing.erase(otherKey) != 0;
    if (!incoming && !outgoing) return Result::NoRequest;
    other->outgoing.erase(myKey);
    other->incoming.erase(myKey);
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Unfriend(std::string_view key, std::string_view otherName) {
    Account* me = Mutable(key);
    Account* other = Mutable(otherName);
    if (me == nullptr || other == nullptr) return Result::UnknownUser;
    const std::string myKey = Key(key), otherKey = Key(otherName);
    if (me->friends.erase(otherKey) == 0) return Result::NotFriends;
    other->friends.erase(myKey);
    Changed();
    return Result::Ok;
}

void AccountStore::RecordMatch(std::string_view key, HistoryEntry entry) {
    Account* a = Mutable(key);
    if (a == nullptr) return;
    const int p = entry.placement;
    AccountStats& s = a->stats;
    ++s.games;
    s.wins += p == 1 ? 1 : 0;
    s.top4 += p >= 1 && p <= 4 ? 1 : 0;
    s.placementSum += p;
    entry.lpChange = 0;
    if (entry.mode == "ranked") {
        ++s.rankedGames;
        s.rankedWins += p == 1 ? 1 : 0;
        s.rankedTop4 += p >= 1 && p <= 4 ? 1 : 0;
        s.rankedPlacementSum += p;
        const int before = a->points;
        a->points = std::max(0, a->points + LpForPlacement(p));   // the floor is Iron IV 0 LP
        a->peakPoints = std::max(a->peakPoints, a->points);
        entry.lpChange = a->points - before;
    }
    entry.pointsAfter = a->points;
    a->history.insert(a->history.begin(), std::move(entry));
    if (static_cast<int>(a->history.size()) > kHistoryKept) a->history.resize(kHistoryKept);
    Changed();
}

void AccountStore::Changed() {
    std::string error;
    Save(&error);   // a failed write is reported by w2f_server at start (it saves once there); the data stays in memory
}

bool AccountStore::Save(std::string* error) const {
    if (path_.empty()) return true;
    JsonWriter w;
    w.BeginObject();
    w.Field("format", 1);
    w.Key("accounts").BeginArray();
    for (const auto& [key, a] : accounts_) {
        w.BeginObject();
        w.Field("name", a.name);
        w.Field("salt", a.salt);
        w.Field("hash", a.hash);
        w.Field("iterations", a.iterations);
        w.Field("created_ms", a.createdMs);
        w.Field("points", a.points);
        w.Field("peak_points", a.peakPoints);
        w.Field("icon", a.icon);
        const AccountStats& s = a.stats;
        w.Key("stats").BeginObject();
        w.Field("games", s.games).Field("wins", s.wins).Field("top4", s.top4).Field("placement_sum", s.placementSum);
        w.Field("ranked_games", s.rankedGames).Field("ranked_wins", s.rankedWins).Field("ranked_top4", s.rankedTop4).Field("ranked_placement_sum", s.rankedPlacementSum);
        w.EndObject();
        w.Key("history").BeginArray();
        for (const HistoryEntry& h : a.history) {
            w.BeginObject();
            w.Field("time_ms", h.timeMs).Field("mode", h.mode).Field("placement", h.placement).Field("level", h.level).Field("round", h.round);
            w.Field("lp_change", h.lpChange).Field("points_after", h.pointsAfter);
            w.Key("board").BeginArray();
            for (const HistoryUnit& u : h.board) {
                w.BeginObject();
                w.Field("champion", u.champion).Field("star", u.star);
                w.Key("items").BeginArray();
                for (int i : u.items) w.Int(i);
                w.EndArray();
                w.EndObject();
            }
            w.EndArray();
            w.Key("players").BeginArray();
            for (const std::string& n : h.players) w.String(n);
            w.EndArray();
            w.EndObject();
        }
        w.EndArray();
        const auto names = [&w](const char* k, const std::set<std::string>& set) {
            w.Key(k).BeginArray();
            for (const std::string& n : set) w.String(n);
            w.EndArray();
        };
        names("friends", a.friends);
        names("incoming", a.incoming);
        names("outgoing", a.outgoing);
        w.Key("sessions").BeginArray();
        for (const std::string& t : a.sessions) w.String(t);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error != nullptr) *error = "cannot write " + tmp; return false; }
        out << w.str();
        out.flush();
        if (!out) { if (error != nullptr) *error = "cannot write " + tmp; return false; }
    }
    if (std::rename(tmp.c_str(), path_.c_str()) != 0) { if (error != nullptr) *error = "cannot replace " + path_; return false; }
    return true;
}

bool AccountStore::Load(std::string* error) {
    if (path_.empty()) return true;
    std::ifstream in(path_, std::ios::binary);
    if (!in) return true;   // no file yet: an empty store
    std::stringstream buffer;
    buffer << in.rdbuf();
    json::Value root;
    std::string parseError;
    if (!json::Parse(buffer.str(), root, &parseError) || !root.IsObject()) { if (error != nullptr) *error = path_ + ": " + parseError; return false; }
    const json::Value* list = root.Find("accounts");
    if (list == nullptr || !list->IsArray()) { if (error != nullptr) *error = path_ + ": no \"accounts\" array"; return false; }
    accounts_.clear();
    for (const json::Value& e : list->Items()) {
        Account a;
        a.name = StrOf(e.Find("name"));
        if (!ValidName(a.name)) continue;
        a.salt = StrOf(e.Find("salt"));
        a.hash = StrOf(e.Find("hash"));
        a.iterations = IntOf(e.Find("iterations"), 1);
        a.createdMs = U64Of(e.Find("created_ms"));
        a.points = IntOf(e.Find("points"));
        a.peakPoints = IntOf(e.Find("peak_points"));
        a.icon = IntOf(e.Find("icon"));   // (revision 9; absent in older files)
        if (const json::Value* s = e.Find("stats")) {
            a.stats.games = IntOf(s->Find("games")); a.stats.wins = IntOf(s->Find("wins")); a.stats.top4 = IntOf(s->Find("top4")); a.stats.placementSum = IntOf(s->Find("placement_sum"));
            a.stats.rankedGames = IntOf(s->Find("ranked_games")); a.stats.rankedWins = IntOf(s->Find("ranked_wins"));
            a.stats.rankedTop4 = IntOf(s->Find("ranked_top4")); a.stats.rankedPlacementSum = IntOf(s->Find("ranked_placement_sum"));
        }
        if (const json::Value* h = e.Find("history"); h != nullptr && h->IsArray()) {
            for (const json::Value& m : h->Items()) {
                HistoryEntry x;
                x.timeMs = U64Of(m.Find("time_ms")); x.mode = StrOf(m.Find("mode")); x.placement = IntOf(m.Find("placement")); x.level = IntOf(m.Find("level"));
                x.round = IntOf(m.Find("round")); x.lpChange = IntOf(m.Find("lp_change")); x.pointsAfter = IntOf(m.Find("points_after"));
                if (const json::Value* b = m.Find("board"); b != nullptr && b->IsArray()) {
                    for (const json::Value& u : b->Items()) {
                        HistoryUnit hu;
                        hu.champion = IntOf(u.Find("champion")); hu.star = IntOf(u.Find("star"), 1);
                        if (const json::Value* it = u.Find("items"); it != nullptr && it->IsArray()) { for (const json::Value& i : it->Items()) hu.items.push_back(IntOf(&i)); }
                        x.board.push_back(std::move(hu));
                    }
                }
                if (const json::Value* p = m.Find("players"); p != nullptr && p->IsArray()) { for (const json::Value& n : p->Items()) x.players.push_back(StrOf(&n)); }
                a.history.push_back(std::move(x));
            }
        }
        const auto names = [&e](const char* k, std::set<std::string>& set) {
            if (const json::Value* v = e.Find(k); v != nullptr && v->IsArray()) { for (const json::Value& n : v->Items()) set.insert(StrOf(&n)); }
        };
        names("friends", a.friends);
        names("incoming", a.incoming);
        names("outgoing", a.outgoing);
        if (const json::Value* v = e.Find("sessions"); v != nullptr && v->IsArray()) { for (const json::Value& t : v->Items()) a.sessions.push_back(StrOf(&t)); }
        accounts_[Key(a.name)] = std::move(a);
    }
    return true;
}

}  // namespace w2f::net
