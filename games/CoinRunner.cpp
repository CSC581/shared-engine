// Coin Runner: run and jump across platforms, collect coins, avoid the patrol.
//
// This is the individual game -- everything here is this game's own choice and
// the engine has no idea any of it exists. Self-contained: class, game logic
// and main() in one file.
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

// An individual game built on the shared engine.
//
// Coin Runner: run and jump across platforms, collect coins, avoid the patrol.
// Every value below is this game's own choice -- the engine has no idea any of
// it exists.
class CoinRunner : public Game {
public:
    explicit CoinRunner(const Engine& engine);

    void handleInput(Engine& engine) override;
    void update(float deltaTime, Engine& engine) override;
    void render(SDL_Renderer* renderer) const override;

private:
    static constexpr float gravity = 1470.0F;
    static constexpr float playerSpeed = 630.0F;
    static constexpr float jumpSpeed = 1050.0F;
    static constexpr float patrolSpeed = 390.0F;

    static constexpr float playerStartX = 120.0F;
    static constexpr float playerStartY = 600.0F;

    void handleCollisions(Engine& engine);
    void resetPlayer();
    void moveCoinToNextSpot();

    Entity player_;
    Entity patrol_;
    Entity coin_;
    std::vector<Entity> platforms_;

    float worldWidth_;

    bool isOnGround_ = false;
    int score_ = 0;
    int lives_ = 3;
};

// Where coins appear, in order. The game cycles through them.
constexpr float coinSpots[][2] = {
    { 630.0F, 630.0F },
    { 1350.0F, 450.0F },
    { 930.0F, 270.0F },
    { 1725.0F, 840.0F },
};
constexpr int coinSpotCount = static_cast<int>(sizeof(coinSpots) / sizeof(coinSpots[0]));


CoinRunner::CoinRunner(const Engine& engine)
    : player_(playerStartX, playerStartY, 75.0F, 75.0F),
      patrol_(600.0F, 855.0F, 90.0F, 90.0F),
      coin_(coinSpots[0][0], coinSpots[0][1], 45.0F, 45.0F),
      worldWidth_(static_cast<float>(engine.getWidth()))
{
    const float groundY = static_cast<float>(engine.getHeight()) - 135.0F;

    // Ground and platforms: static level geometry the player collides against.
    platforms_.emplace_back(0.0F, groundY, worldWidth_, 135.0F);  // ground
    platforms_.emplace_back(525.0F, 735.0F, 390.0F, 45.0F);       // low platform
    platforms_.emplace_back(1245.0F, 555.0F, 390.0F, 45.0F);      // mid platform
    platforms_.emplace_back(825.0F, 375.0F, 330.0F, 45.0F);       // high platform

    patrol_.setVelocityX(patrolSpeed);

    // This game's choice of gravity, not a value baked into the engine.
    Physics::setGravity(gravity);

    std::cout << "Coin Runner: A/D to move, Space to jump, F1 to toggle scaling, Esc to quit.\n";
}

void CoinRunner::handleInput(Engine& engine)
{
    float velocityX = 0.0F;

    if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
        velocityX -= playerSpeed;
    }
    if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
        velocityX += playerSpeed;
    }

    player_.setVelocityX(velocityX);

    // Jump only when standing on something, so the player cannot fly.
    if (isOnGround_ && Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
        player_.setVelocityY(-jumpSpeed);
        isOnGround_ = false;
    }

    if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
        engine.quit();
    }
}

void CoinRunner::update(float deltaTime, Engine& engine)
{
    // The player falls; the patrol floats along its path untouched by gravity.
    Physics::applyGravity(player_, deltaTime);

    player_.update(deltaTime);
    patrol_.update(deltaTime);

    // The patrol walks back and forth along the ground.
    const Rect patrolBounds = patrol_.getBounds();
    if (patrolBounds.x <= 0.0F) {
        patrol_.setVelocityX(patrolSpeed);
    } else if (patrolBounds.x + patrolBounds.width >= worldWidth_) {
        patrol_.setVelocityX(-patrolSpeed);
    }

    handleCollisions(engine);
}

// Everything the game decides to do once the engine reports an overlap.
void CoinRunner::handleCollisions(Engine& engine)
{
    isOnGround_ = false;

    // Static level geometry: stop the player and remember when it has footing.
    for (const Entity& platform : platforms_) {
        float pushX = 0.0F;
        float pushY = 0.0F;

        if (!Collision::getSeparation(player_, platform, pushX, pushY)) {
            continue;
        }

        if (pushY < 0.0F) {
            isOnGround_ = true;
        }

        Collision::resolve(player_, platform);
    }

    // Keep the player inside the world.
    const Rect bounds = player_.getBounds();
    if (bounds.x < 0.0F) {
        player_.setPosition(0.0F, player_.getY());
        player_.setVelocityX(0.0F);
    } else if (bounds.x + bounds.width > worldWidth_) {
        player_.setPosition(worldWidth_ - bounds.width, player_.getY());
        player_.setVelocityX(0.0F);
    }

    // Auto-moving object: costs a life and sends the player back to the start.
    if (player_.collidesWith(patrol_)) {
        lives_ -= 1;
        std::cout << "The patrol caught you! Lives left: " << lives_ << '\n';
        resetPlayer();

        if (lives_ <= 0) {
            std::cout << "Game over. Final score: " << score_ << '\n';
            engine.quit();
        }
        return;
    }

    // Pickup: score and respawn the coin somewhere else.
    if (player_.collidesWith(coin_)) {
        score_ += 1;
        std::cout << "Coin collected! Score: " << score_ << '\n';
        moveCoinToNextSpot();
    }
}

void CoinRunner::resetPlayer()
{
    player_.setPosition(playerStartX, playerStartY);
    player_.setVelocity(0.0F, 0.0F);
}

void CoinRunner::moveCoinToNextSpot()
{
    const int spot = score_ % coinSpotCount;
    coin_.setPosition(coinSpots[spot][0], coinSpots[spot][1]);
}

void CoinRunner::render(SDL_Renderer* renderer) const
{
    const auto drawEntity = [renderer](const Entity& entity, Uint8 red, Uint8 green, Uint8 blue) {
        const Rect bounds = entity.getBounds();
        const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};

        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        SDL_RenderFillRect(renderer, &rect);
    };

    for (const Entity& platform : platforms_) {
        drawEntity(platform, 90, 100, 130);
    }

    drawEntity(coin_, 250, 210, 80);
    drawEntity(patrol_, 230, 70, 70);
    drawEntity(player_, 240, 240, 255);

    char hud[96];
    std::snprintf(hud, sizeof(hud), "Score: %d   Lives: %d", score_, lives_);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDebugText(renderer, 24.0F, 24.0F, hud);
}

} // namespace

int main()
{
    try {
        Engine engine("Coin Runner", 1920, 1080);
        engine.setClearColor(20, 24, 40);

        CoinRunner game(engine);
        engine.run(game);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Engine failed to start: " << error.what() << '\n';
        return 1;
    }
}
