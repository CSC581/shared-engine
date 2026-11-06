// Headless platform host and Mewtwo bot. Run with --help for options.
#include "DeltaTimer.hpp"
#include "Endpoint.hpp"
#include "Multiplayer.hpp"
#include "NetworkEventBridge.hpp"
#include "NetworkServerHost.hpp"
#include "PokemonHuntBoss.hpp"
#include "PokemonHuntEvents.hpp"
#include "PokemonHuntWorld.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace PokemonHunt;

constexpr auto worldTickInterval = std::chrono::milliseconds(16);
constexpr std::int64_t maxWorldStepNs = 50 * kNsPerMs;
constexpr auto trafficLogInterval = std::chrono::milliseconds(1000);

constexpr const char* defaultAdvertiseHost = "127.0.0.1";
// A restarted server greets peer 1 to rejoin a running mesh.
constexpr const char* defaultFirstPeerEndpoint = "tcp://127.0.0.1:7300";

std::atomic<bool> g_running{true};

void onSignal(int)
{
    g_running.store(false);
}

// The host logs from its own threads.
std::mutex g_logMutex;

void say(const std::string& line)
{
    const std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << line << std::endl;
}

// One boss shared across network modes; only client-server text carries combat events.
class BossBot {
public:
    explicit BossBot(const TimeSource& clock) : clock_(clock), frameTimer_(clock, maxWorldStepNs) {}

    void join(const std::string& label, const Multiplayer::Config& config)
    {
        auto session = Multiplayer::Session::open(config, clock_);
        if (session->state() == Multiplayer::State::Failed) {
            say("Mewtwo could not join " + label + ": " + session->status());
            return;
        }
        if (session->supportsEvents() && !bridge_) {
            bridge_ = std::make_unique<NetworkEventBridge>(boss_.events(), *session, sharedEventTypes());
            bridgedLink_ = links_.size();
            // Hits come over this session, so its poses say where hitters are.
            boss_.setPlayerLocator(sessionLocator(*session));
        }
        links_.push_back({label, std::move(session), false});
    }

    void run()
    {
        frameTimer_.reset();
        while (g_running.load()) {
            std::this_thread::sleep_for(worldTickInterval);
            boss_.update(static_cast<float>(frameTimer_.tick()) / static_cast<float>(kNsPerSec));
            for (std::size_t i = 0; i < links_.size(); ++i) {
                links_[i].session->update();
                reportReadiness(links_[i]);
                if (bridge_ && i == bridgedLink_) {
                    // Raised now, handled in the next boss_.update().
                    bridge_->apply();
                }
            }

            const std::string data = encodeBossWorld(boss_.objects());
            for (Link& link : links_) {
                link.session->publishLocalPlayer(boss_.x(), boss_.y(), data);
            }
        }
    }

private:
    struct Link {
        std::string label;
        std::unique_ptr<Multiplayer::Session> session;
        bool announced;
    };

    static void reportReadiness(Link& link)
    {
        if (!link.announced && link.session->state() == Multiplayer::State::Ready) {
            link.announced = true;
            say("Mewtwo joined the " + link.label + " session");
        }
    }

    const TimeSource& clock_;
    DeltaTimer frameTimer_;
    Boss boss_{say};
    std::vector<Link> links_;
    // Last: it must go before the session and the manager it uses.
    std::unique_ptr<NetworkEventBridge> bridge_;
    std::size_t bridgedLink_ = 0;
};

struct ServerOptions {
    std::string handshakeEndpoint = defaultServerBind;
    std::string advertiseHost = defaultAdvertiseHost;
    std::string firstPeer = defaultFirstPeerEndpoint;
    bool logRates = false;
    // Benchmarks add plain platforms past the level's own.
    std::size_t platforms = movingPlatformPaths().size();
};

bool parseArgs(int argc, char* argv[], ServerOptions& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rates") {
            options.logRates = true;
        } else if (arg == "--advertise" || arg == "--peer" || arg == "--platforms") {
            if (i + 1 >= argc) {
                std::cerr << "pokemon-hunt-server: " << arg << " requires a value\n";
                return false;
            }
            const std::string value = argv[++i];
            if (arg == "--platforms") {
                const long count = std::strtol(value.c_str(), nullptr, 10);
                if (count < static_cast<long>(movingPlatformPaths().size())) {
                    std::cerr << "pokemon-hunt-server: --platforms needs at least the level's "
                              << movingPlatformPaths().size() << '\n';
                    return false;
                }
                options.platforms = static_cast<std::size_t>(count);
            } else {
                (arg == "--advertise" ? options.advertiseHost : options.firstPeer) = value;
            }
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "pokemon-hunt-server: unknown option " << arg << '\n';
            return false;
        } else {
            options.handshakeEndpoint = arg;
        }
    }
    return true;
}

Network::ServerConfig makeServerConfig(const ServerOptions& options)
{
    Network::ServerConfig config;
    // Required by the engine; Pokemon use the client's spawn points.
    config.spawnPoints = {{0.0F, 0.0F}};
    config.platforms = movingPlatformPaths();
    // A copy: push_back may reallocate.
    const Network::PlatformPath model = config.platforms.front();
    for (std::size_t i = config.platforms.size(); i < options.platforms; ++i) {
        Network::PlatformPath path;
        path.id = static_cast<std::uint32_t>(i + 1);
        path.startX = static_cast<float>((i * 97) % static_cast<std::size_t>(levelWidth - 200.0F));
        path.endX = path.startX + 150.0F;
        path.startY = path.endY = model.startY;
        path.width = 40.0F;
        path.height = model.height;
        path.speed = 60.0F + static_cast<float>(i % 40);
        config.platforms.push_back(path);
    }
    return config;
}

Network::HostConfig makeHostConfig(const ServerOptions& options)
{
    Network::HostConfig config;
    config.mode = Network::HostMode::Dedicated;
    config.bindEndpoint = options.handshakeEndpoint;
    config.advertiseHost = options.advertiseHost;
    config.log = say;
    if (options.logRates) {
        config.trafficLogInterval = trafficLogInterval;
    }
    return config;
}

Multiplayer::Config clientServerLink(const Network::NetworkServerHost& host)
{
    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::ClientServer;
    config.serverEndpoint = Net::rewriteTcpEndpointHost(host.boundEndpoint(), "127.0.0.1");
    config.playerName = bossPlayerName;
    return config;
}

Multiplayer::Config peerLink(const ServerOptions& options)
{
    Multiplayer::Config config;
    config.mode = Multiplayer::Mode::PeerToPeer;
    config.serverEndpoint.clear();
    config.playerName = bossPlayerName;
    config.peerId = bossPeerId;
    config.basePort = bossPeerBasePort;
    config.advertiseHost = options.advertiseHost;
    config.bootstrapPeers = {options.firstPeer};
    return config;
}

} // namespace

int main(int argc, char* argv[])
{
    ServerOptions options;
    if (!parseArgs(argc, argv, options)) {
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        // The bot's sessions leave before the host stops.
        Network::NetworkServerHost host(makeServerConfig(options), makeHostConfig(options));
        host.start();
        RealTimeClock clock;
        BossBot bot(clock);
        bot.join("client-server", clientServerLink(host));
        bot.join("peer-to-peer", peerLink(options));

        say("Mewtwo's peer listens on port " + std::to_string(bossPeerBasePort));
        say("A wild Mewtwo appeared! It moves on the server's real time.");
        bot.run();
    } catch (const std::exception& exception) {
        std::cerr << "pokemon-hunt-server: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
