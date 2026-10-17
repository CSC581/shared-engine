#include "ApexComponents.hpp"

#include "GameObject.hpp"

void Carrier::onAttach()
{
    transform_ = &owner().require<Transform>();
}

void Carrier::update(const FrameTime& time)
{
    // The first frame only records where we are: an online platform's first
    // position arrives after creation and must not count as a move.
    if (!hasPrevious_) {
        previousX_ = transform_->x;
        previousY_ = transform_->y;
        hasPrevious_ = true;
        return;
    }

    dx = transform_->x - previousX_;
    dy = transform_->y - previousY_;
    previousX_ = transform_->x;
    previousY_ = transform_->y;

    sinceLastMove_ += time.dtSeconds;
    if (dx != 0.0F || dy != 0.0F) {
        if (sinceLastMove_ > 0.0) {
            velocityX = static_cast<float>(dx / sinceLastMove_);
            velocityY = static_cast<float>(dy / sinceLastMove_);
        }
        sinceLastMove_ = 0.0;
    } else if (sinceLastMove_ > stoppedAfterSeconds) {
        velocityX = 0.0F;
        velocityY = 0.0F;
    }
}
