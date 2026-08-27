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

private:
    float x_;
    float y_;
    float width_;
    float height_;
    float velocityX_ = 0.0F;
    float velocityY_ = 0.0F;
};
