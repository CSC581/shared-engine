#pragma once

#include "Entity.hpp"

// Bounding-box (AABB) collision detection for games built on the engine.
//
// The engine only answers questions about geometry: "are these two things
// overlapping, and by how much?". Deciding what happens afterwards (stopping a
// player, resetting a position, printing a message) is the game's job.
class Collision {
public:
    // Core query: do the two axis-aligned boxes overlap?
    // Touching edges are not treated as a collision.
    static bool intersects(const Rect& a, const Rect& b);

    // Generic entity-to-entity check, the form games use most.
    static bool intersects(const Entity& a, const Entity& b);

    // Overlapping region of two boxes. Width/height are zero when they do not
    // intersect. Useful for games that want to react to how deep the overlap is.
    static Rect getIntersection(const Rect& a, const Rect& b);
    static Rect getIntersection(const Entity& a, const Entity& b);

    // Is the point inside the box? Handy for click/target style checks.
    static bool contains(const Rect& box, float x, float y);

    // Smallest push (along one axis) that separates `moving` from `blocker`.
    // Returns false when they are not overlapping, leaving the outputs alone.
    // Games apply this themselves, e.g. to stop a player against a wall.
    static bool getSeparation(const Entity& moving, const Entity& blocker, float& outX, float& outY);

    // Convenience response built on getSeparation(): moves `moving` out of
    // `blocker` and zeroes the velocity on the axis it was pushed along.
    // Returns true when a collision was resolved.
    static bool resolve(Entity& moving, const Entity& blocker);
};
