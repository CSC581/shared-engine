#include "Engine.hpp"
#include "Multiplayer.hpp"
#include "StarfallSalvageGame.hpp"
#include "StarfallSalvageWorld.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

bool endpointValid(const std::string& value)
{
    if (value.rfind("tcp://", 0) != 0) return false;
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon <= 6 || colon + 1 >= value.size()) return false;
    const std::string port = value.substr(colon + 1);
    if (!std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }
    try {
        const int number = std::stoi(port);
        return number > 0 && number <= 65535;
    } catch (const std::exception&) {
        return false;
    }
}

bool number(const std::string& text, std::uint32_t& out)
{
    try {
        std::size_t used = 0;
        const unsigned long long value = std::stoull(text, &used);
        if (used != text.size() || value == 0 ||
            value > std::numeric_limits<std::uint32_t>::max()) return false;
        out = static_cast<std::uint32_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void usage()
{
    std::cerr << "Usage: starfall-salvage --name NAME [--mode client-server|peer-to-peer] "
                 "[--server tcp://HOST:PORT] [--scale 0.5|1|2] [--peer-id ID --port PORT "
                 "--bootstrap tcp://HOST:PORT --advertise HOST]\n";
}

} // namespace

int main(int argc, char* argv[])
{
    Multiplayer::Config config;
    bool peerIdGiven = false;
    bool portGiven = false;
    bool nameGiven = false;
    double initialScale = 1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            usage();
            return 1;
        }
        const std::string value = argv[++i];
        if (arg == "--name") {
            config.playerName = value;
            nameGiven = true;
        } else if (arg == "--mode") {
            if (!Multiplayer::parseMode(value, config.mode)) {
                std::cerr << "Mode must be client-server or peer-to-peer\n";
                return 1;
            }
        } else if (arg == "--server") {
            config.serverEndpoint = value;
        } else if (arg == "--scale") {
            if (value == "0.5") initialScale = 0.5;
            else if (value == "1") initialScale = 1.0;
            else if (value == "2") initialScale = 2.0;
            else {
                std::cerr << "Initial scale must be 0.5, 1, or 2\n";
                return 1;
            }
        } else if (arg == "--peer-id") {
            peerIdGiven = number(value, config.peerId);
            if (!peerIdGiven) {
                std::cerr << "Peer ID must be a positive 32-bit integer\n";
                return 1;
            }
        } else if (arg == "--port") {
            std::uint32_t port = 0;
            portGiven = number(value, port) && port >= 1024 && port < 65534;
            if (!portGiven) {
                std::cerr << "Peer base port must be between 1024 and 65533\n";
                return 1;
            }
            config.basePort = static_cast<int>(port);
        } else if (arg == "--bootstrap") {
            if (!endpointValid(value)) {
                std::cerr << "Bootstrap must be a tcp://HOST:PORT endpoint\n";
                return 1;
            }
            config.bootstrapPeers.push_back(value);
        } else if (arg == "--advertise") {
            config.advertiseHost = value;
        } else {
            usage();
            return 1;
        }
    }

    if (!nameGiven || config.playerName.empty() || config.playerName.size() > 24 ||
        !std::all_of(config.playerName.begin(), config.playerName.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_';
        })) {
        std::cerr << "--name is required (1-24 letters, digits, '-' or '_')\n";
        return 1;
    }
    if (!endpointValid(config.serverEndpoint)) {
        std::cerr << "--server must be a tcp://HOST:PORT endpoint\n";
        return 1;
    }
    if (config.mode == Multiplayer::Mode::PeerToPeer &&
        (!peerIdGiven || !portGiven || config.advertiseHost.empty() ||
         config.advertiseHost == "0.0.0.0")) {
        std::cerr << "Peer mode needs unique --peer-id and --port and a reachable --advertise host\n";
        return 1;
    }

    try {
        const std::string title = "Starfall Salvage - " + config.playerName;
        Engine engine(title.c_str(), StarfallSalvage::width, StarfallSalvage::height);
        engine.gameTime().setScale(initialScale);
        StarfallSalvageGame game(engine, std::move(config));
        engine.run(game);
    } catch (const std::exception& exception) {
        std::cerr << "Starfall Salvage: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
