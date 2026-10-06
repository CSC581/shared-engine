// Headless Section 2/4 demo server: the arena layout from NetworkDemoConfig,
// hosted by the engine's NetworkServerHost. Everything network-shaped — the
// handshake, per-client workers, endpoint advertising, shutdown — is the
// host's job; this file is only the command line and Ctrl-C.

#include "NetworkDemoConfig.hpp"
#include "NetworkServerHost.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int)
{
    g_running.store(false);
}

bool parseCount(const std::string& text, std::size_t& out)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        out = static_cast<std::size_t>(std::stoull(text));
    } catch (const std::out_of_range&) {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Network::HostConfig hostConfig;
    hostConfig.mode = Network::HostMode::Dedicated;
    Network::ServerConfig serverConfig = NetworkDemo::makeServerConfig();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-players") {
            // 0 (the default) accepts any number of players.
            std::size_t parsed = 0;
            if (i + 1 >= argc || !parseCount(argv[i + 1], parsed)) {
                std::cerr << "network-server: --max-players requires a number (0 = no limit)\n";
                return 1;
            }
            serverConfig.maxPlayers = parsed;
            ++i;
            continue;
        }
        if (arg == "--advertise") {
            if (i + 1 >= argc) {
                std::cerr << "network-server: --advertise requires a host\n";
                return 1;
            }
            hostConfig.advertiseHost = argv[++i];
            continue;
        }
        if (arg == "--rates") {
            // Once a second, each player's accepted POSITION/s: run clients at
            // 0.5x / 1x / 2x and the server sees three different rates.
            hostConfig.trafficLogInterval = std::chrono::seconds(1);
            continue;
        }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "network-server: unknown option " << arg << '\n';
            return 1;
        }
        hostConfig.bindEndpoint = arg;
    }
    hostConfig.log = [](const std::string& line) { std::cout << "network-server: " << line << '\n'; };

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        Network::NetworkServerHost host(serverConfig, hostConfig);
        host.start();
        std::cout << "Session endpoints advertise host " << hostConfig.advertiseHost << '\n';
        std::cout << "Each JOIN gets a dedicated REP worker; GET_WORLD is read-only.\n";
        std::cout << "Moving platforms are server-authored on real time.\n";
        std::cout << (serverConfig.maxPlayers == 0 ? std::string("No player limit.")
                                                   : "Up to " + std::to_string(serverConfig.maxPlayers) + " players.")
                  << '\n';

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        host.stop();
    } catch (const std::exception& exception) {
        std::cerr << "Network server error: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
