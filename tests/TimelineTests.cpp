// Tests for the time module and the way the engine is built on it.
//
// Three groups, in one binary:
//
//   1-14  Timeline and DeltaTimer semantics. Every case steps a ManualClock by
//         hand, so nothing sleeps and nothing depends on how fast the machine
//         runs -- the same sequence gives the same answers everywhere.
//   15    The one case that does use a real clock and real threads: several
//         readers hammering a timeline while another pauses and rescales it.
//   16-19 The engine integration. Entity and Game each gained a FrameTime
//         overload beside the float one they already had, rather than having
//         the float one replaced, so that every game written against the old
//         signature keeps working untouched. That forwarding can rot silently
//         -- a name-hiding mistake would compile and simply stop delivering
//         time -- so it is pinned here, along with the three engine
//         requirements stated the way the main loop states them.
//
// Nothing here opens a window or calls SDL_Init: the last group drives the
// objects the way the loop does, without the loop.
#include "DeltaTimer.hpp"
#include "Engine.hpp"
#include "Entity.hpp"
#include "FrameTime.hpp"
#include "Game.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

    // Touched only by the main thread: the one case that runs threads counts
    // its own failures atomically and reports them back here when it joins.
    int failures = 0;

    void check(bool condition, const char *expression, int line)
    {
        if (!condition)
        {
            std::cerr << "Test failed at line " << line << ": " << expression << '\n';
            ++failures;
        }
    }

    // Tic counts are exact integers, so only the two floating-point boundaries
    // need a tolerance: scale(), which is a ratio of tic sizes, and positions,
    // which are floats accumulated over several frames.
    bool nearlyEqual(double actual, double expected)
    {
        return std::fabs(actual - expected) < 1e-9;
    }

    bool nearlyEqual(float actual, float expected)
    {
        return std::fabs(actual - expected) < 0.001F;
    }

#define CHECK(condition) check((condition), #condition, __LINE__)

// Every documented validation failure is the same exception type, so one macro
// covers all of them.
#define CHECK_THROWS(expression)                                                      \
    do                                                                                \
    {                                                                                 \
        bool threw = false;                                                           \
        try                                                                           \
        {                                                                             \
            expression;                                                               \
        }                                                                             \
        catch (const std::invalid_argument &)                                         \
        {                                                                             \
            threw = true;                                                             \
        }                                                                             \
        check(threw, #expression " should throw std::invalid_argument", __LINE__);    \
    } while (false)

    // ---- Timeline and DeltaTimer semantics, on a hand-driven clock. ----

    // 1. A timeline counts whole tics of its anchor and nothing else.
    void basicTicCounting()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(25);
        CHECK(timeline.now() == 2);

        clock.advance(5);
        CHECK(timeline.now() == 3);
    }

    // 2. Speeding time up must not teleport whatever is reading it.
    void scaleDoesNotJump()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(100);
        CHECK(timeline.now() == 10);

        timeline.setScale(2.0);
        CHECK(timeline.ticSize() == 5);
        CHECK(timeline.now() == 10);

        clock.advance(10);
        CHECK(timeline.now() == 12);
    }

    // 3. The same guarantee when the tic size is set directly.
    void ticSizeDoesNotJump()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(100);
        timeline.setTicSize(20);
        CHECK(timeline.now() == 10);

        clock.advance(40);
        CHECK(timeline.now() == 12);
    }

    // 4. A pause freezes the reading however long the anchor runs on.
    void pauseFreezesTime()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(30);
        CHECK(timeline.now() == 3);

        timeline.pause();
        clock.advance(1000);
        CHECK(timeline.now() == 3);

        timeline.unpause();
        clock.advance(10);
        CHECK(timeline.now() == 4);
    }

    // 5. The partial tic in flight survives the pause. Without the carry the
    //    five anchor tics banked before pausing would be silently lost and the
    //    second tic would land late.
    void remainderSurvivesPause()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(15);
        CHECK(timeline.now() == 1);

        timeline.pause();
        clock.advance(100);
        timeline.unpause();

        clock.advance(5);
        CHECK(timeline.now() == 2);
    }

    // 6. The same carry, this time across a rate change rather than a pause.
    void remainderSurvivesRebase()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(15);
        timeline.setTicSize(10);
        CHECK(timeline.now() == 1);

        clock.advance(5);
        CHECK(timeline.now() == 2);
    }

    // 7. Pausing a paused timeline is a no-op, so a key held down or two
    //    subsystems both pausing cannot corrupt the carried remainder.
    void pauseIsIdempotent()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(20);
        timeline.pause();
        timeline.pause();

        clock.advance(50);
        timeline.unpause();
        timeline.unpause();

        clock.advance(10);
        CHECK(timeline.now() == 3);
    }

    // 8. A rate change while paused takes effect on resume and not before.
    void ticSizeChangeWhilePaused()
    {
        ManualClock clock;
        Timeline timeline(clock, 10);

        clock.advance(20);
        timeline.pause();
        timeline.setTicSize(5);

        clock.advance(100);
        CHECK(timeline.now() == 2);

        timeline.unpause();
        clock.advance(10);
        CHECK(timeline.now() == 4);
    }

    // 9. A timeline anchored to another inherits its pauses for free: the
    //    child needs no notification and holds no reference to the pause state.
    void childFollowsParent()
    {
        ManualClock clock;
        Timeline parent(clock, 10);
        Timeline child(parent, 2);

        clock.advance(100);
        CHECK(parent.now() == 10);
        CHECK(child.now() == 5);

        parent.pause();
        clock.advance(100);
        CHECK(parent.now() == 10);
        CHECK(child.now() == 5);

        parent.unpause();
        clock.advance(20);
        CHECK(parent.now() == 12);
        CHECK(child.now() == 6);
    }

    // 10. Scale is measured against the tic size the timeline was built with,
    //     so it never compounds on the last value set.
    void scaleRoundTrip()
    {
        ManualClock clock;
        Timeline timeline(clock, 1000);

        timeline.setScale(0.5);
        CHECK(timeline.ticSize() == 2000);
        CHECK(nearlyEqual(timeline.scale(), 0.5));

        timeline.setScale(2.0);
        CHECK(timeline.ticSize() == 500);
        CHECK(nearlyEqual(timeline.scale(), 2.0));
    }

    // 11. A rate that would stop or reverse time is refused outright.
    void validationRejectsBadRates()
    {
        ManualClock clock;

        CHECK_THROWS(Timeline(clock, 0));
        CHECK_THROWS(Timeline(clock, -1));

        Timeline timeline(clock, 10);

        CHECK_THROWS(timeline.setTicSize(0));
        CHECK_THROWS(timeline.setTicSize(-5));

        CHECK_THROWS(timeline.setScale(0.0));
        CHECK_THROWS(timeline.setScale(-1.0));
        CHECK_THROWS(timeline.setScale(std::nan("")));

        // A rejected call leaves the timeline untouched.
        CHECK(timeline.ticSize() == 10);
    }

    // 12. The per-consumer half: deltas, the stall clamp, and what a paused
    //     source looks like to something asking for elapsed time.
    void deltaTimerClampsAndFollowsPause()
    {
        ManualClock clock;
        Timeline timeline(clock, 1);
        DeltaTimer timer(timeline, 100);

        clock.advance(30);
        CHECK(timer.tick() == 30);
        CHECK(!timer.lastWasClamped());

        clock.advance(500);
        CHECK(timer.tick() == 100);
        CHECK(timer.lastWasClamped());

        // The 400 tics the clamp dropped are gone, not paid back here.
        clock.advance(10);
        CHECK(timer.tick() == 10);
        CHECK(!timer.lastWasClamped());

        timeline.pause();
        clock.advance(1000);
        CHECK(timer.tick() == 0);

        CHECK(timer.maxDelta() == 100);
        CHECK_THROWS(DeltaTimer(timeline, 0));
        CHECK_THROWS(timer.setMaxDelta(-1));
    }

    // 13. Two consumers of one timeline do not steal each other's elapsed
    //     time: that is the whole reason "time since I last looked" lives in a
    //     DeltaTimer rather than in the Timeline.
    void deltaTimersAreIndependent()
    {
        ManualClock clock;
        Timeline timeline(clock, 1);

        DeltaTimer fast(timeline, 1000);
        DeltaTimer slow(timeline, 1000);

        clock.advance(30);
        CHECK(fast.tick() == 30);

        clock.advance(30);
        CHECK(fast.tick() == 30);

        // The second consumer has not looked yet, so it is owed all 60.
        CHECK(slow.tick() == 60);
    }

    // 14. The third scale: a timeline whose anchor counts loop iterations
    //     rather than elapsed time. Nothing about Timeline changes -- pause,
    //     tic size and scale mean exactly what they mean on a clock of
    //     seconds, because a tic was never a second in the first place.
    void loopIterationTimeline()
    {
        FrameClock loop;
        Timeline frames(loop, 1);

        for (int iteration = 0; iteration < 10; ++iteration)
        {
            loop.advance();
        }
        CHECK(frames.now() == 10);

        // One tic every second iteration: half rate, and no jump at the change.
        frames.setTicSize(2);
        CHECK(frames.now() == 10);

        for (int iteration = 0; iteration < 6; ++iteration)
        {
            loop.advance();
        }
        CHECK(frames.now() == 13);

        // A paused loop timeline stops counting frames, however many go by.
        frames.pause();
        for (int iteration = 0; iteration < 100; ++iteration)
        {
            loop.advance();
        }
        CHECK(frames.now() == 13);

        frames.unpause();
        for (int iteration = 0; iteration < 4; ++iteration)
        {
            loop.advance();
        }
        CHECK(frames.now() == 15);

        // And a child of it inherits all of that, exactly as on any other
        // anchor: two loop tics per child tic here.
        Timeline everyOther(frames, 2);
        CHECK(everyOther.now() == 0);

        for (int iteration = 0; iteration < 8; ++iteration)
        {
            loop.advance();
        }
        CHECK(frames.now() == 19);
        CHECK(everyOther.now() == 2);
    }

    // 15. A rate change must never advance the clock by itself, whether the
    //     timeline is running or paused when it happens.
    //
    //     The partial tic in flight is a fraction of a tic, not a count of
    //     anchor units: banked half way through a slow tic, it is still half
    //     way through a fast one. Carrying it across as raw anchor units and
    //     reinterpreting it against the new tic size is what makes a timeline
    //     lurch forward the instant its rate changes, with no time having
    //     passed at all -- a remainder of 5 is most of a tic at a tic size of
    //     10 and a whole one at a tic size of 5.
    void rateChangeNeverJumpsTheClock()
    {
        // Paused half way through a tic, then sped up.
        {
            ManualClock clock;
            Timeline timeline(clock, 10);

            clock.advance(15);  // one whole tic, and half of the next
            CHECK(timeline.now() == 1);

            timeline.pause();
            CHECK(timeline.now() == 1);

            timeline.setTicSize(5);  // twice as fast
            CHECK(timeline.now() == 1);

            timeline.unpause();
            CHECK(timeline.now() == 1);  // no anchor time passed, so no tic

            // The half tic in flight is still half a tic, so it completes
            // after half of the new, shorter one.
            clock.advance(3);
            CHECK(timeline.now() == 2);
        }

        // The same rate change with the timeline running, which goes through
        // the same carry and has always had the same flaw.
        {
            ManualClock clock;
            Timeline timeline(clock, 10);

            clock.advance(15);
            CHECK(timeline.now() == 1);

            timeline.setTicSize(5);
            CHECK(timeline.now() == 1);

            clock.advance(3);
            CHECK(timeline.now() == 2);
        }

        // Slowing down mid-tic is the same story in the other direction: half
        // way through a tic of 100 leaves 50 anchor units to go, not 5.
        {
            ManualClock clock;
            Timeline timeline(clock, 10);

            clock.advance(15);
            timeline.pause();
            timeline.setTicSize(100);
            timeline.unpause();
            CHECK(timeline.now() == 1);

            clock.advance(49);
            CHECK(timeline.now() == 1);

            clock.advance(1);
            CHECK(timeline.now() == 2);
        }

        // And through setScale, which is how a game actually changes rate.
        // 999 carried anchor units would be worth three whole tics at the
        // 250 that 4.0x asks for.
        {
            ManualClock clock;
            Timeline timeline(clock, 1000);

            clock.advance(1999);  // one tic, and 999/1000 of the next
            CHECK(timeline.now() == 1);

            timeline.pause();
            timeline.setScale(4.0);
            timeline.unpause();
            CHECK(timeline.now() == 1);
        }
    }

    // ---- The one case with real threads and a real clock. ----

    constexpr int readerCount = 4;
    constexpr int readsPerReader = 200'000;
    constexpr int writerIterations = 10'000;

    // Readers cannot call check(): several of them would interleave on cerr.
    // They count instead, and the main thread reports once they have joined.
    std::atomic<int> readersThatSawTimeGoBackwards{0};

    void readUntilDone(const Timeline &timeline)
    {
        std::int64_t previous = timeline.now();

        for (int read = 0; read < readsPerReader; ++read)
        {
            const std::int64_t current = timeline.now();

            if (current < previous)
            {
                ++readersThatSawTimeGoBackwards;
                return;
            }

            previous = current;
        }
    }

    void churn(Timeline &timeline)
    {
        constexpr double scales[] = {0.5, 1.0, 2.0};

        for (int iteration = 0; iteration < writerIterations; ++iteration)
        {
            timeline.togglePause();
            timeline.setScale(scales[iteration % 3]);
        }
    }

    // 16. The design has to survive a multithreaded update loop later, so
    //     readers on other threads must never see time run backwards while
    //     another thread pauses and rescales it underneath them.
    void staysMonotonicUnderConcurrentWriters()
    {
        RealTimeClock clock;
        Timeline timeline(clock, kNsPerUs);

        std::vector<std::thread> threads;
        threads.reserve(readerCount + 1);

        for (int reader = 0; reader < readerCount; ++reader)
        {
            threads.emplace_back([&timeline] { readUntilDone(timeline); });
        }

        threads.emplace_back([&timeline] { churn(timeline); });

        for (std::thread &thread : threads)
        {
            thread.join();
        }

        CHECK(readersThatSawTimeGoBackwards.load() == 0);
    }

    // ---- The engine built on top: FrameTime, Entity and Game. ----

    // Game::update takes an Engine& that none of the games below ever touch:
    // what is under test is the loop's dispatch, not anything Engine does.
    // Standing in for one with raw storage keeps this test headless, where
    // constructing a real Engine would call SDL_Init and open a window.
    alignas(Engine) unsigned char engineStorage[sizeof(Engine)];

    Engine &stubEngine()
    {
        return *reinterpret_cast<Engine *>(engineStorage);
    }

    // A game written before the timeline existed: it overrides the float
    // signature and knows nothing about FrameTime.
    class LegacyGame : public Game
    {
    public:
        void handleInput(Engine &) override {}
        void render(SDL_Renderer *) const override {}

        void update(float deltaTime, Engine &) override
        {
            lastDeltaTime = deltaTime;
            ++updateCount;
        }

        float lastDeltaTime = -1.0F;
        int updateCount = 0;
    };

    // A game that wants the whole frame: absolute game time as well as the
    // delta, so its motion can be a function of time rather than a sum of
    // deltas.
    class TimeAwareGame : public Game
    {
    public:
        void handleInput(Engine &) override {}
        void render(SDL_Renderer *) const override {}

        // Kept so the class still satisfies the old pure virtual; the engine
        // never reaches it once the FrameTime overload is present.
        void update(float, Engine &) override { ++secondsOnlyCount; }

        void update(const FrameTime &time, Engine &) override
        {
            lastGameTimeUs = time.gameTimeUs;
            ++frameTimeCount;
        }

        std::int64_t lastGameTimeUs = -1;
        int frameTimeCount = 0;
        int secondsOnlyCount = 0;
    };

    // 17. The engine hands every game a FrameTime; a game that only implements
    //     the old signature must still be driven by it.
    void legacyGameStillReceivesDelta()
    {
        LegacyGame game;
        Game &asInterface = game;

        const FrameTime frame{0.25, 1'000'000};
        asInterface.update(frame, stubEngine());

        CHECK(game.updateCount == 1);
        CHECK(nearlyEqual(game.lastDeltaTime, 0.25F));
    }

    // 18. A game that does override the FrameTime version gets it instead, and
    //     the forwarding to the float version does not also fire.
    void timeAwareGameReceivesAbsoluteTime()
    {
        TimeAwareGame game;
        Game &asInterface = game;

        const FrameTime frame{0.016, 12'345'678};
        asInterface.update(frame, stubEngine());

        CHECK(game.frameTimeCount == 1);
        CHECK(game.secondsOnlyCount == 0);
        CHECK(game.lastGameTimeUs == 12'345'678);
    }

    // 19. Both Entity overloads describe the same move, so handing an entity
    //     the frame instead of a float changes nothing about where it ends up.
    void entityOverloadsAgree()
    {
        Entity viaSeconds(0.0F, 0.0F, 10.0F, 10.0F);
        Entity viaFrame(0.0F, 0.0F, 10.0F, 10.0F);

        viaSeconds.setVelocity(100.0F, -50.0F);
        viaFrame.setVelocity(100.0F, -50.0F);

        const FrameTime frame{0.5, 500'000};
        viaSeconds.update(0.5F);
        viaFrame.update(frame);

        CHECK(nearlyEqual(viaFrame.getX(), viaSeconds.getX()));
        CHECK(nearlyEqual(viaFrame.getY(), viaSeconds.getY()));
        CHECK(nearlyEqual(viaFrame.getX(), 50.0F));
    }

    // 20. The three engine requirements, stated the way the loop states them:
    //     build a FrameTime out of a game timeline exactly as Engine::run does,
    //     and drive a real Entity through it on a clock the test controls.
    //
    //     Testing the timeline and the entity separately does not actually show
    //     any of this: what is being checked here is the composition.
    void gameTimelineDrivesPausesAndScalesAnEntity()
    {
        ManualClock clock;
        Timeline gameTime(clock, kNsPerUs);  // 1 tic = 1 game microsecond
        DeltaTimer frameTimer(gameTime, 50'000);

        Entity mover(0.0F, 0.0F, 10.0F, 10.0F);
        mover.setVelocity(100.0F, 0.0F);  // 100 px per game second

        // One pass of the main loop: real time passes, the loop asks the game
        // timeline how much of that counted, and the entity moves by it.
        const auto frame = [&](std::int64_t realNs) {
            clock.advance(realNs);
            const std::int64_t tics = frameTimer.tick();
            mover.update(FrameTime{static_cast<double>(tics) / 1'000'000.0, gameTime.now()});
        };

        // Requirement 1: position comes from elapsed time on the timeline.
        // 100 px/s for 10 ms is 1 px.
        frame(10 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), 1.0F));

        // Requirement 2: paused, the entity is stationary however much real
        // time passes.
        gameTime.pause();
        const float frozenX = mover.getX();

        frame(500 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), frozenX));
        frame(500 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), frozenX));

        // ...and on unpause it carries on from exactly where it stopped: the
        // second of real time that went by is not paid back as a lurch.
        gameTime.unpause();
        frame(10 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), frozenX + 1.0F));

        // Requirement 3: scale changes the distance covered per real
        // millisecond, and the entity is never touched to make it happen.
        gameTime.setScale(2.0);
        const float beforeDouble = mover.getX();
        frame(10 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), beforeDouble + 2.0F));

        gameTime.setScale(0.5);
        const float beforeHalf = mover.getX();
        frame(10 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), beforeHalf + 0.5F));

        gameTime.setScale(1.0);
        const float beforeNormal = mover.getX();
        frame(10 * kNsPerMs);
        CHECK(nearlyEqual(mover.getX(), beforeNormal + 1.0F));

        // The whole point of requirement 3: nothing about the object changed.
        // Its velocity is the one it was given before any of this happened.
        CHECK(nearlyEqual(mover.getVelocityX(), 100.0F));
    }

} // namespace

int main()
{
    basicTicCounting();
    scaleDoesNotJump();
    ticSizeDoesNotJump();
    pauseFreezesTime();
    remainderSurvivesPause();
    remainderSurvivesRebase();
    pauseIsIdempotent();
    ticSizeChangeWhilePaused();
    childFollowsParent();
    scaleRoundTrip();
    validationRejectsBadRates();
    deltaTimerClampsAndFollowsPause();
    deltaTimersAreIndependent();
    loopIterationTimeline();
    rateChangeNeverJumpsTheClock();

    legacyGameStillReceivesDelta();
    timeAwareGameReceivesAbsoluteTime();
    entityOverloadsAgree();
    gameTimelineDrivesPausesAndScalesAnEntity();

    // Last: the only case that takes measurable time to run.
    staysMonotonicUnderConcurrentWriters();

    if (failures == 0)
    {
        std::cout << "All timeline tests passed.\n";
        return 0;
    }

    std::cerr << failures << " timeline check(s) failed.\n";
    return 1;
}
