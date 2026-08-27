// A test game, not a submission: three entity kinds on the shared engine.
//
//   1. the player  -- controllable, moves left/right along the floor, no gravity
//   2. obstacles   -- fall from the sky under gravity, must be avoided
//   3. coins       -- fall from the sky under gravity, collect them for score
//
// Gravity applies to 2 and 3 but never to 1, which is the point: the engine's
// physics system is opt-in per entity, not something the loop imposes.
// Self-contained: class, game logic and main() in one file.
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <exception>
#include <iostream>
#include <vector>

namespace {

class CatchGame : public Game {
public:
    explicit CatchGame(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 520.0F;
    static constexpr float playerSpeed = 640.0F;

    static constexpr float playerSize = 64.0F;
    static constexpr float obstacleSize = 54.0F;
    static constexpr float coinSize = 38.0F;

    static constexpr float spawnInterval = 0.65F;
    static constexpr float floorHeight = 70.0F;

    void spawnFaller();
    void handleCatches();
    void restart();
    float nextRandom();

    Entity player_;
    Entity floor_;
    std::vector<Entity> obstacles_;
    std::vector<Entity> coins_;

    float worldWidth_;
    float worldHeight_;

    float spawnTimer_ = 0.0F;
    int score_ = 0;
    int lives_ = 3;
    bool isGameOver_ = false;
    unsigned int randomState_ = 987654321U;
};

CatchGame::CatchGame(const Engine& engine)
    : player_(0.0F, 0.0F, playerSize, playerSize),
      floor_(0.0F, 0.0F, 0.0F, 0.0F),
      worldWidth_(static_cast<float>(engine.getWidth())),
      worldHeight_(static_cast<float>(engine.getHeight()))
{
    floor_ = Entity(0.0F, worldHeight_ - floorHeight, worldWidth_, floorHeight);

    // Entity 1: the player sits on the floor and never falls.
    player_.setPosition((worldWidth_ - playerSize) * 0.5F, floor_.getY() - playerSize);

    // This game's own gravity, gentle enough that things are dodgeable.
    Physics::setGravity(gravity);

    std::cout << "Catch: A/D moves, dodge the red blocks, collect the gold ones.\n"
              << "F1 toggles scaling, R restarts, Esc quits.\n";
}

float CatchGame::nextRandom()
{
    randomState_ = randomState_ * 1103515245U + 12345U;
    return static_cast<float>((randomState_ >> 16) & 0x7FFFU) / 32767.0F;
}

// Entities 2 and 3 enter here, at a random column just above the top edge.
void CatchGame::spawnFaller()
{
    const bool isCoin = nextRandom() < 0.5F;
    const float size = isCoin ? coinSize : obstacleSize;
    const float x = nextRandom() * (worldWidth_ - size);

    Entity faller(x, -size, size, size);

    // A small head start, so they do not all drift at an identical rate.
    faller.setVelocityY(40.0F + nextRandom() * 80.0F);

    if (isCoin) {
        coins_.push_back(faller);
    } else {
        obstacles_.push_back(faller);
    }
}

void CatchGame::handleInput(Engine& engine)
{
    float velocityX = 0.0F;

    if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
        velocityX -= playerSpeed;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
        velocityX += playerSpeed;
    }

    player_.setVelocityX(isGameOver_ ? 0.0F : velocityX);

    if (Input::isKeyJustPressed(SDL_SCANCODE_R)) {
        restart();
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
    }
}

void CatchGame::update(float deltaTime, Engine& engine)
{
    (void)engine;

    if (isGameOver_) {
        return;
    }

    // Entity 1: moves horizontally only. Physics::applyGravity is deliberately
    // never called on the player.
    player_.update(deltaTime);

    const Rect playerBounds = player_.getBounds();
    if (playerBounds.x < 0.0F) {
        player_.setPosition(0.0F, player_.getY());
    } else if (playerBounds.x + playerBounds.width > worldWidth_) {
        player_.setPosition(worldWidth_ - playerBounds.width, player_.getY());
    }

    spawnTimer_ += deltaTime;
    if (spawnTimer_ >= spawnInterval) {
        spawnTimer_ -= spawnInterval;
        spawnFaller();
    }

    // Entities 2 and 3: gravity, then integrate.
    for (Entity& obstacle : obstacles_) {
        Physics::applyGravity(obstacle, deltaTime);
        obstacle.update(deltaTime);
    }
    for (Entity& coin : coins_) {
        Physics::applyGravity(coin, deltaTime);
        coin.update(deltaTime);
    }

    handleCatches();
}

// What the game decides to do once the engine reports an overlap.
void CatchGame::handleCatches()
{
    // Obstacles: touching one costs a life.
    for (std::size_t i = 0; i < obstacles_.size();) {
        const bool hitPlayer = player_.collidesWith(obstacles_[i]);
        const bool landed = Collision::intersects(obstacles_[i], floor_);

        if (hitPlayer) {
            lives_ -= 1;
            std::cout << "Hit an obstacle! Lives left: " << lives_ << '\n';

            if (lives_ <= 0) {
                isGameOver_ = true;
                std::cout << "Game over. Final score: " << score_ << " (R to restart)\n";
            }
        }

        if (hitPlayer || landed) {
            obstacles_.erase(obstacles_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }

    // Coins: catching one scores, letting one land loses it.
    for (std::size_t i = 0; i < coins_.size();) {
        const bool caught = player_.collidesWith(coins_[i]);
        const bool landed = Collision::intersects(coins_[i], floor_);

        if (caught) {
            score_ += 1;
            std::cout << "Coin caught! Score: " << score_ << '\n';
        } else if (landed) {
            std::cout << "Missed a coin.\n";
        }

        if (caught || landed) {
            coins_.erase(coins_.begin() + static_cast<long>(i));
        } else {
            ++i;
        }
    }
}

void CatchGame::restart()
{
    obstacles_.clear();
    coins_.clear();

    player_.setPosition((worldWidth_ - playerSize) * 0.5F, floor_.getY() - playerSize);
    player_.setVelocity(0.0F, 0.0F);

    spawnTimer_ = 0.0F;
    score_ = 0;
    lives_ = 3;
    isGameOver_ = false;

    std::cout << "Restarted.\n";
}

void CatchGame::render(SDL_Renderer* renderer) const
{
    const auto drawEntity = [renderer](const Entity& entity, Uint8 red, Uint8 green, Uint8 blue) {
        const Rect bounds = entity.getBounds();
        const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};

        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        SDL_RenderFillRect(renderer, &rect);
    };

    drawEntity(floor_, 60, 70, 95);

    for (const Entity& obstacle : obstacles_) {
        drawEntity(obstacle, 225, 80, 75);
    }
    for (const Entity& coin : coins_) {
        drawEntity(coin, 245, 205, 85);
    }

    drawEntity(player_, 235, 240, 255);

    char hud[128];
    std::snprintf(hud, sizeof(hud), "Score: %d   Lives: %d", score_, lives_);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 20.0F, 20.0F, hud);

    if (isGameOver_) {
        SDL_RenderDebugText(renderer, 20.0F, 44.0F, "Game over -- R to restart, Esc to quit");
    }
}

} // namespace

int main()
{
    try {
        Engine engine("Catch", 1280, 720);
        engine.setClearColor(25, 35, 60);

        CatchGame game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
