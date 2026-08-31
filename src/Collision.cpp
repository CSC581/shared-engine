#include "Collision.hpp"

#include <algorithm>
#include <cmath>

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

bool Collision::intersects(const Entity& a, const Entity& b)
{
    return intersects(a.getBounds(), b.getBounds());
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

Rect Collision::getIntersection(const Entity& a, const Entity& b)
{
    return getIntersection(a.getBounds(), b.getBounds());
}

bool Collision::contains(const Rect& box, float x, float y)
{
    return x >= left(box) && x <= right(box) && y >= top(box) && y <= bottom(box);
}

bool Collision::getSeparation(const Entity& moving, const Entity& blocker, float& outX, float& outY)
{
    const Rect a = moving.getBounds();
    const Rect b = blocker.getBounds();
    const Rect overlap = getIntersection(a, b);

    if (overlap.width <= 0.0F || overlap.height <= 0.0F) {
        return false;
    }

    if (overlap.width < overlap.height) {
        // Push horizontally, away from the blocker's centre.
        const float direction = (left(a) + right(a)) < (left(b) + right(b)) ? -1.0F : 1.0F;
        outX = overlap.width * direction;
        outY = 0.0F;
    } else {
        const float direction = (top(a) + bottom(a)) < (top(b) + bottom(b)) ? -1.0F : 1.0F;
        outX = 0.0F;
        outY = overlap.height * direction;
    }

    return true;
}

bool Collision::resolve(Entity& moving, const Entity& blocker)
{
    float pushX = 0.0F;
    float pushY = 0.0F;

    if (!getSeparation(moving, blocker, pushX, pushY)) {
        return false;
    }

    moving.setPosition(moving.getX() + pushX, moving.getY() + pushY);

    if (pushX != 0.0F) {
        moving.setVelocityX(0.0F);
    }
    if (pushY != 0.0F) {
        moving.setVelocityY(0.0F);
    }

    return true;
}

// --- Entity conveniences -----------------------------------------------------
// Declared on Entity, implemented here so Entity.cpp never has to know about
// the collision system.

bool Entity::collidesWith(const Entity& other) const
{
    return Collision::intersects(*this, other);
}

bool Entity::containsPoint(float x, float y) const
{
    return Collision::contains(getBounds(), x, y);
}
