// A test game, not a submission: a Fruit-Ninja-style slicer used to check that
// every engine system works together -- Entity for the blade, fruit and bombs;
// Physics for the toss arcs; Input for steering the blade; Collision for the
// slice hits. Self-contained in one file.
//
// The engine has no mouse input, so the "blade" is steered with the keyboard:
// arrows/WASD move it, and it only cuts while it is actually swinging fast.
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <iostream>
#include <vector>

namespace {

float randomRange(float low, float high)
{
    const float unit = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    return low + unit * (high - low);
}

class SliceGame : public Game {
public:
    explicit SliceGame(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 700.0F;

    static constexpr float bladeSize = 26.0F;
    static constexpr float bladeSpeed = 900.0F;
    // Below this the blade is just being repositioned, so it does not cut.
    static constexpr float sliceSpeed = 260.0F;

    static constexpr float fruitSize = 52.0F;
    static constexpr float bombSize = 46.0F;
    static constexpr float tossInterval = 0.85F;
    static constexpr float bombChance = 0.22F;

    static constexpr int startingLives = 3;
    static constexpr std::size_t trailLength = 14;

    struct Toss {
        Entity body{0.0F, 0.0F, 0.0F, 0.0F};
        bool isBomb = false;
        bool isSliced = false;
        float spin = 0.0F;
    };

    void spawnToss();
    void sliceAt(std::size_t index);
    void reset();

    Entity blade_;
    std::vector<Toss> tosses_;
    std::deque<SDL_FPoint> trail_;

    float worldWidth_;
    float worldHeight_;

    float spawnTimer_ = 0.0F;
    float bladeSpeedNow_ = 0.0F;

    int score_ = 0;
    int lives_ = startingLives;
    int bestCombo_ = 0;
    bool isGameOver_ = false;
};

SliceGame::SliceGame(const Engine& engine)
    : blade_(0.0F, 0.0F, bladeSize, bladeSize),
      worldWidth_(static_cast<float>(engine.getWidth())),
      worldHeight_(static_cast<float>(engine.getHeight()))
{
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    // This game's own gravity, chosen so a toss hangs near the top of its arc.
    Physics::setGravity(gravity);

    reset();
}

void SliceGame::reset()
{
    blade_.setPosition(worldWidth_ * 0.5F, worldHeight_ * 0.6F);
    blade_.setVelocity(0.0F, 0.0F);

    tosses_.clear();
    trail_.clear();

    spawnTimer_ = 0.0F;
    bladeSpeedNow_ = 0.0F;
    score_ = 0;
    lives_ = startingLives;
    bestCombo_ = 0;
    isGameOver_ = false;
}

void SliceGame::spawnToss()
{
    Toss toss;
    toss.isBomb = randomRange(0.0F, 1.0F) < bombChance;

    const float size = toss.isBomb ? bombSize : fruitSize;
    const float startX = randomRange(worldWidth_ * 0.15F, worldWidth_ * 0.85F - size);

    toss.body = Entity(startX, worldHeight_, size, size);

    // Enough upward speed to reach roughly the upper third of the screen.
    const float rise = randomRange(worldHeight_ * 0.55F, worldHeight_ * 0.85F);
    const float upward = -std::sqrt(2.0F * gravity * rise);
    const float drift = randomRange(-180.0F, 180.0F);

    toss.body.setVelocity(drift, upward);
    toss.spin = randomRange(-260.0F, 260.0F);

    tosses_.push_back(toss);
}

void SliceGame::handleInput(Engine& engine)
{
    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
        return;
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        reset();
        return;
    }

    float velocityX = 0.0F;
    float velocityY = 0.0F;

    if (Input::isKeyPressed(SDL_SCANCODE_LEFT) || Input::isKeyPressed(SDL_SCANCODE_A)) {
        velocityX -= bladeSpeed;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_RIGHT) || Input::isKeyPressed(SDL_SCANCODE_D)) {
        velocityX += bladeSpeed;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_UP) || Input::isKeyPressed(SDL_SCANCODE_W)) {
        velocityY -= bladeSpeed;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_DOWN) || Input::isKeyPressed(SDL_SCANCODE_S)) {
        velocityY += bladeSpeed;
    }

    blade_.setVelocity(velocityX, velocityY);
}

void SliceGame::sliceAt(std::size_t index)
{
    Toss& toss = tosses_[index];
    toss.isSliced = true;

    if (toss.isBomb) {
        lives_ = 0;
        isGameOver_ = true;
        return;
    }

    // The slice halves keep flying, so the cut stays visible for a moment.
    toss.body.setVelocityX(toss.body.getVelocityX() * 0.4F);
    ++score_;
}

void SliceGame::update(float deltaTime, Engine& engine)
{
    (void)engine;

    if (isGameOver_) {
        return;
    }

    blade_.update(deltaTime);

    // Keep the blade on screen; the walls are the world edges.
    const float maxX = worldWidth_ - bladeSize;
    const float maxY = worldHeight_ - bladeSize;
    const float clampedX = blade_.getX() < 0.0F ? 0.0F : (blade_.getX() > maxX ? maxX : blade_.getX());
    const float clampedY = blade_.getY() < 0.0F ? 0.0F : (blade_.getY() > maxY ? maxY : blade_.getY());
    blade_.setPosition(clampedX, clampedY);

    bladeSpeedNow_ = std::sqrt(blade_.getVelocityX() * blade_.getVelocityX() +
                               blade_.getVelocityY() * blade_.getVelocityY());

    trail_.push_back(SDL_FPoint{clampedX + bladeSize * 0.5F, clampedY + bladeSize * 0.5F});
    while (trail_.size() > trailLength) {
        trail_.pop_front();
    }

    spawnTimer_ -= deltaTime;
    if (spawnTimer_ <= 0.0F) {
        spawnToss();
        spawnTimer_ = tossInterval * randomRange(0.7F, 1.3F);
    }

    const bool isSwinging = bladeSpeedNow_ >= sliceSpeed;
    int comboThisFrame = 0;

    for (std::size_t i = 0; i < tosses_.size(); ++i) {
        Physics::applyGravity(tosses_[i].body, deltaTime);
        tosses_[i].body.update(deltaTime);

        if (tosses_[i].isSliced || !isSwinging) {
            continue;
        }

        if (Collision::intersects(blade_, tosses_[i].body)) {
            const bool wasBomb = tosses_[i].isBomb;
            sliceAt(i);
            if (wasBomb) {
                return;
            }
            ++comboThisFrame;
        }
    }

    if (comboThisFrame > bestCombo_) {
        bestCombo_ = comboThisFrame;
    }

    // Drop anything that has fallen back off the bottom. Missing whole fruit
    // costs a life; sliced halves and bombs are free to leave.
    std::vector<Toss> remaining;
    remaining.reserve(tosses_.size());
    for (const Toss& toss : tosses_) {
        if (toss.body.getY() <= worldHeight_) {
            remaining.push_back(toss);
            continue;
        }

        if (!toss.isSliced && !toss.isBomb) {
            --lives_;
        }
    }
    tosses_ = remaining;

    if (lives_ <= 0) {
        lives_ = 0;
        isGameOver_ = true;
    }
}

void SliceGame::render(SDL_Renderer* renderer) const
{
    const auto drawRect = [renderer](const Rect& bounds, Uint8 red, Uint8 green, Uint8 blue) {
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};
        SDL_RenderFillRect(renderer, &rect);
    };

    for (const Toss& toss : tosses_) {
        const Rect bounds = toss.body.getBounds();

        if (toss.isBomb) {
            drawRect(bounds, 30, 30, 36);
            const Rect fuse{bounds.x + bounds.width * 0.4F, bounds.y - 8.0F, bounds.width * 0.2F, 8.0F};
            drawRect(fuse, 230, 120, 40);
            continue;
        }

        if (!toss.isSliced) {
            drawRect(bounds, 235, 90, 80);
            const Rect leaf{bounds.x + bounds.width * 0.35F, bounds.y - 6.0F, bounds.width * 0.3F, 6.0F};
            drawRect(leaf, 90, 190, 100);
            continue;
        }

        // Two halves with a gap where the blade went through.
        const float half = bounds.width * 0.42F;
        drawRect(Rect{bounds.x, bounds.y, half, bounds.height}, 250, 200, 120);
        drawRect(Rect{bounds.x + bounds.width - half, bounds.y, half, bounds.height}, 250, 200, 120);
    }

    // Blade trail: older points are smaller, so the swing reads as a stroke.
    const std::size_t points = trail_.size();
    for (std::size_t i = 0; i < points; ++i) {
        const float age = static_cast<float>(i + 1) / static_cast<float>(points);
        const float size = 4.0F + age * 10.0F;
        const SDL_FPoint& point = trail_[i];
        drawRect(Rect{point.x - size * 0.5F, point.y - size * 0.5F, size, size},
                 static_cast<Uint8>(150 + age * 105.0F),
                 static_cast<Uint8>(200 + age * 55.0F),
                 255);
    }

    const bool isSwinging = bladeSpeedNow_ >= sliceSpeed;
    const Rect bladeBounds = blade_.getBounds();
    if (isSwinging) {
        drawRect(bladeBounds, 255, 255, 255);
    } else {
        drawRect(bladeBounds, 130, 150, 175);
    }

    char hud[160];
    std::snprintf(hud, sizeof(hud), "Sliced: %d   Lives: %d   Best combo: %d", score_, lives_, bestCombo_);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);

    if (isGameOver_) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Game over -- R to play again");
    } else if (!isSwinging) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Move fast to cut: WASD / arrows, ESC to quit");
    }
}

} // namespace

int main()
{
    try {
        Engine engine("Slice", 1600, 900);
        engine.setClearColor(25, 35, 60);

        SliceGame game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
