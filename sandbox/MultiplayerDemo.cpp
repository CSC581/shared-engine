// One game, either network architecture, chosen at the command line.
//
//   ./build/multiplayer-demo --mode client-server
//   ./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200
//
// The point of this demo is what is *not* in the game class below. Search it
// for "Network", "Peer" or "zmq" and there is nothing to find: no join, no
// handshake, no roster, no snapshot, no sequence numbers, no sockets. It asks
// a Multiplayer::Session where everybody is and tells it where its own player
// is, and that same code runs over a central server or over a peer mesh
// depending on one enum filled in by main().
//
// Everything architecture-specific lives in the option parsing, which is to
// say in the deployment decision rather than in the game.

#include "Engine.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "ListenServer.hpp"
#include "Multiplayer.hpp"
#include "NetworkDemoConfig.hpp"
#include "WireFormat.hpp"
#include "TimeSource.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

namespace {

// ---------------------------------------------------------------------------
// The game. Nothing below knows how its data reaches anybody.
// ---------------------------------------------------------------------------
class MultiplayerDemo final : public Game {
public:
    MultiplayerDemo(std::unique_ptr<Multiplayer::Session> session, float spawnX, float spawnY,
                    const TimeSource& realTime, double secondsUntilQuit)
        : session_(std::move(session)), x_(spawnX), y_(spawnY), realTime_(realTime),
          startedAt_(realTime.now()), secondsUntilQuit_(secondsUntilQuit)
    {
    }

    void handleInput(Engine& engine) override
    {
        Timeline& gameTime = engine.gameTime();
        if (Input::isKeyJustPressed(SDL_SCANCODE_P)) {
            gameTime.togglePause();
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_1)) {
            gameTime.setScale(0.5);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_2)) {
            gameTime.setScale(1.0);
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_3)) {
            gameTime.setScale(2.0);
        }

        horizontal_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D));
        vertical_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_W, SDL_SCANCODE_S));
        if (horizontal_ == 0 && vertical_ == 0) {
            horizontal_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT));
            vertical_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_UP, SDL_SCANCODE_DOWN));
        }
    }

    // Never reached: the engine calls the FrameTime overload.
    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine& engine) override
    {
        // Pump the network every frame, including while paused — a session
        // that stopped being pumped would time out while the game sat still.
        session_->update();

        // Move on game time, so this player freezes when the game is paused
        // and halves when it is slowed, exactly as in a single-player game.
        float dx = static_cast<float>(std::clamp(horizontal_, -1, 1));
        float dy = static_cast<float>(std::clamp(vertical_, -1, 1));
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 1.0F) {
            dx /= length;
            dy /= length;
        }
        if (dx != 0.0F || dy != 0.0F) {
            facing_ = std::atan2(dy, dx);
        }
        const float step = static_cast<float>(time.dtSeconds) * NetworkDemo::playerSpeed;
        x_ = std::clamp(x_ + dx * step, 0.0F, NetworkDemo::arenaWidth - NetworkDemo::playerSize);
        y_ = std::clamp(y_ + dy * step, 0.0F, NetworkDemo::arenaHeight - NetworkDemo::playerSize);

        distance_ += std::abs(dx * step) + std::abs(dy * step);

        // Every frame, stationary or not: this is also what says "still here".
        session_->publishLocalPlayer(x_, y_, encodeOurPlayer());

        peakPlayers_ = std::max(peakPlayers_, session_->remotePlayers().size());
        peakPlatforms_ = std::max(peakPlatforms_, session_->platforms().size());

        updateTitle(engine);
        if (secondsUntilQuit_ > 0.0 &&
            realTime_.now() - startedAt_ >=
                static_cast<std::int64_t>(secondsUntilQuit_ * static_cast<double>(kNsPerSec))) {
            engine.quit();
        }
    }

    void render(SDL_Renderer* renderer) const override
    {
        SDL_SetRenderDrawColor(renderer, 48, 72, 105, 255);
        const SDL_FRect arena{NetworkDemo::arenaX, NetworkDemo::arenaY, NetworkDemo::arenaWidth,
                              NetworkDemo::arenaHeight};
        SDL_RenderRect(renderer, &arena);

        for (const Multiplayer::Platform& platform : session_->platforms()) {
            const SDL_FRect rect{NetworkDemo::arenaX + platform.x, NetworkDemo::arenaY + platform.y,
                                 platform.width, platform.height};
            SDL_SetRenderDrawColor(renderer, 90, 140, 110, 255);
            SDL_RenderFillRect(renderer, &rect);
            SDL_SetRenderDrawColor(renderer, 200, 230, 210, 255);
            SDL_RenderRect(renderer, &rect);
        }

        // remotePlayers() never contains this player, so there is no filtering
        // to do and no risk of drawing our own character twice.
        for (const Multiplayer::Player& player : session_->remotePlayers()) {
            drawPlayer(renderer, player.x, player.y, player.id, false);
            drawLabel(renderer, player);
        }
        drawPlayer(renderer, x_, y_, session_->localPlayerId(), true);

        SDL_SetRenderDrawColor(renderer, 190, 210, 235, 255);
        SDL_RenderDebugTextFormat(renderer, 32.0F, 12.0F, "%s  |  player %u  |  %zu other player(s)",
                                  Multiplayer::modeName(session_->mode()),
                                  session_->localPlayerId(), session_->remotePlayers().size());
        SDL_RenderDebugText(renderer, 32.0F, 28.0F,
                            "score and facing are this game's own format, carried verbatim");

        SDL_SetRenderDrawColor(renderer, 150, 170, 195, 255);
        SDL_RenderDebugText(renderer, 32.0F, 516.0F,
                            "WASD/arrows move | P pause | 1/2/3 = 0.5x 1x 2x");

        if (session_->state() != Multiplayer::State::Ready) {
            SDL_SetRenderDrawColor(renderer, session_->state() == Multiplayer::State::Failed ? 255 : 255,
                                   session_->state() == Multiplayer::State::Failed ? 110 : 205,
                                   session_->state() == Multiplayer::State::Failed ? 110 : 100, 255);
            SDL_RenderDebugText(renderer, 32.0F, 500.0F, session_->status().c_str());
        }
    }

    void printSummary() const
    {
        std::cout << "multiplayer-demo summary: mode=" << Multiplayer::modeName(session_->mode())
                  << " player=" << session_->localPlayerId() << " other-players=" << peakPlayers_
                  << " platforms=" << peakPlatforms_ << " status=\"" << session_->status() << "\""
                  << std::endl;
    }

private:
    // ---- This game's own player format, start to finish. ----
    //
    // Four lines to write it and four to read it, in this file, understood
    // nowhere else. The engine, the server and the peer module carry the
    // string and never look inside, so adding a field here is a change to this
    // file alone — no protocol version, no engine header, nothing to rebuild
    // but the game.
    //
    // Note Net::formatFloat rather than std::to_string: the latter rounds a
    // float to six significant figures, and both it and std::stof follow the
    // global locale, so a machine set to decimal commas would write "1,5" and
    // every other machine would misread it. Games are welcome to use anything
    // else — this is one game's choice, not the engine's.
    std::string encodeOurPlayer() const
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << static_cast<long long>(distance_) / 10 << ' ' << Net::formatFloat(facing_);
        return out.str();
    }

    struct RemoteLabel {
        long long score = 0;
        float facing = 0.0F;
    };

    static bool decodePlayer(const std::string& data, RemoteLabel& label)
    {
        std::istringstream in(data);
        in.imbue(std::locale::classic());
        std::string facingField;
        return static_cast<bool>(in >> label.score >> facingField) &&
               Net::parseFloat(facingField, label.facing);
    }

    // Reads back what this game sent, and draws it. Nothing between here and
    // the other player's process understood any of it.
    static void drawLabel(SDL_Renderer* renderer, const Multiplayer::Player& player)
    {
        RemoteLabel label;
        if (!decodePlayer(player.data, label)) {
            // A player whose data this game cannot read is still drawn as a
            // player, just without its label. Never a reason to drop it.
            return;
        }

        SDL_SetRenderDrawColor(renderer, 225, 235, 250, 255);
        SDL_RenderDebugTextFormat(renderer, NetworkDemo::arenaX + player.x - 4.0F,
                                  NetworkDemo::arenaY + player.y - 14.0F, "%s %lld",
                                  player.name.empty() ? "?" : player.name.c_str(), label.score);
    }

    static void drawPlayer(SDL_Renderer* renderer, float x, float y, Multiplayer::PlayerId id,
                           bool isLocal)
    {
        const SDL_FRect rect{NetworkDemo::arenaX + x, NetworkDemo::arenaY + y,
                             NetworkDemo::playerSize, NetworkDemo::playerSize};
        const NetworkDemo::Color color = NetworkDemo::colorForPlayer(id == 0 ? 1 : id);
        SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
        SDL_RenderFillRect(renderer, &rect);
        if (isLocal) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderRect(renderer, &rect);
        }
    }

    void updateTitle(Engine& engine) const
    {
        std::ostringstream title;
        title << "Multiplayer (" << Multiplayer::modeName(session_->mode()) << ") | player "
              << session_->localPlayerId() << " | " << session_->remotePlayers().size() + 1
              << " in session | ";
        if (engine.gameTime().isPaused()) {
            title << "PAUSED";
        } else {
            title << engine.gameTime().scale() << "x";
        }
        SDL_SetWindowTitle(engine.getWindow(), title.str().c_str());
    }

    std::unique_ptr<Multiplayer::Session> session_;
    float x_ = 0.0F;
    float y_ = 0.0F;
    const TimeSource& realTime_;
    std::int64_t startedAt_ = 0;
    double secondsUntilQuit_ = 0.0;
    float facing_ = 0.0F;
    float distance_ = 0.0F;
    std::size_t peakPlayers_ = 0;
    std::size_t peakPlatforms_ = 0;
    int horizontal_ = 0;
    int vertical_ = 0;
};

// ---------------------------------------------------------------------------
// Everything architecture-specific: the deployment decision, and nothing else.
// ---------------------------------------------------------------------------
void printUsage()
{
    std::cout <<
        R"(multiplayer-demo — the same game over either network architecture

  --mode MODE       client-server (default) or peer-to-peer.
  --name TEXT       Display name.
  --server ENDPOINT client-server: the server to join.
                    peer-to-peer: the authority owning the moving platforms.
                    Pass "none" for a peer session with no shared objects and
                    no server process at all.
                    Default tcp://127.0.0.1:5555.
  --seconds N       Run N seconds, print a summary and exit (for scripts).

Peer-to-peer only:
  --id N            This player's id, 1..8, unique across the session.
  --port P          Base port; this peer uses P and P+1. Default 7200.
  --peer ENDPOINT   Any running peer's address, e.g. tcp://127.0.0.1:7200.
  --advertise HOST  Address other machines should dial this peer at.
  --host            Host the shared-world authority inside this process
                    (a listen-server) instead of running network-server
                    separately. Section 5 allows either; with this, one of
                    the players carries the platforms and the players still
                    go peer to peer.

Client-server (start ./build/network-server first):
  ./build/multiplayer-demo --mode client-server
  ./build/multiplayer-demo --mode client-server

Peer-to-peer, same game, platforms still from the server:
  ./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200
  ./build/multiplayer-demo --mode peer-to-peer --id 2 --port 7202 --peer tcp://127.0.0.1:7200
)";
}

bool parseOptions(int argc, char* argv[], Multiplayer::Config& config, double& seconds, bool& help,
                  bool& hostAuthority)
{
    help = false;
    hostAuthority = false;
    config.basePort = 7200;

    auto value = [&](int& index, const char* flag, std::string& out) {
        if (index + 1 >= argc) {
            std::cerr << "multiplayer-demo: " << flag << " requires a value\n";
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
            help = true;
            return false;
        }
        if (arg == "--mode") {
            if (!value(index, "--mode", text)) {
                return false;
            }
            if (!Multiplayer::parseMode(text, config.mode)) {
                std::cerr << "multiplayer-demo: --mode must be client-server or peer-to-peer\n";
                return false;
            }
            continue;
        }
        if (arg == "--name") {
            if (!value(index, "--name", config.playerName)) {
                return false;
            }
            continue;
        }
        if (arg == "--server") {
            if (!value(index, "--server", text)) {
                return false;
            }
            // "none" rather than an empty string, which a shell makes awkward
            // to pass and which reads as a mistake on a command line.
            config.serverEndpoint = text == "none" ? std::string() : text;
            continue;
        }
        if (arg == "--peer") {
            if (!value(index, "--peer", text)) {
                return false;
            }
            config.bootstrapPeers.push_back(text);
            continue;
        }
        if (arg == "--advertise") {
            if (!value(index, "--advertise", config.advertiseHost)) {
                return false;
            }
            continue;
        }
        if (arg == "--id") {
            if (!value(index, "--id", text)) {
                return false;
            }
            const long parsed = std::strtol(text.c_str(), nullptr, 10);
            if (parsed < 1 || parsed > 8) {
                std::cerr << "multiplayer-demo: --id must be between 1 and 8\n";
                return false;
            }
            config.peerId = static_cast<Multiplayer::PlayerId>(parsed);
            continue;
        }
        if (arg == "--port") {
            if (!value(index, "--port", text)) {
                return false;
            }
            const long parsed = std::strtol(text.c_str(), nullptr, 10);
            if (parsed < 1024 || parsed > 65534) {
                std::cerr << "multiplayer-demo: --port must be between 1024 and 65534\n";
                return false;
            }
            config.basePort = static_cast<int>(parsed);
            continue;
        }
        if (arg == "--host") {
            hostAuthority = true;
            continue;
        }
        if (arg == "--seconds") {
            if (!value(index, "--seconds", text)) {
                return false;
            }
            seconds = std::strtod(text.c_str(), nullptr);
            if (!(seconds > 0.0)) {
                std::cerr << "multiplayer-demo: --seconds must be positive\n";
                return false;
            }
            continue;
        }

        std::cerr << "multiplayer-demo: unknown option " << arg << "\n\n";
        printUsage();
        return false;
    }

    if (config.playerName.empty()) {
        config.playerName = "player-" + std::to_string(config.peerId);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    Multiplayer::Config config;
    double seconds = 0.0;
    bool help = false;
    bool hostAuthority = false;
    if (!parseOptions(argc, argv, config, seconds, help, hostAuthority)) {
        return help ? 0 : 1;
    }

    // The listen-server: the shared-world authority carried by one of the
    // players rather than by a process of its own. It has to be running before
    // the session opens, since the session starts connecting immediately.
    std::unique_ptr<ListenServer> listenServer;
    if (hostAuthority) {
        if (config.serverEndpoint.empty()) {
            std::cerr << "multiplayer-demo: --host needs somewhere to bind; drop --server none\n";
            return 1;
        }
        std::string bind = config.serverEndpoint;
        const std::size_t colon = bind.rfind(':');
        if (colon != std::string::npos) {
            bind = "tcp://*" + bind.substr(colon);
        }
        try {
            listenServer = std::make_unique<ListenServer>(bind, NetworkDemo::makeServerConfig());
            listenServer->start();
            std::cout << "Hosting the shared-world authority (listen-server) on " << bind << '\n';
        } catch (const std::exception& exception) {
            std::cerr << "multiplayer-demo: could not host the authority on " << bind << ": "
                      << exception.what() << "\nIs something already hosting?\n";
            return 1;
        }
    }

    // Spread players out so two of them do not start on top of one another.
    // In client-server mode the id is not known until the server assigns one,
    // so this is only ever a starting guess either way.
    const int slot = static_cast<int>(config.peerId - 1) % 4;
    const float spawnX = 48.0F + static_cast<float>(slot) * 261.0F;

    try {
        Engine engine("Multiplayer", NetworkDemo::windowWidth, NetworkDemo::windowHeight);
        engine.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);

        std::cout << "Starting in " << Multiplayer::modeName(config.mode) << " mode\n";
        MultiplayerDemo game(Multiplayer::Session::open(config, engine.realTime()), spawnX, 48.0F,
                             engine.realTime(), seconds);
        engine.run(game);
        game.printSummary();
    } catch (const std::exception& exception) {
        std::cerr << "multiplayer-demo: " << exception.what() << '\n';
        return 1;
    }

    return 0;
}
