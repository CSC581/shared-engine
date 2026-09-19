// Section 3 Milestone 4: persistent worker threads + condition_variable barrier.
//
// Same gameplay as M3, but threads live for the whole run (ThreadExample-style
// wait/notify) instead of spawn+join every frame.
//
// Main thread: SDL events, Input, handleInput, publish frame, wait, carry,
//              collide, render.
// Worker A:    advance the ping-pong platform.
// Worker B:    gravity + player Entity::update.
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
#include <condition_variable>
#include <exception>
#include <iostream>
#include <mutex>
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

constexpr int workerCount = 2;

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

        // Persistent workers — created once, like ThreadExample's two threads.
        platformThread_ = std::thread([this] { platformLoop(); });
        characterThread_ = std::thread([this] { characterLoop(); });
    }

    ~ThreadLoopSandbox() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        // Wake anyone blocked in wait (workers or a late update).
        cv_.notify_all();

        if (platformThread_.joinable()) {
            platformThread_.join();
        }
        if (characterThread_.joinable()) {
            characterThread_.join();
        }
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
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (quit_) {
                return;
            }

            // Publish this frame's work (ThreadExample: flip state + notify).
            frameDt_ = deltaTime;
            workersDone_ = 0;
            ++frameId_;
            cv_.notify_all();

            // Wait until both workers finished this frameId (busy ≈ not done).
            cv_.wait(lock, [this] {
                return quit_ || workersDone_ == workerCount;
            });
        }

        // Carry after the barrier so only main writes the player for ride motion.
        if (groundedOnMoving_) {
            player_.setPosition(player_.getX() + platformDx_, player_.getY() + platformDy_);
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
            "M4 persistent+CV | A/D move  Space jump  Esc quit");
        SDL_RenderDebugTextFormat(
            renderer,
            16.0F,
            36.0F,
            "platform frames: %d   character frames: %d   frameId: %llu",
            platformWorkerFrames_.load(std::memory_order_relaxed),
            characterWorkerFrames_.load(std::memory_order_relaxed),
            static_cast<unsigned long long>(frameId_.load(std::memory_order_relaxed)));
    }

private:
    void platformLoop()
    {
        std::uint64_t lastFrame = 0;
        while (true) {
            float dt = 0.0F;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // ThreadExample thread 1: wait until there is work (or quit).
                cv_.wait(lock, [this, lastFrame] {
                    return quit_ || frameId_ != lastFrame;
                });
                if (quit_) {
                    return;
                }
                lastFrame = frameId_;
                dt = frameDt_;
            }

            advancePlatform(dt, platformDx_, platformDy_);
            platformWorkerFrames_.fetch_add(1, std::memory_order_relaxed);
            markWorkerDone();
        }
    }

    void characterLoop()
    {
        std::uint64_t lastFrame = 0;
        while (true) {
            float dt = 0.0F;
            float moveAxis = 0.0F;
            bool onGround = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this, lastFrame] {
                    return quit_ || frameId_ != lastFrame;
                });
                if (quit_) {
                    return;
                }
                lastFrame = frameId_;
                dt = frameDt_;
                // Snapshot main-thread input/state under the barrier lock.
                moveAxis = moveAxis_;
                onGround = onGround_;
            }

            player_.setVelocityX(moveAxis * moveSpeed);
            if (!onGround) {
                Physics::applyGravity(player_, dt);
            }
            player_.update(dt);
            characterWorkerFrames_.fetch_add(1, std::memory_order_relaxed);
            markWorkerDone();
        }
    }

    void markWorkerDone()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++workersDone_;
        if (workersDone_ == workerCount) {
            // ThreadExample: notify the waiter (main) that work is finished.
            cv_.notify_all();
        }
    }

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

        if (landOn(floor_)) {
            onGround_ = true;
        }

        if (landOn(platform_)) {
            onGround_ = true;
            groundedOnMoving_ = true;
        }

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

    float platformDx_ = 0.0F;
    float platformDy_ = 0.0F;

    // Frame barrier (shared mutex + CV, same tools as ThreadExample).
    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    float frameDt_ = 0.0F;
    int workersDone_ = 0;
    std::atomic<std::uint64_t> frameId_{0};

    std::thread platformThread_;
    std::thread characterThread_;

    mutable std::atomic<int> platformWorkerFrames_{0};
    mutable std::atomic<int> characterWorkerFrames_{0};
};

} // namespace

int main()
{
    try {
        Engine engine("Thread Loop Sandbox (M4)", windowWidth, windowHeight);
        engine.setClearColor(28, 32, 40);
        ThreadLoopSandbox game;
        engine.run(game);
    } catch (const std::exception& ex) {
        std::cerr << "ThreadLoopSandbox failed: " << ex.what() << '\n';
        return 1;
    }
    return 0;
}
