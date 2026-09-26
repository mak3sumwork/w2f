#pragma once

// The lobby and the match host: everything the server does with a connection EXCEPT the sockets.
//
//   transport (TcpServer, or a test's fake) --OnConnect/OnMessage/OnDisconnect--> GameServer --Send/Close--> transport
//
// GameServer knows nothing about sockets or the wall clock (the caller passes `nowMs`, used only to rate-limit clients), so
// the whole protocol -- lobby, reconnects, command validation and routing, event fan-out, privacy -- is tested without a
// network. And it is the ONLY code that talks to the game engine, through its public interface: MatchManager's Try* actions
// to act, IMatchListener to observe, const getters to read. The engine has no idea a network exists.
//
// Rules the transport must follow: Send / Close never call back into the handler synchronously; a closed connection is
// reported later through OnDisconnect (which is also what tells GameServer the client is gone).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "w2f/ChampionDatabase.h"
#include "w2f/Config.h"
#include "w2f/Item.h"
#include "w2f/MatchManager.h"
#include "w2f/Pve.h"
#include "w2f/Text.h"
#include "w2f/Trait.h"
#include "w2f/net/Protocol.h"

namespace w2f::net {

using ConnectionId = std::uint64_t;

class IServerTransport {
public:
    virtual ~IServerTransport() = default;
    virtual void Send(ConnectionId connection, std::string_view text) = 0;
    // WebSocket close code + reason (1000 normal, 1008 policy, 1013 try again later, 4000-4999 application-defined).
    virtual void Close(ConnectionId connection, std::uint16_t code, std::string_view reason) = 0;
};

class IServerHandler {
public:
    virtual ~IServerHandler() = default;
    // `token`: the reconnect token from the URL ("" for a new player).
    virtual void OnConnect(ConnectionId connection, std::string_view token, std::uint64_t nowMs) = 0;
    virtual void OnMessage(ConnectionId connection, std::string_view text, std::uint64_t nowMs) = 0;
    virtual void OnDisconnect(ConnectionId connection) = 0;
};

// The loaded game data and rules a match is built from. Non-owning: the caller keeps everything alive.
struct GameData {
    const ChampionDatabase* champions = nullptr;
    const ItemDatabase* items = nullptr;         // optional
    const TraitDatabase* traits = nullptr;       // optional
    const EncounterDatabase* encounters = nullptr;   // optional (no PvE data: PvE rounds are rounds nobody fights)
    const MotherNatureDatabase* motherNature = nullptr;   // optional (no data: no gift rounds; every round has its shop)
    const TextTable* text = nullptr;             // optional display text; sent in the `catalog` message
    GameConfig config;
};

struct GameServerConfig {
    int seats = kMaxPlayers;   // total seats in the match (2..8); it starts by itself when every HUMAN seat is taken
    // How many of the seats are AI players (0..seats-1; at least one seat stays human). They take the LAST seats, so the humans are seats
    // 0..seats-bots-1 and the lobby waits for `seats - bots` connections. A bot is an AIBotController driven by the server once per tick,
    // acting only through the same MatchManager::Try* calls a client uses.
    int bots = 0;
    // 0 = a fresh random seed per match (from `entropy`). A fixed seed makes every match identical: for tests and debugging.
    std::uint64_t seed = 0;
    // Where seeds and reconnect tokens come from. Default: std::random_device. Tests inject a deterministic source.
    std::function<std::uint64_t()> entropy;

    // Per connection: a token bucket. Each message costs 1 token (get_state 3, get_fight 5); the bucket holds `rateBurst`
    // and refills `rateRefillPerSecond` per second.
    int rateBurst = 30;
    int rateRefillPerSecond = 15;
    // Protocol errors + rate-limit hits a connection may accumulate before it is disconnected.
    int maxViolations = 50;
    // After the match ends the server waits this many ticks (players read the result), then disconnects everyone and
    // opens a fresh lobby.
    int postMatchTicks = kTicksPerSecond * 20;
};

class GameServer : public IServerHandler {
public:
    enum class State { Lobby, Running, Finished };

    GameServer(const GameServerConfig& config, const GameData& data, IServerTransport& transport);
    ~GameServer() override;
    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    void OnConnect(ConnectionId connection, std::string_view token, std::uint64_t nowMs) override;
    void OnMessage(ConnectionId connection, std::string_view text, std::uint64_t nowMs) override;
    void OnDisconnect(ConnectionId connection) override;

    // Advances the game by ONE simulation tick (call it kTicksPerSecond times a second). Does nothing in the lobby.
    void Tick(std::uint64_t nowMs);

    // ---- Observation ----
    State state() const;
    int connectedPlayers() const;   // humans only
    int seats() const;              // all seats, bots included
    int bots() const;
    // Read-only view of the running match (nullptr in the lobby). For tests, logging and admin tooling.
    const MatchManager* match() const;
    std::uint64_t tickCount() const;
    // True if one of this server's seats belongs to `token` (a QueueServer routes a reconnect to the match that knows it).
    bool HoldsToken(std::string_view token) const;

    // Every command that reached the engine FROM A CLIENT, with the tick it ran on and its result (tests use it to replay a networked match
    // against a bare engine and prove the network changed nothing). The bots' actions are not reported: with bots the replay would need them too.
    using CommandObserver = std::function<void(std::uint64_t tick, PlayerId player, const Command& command, ActionResult result)>;
    void SetCommandObserver(CommandObserver observer);
    // Receives the engine's automatic snapshot at the start of every Planning phase (where the server persists it), together with the SEATS sidecar: a
    // small JSON text with what the snapshot cannot hold -- every seat's reconnect token, which seats are bots and the bots' own state. Persist both
    // (w2f_server --autosave FILE writes FILE and FILE.seats); Resume needs the pair.
    using SnapshotSink = std::function<void(int round, const std::vector<std::uint8_t>& snapshot, const std::string& seatsJson)>;
    void SetSnapshotSink(SnapshotSink sink);

    // Called once when a match ends (the moment `match_over` goes out). The server tool deletes its autosave then: a finished match must never be resumed.
    using MatchFinishedHandler = std::function<void()>;
    void SetMatchFinishedHandler(MatchFinishedHandler handler);

    // Crash recovery: turns a freshly started server (no match, nobody connected) into the server that wrote this snapshot, at the start of that round's
    // Planning phase. The humans' seats come back with their old tokens (nobody is connected yet: each reconnects with `?token=...` and is resynced), the
    // bots come back with their state, and the match carries on tick for tick as it would have. The server must be configured like the one that wrote it
    // (seats, bots, data files, rules); anything else is refused, with the reason in *error. False and unchanged on any failure.
    bool Resume(const std::vector<std::uint8_t>& snapshot, const std::string& seatsJson, std::string* error = nullptr);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace w2f::net
