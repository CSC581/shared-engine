// Cost of World::updateComponents() for different component combinations.
//
//   X  objects 100/1000/10000
//      mix  transform  Transform only (walk overhead, no update work)
//           motion     Transform + Motion
//           gravity    Transform + Motion + Gravity
//           path       Transform + Motion + PathMover (linear ping-pong)
//           behavior   Transform + Motion + Behavior (small std::function)
//   Y  update time per frame (mean, p95, p99 microseconds), nanoseconds per
//      object, and the one-off cost of the first update after the objects
//      were created, when the World rebuilds and sorts its update order
//
// No colliders, so the collision pass is not part of this measurement.

#include "BenchmarkSupport.hpp"
#include "Components.hpp"
#include "World.hpp"

#include <string>
#include <vector>

namespace {

void populate(World& world, int count, const std::string& mix)
{
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(i % 100) * 10.0F;
        const float y = static_cast<float>(i / 100) * 10.0F;
        GameObject& object = world.create("object");
        object.add<Transform>(x, y, 8.0F, 8.0F);
        if (mix == "motion" || mix == "gravity" || mix == "behavior") {
            object.add<Motion>(30.0F, 0.0F);
        }
        if (mix == "gravity") {
            object.add<Gravity>();
        }
        if (mix == "path") {
            object.add<PathMover>(PathMover::Linear{x, y, x + 200.0F, y}, 80.0F);
        }
        if (mix == "behavior") {
            Motion* motion = object.get<Motion>();
            object.add<Behavior>([motion](GameObject&, const FrameTime& time) {
                motion->velocityY += static_cast<float>(time.dtSeconds);
            });
        }
        object.setActive(true);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const Bench::Options options = Bench::parseOptions(
        argc, argv, "component-update-benchmark", "World::updateComponents() cost per component mix");

    const std::vector<int> counts = options.quick ? std::vector<int>{100, 1000}
                                                  : std::vector<int>{100, 1000, 10000};
    const std::vector<std::string> mixes{"transform", "motion", "gravity", "path", "behavior"};
    const int repetitions = options.quick ? 1 : 5;
    const int warmupFrames = options.quick ? 5 : 60;
    const int measuredFrames = options.quick ? 30 : 600;
    constexpr double dt = 1.0 / 60.0;

    Bench::Results results({"mix", "objects"},
                           {"update_mean_us", "update_p95_us", "update_p99_us", "ns_per_object", "first_frame_us"});

    for (int rep = 1; rep <= repetitions; ++rep) {
        for (int count : counts) {
            for (const std::string& mix : mixes) {
                World world;
                populate(world, count, mix);

                FrameTime time{dt, 0};
                std::vector<double> frameNs;
                frameNs.reserve(static_cast<std::size_t>(measuredFrames));
                double firstFrameNs = 0.0;
                for (int frame = 0; frame < warmupFrames + measuredFrames; ++frame) {
                    time.gameTimeUs += static_cast<std::int64_t>(dt * 1e6);
                    const std::int64_t start = Bench::nowNs();
                    world.updateComponents(time);
                    const auto elapsed = static_cast<double>(Bench::nowNs() - start);
                    if (frame == 0) {
                        firstFrameNs = elapsed;
                    } else if (frame >= warmupFrames) {
                        frameNs.push_back(elapsed);
                    }
                }

                std::vector<double> metrics = Bench::frameStatsUs(frameNs);
                metrics.push_back(Bench::mean(frameNs) / count);
                metrics.push_back(firstFrameNs / 1e3);
                results.add({mix, std::to_string(count)}, rep, metrics);
            }
        }
        std::cout << "repetition " << rep << '/' << repetitions << " done\n";
    }

    results.write(options);
    results.print();
    return 0;
}
