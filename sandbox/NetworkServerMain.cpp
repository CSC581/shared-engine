// Headless Section 2/4 demo server: the arena layout from NetworkDemoConfig,
// hosted by the engine's NetworkServerHost. Everything network-shaped — the
// handshake, per-client workers, endpoint advertising, shutdown — is the
// host's job; this file is only the command line and Ctrl-C.

#include "NetworkDemoConfig.hpp"
#include "NetworkServerHost.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void onSignal(int)
{
    g_running.store(false);
}

} // namespace

int main(int argc, char* argv[])
{
    Network::HostConfig hostConfig;
    hostConfig.mode = Network::HostMode::Dedicated;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
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
        Network::NetworkServerHost host(NetworkDemo::makeServerConfig(), hostConfig);
        host.start();
        std::cout << "Session endpoints advertise host " << hostConfig.advertiseHost << '\n';
        std::cout << "Each JOIN gets a dedicated REP worker; GET_WORLD is read-only.\n";
        std::cout << "Moving platforms are server-authored on real time.\n";

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
