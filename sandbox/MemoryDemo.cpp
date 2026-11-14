#include "Components.hpp"
#include "Engine.hpp"
#include "Game.hpp"
#include "Input.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
class MemoryDemo final : public Game {
public:
    explicit MemoryDemo(bool smoke) : smoke_(smoke) {}

    void handleInput(Engine& engine) override
    {
        if (Input::isKeyJustPressed(SDL_SCANCODE_ESCAPE)) engine.quit();
        if (Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
            for (int i = 0; i < 20; ++i) {
                if (!spawn()) break;
            }
        }
        if (Input::isKeyJustPressed(SDL_SCANCODE_F)) fill();
        if (Input::isKeyJustPressed(SDL_SCANCODE_R)) clear();
    }

    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine& engine) override
    {
        if (smoke_) {
            checkSmoke();
            engine.quit();
        } else {
            world_.update(time);
        }
    }

    void render(SDL_Renderer* renderer) const override
    {
        renderWorld(renderer, world_);
        SDL_SetRenderDrawColor(renderer, 230, 235, 245, 255);
        SDL_RenderDebugText(renderer, 20.0F, 20.0F, "MEMORY POOL: GameObject slots");
        SDL_RenderDebugText(renderer, 20.0F, 40.0F, "Space: +20   F: fill   R: clear   Esc: quit");
        SDL_RenderDebugTextFormat(renderer, 20.0F, 60.0F, "%zu / %zu slots   rejected: %zu",
                                  world_.objects().size(), world_.objectCapacity(), rejected_);
        SDL_RenderDebugText(renderer, 20.0F, 80.0F, "Objects expire after 4 seconds. Components still use the heap.");
        const float usage = static_cast<float>(world_.objects().size()) / world_.objectCapacity();
        const SDL_FRect background{20.0F, 100.0F, 760.0F, 12.0F};
        SDL_SetRenderDrawColor(renderer, 60, 70, 90, 255);
        SDL_RenderFillRect(renderer, &background);
        const SDL_FRect used{20.0F, 100.0F, 760.0F * usage, 12.0F};
        SDL_SetRenderDrawColor(renderer, usage >= 1.0F ? 240 : 60, usage >= 1.0F ? 70 : 210, 90, 255);
        SDL_RenderFillRect(renderer, &used);
    }

private:
    bool spawn()
    {
        GameObject* object = world_.tryCreate("particle");
        if (object == nullptr) {
            ++rejected_;
            return false;
        }
        const std::size_t position = spawned_++ % world_.objectCapacity();
        object->add<Transform>(30.0F + static_cast<float>(position % 20) * 38.0F,
                               140.0F + static_cast<float>(position / 20) * 38.0F, 20.0F, 20.0F);
        object->add<Renderable>(Color{240, 200, 60});
        object->add<Behavior>([this, remaining = 4.0](GameObject& self, const FrameTime& time) mutable {
            remaining -= time.dtSeconds;
            if (remaining <= 0.0) world_.destroy(self.id());
        });
        object->setActive(true);
        return true;
    }

    void fill()
    {
        while (spawn()) {}
    }

    void clear()
    {
        for (const auto& object : world_.objects()) world_.destroy(object->id());
    }

    void checkSmoke()
    {
        fill();
        if (world_.objects().size() != 200 || rejected_ != 1) {
            throw std::runtime_error("Pool fill/exhaustion check failed");
        }
        clear();
        if (world_.objects().size() != 200 || world_.tryCreate() != nullptr) {
            throw std::runtime_error("Clear released slots before flushing");
        }
        world_.update(FrameTime{0.0, 0});
        if (!world_.objects().empty()) throw std::runtime_error("Clear did not release slots");
        fill();
        if (world_.objects().size() != 200) throw std::runtime_error("Refill failed");
        world_.update(FrameTime{5.0, 0});
        if (!world_.objects().empty()) throw std::runtime_error("Expiry did not release slots");
        fill();
        if (world_.objects().size() != 200) throw std::runtime_error("Refill after expiry failed");
        std::cout << "Memory demo smoke passed: fill, deferred clear, refill, expiry, refill\n";
    }

    World world_{200};
    bool smoke_;
    std::size_t spawned_ = 0;
    std::size_t rejected_ = 0;
};
} // namespace

int main(int argc, char** argv)
{
    const bool smoke = argc == 2 && std::string(argv[1]) == "--smoke";
    if (argc > 1 && !smoke) {
        std::cerr << "Usage: memory-demo [--smoke]\n";
        return 2;
    }
    try {
        Engine engine("Milestone 5 Memory Pool", 800, 600);
        engine.setClearColor(20, 25, 40);
        MemoryDemo demo(smoke);
        engine.run(demo);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
