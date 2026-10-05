#pragma once

// An axis-aligned box: top-left corner plus size, in world units.
struct Rect {
    float x;
    float y;
    float width;
    float height;
};

// Bounding-box (AABB) geometry. This only answers questions about boxes: "do
// these overlap, and by how much?". Acting on the answer belongs to the
// Collider component and World's collision pass.
class Collision {
public:
    // Do the two boxes overlap? Touching edges are not a collision.
    static bool intersects(const Rect& a, const Rect& b);

    // Overlapping region. Width/height are zero when they do not intersect.
    static Rect getIntersection(const Rect& a, const Rect& b);

    // Is the point inside the box? Edges count as inside.
    static bool contains(const Rect& box, float x, float y);

    // Smallest push (along one axis) that separates `moving` from `blocker`.
    // Returns false when they are not overlapping, leaving the outputs alone.
    static bool getSeparation(const Rect& moving, const Rect& blocker, float& outX, float& outY);
};
