#include "Collision.hpp"
#include "Components.hpp"
#include "World.hpp"

#include <cmath>
#include <iostream>

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

// A solid box that never moves: Transform + Collider, no Motion.
GameObject& makeBlocker(World& world, Rect box)
{
    GameObject& blocker = world.create("blocker");
    blocker.add<Transform>(box.x, box.y, box.width, box.height);
    blocker.add<Collider>();
    blocker.setActive(true);
    return blocker;
}

// A solid box with a velocity, which the collision pass may push.
GameObject& makeMover(World& world, Rect box, float velocityX, float velocityY)
{
    GameObject& mover = world.create("mover");
    mover.add<Transform>(box.x, box.y, box.width, box.height);
    mover.add<Motion>(velocityX, velocityY);
    mover.add<Collider>();
    mover.setActive(true);
    return mover;
}

} // namespace

int main()
{
    bool passed = true;

    // --- detection between two boxes ----------------------------------------
    const Rect player{0.0F, 0.0F, 100.0F, 100.0F};
    const Rect overlapping{50.0F, 50.0F, 100.0F, 100.0F};
    const Rect apart{500.0F, 500.0F, 100.0F, 100.0F};

    passed &= expect(Collision::intersects(player, overlapping),
                     "overlapping boxes should collide");
    passed &= expect(!Collision::intersects(player, apart),
                     "distant boxes should not collide");
    passed &= expect(Collision::intersects(overlapping, player),
                     "detection should not depend on argument order");
    passed &= expect(Collision::intersects(player, player),
                     "a box should overlap itself");

    // --- edge cases ---------------------------------------------------------
    const Rect touching{100.0F, 0.0F, 100.0F, 100.0F};
    passed &= expect(!Collision::intersects(player, touching),
                     "boxes that only touch edges should not collide");

    const Rect contained{25.0F, 25.0F, 10.0F, 10.0F};
    passed &= expect(Collision::intersects(player, contained),
                     "a fully contained box should collide");

    const Rect empty{10.0F, 10.0F, 0.0F, 50.0F};
    passed &= expect(!Collision::intersects(player, empty),
                     "a zero-sized box should never collide");

    // --- overlap region -----------------------------------------------------
    const Rect overlap = Collision::getIntersection(player, overlapping);
    passed &= expect(nearlyEqual(overlap.x, 50.0F) && nearlyEqual(overlap.y, 50.0F) &&
                         nearlyEqual(overlap.width, 50.0F) && nearlyEqual(overlap.height, 50.0F),
                     "intersection should describe the overlapping region");

    const Rect noOverlap = Collision::getIntersection(player, apart);
    passed &= expect(nearlyEqual(noOverlap.width, 0.0F) && nearlyEqual(noOverlap.height, 0.0F),
                     "intersection of non-colliding boxes should be empty");

    // --- point containment --------------------------------------------------
    passed &= expect(Collision::contains(player, 50.0F, 50.0F),
                     "a point inside the box should be contained");
    passed &= expect(!Collision::contains(player, 150.0F, 50.0F),
                     "a point outside the box should not be contained");

    // --- separation vector --------------------------------------------------
    const Rect mover{90.0F, 0.0F, 100.0F, 100.0F};
    const Rect wall{180.0F, 0.0F, 100.0F, 100.0F};
    float pushX = 0.0F;
    float pushY = 0.0F;
    passed &= expect(Collision::getSeparation(mover, wall, pushX, pushY),
                     "separation should report the overlap");
    passed &= expect(nearlyEqual(pushX, -10.0F) && nearlyEqual(pushY, 0.0F),
                     "shallow horizontal overlap should push back along x");

    float untouchedX = 12.0F;
    float untouchedY = 34.0F;
    passed &= expect(!Collision::getSeparation(mover, apart, untouchedX, untouchedY),
                     "separation should fail when there is no collision");
    passed &= expect(nearlyEqual(untouchedX, 12.0F) && nearlyEqual(untouchedY, 34.0F),
                     "failed separation should leave the outputs alone");

    // --- resolution: the "stop the player" response a game needs ------------
    // Done by World's collision pass: a solid object with Motion is pushed out
    // of a solid one without, and stops along the axis it was pushed on.
    {
        World world;
        GameObject& movingPlayer = makeMover(world, mover, 500.0F, 200.0F);
        makeBlocker(world, wall);
        world.resolveCollisions();

        const Transform& body = *movingPlayer.get<Transform>();
        const Motion& motion = *movingPlayer.get<Motion>();
        passed &= expect(nearlyEqual(body.x, 80.0F), "resolving should push the player out of the blocker");
        passed &= expect(nearlyEqual(motion.velocityX, 0.0F),
                         "resolving should stop movement along the blocked axis");
        passed &= expect(nearlyEqual(motion.velocityY, 200.0F),
                         "resolving should keep movement along the free axis");
        passed &= expect(!Collision::intersects(body.bounds(), wall),
                         "the player should no longer overlap after resolving");
    }

    // Landing on top of a blocker should push up, not sideways.
    {
        World world;
        GameObject& faller = makeMover(world, {0.0F, 95.0F, 100.0F, 100.0F}, 50.0F, 300.0F);
        makeBlocker(world, {0.0F, 180.0F, 400.0F, 100.0F});
        world.resolveCollisions();

        const Transform& body = *faller.get<Transform>();
        const Motion& motion = *faller.get<Motion>();
        passed &= expect(nearlyEqual(body.y, 80.0F) && nearlyEqual(motion.velocityY, 0.0F),
                         "shallow vertical overlap should push back along y and stop falling");
        passed &= expect(nearlyEqual(motion.velocityX, 50.0F),
                         "horizontal movement should survive a vertical resolution");
    }

    {
        World world;
        GameObject& loner = makeMover(world, {0.0F, 0.0F, 100.0F, 100.0F}, 50.0F, 300.0F);
        makeBlocker(world, apart);
        world.resolveCollisions();
        passed &= expect(nearlyEqual(loner.get<Transform>()->x, 0.0F) &&
                             nearlyEqual(loner.get<Motion>()->velocityY, 300.0F),
                         "resolving should do nothing when the objects are apart");
    }

    // --- moving object over time (auto-moving object scenario) --------------
    {
        World world;
        GameObject& target = world.create("target");
        target.add<Transform>(0.0F, 0.0F, 50.0F, 50.0F);
        target.setActive(true);
        GameObject& autoMover = world.create("auto-mover");
        autoMover.add<Transform>(200.0F, 0.0F, 50.0F, 50.0F);
        autoMover.add<Motion>(-100.0F, 0.0F);
        autoMover.setActive(true);

        const auto meetsTarget = [&]() {
            return Collision::intersects(target.get<Transform>()->bounds(), autoMover.get<Transform>()->bounds());
        };
        passed &= expect(!meetsTarget(), "the auto-moving object should start apart from the target");
        world.update(FrameTime{1.0, 0});
        passed &= expect(!meetsTarget(), "after one second it should still be apart");
        world.update(FrameTime{1.0, 0});
        passed &= expect(meetsTarget(), "after two seconds it should overlap the target");
    }

    if (passed) {
        std::cout << "All collision tests passed.\n";
        return 0;
    }

    return 1;
}
