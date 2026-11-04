#include "Components.hpp"
#include "EventManager.hpp"
#include "ReplaySystem.hpp"
#include "TimeSource.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what, int line)
{
    if (!ok) {
        std::cerr << "line " << line << ": " << what << '\n';
        ++failures;
    }
}
#define CHECK(x) check((x), #x, __LINE__)

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.001F;
}

// A game in miniature: real time drives game time at 1000 real tics per game
// tic, as the engine's nanoseconds drive its microseconds.
struct Rig {
    ManualClock real;
    Timeline game{real, 1000};
    EventManager events{game};
    World world;
    ReplaySystem replay{events, game, real, 1000};
    GameObject* box = nullptr;

    Rig()
    {
        box = &world.create("box");
        box->add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
        box->add<Renderable>(Color{1, 2, 3}).layer = 2;
        box->setActive(true);
    }

    void advance(std::int64_t gameTics) { real.advance(gameTics * 1000); }

    void control(const char* type)
    {
        events.raise(Event(type));
        events.dispatch();
    }

    // Records frames every 100 game tics with the box at x = 10 per frame and
    // the camera at 5 per frame, from game time 0 to 100 * (frames - 1).
    void recordMovingBox(int frames)
    {
        control(Events::ReplayRecordStart);
        for (int i = 0; i < frames; ++i) {
            if (i > 0) {
                advance(100);
            }
            box->get<Transform>()->x = 10.0F * static_cast<float>(i);
            replay.capture(world, 5.0F * static_cast<float>(i), 1.0F);
        }
        control(Events::ReplayRecordStop);
    }

    std::vector<const GameObject*> shown() const
    {
        std::vector<const GameObject*> result;
        for (const auto& object : replay.world().objects()) {
            if (object->isActive()) {
                result.push_back(object.get());
            }
        }
        return result;
    }
};

void playbackFollowsTheRecording()
{
    Rig rig;
    rig.recordMovingBox(11);
    CHECK(!rig.replay.isRecording());
    CHECK(rig.replay.frameCount() == 11);
    CHECK(rig.replay.recordedDuration() == 1000);

    rig.control(Events::ReplayPlayStart);
    CHECK(rig.replay.isPlaying());
    CHECK(rig.game.isPaused());
    auto shown = rig.shown();
    CHECK(shown.size() == 1);
    if (shown.size() == 1) {
        CHECK(shown[0]->tag() == "box");
        CHECK(near(shown[0]->get<Transform>()->x, 0.0F));
        CHECK(shown[0]->get<Renderable>()->layer == 2);
        CHECK(shown[0]->get<Renderable>()->color.blue == 3);
    }

    // Between two frames positions and the camera are blended.
    rig.advance(250);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 250);
    shown = rig.shown();
    CHECK(shown.size() == 1 && near(shown[0]->get<Transform>()->x, 25.0F));
    CHECK(near(rig.replay.cameraX(), 12.5F));
    CHECK(near(rig.replay.cameraY(), 1.0F));
    CHECK(near(rig.box->get<Transform>()->x, 100.0F));

    // The end raises ReplayPlayStop on the live manager.
    rig.advance(800);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 1000);
    shown = rig.shown();
    CHECK(shown.size() == 1 && near(shown[0]->get<Transform>()->x, 100.0F));
    // The engine dispatches before it renders, so the last frame must survive
    // this dispatch. The stop comes with the next update.
    rig.events.dispatch();
    CHECK(rig.replay.isPlaying());
    rig.replay.update();
    rig.events.dispatch();
    CHECK(!rig.replay.isPlaying());
    CHECK(!rig.game.isPaused());
    CHECK(rig.replay.world().objects().empty());
    CHECK(near(rig.box->get<Transform>()->x, 100.0F));
    CHECK(rig.replay.frameCount() == 11);
}

void frameStateComesBackInOrder()
{
    Rig rig;
    int frame = 0;
    rig.replay.setFrameStateHook([&] { return "score " + std::to_string(frame++); });
    rig.recordMovingBox(4);

    rig.control(Events::ReplayPlayStart);
    std::vector<std::string> states{rig.replay.currentFrameState()};
    for (int i = 0; i < 3; ++i) {
        rig.advance(100);
        rig.replay.update();
        states.push_back(rig.replay.currentFrameState());
    }
    CHECK((states == std::vector<std::string>{"score 0", "score 1", "score 2", "score 3"}));
    rig.replay.update();
    rig.events.dispatch();
    CHECK(rig.replay.currentFrameState().empty());
}

void speedAndPauseControlPlayback()
{
    Rig rig;
    rig.recordMovingBox(11);

    // 2x finishes the 1000-tic recording in 500 tics of real time.
    rig.events.raise(Event(Events::ReplayPlayStart).set("speed", 2.0));
    rig.events.dispatch();
    rig.advance(499);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 998);
    rig.advance(1);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 1000);
    rig.replay.update();
    rig.events.dispatch();
    CHECK(!rig.replay.isPlaying());

    // The speed carries over to the next playback; integers are accepted.
    rig.control(Events::ReplayPlayStart);
    CHECK(rig.replay.speed() == 2.0);
    rig.events.raise(Event(Events::ReplaySpeed).set("speed", 1));
    rig.events.dispatch();
    rig.advance(200);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 200);

    // Paused playback does not advance; an explicit `paused` sets the state.
    rig.control(Events::ReplayPause);
    CHECK(rig.replay.isPlaybackPaused());
    rig.advance(300);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 200);
    rig.events.raise(Event(Events::ReplayPause).set("paused", false));
    rig.events.dispatch();
    rig.advance(100);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 300);

    rig.events.raise(Event(Events::ReplaySpeed).set("speed", 0.5));
    rig.events.dispatch();
    rig.advance(200);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 400);

    bool threw = false;
    rig.events.raise(Event(Events::ReplaySpeed).set("speed", -1.0));
    try {
        rig.events.dispatch();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    rig.events.raise(Event(Events::ReplaySpeed));
    try {
        rig.events.dispatch();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(rig.replay.speed() == 0.5);
}

void recordedEventsReachOnlyTheReplayManager()
{
    Rig rig;
    int liveHits = 0;
    rig.events.subscribe("Hit", [&](const Event&) { ++liveHits; });

    rig.control(Events::ReplayRecordStart);
    rig.events.raise(Event("Early"));
    rig.events.dispatch();
    rig.replay.capture(rig.world);
    rig.advance(100);
    Event hit("Hit");
    hit.origin = 7;
    hit.set("n", 1);
    rig.events.raise(hit);
    rig.events.dispatch();
    rig.replay.capture(rig.world);
    rig.advance(100);
    rig.events.raise(Event("Hit").set("n", 2), 50);
    rig.replay.capture(rig.world);
    rig.advance(50);
    rig.events.dispatch();
    rig.advance(50);
    rig.replay.capture(rig.world);
    rig.control(Events::ReplayRecordStop);
    CHECK(liveHits == 2);

    std::vector<std::string> seen;
    std::vector<std::int64_t> positions;
    rig.replay.events().subscribeAll([&](const Event& event) {
        seen.push_back(event.type);
        positions.push_back(rig.replay.playbackPosition());
        if (event.type == "Hit" && event.getOr<std::int64_t>("n", 0) == 1) {
            CHECK(event.origin == 7);
        }
    });

    rig.control(Events::ReplayPlayStart);
    rig.replay.update();
    // Raised before the first frame, so delivered at the start; no controls.
    CHECK((seen == std::vector<std::string>{"Early"}));
    rig.advance(99);
    rig.replay.update();
    CHECK(seen.size() == 1);
    rig.advance(1);
    rig.replay.update();
    CHECK(seen.size() == 2 && positions.back() == 100);
    rig.advance(100);
    rig.replay.update();
    CHECK(seen.size() == 2);
    rig.advance(50);
    rig.replay.update();
    CHECK(seen.size() == 3 && positions.back() == 250);
    CHECK(liveHits == 2);
    rig.advance(50);
    rig.replay.update();
    rig.replay.update();
    rig.events.dispatch();
    CHECK(!rig.replay.isPlaying());
    CHECK(liveHits == 2);
}

void aPausedGameStaysPaused()
{
    Rig rig;
    rig.recordMovingBox(3);
    rig.game.pause();
    rig.control(Events::ReplayPlayStart);
    rig.advance(100);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 100);
    rig.control(Events::ReplayPlayStop);
    CHECK(!rig.replay.isPlaying());
    CHECK(rig.game.isPaused());
}

void rollingWindowKeepsTheLastStretch()
{
    Rig rig;
    rig.replay.setRollingWindow(300);
    rig.control(Events::ReplayRecordStart);
    for (int i = 0; i <= 10; ++i) {
        rig.events.raise(Event("Tick").set("i", i));
        rig.events.dispatch();
        rig.box->get<Transform>()->x = 10.0F * static_cast<float>(i);
        rig.replay.capture(rig.world);
        rig.advance(100);
    }
    rig.control(Events::ReplayRecordStop);
    CHECK(rig.replay.frameCount() == 4);
    CHECK(rig.replay.recordedDuration() == 300);

    std::vector<std::int64_t> ticks;
    rig.replay.events().subscribe("Tick", [&](const Event& event) { ticks.push_back(event.get<std::int64_t>("i")); });
    rig.control(Events::ReplayPlayStart);
    rig.replay.update();
    auto shown = rig.shown();
    CHECK(shown.size() == 1 && near(shown[0]->get<Transform>()->x, 70.0F));
    CHECK((ticks == std::vector<std::int64_t>{7}));
}

// Tags of objects that left the window are dropped, but an object seen only
// in the oldest kept frame keeps its tag.
void rollingWindowKeepsTagsOfKeptObjects()
{
    Rig rig;
    rig.replay.setRollingWindow(300);
    GameObject& coin = rig.world.create("coin");
    coin.add<Transform>(50.0F, 50.0F, 4.0F, 4.0F);
    coin.add<Renderable>(Color{9, 9, 9});
    coin.setActive(true);
    GameObject& gem = rig.world.create("gem");
    gem.add<Transform>(80.0F, 80.0F, 4.0F, 4.0F);
    gem.add<Renderable>(Color{8, 8, 8});

    rig.control(Events::ReplayRecordStart);
    for (int i = 0; i <= 10; ++i) {
        coin.setActive(i < 3);
        gem.setActive(i == 7);
        rig.replay.capture(rig.world);
        rig.advance(100);
    }
    rig.control(Events::ReplayRecordStop);
    CHECK(rig.replay.frameCount() == 4);

    rig.control(Events::ReplayPlayStart);
    const auto shown = rig.shown();
    CHECK(shown.size() == 2 && shown[0]->tag() == "box" && shown[1]->tag() == "gem");
}

void objectsAppearAndDisappear()
{
    Rig rig;
    rig.control(Events::ReplayRecordStart);
    rig.replay.capture(rig.world);
    rig.advance(100);
    GameObject& coin = rig.world.create("coin");
    coin.add<Transform>(50.0F, 50.0F, 4.0F, 4.0F);
    coin.add<Renderable>(Color{9, 9, 9});
    coin.setActive(true);
    rig.replay.capture(rig.world);
    rig.advance(100);
    rig.box->setActive(false);
    rig.world.destroy(coin.id());
    rig.world.flushDestroyed();
    rig.replay.capture(rig.world);
    rig.advance(100);
    rig.box->setActive(true);
    rig.replay.capture(rig.world);
    rig.control(Events::ReplayRecordStop);

    rig.control(Events::ReplayPlayStart);
    CHECK(rig.shown().size() == 1);
    rig.advance(150);
    rig.replay.update();
    auto shown = rig.shown();
    CHECK(shown.size() == 2 && shown[1]->tag() == "coin");
    rig.advance(100);
    rig.replay.update();
    CHECK(rig.shown().empty());
    rig.advance(50);
    rig.replay.update();
    shown = rig.shown();
    // The box comes back in its original place, first in the draw order.
    CHECK(shown.size() == 1 && shown[0]->tag() == "box" && shown[0] == rig.replay.world().objects()[0].get());
}

void controlRules()
{
    Rig rig;

    // Nothing recorded: playing does nothing.
    rig.control(Events::ReplayPlayStart);
    CHECK(!rig.replay.isPlaying());
    CHECK(!rig.game.isPaused());

    // A frame at the same game time replaces the previous one.
    rig.control(Events::ReplayRecordStart);
    rig.replay.capture(rig.world);
    rig.box->get<Transform>()->x = 3.0F;
    rig.replay.capture(rig.world);
    CHECK(rig.replay.frameCount() == 1);
    rig.advance(100);
    rig.replay.capture(rig.world);
    rig.control(Events::ReplayRecordStart);
    CHECK(rig.replay.frameCount() == 2);

    // Playing stops the recording; recording is ignored during playback.
    rig.control(Events::ReplayPlayStart);
    CHECK(rig.replay.isPlaying() && !rig.replay.isRecording());
    rig.control(Events::ReplayRecordStart);
    CHECK(!rig.replay.isRecording());
    rig.replay.capture(rig.world);
    CHECK(rig.replay.frameCount() == 2);
    auto shown = rig.shown();
    CHECK(shown.size() == 1 && near(shown[0]->get<Transform>()->x, 3.0F));

    // Playing again starts over.
    rig.advance(60);
    rig.replay.update();
    CHECK(rig.replay.playbackPosition() == 60);
    rig.control(Events::ReplayPlayStart);
    CHECK(rig.replay.playbackPosition() == 0);
    CHECK(rig.replay.world().objects().size() == 1);
    rig.control(Events::ReplayPlayStop);
    CHECK(!rig.game.isPaused());

    // A new recording replaces the old one.
    rig.control(Events::ReplayRecordStart);
    CHECK(rig.replay.isRecording() && rig.replay.frameCount() == 0);
}

// The engine's order is dispatch, update, dispatch, render. Counts the renders
// that show the given x while playing.
int rendersShowing(Rig& rig, float x, int frames, std::int64_t ticsPerFrame)
{
    int shown = 0;
    for (int i = 0; i < frames; ++i) {
        rig.events.dispatch();
        rig.advance(ticsPerFrame);
        rig.replay.update();
        rig.events.dispatch();
        if (rig.replay.isPlaying()) {
            const auto objects = rig.shown();
            shown += objects.size() == 1 && near(objects[0]->get<Transform>()->x, x) ? 1 : 0;
        }
    }
    return shown;
}

void theLastFrameIsRendered()
{
    // A one-frame recording started before the update, as an input handler does.
    Rig single;
    single.recordMovingBox(1);
    single.events.raise(Event(Events::ReplayPlayStart));
    CHECK(rendersShowing(single, 0.0F, 4, 10) >= 1);
    CHECK(!single.replay.isPlaying());
    CHECK(!single.game.isPaused());

    // A longer recording ends on its exact last position.
    Rig longer;
    longer.recordMovingBox(11);
    longer.events.raise(Event(Events::ReplayPlayStart));
    CHECK(rendersShowing(longer, 100.0F, 40, 30) >= 1);
    CHECK(!longer.replay.isPlaying());

    // Paused on the last frame, playback holds it until unpaused.
    Rig paused;
    paused.recordMovingBox(2);
    paused.control(Events::ReplayPlayStart);
    paused.advance(100);
    paused.replay.update();
    paused.control(Events::ReplayPause);
    paused.replay.update();
    paused.events.dispatch();
    CHECK(paused.replay.isPlaying());
    paused.control(Events::ReplayPause);
    paused.replay.update();
    paused.events.dispatch();
    CHECK(!paused.replay.isPlaying());
}

// Delayed effects from a replay handler end with the playback that raised them.
void delayedReplayEffectsEndWithTheirPlayback()
{
    for (const bool restart : {false, true}) {
        Rig rig;
        rig.control(Events::ReplayRecordStart);
        rig.replay.capture(rig.world);
        rig.advance(100);
        rig.events.raise(Event("Hit"));
        rig.events.dispatch();
        rig.replay.capture(rig.world);
        rig.advance(2000);
        rig.replay.capture(rig.world);
        rig.control(Events::ReplayRecordStop);

        int late = 0;
        EventManager& replayEvents = rig.replay.events();
        bool raised = false;
        replayEvents.subscribe("Hit", [&](const Event&) {
            if (!raised) {
                raised = true;
                replayEvents.raise(Event("Late"), 1000);
            }
        });
        replayEvents.subscribe("Late", [&](const Event&) { ++late; });

        rig.control(Events::ReplayPlayStart);
        rig.advance(100);
        rig.replay.update();
        CHECK(replayEvents.pending() == 1);
        if (!restart) {
            rig.control(Events::ReplayPlayStop);
            CHECK(replayEvents.pending() == 0);
        }
        rig.control(Events::ReplayPlayStart);
        CHECK(replayEvents.pending() == 0);
        rig.advance(1500);
        rig.replay.update();
        CHECK(late == 0);
    }
}

// A restart raised in the frame playback ended must not be undone by the stop.
void aStaleStopDoesNotEndARestart()
{
    Rig rig;
    rig.recordMovingBox(3);
    rig.control(Events::ReplayPlayStart);
    rig.advance(1000);
    rig.replay.update();
    rig.events.raise(Event(Events::ReplayPlayStart));
    rig.replay.update();
    rig.events.dispatch();
    CHECK(rig.replay.isPlaying());
    CHECK(rig.replay.playbackPosition() == 0);

    // A stop from the game, which names no run, always works.
    rig.control(Events::ReplayPlayStop);
    CHECK(!rig.replay.isPlaying());
}

// A full live queue must not leave playback running forever.
void playbackEndsWithAFullLiveQueue()
{
    Rig rig;
    rig.recordMovingBox(2);
    rig.control(Events::ReplayPlayStart);
    rig.events.setMaxPending(1);
    rig.events.raise(Event("Filler"));
    rig.advance(100);
    rig.replay.update();
    rig.replay.update();
    rig.events.dispatch();
    CHECK(!rig.replay.isPlaying());
}

void destroyingDuringPlaybackRestoresTheGame()
{
    ManualClock real;
    Timeline game(real, 1000);
    EventManager events(game);
    World world;
    GameObject& box = world.create("box");
    box.add<Transform>(0.0F, 0.0F, 1.0F, 1.0F);
    box.add<Renderable>();
    box.setActive(true);
    {
        ReplaySystem replay(events, game, real, 1000);
        events.raise(Event(Events::ReplayRecordStart));
        events.dispatch();
        replay.capture(world);
        events.raise(Event(Events::ReplayPlayStart));
        events.dispatch();
        CHECK(game.isPaused());
    }
    CHECK(!game.isPaused());
    // Its subscriptions went with it.
    events.raise(Event(Events::ReplayPlayStart));
    events.dispatch();
    CHECK(!game.isPaused());
}

} // namespace

int main()
{
    playbackFollowsTheRecording();
    frameStateComesBackInOrder();
    speedAndPauseControlPlayback();
    recordedEventsReachOnlyTheReplayManager();
    aPausedGameStaysPaused();
    rollingWindowKeepsTheLastStretch();
    rollingWindowKeepsTagsOfKeptObjects();
    objectsAppearAndDisappear();
    controlRules();
    theLastFrameIsRendered();
    delayedReplayEffectsEndWithTheirPlayback();
    aStaleStopDoesNotEndARestart();
    playbackEndsWithAFullLiveQueue();
    destroyingDuringPlaybackRestoresTheGame();

    if (failures != 0) {
        std::cerr << failures << " replay check(s) failed\n";
        return 1;
    }
    std::cout << "Replay tests passed\n";
    return 0;
}
