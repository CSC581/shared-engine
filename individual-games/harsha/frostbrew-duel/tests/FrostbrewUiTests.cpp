#include "FrostbrewGame.hpp"
#include "Components.hpp"
#include "Engine.hpp"
#include "Input.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
std::array<bool, SDL_SCANCODE_COUNT> keys{};
}
int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    try {
        Engine engine("Frostbrew input tests", 1200, 800);
        Input::setKeyboardStateSource(keys.data(), static_cast<int>(keys.size()));
        Input::update();
        for (const bool reverse : {false, true}) {
            Frostbrew::DuelGame game(engine);
            const auto frame = [&](double dt = 1.0 / 60.0) {
                Input::update();
                Input::raiseEvents(engine.events(), {{"frostbrew.focus", {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN}}});
                engine.events().dispatch();
                game.update(FrameTime{dt, 0}, engine);
                engine.events().dispatch();
            };
            const auto first = reverse ? SDL_SCANCODE_DOWN : SDL_SCANCODE_UP;
            const auto second = reverse ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN;
            keys[first] = true; frame();
            CHECK(game.arena().angle() != 45);
            const float angle = game.arena().angle();
            keys[second] = true; frame();
            CHECK(game.focused());
            CHECK(game.arena().angle() == angle);
            frame(); CHECK(game.arena().angle() == angle);
            keys[second] = false; frame();
            CHECK(!game.focused());
            keys.fill(false); frame();
            keys[SDL_SCANCODE_SPACE] = true; frame();
            CHECK(game.arena().match().phase() == Frostbrew::Phase::Resolving);
            keys[SDL_SCANCODE_P] = true; frame();
            CHECK(engine.gameTime().isPaused());
            const auto count = game.arena().projectileCount();
            frame(0.05); CHECK(game.arena().projectileCount() == count);
            keys[SDL_SCANCODE_R] = true; frame();
            CHECK(!engine.gameTime().isPaused());
            CHECK(game.arena().match().phase() == Frostbrew::Phase::Aim);
            CHECK(game.arena().world().objects().size() == 2);
            keys.fill(false); keys[SDL_SCANCODE_D] = true; frame();
            const auto* walker = game.arena().world().find(game.arena().penguin(0))->get<Transform>();
            CHECK(walker->x > 180 && game.arena().stride() < Frostbrew::Arena::Stride);
            keys[SDL_SCANCODE_D] = false; frame();
            SDL_Event lost{}; lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
            game.onEvent(lost); CHECK(!game.focused());
            game.render(engine.getRenderer());
            SDL_Surface* pixels = SDL_RenderReadPixels(engine.getRenderer(), nullptr);
            CHECK(pixels != nullptr);
            if (pixels) SDL_DestroySurface(pixels);
            keys.fill(false); frame(0);
        }
        const auto goodBase = SDL_GetBasePath();
        CHECK(goodBase != nullptr);
        Input::setKeyboardStateSource(nullptr, 0);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    std::cout << "Frostbrew UI: " << failures << " failures\n";
    return failures ? 1 : 0;
}
