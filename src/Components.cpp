#include "Components.hpp"

#include "GameObject.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

constexpr float kTwoPi = 6.28318530718F;

} // namespace

// --- Transform ---------------------------------------------------------------

Transform::Transform(float x, float y, float width, float height)
    : x(x), y(y), width(width), height(height)
{
}

Rect Transform::bounds() const
{
    return { x, y, width, height };
}

// --- Motion ------------------------------------------------------------------

Motion::Motion(float velocityX, float velocityY)
    : velocityX(velocityX), velocityY(velocityY)
{
}

void Motion::onAttach()
{
    transform_ = &owner().require<Transform>();
}

void Motion::update(const FrameTime& time)
{
    const auto dt = static_cast<float>(time.dtSeconds);
    transform_->x += velocityX * dt;
    transform_->y += velocityY * dt;
}

// --- Gravity -----------------------------------------------------------------

Gravity::Gravity(float acceleration)
    : acceleration(acceleration)
{
}

void Gravity::onAttach()
{
    motion_ = &owner().require<Motion>();
}

void Gravity::update(const FrameTime& time)
{
    motion_->velocityY += acceleration * static_cast<float>(time.dtSeconds);
}

// --- PathMover ---------------------------------------------------------------

PathMover::PathMover(Linear path, float speed)
    : shape_(Shape::Linear),
      linear_(path),
      speed_(speed)
{
    const float dx = path.endX - path.startX;
    const float dy = path.endY - path.startY;
    length_ = std::sqrt(dx * dx + dy * dy);

    if (!(speed > 0.0F) || !(length_ > 0.0F)) {
        throw std::invalid_argument("PathMover needs a positive speed and a non-zero path");
    }
}

PathMover::PathMover(Circular path, float speed)
    : shape_(Shape::Circular),
      circular_(path),
      speed_(speed)
{
    if (!(speed > 0.0F) || !(path.radius > 0.0F)) {
        throw std::invalid_argument("PathMover needs a positive speed and radius");
    }
}

void PathMover::onAttach()
{
    motion_ = &owner().require<Motion>();
    motion_->kinematic = true;
    transform_ = &owner().require<Transform>();

    // Start on the path rather than wherever the Transform happened to be.
    pointAt(transform_->x, transform_->y);
}

void PathMover::update(const FrameTime& time)
{
    const auto dt = static_cast<float>(time.dtSeconds);
    if (dt <= 0.0F) {
        motion_->velocityX = 0.0F;
        motion_->velocityY = 0.0F;
        return;
    }

    const float travel = speed_ * dt;
    if (shape_ == Shape::Linear) {
        progress_ = pingPong(length_, travel, progress_, direction_);
    } else {
        progress_ = std::fmod(progress_ + travel / circular_.radius, kTwoPi);
    }

    // The velocity that takes Motion from where the object is to the next
    // point of the path in exactly this frame.
    float targetX = 0.0F;
    float targetY = 0.0F;
    pointAt(targetX, targetY);
    motion_->velocityX = (targetX - transform_->x) / dt;
    motion_->velocityY = (targetY - transform_->y) / dt;
}

float PathMover::pingPong(float length, float travel, float distance, float& direction)
{
    if (!(length > 0.0F)) {
        return 0.0F;
    }

    while (travel > 0.0F) {
        const float room = direction > 0.0F ? length - distance : distance;
        if (room <= 0.0F) {
            direction = -direction;
            continue;
        }
        const float step = std::min(travel, room);
        distance += step * direction;
        travel -= step;
    }
    return distance;
}

void PathMover::pointAt(float& outX, float& outY) const
{
    if (shape_ == Shape::Linear) {
        const float t = progress_ / length_;
        outX = linear_.startX + (linear_.endX - linear_.startX) * t;
        outY = linear_.startY + (linear_.endY - linear_.startY) * t;
    } else {
        outX = circular_.centerX + circular_.radius * std::cos(progress_);
        outY = circular_.centerY + circular_.radius * std::sin(progress_);
    }
}

// --- Collider ----------------------------------------------------------------

Collider::Collider(Kind kind, std::uint32_t layers)
    : kind(kind), layers(layers)
{
}

void Collider::onAttach()
{
    transform_ = &owner().require<Transform>();
}

Rect Collider::bounds() const
{
    return transform_->bounds();
}

// --- Renderable --------------------------------------------------------------

Renderable::Renderable(Color color, SDL_Texture* texture)
    : color(color), texture(texture)
{
}

void Renderable::onAttach()
{
    transform_ = &owner().require<Transform>();
}

const Transform& Renderable::transform() const
{
    return *transform_;
}

// --- Behavior ----------------------------------------------------------------

Behavior::Behavior(UpdateFn fn)
    : fn_(std::move(fn))
{
}

void Behavior::update(const FrameTime& time)
{
    if (fn_) {
        fn_(owner(), time);
    }
}

// --- NetworkIdentity ---------------------------------------------------------

NetworkIdentity::NetworkIdentity(Kind kind, std::uint32_t id)
    : kind(kind), id(id)
{
}
