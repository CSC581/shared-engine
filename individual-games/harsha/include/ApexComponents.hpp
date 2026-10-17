// Apex-only components. The engine's components (Components.hpp) cover
// position, movement, collision and drawing; these add what a climbing game
// needs to know about a platform on top of that.

#pragma once

#include "Component.hpp"
#include "Components.hpp"

#include <cmath>

// What a platform is. Drives its colour and the per-kind rules (slingshot
// fling, the summit banner).
class ApexPlatform : public Component {
public:
    enum class Kind { Stone, Moss, Sandstone, Ice, Mover, Slingshot, Orbit, Summit };

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
    // Ice: about 4x the slide of stone. (0.6 slid ~590 px after a walk,
    // wider than any ice ledge.)
    static constexpr float ice = 0.02F;

    explicit Surface(float friction = ground)
        : friction(friction)
    {
    }

    float friction;
};

// A hidden spawn point. Room 0 holds the four starting slots (slot 0-3); each
// higher room holds one checkpoint (slot 0) on its first ledge. Its Transform
// is where a climber stands when spawned there.
class SpawnMarker : public Component {
public:
    SpawnMarker(int room, int slot)
        : room(room), slot(slot)
    {
    }

    int room;
    int slot;
};

// A hidden gusty region (a trigger with no Renderable). While a climber is
// airborne inside it, sideways speed gains amplitude * sin(2*pi*t / period)
// per second, t being game time: gusts swing left and right, freeze on pause
// and follow the timeline's scale.
class WindZone : public Component {
public:
    WindZone(float amplitude, float period)
        : amplitude(amplitude), period(period)
    {
    }

    // Sideways acceleration (units per second squared) at game time t.
    float accelerationAt(double seconds) const
    {
        return amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * seconds / period));
    }

    float amplitude;
    float period;
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
