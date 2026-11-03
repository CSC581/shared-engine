// Milestone 4 Phase 5: replay, driven only by replay events.
// Arrows move through a level three screens wide; the camera follows. Red
// blocks raise DeathEvent and a respawn 1 s later. R starts and stops
// recording, Enter plays it back, 1/2/3 pick 0.5x/1x/2x, Space pauses the
// playback, Backspace returns to the live game, P pauses the live game.
#include "Components.hpp"
#include "Engine.hpp"
#include "Game.hpp"
#include "ReplaySystem.hpp"
#include "World.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr float levelWidth = 2400.0F;
constexpr float screenWidth = 800.0F;
constexpr float startX = 60.0F;
constexpr float startY = 400.0F;

class ReplayDemo : public Game {
public:
    explicit ReplayDemo(Engine& engine)
        : engine_(engine),
          events_(engine.events()),
          replay_(engine.events(), engine.gameTime(), engine.realTime())
    {
        world_.setEventManager(&events_);
        replay_.setFrameStateHook([this] { return "deaths " + std::to_string(deaths_); });

        GameObject& player = world_.create("player");
        player.add<Transform>(startX, startY, 30.0F, 30.0F);
        player.add<Motion>();
        player.add<Collider>();
        player.add<Renderable>(Color{80, 170, 255}).layer = 2;
        player.setActive(true);
        player_ = &player;

        // Markers every 200 units, so the scrolling shows.
        for (float x = 0.0F; x < levelWidth; x += 200.0F) {
            GameObject& marker = world_.create("marker");
            marker.add<Transform>(x, 520.0F, 100.0F, 20.0F);
            marker.add<Renderable>(Color{60, 80, 120});
            marker.setActive(true);
        }
        makeHazard(700.0F, 150.0F, 700.0F, 450.0F);
        makeHazard(1200.0F, 300.0F, 1500.0F, 300.0F);
        makeHazard(1900.0F, 100.0F, 1900.0F, 480.0F);

        subscriptions_ = {
            events_.subscribe(Events::Input, [this](const Event& e) { onInput(e); }),
            events_.subscribe(Events::Collision, [this](const Event& e) { onCollision(e); }),
            events_.subscribe(Events::Death, [this](const Event& e) { onDeath(e); }),
            events_.subscribe(Events::Spawn, [this](const Event& e) { onSpawn(e); }),
        };
        // Visual only: a replayed death flashes the screen, nothing more.
        replay_.events().subscribe(Events::Death, [this](const Event&) {
            flashUntil_ = engine_.realTime().now() + 300'000'000;
        });
    }

    ~ReplayDemo() override
    {
        for (const auto id : subscriptions_) {
            events_.unsubscribe(id);
        }
    }

    void handleInput(Engine&) override {}
    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine&) override
    {
        replay_.update();
        if (replay_.isPlaying()) {
            return;
        }
        auto* motion = player_->get<Motion>();
        motion->velocityX = 260.0F * static_cast<float>(held(SDL_SCANCODE_RIGHT) - held(SDL_SCANCODE_LEFT));
        motion->velocityY = 260.0F * static_cast<float>(held(SDL_SCANCODE_DOWN) - held(SDL_SCANCODE_UP));
        world_.update(time);

        Transform& body = *player_->get<Transform>();
        body.x = std::clamp(body.x, 0.0F, levelWidth - body.width);
        body.y = std::clamp(body.y, 0.0F, 600.0F - body.height);
        cameraX_ = std::clamp(body.x + body.width / 2.0F - screenWidth / 2.0F, 0.0F, levelWidth - screenWidth);
        replay_.capture(world_, cameraX_, 0.0F);
    }

    void render(SDL_Renderer* renderer) const override
    {
        const bool playing = replay_.isPlaying();
        if (playing) {
            renderWorld(renderer, replay_.world(), replay_.cameraX(), replay_.cameraY());
        } else {
            renderWorld(renderer, world_, cameraX_, 0.0F);
        }

        if (engine_.realTime().now() < flashUntil_) {
            SDL_SetRenderDrawColor(renderer, 230, 40, 40, 255);
            const SDL_FRect border{4.0F, 4.0F, 792.0F, 592.0F};
            SDL_RenderRect(renderer, &border);
        }

        SDL_SetRenderDrawColor(renderer, 230, 235, 245, 255);
        const double seconds = 1.0 / static_cast<double>(kGameTicsPerSecond);
        if (playing) {
            SDL_RenderDebugTextFormat(renderer, 16.0F, 12.0F, "REPLAY %.1fx%s  %.1f / %.1f s  (recorded %s)",
                                      replay_.speed(), replay_.isPlaybackPaused() ? " paused" : "",
                                      static_cast<double>(replay_.playbackPosition()) * seconds,
                                      static_cast<double>(replay_.recordedDuration()) * seconds,
                                      replay_.currentFrameState().c_str());
        } else {
            SDL_RenderDebugTextFormat(renderer, 16.0F, 12.0F, "%s  %.1f s recorded  next speed %.1fx  deaths %d%s",
                                      replay_.isRecording() ? "REC" : "LIVE",
                                      static_cast<double>(replay_.recordedDuration()) * seconds, replay_.speed(),
                                      deaths_, engine_.gameTime().isPaused() ? "  (paused)" : "");
        }
        SDL_RenderDebugText(renderer, 16.0F, 580.0F,
                            "arrows move | R record | Enter play | 1/2/3 speed | Space pause | Backspace stop | P pause game");
    }

private:
    void makeHazard(float fromX, float fromY, float toX, float toY)
    {
        GameObject& hazard = world_.create("hazard");
        hazard.add<Transform>(fromX, fromY, 40.0F, 40.0F);
        hazard.add<Motion>();
        hazard.add<PathMover>(PathMover::Linear{fromX, fromY, toX, toY}, 160.0F);
        hazard.add<Collider>(Collider::Kind::Trigger);
        hazard.add<Renderable>(Color{200, 50, 50}).layer = 1;
        hazard.setActive(true);
    }

    int held(SDL_Scancode key) const
    {
        const auto found = held_.find(key);
        return found != held_.end() && found->second ? 1 : 0;
    }

    void onInput(const Event& e)
    {
        if (e.get<std::string>("kind") != "key") {
            return;
        }
        const auto key = static_cast<SDL_Scancode>(e.get<std::int64_t>("key"));
        const bool pressed = e.get<std::string>("state") == "pressed";
        held_[key] = pressed;
        if (!pressed) {
            return;
        }
        switch (key) {
        case SDL_SCANCODE_R:
            events_.raise(Event(replay_.isRecording() ? Events::ReplayRecordStop : Events::ReplayRecordStart));
            break;
        case SDL_SCANCODE_RETURN:
            events_.raise(Event(Events::ReplayPlayStart));
            break;
        case SDL_SCANCODE_1:
            events_.raise(Event(Events::ReplaySpeed).set("speed", 0.5));
            break;
        case SDL_SCANCODE_2:
            events_.raise(Event(Events::ReplaySpeed).set("speed", 1.0));
            break;
        case SDL_SCANCODE_3:
            events_.raise(Event(Events::ReplaySpeed).set("speed", 2.0));
            break;
        case SDL_SCANCODE_SPACE:
            events_.raise(Event(Events::ReplayPause));
            break;
        case SDL_SCANCODE_BACKSPACE:
            events_.raise(Event(Events::ReplayPlayStop));
            break;
        case SDL_SCANCODE_P:
            // The replay owns the live timeline's pause while it plays.
            if (!replay_.isPlaying()) {
                engine_.gameTime().togglePause();
            }
            break;
        default:
            break;
        }
    }

    void onCollision(const Event& e)
    {
        // The player is created first, so it is always `a`.
        if (e.get<std::string>("phase") == "enter" && e.get<std::int64_t>("a") == player_->id() &&
            e.get<std::string>("tagB") == "hazard") {
            events_.raise(Event{Events::Death}.set("object", player_->id()).set("cause", "hazard"));
        }
    }

    void onDeath(const Event& e)
    {
        player_->setActive(false);
        ++deaths_;
        events_.raise(Event{Events::Spawn}.set("object", e.get<std::int64_t>("object"))
                                          .set("x", static_cast<double>(startX))
                                          .set("y", static_cast<double>(startY)),
                      kGameTicsPerSecond);
    }

    void onSpawn(const Event& e)
    {
        Transform& body = *player_->get<Transform>();
        body.x = static_cast<float>(e.get<double>("x"));
        body.y = static_cast<float>(e.get<double>("y"));
        player_->setActive(true);
    }

    Engine& engine_;
    EventManager& events_;
    World world_;
    ReplaySystem replay_;
    GameObject* player_ = nullptr;
    float cameraX_ = 0.0F;
    int deaths_ = 0;
    std::int64_t flashUntil_ = 0;
    std::map<SDL_Scancode, bool> held_;
    std::vector<EventManager::SubscriptionId> subscriptions_;
};

} // namespace

int main()
{
    Engine engine("Replay Demo", 800, 600);
    ReplayDemo game(engine);
    engine.run(game);
    return 0;
}
