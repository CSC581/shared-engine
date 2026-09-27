// Headless Apex Ascent network server: tower world + server-authored movers.
// The engine's NetworkServerHost does the handshake, per-client workers,
// endpoint advertising, and shutdown; this file is only Apex's world config,
// the command line, and Ctrl-C.

#include "ApexNetworkConfig.hpp"
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
                std::cerr << "apex-network-server: --advertise requires a host\n";
                return 1;
            }
            hostConfig.advertiseHost = argv[++i];
            continue;
        }
        if (arg == "--rates") {
            // Once a second, each climber's accepted POSITION/s.
            hostConfig.trafficLogInterval = std::chrono::seconds(1);
            continue;
        }
        if (arg.rfind("--", 0) == 0) {
            std::cerr << "apex-network-server: unknown option " << arg << '\n';
            return 1;
        }
        hostConfig.bindEndpoint = arg;
    }
    hostConfig.log = [](const std::string& line) {
        std::cout << "apex-network-server: " << line << '\n';
    };

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    try {
        Network::NetworkServerHost host(ApexNetwork::makeServerConfig(), hostConfig);
        host.start();
        std::cout << "Session endpoints advertise host " << hostConfig.advertiseHost << '\n';
        std::cout << "Apex tower moving platforms are server-authored on real time.\n";

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        host.stop();
    } catch (const std::exception& exception) {
        std::cerr << "Apex network server error: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
