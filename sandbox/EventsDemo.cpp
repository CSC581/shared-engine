// Milestone 4 Phase 2: the four required events, all through engine.events().
// Arrows move, SPACE spawns a coin, LSHIFT+J dashes (chord). Touching red
// raises DeathEvent, which respawns the player 1 s later in game time.
#include "Components.hpp"
#include "Engine.hpp"
#include "Game.hpp"
#include "World.hpp"

#include <map>
#include <random>
#include <string>
#include <vector>

namespace {

class EventsDemo : public Game {
public:
    explicit EventsDemo(Engine& engine) : events_(engine.events())
    {
        world_.setEventManager(&events_);
        engine.registerChord("dash", {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_J});

        GameObject& player = world_.create("player");
        player.add<Transform>(60, 280, 30, 30);
        player.add<Motion>();
        player.add<Collider>();
        player.add<Renderable>(Color{80, 170, 255}).layer = 1;
        player.setActive(true);
        player_ = &player;

        GameObject& zone = world_.create("deathzone");
        zone.add<Transform>(380, 120, 60, 360);
        zone.add<Collider>(Collider::Kind::Trigger);
        zone.add<Renderable>(Color{200, 50, 50});
        zone.setActive(true);

        subscriptions_ = {
            events_.subscribe(Events::Input, [this](const Event& e) { onInput(e); }),
            events_.subscribe(Events::Collision, [this](const Event& e) { onCollision(e); }),
            events_.subscribe(Events::Death, [this](const Event& e) { onDeath(e); }),
            events_.subscribe(Events::Spawn, [this](const Event& e) { onSpawn(e); }),
        };
    }

    ~EventsDemo() override
    {
        for (const auto id : subscriptions_) {
            events_.unsubscribe(id);
        }
    }

    void handleInput(Engine&) override {}
    void update(float, Engine&) override {}
    void update(const FrameTime& time, Engine&) override { world_.update(time); }
    void render(SDL_Renderer* renderer) const override { renderWorld(renderer, world_); }

private:
    void onInput(const Event& e)
    {
        const bool pressed = e.get<std::string>("state") == "pressed";
        if (e.get<std::string>("kind") == "chord") {
            if (pressed && player_->isActive()) {
                player_->get<Transform>()->x += 120.0F;
            }
            return;
        }
        const auto key = static_cast<SDL_Scancode>(e.get<std::int64_t>("key"));
        held_[key] = pressed;
        if (key == SDL_SCANCODE_SPACE && pressed) {
            std::uniform_real_distribution<double> x(20, 760);
            std::uniform_real_distribution<double> y(20, 560);
            events_.raise(Event{Events::Spawn}.set("kind", "coin").set("x", x(rng_)).set("y", y(rng_)));
        }
        auto* motion = player_->get<Motion>();
        motion->velocityX = 220.0F * (held_[SDL_SCANCODE_RIGHT] - held_[SDL_SCANCODE_LEFT]);
        motion->velocityY = 220.0F * (held_[SDL_SCANCODE_DOWN] - held_[SDL_SCANCODE_UP]);
    }

    void onCollision(const Event& e)
    {
        // The player is created first, so it is always `a`.
        if (e.get<std::string>("phase") != "enter" || e.get<std::int64_t>("a") != player_->id()) {
            return;
        }
        if (e.get<std::string>("tagB") == "deathzone") {
            events_.raise(Event{Events::Death}.set("object", player_->id()).set("cause", "deathzone"));
        } else if (e.get<std::string>("tagB") == "coin") {
            world_.destroy(static_cast<ObjectId>(e.get<std::int64_t>("b")));
        }
    }

    void onDeath(const Event& e)
    {
        player_->setActive(false);
        events_.raise(Event{Events::Spawn}.set("object", e.get<std::int64_t>("object"))
                                          .set("x", 60.0).set("y", 280.0),
                      kGameTicsPerSecond);
    }

    void onSpawn(const Event& e)
    {
        const auto x = static_cast<float>(e.get<double>("x"));
        const auto y = static_cast<float>(e.get<double>("y"));
        if (e.has("object")) {
            player_->get<Transform>()->x = x;
            player_->get<Transform>()->y = y;
            player_->setActive(true);
            return;
        }
        GameObject& coin = world_.create("coin");
        coin.add<Transform>(x, y, 16, 16);
        coin.add<Collider>(Collider::Kind::Trigger);
        coin.add<Renderable>(Color{240, 200, 40});
        coin.setActive(true);
    }

    EventManager& events_;
    World world_;
    GameObject* player_ = nullptr;
    std::map<SDL_Scancode, bool> held_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
    std::mt19937 rng_{581};
};

} // namespace

int main()
{
    Engine engine("Events Demo", 800, 600);
    EventsDemo game(engine);
    engine.run(game);
    return 0;
}
