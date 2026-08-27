#pragma once

struct Rect {
    float x;
    float y;
    float width;
    float height;
};

class Entity {
public:
    Entity(float x, float y, float width, float height);

    void update(float deltaTime);

    void setPosition(float x, float y);
    void setSize(float width, float height);
    void setVelocity(float velocityX, float velocityY);
    void setVelocityX(float velocityX);
    void setVelocityY(float velocityY);

    float getX() const;
    float getY() const;
    float getVelocityX() const;
    float getVelocityY() const;
    Rect getBounds() const;

    // Collision helpers. Implemented in Collision.cpp so that Entity stays free
    // of any dependency on the collision system while games can still ask the
    // question in the natural place: player.collidesWith(wall).
    bool collidesWith(const Entity& other) const;
    bool containsPoint(float x, float y) const;

private:
    float x_;
    float y_;
    float width_;
    float height_;
    float velocityX_ = 0.0F;
    float velocityY_ = 0.0F;
};
