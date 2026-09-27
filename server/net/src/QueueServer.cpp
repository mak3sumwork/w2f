#include "w2f/net/QueueServer.h"

#include <algorithm>
#include <map>
#include <vector>

#include "w2f/net/Messages.h"

namespace w2f::net {

namespace {

constexpr std::uint16_t kCloseNormal = 1000;   // what a GameServer closes its players with when a match is over: here they go back to idle instead
constexpr std::uint16_t kClosePolicy = 1008;

struct Bucket {   // the same token bucket GameServer uses, for the messages of idle / searching connections
    long long milli = 0;
    std::uint64_t last = 0;
    bool started = false;
    bool Take(std::uint64_t now, int cost, int burst, int refillPerSecond) {
        const long long cap = static_cast<long long>(burst) * 1000;
        if (!started) { milli = cap; last = now; started = true; }
        if (now > last) milli = std::min(cap, milli + static_cast<long long>(now - last) * refillPerSecond);
        last = std::max(last, now);
        if (milli < static_cast<long long>(cost) * 1000) return false;
        milli -= static_cast<long long>(cost) * 1000;
        return true;
    }
};

}  // namespace

class QueueServer::Impl {
public:
    // What a match's GameServer is given as its transport: sends go straight out, closes come to the hub first.
    class RoomTransport : public IServerTransport {
    public:
        RoomTransport(Impl& hub, std::uint64_t room) : hub_(hub), room_(room) {}
        void Send(ConnectionId id, std::string_view text) override { hub_.transport_.Send(id, text); }
        void Close(ConnectionId id, std::uint16_t code, std::string_view reason) override { hub_.OnRoomClose(room_, id, code, reason); }
    private:
        Impl& hub_;
        std::uint64_t room_;
    };

    struct Room {
        std::uint64_t id = 0;
        QueueMode mode = QueueMode::Bots;
        std::unique_ptr<RoomTransport> transport;   // declared before the game: the game holds a reference to it
        std::unique_ptr<GameServer> game;
        int unattendedTicks = 0;
        bool dead = false;
        std::vector<std::string> accounts;   // the account key of each human seat ("" = anonymous)
    };

    struct Client {
        std::uint64_t room = 0;   // 0 = not in a match
        bool searching = false;
        QueueMode mode = QueueMode::Bots;
        std::uint64_t searchingSinceMs = 0;
        std::uint64_t order = 0;    // first come, first served
        Bucket bucket;
        int violations = 0;
        bool closing = false;
        std::string account, session;   // logged in (revision 8)
    };

    Impl(const QueueServerConfig& config, const GameData& data, IServerTransport& transport) : cfg_(config), data_(data), transport_(transport) {
        cfg_.match.seats = std::max(2, std::min(kMaxPlayers, cfg_.match.seats));
    }

    void OnConnect(ConnectionId id, std::string_view token, std::uint64_t nowMs) {
        now_ = nowMs;
        Client& c = clients_[id];
        c = Client{};
        if (!token.empty()) {
            for (const auto& room : rooms_) {
                if (!room->dead && room->game->HoldsToken(token)) {   // a player coming back to their match
                    c.room = room->id;
                    room->game->OnConnect(id, token, nowMs);
                    return;
                }
            }
        }
        SendStatus(id, "");
    }

    void OnMessage(ConnectionId id, std::string_view text, std::uint64_t nowMs) {
        now_ = nowMs;
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.closing) return;
        Client& c = it->second;

        if (Room* room = Find(c.room)) {
            // In a match everything goes to the match, except the hub's own commands (the match rate-limits and validates the rest).
            const ParseResult parsed = ParseCommand(text);
            if (parsed.ok && parsed.command.type == CommandType::LeaveMatch) {
                room->game->OnDisconnect(id);   // the seat stays reserved for its token; the match goes on
                c.room = 0;
                SendStatus(id, "left_match");
                return PresenceChanged(c.account);
            }
            if (parsed.ok && IsAccountCommand(parsed.command.type)) {   // profiles and friends work in a match too
                if (!c.bucket.Take(nowMs, 1, cfg_.match.rateBurst, cfg_.match.rateRefillPerSecond)) return Violation(id, c, "rate_limited", "too many messages, slow down");
                return AccountCommand(id, parsed.command);
            }
            if (parsed.ok && (parsed.command.type == CommandType::JoinQueue || parsed.command.type == CommandType::LeaveQueue)) {
                return transport_.Send(id, msg::Error("in_match", "you are in a match: leave_match first", parsed.command.hasId, parsed.command.id));
            }
            return room->game->OnMessage(id, text, nowMs);
        }

        if (!c.bucket.Take(nowMs, 1, cfg_.match.rateBurst, cfg_.match.rateRefillPerSecond)) return Violation(id, c, "rate_limited", "too many messages, slow down");
        const ParseResult parsed = ParseCommand(text);
        if (!parsed.ok) return Violation(id, c, parsed.error.code, parsed.error.detail, parsed.hasId, parsed.id);
        const Command& cmd = parsed.command;
        if (IsAccountCommand(cmd.type)) return AccountCommand(id, cmd);
        switch (cmd.type) {
            case CommandType::Ping: return transport_.Send(id, msg::Pong(cmd.hasId, cmd.id));
            case CommandType::GetCatalog:
                if (!c.bucket.Take(nowMs, 4, cfg_.match.rateBurst, cfg_.match.rateRefillPerSecond)) return Violation(id, c, "rate_limited", "too many messages, slow down", cmd.hasId, cmd.id);
                return transport_.Send(id, Catalog());
            case CommandType::JoinQueue:
                if (cfg_.accounts != nullptr && c.account.empty()) return transport_.Send(id, msg::Error("not_logged_in", "log in first", cmd.hasId, cmd.id));
                if (cmd.queueMode == QueueMode::Ranked && cfg_.accounts == nullptr) return transport_.Send(id, msg::Error("no_accounts", "ranked needs accounts (start the server with --accounts FILE)", cmd.hasId, cmd.id));
                if (!c.searching || c.mode != cmd.queueMode) {
                    c.searching = true;
                    c.mode = cmd.queueMode;
                    c.searchingSinceMs = nowMs;
                    c.order = ++order_;
                }
                SendStatus(id, "");
                PresenceChanged(c.account);
                return Matchmake();   // (a bots match starts at once, unless the server is at its match limit)
            case CommandType::LeaveQueue:
                if (!c.searching) return transport_.Send(id, msg::Error("not_searching", "you are not in a queue", cmd.hasId, cmd.id));
                c.searching = false;
                SendStatus(id, "cancelled");
                return PresenceChanged(c.account);
            case CommandType::LeaveMatch: return transport_.Send(id, msg::Error("not_in_match", "you are not in a match", cmd.hasId, cmd.id));
            default: return transport_.Send(id, msg::Error("not_in_match", "no match yet: queue first", cmd.hasId, cmd.id));
        }
    }

    void OnDisconnect(ConnectionId id) {
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        if (Room* room = Find(it->second.room)) room->game->OnDisconnect(id);
        const std::string account = it->second.account;
        clients_.erase(it);
        PresenceChanged(account);
    }

    void Tick(std::uint64_t nowMs) {
        now_ = nowMs;
        for (const auto& room : rooms_) {
            if (room->dead) continue;
            room->game->Tick(nowMs);
            if (room->game->state() == GameServer::State::Lobby) { room->dead = true; continue; }   // it was over and has let its players go
            room->unattendedTicks = room->game->connectedPlayers() == 0 ? room->unattendedTicks + 1 : 0;
            if (room->unattendedTicks >= cfg_.abandonTicks) room->dead = true;   // nobody is coming back: stop simulating it
        }
        Reap();
        Matchmake();
        if (nowMs >= lastStatusMs_ + static_cast<std::uint64_t>(cfg_.statusEveryMs)) {
            lastStatusMs_ = nowMs;
            for (const auto& [id, c] : clients_) {
                if (c.searching && !c.closing) SendStatus(id, "");
            }
        }
    }

    // A match closed one of its connections. "Match over" means: back to the home screen, the socket stays open. Anything else (a protocol
    // violation, a newer connection took the seat) really closes it; the match still expects that connection's OnDisconnect, so keep routing it.
    void OnRoomClose(std::uint64_t roomId, ConnectionId id, std::uint16_t code, std::string_view reason) {
        auto it = clients_.find(id);
        if (it == clients_.end() || it->second.room != roomId) return transport_.Close(id, code, reason);
        if (code == kCloseNormal) {
            it->second.room = 0;
            pendingIdle_.push_back(id);   // not now: we are inside the match's call, and the status must follow its last messages
            return;
        }
        it->second.closing = true;
        transport_.Close(id, code, reason);
    }

    int Searching(QueueMode mode) const {
        int n = 0;
        for (const auto& [id, c] : clients_) n += c.searching && c.mode == mode ? 1 : 0;
        return n;
    }
    int LiveRooms() const {
        int n = 0;
        for (const auto& room : rooms_) n += room->dead ? 0 : 1;
        return n;
    }
    const GameServer* MatchOf(ConnectionId id) const {
        auto it = clients_.find(id);
        if (it == clients_.end()) return nullptr;
        for (const auto& room : rooms_) {
            if (room->id == it->second.room && !room->dead) return room->game.get();
        }
        return nullptr;
    }

    std::map<ConnectionId, Client> clients_;   // ordered: every sweep over it is deterministic

private:
    Room* Find(std::uint64_t id) {
        if (id == 0) return nullptr;
        for (const auto& room : rooms_) {
            if (room->id == id && !room->dead) return room.get();
        }
        return nullptr;
    }

    void Violation(ConnectionId id, Client& c, std::string_view code, std::string_view detail, bool hasId = false, long long msgId = 0) {
        transport_.Send(id, msg::Error(code, detail, hasId, msgId));
        if (++c.violations > cfg_.match.maxViolations) {
            transport_.Send(id, msg::Error("too_many_violations", "disconnecting"));
            transport_.Close(id, kClosePolicy, "too many protocol violations");
            c.closing = true;
            c.searching = false;
        }
    }

    void SendStatus(ConnectionId id, const char* reason) {
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        const Client& c = it->second;
        msg::QueueInfo info;
        info.searching = c.searching;
        info.mode = c.mode;
        info.inQueue = c.searching ? Searching(c.mode) : 0;
        info.waitedMs = c.searching && now_ > c.searchingSinceMs ? static_cast<long long>(now_ - c.searchingSinceMs) : 0;
        info.fillMs = c.mode == QueueMode::Normal ? cfg_.fillMs : 0;
        info.seats = cfg_.match.seats;
        info.online = static_cast<int>(clients_.size());
        info.matches = LiveRooms();
        info.reason = reason;
        transport_.Send(id, msg::QueueStatus(info));
    }

    const std::string& Catalog() {
        if (catalog_.empty()) catalog_ = msg::Catalog(*data_.champions, data_.items, data_.traits, data_.encounters, data_.config.combat, data_.text);
        return catalog_;
    }

    // Deletes finished / abandoned matches (outside every call into them) and sends the players who came back from one their idle status.
    void Reap() {
        for (ConnectionId id : pendingIdle_) {
            if (clients_.count(id) && !clients_[id].closing) {
                SendStatus(id, "match_over");
                PresenceChanged(clients_[id].account);
            }
        }
        pendingIdle_.clear();
        for (auto& [id, c] : clients_) {
            if (c.room != 0 && Find(c.room) == nullptr) c.room = 0;   // (an abandoned match: nobody was connected, but never assume)
        }
        rooms_.erase(std::remove_if(rooms_.begin(), rooms_.end(), [](const std::unique_ptr<Room>& r) { return r->dead; }), rooms_.end());
    }

    void Matchmake() {
        for (QueueMode mode : {QueueMode::Bots, QueueMode::Normal, QueueMode::Ranked}) {
            for (;;) {
                if (LiveRooms() >= cfg_.maxMatches) return;
                std::vector<std::pair<std::uint64_t, ConnectionId>> waiting;   // (order, connection): first come, first served
                for (const auto& [id, c] : clients_) {
                    if (c.searching && !c.closing && c.mode == mode) waiting.emplace_back(c.order, id);
                }
                if (waiting.empty()) break;
                std::sort(waiting.begin(), waiting.end());
                std::vector<ConnectionId> players;
                if (mode == QueueMode::Bots) {
                    players.push_back(waiting.front().second);
                } else {
                    const Client& oldest = clients_[waiting.front().second];
                    const bool full = static_cast<int>(waiting.size()) >= cfg_.match.seats;
                    const bool waitedEnough = now_ >= oldest.searchingSinceMs + static_cast<std::uint64_t>(cfg_.fillMs);
                    if (!full && !waitedEnough) break;
                    for (std::size_t i = 0; i < waiting.size() && static_cast<int>(players.size()) < cfg_.match.seats; ++i) players.push_back(waiting[i].second);
                }
                StartRoom(mode, players);
            }
        }
    }

    void StartRoom(QueueMode mode, const std::vector<ConnectionId>& players) {
        auto room = std::make_unique<Room>();
        room->id = ++roomIds_;
        room->mode = mode;
        room->transport = std::make_unique<RoomTransport>(*this, room->id);
        GameServerConfig gc = cfg_.match;
        const int humans = static_cast<int>(players.size());
        gc.bots = gc.seats - humans;
        for (ConnectionId id : players) {   // the players connect in this order, so seat i is players[i]: their usernames name the seats
            const Account* a = cfg_.accounts != nullptr ? cfg_.accounts->Find(clients_[id].account) : nullptr;
            gc.seatNames.push_back(a != nullptr ? a->name : std::string());
            room->accounts.push_back(a != nullptr ? clients_[id].account : std::string());
        }
        room->game = std::make_unique<GameServer>(gc, data_, *room->transport);
        const std::uint64_t roomId = room->id;
        room->game->SetMatchResultHandler([this, roomId](const MatchResult& result) { Record(roomId, result); });
        Room& r = *room;
        rooms_.push_back(std::move(room));
        for (ConnectionId id : players) {
            Client& c = clients_[id];
            c.searching = false;
            c.room = r.id;
            transport_.Send(id, msg::MatchFound(mode, humans, gc.bots));
        }
        for (ConnectionId id : players) r.game->OnConnect(id, "", now_);   // the last one fills the lobby: the match starts
        for (ConnectionId id : players) PresenceChanged(clients_[id].account);
    }

    // ---- accounts (revision 8) -------------------------------------------------------------------

    void AccountCommand(ConnectionId id, const Command& cmd) {
        AccountStore* store = cfg_.accounts;
        if (store == nullptr) return transport_.Send(id, msg::Error("no_accounts", "this server has no accounts (start it with --accounts FILE)", cmd.hasId, cmd.id));
        Client& c = clients_[id];
        const auto fail = [&](AccountStore::Result r) { transport_.Send(id, msg::Error(AccountStore::Code(r), AccountStore::Describe(r), cmd.hasId, cmd.id)); };
        switch (cmd.type) {
            case CommandType::Register:
            case CommandType::Login:
            case CommandType::ResumeSession: {
                if (c.room != 0) return transport_.Send(id, msg::Error("in_match", "you are in a match", cmd.hasId, cmd.id));
                std::string key, session = cmd.session;
                AccountStore::Result r = AccountStore::Result::Ok;
                if (cmd.type == CommandType::Register) r = store->Register(cmd.username, cmd.password, WallClock(), key, session);
                else if (cmd.type == CommandType::Login) r = store->Login(cmd.username, cmd.password, key, session);
                else r = store->Resume(cmd.session, key);
                if (r != AccountStore::Result::Ok) return fail(r);
                const std::string previous = c.account;
                c.searching = false;
                for (auto& [otherId, other] : clients_) {   // one connection per account: the older one is logged out
                    if (otherId != id && other.account == key) {
                        transport_.Send(otherId, msg::LoggedOut("elsewhere"));
                        other.account.clear();
                        other.session.clear();
                        other.searching = false;
                    }
                }
                c.account = key;
                c.session = session;
                const Account* a = store->Find(key);
                transport_.Send(id, msg::Auth(*a, session));
                transport_.Send(id, msg::Profile(*a, true));
                SendFriends(id);
                if (!previous.empty() && previous != key) PresenceChanged(previous);
                return PresenceChanged(key);
            }
            default: break;
        }
        if (c.account.empty()) return transport_.Send(id, msg::Error("not_logged_in", "log in first", cmd.hasId, cmd.id));
        const std::string me = c.account;
        switch (cmd.type) {
            case CommandType::Logout: {
                store->Logout(me, c.session);
                c.account.clear();
                c.session.clear();
                c.searching = false;
                transport_.Send(id, msg::LoggedOut("logout"));
                return PresenceChanged(me);
            }
            case CommandType::GetProfile: {
                const Account* a = store->Find(cmd.username.empty() ? me : cmd.username);
                if (a == nullptr) return fail(AccountStore::Result::UnknownUser);
                return transport_.Send(id, msg::Profile(*a, Status(AccountStore::Key(a->name)) != "offline"));
            }
            case CommandType::GetFriends: return SendFriends(id);
            case CommandType::FriendRequest:
            case CommandType::FriendAccept:
            case CommandType::FriendDecline:
            case CommandType::FriendRemove: {
                AccountStore::Result r = AccountStore::Result::Ok;
                bool became = false;
                if (cmd.type == CommandType::FriendRequest) r = store->Request(me, cmd.username, became);
                else if (cmd.type == CommandType::FriendAccept) r = store->Accept(me, cmd.username);
                else if (cmd.type == CommandType::FriendDecline) r = store->Decline(me, cmd.username);
                else r = store->Unfriend(me, cmd.username);
                if (r != AccountStore::Result::Ok) return fail(r);
                SendFriendsToAccount(me);
                return SendFriendsToAccount(AccountStore::Key(cmd.username));
            }
            default: return;
        }
    }

    std::uint64_t WallClock() const { return cfg_.wallClockMs ? cfg_.wallClockMs() : 0; }

    std::string Status(const std::string& key) const {
        std::string best = "offline";
        for (const auto& [id, c] : clients_) {
            if (c.account != key || c.closing) continue;
            if (c.room != 0) return "in_match";
            best = c.searching ? "searching" : "online";
        }
        return best;
    }

    void SendFriends(ConnectionId id) {
        const Account* a = cfg_.accounts != nullptr ? cfg_.accounts->Find(clients_[id].account) : nullptr;
        if (a == nullptr) return;
        std::vector<msg::FriendInfo> list;
        for (const std::string& k : a->friends) {
            const Account* f = cfg_.accounts->Find(k);
            if (f != nullptr) list.push_back({f->name, Status(k), f->points});
        }
        std::sort(list.begin(), list.end(), [](const msg::FriendInfo& x, const msg::FriendInfo& y) {   // online first, then by name
            const bool xo = x.status != "offline", yo = y.status != "offline";
            return xo != yo ? xo : AccountStore::Key(x.name) < AccountStore::Key(y.name);
        });
        std::vector<std::string> in, out;
        for (const std::string& k : a->incoming) { if (const Account* f = cfg_.accounts->Find(k)) in.push_back(f->name); }
        for (const std::string& k : a->outgoing) { if (const Account* f = cfg_.accounts->Find(k)) out.push_back(f->name); }
        transport_.Send(id, msg::Friends(list, in, out));
    }

    void SendFriendsToAccount(const std::string& key) {
        for (const auto& [id, c] : clients_) {
            if (c.account == key && !c.closing) SendFriends(id);
        }
    }

    // A player's status changed (logged in / out, queued, in a match, back): their online friends get a fresh list.
    void PresenceChanged(const std::string& key) {
        if (key.empty() || cfg_.accounts == nullptr) return;
        const Account* a = cfg_.accounts->Find(key);
        if (a == nullptr) return;
        for (const std::string& f : a->friends) SendFriendsToAccount(f);
    }

    // A match of this room ended: every logged-in player's history, stats and (ranked) LP; they get their new profile.
    void Record(std::uint64_t roomId, const MatchResult& result) {
        if (cfg_.accounts == nullptr) return;
        const Room* room = nullptr;
        for (const auto& r : rooms_) { if (r->id == roomId) room = r.get(); }
        if (room == nullptr) return;
        std::vector<std::pair<int, std::string>> order;
        for (const MatchResultSeat& s : result.seats) order.emplace_back(s.placement > 0 ? s.placement : 99, s.name);
        std::sort(order.begin(), order.end());
        std::vector<std::string> lobby;
        for (const auto& [p, n] : order) lobby.push_back(n);
        for (std::size_t i = 0; i < room->accounts.size() && i < result.seats.size(); ++i) {
            const std::string& key = room->accounts[i];
            if (key.empty()) continue;
            const MatchResultSeat& s = result.seats[i];
            HistoryEntry h;
            h.timeMs = WallClock();
            h.mode = ToString(room->mode);
            h.placement = s.placement;
            h.level = s.level;
            h.round = s.round;
            for (const MatchResultUnit& u : s.board) h.board.push_back({u.champion, u.star, u.items});
            h.players = lobby;
            cfg_.accounts->RecordMatch(key, std::move(h));
            for (const auto& [id, c] : clients_) {
                if (c.account == key && !c.closing) transport_.Send(id, msg::Profile(*cfg_.accounts->Find(key), true));
            }
        }
    }

    QueueServerConfig cfg_;
    const GameData data_;
    IServerTransport& transport_;
    std::vector<std::unique_ptr<Room>> rooms_;
    std::vector<ConnectionId> pendingIdle_;
    std::string catalog_;
    std::uint64_t now_ = 0;
    std::uint64_t lastStatusMs_ = 0;
    std::uint64_t order_ = 0;
    std::uint64_t roomIds_ = 0;
};

QueueServer::QueueServer(const QueueServerConfig& config, const GameData& data, IServerTransport& transport)
    : impl_(std::make_unique<Impl>(config, data, transport)) {}
QueueServer::~QueueServer() = default;

void QueueServer::OnConnect(ConnectionId id, std::string_view token, std::uint64_t nowMs) { impl_->OnConnect(id, token, nowMs); }
void QueueServer::OnMessage(ConnectionId id, std::string_view text, std::uint64_t nowMs) { impl_->OnMessage(id, text, nowMs); }
void QueueServer::OnDisconnect(ConnectionId id) { impl_->OnDisconnect(id); }
void QueueServer::Tick(std::uint64_t nowMs) { impl_->Tick(nowMs); }
int QueueServer::online() const { return static_cast<int>(impl_->clients_.size()); }
int QueueServer::searching(QueueMode mode) const { return impl_->Searching(mode); }
int QueueServer::matches() const { return impl_->LiveRooms(); }
const GameServer* QueueServer::MatchOf(ConnectionId id) const { return impl_->MatchOf(id); }

}  // namespace w2f::net
