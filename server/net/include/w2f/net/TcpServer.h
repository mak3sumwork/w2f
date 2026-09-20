#pragma once

// The socket side: a single-threaded, non-blocking WebSocket server over TCP. It turns bytes into the calls GameServer
// understands (OnConnect / OnMessage / OnDisconnect) and GameServer's Send / Close into frames.
//
// One thread, one poll() loop: no locks, no data races, and the game tick runs on the same thread between network reads, so a
// message is always handled either entirely before or entirely after a tick.
//
// Hardening (each has a test): a connection that does not finish its handshake in time, or sits silent, is dropped; a client that
// does not read (its send queue would grow without bound) is dropped; oversized frames are refused from their header; a peer that
// vanishes mid-frame costs nothing; the number of open sockets is capped. Malformed WebSocket input is answered with the
// close code the RFC prescribes, never with a crash.
//
// POSIX (Linux, macOS) is tested locally; the Winsock branch (Windows) is built and run by the windows-latest job of CI (.github/workflows/build.yml).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "w2f/net/GameServer.h"

namespace w2f::net {

struct TcpServerConfig {
    std::string bindAddress = "0.0.0.0";    // IPv4
    std::uint16_t port = 0;                 // 0 = the OS picks (tests); read it back with port()
    std::size_t maxConnections = 64;        // open sockets, handshaking or not
    std::size_t maxMessageBytes = 2 * kMaxCommandBytes;   // largest WebSocket message a client may send
    std::uint64_t handshakeTimeoutMs = 5000;
    std::uint64_t idleTimeoutMs = 60000;    // nothing received at all (a live client answers our pings)
    std::uint64_t pingIntervalMs = 15000;
    std::uint64_t closeGraceMs = 2000;      // after a close frame, how long to wait for the peer before dropping the socket
    std::size_t maxOutboundBytes = 16u * 1024 * 1024;   // per connection; a client further behind than this is disconnected
};

class TcpServer : public IServerTransport {
public:
    explicit TcpServer(const TcpServerConfig& config);
    ~TcpServer() override;
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void SetHandler(IServerHandler* handler);
    bool Listen(std::string* error = nullptr);
    std::uint16_t port() const;   // the bound port (after Listen)

    // One pass of the event loop: waits up to `timeoutMs` for socket activity, then does all the reading, writing, handshakes,
    // frame handling and timeouts that are due. `nowMs` is a monotonic clock in milliseconds (the caller owns the clock).
    void RunOnce(int timeoutMs, std::uint64_t nowMs);
    // Closes every connection (with a 1001 "going away") and the listening socket.
    void Shutdown();

    std::size_t connectionCount() const;

    void Send(ConnectionId connection, std::string_view text) override;
    void Close(ConnectionId connection, std::uint16_t code, std::string_view reason) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Runs the server until `stop` becomes true: network I/O plus GameServer::Tick at kTicksPerSecond (wall-clock paced here and
// ONLY here: the engine itself never reads a clock). If the process falls behind it skips ahead rather than spiralling.
void RunServerLoop(TcpServer& tcp, GameServer& game, const std::atomic<bool>& stop);

}  // namespace w2f::net
