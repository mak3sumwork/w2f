#include "w2f/net/QueueServer.h"

#include <algorithm>
#include <deque>
#include <map>
#include <vector>

#include "w2f/net/Messages.h"

namespace w2f::net {

namespace {

constexpr std::uint16_t kCloseNormal = 1000;   // what a GameServer closes its players with when a match is over: here they go back to idle instead
constexpr std::uint16_t kClosePolicy = 1008;
constexpr int kLadderShown = 50;

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

// A chat line as it is kept: control characters become spaces, the ends are trimmed.
std::string CleanChat(std::string_view text) {
    std::string out;
    for (char ch : text) out.push_back(static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F ? ' ' : ch);
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

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
        bool readyCheck = false;        // (revision 9) queued asking for a ready check
        std::uint64_t proposal = 0;     // in this ready check (0 = none); `searching` is false meanwhile, the place in the queue is kept
        bool accepted = false;
        std::uint64_t group = 0;        // (revision 10) queued as part of this party (0 = alone): the matchmaker keeps a group together
    };

    // (revision 9) A match waiting for its ready check: it starts when every player accepted; a decline or the timeout sends the others back into
    // the queue in their old place and takes the decliners out of it.
    struct Proposal {
        std::uint64_t id = 0;
        QueueMode mode = QueueMode::Bots;
        std::vector<ConnectionId> players;
        std::uint64_t deadlineMs = 0;
    };

    // (revision 10) A party: friends who queue together. Members are ACCOUNTS (their connection is looked up), the leader first.
    struct Party {
        std::uint64_t id = 0;
        std::vector<std::string> members;
        QueueMode mode = QueueMode::Normal;
        std::map<std::string, std::uint64_t> invited;   // account key -> when the invite runs out (ms)
    };

    Impl(const QueueServerConfig& config, const GameData& data, IServerTransport& transport) : cfg_(config), data_(data), transport_(transport) {
        cfg_.match.seats = std::max(2, std::min(kMaxPlayers, cfg_.match.seats));
        if (cfg_.accounts != nullptr && cfg_.progression != nullptr) cfg_.accounts->AttachProgression(cfg_.progression);
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
            if (parsed.ok && IsAccountCommand(parsed.command.type)) {   // profiles, friends, chat ... work in a match too
                if (!c.bucket.Take(nowMs, 1, cfg_.match.rateBurst, cfg_.match.rateRefillPerSecond)) return Violation(id, c, "rate_limited", "too many messages, slow down");
                return AccountCommand(id, parsed.command);
            }
            if (parsed.ok && (parsed.command.type == CommandType::AcceptMatch || parsed.command.type == CommandType::DeclineMatch)) {
                return transport_.Send(id, msg::Error("no_ready_check", "no match is waiting for you", parsed.command.hasId, parsed.command.id));
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
            case CommandType::JoinQueue: return JoinQueue(id, cmd);
            case CommandType::LeaveQueue:
                if (c.proposal != 0) return Decline(id);
                if (!c.searching) return transport_.Send(id, msg::Error("not_searching", "you are not in a queue", cmd.hasId, cmd.id));
                return StopSearch(id, "cancelled");
            case CommandType::LeaveMatch: return transport_.Send(id, msg::Error("not_in_match", "you are not in a match", cmd.hasId, cmd.id));
            case CommandType::AcceptMatch: {
                Proposal* p = FindProposal(c.proposal);
                if (p == nullptr) return transport_.Send(id, msg::Error("no_ready_check", "no match is waiting for you", cmd.hasId, cmd.id));
                c.accepted = true;
                return Progress(*p);
            }
            case CommandType::DeclineMatch:
                if (c.proposal == 0) return transport_.Send(id, msg::Error("no_ready_check", "no match is waiting for you", cmd.hasId, cmd.id));
                return Decline(id);
            default: return transport_.Send(id, msg::Error("not_in_match", "no match yet: queue first", cmd.hasId, cmd.id));
        }
    }

    void OnDisconnect(ConnectionId id) {
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        if (Room* room = Find(it->second.room)) room->game->OnDisconnect(id);
        if (it->second.proposal != 0) { it->second.closing = true; Decline(id); }   // gone: as if they declined (nothing is sent to it any more)
        it = clients_.find(id);
        if (it->second.searching) { it->second.closing = true; StopSearch(id, "cancelled"); }   // (a party's search stops with it)
        it = clients_.find(id);
        const std::string account = it->second.account;
        clients_.erase(it);
        if (!account.empty() && ConnOf(account) == 0) LeaveParty(account, false);   // (revision 10) gone: out of the party
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
        for (std::size_t i = 0; i < proposals_.size();) {   // ready checks out of time: whoever has not accepted declined
            if (nowMs < proposals_[i].deadlineMs) { ++i; continue; }
            std::vector<ConnectionId> late;
            for (ConnectionId p : proposals_[i].players) { if (clients_.count(p) && !clients_[p].accepted) late.push_back(p); }
            FailProposal(proposals_[i].id, late);
        }
        Matchmake();
        if (nowMs >= lastStatusMs_ + static_cast<std::uint64_t>(cfg_.statusEveryMs)) {
            lastStatusMs_ = nowMs;
            for (const auto& [id, c] : clients_) {
                if (c.searching && !c.closing) SendStatus(id, "");
            }
            ExpireInvites();
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
            if (c.searching) StopSearch(id, "cancelled");
            if (c.proposal != 0) Decline(id);
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

    // ---- the queue ------------------------------------------------------------------------------------------------------------------------------------

    void JoinQueue(ConnectionId id, const Command& cmd) {
        Client& c = clients_[id];
        if (cfg_.accounts != nullptr && c.account.empty()) return transport_.Send(id, msg::Error("not_logged_in", "log in first", cmd.hasId, cmd.id));
        if (cmd.queueMode == QueueMode::Ranked && cfg_.accounts == nullptr) return transport_.Send(id, msg::Error("no_accounts", "ranked needs accounts (start the server with --accounts FILE)", cmd.hasId, cmd.id));
        if (c.proposal != 0) return transport_.Send(id, msg::Error("in_ready_check", "a match was found: accept_match or decline_match", cmd.hasId, cmd.id));
        // (revision 10) in a party of two or more, the leader queues for everyone
        std::vector<ConnectionId> group = {id};
        Party* party = PartyOfKey(c.account);
        if (party != nullptr && party->members.size() > 1) {
            if (party->members.front() != c.account) return transport_.Send(id, msg::Error("not_leader", "only the party leader can start the queue", cmd.hasId, cmd.id));
            if (cmd.queueMode == QueueMode::Ranked && static_cast<int>(party->members.size()) > cfg_.rankedPartyMax) {
                return transport_.Send(id, msg::Error("party_too_big", "Ranked takes parties of at most " + std::to_string(cfg_.rankedPartyMax), cmd.hasId, cmd.id));
            }
            group.clear();
            for (const std::string& m : party->members) {
                const ConnectionId mc = ConnOf(m);
                const Client* other = mc != 0 ? &clients_[mc] : nullptr;
                if (other == nullptr || other->room != 0 || other->proposal != 0) {
                    const Account* a = cfg_.accounts->Find(m);
                    return transport_.Send(id, msg::Error("party_busy", (a != nullptr ? a->name : m) + " is not ready (in a match or offline)", cmd.hasId, cmd.id));
                }
                group.push_back(mc);
            }
            if (party->mode != cmd.queueMode) { party->mode = cmd.queueMode; SendParty(party->id); }
        }
        const bool fresh = !c.searching || c.mode != cmd.queueMode;
        const std::uint64_t order = fresh ? ++order_ : c.order;
        const std::uint64_t groupId = group.size() > 1 ? party->id : 0;
        for (ConnectionId mc : group) {
            Client& m = clients_[mc];
            m.readyCheck = cmd.readyCheck;
            m.group = groupId;
            if (fresh || !m.searching) {
                m.searching = true;
                m.mode = cmd.queueMode;
                m.searchingSinceMs = now_;
                m.order = order;
            }
            SendStatus(mc, "");
            PresenceChanged(m.account);
        }
        Matchmake();   // (a bots match starts at once, unless the server is at its match limit)
    }

    // Stops a search -- the whole group's when it is a party's.
    void StopSearch(ConnectionId id, const char* reason) {
        const std::uint64_t group = clients_[id].group;
        std::vector<ConnectionId> who = {id};
        if (group != 0) {
            for (const auto& [other, oc] : clients_) {
                if (other != id && oc.searching && oc.group == group) who.push_back(other);
            }
        }
        for (ConnectionId w : who) {
            Client& c = clients_[w];
            c.searching = false;
            c.group = 0;
            if (!c.closing) SendStatus(w, reason);
            PresenceChanged(c.account);
        }
    }

    void Matchmake() {
        for (QueueMode mode : {QueueMode::Bots, QueueMode::Normal, QueueMode::Ranked}) {
            for (;;) {
                if (LiveRooms() + static_cast<int>(proposals_.size()) >= cfg_.maxMatches) return;
                // the waiting groups (a party, or one player), first come first served
                std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<ConnectionId>> groups;   // (order, group key) -> its players
                for (const auto& [id, c] : clients_) {
                    if (!c.searching || c.closing || c.mode != mode) continue;
                    const std::uint64_t key = c.group != 0 ? c.group : (std::uint64_t{1} << 63) | id;
                    groups[{c.order, key}].push_back(id);
                }
                if (groups.empty()) break;
                std::vector<ConnectionId> players;
                if (mode == QueueMode::Bots) {
                    players = groups.begin()->second;
                } else {
                    const Client& oldest = clients_[groups.begin()->second.front()];
                    const bool waitedEnough = now_ >= oldest.searchingSinceMs + static_cast<std::uint64_t>(cfg_.fillMs);
                    for (const auto& [key, members] : groups) {   // a group that does not fit waits for the next match
                        if (static_cast<int>(players.size() + members.size()) <= cfg_.match.seats) players.insert(players.end(), members.begin(), members.end());
                    }
                    const bool full = static_cast<int>(players.size()) >= cfg_.match.seats;
                    if (!full && !waitedEnough) break;
                }
                Propose(mode, players);
            }
        }
    }

    Proposal* FindProposal(std::uint64_t id) {
        for (Proposal& p : proposals_) { if (id != 0 && p.id == id) return &p; }
        return nullptr;
    }

    // The players are found. Nobody asked for a ready check (older clients): the match starts at once, as before revision 9.
    void Propose(QueueMode mode, const std::vector<ConnectionId>& players) {
        bool anyCheck = false;
        for (ConnectionId id : players) anyCheck = anyCheck || clients_[id].readyCheck;
        if (!anyCheck) return StartRoom(mode, players);
        Proposal p;
        p.id = ++proposalIds_;
        p.mode = mode;
        p.players = players;
        p.deadlineMs = now_ + static_cast<std::uint64_t>(cfg_.acceptMs);
        const int humans = static_cast<int>(players.size());
        for (ConnectionId id : players) {
            Client& c = clients_[id];
            c.searching = false;
            c.proposal = p.id;
            c.accepted = !c.readyCheck;   // a client without a ready check accepts by queueing
            if (c.readyCheck) transport_.Send(id, msg::MatchFound(mode, humans, cfg_.match.seats - humans, true, static_cast<int>(cfg_.acceptMs)));
        }
        proposals_.push_back(p);
        Progress(proposals_.back());
    }

    // Someone accepted: everyone in the check hears the count; when all have, the match starts.
    void Progress(const Proposal& p) {
        int accepted = 0;
        for (ConnectionId id : p.players) accepted += clients_[id].accepted ? 1 : 0;
        const int humans = static_cast<int>(p.players.size());
        if (accepted == humans) {
            const QueueMode mode = p.mode;
            const std::vector<ConnectionId> players = p.players;
            const std::uint64_t done = p.id;   // (copied out: erasing moves the element `p` refers to)
            proposals_.erase(std::remove_if(proposals_.begin(), proposals_.end(), [done](const Proposal& x) { return x.id == done; }), proposals_.end());
            for (ConnectionId id : players) {
                clients_[id].proposal = 0;
                if (clients_[id].readyCheck) transport_.Send(id, msg::ReadyCheck(accepted, humans, true, 0));
            }
            return StartRoom(mode, players);
        }
        const int remaining = now_ < p.deadlineMs ? static_cast<int>(p.deadlineMs - now_) : 0;
        for (ConnectionId id : p.players) {
            if (clients_[id].readyCheck) transport_.Send(id, msg::ReadyCheck(accepted, humans, clients_[id].accepted, remaining));
        }
    }

    void Decline(ConnectionId id) {
        const std::uint64_t proposal = clients_[id].proposal;
        if (proposal != 0) FailProposal(proposal, {id});
    }

    // A ready check failed: the decliners leave the queue (queue_status reason "declined") and so do their party mates ("party_declined"); everyone
    // else is searching again in their old place (reason "requeued").
    void FailProposal(std::uint64_t proposalId, const std::vector<ConnectionId>& decliners) {
        Proposal* found = FindProposal(proposalId);
        if (found == nullptr) return;
        const std::vector<ConnectionId> players = found->players;
        proposals_.erase(std::remove_if(proposals_.begin(), proposals_.end(), [&](const Proposal& x) { return x.id == proposalId; }), proposals_.end());
        std::vector<std::uint64_t> outGroups;
        for (ConnectionId d : decliners) {
            if (clients_.count(d) && clients_[d].group != 0) outGroups.push_back(clients_[d].group);
        }
        for (ConnectionId id : players) {
            if (!clients_.count(id)) continue;
            Client& c = clients_[id];
            c.proposal = 0;
            c.accepted = false;
            const bool declined = std::find(decliners.begin(), decliners.end(), id) != decliners.end();
            const bool mate = !declined && c.group != 0 && std::find(outGroups.begin(), outGroups.end(), c.group) != outGroups.end();
            c.searching = !declined && !mate && !c.closing;
            if (!c.searching) c.group = 0;
            if (!c.closing) SendStatus(id, declined ? "declined" : (mate ? "party_declined" : "requeued"));
            PresenceChanged(c.account);
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
            gc.seatIcons.push_back(a != nullptr ? a->icon : 0);
            if (a != nullptr && cfg_.progression != nullptr) gc.seatBorders.push_back(cfg_.accounts->Equipped(*a, "border"));
            room->accounts.push_back(a != nullptr ? clients_[id].account : std::string());
        }
        if (!gc.seatBorders.empty()) gc.seatBorders.resize(players.size());
        room->game = std::make_unique<GameServer>(gc, data_, *room->transport);
        const std::uint64_t roomId = room->id;
        room->game->SetMatchResultHandler([this, roomId](const MatchResult& result) { Record(roomId, result); });
        Room& r = *room;
        rooms_.push_back(std::move(room));
        for (ConnectionId id : players) {
            Client& c = clients_[id];
            c.searching = false;
            c.group = 0;
            c.room = r.id;
            if (!c.readyCheck) transport_.Send(id, msg::MatchFound(mode, humans, gc.bots));   // (a ready check already announced it)
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
        const auto error = [&](const char* code, const std::string& detail) { transport_.Send(id, msg::Error(code, detail, cmd.hasId, cmd.id)); };
        switch (cmd.type) {
            case CommandType::Register:
            case CommandType::Login:
            case CommandType::ResumeSession: {
                if (c.room != 0) return error("in_match", "you are in a match");
                std::string key, session = cmd.session;
                AccountStore::Result r = AccountStore::Result::Ok;
                if (cmd.type == CommandType::Register) r = store->Register(cmd.username, cmd.password, WallClock(), key, session);
                else if (cmd.type == CommandType::Login) r = store->Login(cmd.username, cmd.password, key, session);
                else r = store->Resume(cmd.session, key);
                if (r != AccountStore::Result::Ok) return fail(r);
                const std::string previous = c.account;
                if (c.proposal != 0) Decline(id);
                if (c.searching) StopSearch(id, "cancelled");
                for (auto& [otherId, other] : clients_) {   // one connection per account: the older one is logged out
                    if (otherId != id && other.account == key) {
                        transport_.Send(otherId, msg::LoggedOut("elsewhere"));
                        if (other.proposal != 0) Decline(otherId);
                        if (other.searching) StopSearch(otherId, "cancelled");
                        other.account.clear();
                        other.session.clear();
                    }
                }
                if (!previous.empty() && previous != key) { c.account.clear(); LeaveParty(previous, false); }
                c.account = key;
                c.session = session;
                const Account* a = store->Find(key);
                transport_.Send(id, msg::Auth(*a, session));
                transport_.Send(id, msg::Profile(*a, true, store));
                SendFriends(id);
                SendProgress(key);
                SendPartyTo(id, PartyOfKey(key));
                DeliverOffline(key);
                if (!previous.empty() && previous != key) PresenceChanged(previous);
                return PresenceChanged(key);
            }
            default: break;
        }
        if (c.account.empty()) return error("not_logged_in", "log in first");
        const std::string me = c.account;
        switch (cmd.type) {
            case CommandType::Logout: {
                if (c.proposal != 0) Decline(id);
                if (c.searching) StopSearch(id, "cancelled");
                store->Logout(me, c.session);
                c.account.clear();
                c.session.clear();
                LeaveParty(me, false);
                transport_.Send(id, msg::LoggedOut("logout"));
                return PresenceChanged(me);
            }
            case CommandType::GetProfile: {
                const Account* a = store->Find(cmd.username.empty() ? me : cmd.username);
                if (a == nullptr) return fail(AccountStore::Result::UnknownUser);
                return transport_.Send(id, msg::Profile(*a, Status(AccountStore::Key(a->name)) != "offline", store));
            }
            case CommandType::GetFriends: return SendFriends(id);
            case CommandType::SetIcon: {   // (revision 9) any champion of the pool
                const ChampionDefinition* def = data_.champions != nullptr ? data_.champions->Find(static_cast<ChampionId>(cmd.icon)) : nullptr;
                if (def == nullptr || !def->IsPooled()) return error("unknown_icon", "icon must be the id of a champion");
                store->SetIcon(me, cmd.icon);
                transport_.Send(id, msg::Profile(*store->Find(me), true, store));
                return PresenceChanged(me);
            }
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
            // ---- revision 10 ----
            case CommandType::PartyInvite:
            case CommandType::PartyAccept:
            case CommandType::PartyDecline:
            case CommandType::PartyLeave:
            case CommandType::PartyKick:
            case CommandType::PartyPromote:
            case CommandType::PartyMode: return PartyCommand(id, cmd);
            case CommandType::Chat:
            case CommandType::GetChat: return ChatCommand(id, cmd);
            case CommandType::GetMatch: {
                const MatchRecord* m = store->FindMatch(cmd.matchId);
                if (m == nullptr) return fail(AccountStore::Result::UnknownMatch);
                return transport_.Send(id, msg::MatchDetail(*m));
            }
            case CommandType::GetLadder: return SendLadder(id);
            case CommandType::GetStore:
            case CommandType::BuyItem:
            case CommandType::EquipCosmetic:
            case CommandType::GetMissions: {
                if (cfg_.progression == nullptr) return error("no_progression", "this server has no store or missions (data/progression.json)");
                if (cmd.type == CommandType::GetMissions) return transport_.Send(id, msg::Missions(store->Missions(me, WallClock()), WallClock()));
                if (cmd.type == CommandType::BuyItem || cmd.type == CommandType::EquipCosmetic) {
                    const AccountStore::Result r = cmd.type == CommandType::BuyItem ? store->Buy(me, cmd.storeItem) : store->Equip(me, cmd.storeItem);
                    if (r != AccountStore::Result::Ok) return fail(r);
                    transport_.Send(id, msg::Wallet(*store->Find(me), *store));
                    if (cmd.type == CommandType::EquipCosmetic) {   // friends and the party see the new border / title
                        transport_.Send(id, msg::Profile(*store->Find(me), true, store));
                        PresenceChanged(me);
                    }
                }
                return transport_.Send(id, msg::Store(*store->Find(me), *store));
            }
            case CommandType::ChangePassword: {
                const AccountStore::Result r = store->ChangePassword(me, cmd.password, cmd.newPassword, c.session);
                if (r != AccountStore::Result::Ok) return fail(r);
                return transport_.Send(id, msg::Done("change_password", cmd.hasId, cmd.id));
            }
            default: return;
        }
    }

    std::uint64_t WallClock() const { return cfg_.wallClockMs ? cfg_.wallClockMs() : 0; }

    // The connection an account is logged in on (0 = offline).
    ConnectionId ConnOf(const std::string& key) const {
        if (key.empty()) return 0;
        for (const auto& [id, c] : clients_) {
            if (c.account == key && !c.closing) return id;
        }
        return 0;
    }

    std::string Status(const std::string& key) const {
        std::string best = "offline";
        for (const auto& [id, c] : clients_) {
            if (c.account != key || c.closing) continue;
            if (c.room != 0) return "in_match";
            best = c.searching || c.proposal != 0 ? "searching" : "online";
        }
        return best;
    }

    void SendFriends(ConnectionId id) {
        const Account* a = cfg_.accounts != nullptr ? cfg_.accounts->Find(clients_[id].account) : nullptr;
        if (a == nullptr) return;
        std::vector<msg::FriendInfo> list;
        for (const std::string& k : a->friends) {
            const Account* f = cfg_.accounts->Find(k);
            if (f == nullptr) continue;
            msg::FriendInfo info;
            info.name = f->name;
            info.status = Status(k);
            info.points = f->points;
            info.icon = f->icon;
            if (cfg_.progression != nullptr) {
                info.level = cfg_.accounts->LevelOf(*f);
                info.border = cfg_.accounts->Equipped(*f, "border");
                info.title = cfg_.accounts->Equipped(*f, "title");
            }
            if (const Party* p = PartyOfKey(k)) info.partySize = static_cast<int>(p->members.size());
            list.push_back(info);
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

    // A player's status changed (logged in / out, queued, in a match, back): their online friends get a fresh list, their party a fresh roster.
    void PresenceChanged(const std::string& key) {
        if (key.empty() || cfg_.accounts == nullptr) return;
        const Account* a = cfg_.accounts->Find(key);
        if (a == nullptr) return;
        for (const std::string& f : a->friends) SendFriendsToAccount(f);
        if (const Party* p = PartyOfKey(key)) SendParty(p->id);
    }

    // ---- progression (revision 10) ----------------------------------------------------------------------------------------------------------------------

    void SendProgress(const std::string& key) {
        if (cfg_.progression == nullptr || cfg_.accounts == nullptr) return;
        const ConnectionId id = ConnOf(key);
        if (id == 0) return;
        transport_.Send(id, msg::Wallet(*cfg_.accounts->Find(key), *cfg_.accounts));
        transport_.Send(id, msg::Missions(cfg_.accounts->Missions(key, WallClock()), WallClock()));
    }

    void SendLadder(ConnectionId id) {
        const std::vector<const Account*> all = cfg_.accounts->Ladder();
        std::vector<msg::LadderEntry> top;
        int you = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            const Account& a = *all[i];
            if (AccountStore::Key(a.name) == clients_[id].account) you = static_cast<int>(i) + 1;
            if (static_cast<int>(top.size()) >= kLadderShown) continue;
            msg::LadderEntry e;
            e.name = a.name;
            e.icon = a.icon;
            e.points = a.points;
            if (cfg_.progression != nullptr) {
                e.level = cfg_.accounts->LevelOf(a);
                e.border = cfg_.accounts->Equipped(a, "border");
                e.title = cfg_.accounts->Equipped(a, "title");
            }
            top.push_back(e);
        }
        transport_.Send(id, msg::Ladder(top, you, static_cast<int>(all.size())));
    }

    // ---- parties (revision 10) -----------------------------------------------------------------------------------------------------------------------

    Party* PartyOfKey(const std::string& key) {
        if (key.empty()) return nullptr;
        for (auto& [id, p] : parties_) {
            if (std::find(p.members.begin(), p.members.end(), key) != p.members.end()) return &p;
        }
        return nullptr;
    }
    const Party* PartyOfKey(const std::string& key) const { return const_cast<Impl*>(this)->PartyOfKey(key); }

    msg::PartyInfo InfoOf(const Party* p) const {
        msg::PartyInfo info;
        if (p == nullptr) return info;
        info.id = p->id;
        info.mode = p->mode;
        for (const std::string& k : p->members) {
            const Account* a = cfg_.accounts->Find(k);
            if (a == nullptr) continue;
            if (info.leader.empty()) info.leader = a->name;
            msg::PartyMember m;
            m.name = a->name;
            m.icon = a->icon;
            m.points = a->points;
            m.status = Status(k);
            if (cfg_.progression != nullptr) {
                m.level = cfg_.accounts->LevelOf(*a);
                m.border = cfg_.accounts->Equipped(*a, "border");
                m.title = cfg_.accounts->Equipped(*a, "title");
            }
            info.members.push_back(m);
        }
        for (const auto& [k, until] : p->invited) {
            if (const Account* a = cfg_.accounts->Find(k)) info.invited.push_back(a->name);
        }
        return info;
    }

    void SendPartyTo(ConnectionId id, const Party* p) {
        if (id != 0) transport_.Send(id, msg::Party(InfoOf(p)));
    }

    void SendParty(std::uint64_t partyId) {
        auto it = parties_.find(partyId);
        if (it == parties_.end()) return;
        const msg::PartyInfo info = InfoOf(&it->second);
        const std::string text = msg::Party(info);
        for (const std::string& k : it->second.members) {
            if (const ConnectionId c = ConnOf(k)) transport_.Send(c, text);
        }
    }

    // A party of one with no open invites is no party.
    void Tidy(std::uint64_t partyId) {
        auto it = parties_.find(partyId);
        if (it == parties_.end()) return;
        if (it->second.members.empty() || (it->second.members.size() == 1 && it->second.invited.empty())) {
            const std::vector<std::string> left = it->second.members;
            parties_.erase(it);
            for (const std::string& k : left) {
                SendPartyTo(ConnOf(k), nullptr);
                PresenceChanged(k);
            }
            return;
        }
        SendParty(partyId);
    }

    // The party stops searching (somebody joined, left or was kicked): its members are idle again.
    void HaltPartySearch(const Party& p) {
        for (const std::string& k : p.members) {
            const ConnectionId c = ConnOf(k);
            if (c != 0 && clients_[c].searching) { StopSearch(c, "cancelled"); return; }   // (stops the whole group)
        }
    }

    void LeaveParty(const std::string& key, bool kicked) {
        Party* p = PartyOfKey(key);
        if (p == nullptr) return;
        HaltPartySearch(*p);
        const std::uint64_t pid = p->id;
        p->members.erase(std::remove(p->members.begin(), p->members.end(), key), p->members.end());
        if (const ConnectionId c = ConnOf(key)) {
            SendPartyTo(c, nullptr);
            if (kicked) transport_.Send(c, msg::Error("kicked", "you were removed from the party"));
        }
        Tidy(pid);
    }

    void PartyCommand(ConnectionId id, const Command& cmd) {
        AccountStore& store = *cfg_.accounts;
        Client& c = clients_[id];
        const std::string me = c.account;
        const auto error = [&](const char* code, const std::string& detail) { transport_.Send(id, msg::Error(code, detail, cmd.hasId, cmd.id)); };
        Party* mine = PartyOfKey(me);
        const std::string otherKey = AccountStore::Key(cmd.username);
        switch (cmd.type) {
            case CommandType::PartyInvite: {
                const Account* other = store.Find(otherKey);
                if (other == nullptr) return error("unknown_user", "There is no player with that name.");
                if (otherKey == me) return error("self", "That is you.");
                if (store.Find(me)->friends.count(otherKey) == 0) return error("not_friends", "You can only invite friends.");
                if (ConnOf(otherKey) == 0) return error("not_online", other->name + " is offline.");
                if (mine != nullptr && std::find(mine->members.begin(), mine->members.end(), otherKey) != mine->members.end()) return error("already_in_party", other->name + " is already in your party.");
                if (mine != nullptr && static_cast<int>(mine->members.size() + mine->invited.size()) >= cfg_.match.seats) return error("party_full", "The party is full.");
                if (mine == nullptr) {
                    Party p;
                    p.id = ++partyIds_;
                    p.members.push_back(me);
                    parties_[p.id] = p;
                    mine = &parties_[p.id];
                }
                mine->invited[otherKey] = now_ + static_cast<std::uint64_t>(cfg_.inviteMs);
                const Account* a = store.Find(me);
                transport_.Send(ConnOf(otherKey), msg::PartyInviteMsg(a->name, a->icon, static_cast<int>(mine->members.size()), mine->mode));
                SendParty(mine->id);
                return;
            }
            case CommandType::PartyAccept:
            case CommandType::PartyDecline: {
                Party* theirs = PartyOfKey(otherKey);
                if (theirs == nullptr || theirs->invited.count(me) == 0 || theirs->invited[me] < now_) return error("no_invite", "That invite is no longer open.");
                const std::uint64_t pid = theirs->id;
                theirs->invited.erase(me);
                if (cmd.type == CommandType::PartyDecline) return Tidy(pid);
                if (c.room != 0) { Tidy(pid); return error("in_match", "Finish your match first."); }
                if (c.proposal != 0) { Tidy(pid); return error("in_ready_check", "Answer your ready check first."); }
                if (static_cast<int>(theirs->members.size()) >= cfg_.match.seats) { Tidy(pid); return error("party_full", "The party is full."); }
                if (c.searching) StopSearch(id, "cancelled");
                if (mine != nullptr) LeaveParty(me, false);
                Party& joined = parties_[pid];
                HaltPartySearch(joined);
                joined.members.push_back(me);
                SendParty(pid);
                for (const std::string& k : joined.members) PresenceChanged(k);
                return;
            }
            case CommandType::PartyLeave:
                if (mine == nullptr) return error("not_in_party", "You are not in a party.");
                LeaveParty(me, false);
                return PresenceChanged(me);
            case CommandType::PartyKick:
            case CommandType::PartyPromote: {
                if (mine == nullptr) return error("not_in_party", "You are not in a party.");
                if (mine->members.front() != me) return error("not_leader", "Only the party leader can do that.");
                const auto at = std::find(mine->members.begin(), mine->members.end(), otherKey);
                if (at == mine->members.end() || otherKey == me) return error("not_in_party", "They are not in your party.");
                if (cmd.type == CommandType::PartyKick) {
                    LeaveParty(otherKey, true);
                    return PresenceChanged(otherKey);
                }
                mine->members.erase(at);
                mine->members.insert(mine->members.begin(), otherKey);
                return SendParty(mine->id);
            }
            case CommandType::PartyMode:
                if (mine == nullptr) return error("not_in_party", "You are not in a party.");
                if (mine->members.front() != me) return error("not_leader", "Only the party leader can choose the mode.");
                if (c.searching) return error("searching", "Leave the queue first.");
                mine->mode = cmd.queueMode;
                return SendParty(mine->id);
            default: return;
        }
    }

    void ExpireInvites() {
        std::vector<std::uint64_t> changed;
        for (auto& [pid, p] : parties_) {
            const std::size_t before = p.invited.size();
            for (auto it = p.invited.begin(); it != p.invited.end();) {
                if (it->second < now_) it = p.invited.erase(it);
                else ++it;
            }
            if (p.invited.size() != before) changed.push_back(pid);
        }
        for (std::uint64_t pid : changed) Tidy(pid);
    }

    // ---- chat (revision 10) --------------------------------------------------------------------------------------------------------------------------

    static std::pair<std::string, std::string> PairOf(const std::string& a, const std::string& b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

    void ChatCommand(ConnectionId id, const Command& cmd) {
        AccountStore& store = *cfg_.accounts;
        const std::string me = clients_[id].account;
        const Account* self = store.Find(me);
        const auto error = [&](const char* code, const std::string& detail) { transport_.Send(id, msg::Error(code, detail, cmd.hasId, cmd.id)); };
        if (cmd.type == CommandType::GetChat) {
            const std::string other = AccountStore::Key(cmd.username);
            const Account* a = store.Find(other);
            if (a == nullptr) return error("unknown_user", "There is no player with that name.");
            if (self->friends.count(other) == 0) return error("not_friends", "You can only chat with friends.");
            std::vector<msg::ChatLine> lines;
            auto it = chats_.find(PairOf(me, other));
            if (it != chats_.end()) lines.assign(it->second.begin(), it->second.end());
            return transport_.Send(id, msg::ChatHistory(a->name, lines));
        }
        const std::string text = CleanChat(cmd.text);
        if (text.empty()) return error("empty_text", "Type something first.");
        const std::uint64_t when = WallClock();
        if (cmd.to == "party") {
            const Party* p = PartyOfKey(me);
            if (p == nullptr || p->members.size() < 2) return error("not_in_party", "You are not in a party.");
            const std::string line = msg::ChatMsg("party", self->name, "party", text, when, false);
            for (const std::string& k : p->members) {
                if (const ConnectionId c = ConnOf(k)) transport_.Send(c, line);
            }
            return;
        }
        const std::string other = AccountStore::Key(cmd.to);
        const Account* a = store.Find(other);
        if (a == nullptr) return error("unknown_user", "There is no player with that name.");
        if (other == me) return error("self", "That is you.");
        if (self->friends.count(other) == 0) return error("not_friends", "You can only chat with friends.");
        std::deque<msg::ChatLine>& log = chats_[PairOf(me, other)];
        log.push_back({self->name, text, when});
        while (static_cast<int>(log.size()) > cfg_.chatKept) log.pop_front();
        transport_.Send(id, msg::ChatMsg("whisper", self->name, a->name, text, when, false));
        if (const ConnectionId c = ConnOf(other)) return transport_.Send(c, msg::ChatMsg("whisper", self->name, a->name, text, when, false));
        std::deque<msg::ChatLine>& waiting = offline_[other];   // they read it when they log in
        waiting.push_back({self->name, text, when});
        while (static_cast<int>(waiting.size()) > cfg_.offlineChatKept) waiting.pop_front();
    }

    void DeliverOffline(const std::string& key) {
        auto it = offline_.find(key);
        if (it == offline_.end()) return;
        const ConnectionId c = ConnOf(key);
        const Account* me = cfg_.accounts->Find(key);
        if (c == 0 || me == nullptr) return;
        for (const msg::ChatLine& l : it->second) transport_.Send(c, msg::ChatMsg("whisper", l.from, me->name, l.text, l.timeMs, true));
        offline_.erase(it);
    }

    // A match of this room ended: every logged-in player's history, stats and (ranked) LP, coins, XP and missions; the whole match is kept as a record;
    // every player gets their new profile and the post-game screen.
    void Record(std::uint64_t roomId, const MatchResult& result) {
        if (cfg_.accounts == nullptr) return;
        AccountStore& store = *cfg_.accounts;
        const Room* room = nullptr;
        for (const auto& r : rooms_) { if (r->id == roomId) room = r.get(); }
        if (room == nullptr) return;
        // the record: every seat, 1st place first
        MatchRecord record;
        record.timeMs = WallClock();
        record.mode = ToString(room->mode);
        record.rounds = result.rounds;
        std::vector<std::pair<int, std::size_t>> order;
        for (std::size_t i = 0; i < result.seats.size(); ++i) order.emplace_back(result.seats[i].placement > 0 ? result.seats[i].placement : 99, i);
        std::sort(order.begin(), order.end());
        std::vector<std::string> lobby;
        for (const auto& [placement, i] : order) {
            const MatchResultSeat& s = result.seats[i];
            MatchRecordSeat seat;
            seat.name = s.name;
            seat.bot = s.bot;
            seat.placement = s.placement;
            seat.level = s.level;
            seat.round = s.round;
            for (const MatchResultUnit& u : s.board) seat.board.push_back({u.champion, u.star, u.items});
            const std::string key = i < room->accounts.size() ? room->accounts[i] : std::string();
            if (const Account* a = key.empty() ? nullptr : store.Find(key)) {
                seat.icon = a->icon;
                if (cfg_.progression != nullptr) { seat.border = store.Equipped(*a, "border"); seat.title = store.Equipped(*a, "title"); }
            }
            lobby.push_back(s.name);
            record.seats.push_back(std::move(seat));
        }
        const std::uint64_t matchId = store.AddMatch(record);
        const MatchRecord* kept = store.FindMatch(matchId);
        for (std::size_t i = 0; i < room->accounts.size() && i < result.seats.size(); ++i) {
            const std::string& key = room->accounts[i];
            if (key.empty() || store.Find(key) == nullptr) continue;
            const MatchResultSeat& s = result.seats[i];
            HistoryEntry h;
            h.timeMs = WallClock();
            h.mode = ToString(room->mode);
            h.placement = s.placement;
            h.level = s.level;
            h.round = s.round;
            h.matchId = matchId;
            for (const MatchResultUnit& u : s.board) h.board.push_back({u.champion, u.star, u.items});
            h.players = lobby;
            msg::PostGameInfo post;
            post.record = kept;
            post.placement = s.placement;
            if (const Progression* prog = cfg_.progression) {
                h.coins = prog->CoinsFor(h.mode, s.placement);
                h.xp = prog->XpFor(h.mode, s.placement);
                post.levelBefore = store.LevelOf(*store.Find(key));
                const int coinsBefore = store.Find(key)->coins;
                store.Grant(key, h.coins, h.xp);
                MissionMatch mm;
                mm.mode = h.mode;
                mm.placement = s.placement;
                mm.level = s.level;
                mm.units = static_cast<int>(s.board.size());
                for (const MatchResultUnit& u : s.board) {
                    mm.items += static_cast<int>(u.items.size());
                    mm.threeStar = mm.threeStar || u.star >= 3;
                    const ChampionDefinition* def = data_.champions != nullptr ? data_.champions->Find(static_cast<ChampionId>(u.champion)) : nullptr;
                    if (def != nullptr && def->IsPooled()) mm.maxCost = std::max(mm.maxCost, def->cost);
                }
                post.missions = store.CountMissions(key, WallClock(), mm);
                const Account& after = *store.Find(key);
                int level = 1, into = 0, need = 1;
                prog->LevelOf(after.xp, level, into, need);
                post.level = level;
                post.xpInto = into;
                post.xpNext = need;
                post.coinsTotal = after.coins;
                post.coins = after.coins - coinsBefore;   // match + level-ups + missions
                post.xp = h.xp;
                for (const AccountStore::MissionUpdate& u : post.missions) post.xp += u.justDone ? u.mission->xp : 0;
            }
            store.RecordMatch(key, std::move(h));
            const Account& a = *store.Find(key);
            post.lpChange = a.history.empty() ? 0 : a.history.front().lpChange;
            post.points = a.points;
            for (const auto& [id, c] : clients_) {
                if (c.account != key || c.closing) continue;
                transport_.Send(id, msg::Profile(a, true, &store));
                if (cfg_.progression != nullptr) {
                    transport_.Send(id, msg::Wallet(a, store));
                    transport_.Send(id, msg::Missions(store.Missions(key, WallClock()), WallClock()));
                }
                transport_.Send(id, msg::PostGame(post, WallClock()));
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
    std::vector<Proposal> proposals_;
    std::uint64_t proposalIds_ = 0;
    std::map<std::uint64_t, Party> parties_;   // (revision 10) by id
    std::uint64_t partyIds_ = 0;
    std::map<std::pair<std::string, std::string>, std::deque<msg::ChatLine>> chats_;   // (revision 10) a friend pair's recent whispers
    std::map<std::string, std::deque<msg::ChatLine>> offline_;                          // (revision 10) whispers waiting for an offline friend
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
