// The real Engine::run under SDL's dummy video driver: what other event and
// replay tests assume about the loop, checked against the loop itself.
#include "Components.hpp"
#include "Engine.hpp"
#include "Game.hpp"
#include "ReplaySystem.hpp"
#include "World.hpp"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_video.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
        ++failures;
    }
}

constexpr int kDesignWidth = 320;
constexpr int kDesignHeight = 240;
// A loop that never reaches its condition fails instead of hanging the test.
constexpr int kMaxFrames = 20000;

// An Engine with no window on the screen. SDL_Quit resets hints, so each Engine
// needs them set again.
struct HeadlessEngine {
    HeadlessEngine()
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
    }
};

class TestEngine : private HeadlessEngine, public Engine {
public:
    TestEngine() : Engine("engine-loop-tests", kDesignWidth, kDesignHeight)
    {
        expect(std::string(SDL_GetCurrentVideoDriver()) == "dummy", "the test runs without a real window");
    }
};

class OrderGame final : public Game {
public:
    explicit OrderGame(Engine& engine) : engine_(engine)
    {
        engine_.events().subscribe("FromInput", [this](const Event&) { log_.push_back("input event"); });
        engine_.events().subscribe("FromUpdate", [this](const Event&) { log_.push_back("update event"); });
    }

    void handleInput(Engine&) override
    {
        log_.push_back("handleInput");
        engine_.events().raise(Event("FromInput"));
    }

    using Game::update;
    void update(float, Engine&) override
    {
        log_.push_back("update");
        engine_.events().raise(Event("FromUpdate"));
    }

    void render(SDL_Renderer*) const override
    {
        log_.push_back("render");
        engine_.quit();
    }

    const std::vector<std::string>& log() const { return log_; }

private:
    Engine& engine_;
    mutable std::vector<std::string> log_;
};

void eventsRunBetweenTheFrameSteps()
{
    TestEngine engine;
    OrderGame game(engine);
    engine.run(game);
    expect(game.log() == std::vector<std::string>{"handleInput", "input event", "update", "update event", "render"},
           "events raised in handleInput run before update, and events raised in update before render");
}

// Raises Low then High with time between them, so their stamps differ.
class PriorityGame final : public Game {
public:
    explicit PriorityGame(Engine& engine) : engine_(engine)
    {
        engine_.events().subscribe("Ordered", [this](const Event& event) {
            order.push_back(event.get<std::string>("name"));
        });
    }

    void handleInput(Engine&) override
    {
        if (frame_ == 0) {
            raiseLowThenHigh("input low", "input high");
        }
    }

    using Game::update;
    void update(float, Engine&) override
    {
        if (frame_ == 0) {
            raiseLowThenHigh("update low", "update high");
        }
        if (++frame_ == 2) {
            engine_.quit();
        }
    }

    void render(SDL_Renderer*) const override {}

    std::vector<std::string> order;

private:
    void raiseLowThenHigh(const char* low, const char* high)
    {
        Event first("Ordered");
        first.priority = EventPriority::Low;
        first.set("name", low);
        engine_.events().raise(first);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        Event second("Ordered");
        second.priority = EventPriority::High;
        second.set("name", high);
        engine_.events().raise(second);
    }

    Engine& engine_;
    int frame_ = 0;
};

void priorityWorksOnTheGameClock()
{
    TestEngine engine;
    PriorityGame game(engine);
    engine.run(game);
    expect(game.order == std::vector<std::string>{"input high", "input low", "update high", "update low"},
           "High runs before an older Low at both dispatches of a frame");
}

// Records a moving box, plays it back, and notes what was actually drawn.
class ReplayGame final : public Game {
public:
    enum class StartFrom { HandleInput, Update };

    ReplayGame(Engine& engine, int framesToRecord, StartFrom startFrom)
        : engine_(engine),
          replay_(engine.events(), engine.gameTime(), engine.realTime()),
          framesToRecord_(framesToRecord),
          startFrom_(startFrom)
    {
        box_ = &world_.create("box");
        box_->add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
        box_->add<Renderable>(Color{1, 2, 3});
        box_->setActive(true);
        replay_.setFrameStateHook([this] { return std::to_string(static_cast<int>(box_->get<Transform>()->x)); });
        engine_.events().raise(Event(Events::ReplayRecordStart));
    }

    void handleInput(Engine&) override
    {
        if (startPlaybackNext_ && startFrom_ == StartFrom::HandleInput) {
            startPlayback();
        }
    }

    using Game::update;
    void update(float, Engine&) override {}

    void update(const FrameTime& time, Engine&) override
    {
        replay_.update();
        if (++frames_ == kMaxFrames) {
            engine_.quit();
        }
        if (replay_.isPlaying()) {
            return;
        }
        if (started_) {
            engine_.quit();
        } else if (startPlaybackNext_) {
            if (startFrom_ == StartFrom::Update) {
                startPlayback();
            }
        } else if (replay_.isRecording()) {
            box_->get<Transform>()->x = static_cast<float>(recorded_ * 10);
            world_.update(time);
            replay_.capture(world_);
            if (++recorded_ == framesToRecord_) {
                engine_.events().raise(Event(Events::ReplayRecordStop));
                startPlaybackNext_ = true;
            }
        }
    }

    void render(SDL_Renderer* renderer) const override
    {
        renderWorld(renderer, replay_.isPlaying() ? replay_.world() : world_);
        if (replay_.isPlaying()) {
            ++playingRenders;
            lastHud = replay_.currentFrameState();
            for (const auto& object : replay_.world().objects()) {
                if (object->isActive()) {
                    lastShownX = object->get<Transform>()->x;
                }
            }
        }
    }

    bool timelineRunningAgain() const { return !engine_.gameTime().isPaused(); }
    bool endedByItself() const { return started_ && !replay_.isPlaying() && frames_ < kMaxFrames; }

    mutable int playingRenders = 0;
    mutable float lastShownX = -1.0F;
    mutable std::string lastHud;

private:
    void startPlayback()
    {
        startPlaybackNext_ = false;
        started_ = true;
        engine_.events().raise(Event(Events::ReplayPlayStart));
    }

    Engine& engine_;
    World world_;
    ReplaySystem replay_;
    GameObject* box_ = nullptr;
    int framesToRecord_;
    StartFrom startFrom_;
    int recorded_ = 0;
    int frames_ = 0;
    bool startPlaybackNext_ = false;
    bool started_ = false;
};

void replayDrawsItsLastFrame(int framesToRecord, ReplayGame::StartFrom startFrom, const std::string& name)
{
    TestEngine engine;
    ReplayGame game(engine, framesToRecord, startFrom);
    engine.run(game);

    const int lastX = (framesToRecord - 1) * 10;
    expect(game.endedByItself(), name + ": playback ends by itself");
    expect(game.playingRenders >= 1, name + ": playback is drawn at least once");
    expect(static_cast<int>(game.lastShownX) == lastX, name + ": the last recorded position is drawn");
    expect(game.lastHud == std::to_string(lastX), name + ": the last recorded state is available when drawing");
    expect(game.timelineRunningAgain(), name + ": the game runs again afterwards");
}

} // namespace

int main()
{
    try {
        eventsRunBetweenTheFrameSteps();
        priorityWorksOnTheGameClock();
        replayDrawsItsLastFrame(1, ReplayGame::StartFrom::HandleInput, "one frame, started in handleInput");
        replayDrawsItsLastFrame(1, ReplayGame::StartFrom::Update, "one frame, started in update");
        replayDrawsItsLastFrame(120, ReplayGame::StartFrom::HandleInput, "120 frames, started in handleInput");
        replayDrawsItsLastFrame(120, ReplayGame::StartFrom::Update, "120 frames, started in update");
    } catch (const std::exception& exception) {
        std::cerr << "Test failed with exception: " << exception.what() << '\n';
        return 1;
    }

    if (failures != 0) {
        return 1;
    }
    std::cout << "Engine loop tests passed\n";
    return 0;
}
