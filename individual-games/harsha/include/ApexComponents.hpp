// Apex-only components. The engine's components (Components.hpp) cover
// position, movement, collision and drawing; these add what a climbing game
// needs to know about a platform on top of that.

#pragma once

#include "Component.hpp"
#include "Components.hpp"

// What a platform is. Drives its colour and, later, the per-kind rules
// (slingshot fling, orbit hops).
class ApexPlatform : public Component {
public:
    enum class Kind { Stone, Moss, Sandstone, Ice, Mover, Slingshot, Orbit };

    explicit ApexPlatform(Kind kind)
        : kind(kind)
    {
    }

    Kind kind;
};

// How much of the climber's horizontal speed survives one second of standing
// on this platform. Tiny means stop almost at once; closer to 1 means slide.
class Surface : public Component {
public:
    static constexpr float ground = 0.00002F;

    explicit Surface(float friction = ground)
        : friction(friction)
    {
    }

    float friction;
};

// Measures how far its object moved this frame, so a climber standing on it
// can be carried along.
//
// It reads only the Transform, never PathMover, so it works the same offline
// (PathMover moves the platform) and online (NetworkWorldSync writes server
// positions into the Transform). Runs after Motion so the move is complete.
class Carrier : public Component {
public:
    void onAttach() override;
    void update(const FrameTime& time) override;
    int priority() const override { return UpdateOrder::Default; }

    // Movement since the previous frame, in world units.
    float dx = 0.0F;
    float dy = 0.0F;

    // Speed in units per game second. Online, positions only change when a
    // snapshot arrives, so this is the last move divided by the game time it
    // took, and it holds between snapshots instead of dropping to zero.
    float velocityX = 0.0F;
    float velocityY = 0.0F;

private:
    // A platform that has not moved for this long is treated as stopped.
    static constexpr double stoppedAfterSeconds = 0.25;

    Transform* transform_ = nullptr;
    bool hasPrevious_ = false;
    float previousX_ = 0.0F;
    float previousY_ = 0.0F;
    double sinceLastMove_ = 0.0;
};
