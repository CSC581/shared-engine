#include "Collision.hpp"

#include <algorithm>

namespace {

float left(const Rect& rect)
{
    return rect.x;
}

float right(const Rect& rect)
{
    return rect.x + rect.width;
}

float top(const Rect& rect)
{
    return rect.y;
}

float bottom(const Rect& rect)
{
    return rect.y + rect.height;
}

} // namespace

bool Collision::intersects(const Rect& a, const Rect& b)
{
    if (a.width <= 0.0F || a.height <= 0.0F || b.width <= 0.0F || b.height <= 0.0F) {
        return false;
    }

    return left(a) < right(b) && right(a) > left(b) &&
           top(a) < bottom(b) && bottom(a) > top(b);
}

Rect Collision::getIntersection(const Rect& a, const Rect& b)
{
    if (!intersects(a, b)) {
        return { 0.0F, 0.0F, 0.0F, 0.0F };
    }

    const float x = std::max(left(a), left(b));
    const float y = std::max(top(a), top(b));

    return { x, y, std::min(right(a), right(b)) - x, std::min(bottom(a), bottom(b)) - y };
}

bool Collision::contains(const Rect& box, float x, float y)
{
    return x >= left(box) && x <= right(box) && y >= top(box) && y <= bottom(box);
}

bool Collision::getSeparation(const Rect& moving, const Rect& blocker, float& outX, float& outY)
{
    const Rect overlap = getIntersection(moving, blocker);

    if (overlap.width <= 0.0F || overlap.height <= 0.0F) {
        return false;
    }

    if (overlap.width < overlap.height) {
        // Push horizontally, away from the blocker's centre.
        const float direction =
            (left(moving) + right(moving)) < (left(blocker) + right(blocker)) ? -1.0F : 1.0F;
        outX = overlap.width * direction;
        outY = 0.0F;
    } else {
        const float direction =
            (top(moving) + bottom(moving)) < (top(blocker) + bottom(blocker)) ? -1.0F : 1.0F;
        outX = 0.0F;
        outY = overlap.height * direction;
    }

    return true;
}
