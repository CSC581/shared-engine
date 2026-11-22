#include "FrostbrewGame.hpp"

#include "Engine.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace Frostbrew {
DuelGame::DuelGame(Engine& engine) : engine_(engine), arena_(engine.events())
{
    try {
        loadSprites();
        engine_.registerChord("frostbrew.focus", {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN});
        inputSubscription_ = engine_.events().subscribe(Events::Input, [this](const Event& event) { input(event); });
    } catch (...) {
        releaseSprites();
        throw;
    }
    engine_.setClearColor(16, 26, 48);
}
DuelGame::~DuelGame()
{
    engine_.events().unsubscribe(inputSubscription_);
    releaseSprites();
}
void DuelGame::loadSprites()
{
    const char* base = SDL_GetBasePath();
    if (!base) throw std::runtime_error(SDL_GetError());
    const std::array<const char*, 3> files{{"Idle.png", "Hurt.png", "Death.png"}};
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::string path = std::string(base) + "media/frostbrew-duel/" + files[i];
        SDL_Surface* surface = SDL_LoadPNG(path.c_str());
        if (!surface) throw std::runtime_error("Cannot load " + path + ": " + SDL_GetError());
        const int width = surface->w;
        if (width < 64 || width % 64 != 0 || surface->h != 64) {
            SDL_DestroySurface(surface);
            throw std::runtime_error("Invalid 64px penguin sheet: " + path);
        }
        sprites_[i] = SDL_CreateTextureFromSurface(engine_.getRenderer(), surface);
        SDL_DestroySurface(surface);
        if (!sprites_[i]) throw std::runtime_error("Cannot create penguin texture: " + std::string(SDL_GetError()));
        SDL_SetTextureScaleMode(sprites_[i], SDL_SCALEMODE_NEAREST);
        frameCounts_[i] = width / 64;
    }
}
void DuelGame::releaseSprites()
{
    for (auto*& texture : sprites_) {
        if (texture) SDL_DestroyTexture(texture);
        texture = nullptr;
    }
}
void DuelGame::input(const Event& event)
{
    if (event.origin != 0) return;
    const auto kind = event.getOr<std::string>("kind", "");
    const bool pressed = event.getOr<std::string>("state", "") == "pressed";
    if (kind == "chord") {
        if (event.getOr<std::string>("name", "") == "frostbrew.focus")
            focus_ = pressed && !engine_.gameTime().isPaused() && arena_.match().phase() == Phase::Aim;
        return;
    }
    if (kind != "key") return;
    const auto key = event.get<std::int64_t>("key");
    if (key < 0 || key >= SDL_SCANCODE_COUNT) return;
    const bool repeated = held_[key] == pressed;
    held_[key] = pressed;
    if (!pressed || repeated) return;
    if (key == SDL_SCANCODE_ESCAPE) { engine_.quit(); return; }
    if (key == SDL_SCANCODE_P) {
        if (engine_.gameTime().isPaused()) engine_.gameTime().unpause();
        else engine_.gameTime().pause();
        focus_ = false; fire_ = false;
        held_.fill(false);
        return;
    }
    if (key == SDL_SCANCODE_R) {
        arena_.reset();
        engine_.gameTime().unpause();
        ingredients_.fill(Ingredient::Ember); slot_ = 0;
        held_.fill(false); focus_ = false; fire_ = false; book_ = false; rejected_ = false;
        previousHealth_.fill(100); hurt_.fill(0); throwing_.fill(0); animation_ = 0;
        return;
    }
    if (engine_.gameTime().isPaused()) return;
    if (key == SDL_SCANCODE_B) { book_ = !book_; return; }
    if (arena_.match().phase() != Phase::Aim) return;
    if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_3) {
        ingredients_[slot_] = static_cast<Ingredient>(key - SDL_SCANCODE_1);
        slot_ = 1 - slot_;
    }
    if (key == SDL_SCANCODE_TAB) slot_ = 1 - slot_;
    if (key == SDL_SCANCODE_SPACE) fire_ = true;
}
void DuelGame::onEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        held_.fill(false); focus_ = false; fire_ = false;
    }
}
void DuelGame::handleInput(Engine&) {}
void DuelGame::update(float seconds, Engine& engine)
{
    update(FrameTime{seconds, engine.gameTime().now()}, engine);
}
void DuelGame::update(const FrameTime& time, Engine&)
{
    if (engine_.gameTime().isPaused()) return;
    if (arena_.match().phase() == Phase::Aim) {
        const float dt = static_cast<float>(std::min(time.dtSeconds, 0.05));
        const float angle = arena_.angle() + (focus_ ? 0.0F : float(held_[SDL_SCANCODE_UP] - held_[SDL_SCANCODE_DOWN]) * dt * 35);
        const float power = arena_.power() + float(held_[SDL_SCANCODE_RIGHT] - held_[SDL_SCANCODE_LEFT]) * dt * 180;
        arena_.aim(angle, power);
        if (fire_) {
            rejected_ = !arena_.fire(ingredients_[0], ingredients_[1]);
            if (!rejected_) throwing_[arena_.match().player()] = 0.3;
            focus_ = false;
            prediction_.clear();
        }
        // Half a second of flight (120 steps at 240 Hz) shows the launch, not the
        // landing, so aiming stays a skill.
        if (focus_) prediction_ = arena_.preview(120);
    }
    fire_ = false;
    const auto before = arena_.match().player();
    arena_.update(time);
    if (arena_.match().player() != before) {
        ingredients_.fill(Ingredient::Ember); slot_ = 0;
        focus_ = false; held_.fill(false); book_ = false;
    }
    animation_ += time.dtSeconds;
    for (std::size_t i = 0; i < 2; ++i) {
        hurt_[i] = std::max(0.0, hurt_[i] - time.dtSeconds);
        throwing_[i] = std::max(0.0, throwing_[i] - time.dtSeconds);
        if (arena_.match().health(i) < previousHealth_[i]) hurt_[i] = 0.45;
        previousHealth_[i] = arena_.match().health(i);
    }
}
Arena& DuelGame::arena() { return arena_; }
bool DuelGame::focused() const { return focus_; }
} // namespace Frostbrew
