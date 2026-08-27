#pragma once

class Entity;

class Physics {
public:
    static void setGravity(float gravity);
    static float getGravity();
    static void applyGravity(Entity& entity, float deltaTime);

private:
    static float gravity_;
};
