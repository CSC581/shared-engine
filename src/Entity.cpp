#include "Entity.hpp"

Entity::Entity(float x, float y, float width, float height)
    : x_(x), y_(y), width_(width), height_(height)
{
}

Entity::Entity(Entity&& other) noexcept
{
    // other.mutex_ is left alone: the destination owns a fresh lock. Callers
    // must not move an Entity still shared with other threads.
    const std::lock_guard<std::mutex> lock(other.mutex_);
    x_ = other.x_;
    y_ = other.y_;
    width_ = other.width_;
    height_ = other.height_;
    velocityX_ = other.velocityX_;
    velocityY_ = other.velocityY_;
}

Entity& Entity::operator=(Entity&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    // Lock in address order so two concurrent moves of the same pair cannot
    // deadlock on opposite acquisition orders.
    if (this < &other) {
        const std::lock_guard<std::mutex> lockThis(mutex_);
        const std::lock_guard<std::mutex> lockOther(other.mutex_);
        x_ = other.x_;
        y_ = other.y_;
        width_ = other.width_;
        height_ = other.height_;
        velocityX_ = other.velocityX_;
        velocityY_ = other.velocityY_;
    } else {
        const std::lock_guard<std::mutex> lockOther(other.mutex_);
        const std::lock_guard<std::mutex> lockThis(mutex_);
        x_ = other.x_;
        y_ = other.y_;
        width_ = other.width_;
        height_ = other.height_;
        velocityX_ = other.velocityX_;
        velocityY_ = other.velocityY_;
    }

    return *this;
}

void Entity::updateLocked(float deltaTime)
{
    x_ += velocityX_ * deltaTime;
    y_ += velocityY_ * deltaTime;
}

void Entity::update(float deltaTime)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    updateLocked(deltaTime);
}

void Entity::update(const FrameTime& time)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    updateLocked(static_cast<float>(time.dtSeconds));
}

void Entity::setPosition(float x, float y)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    x_ = x;
    y_ = y;
}

void Entity::setSize(float width, float height)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    width_ = width;
    height_ = height;
}

void Entity::setVelocity(float velocityX, float velocityY)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    velocityX_ = velocityX;
    velocityY_ = velocityY;
}

void Entity::setVelocityX(float velocityX)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    velocityX_ = velocityX;
}

void Entity::setVelocityY(float velocityY)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    velocityY_ = velocityY;
}

float Entity::getX() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return x_;
}

float Entity::getY() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return y_;
}

float Entity::getVelocityX() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return velocityX_;
}

float Entity::getVelocityY() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return velocityY_;
}

Rect Entity::getBounds() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return { x_, y_, width_, height_ };
}
