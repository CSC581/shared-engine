#include "PirateCannonsGame.hpp"

#include "ArcadeDraw.hpp"
#include "Components.hpp"
#include "Engine.hpp"
#include "PirateCannonsRules.hpp"
#include "TimeUnits.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using namespace PirateCannons;

constexpr Color sky{70, 110, 160};
constexpr Color cream{245, 236, 210};
constexpr Color dim{170, 180, 190};
constexpr Color ink{20, 20, 26};
constexpr Color hull{110, 70, 40};
constexpr Color hullDark{70, 45, 28};
constexpr Color sail{235, 228, 205};
constexpr Color rock{120, 112, 100};
constexpr Color rockLight{150, 142, 128};
constexpr Color sand{220, 200, 140};
constexpr Color palm{60, 150, 70};
constexpr Color water{30, 70, 120};
constexpr Color crest{150, 200, 235};
constexpr Color ready{130, 220, 120};
constexpr Color alert{240, 110, 90};
constexpr float degrees = 3.14159265F / 180.0F;

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

const char* outcomeText(Match::Outcome outcome)
{
    switch (outcome) {
    case Match::Outcome::Hit:
        return "HIT!";
    case Match::Outcome::Splash:
        return "SPLASH";
    case Match::Outcome::Rocks:
        return "THE ROCKS TOOK IT";
    case Match::Outcome::Gull:
        return "SQUAWK! A GULL GOT IN THE WAY";
    case Match::Outcome::Miss:
        return "MISS";
    case Match::Outcome::None:
        break;
    }
    return "";
}

} // namespace

PirateCannonsGame::PirateCannonsGame(Engine& engine, std::uint32_t seed)
    : engine_(engine), match_(engine.events(), seed)
{
    engine.setClearColor(sky);
    // Left and Right alone set the power and Space fires; all three pressed
    // together fire the chain shot instead.
    engine.registerChord("chain-shot", {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_SPACE});
    subscriptions_.push_back(engine.events().subscribe(Events::Input, [this](const Event& event) { onInput(event); }));
}

PirateCannonsGame::~PirateCannonsGame()
{
    for (const EventManager::SubscriptionId id : subscriptions_) {
        engine_.events().unsubscribe(id);
    }
}

bool PirateCannonsGame::held(int scancode) const
{
    return heldKeys_.count(scancode) != 0;
}

double PirateCannonsGame::pressedAt(int scancode) const
{
    const auto it = pressedAt_.find(scancode);
    return it == pressedAt_.end() ? 0.0 : it->second;
}

void PirateCannonsGame::onInput(const Event& event)
{
    if (event.origin != 0) {
        return;
    }
    const std::string kind = event.getOr<std::string>("kind", "");
    const bool pressed = event.getOr<std::string>("state", "") == "pressed";
    if (kind == "chord") {
        // Key events come before chord events in a frame, so all three press
        // times are recorded. A Space still held from the last shot is not
        // part of a chord.
        if (pressed && !paused_ && event.getOr<std::string>("name", "") == "chain-shot" &&
            Arcade::pressedTogether({pressedAt(SDL_SCANCODE_LEFT), pressedAt(SDL_SCANCODE_RIGHT),
                                     pressedAt(SDL_SCANCODE_SPACE)})) {
            match_.fire(true);
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
        spaceQueued_ = !paused_;
        break;
    case SDL_SCANCODE_R:
        if (!paused_ && match_.state() == Match::State::MatchOver) {
            match_.restart();
        }
        break;
    default:
        break;
    }
}

void PirateCannonsGame::handleInput(Engine& engine)
{
    // Input arrives as InputEvents (onInput); this only paces the loop.
    pacer_.wait(engine.realTime());
}

void PirateCannonsGame::update(float deltaTime, Engine& engine)
{
    update(FrameTime{static_cast<double>(deltaTime), engine.gameTime().now()}, engine);
}

void PirateCannonsGame::update(const FrameTime& time, Engine& engine)
{
    paused_ = engine.gameTime().isPaused();
    realSeconds_ = static_cast<double>(engine.realTime().now()) / static_cast<double>(kNsPerSec);
    const float angleAxis = (held(SDL_SCANCODE_UP) ? 1.0F : 0.0F) - (held(SDL_SCANCODE_DOWN) ? 1.0F : 0.0F);
    const float powerAxis = (held(SDL_SCANCODE_RIGHT) ? 1.0F : 0.0F) - (held(SDL_SCANCODE_LEFT) ? 1.0F : 0.0F);
    match_.adjustAim(angleAxis, powerAxis, static_cast<float>(time.dtSeconds));
    // A chain shot fired this frame has already taken the turn, so this
    // plain shot is then refused.
    if (spaceQueued_) {
        match_.fire(false);
        spaceQueued_ = false;
    }
    match_.update(time);
}

void PirateCannonsGame::render(SDL_Renderer* renderer) const
{
    drawScenery(renderer);
    drawShip(renderer, Side::Player);
    drawShip(renderer, Side::Cpu);
    // Drawn over the hulls, so a sinking ship goes under.
    drawSea(renderer);
    drawGulls(renderer);
    // Cannonballs, splinters, splashes and feathers: plain coloured boxes.
    renderWorld(renderer, match_.world());
    drawAim(renderer);
    drawHud(renderer);
    if (showPool_) {
        Arcade::poolCounter(renderer, 16.0F, 40.0F, match_.world(), gameplayReserve, match_.cosmeticsSkipped());
    }
}

void PirateCannonsGame::drawScenery(SDL_Renderer* renderer) const
{
    rect(renderer, 820.0F, 70.0F, 44.0F, 44.0F, {250, 220, 140});
    rect(renderer, 812.0F, 78.0F, 60.0F, 28.0F, {250, 220, 140});
    for (const float x : {120.0F, 380.0F, 640.0F}) {
        rect(renderer, x, 60.0F + std::fmod(x, 50.0F), 90.0F, 16.0F, {200, 215, 235});
        rect(renderer, x + 18.0F, 50.0F + std::fmod(x, 50.0F), 50.0F, 14.0F, {200, 215, 235});
    }

    // The island: rock in three steps, sand at the waterline, a palm on top.
    rect(renderer, islandX, islandY + 80.0F, islandWidth, islandHeight - 80.0F, rock);
    rect(renderer, islandX + 15.0F, islandY + 40.0F, islandWidth - 30.0F, 40.0F, rock);
    rect(renderer, islandX + 32.0F, islandY, islandWidth - 64.0F, 40.0F, rockLight);
    rect(renderer, islandX + 20.0F, islandY + 46.0F, 12.0F, 6.0F, rockLight);
    rect(renderer, islandX - 12.0F, seaY - 8.0F, islandWidth + 24.0F, 8.0F, sand);
    const float trunkX = islandX + islandWidth * 0.5F;
    for (int i = 0; i < 5; ++i) {
        rect(renderer, trunkX - 2.0F + static_cast<float>(i), islandY - 8.0F * static_cast<float>(i + 1), 5.0F, 8.0F,
             {130, 95, 60});
    }
    rect(renderer, trunkX - 22.0F, islandY - 46.0F, 22.0F, 6.0F, palm);
    rect(renderer, trunkX + 4.0F, islandY - 46.0F, 22.0F, 6.0F, palm);
    rect(renderer, trunkX - 14.0F, islandY - 54.0F, 30.0F, 6.0F, palm);
}

void PirateCannonsGame::drawShip(SDL_Renderer* renderer, Side side) const
{
    const Rect bounds = shipBounds(side);
    const bool sinking =
        (match_.state() == Match::State::RoundOver || match_.state() == Match::State::MatchOver) &&
        match_.hp(side) == 0;
    const float x = bounds.x;
    const float y = bounds.y + (sinking ? 60.0F * match_.sinkProgress() : 0.0F);
    const bool facingRight = side == Side::Player;

    // Hull, rail and the stern cabin.
    rect(renderer, x, y + 14.0F, shipWidth, 18.0F, hull);
    rect(renderer, x + 8.0F, y + 32.0F, shipWidth - 16.0F, 10.0F, hull);
    rect(renderer, x, y + 12.0F, shipWidth, 3.0F, hullDark);
    rect(renderer, facingRight ? x + 4.0F : x + shipWidth - 26.0F, y, 22.0F, 13.0F, hullDark);
    // Holes for each hit taken.
    for (int i = 0; i < shipHp - match_.hp(side); ++i) {
        rect(renderer, x + 20.0F + 18.0F * static_cast<float>(i), y + 20.0F, 7.0F, 6.0F, ink);
    }
    // Mast, sail and flag: blue for you, black for the computer.
    const float mastX = x + shipWidth * 0.5F - 2.0F;
    rect(renderer, mastX, y - 52.0F, 4.0F, 66.0F, hullDark);
    rect(renderer, mastX - 18.0F, y - 46.0F, 40.0F, 34.0F, sail);
    if (match_.hp(side) < shipHp) {
        rect(renderer, mastX - 8.0F, y - 34.0F, 6.0F, 6.0F, sky);
    }
    const Color flag = facingRight ? Color{60, 110, 220} : ink;
    rect(renderer, facingRight ? mastX + 4.0F : mastX - 16.0F, y - 52.0F, 16.0F, 10.0F, flag);
    if (!facingRight) {
        rect(renderer, mastX - 10.0F, y - 49.0F, 4.0F, 4.0F, cream);
    }

    // The cannon points along this side's aim.
    if (!sinking) {
        const Shot& shot = match_.shot(side);
        const float pivotX = muzzleX(side) + ballSize * 0.5F;
        const float pivotY = muzzleY() + ballSize * 0.5F + 6.0F;
        const float dx = (facingRight ? 1.0F : -1.0F) * std::cos(shot.angle * degrees);
        const float dy = -std::sin(shot.angle * degrees);
        for (int i = 0; i < 6; ++i) {
            const float step = 3.0F * static_cast<float>(i);
            rect(renderer, pivotX + dx * step - 3.0F, pivotY + dy * step - 3.0F, 6.0F, 6.0F, ink);
        }
    }

    // Health over the mast.
    for (int i = 0; i < shipHp; ++i) {
        rect(renderer, x + shipWidth * 0.5F - 17.0F + 12.0F * static_cast<float>(i), bounds.y - 68.0F, 10.0F, 6.0F,
             i < match_.hp(side) ? ready : Color{60, 60, 70});
    }
}

void PirateCannonsGame::drawSea(SDL_Renderer* renderer) const
{
    rect(renderer, 0.0F, seaY, static_cast<float>(width), height - seaY, water);
    const float drift = static_cast<float>(std::fmod(realSeconds_ * 30.0, 48.0));
    for (float x = -48.0F + drift; x < static_cast<float>(width); x += 48.0F) {
        rect(renderer, x, seaY, 20.0F, 3.0F, crest);
        rect(renderer, x + 24.0F, seaY + 26.0F, 14.0F, 2.0F, crest);
    }
}

void PirateCannonsGame::drawGulls(SDL_Renderer* renderer) const
{
    const bool wingsUp = std::fmod(realSeconds_, 0.4) < 0.2;
    for (const auto& object : match_.world().objects()) {
        if (!object->isActive() || object->tag() != "gull") {
            continue;
        }
        const Transform& t = *object->get<Transform>();
        const float wingY = wingsUp ? t.y : t.y + 5.0F;
        rect(renderer, t.x, wingY, 9.0F, 3.0F, cream);
        rect(renderer, t.x + 13.0F, wingY, 9.0F, 3.0F, cream);
        rect(renderer, t.x + 8.0F, t.y + 4.0F, 6.0F, 5.0F, cream);
        rect(renderer, t.x + 14.0F, t.y + 5.0F, 3.0F, 2.0F, {240, 170, 60});
    }
}

// Dots along the launch direction, longer with more power. The flight
// itself is left for the player to judge, wind and all.
void PirateCannonsGame::drawAim(SDL_Renderer* renderer) const
{
    if (match_.state() != Match::State::Aiming) {
        return;
    }
    const Shot& shot = match_.shot(Side::Player);
    const Velocity velocity = launchVelocity(shot, Side::Player);
    const float scale = 0.025F;
    for (int i = 1; i <= 8; ++i) {
        const float step = scale * static_cast<float>(i);
        rect(renderer, muzzleX(Side::Player) + ballSize * 0.5F + velocity.x * step - 2.0F,
             muzzleY() + ballSize * 0.5F + velocity.y * step - 2.0F, 4.0F, 4.0F, cream);
    }
}

void PirateCannonsGame::drawHud(SDL_Renderer* renderer) const
{
    rect(renderer, 0.0F, 0.0F, static_cast<float>(width), 30.0F, {30, 45, 70});
    text(renderer, 16.0F, 11.0F, "PIRATE CANNONS", cream);
    char line[96];
    std::snprintf(line, sizeof line, "ROUND %d   YOU %d - %d CPU", match_.round(), match_.roundsWon(Side::Player),
                  match_.roundsWon(Side::Cpu));
    centeredText(renderer, 11.0F, line, cream);

    // Wind: arrows for direction, one per 15 units of strength.
    const float wind = match_.wind();
    const int arrows = std::max(1, static_cast<int>(std::abs(wind) / 15.0F + 0.5F));
    const std::string windArrows(static_cast<std::size_t>(arrows), wind < 0.0F ? '<' : '>');
    std::snprintf(line, sizeof line, "WIND %s %.0f", windArrows.c_str(), std::abs(wind));
    text(renderer, static_cast<float>(width) - 8.0F * static_cast<float>(std::string(line).size()) - 16.0F, 11.0F,
         line, std::abs(wind) > 40.0F ? alert : cream);

    // Your cannon's settings and the chain shot, in the sea below your ship.
    const Shot& shot = match_.shot(Side::Player);
    std::snprintf(line, sizeof line, "ANGLE %2.0f  POWER %3.0f", shot.angle, shot.power);
    text(renderer, 20.0F, seaY + 46.0F, line, cream);
    const float power = (shot.power - minPower) / (maxPower - minPower);
    rect(renderer, 20.0F, seaY + 60.0F, 160.0F, 6.0F, {20, 40, 70});
    rect(renderer, 20.0F, seaY + 60.0F, 160.0F * power, 6.0F, {250, 200, 90});
    std::snprintf(line, sizeof line, "CHAIN SHOTS %d: LEFT+RIGHT+SPACE", match_.chainShotsLeft());
    text(renderer, 20.0F, seaY + 76.0F, line, match_.chainShotsLeft() > 0 ? ready : dim);
    text(renderer, 20.0F, static_cast<float>(height) - 14.0F,
         "UP/DOWN ANGLE  LEFT/RIGHT POWER  SPACE FIRE  P PAUSE  F3 POOL", dim);

    switch (match_.state()) {
    case Match::State::Aiming:
        notice(renderer, 44.0F, "YOUR TURN", {40, 90, 60}, cream);
        break;
    case Match::State::CpuAiming:
        notice(renderer, 44.0F, "THE CPU IS AIMING...", {90, 40, 40}, cream);
        break;
    case Match::State::RoundOver:
        if (match_.hp(match_.sinking()) == 0) {
            notice(renderer, 200.0F,
                   match_.sinking() == Side::Cpu ? "YOU SANK THE CPU'S SHIP!" : "YOUR SHIP WAS SUNK",
                   {30, 45, 70}, cream);
            notice(renderer, 232.0F, "NEXT ROUND COMING UP", {30, 45, 70}, cream);
        }
        break;
    case Match::State::MatchOver:
        notice(renderer, 200.0F,
               match_.roundsWon(Side::Player) > match_.roundsWon(Side::Cpu) ? "YOU WIN THE MATCH!"
                                                                            : "THE CPU WINS THE MATCH",
               {30, 45, 70}, cream);
        notice(renderer, 232.0F, "PRESS R TO PLAY AGAIN", {30, 45, 70}, cream);
        break;
    case Match::State::Flying:
        break;
    }
    if (match_.outcomeSecondsLeft() > 0.0F && match_.state() != Match::State::MatchOver) {
        centeredText(renderer, 80.0F, outcomeText(match_.outcome()),
                     match_.outcome() == Match::Outcome::Hit ? alert : cream);
    }
    if (paused_) {
        shade(renderer, 0.0F, 30.0F, static_cast<float>(width), static_cast<float>(height) - 30.0F, ink, 140);
        notice(renderer, 260.0F, "PAUSED - PRESS P TO RESUME", {30, 45, 70}, cream);
    }
}
