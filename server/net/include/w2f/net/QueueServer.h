#pragma once

// Matchmaking: a server that runs MANY matches at once and puts players into them from a queue (w2f_server --queue).
//
//   transport --OnConnect/OnMessage/OnDisconnect--> QueueServer --routes--> one GameServer per running match --Send/Close--> transport
//
// A new connection is "idle" (the client's home screen). It sends {"action":"queue","mode":"bots"|"normal"}:
//   * bots:   a match starts at once, the player + (seats - 1) AI players;
//   * normal: players wait together; the match starts when `seats` players are searching, or when the longest waiter has waited
//             `fillMs`, with AI players in the empty seats.
// The player gets `match_found`, then exactly the messages a single-lobby server sends (welcome, lobby, match_started, ...): the match itself
// is an unchanged GameServer. When it ends (or the player sends leave_match) the connection is idle again -- the socket stays open -- and
// can queue for the next one. A reconnect with a match's token (?token=...) goes straight back into that match.
//
// Like GameServer it knows nothing about sockets or the wall clock (the caller passes nowMs), so it is tested without a network.

#include <cstdint>
#include <memory>
#include <string_view>

#include "w2f/net/GameServer.h"

namespace w2f::net {

struct QueueServerConfig {
    GameServerConfig match;           // every match: seats, rate limits, post-match time, seed / entropy. `match.bots` is ignored (the queue decides).
    long long fillMs = 30'000;        // normal queue: start with bots after the longest waiter waited this long
    long long statusEveryMs = 1'000;  // searching players get a queue_status this often
    int abandonTicks = kTicksPerSecond * 120;   // a running match nobody is connected to is closed after this long
    int maxMatches = 64;              // at most this many matches at once; more queue up
};

class QueueServer : public IServerHandler {
public:
    QueueServer(const QueueServerConfig& config, const GameData& data, IServerTransport& transport);
    ~QueueServer() override;
    QueueServer(const QueueServer&) = delete;
    QueueServer& operator=(const QueueServer&) = delete;

    void OnConnect(ConnectionId connection, std::string_view token, std::uint64_t nowMs) override;
    void OnMessage(ConnectionId connection, std::string_view text, std::uint64_t nowMs) override;
    void OnDisconnect(ConnectionId connection) override;

    // One simulation tick of every running match, plus the matchmaking. Call it kTicksPerSecond times a second.
    void Tick(std::uint64_t nowMs);

    // ---- Observation (tests, logging) ----
    int online() const;                 // connections
    int searching(QueueMode mode) const;
    int matches() const;                // matches that exist (running, or finished and showing the result)
    // The match this connection plays in (nullptr: idle or searching).
    const GameServer* MatchOf(ConnectionId connection) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace w2f::net
