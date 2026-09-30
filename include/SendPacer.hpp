#pragma once

#include "TimeSource.hpp"

#include <cstdint>

namespace Multiplayer {

// Decides whether a position publish should go out now.
//
// Without pacing, a game that publishes once a frame sends once a frame at any
// speed: 0.5x and 2x only change how far the player moves per message, not how
// many messages there are. Pacing on game time makes the send rate follow the
// game's speed instead — a client at 0.5x sends half as often and one at 2x
// twice as often — so a server sees clients that really do run at different
// rates.
//
// Game time stops while paused, so a paced client would fall silent and be
// timed out by a server that has not heard from it. The heartbeat, on real
// time, keeps a paused client present.
//
// A client can never send faster than it publishes, which is once a frame at
// most. Pick an interval that 2x still fits into: at 60 fps, 1/30 game-second
// gives 30/s at 1x, 15/s at 0.5x and 60/s at 2x.
class SendPacer {
public:
    // `gameInterval` is in `gameTime`'s tics and `heartbeatInterval` in
    // `realTime`'s. A `gameInterval` of 0 disables pacing: every call sends,
    // and `gameTime` may be null. Both clocks must outlive the pacer.
    //
    // Throws std::invalid_argument for a negative interval, for pacing without
    // a game clock, and for pacing without a positive heartbeat.
    SendPacer(const TimeSource* gameTime, std::int64_t gameInterval, const TimeSource& realTime,
              std::int64_t heartbeatInterval);

    // True when a send is due, and records it as sent. Call only when the send
    // will actually happen, so a publish dropped for being disconnected does
    // not use up a slot.
    bool shouldSend();

private:
    const TimeSource* gameTime_;
    std::int64_t gameInterval_;
    const TimeSource& realTime_;
    std::int64_t heartbeatInterval_;

    bool sentOnce_ = false;
    std::int64_t nextGameSend_ = 0;
    std::int64_t lastRealSend_ = 0;
};

} // namespace Multiplayer
