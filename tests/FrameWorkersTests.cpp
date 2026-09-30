#include "FrameWorkers.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }
    return condition;
}

// Every task runs exactly once per frame, with that frame's delta, and on a
// thread other than the caller's.
bool testEachTaskRunsOncePerFrame()
{
    std::atomic<int> platformRuns{0};
    std::atomic<int> playerRuns{0};
    std::atomic<bool> ranOnCaller{false};
    std::atomic<bool> wrongDelta{false};
    const std::thread::id caller = std::this_thread::get_id();
    float expectedDelta = 0.0F;

    const auto check = [&](float dt) {
        if (std::this_thread::get_id() == caller) {
            ranOnCaller = true;
        }
        if (dt != expectedDelta) {
            wrongDelta = true;
        }
    };

    FrameWorkers workers({
        [&](float dt) { ++platformRuns; check(dt); },
        [&](float dt) { ++playerRuns; check(dt); },
    });

    for (int frame = 1; frame <= 100; ++frame) {
        expectedDelta = static_cast<float>(frame) / 1000.0F;
        workers.runFrame(expectedDelta);
    }

    bool passed = true;
    passed &= expect(workers.workerCount() == 2, "two tasks should get two workers");
    passed &= expect(platformRuns == 100 && playerRuns == 100,
                     "each task should run once per frame");
    passed &= expect(workers.framesRun() == 100, "framesRun should count frames");
    passed &= expect(!ranOnCaller, "tasks should run on worker threads, not the caller's");
    passed &= expect(!wrongDelta, "each task should get that frame's delta time");
    return passed;
}

// runFrame() must not return until every task is done, so what a slow task
// wrote is visible right after it.
bool testRunFrameWaitsForTheSlowestTask()
{
    int fastResult = 0;
    int slowResult = 0;
    FrameWorkers workers({
        [&](float) { fastResult = 1; },
        [&](float) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            slowResult = 1;
        },
    });

    workers.runFrame(0.016F);
    return expect(fastResult == 1 && slowResult == 1,
                  "runFrame should return only after every task has finished");
}

// Tasks really overlap: two tasks that each wait for the other can only both
// finish if they run at the same time.
bool testTasksRunInParallel()
{
    std::atomic<int> arrived{0};
    std::atomic<bool> timedOut{false};
    const auto meet = [&](float) {
        ++arrived;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (arrived.load() < 2) {
            if (std::chrono::steady_clock::now() > deadline) {
                timedOut = true;
                return;
            }
            std::this_thread::yield();
        }
    };

    FrameWorkers workers({meet, meet});
    workers.runFrame(0.016F);
    return expect(!timedOut, "the two tasks should run at the same time");
}

// A throwing task neither kills its worker nor hangs the frame: runFrame()
// rethrows on the caller, and the next frame runs normally.
bool testTaskExceptionReachesTheCaller()
{
    std::atomic<int> otherRuns{0};
    bool throwNow = true;
    FrameWorkers workers({
        [&](float) {
            if (throwNow) {
                throw std::runtime_error("task failed");
            }
        },
        [&](float) { ++otherRuns; },
    });

    bool passed = true;
    bool caught = false;
    try {
        workers.runFrame(0.016F);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    passed &= expect(caught, "a task's exception should be rethrown by runFrame");

    throwNow = false;
    workers.runFrame(0.016F);
    passed &= expect(otherRuns == 2, "workers should keep running after a task threw");
    return passed;
}

bool testRejectsBadTaskLists()
{
    bool passed = true;
    try {
        FrameWorkers none({});
        passed &= expect(false, "an empty task list should be rejected");
    } catch (const std::invalid_argument&) {
    }
    try {
        FrameWorkers empty({FrameWorkers::Task{}});
        passed &= expect(false, "an empty task should be rejected");
    } catch (const std::invalid_argument&) {
    }
    return passed;
}

// Destroying the workers joins every thread promptly, whether or not a frame
// was ever run.
bool testShutdownJoinsWorkers()
{
    const auto start = std::chrono::steady_clock::now();
    {
        FrameWorkers idle({[](float) {}, [](float) {}});
    }
    {
        FrameWorkers used({[](float) {}, [](float) {}});
        used.runFrame(0.016F);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return expect(elapsed < std::chrono::seconds(1), "shutdown should join workers promptly");
}

} // namespace

int main()
{
    bool passed = true;

    passed &= testEachTaskRunsOncePerFrame();
    passed &= testRunFrameWaitsForTheSlowestTask();
    passed &= testTasksRunInParallel();
    passed &= testTaskExceptionReachesTheCaller();
    passed &= testRejectsBadTaskLists();
    passed &= testShutdownJoinsWorkers();

    if (passed) {
        std::cout << "frame-workers-tests: all checks passed\n";
    }
    return passed ? 0 : 1;
}
