#include "Collision.hpp"
#include "Entity.hpp"

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

} // namespace

int main()
{
    bool passed = true;

    // --- detection between two entities -------------------------------------
    Entity player(0.0F, 0.0F, 100.0F, 100.0F);
    Entity overlapping(50.0F, 50.0F, 100.0F, 100.0F);
    Entity apart(500.0F, 500.0F, 100.0F, 100.0F);

    passed &= expect(Collision::intersects(player, overlapping),
                     "overlapping entities should collide");
    passed &= expect(!Collision::intersects(player, apart),
                     "distant entities should not collide");
    passed &= expect(Collision::intersects(overlapping, player),
                     "detection should not depend on argument order");
    passed &= expect(Collision::intersects(player, player),
                     "an entity should overlap itself");

    // --- edge cases ---------------------------------------------------------
    Entity touching(100.0F, 0.0F, 100.0F, 100.0F);
    passed &= expect(!Collision::intersects(player, touching),
                     "entities that only touch edges should not collide");

    Entity contained(25.0F, 25.0F, 10.0F, 10.0F);
    passed &= expect(Collision::intersects(player, contained),
                     "a fully contained entity should collide");

    Entity empty(10.0F, 10.0F, 0.0F, 50.0F);
    passed &= expect(!Collision::intersects(player, empty),
                     "a zero-sized entity should never collide");

    // --- overlap region -----------------------------------------------------
    const Rect overlap = Collision::getIntersection(player, overlapping);
    passed &= expect(nearlyEqual(overlap.x, 50.0F) && nearlyEqual(overlap.y, 50.0F) &&
                         nearlyEqual(overlap.width, 50.0F) && nearlyEqual(overlap.height, 50.0F),
                     "intersection should describe the overlapping region");

    const Rect noOverlap = Collision::getIntersection(player, apart);
    passed &= expect(nearlyEqual(noOverlap.width, 0.0F) && nearlyEqual(noOverlap.height, 0.0F),
                     "intersection of non-colliding entities should be empty");

    // --- point containment --------------------------------------------------
    passed &= expect(Collision::contains(player.getBounds(), 50.0F, 50.0F),
                     "a point inside the box should be contained");
    passed &= expect(!Collision::contains(player.getBounds(), 150.0F, 50.0F),
                     "a point outside the box should not be contained");

    // --- separation vector --------------------------------------------------
    Entity mover(90.0F, 0.0F, 100.0F, 100.0F);
    Entity wall(180.0F, 0.0F, 100.0F, 100.0F);
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
    Entity movingPlayer(90.0F, 0.0F, 100.0F, 100.0F);
    movingPlayer.setVelocity(500.0F, 200.0F);
    passed &= expect(Collision::resolve(movingPlayer, wall),
                     "resolve should report that it handled a collision");
    passed &= expect(nearlyEqual(movingPlayer.getX(), 80.0F),
                     "resolve should push the player out of the blocker");
    passed &= expect(nearlyEqual(movingPlayer.getVelocityX(), 0.0F),
                     "resolve should stop movement along the blocked axis");
    passed &= expect(nearlyEqual(movingPlayer.getVelocityY(), 200.0F),
                     "resolve should keep movement along the free axis");
    passed &= expect(!Collision::intersects(movingPlayer, wall),
                     "the player should no longer overlap after resolving");

    // Landing on top of a blocker should push up, not sideways.
    Entity faller(0.0F, 95.0F, 100.0F, 100.0F);
    Entity ground(0.0F, 180.0F, 400.0F, 100.0F);
    faller.setVelocity(50.0F, 300.0F);
    passed &= expect(Collision::resolve(faller, ground), "faller should collide with the ground");
    passed &= expect(nearlyEqual(faller.getY(), 80.0F) && nearlyEqual(faller.getVelocityY(), 0.0F),
                     "shallow vertical overlap should push back along y and stop falling");
    passed &= expect(nearlyEqual(faller.getVelocityX(), 50.0F),
                     "horizontal movement should survive a vertical resolution");

    passed &= expect(!Collision::resolve(faller, apart),
                     "resolve should do nothing when the entities are apart");

    // --- moving entity over time (auto-moving object scenario) --------------
    Entity target(0.0F, 0.0F, 50.0F, 50.0F);
    Entity autoMover(200.0F, 0.0F, 50.0F, 50.0F);
    autoMover.setVelocity(-100.0F, 0.0F);
    passed &= expect(!Collision::intersects(target, autoMover),
                     "the auto-moving object should start apart from the target");
    autoMover.update(1.0F);
    passed &= expect(!Collision::intersects(target, autoMover),
                     "after one second it should still be apart");
    autoMover.update(1.0F);
    passed &= expect(Collision::intersects(target, autoMover),
                     "after two seconds it should overlap the target");

    // --- Entity-level convenience API ---------------------------------------
    passed &= expect(player.collidesWith(overlapping) && !player.collidesWith(apart),
                     "Entity::collidesWith should match Collision::intersects");
    passed &= expect(player.containsPoint(50.0F, 50.0F) && !player.containsPoint(150.0F, 50.0F),
                     "Entity::containsPoint should match Collision::contains");

    if (passed) {
        std::cout << "All collision tests passed.\n";
        return 0;
    }

    return 1;
}
