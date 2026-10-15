// Client-server versus peer-to-peer, through the same Multiplayer::Session a
// game uses, on loopback in one process.
//
//   X  mode     client-server text / client-server binary / peer-to-peer
//      players  2/4/8 sessions, each publishing every frame at 60 Hz
//   Y  propagation latency: time from one session's publishLocalPlayer() to
//      another session seeing that pose after its update() (mean, p50, p95,
//      p99 milliseconds), delivery ratio (share of published poses another
//      session ever saw), and main-thread cost of update() + publish per
//      session per frame (microseconds)
//
// Each pose carries its send time in the opaque player data, so latency is
// measured end to end with one clock. Latency includes waiting for the
// receiver's next frame, as it would in a game. Neither mode has platforms:
// the client-server host has none and the peers have no authority, so both
// carry players only. Peer-to-peer is text only; binary is a client-server
// encoding.

#include "BenchmarkSupport.hpp"
#include "Multiplayer.hpp"
#include "NetworkServerHost.hpp"
#include "TimeSource.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto frameInterval = std::chrono::microseconds(16667);
constexpr auto readyTimeout = std::chrono::seconds(15);
// Peer ports for run n start at peerPortBase + n * portsPerRun, so a run never
// rebinds a port the previous one just released.
constexpr int peerPortBase = 41000;
constexpr int portsPerRun = 20;

std::unique_ptr<Network::NetworkServerHost> startHost()
{
    Network::ServerConfig server;
    for (int i = 0; i < 8; ++i) {
        server.spawnPoints.push_back({48.0F + 100.0F * i, 48.0F});
    }
    Network::HostConfig host;
    host.bindEndpoint = "tcp://127.0.0.1:*";
    host.pollInterval = std::chrono::milliseconds(20);
    auto result = std::make_unique<Network::NetworkServerHost>(server, host);
    result->start();
    return result;
}

std::vector<std::unique_ptr<Multiplayer::Session>> openSessions(const std::string& mode, int players,
                                                                const std::string& endpoint, int runIndex,
                                                                const TimeSource& clock)
{
    std::vector<std::unique_ptr<Multiplayer::Session>> sessions;
    const int base = peerPortBase + runIndex * portsPerRun;
    for (int i = 0; i < players; ++i) {
        Multiplayer::Config config;
        config.playerName = "bench" + std::to_string(i + 1);
        if (mode == "peer-to-peer") {
            config.mode = Multiplayer::Mode::PeerToPeer;
            config.serverEndpoint = {};
            config.peerId = static_cast<Multiplayer::PlayerId>(i + 1);
            config.basePort = base + 2 * i;
            if (i > 0) {
                config.bootstrapPeers = {"tcp://127.0.0.1:" + std::to_string(base)};
            }
        } else {
            config.mode = Multiplayer::Mode::ClientServer;
            config.serverEndpoint = endpoint;
            config.clientServerDataFormat =
                mode == "client-server-binary" ? Net::DataFormat::Binary : Net::DataFormat::TextMultipart;
        }
        sessions.push_back(Multiplayer::Session::open(config, clock));
    }
    return sessions;
}

bool parseStamp(const std::string& data, std::int64_t& stamp)
{
    if (data.empty()) {
        return false;
    }
    try {
        std::size_t used = 0;
        stamp = std::stoll(data, &used);
        return used == data.size();
    } catch (const std::exception&) {
        return false;
    }
}

struct RunResult {
    bool ready = false;
    std::vector<double> latencyMs;
    double deliveryRatio = 0.0;
    double sessionCostUs = 0.0;
};

RunResult runOnce(const std::string& mode, int players, int runIndex, int warmupFrames, int measuredFrames)
{
    RealTimeClock clock;
    std::unique_ptr<Network::NetworkServerHost> host;
    if (mode != "peer-to-peer") {
        host = startHost();
    }
    auto sessions = openSessions(mode, players, host ? host->boundEndpoint() : std::string{}, runIndex, clock);

    RunResult result;
    // Last stamp each receiver saw from each sender, and how many distinct
    // stamps it saw while measuring.
    std::vector<std::map<Multiplayer::PlayerId, std::int64_t>> lastSeen(sessions.size());
    std::uint64_t delivered = 0;
    double sessionNs = 0.0;
    int measuring = -1;  // frames measured so far, or -1 before the party is ready
    const auto readyDeadline = Bench::Clock::now() + readyTimeout;
    auto nextFrame = Bench::Clock::now();

    for (int frame = 0;; ++frame) {
        const bool counting = measuring >= warmupFrames;
        for (std::size_t s = 0; s < sessions.size(); ++s) {
            Multiplayer::Session& session = *sessions[s];
            const std::int64_t start = Bench::nowNs();
            session.update();
            const std::int64_t received = Bench::nowNs();
            for (const Multiplayer::Player& player : session.remotePlayers()) {
                std::int64_t stamp = 0;
                if (!parseStamp(player.data, stamp)) {
                    continue;
                }
                std::int64_t& last = lastSeen[s][player.id];
                if (stamp != last) {
                    last = stamp;
                    if (counting) {
                        result.latencyMs.push_back(static_cast<double>(received - stamp) / 1e6);
                        ++delivered;
                    }
                }
            }
            const std::int64_t sent = Bench::nowNs();
            session.publishLocalPlayer(10.0F * static_cast<float>(s), static_cast<float>(frame % 500),
                                       std::to_string(sent));
            if (counting) {
                sessionNs += static_cast<double>((received - start) + (Bench::nowNs() - sent));
            }
        }

        if (measuring < 0) {
            bool allReady = true;
            for (const auto& session : sessions) {
                allReady &= session->state() == Multiplayer::State::Ready &&
                            session->remotePlayers().size() == sessions.size() - 1;
            }
            if (allReady) {
                measuring = 0;
            } else if (Bench::Clock::now() > readyDeadline) {
                return result;  // not ready; reported as a failed run
            }
        } else if (++measuring >= warmupFrames + measuredFrames) {
            break;
        }

        nextFrame += frameInterval;
        std::this_thread::sleep_until(nextFrame);
    }

    result.ready = true;
    const double possible = static_cast<double>(measuredFrames) * players * (players - 1);
    result.deliveryRatio = static_cast<double>(delivered) / possible;
    result.sessionCostUs = sessionNs / (static_cast<double>(measuredFrames) * players) / 1e3;
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    const Bench::Options options = Bench::parseOptions(
        argc, argv, "multiplayer-mode-benchmark", "client-server versus peer-to-peer through Multiplayer::Session");

    const std::vector<std::string> modes{"client-server-text", "client-server-binary", "peer-to-peer"};
    const std::vector<int> playerCounts = options.quick ? std::vector<int>{2} : std::vector<int>{2, 4, 8};
    const int repetitions = options.quick ? 1 : 5;
    const int warmupFrames = options.quick ? 10 : 60;
    const int measuredFrames = options.quick ? 60 : 300;

    Bench::Results results({"mode", "players"}, {"latency_mean_ms", "latency_p50_ms", "latency_p95_ms",
                                                 "latency_p99_ms", "delivery_ratio", "session_cost_us"});

    int runIndex = 0;
    int failures = 0;
    for (int rep = 1; rep <= repetitions; ++rep) {
        for (int players : playerCounts) {
            for (const std::string& mode : modes) {
                const RunResult run = runOnce(mode, players, runIndex++, warmupFrames, measuredFrames);
                if (!run.ready) {
                    std::cerr << "  " << mode << " with " << players << " players never became ready; skipped\n";
                    ++failures;
                    continue;
                }
                results.add({mode, std::to_string(players)}, rep,
                            {Bench::mean(run.latencyMs), Bench::percentile(run.latencyMs, 50.0),
                             Bench::percentile(run.latencyMs, 95.0), Bench::percentile(run.latencyMs, 99.0),
                             run.deliveryRatio, run.sessionCostUs});
                // Let sockets from this run close before the next binds.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        std::cout << "repetition " << rep << '/' << repetitions << " done\n";
    }

    results.write(options);
    results.print();
    return failures == 0 ? 0 : 1;
}
