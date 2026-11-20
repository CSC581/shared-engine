#include "GardenDefenseGame.hpp"

#include "ArcadeDraw.hpp"
#include "Components.hpp"
#include "Engine.hpp"
#include "GardenDefenseRules.hpp"
#include "TimeUnits.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace GardenDefense;

constexpr Color background{28, 44, 38};
constexpr Color cream{240, 232, 200};
constexpr Color dim{150, 160, 140};
constexpr Color wood{196, 160, 110};
constexpr Color soil{92, 62, 40};
constexpr Color ink{20, 22, 18};
constexpr Color limb{205, 190, 150};
constexpr Color tin{150, 180, 200};
constexpr Color tinDark{90, 115, 135};
constexpr Color leaf{80, 170, 80};
constexpr Color carrot{240, 130, 40};
constexpr Color ready{130, 220, 120};

using Arcade::rect;
using Arcade::shade;
using Arcade::text;

constexpr float screenWidth = static_cast<float>(width);

void centeredText(SDL_Renderer* renderer, float y, const std::string& message, Color color)
{
    Arcade::centeredText(renderer, screenWidth, y, message, color);
}

void notice(SDL_Renderer* renderer, float y, const std::string& message, Color box, Color color)
{
    Arcade::notice(renderer, screenWidth, y, message, box, color);
}

std::string format(const char* pattern, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

// Six legs that swap stance with the march.
void legs(SDL_Renderer* renderer, float x, float y, float w, float h, bool frame, Color color)
{
    for (int i = 0; i < 3; ++i) {
        const float legY = y + h * (0.3F + 0.25F * static_cast<float>(i));
        const float reach = ((i % 2 == 0) == frame) ? 5.0F : 3.0F;
        rect(renderer, x - reach, legY, reach, 2.0F, color);
        rect(renderer, x + w, legY, reach, 2.0F, color);
    }
}

void drawPest(SDL_Renderer* renderer, const Transform& t, int row, bool frame)
{
    const Color body = pestColor(row);
    if (row == 0) {
        // Beetle: a split shell with a dark head.
        legs(renderer, t.x + 6.0F, t.y + 4.0F, t.width - 12.0F, t.height - 6.0F, frame, limb);
        rect(renderer, t.x + 6.0F, t.y + 5.0F, t.width - 12.0F, t.height - 6.0F, body);
        rect(renderer, t.x + t.width * 0.5F - 1.0F, t.y + 6.0F, 2.0F, t.height - 8.0F, ink);
        rect(renderer, t.x + t.width * 0.5F - 5.0F, t.y, 10.0F, 6.0F, {95, 65, 160});
    } else if (row == 1) {
        // Caterpillar: segments that bob in turn.
        for (int i = 0; i < 4; ++i) {
            const float bob = ((i % 2 == 0) == frame) ? 0.0F : 3.0F;
            rect(renderer, t.x + 1.0F + 8.0F * static_cast<float>(i), t.y + 8.0F + bob, 8.0F, 10.0F, body);
        }
        rect(renderer, t.x + 25.0F, t.y + 3.0F, 8.0F, 8.0F, body);
        rect(renderer, t.x + 29.0F, t.y + 5.0F, 2.0F, 2.0F, ink);
        rect(renderer, t.x + 27.0F, t.y, 1.0F, 4.0F, limb);
        rect(renderer, t.x + 31.0F, t.y, 1.0F, 4.0F, limb);
    } else {
        // Aphid: a round body on six legs.
        legs(renderer, t.x + 9.0F, t.y + 6.0F, t.width - 18.0F, t.height - 8.0F, frame, limb);
        rect(renderer, t.x + 9.0F, t.y + 6.0F, t.width - 18.0F, t.height - 8.0F, body);
        rect(renderer, t.x + 7.0F, t.y + 9.0F, t.width - 14.0F, t.height - 14.0F, body);
        rect(renderer, t.x + 13.0F, t.y + 10.0F, 3.0F, 3.0F, ink);
        rect(renderer, t.x + t.width - 16.0F, t.y + 10.0F, 3.0F, 3.0F, ink);
    }
}

void drawLadybug(SDL_Renderer* renderer, const Transform& t)
{
    rect(renderer, t.x + 2.0F, t.y + 4.0F, t.width - 4.0F, t.height - 4.0F, {225, 55, 55});
    rect(renderer, t.x + t.width * 0.5F - 1.0F, t.y + 4.0F, 2.0F, t.height - 4.0F, ink);
    rect(renderer, t.x + t.width * 0.5F - 5.0F, t.y, 10.0F, 5.0F, ink);
    rect(renderer, t.x + t.width * 0.5F - 4.0F, t.y + 1.0F, 2.0F, 2.0F, limb);
    rect(renderer, t.x + t.width * 0.5F + 2.0F, t.y + 1.0F, 2.0F, 2.0F, limb);
    rect(renderer, t.x + 7.0F, t.y + 9.0F, 4.0F, 4.0F, ink);
    rect(renderer, t.x + t.width - 11.0F, t.y + 9.0F, 4.0F, 4.0F, ink);
    rect(renderer, t.x + 9.0F, t.y + 15.0F, 3.0F, 3.0F, ink);
    rect(renderer, t.x + t.width - 12.0F, t.y + 15.0F, 3.0F, 3.0F, ink);
}

} // namespace

GardenDefenseGame::GardenDefenseGame(Engine& engine, std::uint32_t seed)
    : engine_(engine), match_(engine.events(), seed)
{
    engine.setClearColor(background);
    // Left and Right alone move the can; held together they would cancel
    // out, so pressed together they open the umbrella. A + D is the same
    // chord under its own name, because the engine reports each chord
    // separately.
    engine.registerChord("umbrella", {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT});
    engine.registerChord("umbrella-ad", {SDL_SCANCODE_A, SDL_SCANCODE_D});
    subscriptions_.push_back(engine.events().subscribe(Events::Input, [this](const Event& event) { onInput(event); }));
}

GardenDefenseGame::~GardenDefenseGame()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        engine_.events().unsubscribe(id);
    }
}

bool GardenDefenseGame::held(int scancode) const
{
    return heldKeys_.count(scancode) != 0;
}

double GardenDefenseGame::pressedAt(int scancode) const
{
    const auto it = pressedAt_.find(scancode);
    return it == pressedAt_.end() ? 0.0 : it->second;
}

void GardenDefenseGame::onInput(const Event& event)
{
    if (event.origin != 0) {
        return;
    }
    const std::string kind = event.getOr<std::string>("kind", "");
    const bool pressed = event.getOr<std::string>("state", "") == "pressed";
    if (kind == "chord") {
        // Key events come before chord events in a frame, so both press
        // times are already recorded. Turning around (Right pressed while
        // Left is still held) is not a chord.
        const std::string name = event.getOr<std::string>("name", "");
        const bool together =
            name == "umbrella" ? Arcade::pressedTogether({pressedAt(SDL_SCANCODE_LEFT), pressedAt(SDL_SCANCODE_RIGHT)})
            : name == "umbrella-ad" && Arcade::pressedTogether({pressedAt(SDL_SCANCODE_A), pressedAt(SDL_SCANCODE_D)});
        if (pressed && !paused_ && together) {
            match_.openUmbrella();
        }
        return;
    }
    if (kind != "key") {
        return;
    }
    const auto key = static_cast<int>(event.getOr<std::int64_t>("key", -1));
    if (!pressed) {
        heldKeys_.erase(key);
        return;
    }
    heldKeys_.insert(key);
    pressedAt_[key] = static_cast<double>(engine_.realTime().now()) / static_cast<double>(kNsPerSec);

    switch (key) {
    case SDL_SCANCODE_ESCAPE:
        engine_.quit();
        break;
    case SDL_SCANCODE_P:
        engine_.gameTime().togglePause();
        paused_ = engine_.gameTime().isPaused();
        break;
    case SDL_SCANCODE_F3:
        showPool_ = !showPool_;
        break;
    case SDL_SCANCODE_SPACE:
        if (!paused_) {
            match_.fire();
        }
        break;
    case SDL_SCANCODE_R:
        if (!paused_ && (match_.state() == Match::State::Won || match_.state() == Match::State::Lost)) {
            match_.restart();
        }
        break;
    default:
        break;
    }
}

void GardenDefenseGame::handleInput(Engine& engine)
{
    // Input arrives as InputEvents (onInput); this only paces the loop.
    pacer_.wait(engine.realTime());
}

void GardenDefenseGame::update(float deltaTime, Engine& engine)
{
    update(FrameTime{static_cast<double>(deltaTime), engine.gameTime().now()}, engine);
}

void GardenDefenseGame::update(const FrameTime& time, Engine& engine)
{
    paused_ = engine.gameTime().isPaused();
    realSeconds_ = static_cast<double>(engine.realTime().now()) / static_cast<double>(kNsPerSec);
    float axis = 0.0F;
    if (held(SDL_SCANCODE_LEFT) || held(SDL_SCANCODE_A)) {
        axis -= 1.0F;
    }
    if (held(SDL_SCANCODE_RIGHT) || held(SDL_SCANCODE_D)) {
        axis += 1.0F;
    }
    match_.setMove(axis);
    match_.update(time);
}

void GardenDefenseGame::render(SDL_Renderer* renderer) const
{
    drawGarden(renderer);
    drawPests(renderer);
    // Drops, seeds, splats and petals: plain coloured boxes.
    renderWorld(renderer, match_.world());
    drawCan(renderer);
    drawHud(renderer);
    if (showPool_) {
        Arcade::poolCounter(renderer, 16.0F, 42.0F, match_.world(), gameplayReserve, match_.cosmeticsSkipped());
    }
}

void GardenDefenseGame::drawGarden(SDL_Renderer* renderer) const
{
    // The fence the pests must not reach.
    for (float x = 6.0F; x < static_cast<float>(width); x += 32.0F) {
        rect(renderer, x, gardenLineY - 10.0F, 6.0F, 22.0F, wood);
        rect(renderer, x + 1.0F, gardenLineY - 13.0F, 4.0F, 3.0F, wood);
    }
    rect(renderer, 0.0F, gardenLineY - 4.0F, static_cast<float>(width), 3.0F, wood);
    rect(renderer, 0.0F, gardenLineY + 5.0F, static_cast<float>(width), 3.0F, wood);

    // The vegetable patch the can defends.
    rect(renderer, 0.0F, 566.0F, static_cast<float>(width), static_cast<float>(height) - 566.0F, soil);
    for (int i = 0; i < 16; ++i) {
        const float x = 18.0F + 50.0F * static_cast<float>(i);
        if (i % 2 == 0) {
            rect(renderer, x, 570.0F, 10.0F, 14.0F, carrot);
            rect(renderer, x + 1.0F, 562.0F, 3.0F, 8.0F, leaf);
            rect(renderer, x + 6.0F, 560.0F, 3.0F, 10.0F, leaf);
        } else {
            rect(renderer, x - 4.0F, 566.0F, 20.0F, 12.0F, leaf);
            rect(renderer, x, 563.0F, 12.0F, 4.0F, {120, 200, 110});
        }
    }
}

void GardenDefenseGame::drawPests(SDL_Renderer* renderer) const
{
    for (const auto& object : match_.world().objects()) {
        if (!object->isActive()) {
            continue;
        }
        const Transform& transform = *object->get<Transform>();
        if (const Pest* pest = object->get<Pest>()) {
            drawPest(renderer, transform, pest->row, match_.marchFrame());
        } else if (object->tag() == "ladybug") {
            drawLadybug(renderer, transform);
        }
    }
}

void GardenDefenseGame::drawCan(SDL_Renderer* renderer) const
{
    if (!match_.canUp()) {
        return;
    }
    // Blinks while the respawn shield lasts.
    if (match_.shieldSecondsLeft() > 0.0F && std::fmod(realSeconds_, 0.2) < 0.1) {
        return;
    }
    const Transform& t = *match_.can().get<Transform>();
    if (match_.umbrellaOpen()) {
        // A striped canopy over the can.
        const float centerX = t.x + t.width * 0.5F;
        for (int band = 0; band < 6; ++band) {
            const float half = 14.0F + 5.0F * static_cast<float>(band);
            for (int stripe = 0; stripe < 6; ++stripe) {
                const float stripeWidth = 2.0F * half / 6.0F;
                rect(renderer, centerX - half + stripeWidth * static_cast<float>(stripe),
                     t.y - 34.0F + 3.0F * static_cast<float>(band), stripeWidth, 3.0F,
                     stripe % 2 == 0 ? Color{235, 80, 90} : cream);
            }
        }
        rect(renderer, centerX - 1.0F, t.y - 16.0F, 2.0F, 16.0F, ink);
    }
    rect(renderer, t.x + 6.0F, t.y + 8.0F, t.width - 12.0F, t.height - 8.0F, tin);
    rect(renderer, t.x + 6.0F, t.y + 8.0F, t.width - 12.0F, 3.0F, tinDark);
    // The spout points up, where the drops come out.
    rect(renderer, t.x + t.width * 0.5F - 2.0F, t.y, 4.0F, 9.0F, tinDark);
    rect(renderer, t.x + t.width * 0.5F - 6.0F, t.y - 2.0F, 12.0F, 3.0F, tin);
    // The handle.
    rect(renderer, t.x + t.width - 8.0F, t.y + 4.0F, 6.0F, 3.0F, tinDark);
    rect(renderer, t.x + t.width - 4.0F, t.y + 4.0F, 3.0F, 14.0F, tinDark);
}

void GardenDefenseGame::drawHud(SDL_Renderer* renderer) const
{
    rect(renderer, 0.0F, 0.0F, static_cast<float>(width), 34.0F, {18, 28, 24});
    text(renderer, 16.0F, 13.0F, "GARDEN DEFENSE", cream);
    char status[64];
    std::snprintf(status, sizeof status, "SCORE %05d   WAVE %d/2", match_.score(), match_.wave());
    centeredText(renderer, 13.0F, status, cream);
    text(renderer, static_cast<float>(width) - 88.0F, 13.0F, "LIVES", cream);
    for (int i = 0; i < match_.lives(); ++i) {
        rect(renderer, static_cast<float>(width) - 40.0F + 10.0F * static_cast<float>(i), 12.0F, 7.0F, 9.0F, tin);
    }

    // The umbrella's state, under the HUD bar on the right.
    std::string umbrella;
    Color umbrellaColor = dim;
    if (match_.umbrellaOpen()) {
        umbrella = "UMBRELLA OPEN";
        umbrellaColor = ready;
    } else if (match_.umbrellaCooldownLeft() > 0.0F) {
        umbrella = format("UMBRELLA %.1fS", match_.umbrellaCooldownLeft());
    } else {
        umbrella = "UMBRELLA READY: LEFT+RIGHT";
        umbrellaColor = ready;
    }
    text(renderer, static_cast<float>(width) - 8.0F * static_cast<float>(umbrella.size()) - 16.0F, 42.0F, umbrella,
         umbrellaColor);

    text(renderer, 16.0F, static_cast<float>(height) - 12.0F,
         "ARROWS/AD MOVE  SPACE SPRAY  LEFT+RIGHT UMBRELLA  P PAUSE  F3 POOL", cream);

    switch (match_.state()) {
    case Match::State::WaveCleared:
        notice(renderer, 250.0F, "WAVE CLEARED! WAVE 2 IS COMING", {40, 90, 50}, cream);
        break;
    case Match::State::Won:
        notice(renderer, 240.0F, "THE GARDEN IS SAFE!", {40, 90, 50}, cream);
        notice(renderer, 272.0F, "PRESS R TO PLAY AGAIN", {30, 50, 40}, cream);
        break;
    case Match::State::Lost:
        notice(renderer, 240.0F, "THE PESTS GOT THE GARDEN", {110, 40, 35}, cream);
        notice(renderer, 272.0F, "PRESS R TO PLAY AGAIN", {30, 50, 40}, cream);
        break;
    case Match::State::Playing:
        break;
    }
    if (paused_) {
        shade(renderer, 0.0F, 34.0F, static_cast<float>(width), static_cast<float>(height) - 34.0F, ink, 140);
        notice(renderer, 290.0F, "PAUSED - PRESS P TO RESUME", {30, 50, 40}, cream);
    }
}
