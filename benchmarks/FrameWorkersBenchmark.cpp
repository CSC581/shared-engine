// Does splitting World::updateComponents() across FrameWorkers pay off?
//
//   X  workers   serial (no FrameWorkers, one updateComponents() call) or
//                1/2/4/8 persistent workers, each updating its own tag
//      objects   1000/10000, split evenly over the workers' tags
//      workload  light  Transform + Motion + Gravity (built-ins only)
//                heavy  + a Behavior doing some trigonometry per object,
//                       standing in for game logic such as AI
//   Y  frame time (mean, p95, p99 microseconds) and speedup over serial in
//      the same repetition
//
// Each frame is runFrame() plus nothing else, as a game would run its
// threaded update. Collision and destruction stay on the main thread and are
// left out because they do not change with the worker count.

#include "BenchmarkSupport.hpp"
#include "Components.hpp"
#include "FrameWorkers.hpp"
#include "World.hpp"

#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int heavyIterations = 48;

std::string tagFor(int index)
{
    return "worker" + std::to_string(index);
}

void populate(World& world, int count, int tags, bool heavy)
{
    for (int i = 0; i < count; ++i) {
        GameObject& object = world.create(tagFor(i % tags));
        Transform* transform = &object.add<Transform>(static_cast<float>(i % 100), 0.0F, 8.0F, 8.0F);
        Motion* motion = &object.add<Motion>(10.0F, 0.0F);
        object.add<Gravity>();
        if (heavy) {
            object.add<Behavior>([transform, motion](GameObject&, const FrameTime&) {
                float steer = 0.0F;
                for (int k = 0; k < heavyIterations; ++k) {
                    steer += std::sin(transform->x * 0.01F + static_cast<float>(k)) *
                             std::cos(transform->y * 0.01F - static_cast<float>(k));
                }
                motion->velocityX = 10.0F + steer * 1e-3F;
            });
        }
        object.setActive(true);
    }
}

// Mean, p95, p99 frame time in microseconds for one configuration.
std::vector<double> measure(int workers, int objects, bool heavy, int warmupFrames, int measuredFrames)
{
    constexpr double dt = 1.0 / 60.0;
    World world;
    populate(world, objects, workers == 0 ? 1 : workers, heavy);

    // Written by the main thread before runFrame(); its barrier publishes it
    // to the workers.
    FrameTime time{dt, 0};
    std::unique_ptr<FrameWorkers> pool;
    if (workers > 0) {
        std::vector<FrameWorkers::Task> tasks;
        for (int i = 0; i < workers; ++i) {
            const std::string tag = tagFor(i);
            tasks.emplace_back([&world, &time, tag](float) { world.updateComponents(time, tag); });
        }
        pool = std::make_unique<FrameWorkers>(std::move(tasks));
    }

    std::vector<double> frameNs;
    frameNs.reserve(static_cast<std::size_t>(measuredFrames));
    for (int frame = 0; frame < warmupFrames + measuredFrames; ++frame) {
        time.gameTimeUs += static_cast<std::int64_t>(dt * 1e6);
        const std::int64_t start = Bench::nowNs();
        if (pool) {
            pool->runFrame(static_cast<float>(dt));
        } else {
            world.updateComponents(time);
        }
        const auto elapsed = static_cast<double>(Bench::nowNs() - start);
        if (frame >= warmupFrames) {
            frameNs.push_back(elapsed);
        }
    }
    return Bench::frameStatsUs(frameNs);
}

} // namespace

int main(int argc, char** argv)
{
    const Bench::Options options = Bench::parseOptions(
        argc, argv, "frame-workers-benchmark", "World update time versus FrameWorkers thread count");

    const std::vector<int> workerCounts = options.quick ? std::vector<int>{0, 2} : std::vector<int>{0, 1, 2, 4, 8};
    const std::vector<int> objectCounts = options.quick ? std::vector<int>{1000} : std::vector<int>{1000, 10000};
    const std::vector<std::string> workloads{"light", "heavy"};
    const int repetitions = options.quick ? 1 : 5;
    const int warmupFrames = options.quick ? 5 : 60;
    const int measuredFrames = options.quick ? 30 : 600;

    std::cout << "hardware threads: " << std::thread::hardware_concurrency() << '\n';

    Bench::Results results({"workload", "objects", "workers"},
                           {"frame_mean_us", "frame_p95_us", "frame_p99_us", "speedup_vs_serial"});

    for (int rep = 1; rep <= repetitions; ++rep) {
        for (int objects : objectCounts) {
            for (const std::string& workload : workloads) {
                double serialMean = 0.0;
                for (int workers : workerCounts) {
                    std::vector<double> metrics =
                        measure(workers, objects, workload == "heavy", warmupFrames, measuredFrames);
                    if (workers == 0) {
                        serialMean = metrics[0];
                    }
                    metrics.push_back(serialMean / metrics[0]);
                    results.add({workload, std::to_string(objects), workers == 0 ? "serial" : std::to_string(workers)},
                                rep, metrics);
                }
            }
        }
        std::cout << "repetition " << rep << '/' << repetitions << " done\n";
    }

    results.write(options);
    results.print();
    return 0;
}
