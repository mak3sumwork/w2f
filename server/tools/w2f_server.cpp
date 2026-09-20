// The W2F game server: a standalone WebSocket server that hosts one lobby and one match at a time.
//
//   w2f_server [--port 7777] [--bind 0.0.0.0] [--players 8] [--data DIR] [--seed N] [--autosave FILE] [--fast]
//
// Clients connect with a WebSocket (ws://host:port/), are given a seat, and when the last seat fills the match starts by itself.
// The protocol is documented in docs/network-protocol.md.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "w2f/ChampionLoader.h"
#include "w2f/net/GameServer.h"
#include "w2f/net/TcpServer.h"

#ifndef W2F_DATA_DIR
#define W2F_DATA_DIR "data"
#endif

using namespace w2f;
using namespace w2f::net;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop.store(true); }

struct Options {
    std::uint16_t port = 7777;
    std::string bind = "0.0.0.0";
    int players = kMaxPlayers;
    std::string dataDir = W2F_DATA_DIR;
    std::uint64_t seed = 0;
    std::string autosave;
    bool fast = false;   // development: much shorter phases, so a client can be tried against a whole match in minutes
};

bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", name); return nullptr; }
            return argv[++i];
        };
        const char* v = nullptr;
        if (a == "--port") { if (!(v = value("--port"))) return false; o.port = static_cast<std::uint16_t>(std::strtoul(v, nullptr, 10)); }
        else if (a == "--bind") { if (!(v = value("--bind"))) return false; o.bind = v; }
        else if (a == "--players") { if (!(v = value("--players"))) return false; o.players = std::atoi(v); }
        else if (a == "--data") { if (!(v = value("--data"))) return false; o.dataDir = v; }
        else if (a == "--seed") { if (!(v = value("--seed"))) return false; o.seed = std::strtoull(v, nullptr, 10); }
        else if (a == "--fast") { o.fast = true; }
        else if (a == "--autosave") { if (!(v = value("--autosave"))) return false; o.autosave = v; }
        else {
            std::fprintf(stderr, "unknown option %s\nusage: w2f_server [--port N] [--bind ADDR] [--players 2..8] [--data DIR] [--seed N] [--autosave FILE] [--fast]\n", a.c_str());
            return false;
        }
    }
    if (o.players < 2 || o.players > kMaxPlayers) { std::fprintf(stderr, "--players must be between 2 and %d\n", kMaxPlayers); return false; }
    return true;
}

// Passes everything through to the GameServer and logs the connection life cycle (never the tokens).
class LoggingHandler : public IServerHandler {
public:
    explicit LoggingHandler(GameServer& game) : game_(game) {}
    void OnConnect(ConnectionId id, std::string_view token, std::uint64_t now) override {
        game_.OnConnect(id, token, now);
        std::printf("[conn %llu] connected%s  (%d/%d seats, %s)\n", static_cast<unsigned long long>(id), token.empty() ? "" : " with a reconnect token",
                    game_.connectedPlayers(), game_.seats(), Describe());
    }
    void OnMessage(ConnectionId id, std::string_view text, std::uint64_t now) override { game_.OnMessage(id, text, now); }
    void OnDisconnect(ConnectionId id) override {
        game_.OnDisconnect(id);
        std::printf("[conn %llu] disconnected  (%d/%d seats, %s)\n", static_cast<unsigned long long>(id), game_.connectedPlayers(), game_.seats(), Describe());
    }
private:
    const char* Describe() const {
        switch (game_.state()) {
            case GameServer::State::Lobby: return "lobby";
            case GameServer::State::Running: return "match running";
            case GameServer::State::Finished: return "match finished";
        }
        return "?";
    }
    GameServer& game_;
};

// Write-then-rename, so a crash mid-write never leaves a torn file.
void WriteFileAtomically(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    const std::string temp = path + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) { std::fprintf(stderr, "autosave: cannot write %s\n", temp.c_str()); return; }
    }
#ifdef _WIN32
    std::remove(path.c_str());   // (rename does not replace an existing file on Windows; this leaves a tiny window without the old file)
#endif
    if (std::rename(temp.c_str(), path.c_str()) != 0) std::fprintf(stderr, "autosave: cannot rename %s\n", temp.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) return 2;

    // ---- game data: a bad file stops the server with the exact location of the mistake ----
    std::string error;
    auto champions = LoadChampionDatabaseFromFile(opt.dataDir + "/champions.json", &error);
    if (!champions) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }
    auto traits = LoadTraitDatabaseFromFile(opt.dataDir + "/traits.json", &error);
    if (!traits || !ValidateChampionTraits(*champions, *traits, &error)) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }
    auto items = LoadItemDatabaseFromFile(opt.dataDir + "/items.json", &error);
    if (!items || !ValidateItemTraits(*items, *traits, &error)) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }
    auto encounters = LoadEncounterDatabaseFromFile(opt.dataDir + "/pve.json", champions.get(), items.get(), &error);
    if (!encounters) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }
    auto motherNature = LoadMotherNatureDatabaseFromFile(opt.dataDir + "/mother_nature.json", items.get(), &error);
    if (!motherNature) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }

    GameData data;
    data.champions = champions.get();
    data.items = items.get();
    data.traits = traits.get();
    data.encounters = encounters.get();
    data.motherNature = motherNature.get();
    if (opt.fast) {
        data.config.match.motherNatureTicks = Seconds(3);
        data.config.match.planningTicks = Seconds(4);
        data.config.match.combatTicks = Seconds(6);
        data.config.match.resolutionTicks = Seconds(1);
    }
    if (!data.config.Validate(&error)) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }

    GameServerConfig gsc;
    gsc.seats = opt.players;
    gsc.seed = opt.seed;
    if (opt.fast) gsc.postMatchTicks = Seconds(3);

    TcpServerConfig tcfg;
    tcfg.bindAddress = opt.bind;
    tcfg.port = opt.port;
    TcpServer tcp(tcfg);
    GameServer game(gsc, data, tcp);
    if (!opt.autosave.empty()) {
        game.SetSnapshotSink([&](int round, const std::vector<std::uint8_t>& bytes) {
            WriteFileAtomically(opt.autosave, bytes);
            std::printf("[autosave] round %d: %zu bytes -> %s\n", round, bytes.size(), opt.autosave.c_str());
        });
    }
    LoggingHandler logging(game);
    tcp.SetHandler(&logging);
    if (!tcp.Listen(&error)) { std::fprintf(stderr, "Cannot start: %s\n", error.c_str()); return 2; }

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::printf("W2F server listening on %s:%u  (%d players per match, %zu champions, %zu items, %zu encounters)\n", opt.bind.c_str(),
                static_cast<unsigned>(tcp.port()), opt.players, champions->All().size(), items->All().size(), encounters->All().size());
    std::fflush(stdout);
    RunServerLoop(tcp, game, g_stop);
    std::printf("W2F server stopped\n");
    return 0;
}
