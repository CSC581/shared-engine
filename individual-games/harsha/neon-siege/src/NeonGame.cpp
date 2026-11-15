#include "NeonGame.hpp"
#include "Engine.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace Neon {
SiegeGame::SiegeGame(Engine& engine) : engine_(engine), arena_(std::make_unique<Arena>())
{
    engine_.registerChord("neon.shield.arrows", {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT});
    engine_.registerChord("neon.shield.letters", {SDL_SCANCODE_A, SDL_SCANCODE_D});
    subscription_ = engine_.events().subscribe(Events::Input, [this](const Event& event) { input(event); });
    engine_.setClearColor(8, 12, 30);
}
SiegeGame::~SiegeGame() { engine_.events().unsubscribe(subscription_); }
void SiegeGame::clearInput() { held_.fill(false); shield_ = false; }
void SiegeGame::input(const Event& event)
{
    if (event.origin != 0) return;
    const auto kind = event.getOr<std::string>("kind", "");
    const bool pressed = event.getOr<std::string>("state", "") == "pressed";
    if (kind == "chord") {
        const auto name = event.getOr<std::string>("name", "");
        if (pressed && !engine_.gameTime().isPaused() && (name == "neon.shield.arrows" || name == "neon.shield.letters"))
            shield_ = true;
        return;
    }
    if (kind != "key") return;
    const auto key = event.getOr<std::int64_t>("key", -1);
    if (key < 0 || key >= SDL_SCANCODE_COUNT) return;
    const bool repeated = held_[key] == pressed;
    held_[key] = pressed;
    if (!pressed || repeated) return;
    if (key == SDL_SCANCODE_ESCAPE) engine_.quit();
    else if (key == SDL_SCANCODE_P) {
        if (engine_.gameTime().isPaused()) engine_.gameTime().unpause(); else engine_.gameTime().pause();
        clearInput();
    } else if (key == SDL_SCANCODE_R) {
        arena_ = std::make_unique<Arena>(); engine_.gameTime().unpause(); clearInput();
    }
}
void SiegeGame::onEvent(const SDL_Event& event) { if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) clearInput(); }
void SiegeGame::handleInput(Engine&) {}
void SiegeGame::update(float seconds, Engine& engine) { update({seconds, engine.gameTime().now()}, engine); }
void SiegeGame::update(const FrameTime& time, Engine&)
{
    if (engine_.gameTime().isPaused()) return;
    const bool left = held_[SDL_SCANCODE_LEFT] || held_[SDL_SCANCODE_A];
    const bool right = held_[SDL_SCANCODE_RIGHT] || held_[SDL_SCANCODE_D];
    Controls controls{float(right - left), held_[SDL_SCANCODE_SPACE], shield_};
    if (autoplay_) {
        const auto* player = arena_->storage().get(arena_->player());
        float target = 480;
        for (std::size_t i = 0; i < arena_->storage().slots().size(); ++i) {
            const auto* slot = arena_->storage().get(arena_->storage().handle(i));
            if (slot && (slot->kind == Kind::Enemy || slot->kind == Kind::Boss)) {
                target = slot->body->x + slot->body->width / 2;
                break;
            }
        }
        const float delta = target - player->body->x - 16;
        controls = {std::clamp(delta / 12, -1.0F, 1.0F), true, arena_->rules().shieldCooldown() == 0};
    }
    arena_->update(time, controls);
    shield_ = false;
}
Arena& SiegeGame::arena() { return *arena_; }
void SiegeGame::setAutoplay(bool enabled) { autoplay_ = enabled; }
} // namespace Neon
