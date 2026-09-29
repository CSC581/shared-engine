#include "SendPacer.hpp"

#include <stdexcept>

namespace Multiplayer {

SendPacer::SendPacer(const TimeSource* gameTime, std::int64_t gameInterval,
                     const TimeSource& realTime, std::int64_t heartbeatInterval)
    : gameTime_(gameTime),
      gameInterval_(gameInterval),
      realTime_(realTime),
      heartbeatInterval_(heartbeatInterval)
{
    if (gameInterval_ < 0) {
        throw std::invalid_argument("SendPacer: send interval must not be negative");
    }
    if (gameInterval_ > 0 && gameTime_ == nullptr) {
        throw std::invalid_argument("SendPacer: pacing on game time needs a game clock");
    }
    if (gameInterval_ > 0 && heartbeatInterval_ <= 0) {
        throw std::invalid_argument("SendPacer: pacing needs a positive heartbeat interval");
    }
}

bool SendPacer::shouldSend()
{
    if (gameInterval_ == 0) {
        return true;
    }

    const std::int64_t gameNow = gameTime_->now();
    const std::int64_t realNow = realTime_.now();

    const bool first = !sentOnce_;
    const bool gameDue = gameNow >= nextGameSend_;
    const bool heartbeatDue = realNow - lastRealSend_ >= heartbeatInterval_;
    if (!first && !gameDue && !heartbeatDue) {
        return false;
    }

    // Step the schedule by whole intervals so frame jitter does not lower the
    // rate, but never let it fall behind now: after a stall there is no burst
    // of catch-up sends, only the next one on time.
    if (first || gameNow - nextGameSend_ >= gameInterval_) {
        nextGameSend_ = gameNow + gameInterval_;
    } else if (gameDue) {
        nextGameSend_ += gameInterval_;
    }
    lastRealSend_ = realNow;
    sentOnce_ = true;
    return true;
}

} // namespace Multiplayer
