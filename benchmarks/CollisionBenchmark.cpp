// Cost of World::resolveCollisions() as the number of colliders grows.
//
//   X  colliders 10/100/500/1000/2000
//      scenario  static   solid boxes without Motion (level geometry)
//                moving   solid boxes with Motion, bouncing inside the arena
//                trigger  the same moving boxes as trigger colliders
//   Y  collision pass time per frame (mean, p95, p99 microseconds) and
//      onCollide callbacks per frame, which shows how many pairs overlapped
//
// The arena grows with the collider count so density stays the same, and the
// pass is O(n^2) over active colliders, so time should grow with the square of
// the count while callbacks grow linearly. Only resolveCollisions() is timed.
// The component update that moves the boxes runs untimed before it.

#include "BenchmarkSupport.hpp"
#include "Components.hpp"
#include "World.hpp"

#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr float boxSize = 24.0F;
// Arena area per collider. Boxes cover about 6% of the arena, so some overlap.
constexpr float areaPerCollider = 96.0F * 96.0F;
constexpr float maxSpeed = 120.0F;

struct Scene {
    float side = 0.0F;
    std::uint64_t callbacks = 0;
};

void populate(World& world, Scene& scene, int colliders, const std::string& scenario, std::mt19937& random)
{
    scene.side = std::sqrt(static_cast<float>(colliders) * areaPerCollider);
    std::uniform_real_distribution<float> position(0.0F, scene.side - boxSize);
    std::uniform_real_distribution<float> velocity(-maxSpeed, maxSpeed);
    const bool moving = scenario != "static";
    const auto kind = scenario == "trigger" ? Collider::Kind::Trigger : Collider::Kind::Solid;

    for (int i = 0; i < colliders; ++i) {
        GameObject& object = world.create("box");
        Transform* transform = &object.add<Transform>(position(random), position(random), boxSize, boxSize);
        if (moving) {
            Motion* motion = &object.add<Motion>(velocity(random), velocity(random));
            const float side = scene.side;
            // Keeps the box inside the arena so density stays constant.
            object.add<Behavior>([side, transform, motion](GameObject&, const FrameTime&) {
                if ((transform->x < 0.0F && motion->velocityX < 0.0F) ||
                    (transform->x > side - boxSize && motion->velocityX > 0.0F)) {
                    motion->velocityX = -motion->velocityX;
                }
                if ((transform->y < 0.0F && motion->velocityY < 0.0F) ||
                    (transform->y > side - boxSize && motion->velocityY > 0.0F)) {
                    motion->velocityY = -motion->velocityY;
                }
                if (motion->velocityX == 0.0F && motion->velocityY == 0.0F) {
                    motion->velocityX = maxSpeed;  // a solid hit can zero both axes
                }
            });
        }
        Collider& collider = object.add<Collider>(kind);
        collider.onCollide = [&scene](GameObject&, GameObject&) { ++scene.callbacks; };
        object.setActive(true);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const Bench::Options options = Bench::parseOptions(
        argc, argv, "collision-benchmark", "World::resolveCollisions() cost versus collider count");

    const std::vector<int> counts = options.quick ? std::vector<int>{10, 100}
                                                  : std::vector<int>{10, 100, 500, 1000, 2000};
    const std::vector<std::string> scenarios{"static", "moving", "trigger"};
    const int repetitions = options.quick ? 1 : 5;
    const int warmupFrames = options.quick ? 5 : 60;
    const int measuredFrames = options.quick ? 30 : 600;
    constexpr double dt = 1.0 / 60.0;

    Bench::Results results({"scenario", "colliders"},
                           {"collision_mean_us", "collision_p95_us", "collision_p99_us", "callbacks_per_frame"});

    for (int rep = 1; rep <= repetitions; ++rep) {
        for (int colliders : counts) {
            for (const std::string& scenario : scenarios) {
                World world;
                Scene scene;
                std::mt19937 random(581U + static_cast<unsigned>(rep));
                populate(world, scene, colliders, scenario, random);

                std::vector<double> frameNs;
                frameNs.reserve(static_cast<std::size_t>(measuredFrames));
                std::uint64_t measuredCallbacks = 0;
                FrameTime time{dt, 0};
                for (int frame = 0; frame < warmupFrames + measuredFrames; ++frame) {
                    time.gameTimeUs += static_cast<std::int64_t>(dt * 1e6);
                    world.updateComponents(time);
                    const std::uint64_t before = scene.callbacks;
                    const std::int64_t start = Bench::nowNs();
                    world.resolveCollisions();
                    const std::int64_t end = Bench::nowNs();
                    world.flushDestroyed();
                    if (frame >= warmupFrames) {
                        frameNs.push_back(static_cast<double>(end - start));
                        measuredCallbacks += scene.callbacks - before;
                    }
                }

                std::vector<double> metrics = Bench::frameStatsUs(frameNs);
                metrics.push_back(static_cast<double>(measuredCallbacks) / measuredFrames);
                results.add({scenario, std::to_string(colliders)}, rep, metrics);
            }
        }
        std::cout << "repetition " << rep << '/' << repetitions << " done\n";
    }

    results.write(options);
    results.print();
    return 0;
}
