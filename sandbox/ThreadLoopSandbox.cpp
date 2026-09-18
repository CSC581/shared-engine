// Section 3 Milestone 3: multithreaded update via per-frame spawn + join.
//
// Main thread: SDL events, Input, handleInput, collide, render.
// Worker A:    advance the ping-pong platform (Entity writes only).
// Worker B:    gravity + player Entity::update.
// After join:  carry the player if grounded, then collide.
//
// Build: cmake --build build --target thread-loop-sandbox
// Run:   ./build/thread-loop-sandbox
#include "Collision.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "Physics.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <thread>

namespace {

constexpr int windowWidth = 900;
constexpr int windowHeight = 600;

constexpr float playerWidth = 40.0F;
constexpr float playerHeight = 56.0F;
constexpr float moveSpeed = 220.0F;
constexpr float jumpSpeed = 420.0F;
constexpr float gravity = 980.0F;

constexpr float floorY = 520.0F;
constexpr float floorHeight = 40.0F;

constexpr float platformWidth = 140.0F;
constexpr float platformHeight = 28.0F;
constexpr float platformY = 360.0F;
constexpr float platformSpeed = 160.0F;
constexpr float platformLeft = 120.0F;
constexpr float platformRight = 640.0F;

void fillRect(SDL_Renderer* renderer, const Rect& bounds, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    const SDL_FRect rect{bounds.x, bounds.y, bounds.width, bounds.height};
    SDL_RenderFillRect(renderer, &rect);
}

class ThreadLoopSandbox final : public Game {
public:
    ThreadLoopSandbox()
        : player_((windowWidth - playerWidth) * 0.5F, floorY - playerHeight, playerWidth, playerHeight),
          floor_(0.0F, floorY, static_cast<float>(windowWidth), floorHeight),
          platform_(platformLeft, platformY, platformWidth, platformHeight)
    {
        Physics::setGravity(gravity);
    }

    void handleInput(Engine& engine) override
    {
        // Input polling stays on the main thread (static Input + SDL).
        moveAxis_ = 0.0F;
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            moveAxis_ -= 1.0F;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            moveAxis_ += 1.0F;
        }

        if (onGround_ && Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
            player_.setVelocityY(-jumpSpeed);
            onGround_ = false;
            groundedOnMoving_ = false;
        }

        if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
            engine.quit();
        }
    }

    void update(float deltaTime, Engine&) override
    {
        // Snapshot of this frame's platform motion, filled by the platform
        // worker and applied to the player only after both workers join.
        float platformDx = 0.0F;
        float platformDy = 0.0F;

        std::thread platformWorker([this, deltaTime, &platformDx, &platformDy] {
            advancePlatform(deltaTime, platformDx, platformDy);
            platformWorkerFrames_.fetch_add(1, std::memory_order_relaxed);
        });

        std::thread characterWorker([this, deltaTime] {
            // Horizontal intent was sampled on main; integrate on this thread.
            player_.setVelocityX(moveAxis_ * moveSpeed);
            if (!onGround_) {
                Physics::applyGravity(player_, deltaTime);
            }
            player_.update(deltaTime);
            characterWorkerFrames_.fetch_add(1, std::memory_order_relaxed);
        });

        platformWorker.join();
        characterWorker.join();

        // Carry after join so only one thread writes the player for ride motion.
        if (groundedOnMoving_) {
            player_.setPosition(player_.getX() + platformDx, player_.getY() + platformDy);
        }

        resolveCollisions();
    }

    void render(SDL_Renderer* renderer) const override
    {
        fillRect(renderer, floor_.getBounds(), 70, 90, 70);
        fillRect(renderer, platform_.getBounds(), 200, 150, 60);
        fillRect(renderer, player_.getBounds(), 90, 170, 230);

        SDL_SetRenderDrawColor(renderer, 230, 230, 230, 255);
        SDL_RenderDebugTextFormat(
            renderer,
            16.0F,
            16.0F,
            "M3 spawn+join | A/D move  Space jump  Esc quit");
        SDL_RenderDebugTextFormat(
            renderer,
            16.0F,
            36.0F,
            "platform frames: %d   character frames: %d",
            platformWorkerFrames_.load(std::memory_order_relaxed),
            characterWorkerFrames_.load(std::memory_order_relaxed));
    }

private:
    void advancePlatform(float deltaTime, float& outDx, float& outDy)
    {
        const float prevX = platform_.getX();
        const float prevY = platform_.getY();
        const float targetX = movingToRight_ ? platformRight : platformLeft;
        const float dx = targetX - prevX;
        const float distance = std::fabs(dx);
        const float step = platformSpeed * deltaTime;

        if (distance <= step) {
            platform_.setPosition(targetX, platformY);
            movingToRight_ = !movingToRight_;
        } else {
            const float dir = dx > 0.0F ? 1.0F : -1.0F;
            platform_.setPosition(prevX + dir * step, platformY);
        }

        outDx = platform_.getX() - prevX;
        outDy = platform_.getY() - prevY;
    }

    void resolveCollisions()
    {
        onGround_ = false;
        groundedOnMoving_ = false;

        // Floor support.
        if (landOn(floor_)) {
            onGround_ = true;
        }

        // Moving platform support (after carry, so feet meet the top).
        if (landOn(platform_)) {
            onGround_ = true;
            groundedOnMoving_ = true;
        }

        // Side push out of the platform body if overlapping horizontally mid-air.
        float pushX = 0.0F;
        float pushY = 0.0F;
        if (Collision::getSeparation(player_, platform_, pushX, pushY)) {
            if (std::fabs(pushX) >= std::fabs(pushY)) {
                player_.setPosition(player_.getX() + pushX, player_.getY());
                player_.setVelocityX(0.0F);
            }
        }
    }

    bool landOn(const Entity& support)
    {
        const Rect player = player_.getBounds();
        const Rect box = support.getBounds();
        const bool overlapsX =
            player.x < box.x + box.width && player.x + player.width > box.x;
        const float foot = player.y + player.height;
        const bool crossingTop =
            player_.getVelocityY() >= 0.0F && overlapsX &&
            foot >= box.y && foot <= box.y + box.height + 8.0F;

        if (!crossingTop) {
            return false;
        }

        player_.setPosition(player_.getX(), box.y - playerHeight);
        player_.setVelocityY(0.0F);
        return true;
    }

    Entity player_;
    Entity floor_;
    Entity platform_;

    float moveAxis_ = 0.0F;
    bool onGround_ = true;
    bool groundedOnMoving_ = false;
    bool movingToRight_ = true;

    // Prove both workers ran (increments once per frame each).
    mutable std::atomic<int> platformWorkerFrames_{0};
    mutable std::atomic<int> characterWorkerFrames_{0};
};

} // namespace

int main()
{
    try {
        Engine engine("Thread Loop Sandbox (M3)", windowWidth, windowHeight);
        engine.setClearColor(28, 32, 40);
        ThreadLoopSandbox game;
        engine.run(game);
    } catch (const std::exception& ex) {
        std::cerr << "ThreadLoopSandbox failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
