#include "w2f/net/TcpServer.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <unordered_map>

#include "w2f/net/WebSocket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace w2f::net {

namespace {

#ifdef _WIN32
using Fd = SOCKET;
constexpr Fd kNoFd = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
int PollSockets(PollFd* fds, std::size_t n, int timeoutMs) { return WSAPoll(fds, static_cast<ULONG>(n), timeoutMs); }
void CloseFd(Fd fd) { closesocket(fd); }
bool SetNonBlocking(Fd fd) { u_long mode = 1; return ioctlsocket(fd, FIONBIO, &mode) == 0; }
bool WouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK; }
bool Interrupted() { return false; }
int ReadSome(Fd fd, char* buf, std::size_t n) { return recv(fd, buf, static_cast<int>(n), 0); }
int WriteSome(Fd fd, const char* buf, std::size_t n) { return send(fd, buf, static_cast<int>(n), 0); }
void ShutdownWrite(Fd fd) { shutdown(fd, SD_SEND); }
#else
using Fd = int;
constexpr Fd kNoFd = -1;
using PollFd = pollfd;
int PollSockets(PollFd* fds, std::size_t n, int timeoutMs) { return poll(fds, static_cast<nfds_t>(n), timeoutMs); }
void CloseFd(Fd fd) { close(fd); }
bool SetNonBlocking(Fd fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool Interrupted() { return errno == EINTR; }
int ReadSome(Fd fd, char* buf, std::size_t n) { return static_cast<int>(recv(fd, buf, n, 0)); }
int WriteSome(Fd fd, const char* buf, std::size_t n) {
#ifdef MSG_NOSIGNAL
    return static_cast<int>(send(fd, buf, n, MSG_NOSIGNAL));   // a write to a dead peer is an error return, never a SIGPIPE
#else
    return static_cast<int>(send(fd, buf, n, 0));
#endif
}
void ShutdownWrite(Fd fd) { shutdown(fd, SHUT_WR); }
#endif

constexpr std::size_t kReadChunk = 16 * 1024;
constexpr std::size_t kMaxReadPerPass = 64 * 1024;
constexpr int kMaxAcceptsPerPass = 32;

}  // namespace

class TcpServer::Impl {
public:
    enum class State { Handshake, Open, Closing };

    struct Connection {
        ConnectionId id = 0;
        Fd fd = kNoFd;
        State state = State::Handshake;
        std::string handshake;          // request bytes until the handshake is complete
        WebSocketParser parser;
        std::string out;                // bytes waiting to be written
        std::size_t outOffset = 0;
        std::uint64_t acceptedAt = 0;
        std::uint64_t lastRecv = 0;
        std::uint64_t lastPing = 0;
        std::uint64_t closeDeadline = 0;
        bool announced = false;         // the handler has been told about this connection
        bool closeAfterFlush = false;   // Closing: waiting for `out` to drain, then half-close
        bool halfClosed = false;
        bool dead = false;
        explicit Connection(std::size_t maxMessage) : parser(maxMessage) {}
    };

    explicit Impl(const TcpServerConfig& config) : cfg_(config) {
#ifdef _WIN32
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
    ~Impl() {
        Shutdown();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool Listen(std::string* error) {
        auto fail = [&](const std::string& why) {
            if (error) *error = why;
            if (listenFd_ != kNoFd) { CloseFd(listenFd_); listenFd_ = kNoFd; }
            return false;
        };
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ == kNoFd) return fail("cannot create a socket");
        const int yes = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg_.port);
        if (inet_pton(AF_INET, cfg_.bindAddress.c_str(), &addr.sin_addr) != 1) return fail("bad bind address '" + cfg_.bindAddress + "' (IPv4 expected)");
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return fail("cannot bind " + cfg_.bindAddress + ":" + std::to_string(cfg_.port) + ": " + std::strerror(errno));
        if (listen(listenFd_, 128) != 0) return fail("cannot listen");
        if (!SetNonBlocking(listenFd_)) return fail("cannot make the listening socket non-blocking");
        sockaddr_in bound;
        socklen_t len = sizeof(bound);
        if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) port_ = ntohs(bound.sin_port);
        return true;
    }

    void RunOnce(int timeoutMs, std::uint64_t now) {
        now_ = now;
        FlushWrites();   // things queued since the last pass (a game tick's messages) go out before we wait

        std::vector<PollFd> fds;
        std::vector<ConnectionId> ids;   // parallel to fds[1..]
        PollFd listenEntry;
        std::memset(&listenEntry, 0, sizeof(listenEntry));
        listenEntry.fd = listenFd_;
        listenEntry.events = (listenFd_ != kNoFd && conns_.size() < cfg_.maxConnections) ? POLLIN : 0;
        fds.push_back(listenEntry);
        for (auto& [id, c] : conns_) {
            PollFd p;
            std::memset(&p, 0, sizeof(p));
            p.fd = c->fd;
            p.events = POLLIN;
            if (c->outOffset < c->out.size()) p.events |= POLLOUT;
            fds.push_back(p);
            ids.push_back(id);
        }
        const int ready = PollSockets(fds.data(), fds.size(), timeoutMs);
        if (ready < 0 && !Interrupted()) return;

        if (ready > 0) {
            if (fds[0].revents & POLLIN) AcceptPending();
            for (std::size_t i = 0; i < ids.size(); ++i) {
                auto it = conns_.find(ids[i]);
                if (it == conns_.end()) continue;
                Connection& c = *it->second;
                if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR)) ReadFrom(c);
                if (!c.dead && (fds[i + 1].revents & POLLNVAL)) c.dead = true;
            }
        }
        FlushWrites();
        Timers();
        Reap();
    }

    void Shutdown() {
        std::vector<ConnectionId> ids;
        for (auto& [id, c] : conns_) ids.push_back(id);
        for (ConnectionId id : ids) {
            auto it = conns_.find(id);
            if (it == conns_.end()) continue;
            Connection& c = *it->second;
            if (c.state == State::Open) {   // best effort: the frame goes out if the socket takes it right now
                const std::string frame = EncodeClose(1001, "server shutting down");
                WriteSome(c.fd, frame.data(), frame.size());
            }
            c.dead = true;
        }
        Reap();
        if (listenFd_ != kNoFd) { CloseFd(listenFd_); listenFd_ = kNoFd; }
    }

    void Send(ConnectionId id, std::string_view text) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        Connection& c = *it->second;
        if (c.state != State::Open || c.dead) return;
        if (c.out.size() - c.outOffset + text.size() + 10 > cfg_.maxOutboundBytes) {
            // The client is not keeping up. Buffering without bound would let one slow reader exhaust the server's memory.
            c.out.clear();
            c.outOffset = 0;
            BeginClose(c, 1008, "send queue overflow: client too slow");
            return;
        }
        c.out += EncodeFrame(WsOpcode::Text, text);
    }

    void Close(ConnectionId id, std::uint16_t code, std::string_view reason) {
        auto it = conns_.find(id);
        if (it == conns_.end()) return;
        Connection& c = *it->second;
        if (c.state == State::Open) BeginClose(c, code, reason);
        else if (c.state == State::Handshake) c.dead = true;
    }

    IServerHandler* handler_ = nullptr;
    TcpServerConfig cfg_;
    std::uint16_t port_ = 0;
    std::size_t Count() const { return conns_.size(); }

private:
    void AcceptPending() {
        for (int i = 0; i < kMaxAcceptsPerPass && conns_.size() < cfg_.maxConnections; ++i) {
            Fd fd = accept(listenFd_, nullptr, nullptr);
            if (fd == kNoFd) return;   // nothing (more) to accept, or a transient error: try again next pass
            if (!SetNonBlocking(fd)) { CloseFd(fd); continue; }
            const int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_NOSIGPIPE
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            auto c = std::make_unique<Connection>(cfg_.maxMessageBytes);
            c->id = ++nextId_;
            c->fd = fd;
            c->acceptedAt = c->lastRecv = c->lastPing = now_;
            conns_[c->id] = std::move(c);
        }
    }

    void ReadFrom(Connection& c) {
        char buf[kReadChunk];
        std::size_t total = 0;
        while (!c.dead && total < kMaxReadPerPass) {
            const int n = ReadSome(c.fd, buf, sizeof(buf));
            if (n > 0) {
                total += static_cast<std::size_t>(n);
                c.lastRecv = now_;
                if (c.state == State::Closing) continue;   // draining what a closing peer still sends
                Consume(c, buf, static_cast<std::size_t>(n));
            } else if (n == 0) {
                c.dead = true;   // the peer closed
            } else if (WouldBlock()) {
                return;
            } else if (Interrupted()) {
                continue;
            } else {
                c.dead = true;   // reset / broken
            }
        }
    }

    void Consume(Connection& c, const char* data, std::size_t n) {
        if (c.state == State::Handshake) {
            c.handshake.append(data, n);
            const HandshakeResult hs = ParseHandshake(c.handshake);
            if (hs.status == HandshakeStatus::NeedMore) return;
            if (hs.status == HandshakeStatus::Bad) {
                c.out += BuildHttpError(hs.httpStatus, hs.reason);
                c.state = State::Closing;
                c.closeAfterFlush = true;
                c.closeDeadline = now_ + cfg_.closeGraceMs;
                c.handshake.clear();
                return;
            }
            if (!cfg_.joinCode.empty() && QueryParam(hs.query, "code") != cfg_.joinCode) {   // a private server: wrong or missing join code
                c.out += BuildHttpError(403, "join code required");
                c.state = State::Closing;
                c.closeAfterFlush = true;
                c.closeDeadline = now_ + cfg_.closeGraceMs;
                c.handshake.clear();
                return;
            }
            c.out += BuildHandshakeResponse(hs.key);
            c.state = State::Open;
            const std::string rest = c.handshake.substr(hs.consumed);
            c.handshake.clear();
            c.announced = true;
            if (handler_) handler_->OnConnect(c.id, QueryParam(hs.query, "token"), now_);
            if (!rest.empty() && !c.dead && c.state == State::Open) Consume(c, rest.data(), rest.size());
            return;
        }
        if (c.state != State::Open) return;

        std::vector<WsEvent> events;
        c.parser.Feed(reinterpret_cast<const std::uint8_t*>(data), n, events);
        for (WsEvent& e : events) {
            if (c.state != State::Open || c.dead) break;   // the handler closed it while we were dispatching
            switch (e.kind) {
                case WsEvent::Kind::Text:
                    if (handler_) handler_->OnMessage(c.id, e.payload, now_);
                    break;
                case WsEvent::Kind::Ping:
                    c.out += EncodeFrame(WsOpcode::Pong, e.payload);
                    break;
                case WsEvent::Kind::Pong:
                    break;
                case WsEvent::Kind::Close:
                    BeginClose(c, e.code == 1005 ? std::uint16_t{1000} : e.code, "");   // echo the peer's close and finish
                    break;
                case WsEvent::Kind::Error:
                    BeginClose(c, e.code, e.reason);
                    break;
            }
        }
    }

    // Queues a close frame and stops taking application data; the socket is half-closed once everything queued is written.
    void BeginClose(Connection& c, std::uint16_t code, std::string_view reason) {
        if (c.state == State::Closing) return;
        c.out += EncodeClose(code, reason);
        c.state = State::Closing;
        c.closeAfterFlush = true;
        c.closeDeadline = now_ + cfg_.closeGraceMs;
    }

    void FlushWrites() {
        for (auto& [id, cp] : conns_) {
            Connection& c = *cp;
            while (!c.dead && c.outOffset < c.out.size()) {
                const int n = WriteSome(c.fd, c.out.data() + c.outOffset, c.out.size() - c.outOffset);
                if (n > 0) {
                    c.outOffset += static_cast<std::size_t>(n);
                } else if (n < 0 && WouldBlock()) {
                    break;
                } else if (n < 0 && Interrupted()) {
                    continue;
                } else {
                    c.dead = true;
                }
            }
            if (c.outOffset >= c.out.size()) {
                c.out.clear();
                c.outOffset = 0;
                if (c.state == State::Closing && c.closeAfterFlush && !c.halfClosed && !c.dead) {
                    ShutdownWrite(c.fd);   // FIN after our last bytes; then wait (briefly) for the peer's
                    c.halfClosed = true;
                }
            }
        }
    }

    void Timers() {
        for (auto& [id, cp] : conns_) {
            Connection& c = *cp;
            if (c.dead) continue;
            switch (c.state) {
                case State::Handshake:
                    if (now_ - c.acceptedAt > cfg_.handshakeTimeoutMs) c.dead = true;   // slowloris: never finished the request
                    break;
                case State::Open:
                    if (now_ - c.lastRecv > cfg_.idleTimeoutMs) {
                        BeginClose(c, 1008, "idle timeout");
                    } else if (now_ - c.lastPing >= cfg_.pingIntervalMs) {
                        c.out += EncodeFrame(WsOpcode::Ping, "");
                        c.lastPing = now_;
                    }
                    break;
                case State::Closing:
                    if (now_ >= c.closeDeadline) c.dead = true;   // the peer did not finish the closing handshake in time
                    break;
            }
        }
        FlushWrites();
    }

    void Reap() {
        std::vector<ConnectionId> gone;
        for (auto& [id, cp] : conns_) {
            if (cp->dead) gone.push_back(id);
        }
        for (ConnectionId id : gone) {
            auto it = conns_.find(id);
            std::unique_ptr<Connection> c = std::move(it->second);
            conns_.erase(it);
            if (c->fd != kNoFd) CloseFd(c->fd);
            if (c->announced && handler_) handler_->OnDisconnect(id);   // after the socket is gone and the map entry removed
        }
    }

    Fd listenFd_ = kNoFd;
    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> conns_;
    ConnectionId nextId_ = 0;
    std::uint64_t now_ = 0;
};

TcpServer::TcpServer(const TcpServerConfig& config) : impl_(std::make_unique<Impl>(config)) {}
TcpServer::~TcpServer() = default;
void TcpServer::SetHandler(IServerHandler* handler) { impl_->handler_ = handler; }
bool TcpServer::Listen(std::string* error) { return impl_->Listen(error); }
std::uint16_t TcpServer::port() const { return impl_->port_; }
void TcpServer::RunOnce(int timeoutMs, std::uint64_t nowMs) { impl_->RunOnce(timeoutMs, nowMs); }
void TcpServer::Shutdown() { impl_->Shutdown(); }
std::size_t TcpServer::connectionCount() const { return impl_->Count(); }
void TcpServer::Send(ConnectionId id, std::string_view text) { impl_->Send(id, text); }
void TcpServer::Close(ConnectionId id, std::uint16_t code, std::string_view reason) { impl_->Close(id, code, reason); }

void RunServerLoop(TcpServer& tcp, GameServer& game, const std::atomic<bool>& stop) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto elapsedNs = [&]() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count()); };
    const std::uint64_t tickNs = 1'000'000'000ull / kTicksPerSecond;
    std::uint64_t nextTick = tickNs;
    while (!stop.load()) {
        std::uint64_t now = elapsedNs();
        int ran = 0;
        while (now >= nextTick && ran < 5) {   // catch up a few ticks after a stall...
            game.Tick(now / 1'000'000);
            nextTick += tickNs;
            ++ran;
            now = elapsedNs();
        }
        if (now >= nextTick) nextTick = now + tickNs;   // ...but never spiral: drop the backlog if it is longer than that
        const std::uint64_t waitNs = nextTick > now ? nextTick - now : 0;
        tcp.RunOnce(static_cast<int>((waitNs + 999'999) / 1'000'000), elapsedNs() / 1'000'000);
    }
    tcp.Shutdown();
}

}  // namespace w2f::net
