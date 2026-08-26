#include "Physics.hpp"

#include "Entity.hpp"

float Physics::gravity_ = 980.0F;

void Physics::setGravity(float gravity)
{
    gravity_ = gravity;
}

float Physics::getGravity()
{
    return gravity_;
}

void Physics::applyGravity(Entity& entity, float deltaTime)
{
    entity.setVelocityY(entity.getVelocityY() + gravity_ * deltaTime);
}
