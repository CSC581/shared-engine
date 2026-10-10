// Section 3 Milestone 4: persistent worker threads + condition_variable barrier.
//
// Main thread: SDL events, Input, publish frame, wait, carry, collide, render.
// Worker A:    update the PathMover-driven platform tag.
// Worker B:    update the Gravity/Motion-driven player tag.
//
// Build: cmake --build build --target thread-loop-sandbox
// Run:   ./build/thread-loop-sandbox
#include "Components.hpp"
#include "Engine.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
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

class ThreadLoopSandbox final : public Game {
public:
    ThreadLoopSandbox()
    {
        floor_ = &world_.create("floor");
        floor_->add<Transform>(0.0F, floorY, static_cast<float>(windowWidth), floorHeight);
        floor_->add<Collider>();
        floor_->add<Renderable>(Color{70, 90, 70}).layer = 0;
        floor_->setActive(true);

        platform_ = &world_.create("platform");
        platform_->add<PathMover>(
            PathMover::Linear{platformLeft, platformY, platformRight, platformY}, platformSpeed);
        Transform& platformTransform = *platform_->get<Transform>();
        platformTransform.width = platformWidth;
        platformTransform.height = platformHeight;
        platform_->add<Collider>();
        platform_->add<Renderable>(Color{200, 150, 60}).layer = 1;
        platform_->setActive(true);

        player_ = &world_.create("player");
        player_->add<Transform>((windowWidth - playerWidth) * 0.5F,
                                floorY - playerHeight, playerWidth, playerHeight);
        player_->add<Gravity>(gravity);
        Collider& playerCollider = player_->add<Collider>();
        playerCollider.onCollide = [this](GameObject& self, GameObject& other) {
            const Transform& playerTransform = *self.get<Transform>();
            const Transform& support = *other.get<Transform>();
            const float playerBottom = playerTransform.y + playerTransform.height;
            if (std::fabs(playerBottom - support.y) > 0.01F) {
                return;
            }
            onGround_ = true;
            if (other.id() == platform_->id()) {
                groundedOnMoving_ = true;
            }
        };
        player_->add<Renderable>(Color{90, 170, 230}).layer = 2;
        player_->setActive(true);

        // Persistent workers are created only after the World is fully built;
        // no object or component structure changes while they update it.
        platformThread_ = std::thread([this] { platformLoop(); });
        characterThread_ = std::thread([this] { characterLoop(); });
    }

    ~ThreadLoopSandbox() override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
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
        moveAxis_ = 0.0F;
        if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) {
            moveAxis_ -= 1.0F;
        }
        if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) {
            moveAxis_ += 1.0F;
        }

        if (onGround_ && Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
            jumpRequested_ = true;
            onGround_ = false;
            groundedOnMoving_ = false;
        }

        if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) {
            engine.quit();
        }
    }

    // The engine dispatches the FrameTime overload below. This remains only
    // because the older seconds-only function is still pure virtual in Game.
    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine&) override
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (quit_) {
                return;
            }

            frameTime_ = time;
            publishedMoveAxis_ = moveAxis_;
            publishedOnGround_ = onGround_;
            publishedJump_ = jumpRequested_;
            jumpRequested_ = false;
            workersDone_ = 0;
            ++frameId_;
            cv_.notify_all();

            cv_.wait(lock, [this] {
                return quit_ || workersDone_ == workerCount;
            });
        }

        // The platform's PathMover and Motion ran on its worker. Carrying is a
        // main-thread game rule, deliberately separate from generic collision.
        if (groundedOnMoving_) {
            Transform& playerTransform = *player_->get<Transform>();
            playerTransform.x += platformDx_;
            playerTransform.y += platformDy_;
        }

        onGround_ = false;
        groundedOnMoving_ = false;
        world_.resolveCollisions();
        world_.flushDestroyed();
    }

    void render(SDL_Renderer* renderer) const override
    {
        renderWorld(renderer, world_);

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
            FrameTime time{0.0, 0};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this, lastFrame] {
                    return quit_ || frameId_ != lastFrame;
                });
                if (quit_) {
                    return;
                }
                lastFrame = frameId_;
                time = frameTime_;
            }

            const Transform& before = *platform_->get<Transform>();
            const float previousX = before.x;
            const float previousY = before.y;
            world_.updateComponents(time, "platform");
            const Transform& after = *platform_->get<Transform>();
            platformDx_ = after.x - previousX;
            platformDy_ = after.y - previousY;

            platformWorkerFrames_.fetch_add(1, std::memory_order_relaxed);
            markWorkerDone();
        }
    }

    void characterLoop()
    {
        std::uint64_t lastFrame = 0;
        while (true) {
            FrameTime time{0.0, 0};
            float moveAxis = 0.0F;
            bool onGround = false;
            bool jump = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this, lastFrame] {
                    return quit_ || frameId_ != lastFrame;
                });
                if (quit_) {
                    return;
                }
                lastFrame = frameId_;
                time = frameTime_;
                moveAxis = publishedMoveAxis_;
                onGround = publishedOnGround_;
                jump = publishedJump_;
            }

            Motion& motion = *player_->get<Motion>();
            motion.velocityX = moveAxis * moveSpeed;
            if (jump) {
                motion.velocityY = -jumpSpeed;
            } else if (onGround && motion.velocityY > 0.0F) {
                motion.velocityY = 0.0F;
            }
            // Gravity remains active while grounded so the player overlaps its
            // support by a tiny amount each frame. World then resolves that
            // overlap and the collision callback refreshes grounded state.
            player_->get<Gravity>()->acceleration = gravity;
            world_.updateComponents(time, "player");

            characterWorkerFrames_.fetch_add(1, std::memory_order_relaxed);
            markWorkerDone();
        }
    }

    void markWorkerDone()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++workersDone_;
        if (workersDone_ == workerCount) {
            cv_.notify_all();
        }
    }

    World world_;
    GameObject* player_ = nullptr;
    GameObject* floor_ = nullptr;
    GameObject* platform_ = nullptr;

    float moveAxis_ = 0.0F;
    bool jumpRequested_ = false;
    bool onGround_ = true;
    bool groundedOnMoving_ = false;

    float publishedMoveAxis_ = 0.0F;
    bool publishedOnGround_ = true;
    bool publishedJump_ = false;

    float platformDx_ = 0.0F;
    float platformDy_ = 0.0F;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false;
    FrameTime frameTime_{0.0, 0};
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
    } catch (const std::exception& exception) {
        std::cerr << "ThreadLoopSandbox failed: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
