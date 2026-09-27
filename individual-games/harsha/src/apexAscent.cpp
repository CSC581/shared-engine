// Apex Ascent entry point: parse the command line, optionally host the
// platform authority in-process, then run the game offline or online.

#include "ApexGame.hpp"
#include "ApexNetworkConfig.hpp"
#include "ApexOptions.hpp"
#include "Endpoint.hpp"
#include "Engine.hpp"
#include "Multiplayer.hpp"
#include "NetworkServerHost.hpp"

#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

int main(int argc, char* argv[])
{
    try {
        ApexOptions options;
        if (!parseOptions(argc, argv, options)) {
            return options.help ? 0 : 1;
        }

        // Listen-server: the engine hosts the platform authority in this process.
        std::unique_ptr<Network::NetworkServerHost> listenServer;
        if (options.hostAuthority) {
            if (options.network.mode != Multiplayer::Mode::PeerToPeer) {
                std::cerr << "apex-ascent: --host requires --mode peer-to-peer\n";
                return 1;
            }

            const std::string bindEndpoint =
                Net::wildcardBindEndpoint(options.network.serverEndpoint);
            if (bindEndpoint.empty()) {
                std::cerr << "apex-ascent: --host requires a tcp server endpoint with a port\n";
                return 1;
            }
            try {
                Network::HostConfig hostConfig;
                hostConfig.mode = Network::HostMode::Listen;
                hostConfig.bindEndpoint = bindEndpoint;
                listenServer = std::make_unique<Network::NetworkServerHost>(
                    ApexNetwork::makeServerConfig(), hostConfig);
                listenServer->start();
            } catch (const std::exception& error) {
                std::cerr << "apex-ascent: could not host the platform authority on "
                          << bindEndpoint << ": " << error.what()
                          << "\nIs something already hosting?\n";
                return 1;
            }
            std::cout << "Hosting Apex platform authority on " << bindEndpoint << '\n';
        }

        // 900x900 design resolution (fits ordinary screens in Constant mode).
        Engine engine(options.online ? "Apex Ascent (online)" : "Apex Ascent", 900, 900);
        engine.setClearColor(18, 20, 32);

        std::unique_ptr<Multiplayer::Session> session;
        if (options.online) {
            std::cout << "Starting Apex in " << Multiplayer::modeName(options.network.mode)
                      << " mode\n";
            session = Multiplayer::Session::open(std::move(options.network), engine.realTime());
        }

        ApexGame game(engine, std::move(session));
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Apex Ascent failed to start: " << error.what() << '\n';
        return 1;
    }
}
