#include "FrostbrewGame.hpp"
#include "Components.hpp"
#include "Engine.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
class Run final : public Game {
public:
    Run(Engine& engine, bool smoke, std::string screenshot)
        : engine_(engine), game_(engine), smoke_(smoke), screenshot_(std::move(screenshot)) {}
    void onEvent(const SDL_Event& event) override { game_.onEvent(event); }
    void handleInput(Engine& engine) override { game_.handleInput(engine); }
    void update(float seconds, Engine& engine) override { game_.update(seconds, engine); }
    void update(const FrameTime& time, Engine& engine) override
    {
        auto& arena = game_.arena();
        if (smoke_ && arena.match().phase() == Frostbrew::Phase::Aim) {
            const auto* target = arena.world().find(arena.penguin(1 - arena.match().player()))->get<Transform>();
            float best = std::numeric_limits<float>::max(), angle = 45, power = 600;
            for (int a = 5; a <= 85; a += 5) for (int p = 200; p <= 700; p += 50) {
                arena.aim(static_cast<float>(a), static_cast<float>(p));
                const auto end = arena.preview().back();
                const float distance = std::hypot(end.x - target->x - 16, end.y - target->y - 16);
                if (distance < best) { best = distance; angle = static_cast<float>(a); power = static_cast<float>(p); }
            }
            arena.aim(angle, power);
            if (!arena.fire(Frostbrew::Ingredient::Ember, Frostbrew::Ingredient::Ember))
                throw std::runtime_error("Smoke launch failed");
        }
        game_.update(smoke_ ? FrameTime{1.0 / 60.0, frames_ * 16667} : time, engine);
        if (smoke_ && arena.match().phase() == Frostbrew::Phase::GameOver) {
            std::cout << "Frostbrew smoke: terminal match after " << arena.shot() << " shots; health "
                      << arena.match().health(0) << '/' << arena.match().health(1) << '\n';
            engine.quit();
        }
        if (smoke_ && ++frames_ > 20000) throw std::runtime_error("Smoke match did not terminate");
    }
    void render(SDL_Renderer* renderer) const override
    {
        game_.render(renderer);
        if (!screenshot_.empty()) {
            SDL_Surface* image = SDL_RenderReadPixels(renderer, nullptr);
            if (!image) throw std::runtime_error(SDL_GetError());
            const bool saved = SDL_SavePNG(image, screenshot_.c_str());
            SDL_DestroySurface(image);
            if (!saved) throw std::runtime_error(SDL_GetError());
            engine_.quit();
        }
    }
private:
    Engine& engine_;
    Frostbrew::DuelGame game_;
    bool smoke_;
    std::string screenshot_;
    std::int64_t frames_ = 0;
};
} // namespace

int main(int argc, char** argv)
{
    bool smoke = false;
    std::string screenshot;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--smoke") smoke = true;
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else {
            std::cerr << "Usage: frostbrew-duel [--smoke] [--screenshot output.png]\n";
            return 2;
        }
    }
    try {
        Engine engine("Frostbrew Duel", Frostbrew::Arena::Width, Frostbrew::Arena::Height);
        Run game(engine, smoke, screenshot);
        engine.run(game);
    } catch (const std::exception& error) {
        std::cerr << "Frostbrew: " << error.what() << '\n';
        return 1;
    }
}
