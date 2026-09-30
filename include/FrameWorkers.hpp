#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Splits a game's per-frame update across worker threads (Section 3).
//
// Each task gets one thread that lives for the whole run, as in the Moodle
// ThreadExample: creating and joining threads every frame would repeat that
// setup cost sixty times a second. Each frame, runFrame() hands every worker
// the frame's delta time, and returns only once all of them have finished it.
// That wait is a frame barrier: code after runFrame() can read everything the
// tasks wrote without racing them, and the next frame never starts on top of
// an unfinished one.
//
// What stays on the calling (main) thread is the caller's choice, but SDL
// input and drawing must: SDL's event and render calls are not safe from
// other threads. Tasks running at the same time must not write the same data;
// anything two tasks both need to change (for example a platform carrying the
// player) should be recorded by one and applied on the main thread after
// runFrame() returns.
//
// A task that throws does not take down its thread or leave runFrame()
// waiting forever: the exception is caught, the frame still completes, and
// runFrame() rethrows the first one on the main thread.
class FrameWorkers {
public:
    using Task = std::function<void(float deltaTime)>;

    // Starts one persistent thread per task. Throws std::invalid_argument for
    // an empty list or an empty task.
    explicit FrameWorkers(std::vector<Task> tasks);

    // Stops and joins every worker. Waits for a frame in progress to finish.
    ~FrameWorkers();

    FrameWorkers(const FrameWorkers&) = delete;
    FrameWorkers& operator=(const FrameWorkers&) = delete;

    // Runs every task once with `deltaTime`, in parallel, and blocks until all
    // have finished. Call from one thread only (the game loop).
    void runFrame(float deltaTime);

    std::size_t workerCount() const { return tasks_.size(); }

    // Frames completed so far; each worker ran exactly this many times.
    std::uint64_t framesRun() const;

private:
    void workerLoop(std::size_t index);

    std::vector<Task> tasks_;
    std::vector<std::thread> threads_;

    mutable std::mutex mutex_;
    std::condition_variable frameReady_;
    std::condition_variable frameDone_;
    bool quit_ = false;
    std::uint64_t frameId_ = 0;
    float frameDelta_ = 0.0F;
    std::size_t workersDone_ = 0;
    std::exception_ptr failure_;
};
