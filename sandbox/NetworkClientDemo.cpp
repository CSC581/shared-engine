#include "Engine.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "NetworkClient.hpp"
#include "NetworkDemoConfig.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

namespace {

const char* stateLabel(Network::ConnectionState state)
{
    switch (state) {
    case Network::ConnectionState::Connecting:
        return "Connecting";
    case Network::ConnectionState::Connected:
        return "Connected";
    case Network::ConnectionState::Error:
        return "Error";
    case Network::ConnectionState::Disconnected:
        return "Disconnected";
    }
    return "Unknown";
}

class NetworkClientDemo final : public Game {
public:
    explicit NetworkClientDemo(std::string endpoint)
        : client_(std::move(endpoint))
    {
        client_.start();
    }

    ~NetworkClientDemo() override { client_.leave(); }

    void handleInput(Engine&) override
    {
        horizontal_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D));
        vertical_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_W, SDL_SCANCODE_S));
        if (horizontal_ == 0 && vertical_ == 0) {
            horizontal_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT));
            vertical_ = static_cast<int>(Input::getAxis(SDL_SCANCODE_UP, SDL_SCANCODE_DOWN));
        }
    }

    void update(float, Engine& engine) override
    {
        client_.poll();
        if (client_.state() == Network::ConnectionState::Disconnected ||
            client_.state() == Network::ConnectionState::Error) {
            client_.start();
        }
        client_.submitInput(horizontal_, vertical_);

        const std::string title = std::string("Network Client | ") + stateLabel(client_.state()) +
                                  " | Player " + std::to_string(client_.playerId()) + " | " +
                                  std::to_string(client_.snapshot().players.size()) + " players";
        SDL_SetWindowTitle(engine.getWindow(), title.c_str());
    }

    void render(SDL_Renderer* renderer) const override
    {
        SDL_SetRenderDrawColor(renderer, 48, 72, 105, 255);
        const SDL_FRect arena{NetworkDemo::arenaX, NetworkDemo::arenaY,
                              NetworkDemo::arenaWidth, NetworkDemo::arenaHeight};
        SDL_RenderRect(renderer, &arena);

        SDL_SetRenderDrawColor(renderer, 180, 205, 230, 255);
        SDL_RenderDebugTextFormat(renderer, 32.0F, 18.0F, "%s | WASD or arrows to move", stateLabel(client_.state()));

        if (client_.state() != Network::ConnectionState::Connected) {
            SDL_SetRenderDrawColor(renderer, 255, 205, 100, 255);
            SDL_RenderDebugText(renderer, 32.0F, 526.0F, "Start ./build/network-server to join the arena.");
        }

        for (const Network::PlayerState& player : client_.snapshot().players) {
            const SDL_FRect rect{NetworkDemo::arenaX + player.x, NetworkDemo::arenaY + player.y,
                                 NetworkDemo::playerSize, NetworkDemo::playerSize};
            const NetworkDemo::Color color = NetworkDemo::colorForPlayer(player.id);
            SDL_SetRenderDrawColor(renderer, color.red, color.green, color.blue, 255);
            SDL_RenderFillRect(renderer, &rect);

            if (player.id == client_.playerId()) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                SDL_RenderRect(renderer, &rect);
            }
        }
    }

private:
    Network::NetworkClient client_;
    int horizontal_ = 0;
    int vertical_ = 0;
};

} // namespace

int main(int argc, char* argv[])
{
    const std::string endpoint = argc > 1 ? argv[1] : "tcp://127.0.0.1:5555";
    Engine engine("Network Client", NetworkDemo::windowWidth, NetworkDemo::windowHeight);
    engine.setScaleToggleKey(SDL_SCANCODE_UNKNOWN);
    NetworkClientDemo game(endpoint);
    engine.run(game);
    return 0;
}
