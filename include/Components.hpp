#pragma once

#include "Collision.hpp"
#include "Color.hpp"
#include "Component.hpp"

#include <cstdint>
#include <functional>

class GameObject;
struct SDL_Texture;

// The engine's built-in components. Each one owns one piece of data and the
// one behaviour that changes it; a game object is whatever combination of them
// it carries.
//
//   Transform   where the object is and how big
//   Motion      velocity, integrated into Transform
//   Gravity     constant downward acceleration, added to Motion
//   PathMover   drives Motion along a linear or circular path
//   Collider    takes part in World's collision pass
//   Renderable  drawn by renderWorld()
//   Behavior    game-specific per-frame logic
//   NetworkIdentity  which networked player or platform this object stands for

// Position and size. Every other built-in component depends on it.
class Transform : public Component {
public:
    Transform() = default;
    Transform(float x, float y, float width, float height);

    Rect bounds() const;

    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
};

// Velocity in world units per game second, applied to the Transform each
// frame. An object without Motion never moves on its own and is treated as
// immovable by the collision pass.
class Motion : public Component {
public:
    Motion() = default;
    Motion(float velocityX, float velocityY);

    void onAttach() override;
    void update(const FrameTime& time) override;
    int priority() const override { return UpdateOrder::Motion; }

    float velocityX = 0.0F;
    float velocityY = 0.0F;

    // Set when another component owns this velocity and rewrites it every
    // frame (PathMover does). The collision pass never pushes a kinematic
    // object or changes its velocity, so whatever drives it stays in control.
    bool kinematic = false;

private:
    Transform* transform_ = nullptr;
};

// Constant downward acceleration, in world units per game second squared.
class Gravity : public Component {
public:
    static constexpr float defaultAcceleration = 980.0F;

    Gravity() = default;
    explicit Gravity(float acceleration);

    void onAttach() override;
    void update(const FrameTime& time) override;
    int priority() const override { return UpdateOrder::Gravity; }

    float acceleration = defaultAcceleration;

private:
    Motion* motion_ = nullptr;
};

// Moves an object along a fixed path at a constant speed (units per game
// second). It does not set the position directly: it sets Motion's velocity so
// that Motion lands exactly on the next point of the path. Anything standing on
// a moving platform can therefore read the platform's velocity. It marks that
// Motion kinematic, so collisions never knock the object off its path.
class PathMover : public Component {
public:
    // Back and forth between two points, starting at the first.
    struct Linear {
        float startX;
        float startY;
        float endX;
        float endY;
    };

    // Around a circle, starting at angle 0 (the rightmost point), clockwise in
    // screen coordinates. The centre is where the Transform's top-left corner
    // orbits.
    struct Circular {
        float centerX;
        float centerY;
        float radius;
    };

    PathMover(Linear path, float speed);
    PathMover(Circular path, float speed);

    void onAttach() override;
    void update(const FrameTime& time) override;
    int priority() const override { return UpdateOrder::Path; }

    // Ping-pong along a segment of `length`: moves `distance` by `travel`,
    // bouncing at either end, and flips `direction` (+1 or -1) on each bounce.
    // Returns the new distance. Shared with NetworkServer's moving platforms so
    // the bounce rule exists once.
    static float pingPong(float length, float travel, float distance, float& direction);

private:
    enum class Shape { Linear, Circular };

    void pointAt(float& outX, float& outY) const;

    Shape shape_;
    Linear linear_{};
    Circular circular_{};
    float speed_;
    float length_ = 0.0F;

    // Linear: distance travelled from the start. Circular: angle in radians.
    float progress_ = 0.0F;
    float direction_ = 1.0F;

    Transform* transform_ = nullptr;
    Motion* motion_ = nullptr;
};

// Makes an object take part in World's collision pass.
//
// Solid colliders block each other: an overlapping object with Motion is pushed
// out and loses its velocity along that axis. Objects without Motion, or whose
// Motion is kinematic, are never pushed. Trigger colliders never push or get
// pushed; they only report the overlap. Either kind calls back with (this
// object, the other object):
//   onEnter    once, on the first frame the two overlap
//   onCollide  every frame the overlap lasts, including the first
//   onExit     once, on the first frame they no longer overlap (or one of
//              them was deactivated); skipped if either was destroyed
//
// Two colliders interact only when their layer masks share a bit.
class Collider : public Component {
public:
    enum class Kind { Solid, Trigger };

    using Callback = std::function<void(GameObject& self, GameObject& other)>;

    explicit Collider(Kind kind = Kind::Solid, std::uint32_t layers = 1U);

    void onAttach() override;

    Rect bounds() const;

    Kind kind;
    std::uint32_t layers;
    Callback onEnter;
    Callback onCollide;
    Callback onExit;

private:
    Transform* transform_ = nullptr;
};

// Drawn by renderWorld() as a filled rectangle, or as `texture` stretched over
// the Transform when one is set. The texture is borrowed, not owned. An object
// without a Renderable is never drawn, which is how spawn points, death zones
// and scroll boundaries stay hidden.
class Renderable : public Component {
public:
    Renderable() = default;
    explicit Renderable(Color color, SDL_Texture* texture = nullptr);

    void onAttach() override;

    const Transform& transform() const;

    Color color{255, 255, 255};
    SDL_Texture* texture = nullptr;
    bool visible = true;

private:
    Transform* transform_ = nullptr;
};

// Game-specific per-frame logic, such as reading the keyboard for a player.
// Runs before every other built-in component.
class Behavior : public Component {
public:
    using UpdateFn = std::function<void(GameObject& self, const FrameTime& time)>;

    explicit Behavior(UpdateFn fn);

    void update(const FrameTime& time) override;
    int priority() const override { return UpdateOrder::Behavior; }

private:
    UpdateFn fn_;
};

// Ties an object to the network id it stands for: a player id assigned by the
// server or a peer, or a server platform's id. This is how the network layer
// finds the object a message is about, on the server and on every client. It
// holds data only; whoever owns the networking keeps it up to date.
class NetworkIdentity : public Component {
public:
    enum class Kind { Player, Platform };

    NetworkIdentity(Kind kind, std::uint32_t id);

    Kind kind;
    std::uint32_t id;
};
