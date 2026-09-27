#include "SignalBloomWorld.hpp"

#include "NetworkServerHost.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> running{true};

void stopServer(int)
{
    running.store(false);
}

} // namespace

int main(int argc, char* argv[])
{
    Network::HostConfig hostConfig;
    hostConfig.mode = Network::HostMode::Dedicated;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--bind" || arg == "--advertise") && i + 1 < argc) {
            (arg == "--bind" ? hostConfig.bindEndpoint : hostConfig.advertiseHost) = argv[++i];
        } else {
            std::cerr << "Usage: signal-bloom-server [--bind tcp://*:5555] "
                         "[--advertise reachable-host]\n";
            return 1;
        }
    }

    if (hostConfig.advertiseHost.empty() || hostConfig.advertiseHost == "0.0.0.0" ||
        hostConfig.advertiseHost == "*") {
        std::cerr << "--advertise must be an address the clients can reach\n";
        return 1;
    }

    std::signal(SIGINT, stopServer);
    std::signal(SIGTERM, stopServer);

    try {
        hostConfig.log = [](const std::string& line) {
            std::cout << "signal-bloom-server: " << line << '\n';
        };
        Network::NetworkServerHost host(SignalBloom::serverConfig(), hostConfig);
        host.start();

        std::cout << "Private client workers advertise " << hostConfig.advertiseHost << '\n'
                  << "GET_WORLD reads shuttles only; player updates use private workers.\n";
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        host.stop();
    } catch (const std::exception& exception) {
        std::cerr << "Signal Bloom server: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
