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

void WriteBoard(JsonWriter& w, const std::vector<HistoryUnit>& board) {
    w.Key("board").BeginArray();
    for (const HistoryUnit& u : board) {
        w.BeginObject();
        w.Field("champion", u.champion).Field("star", u.star);
        w.Key("items").BeginArray();
        for (int i : u.items) w.Int(i);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
}

std::vector<HistoryUnit> ReadBoard(const json::Value* b) {
    std::vector<HistoryUnit> out;
    if (b == nullptr || !b->IsArray()) return out;
    for (const json::Value& u : b->Items()) {
        HistoryUnit hu;
        hu.champion = IntOf(u.Find("champion")); hu.star = IntOf(u.Find("star"), 1);
        if (const json::Value* it = u.Find("items"); it != nullptr && it->IsArray()) { for (const json::Value& i : it->Items()) hu.items.push_back(IntOf(&i)); }
        out.push_back(std::move(hu));
    }
    return out;
}

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
        case Result::UnknownItem: return "unknown_item";
        case Result::AlreadyOwned: return "already_owned";
        case Result::NotEnoughCoins: return "not_enough_coins";
        case Result::NotOwned: return "not_owned";
        case Result::UnknownMatch: return "unknown_match";
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
        case Result::UnknownItem: return "There is no such item in the store.";
        case Result::AlreadyOwned: return "You already own it.";
        case Result::NotEnoughCoins: return "Not enough coins.";
        case Result::NotOwned: return "Buy it first.";
        case Result::UnknownMatch: return "That match is no longer kept.";
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
    a.coins = progression_ != nullptr ? progression_->startingCoins : 0;
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

AccountStore::Result AccountStore::ChangePassword(std::string_view key, std::string_view oldPassword, std::string_view newPassword, std::string_view keepSession) {
    Account* a = Mutable(key);
    if (a == nullptr) return Result::UnknownUser;
    if (!ValidPassword(oldPassword) || !ConstantTimeEquals(Hash(oldPassword, a->salt, a->iterations), a->hash)) return Result::BadCredentials;
    if (!ValidPassword(newPassword)) return Result::InvalidPassword;
    a->salt = NewHex(16);
    a->iterations = iterations_;
    a->hash = Hash(newPassword, a->salt, a->iterations);
    std::vector<std::string> kept;   // a changed password signs every other device out
    for (const std::string& s : a->sessions) {
        if (s == keepSession) kept.push_back(s);
    }
    a->sessions = kept;
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Buy(std::string_view key, std::string_view itemId) {
    Account* a = Mutable(key);
    if (a == nullptr) return Result::UnknownUser;
    const StoreItem* item = progression_ != nullptr ? progression_->FindItem(itemId) : nullptr;
    if (item == nullptr) return Result::UnknownItem;
    if (Owns(*a, *item)) return Result::AlreadyOwned;
    if (a->coins < item->price) return Result::NotEnoughCoins;
    a->coins -= item->price;
    a->owned.insert(item->id);
    Changed();
    return Result::Ok;
}

AccountStore::Result AccountStore::Equip(std::string_view key, std::string_view itemId) {
    Account* a = Mutable(key);
    if (a == nullptr) return Result::UnknownUser;
    const StoreItem* item = progression_ != nullptr ? progression_->FindItem(itemId) : nullptr;
    if (item == nullptr) return Result::UnknownItem;
    if (!Owns(*a, *item)) return Result::NotOwned;
    std::string& slot = item->kind == "border" ? a->border : (item->kind == "title" ? a->title : a->arena);
    slot = item->isDefault ? std::string() : item->id;
    Changed();
    return Result::Ok;
}

std::string AccountStore::Equipped(const Account& account, std::string_view kind) const {
    const std::string& worn = kind == "border" ? account.border : (kind == "title" ? account.title : account.arena);
    if (!worn.empty()) return worn;
    const StoreItem* d = progression_ != nullptr ? progression_->DefaultOf(kind) : nullptr;
    return d != nullptr ? d->id : std::string();
}

int AccountStore::LevelOf(const Account& account) const {
    if (progression_ == nullptr) return 1;
    int level = 1, into = 0, need = 0;
    progression_->LevelOf(account.xp, level, into, need);
    return level;
}

int AccountStore::Grant(std::string_view key, int coins, int xp) {
    Account* a = Mutable(key);
    if (a == nullptr || progression_ == nullptr) return 0;
    const int before = LevelOf(*a);
    a->coins += std::max(0, coins);
    a->xp += std::max(0, xp);
    const int gained = LevelOf(*a) - before;
    a->coins += gained * progression_->levelUpCoins;
    Changed();
    return gained;
}

void AccountStore::RefreshMissions(std::string_view key, std::uint64_t nowMs) {
    Account* a = Mutable(key);
    if (a == nullptr || progression_ == nullptr) return;
    const long long day = DayOf(nowMs), week = WeekOf(nowMs);
    if (a->missionDay == day && a->missionWeek == week) return;
    // a new day forgets the daily missions' progress, a new week the weekly ones'
    std::map<std::string, int> progress;
    std::set<std::string> done;
    for (const MissionDef& m : progression_->missions) {
        const bool keep = m.period == "daily" ? a->missionDay == day : a->missionWeek == week;
        if (!keep) continue;
        if (auto it = a->missionProgress.find(m.id); it != a->missionProgress.end()) progress[m.id] = it->second;
        if (a->missionsDone.count(m.id) != 0) done.insert(m.id);
    }
    a->missionProgress = progress;
    a->missionsDone = done;
    a->missionDay = day;
    a->missionWeek = week;
    Changed();
}

std::vector<AccountStore::MissionUpdate> AccountStore::Missions(std::string_view key, std::uint64_t nowMs) {
    std::vector<MissionUpdate> out;
    RefreshMissions(key, nowMs);
    const Account* a = Find(key);
    if (a == nullptr || progression_ == nullptr) return out;
    std::vector<const MissionDef*> current = progression_->Daily(DayOf(nowMs));
    for (const MissionDef* w : progression_->Weekly(WeekOf(nowMs))) current.push_back(w);
    for (const MissionDef* m : current) {
        MissionUpdate u;
        u.mission = m;
        auto it = a->missionProgress.find(m->id);
        u.progress = it == a->missionProgress.end() ? 0 : it->second;
        u.done = a->missionsDone.count(m->id) != 0;
        out.push_back(u);
    }
    return out;
}

std::vector<AccountStore::MissionUpdate> AccountStore::CountMissions(std::string_view key, std::uint64_t nowMs, const MissionMatch& match) {
    std::vector<MissionUpdate> list = Missions(key, nowMs);
    Account* a = Mutable(key);
    if (a == nullptr) return list;
    int coins = 0, xp = 0;
    for (MissionUpdate& u : list) {
        if (u.done || !MissionCounts(*u.mission, match)) continue;
        u.progress = std::min(u.mission->count, u.progress + 1);
        a->missionProgress[u.mission->id] = u.progress;
        if (u.progress >= u.mission->count) {
            u.done = u.justDone = true;
            a->missionsDone.insert(u.mission->id);
            coins += u.mission->coins;
            xp += u.mission->xp;
        }
    }
    Changed();
    if (coins > 0 || xp > 0) Grant(key, coins, xp);
    return list;
}

std::uint64_t AccountStore::AddMatch(MatchRecord record) {
    record.id = nextMatchId_++;
    const std::uint64_t id = record.id;
    matches_[id] = std::move(record);
    while (static_cast<int>(matches_.size()) > kMatchesKept) matches_.erase(matches_.begin());
    Changed();
    return id;
}

const MatchRecord* AccountStore::FindMatch(std::uint64_t id) const {
    auto it = matches_.find(id);
    return it == matches_.end() ? nullptr : &it->second;
}

std::vector<const Account*> AccountStore::Ladder() const {
    std::vector<const Account*> out;
    for (const auto& [key, a] : accounts_) {
        if (a.stats.rankedGames > 0) out.push_back(&a);
    }
    std::sort(out.begin(), out.end(), [](const Account* x, const Account* y) { return x->points != y->points ? x->points > y->points : Key(x->name) < Key(y->name); });
    return out;
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
            w.Field("match_id", h.matchId).Field("coins", h.coins).Field("xp", h.xp);
            WriteBoard(w, h.board);
            w.Key("players").BeginArray();
            for (const std::string& n : h.players) w.String(n);
            w.EndArray();
            w.EndObject();
        }
        w.EndArray();
        w.Field("coins", a.coins).Field("xp", a.xp).Field("border", a.border).Field("title", a.title).Field("arena", a.arena);
        w.Field("mission_day", a.missionDay).Field("mission_week", a.missionWeek);
        w.Key("mission_progress").BeginObject();
        for (const auto& [id, n] : a.missionProgress) w.Field(id, n);
        w.EndObject();
        const auto names = [&w](const char* k, const std::set<std::string>& set) {
            w.Key(k).BeginArray();
            for (const std::string& n : set) w.String(n);
            w.EndArray();
        };
        names("friends", a.friends);
        names("incoming", a.incoming);
        names("outgoing", a.outgoing);
        names("owned", a.owned);
        names("missions_done", a.missionsDone);
        w.Key("sessions").BeginArray();
        for (const std::string& t : a.sessions) w.String(t);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();
    w.Field("next_match_id", nextMatchId_);
    w.Key("matches").BeginArray();
    for (const auto& [id, m] : matches_) {
        w.BeginObject();
        w.Field("id", m.id).Field("time_ms", m.timeMs).Field("mode", m.mode).Field("rounds", m.rounds);
        w.Key("seats").BeginArray();
        for (const MatchRecordSeat& s : m.seats) {
            w.BeginObject();
            w.Field("name", s.name).Field("bot", s.bot).Field("icon", s.icon).Field("border", s.border).Field("title", s.title);
            w.Field("placement", s.placement).Field("level", s.level).Field("round", s.round);
            WriteBoard(w, s.board);
            w.EndObject();
        }
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
                x.matchId = U64Of(m.Find("match_id")); x.coins = IntOf(m.Find("coins")); x.xp = IntOf(m.Find("xp"));   // (revision 10)
                x.board = ReadBoard(m.Find("board"));
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
        names("owned", a.owned);   // (revision 10; absent in older files)
        names("missions_done", a.missionsDone);
        a.coins = IntOf(e.Find("coins")); a.xp = IntOf(e.Find("xp"));
        a.border = StrOf(e.Find("border")); a.title = StrOf(e.Find("title")); a.arena = StrOf(e.Find("arena"));
        a.missionDay = IntOf(e.Find("mission_day"), -1); a.missionWeek = IntOf(e.Find("mission_week"), -1);
        if (const json::Value* mp = e.Find("mission_progress"); mp != nullptr && mp->IsObject()) {
            for (std::size_t i = 0; i < mp->MemberCount(); ++i) a.missionProgress[mp->MemberKey(i)] = IntOf(&mp->MemberValue(i));
        }
        if (const json::Value* v = e.Find("sessions"); v != nullptr && v->IsArray()) { for (const json::Value& t : v->Items()) a.sessions.push_back(StrOf(&t)); }
        accounts_[Key(a.name)] = std::move(a);
    }
    matches_.clear();
    nextMatchId_ = std::max<std::uint64_t>(1, U64Of(root.Find("next_match_id")));
    if (const json::Value* ms = root.Find("matches"); ms != nullptr && ms->IsArray()) {
        for (const json::Value& m : ms->Items()) {
            MatchRecord r;
            r.id = U64Of(m.Find("id")); r.timeMs = U64Of(m.Find("time_ms")); r.mode = StrOf(m.Find("mode")); r.rounds = IntOf(m.Find("rounds"));
            if (const json::Value* seats = m.Find("seats"); seats != nullptr && seats->IsArray()) {
                for (const json::Value& e : seats->Items()) {
                    MatchRecordSeat seat;
                    seat.name = StrOf(e.Find("name")); seat.bot = e.Find("bot") != nullptr && e.Find("bot")->IsBool() && e.Find("bot")->AsBool();
                    seat.icon = IntOf(e.Find("icon")); seat.border = StrOf(e.Find("border")); seat.title = StrOf(e.Find("title"));
                    seat.placement = IntOf(e.Find("placement")); seat.level = IntOf(e.Find("level")); seat.round = IntOf(e.Find("round"));
                    seat.board = ReadBoard(e.Find("board"));
                    r.seats.push_back(std::move(seat));
                }
            }
            if (r.id == 0) continue;
            nextMatchId_ = std::max(nextMatchId_, r.id + 1);
            matches_[r.id] = std::move(r);
        }
    }
    return true;
}

}  // namespace w2f::net
