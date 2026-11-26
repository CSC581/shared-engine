#include "NeonGame.hpp"
#include "Engine.hpp"
#include "Input.hpp"
#include <SDL3/SDL.h>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
}
int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"); SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    Input::setKeyboardStateSource(keys.data(), SDL_SCANCODE_COUNT);
    {
        Engine engine("Neon UI tests", Neon::Arena::Width, Neon::Arena::Height);
        Neon::SiegeGame game(engine);
        const std::vector<InputChord> chords{{"neon.shield.arrows", {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT}, Neon::SiegeGame::ShieldWindowMs},
                                             {"neon.shield.letters", {SDL_SCANCODE_A, SDL_SCANCODE_D}, Neon::SiegeGame::ShieldWindowMs}};
        const auto frame = [&] {
            Input::update(); Input::raiseEvents(engine.events(), chords); engine.events().dispatch();
            game.update({1.0 / 60, 0}, engine);
        };
        frame();
        keys[SDL_SCANCODE_LEFT] = true; frame();
        const auto x = game.arena().storage().get(game.arena().player())->body->x;
        keys[SDL_SCANCODE_RIGHT] = true; frame();
        CHECK(game.arena().storage().get(game.arena().player())->body->x == x);
        CHECK(game.arena().rules().shieldTicks() > 0);
        for (int i = 0; i < 200; ++i) frame();
        CHECK(game.arena().rules().shieldTicks() == 0);
        keys[SDL_SCANCODE_P] = true; frame(); CHECK(engine.gameTime().isPaused());
        const auto paused = game.arena().digest();
        for (int i = 0; i < 10; ++i) frame();
        CHECK(game.arena().digest() == paused);
        keys[SDL_SCANCODE_R] = true; frame(); CHECK(!engine.gameTime().isPaused());
        CHECK(game.arena().rules().wave() == 1 && game.arena().rules().lives() == 3);
        keys.fill(false); frame();
        // Holding Left, then pressing Right later, is a turn and must not shield.
        keys[SDL_SCANCODE_LEFT] = true; frame();
        SDL_Delay(150);
        keys[SDL_SCANCODE_RIGHT] = true; frame();
        CHECK(game.arena().rules().shieldTicks() == 0);
        keys.fill(false); frame();
        keys[SDL_SCANCODE_D] = true; frame();
        keys[SDL_SCANCODE_A] = true; frame(); CHECK(game.arena().rules().shieldTicks() > 0);
        SDL_Event lost{}; lost.type = SDL_EVENT_WINDOW_FOCUS_LOST; game.onEvent(lost);
        const auto still = game.arena().storage().get(game.arena().player())->body->x;
        game.update({1.0 / 60, 0}, engine);
        CHECK(game.arena().storage().get(game.arena().player())->body->x == still);
        game.render(engine.getRenderer());
        SDL_Surface* image = SDL_RenderReadPixels(engine.getRenderer(), nullptr);
        CHECK(image != nullptr);
        if (image) {
            Uint8 r = 0, g = 0, b = 0, a = 0;
            CHECK(SDL_ReadSurfacePixel(image, 480, 666, &r, &g, &b, &a));
            CHECK(g > 100 && b > 100);
            SDL_DestroySurface(image);
        }
        keys.fill(false); Input::update();
    }
    Input::setKeyboardStateSource(nullptr, 0);
    std::cout << "Neon UI: " << failures << " failures\n";
    return failures != 0;
}
