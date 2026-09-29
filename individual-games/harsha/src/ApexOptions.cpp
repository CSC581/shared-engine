#include "ApexOptions.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

void printUsage()
{
    std::cout <<
        R"(Apex Ascent

  ./build/apex-ascent                              Offline
  ./build/apex-ascent --join [SERVER]              Legacy client-server shortcut
  ./build/apex-ascent --mode client-server         Client-server online mode
  ./build/apex-ascent --mode peer-to-peer [options]

Network options:
  --mode MODE       client-server or peer-to-peer
  --name TEXT       Display name
  --server ENDPOINT Dedicated/listen authority for shared platforms

Peer-to-peer options:
  --id N            Unique peer id, 1..4294967295
  --port P          Introduction port; P+1 broadcasts state (default 7200)
  --peer ENDPOINT   Introduction endpoint of any running peer
  --advertise HOST  Reachable host this peer advertises
  --host            Host the Apex platform authority in this process
)";
}

bool parseOptions(int argc, char* argv[], ApexOptions& options)
{
    options.network.basePort = 7200;

    auto value = [&](int& index, const char* flag, std::string& out) {
        if (index + 1 >= argc) {
            std::cerr << "apex-ascent: " << flag << " requires a value\n";
            return false;
        }
        out = argv[++index];
        return true;
    };

    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        std::string text;

        if (arg == "--help" || arg == "-h") {
            printUsage();
            options.help = true;
            return false;
        }
        if (arg == "--join") {
            options.online = true;
            options.network.mode = Multiplayer::Mode::ClientServer;
            if (index + 1 < argc && argv[index + 1][0] != '-') {
                options.network.serverEndpoint = argv[++index];
            }
            continue;
        }
        if (arg == "--mode") {
            if (!value(index, "--mode", text) || !Multiplayer::parseMode(text, options.network.mode)) {
                std::cerr << "apex-ascent: --mode must be client-server or peer-to-peer\n";
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--name") {
            if (!value(index, "--name", options.network.playerName)) {
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--server") {
            if (!value(index, "--server", options.network.serverEndpoint)) {
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--peer") {
            if (!value(index, "--peer", text)) {
                return false;
            }
            options.network.bootstrapPeers.push_back(text);
            options.online = true;
            continue;
        }
        if (arg == "--advertise") {
            if (!value(index, "--advertise", options.network.advertiseHost)) {
                return false;
            }
            options.online = true;
            continue;
        }
        if (arg == "--id") {
            if (!value(index, "--id", text)) {
                return false;
            }
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || parsed == 0 ||
                parsed > std::numeric_limits<Multiplayer::PlayerId>::max()) {
                std::cerr << "apex-ascent: --id must be a non-zero 32-bit integer\n";
                return false;
            }
            options.network.peerId = static_cast<Multiplayer::PlayerId>(parsed);
            options.online = true;
            continue;
        }
        if (arg == "--port") {
            if (!value(index, "--port", text)) {
                return false;
            }
            char* end = nullptr;
            const long parsed = std::strtol(text.c_str(), &end, 10);
            if (end == text.c_str() || *end != '\0' || parsed < 1024 || parsed > 65534) {
                std::cerr << "apex-ascent: --port must be between 1024 and 65534\n";
                return false;
            }
            options.network.basePort = static_cast<int>(parsed);
            options.online = true;
            continue;
        }
        if (arg == "--host") {
            options.hostAuthority = true;
            options.online = true;
            continue;
        }
        // Bare endpoint for convenience: ./apex-ascent tcp://127.0.0.1:5555
        if (arg.rfind("tcp://", 0) == 0) {
            options.online = true;
            options.network.mode = Multiplayer::Mode::ClientServer;
            options.network.serverEndpoint = arg;
            continue;
        }

        std::cerr << "apex-ascent: unknown option " << arg << "\n\n";
        printUsage();
        return false;
    }

    if (options.network.playerName.empty()) {
        options.network.playerName = "apex-" + std::to_string(options.network.peerId);
    }
    return true;
}
