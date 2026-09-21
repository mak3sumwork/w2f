#include "w2f/net/GameServer.h"

#include <algorithm>
#include <random>
#include <unordered_map>

#include "w2f/AIBotController.h"
#include "w2f/CombatSimulator.h"
#include "w2f/Json.h"
#include "w2f/net/JsonWriter.h"
#include "w2f/net/Encoding.h"
#include "w2f/net/Messages.h"

namespace w2f::net {

namespace {

constexpr std::uint16_t kCloseNormal = 1000;
constexpr std::uint16_t kClosePolicy = 1008;
constexpr std::uint16_t kCloseTryLater = 1013;
constexpr std::uint16_t kCloseReplaced = 4001;   // application-defined: a newer connection took this seat
constexpr std::size_t kTokenHexChars = 32;

struct Bucket {
    long long milli = 0;
    std::uint64_t last = 0;
    bool started = false;
    bool Take(std::uint64_t now, int cost, int burst, int refillPerSecond) {
        const long long cap = static_cast<long long>(burst) * 1000;
        if (!started) { milli = cap; last = now; started = true; }
        if (now > last) milli = std::min(cap, milli + static_cast<long long>(now - last) * refillPerSecond);   // ms x tokens/s = milli-tokens
        last = std::max(last, now);
        if (milli < static_cast<long long>(cost) * 1000) return false;
        milli -= static_cast<long long>(cost) * 1000;
        return true;
    }
};

struct Conn {
    int seat = -1;
    Bucket bucket;
    int violations = 0;
    bool closing = false;
};

struct Seat {
    bool bot = false;        // an AI player: never given to a connection, never has a token
    std::string token;       // empty = free (lobby only: once a match runs, a seat is never freed)
    ConnectionId conn = 0;   // 0 = nobody connected
    std::string lastPrivate; // what the owner was last told, to send only real changes
};

int CommandCost(CommandType t) {
    switch (t) {
        case CommandType::GetState: return 3;
        case CommandType::GetFight: return 5;
        case CommandType::GetCatalog: return 5;
        default: return 1;
    }
}

}  // namespace

class GameServer::Impl : public IMatchListener {
public:
    Impl(const GameServerConfig& config, const GameData& data, IServerTransport& transport)
        : cfg_(config), seats_(static_cast<std::size_t>(config.seats)), data_(data), transport_(transport) {
        for (int i = cfg_.seats - cfg_.bots; i < cfg_.seats; ++i) seats_[static_cast<std::size_t>(i)].bot = true;
        if (!cfg_.entropy) {
            cfg_.entropy = [] {
                static std::random_device device;
                return (static_cast<std::uint64_t>(device()) << 32) ^ device();
            };
        }
    }

    // ---- connections --------------------------------------------------------------------------

    void OnConnect(ConnectionId id, std::string_view token, std::uint64_t) {
        conns_[id] = Conn{};
        int seat = -1;
        bool reconnected = false;

        if (token.size() == kTokenHexChars && IsLowerHex(token)) {
            for (std::size_t i = 0; i < seats_.size(); ++i) {
                if (!seats_[i].token.empty() && seats_[i].token == token) seat = static_cast<int>(i);
            }
        }
        if (seat >= 0) {
            reconnected = true;
            Seat& s = seats_[static_cast<std::size_t>(seat)];
            if (s.conn != 0 && s.conn != id) {   // the same player connecting again: the newer connection wins
                SendTo(s.conn, msg::Error("replaced", "this seat was taken over by a newer connection"));
                transport_.Close(s.conn, kCloseReplaced, "replaced by a newer connection");
                auto old = conns_.find(s.conn);
                if (old != conns_.end()) { old->second.seat = -1; old->second.closing = true; }
            }
        } else {
            if (state_ != State::Lobby) {
                SendTo(id, msg::Error(state_ == State::Running ? "match_in_progress" : "match_finished",
                                      "a match is already running; only its players can reconnect (with their token)"));
                Reject(id, kCloseTryLater, "match in progress");
                return;
            }
            for (std::size_t i = 0; i < seats_.size(); ++i) {
                if (seats_[i].token.empty() && !seats_[i].bot) { seat = static_cast<int>(i); break; }
            }
            if (seat < 0) {   // cannot happen (the match starts the moment the last seat fills), but never assume
                SendTo(id, msg::Error("server_full", "no free seat"));
                Reject(id, kCloseTryLater, "server full");
                return;
            }
            seats_[static_cast<std::size_t>(seat)].token = NewToken();
            seats_[static_cast<std::size_t>(seat)].lastPrivate.clear();
        }

        Seat& s = seats_[static_cast<std::size_t>(seat)];
        s.conn = id;
        conns_[id].seat = seat;
        SendTo(id, msg::Welcome(static_cast<PlayerId>(seat), s.token, reconnected, ConnectedCount(), cfg_.seats, cfg_.bots, state_ != State::Lobby));

        if (state_ == State::Lobby) {
            BroadcastLobby();
            if (TakenSeats() == cfg_.seats - cfg_.bots) StartMatch();
        } else {
            FullSync(seat);
        }
    }

    void OnDisconnect(ConnectionId id) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        const int seat = it->second.seat;
        conns_.erase(it);
        if (seat < 0) return;
        Seat& s = seats_[static_cast<std::size_t>(seat)];
        if (s.conn != id) return;   // already handed to a newer connection
        s.conn = 0;
        if (state_ == State::Lobby) {   // before the match nobody holds a seat they are not connected to
            s.token.clear();
            s.lastPrivate.clear();
            BroadcastLobby();
        }
        // During a match the seat stays reserved for its token: the match does not wait for anyone, but the player can come back.
    }

    // ---- messages -----------------------------------------------------------------------------

    void OnMessage(ConnectionId id, std::string_view text, std::uint64_t now) {
        auto it = conns_.find(id);
        if (it == conns_.end() || it->second.closing) return;
        Conn& conn = it->second;

        if (!conn.bucket.Take(now, 1, cfg_.rateBurst, cfg_.rateRefillPerSecond)) return Violation(id, "rate_limited", "too many messages, slow down");
        const ParseResult parsed = ParseCommand(text);
        if (!parsed.ok) return Violation(id, parsed.error.code, parsed.error.detail, parsed.hasId, parsed.id);
        const Command& cmd = parsed.command;
        if (CommandCost(cmd.type) > 1 && !conn.bucket.Take(now, CommandCost(cmd.type) - 1, cfg_.rateBurst, cfg_.rateRefillPerSecond)) {
            return Violation(id, "rate_limited", "too many messages, slow down", cmd.hasId, cmd.id);
        }

        if (cmd.type == CommandType::Ping) return SendTo(id, msg::Pong(cmd.hasId, cmd.id));
        if (cmd.type == CommandType::GetCatalog) return SendTo(id, Catalog());
        if (!match_ || conn.seat < 0) return Violation(id, "not_in_match", "no match is running yet", cmd.hasId, cmd.id);
        const PlayerId player = static_cast<PlayerId>(conn.seat);

        if (cmd.type == CommandType::GetState) {
            SendTo(id, msg::PrivateState(*match_, player));
            SendTo(id, msg::PublicState(*match_));
            return;
        }
        if (cmd.type == CommandType::GetFight) {
            if (cmd.fightIndex >= static_cast<int>(fightJson_.size())) {
                return Violation(id, "no_such_fight", "there is no fight " + std::to_string(cmd.fightIndex) + " this round", cmd.hasId, cmd.id);
            }
            SendTo(id, fightJson_[static_cast<std::size_t>(cmd.fightIndex)]);
            return;
        }

        ActionResult result = ActionResult::Ok;
        switch (cmd.type) {
            case CommandType::BuyUnit: result = match_->TryBuyShopUnit(player, static_cast<std::size_t>(cmd.shopIndex)); break;
            case CommandType::RerollShop: result = match_->TryRerollShop(player); break;
            case CommandType::PickGift: result = match_->TryPickGift(player, static_cast<std::size_t>(cmd.giftIndex)); break;
            case CommandType::BuyXp: result = match_->TryBuyXp(player); break;
            case CommandType::SellUnit: result = match_->TrySellUnit(player, cmd.unit); break;
            case CommandType::MoveUnit: result = match_->TryMoveUnit(player, cmd.unit, cmd.location, cmd.x, cmd.y); break;
            case CommandType::EquipItem: result = match_->TryEquipItem(player, cmd.unit, cmd.item); break;
            case CommandType::UnequipItem: result = match_->TryUnequipItem(player, cmd.unit, cmd.slot); break;
            case CommandType::GetState:
            case CommandType::GetFight:
            case CommandType::GetCatalog:
            case CommandType::Ping: break;   // handled above
        }
        if (observer_) observer_(tick_, player, cmd, result);
        // The answer first, then whatever the action caused (events the engine fired, then any state that changed).
        outbox_.insert(outbox_.begin(), Out{conn.seat, msg::Result(cmd, result)});
        FlushOutbox();
        SyncAfterEngineCall();
    }

    // ---- time ---------------------------------------------------------------------------------

    void Tick() {
        if (state_ == State::Lobby) return;
        ++tick_;
        if (state_ == State::Running) {
            match_->Tick();
            for (AIBotController& bot : bots_) bot.Tick(*match_);   // after the engine's tick, so a bot sees the phase that just began
            if (dirty_) SyncAfterEngineCall();
            if (match_->IsFinished()) {
                state_ = State::Finished;
                finishedTicks_ = 0;
                Broadcast(msg::MatchOver(*match_));
                if (finishedHandler_) finishedHandler_();
            }
            return;
        }
        if (++finishedTicks_ >= cfg_.postMatchTicks) ResetToLobby();
    }

    // ---- IMatchListener: the engine's events become messages -----------------------------------
    // Callbacks run inside the engine's Tick / action: they only serialize (the references are valid for the call) and queue.

    void OnPhaseChanged(MatchPhase, MatchPhase to, int round) override {
        if (to == MatchPhase::Combat) QueueCombat(round);
        combatBatchOpen_ = false;
        Queue(-1, msg::Phase(config_, to, round, 0, match_ != nullptr ? match_->PhaseTicks() : 0, tick_, match_ != nullptr && match_->IsMotherNatureRound(round), match_ != nullptr && match_->IsShopClosed(round)));
    }
    void OnPlayerEliminated(PlayerId p, int placement) override { Queue(-1, msg::PlayerEliminated(p, placement)); }
    void OnMatchEnded(PlayerId) override { dirty_ = true; }   // the final message needs the placements: sent when Tick() returns
    void OnCombatSimulated(int round, const CombatOutcome& o) override {
        if (!combatBatchOpen_) {   // the first fight of a new round: the previous round's fights are history
            combatBatchOpen_ = true;
            fights_.clear();
            fightJson_.clear();
            fightRound_ = round;
        }
        const int index = static_cast<int>(fights_.size());
        fights_.push_back(msg::Summarize(index, o));
        fightJson_.push_back(msg::Combat(round, index, o));
    }
    void OnPlayerDamaged(PlayerId p, int damage, int health) override { Queue(-1, msg::PlayerDamaged(p, damage, health)); }
    void OnPveDrop(PlayerId p, const PveDrop& drop) override { Queue(p, msg::PveDropMsg(drop)); }
    void OnGiftsOffered(PlayerId p, const std::vector<GiftOffer>& offers) override { Queue(p, msg::GiftsOffered(offers, data_.motherNature)); }
    void OnGiftPicked(PlayerId p, int index, const GiftOffer& gift, bool automatic, int goldConverted) override {
        Queue(p, msg::GiftPicked(index, gift, data_.motherNature, automatic, goldConverted));
    }
    void OnAutoSnapshot(int round, const std::vector<std::uint8_t>& bytes) override {
        if (snapshotSink_) snapshotSink_(round, bytes, SeatsJson(round));
    }
    void OnIncomeGranted(PlayerId p, int round, const IncomeBreakdown& income) override { Queue(p, msg::Income(p, round, income)); }
    void OnUnitBought(PlayerId p, const UnitInstance& u, int gold) override { Queue(p, msg::UnitBought(u, gold)); }
    void OnUnitSold(PlayerId p, const UnitInstance& u, int gold) override { Queue(p, msg::UnitSold(u, gold)); }
    void OnUnitMoved(PlayerId p, const UnitMove& m) override { Queue(p, msg::UnitMoved(m)); }
    void OnUnitMerged(PlayerId p, const UnitMerge& m) override { Queue(p, msg::UnitMerged(m)); }
    void OnItemEquipped(PlayerId p, const UnitInstance& u, ItemId item) override { Queue(p, msg::ItemEquipped(u, item)); }
    void OnItemUnequipped(PlayerId p, const UnitInstance& u, ItemId item) override { Queue(p, msg::ItemUnequipped(u, item)); }
    void OnItemsCombined(PlayerId p, const UnitInstance& u, const ItemCombination& c) override { Queue(p, msg::ItemsCombined(u, c)); }
    void OnItemConsumed(PlayerId p, const UnitInstance& u, ItemId item, const std::vector<ItemId>& returned) override { Queue(p, msg::ItemConsumed(u, item, returned)); }

    // ---- accessors ----------------------------------------------------------------------------

    State state_ = State::Lobby;
    std::unique_ptr<MatchManager> match_;
    std::uint64_t tick_ = 0;
    CommandObserver observer_;
    SnapshotSink snapshotSink_;
    MatchFinishedHandler finishedHandler_;
    GameServerConfig cfg_;
    std::unordered_map<ConnectionId, Conn> conns_;
    std::vector<Seat> seats_;

    int MotherNatureEvery() const { return data_.motherNature != nullptr ? config_.match.motherNatureEveryRounds : 0; }

    bool Resume(const std::vector<std::uint8_t>& snapshot, const std::string& seatsJson, std::string* error) { return DoResume(snapshot, seatsJson, error); }

    int ConnectedCount() const {
        int n = 0;
        for (const Seat& s : seats_) n += s.conn != 0 ? 1 : 0;
        return n;
    }

private:
    struct Out {
        int seat;   // -1 = every connected player
        std::string text;
    };

    std::vector<PlayerId> BotSeats() const {
        std::vector<PlayerId> out;
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            if (seats_[i].bot) out.push_back(static_cast<PlayerId>(i));
        }
        return out;
    }

    // The same for every connection: built once, on first use.
    const std::string& Catalog() {
        if (catalog_.empty()) catalog_ = msg::Catalog(*data_.champions, data_.items, data_.traits, data_.encounters, data_.config.combat);
        return catalog_;
    }

    int TakenSeats() const {
        int n = 0;
        for (const Seat& s : seats_) n += s.token.empty() ? 0 : 1;
        return n;
    }

    std::string NewToken() {
        for (;;) {
            std::uint8_t bytes[16];
            for (int i = 0; i < 2; ++i) {
                const std::uint64_t v = cfg_.entropy();
                for (int j = 0; j < 8; ++j) bytes[i * 8 + j] = static_cast<std::uint8_t>(v >> (8 * j));
            }
            std::string token = HexEncode(bytes, 16);
            bool clash = false;
            for (const Seat& s : seats_) clash = clash || s.token == token;
            if (!clash) return token;
        }
    }

    void SendTo(ConnectionId id, std::string_view text) { transport_.Send(id, text); }

    void Reject(ConnectionId id, std::uint16_t code, const char* reason) {
        transport_.Close(id, code, reason);
        auto it = conns_.find(id);
        if (it != conns_.end()) it->second.closing = true;
    }

    void Violation(ConnectionId id, std::string_view code, std::string_view detail, bool hasId = false, long long msgId = 0) {
        SendTo(id, msg::Error(code, detail, hasId, msgId));
        auto it = conns_.find(id);
        if (it != conns_.end() && ++it->second.violations > cfg_.maxViolations) {
            SendTo(id, msg::Error("too_many_violations", "disconnecting"));
            Reject(id, kClosePolicy, "too many protocol violations");
        }
    }

    void Queue(int seat, std::string text) {
        dirty_ = true;
        outbox_.push_back(Out{seat, std::move(text)});
    }
    void Broadcast(std::string text) {
        for (const Seat& s : seats_) {
            if (s.conn != 0) SendTo(s.conn, text);
        }
    }
    void FlushOutbox() {
        std::vector<Out> out;
        out.swap(outbox_);
        for (const Out& o : out) {
            if (o.seat < 0) Broadcast(o.text);
            else if (seats_[static_cast<std::size_t>(o.seat)].conn != 0) SendTo(seats_[static_cast<std::size_t>(o.seat)].conn, o.text);
        }
    }

    void BroadcastLobby() {
        std::vector<PlayerId> taken;
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            if (!seats_[i].token.empty()) taken.push_back(static_cast<PlayerId>(i));
        }
        Broadcast(msg::Lobby(taken, cfg_.seats, cfg_.bots));
    }

    // The fights of a round go out as one summary for everybody, then each fight's log to the players in it (fights are public
    // information; any player can ask for another's with get_fight -- they are not pushed to everyone, to spare bandwidth).
    void QueueCombat(int round) {
        Queue(-1, msg::CombatSummary(round, fights_));
        for (std::size_t i = 0; i < fights_.size(); ++i) {
            const msg::FightSummary& f = fights_[i];
            if (f.home != kInvalidPlayerId) Queue(f.home, fightJson_[i]);
            if (f.away != kInvalidPlayerId && !f.ghost && !f.monsters) Queue(f.away, fightJson_[i]);
        }
    }

    // Sends what changed in the state players can see: each connected player's private state, and the public state to all.
    void SyncAfterEngineCall() {
        FlushOutbox();
        dirty_ = false;
        if (!match_) return;
        for (std::size_t i = 0; i < seats_.size(); ++i) SyncPrivate(static_cast<int>(i));
        SyncPublic();
    }
    void SyncPrivate(int seat) {
        Seat& s = seats_[static_cast<std::size_t>(seat)];
        if (s.conn == 0) return;
        std::string state = msg::PrivateState(*match_, static_cast<PlayerId>(seat));
        if (state == s.lastPrivate) return;
        SendTo(s.conn, state);
        s.lastPrivate = std::move(state);
    }
    void SyncPublic() {
        std::string state = msg::PublicState(*match_);
        if (state == lastPublic_) return;
        Broadcast(state);
        lastPublic_ = std::move(state);
    }

    // A player (re)joining a running match gets everything they need to draw the game as it is right now.
    void FullSync(int seat) {
        Seat& s = seats_[static_cast<std::size_t>(seat)];
        SendTo(s.conn, msg::MatchStarted(config_, cfg_.seats, static_cast<PlayerId>(seat), MotherNatureEvery(), BotSeats()));
        SendTo(s.conn, msg::Phase(config_, match_->Phase(), match_->Round(), match_->TicksInPhase(), match_->PhaseTicks(), tick_, match_->IsMotherNatureRound(), match_->IsShopClosed()));
        s.lastPrivate.clear();
        SyncPrivate(seat);
        SendTo(s.conn, msg::PublicState(*match_));
        if ((match_->Phase() == MatchPhase::Combat || match_->Phase() == MatchPhase::Resolution) && !fights_.empty()) {
            SendTo(s.conn, msg::CombatSummary(fightRound_, fights_));
            for (std::size_t i = 0; i < fights_.size(); ++i) {
                const msg::FightSummary& f = fights_[i];
                if (f.home == seat || (f.away == seat && !f.ghost && !f.monsters)) SendTo(s.conn, fightJson_[i]);
            }
        }
        if (state_ == State::Finished) SendTo(s.conn, msg::MatchOver(*match_));
    }

    void StartMatch() {
        config_ = data_.config;
        config_.match.playerCount = cfg_.seats;
        const std::uint64_t seed = cfg_.seed != 0 ? cfg_.seed : cfg_.entropy();
        std::string error;
        auto simulator = std::make_unique<CombatSimulator>(config_.combat, data_.traits, data_.items);
        match_ = MatchManager::Create(config_, *data_.champions, seed, std::move(simulator), &error, data_.items, data_.encounters, data_.motherNature);
        if (!match_) {   // a bad configuration is the operator's problem; tell the players and start over
            Broadcast(msg::Error("match_failed", "the server could not start the match: " + error));
            ResetToLobby();
            return;
        }
        match_->AddListener(this);
        state_ = State::Running;
        tick_ = 0;
        lastPublic_.clear();
        fights_.clear();
        fightJson_.clear();
        combatBatchOpen_ = false;
        bots_.clear();
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            if (seats_[i].bot) bots_.emplace_back(static_cast<PlayerId>(i), seed, BotProfile{}, data_.traits);
        }
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            if (!seats_[i].bot) SendTo(seats_[i].conn, msg::MatchStarted(config_, cfg_.seats, static_cast<PlayerId>(i), MotherNatureEvery(), BotSeats()));
        }
        match_->Start();
        SyncAfterEngineCall();
    }

    // What a snapshot cannot hold (see GameServer::SetSnapshotSink). Called from inside the engine's tick at the start of Planning, i.e. before the bots
    // have acted this round -- exactly the moment the snapshot is of.
    std::string SeatsJson(int round) const {
        JsonWriter w;
        w.BeginObject();
        w.Field("format", 1);
        w.Field("seats", cfg_.seats);
        w.Field("round", round);
        w.Field("tick", tick_);
        w.Key("players").BeginArray();
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            w.BeginObject();
            w.Field("seat", static_cast<int>(i));
            w.Field("bot", seats_[i].bot);
            w.Field("token", seats_[i].token);
            w.EndObject();
        }
        w.EndArray();
        w.Key("bots").BeginArray();   // (u64 words travel as decimal strings: JSON numbers do not carry 64 bits safely)
        for (const AIBotController& bot : bots_) {
            const AIBotController::State state = bot.GetState();
            w.BeginObject();
            w.Field("seat", static_cast<int>(bot.Player()));
            w.Key("rng").BeginArray();
            for (std::uint64_t word : state.rng.words) w.String(std::to_string(word));
            w.EndArray();
            w.Field("last_acted_round", state.lastActedRound);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
        return w.Take();
    }

    bool DoResume(const std::vector<std::uint8_t>& snapshot, const std::string& seatsJson, std::string* error) {
        const auto fail = [error](const std::string& why) {
            if (error) *error = "cannot resume: " + why;
            return false;
        };
        if (state_ != State::Lobby || match_ != nullptr || !conns_.empty() || TakenSeats() != 0) return fail("the server is not fresh (a match or a connection already exists)");
        json::Value sidecar;
        std::string parseError;
        if (!json::Parse(seatsJson, sidecar, &parseError) || !sidecar.IsObject()) return fail("the seats file is not valid JSON (" + parseError + ")");
        long long format = 0, seats = 0, tick = 0;
        const json::Value* players = sidecar.Find("players");
        const json::Value* bots = sidecar.Find("bots");
        if (sidecar.Find("format") == nullptr || !sidecar.Find("format")->ToInt(format) || format != 1) return fail("unknown seats file format");
        if (sidecar.Find("seats") == nullptr || !sidecar.Find("seats")->ToInt(seats) || seats != cfg_.seats) return fail("the seats file is for a different number of seats (server: " + std::to_string(cfg_.seats) + ")");
        if (sidecar.Find("tick") == nullptr || !sidecar.Find("tick")->ToInt(tick) || tick < 0) return fail("bad tick");
        if (players == nullptr || !players->IsArray() || players->Items().size() != seats_.size() || bots == nullptr || !bots->IsArray()) return fail("bad players / bots");

        std::vector<std::string> tokens(seats_.size());
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            const json::Value& p = players->Items()[i];
            const json::Value* bot = p.Find("bot");
            const json::Value* token = p.Find("token");
            if (bot == nullptr || !bot->IsBool() || token == nullptr || !token->IsString()) return fail("bad player entry");
            if (bot->AsBool() != seats_[i].bot) return fail("the seats file has bots in different seats (start the server with the same --bots)");
            if (!bot->AsBool() && (token->AsString().size() != kTokenHexChars || !IsLowerHex(token->AsString()))) return fail("a human seat without a valid token");
            tokens[i] = bot->AsBool() ? std::string() : token->AsString();
        }
        std::vector<AIBotController> restoredBots;
        for (const json::Value& b : bots->Items()) {
            long long seat = 0, lastActed = 0;
            const json::Value* rng = b.Find("rng");
            if (b.Find("seat") == nullptr || !b.Find("seat")->ToInt(seat) || seat < 0 || seat >= seats || !seats_[static_cast<std::size_t>(seat)].bot) return fail("bad bot entry");
            if (b.Find("last_acted_round") == nullptr || !b.Find("last_acted_round")->ToInt(lastActed) || rng == nullptr || !rng->IsArray() || rng->Items().size() != 4) return fail("bad bot state");
            AIBotController::State state;
            for (std::size_t k = 0; k < 4; ++k) {
                if (!rng->Items()[k].IsString()) return fail("bad bot rng");
                state.rng.words[k] = std::strtoull(rng->Items()[k].AsString().c_str(), nullptr, 10);
            }
            if (!state.rng.IsValid()) return fail("bad bot rng");
            state.lastActedRound = static_cast<int>(lastActed);
            restoredBots.emplace_back(static_cast<PlayerId>(seat), 0, BotProfile{}, data_.traits);
            restoredBots.back().SetState(state);
        }
        if (static_cast<int>(restoredBots.size()) != cfg_.bots) return fail("the seats file has a different number of bots");

        config_ = data_.config;
        config_.match.playerCount = cfg_.seats;
        std::string restoreError;
        auto simulator = std::make_unique<CombatSimulator>(config_.combat, data_.traits, data_.items);
        auto restored = MatchManager::Restore(snapshot, config_, *data_.champions, std::move(simulator), &restoreError, data_.items, data_.encounters, data_.motherNature);
        if (!restored) return fail(restoreError);

        match_ = std::move(restored);
        match_->AddListener(this);
        for (std::size_t i = 0; i < seats_.size(); ++i) seats_[i].token = tokens[i];
        bots_ = std::move(restoredBots);
        state_ = State::Running;
        tick_ = static_cast<std::uint64_t>(tick);
        lastPublic_.clear();
        fights_.clear();
        fightJson_.clear();
        combatBatchOpen_ = false;
        dirty_ = false;
        return true;
    }

    void ResetToLobby() {
        for (const auto& [id, conn] : conns_) {
            (void)conn;
            transport_.Close(id, kCloseNormal, "match over");
        }
        conns_.clear();
        for (Seat& s : seats_) {
            const bool bot = s.bot;
            s = Seat{};
            s.bot = bot;
        }
        bots_.clear();
        match_.reset();
        outbox_.clear();
        fights_.clear();
        fightJson_.clear();
        state_ = State::Lobby;
        dirty_ = false;
    }

    const GameData data_;
    IServerTransport& transport_;
    GameConfig config_;
    std::vector<Out> outbox_;
    std::vector<AIBotController> bots_;   // one per bot seat while a match runs
    std::string catalog_;
    std::string lastPublic_;
    std::vector<msg::FightSummary> fights_;
    std::vector<std::string> fightJson_;
    int fightRound_ = 0;
    bool combatBatchOpen_ = false;
    bool dirty_ = false;
    int finishedTicks_ = 0;
};

GameServer::GameServer(const GameServerConfig& config, const GameData& data, IServerTransport& transport) {
    GameServerConfig c = config;
    c.seats = std::max(2, std::min(kMaxPlayers, c.seats));
    c.bots = std::max(0, std::min(c.seats - 1, c.bots));   // at least one seat stays human, or the lobby would never fill
    impl_ = std::make_unique<Impl>(c, data, transport);
}
GameServer::~GameServer() = default;

void GameServer::OnConnect(ConnectionId id, std::string_view token, std::uint64_t nowMs) { impl_->OnConnect(id, token, nowMs); }
void GameServer::OnMessage(ConnectionId id, std::string_view text, std::uint64_t nowMs) { impl_->OnMessage(id, text, nowMs); }
void GameServer::OnDisconnect(ConnectionId id) { impl_->OnDisconnect(id); }
void GameServer::Tick(std::uint64_t) { impl_->Tick(); }
GameServer::State GameServer::state() const { return impl_->state_; }
int GameServer::connectedPlayers() const { return impl_->ConnectedCount(); }
int GameServer::seats() const { return impl_->cfg_.seats; }
int GameServer::bots() const { return impl_->cfg_.bots; }
const MatchManager* GameServer::match() const { return impl_->match_.get(); }
std::uint64_t GameServer::tickCount() const { return impl_->tick_; }
void GameServer::SetCommandObserver(CommandObserver observer) { impl_->observer_ = std::move(observer); }
void GameServer::SetSnapshotSink(SnapshotSink sink) { impl_->snapshotSink_ = std::move(sink); }
void GameServer::SetMatchFinishedHandler(MatchFinishedHandler handler) { impl_->finishedHandler_ = std::move(handler); }
bool GameServer::Resume(const std::vector<std::uint8_t>& snapshot, const std::string& seatsJson, std::string* error) { return impl_->Resume(snapshot, seatsJson, error); }

}  // namespace w2f::net
