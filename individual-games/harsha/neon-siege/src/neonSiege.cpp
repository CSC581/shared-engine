#include "NeonGame.hpp"
#include "Engine.hpp"
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
class Run final : public Game {
public:
    Run(Engine& engine, bool smoke, std::string screenshot)
        : engine_(engine), game_(engine), smoke_(smoke), screenshot_(std::move(screenshot)) { game_.setAutoplay(smoke); }
    void onEvent(const SDL_Event& event) override { game_.onEvent(event); }
    void handleInput(Engine& engine) override { game_.handleInput(engine); }
    void update(float seconds, Engine& engine) override { game_.update(seconds, engine); }
    void update(const FrameTime& time, Engine& engine) override {
        game_.update(smoke_ ? FrameTime{1.0 / 60, frames_ * 16667} : time, engine);
        if (smoke_ && game_.arena().rules().phase() != Neon::Phase::Playing) {
            if (game_.arena().rules().wave() != 3 || game_.arena().rules().score() < 300)
                throw std::runtime_error("Neon smoke ended before clearing both formations");
            std::cout << "Neon smoke: " << (game_.arena().rules().phase() == Neon::Phase::Won ? "won" : "lost")
                      << " wave " << game_.arena().rules().wave() << " score " << game_.arena().rules().score() << '\n';
            engine.quit();
        }
        if (smoke_ && ++frames_ > 18000) throw std::runtime_error("Neon smoke match did not terminate");
    }
    void render(SDL_Renderer* renderer) const override {
        game_.render(renderer);
        if (!screenshot_.empty()) {
            SDL_Surface* image = SDL_RenderReadPixels(renderer, nullptr);
            if (!image) throw std::runtime_error(SDL_GetError());
            const bool saved = SDL_SavePNG(image, screenshot_.c_str()); SDL_DestroySurface(image);
            if (!saved) throw std::runtime_error(SDL_GetError());
            engine_.quit();
        }
    }
private:
    Engine& engine_; Neon::SiegeGame game_; bool smoke_; std::string screenshot_; std::int64_t frames_ = 0;
};
}
int main(int argc, char** argv)
{
    bool smoke = false; std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke") smoke = true;
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else {
            std::cout << "Usage: neon-siege [--smoke] [--screenshot output.png]\n";
            return arg == "--help" ? 0 : 2;
        }
    }
    try {
        Engine engine("Neon Siege", Neon::Arena::Width, Neon::Arena::Height);
        Run game(engine, smoke, screenshot); engine.run(game);
    } catch (const std::exception& error) { std::cerr << "Neon Siege: " << error.what() << '\n'; return 1; }
}
