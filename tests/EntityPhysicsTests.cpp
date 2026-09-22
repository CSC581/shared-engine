#include "Entity.hpp"
#include "Physics.hpp"

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

namespace {

bool nearlyEqual(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.001F;
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }

    return condition;
}

constexpr int readerCount = 4;
constexpr int readsPerReader = 100'000;
constexpr int writerIterations = 50'000;

} // namespace

int main()
{
    bool passed = true;

    Entity entity(10.0F, 20.0F, 30.0F, 40.0F);
    const Rect initialBounds = entity.getBounds();
    passed &= expect(nearlyEqual(initialBounds.x, 10.0F) && nearlyEqual(initialBounds.y, 20.0F),
                     "bounds should start at the entity position");
    passed &= expect(nearlyEqual(initialBounds.width, 30.0F) && nearlyEqual(initialBounds.height, 40.0F),
                     "bounds should contain the entity size");

    entity.setVelocity(100.0F, -50.0F);
    entity.update(0.5F);
    passed &= expect(nearlyEqual(entity.getX(), 60.0F) && nearlyEqual(entity.getY(), -5.0F),
                     "update should move position using velocity and delta time");

    const Rect movedBounds = entity.getBounds();
    passed &= expect(nearlyEqual(movedBounds.x, 60.0F) && nearlyEqual(movedBounds.y, -5.0F),
                     "bounds should follow the entity position");

    entity.setSize(50.0F, 60.0F);
    const Rect resizedBounds = entity.getBounds();
    passed &= expect(nearlyEqual(resizedBounds.width, 50.0F) && nearlyEqual(resizedBounds.height, 60.0F),
                     "bounds should reflect an updated entity size");

    entity.setVelocity(100.0F, -50.0F);
    entity.setVelocityX(25.0F);
    entity.setVelocityY(75.0F);
    passed &= expect(nearlyEqual(entity.getVelocityX(), 25.0F) && nearlyEqual(entity.getVelocityY(), 75.0F),
                     "individual velocity setters should change only their own axis");

    entity.setVelocity(0.0F, 0.0F);
    Physics::setGravity(980.0F);
    Physics::applyGravity(entity, 0.5F);
    passed &= expect(nearlyEqual(entity.getVelocityY(), 490.0F),
                     "gravity should increase downward velocity");

    Physics::setGravity(0.0F);
    Physics::applyGravity(entity, 1.0F);
    passed &= expect(nearlyEqual(entity.getVelocityY(), 490.0F),
                     "zero gravity should not change vertical velocity");

    Entity stationary(0.0F, 0.0F, 10.0F, 10.0F);
    stationary.update(1.0F);
    passed &= expect(nearlyEqual(stationary.getX(), 0.0F) && nearlyEqual(stationary.getY(), 0.0F),
                     "an entity with no velocity should remain still");

    // Concurrent readers + one writer. setPosition writes x and y under one
    // lock; getBounds must never return a torn pair where x != y.
    {
        Entity shared(0.0F, 0.0F, 10.0F, 10.0F);
        std::atomic<int> tornReads{0};

        std::vector<std::thread> threads;
        threads.reserve(readerCount + 1);

        for (int reader = 0; reader < readerCount; ++reader) {
            threads.emplace_back([&shared, &tornReads] {
                for (int i = 0; i < readsPerReader; ++i) {
                    const Rect bounds = shared.getBounds();
                    if (bounds.x != bounds.y) {
                        tornReads.fetch_add(1);
                        return;
                    }
                }
            });
        }

        threads.emplace_back([&shared] {
            for (int i = 0; i < writerIterations; ++i) {
                const float value = static_cast<float>(i);
                shared.setPosition(value, value);
            }
        });

        for (std::thread& thread : threads) {
            thread.join();
        }

        passed &= expect(tornReads.load() == 0,
                         "getBounds must not tear under concurrent setPosition");
    }

    if (passed) {
        std::cout << "Entity and Physics tests passed.\n";
        return 0;
    }

    return 1;
}
