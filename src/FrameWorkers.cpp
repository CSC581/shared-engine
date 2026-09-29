#include "FrameWorkers.hpp"

#include <stdexcept>
#include <utility>

FrameWorkers::FrameWorkers(std::vector<Task> tasks) : tasks_(std::move(tasks))
{
    if (tasks_.empty()) {
        throw std::invalid_argument("FrameWorkers: needs at least one task");
    }
    for (const Task& task : tasks_) {
        if (!task) {
            throw std::invalid_argument("FrameWorkers: a task is empty");
        }
    }

    threads_.reserve(tasks_.size());
    try {
        for (std::size_t index = 0; index < tasks_.size(); ++index) {
            threads_.emplace_back([this, index] { workerLoop(index); });
        }
    } catch (...) {
        // A thread failed to start: stop the ones that did before rethrowing,
        // since a destructor does not run for a half-built object.
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        frameReady_.notify_all();
        for (std::thread& thread : threads_) {
            thread.join();
        }
        throw;
    }
}

FrameWorkers::~FrameWorkers()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    frameReady_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void FrameWorkers::runFrame(float deltaTime)
{
    std::unique_lock<std::mutex> lock(mutex_);
    frameDelta_ = deltaTime;
    workersDone_ = 0;
    failure_ = nullptr;
    ++frameId_;
    frameReady_.notify_all();

    frameDone_.wait(lock, [this] { return workersDone_ == tasks_.size(); });

    if (failure_) {
        std::exception_ptr failure = std::exchange(failure_, nullptr);
        lock.unlock();
        std::rethrow_exception(failure);
    }
}

std::uint64_t FrameWorkers::framesRun() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return frameId_;
}

void FrameWorkers::workerLoop(std::size_t index)
{
    std::uint64_t lastFrame = 0;
    while (true) {
        float deltaTime = 0.0F;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            frameReady_.wait(lock, [this, lastFrame] { return quit_ || frameId_ != lastFrame; });
            // A frame already handed out is always finished before quitting,
            // so a destructor that races a runFrame() never strands it.
            if (frameId_ == lastFrame) {
                return;
            }
            lastFrame = frameId_;
            deltaTime = frameDelta_;
        }

        std::exception_ptr failure;
        try {
            tasks_[index](deltaTime);
        } catch (...) {
            failure = std::current_exception();
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (failure && !failure_) {
                failure_ = failure;
            }
            ++workersDone_;
            if (workersDone_ == tasks_.size()) {
                frameDone_.notify_one();
            }
        }
    }
}
