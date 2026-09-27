// How Apex presents online climbers: HUD labels, ghost tints, and the
// animation hint each climber publishes alongside its pose.

#pragma once

#include "Multiplayer.hpp"
#include "animation/PlayerAnimation.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace ApexPresence {

const char* connectionLabel(Multiplayer::State state);
const char* authorityLabel(Multiplayer::AuthorityState state);

// Distinct tints for remote climbers (ghosts); local player keeps the sprite.
void ghostColor(Multiplayer::PlayerId playerId, Uint8& red, Uint8& green, Uint8& blue);

struct RemoteAnimation {
    PlayerAnimation::Clip clip = PlayerAnimation::Clip::Idle;
    int frame = 0;
    bool facingRight = true;
};

std::string encodeAnimation(const PlayerAnimation& animation);
bool decodeAnimation(const std::string& data, RemoteAnimation& animation);

} // namespace ApexPresence
