#include "Entity.hpp"

Entity::Entity(float x, float y, float width, float height)
    : x_(x), y_(y), width_(width), height_(height)
{
}

void Entity::update(float deltaTime)
{
    x_ += velocityX_ * deltaTime;
    y_ += velocityY_ * deltaTime;
}

void Entity::setPosition(float x, float y)
{
    x_ = x;
    y_ = y;
}

void Entity::setSize(float width, float height)
{
    width_ = width;
    height_ = height;
}

void Entity::setVelocity(float velocityX, float velocityY)
{
    velocityX_ = velocityX;
    velocityY_ = velocityY;
}

void Entity::setVelocityX(float velocityX)
{
    velocityX_ = velocityX;
}

void Entity::setVelocityY(float velocityY)
{
    velocityY_ = velocityY;
}

float Entity::getX() const
{
    return x_;
}

float Entity::getY() const
{
    return y_;
}

float Entity::getVelocityX() const
{
    return velocityX_;
}

float Entity::getVelocityY() const
{
    return velocityY_;
}

Rect Entity::getBounds() const
{
    return { x_, y_, width_, height_ };
}
