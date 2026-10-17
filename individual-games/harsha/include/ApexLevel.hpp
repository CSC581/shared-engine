// Builds the Apex tower as GameObjects. Each kind of object has one factory
// function, so what makes a platform or a mover is written down once.

#pragma once

#include "ApexComponents.hpp"
#include "Color.hpp"
#include "Components.hpp"
#include "World.hpp"

#include <cstdint>

namespace ApexLevel {

// Collider layers. Platforms and triggers never interact; the player is on
// both, so it collides with platforms and sets off triggers.
constexpr std::uint32_t platformLayer = 0b01U;
constexpr std::uint32_t triggerLayer = 0b10U;

Color colorFor(ApexPlatform::Kind kind);

// A platform that never moves. Also used for the ground and side walls.
GameObject& makeStatic(World& world, float x, float y, float width, float height,
                       ApexPlatform::Kind kind);

// A platform that rides a ping-pong path on its own (offline only; online the
// server owns movers). Carrier lets a climber standing on it be carried.
GameObject& makeMover(World& world, PathMover::Linear path, float speed, float width,
                      float height);

// The local climber. No Renderable: ApexGame draws the animated sprite itself.
GameObject& makePlayer(World& world, float x, float y, float width, float height, float gravity);

// The whole tower. Online, movers are left out: NetworkWorldSync creates them
// from server snapshots instead.
void buildLevel(World& world, float worldWidth, float worldHeight, float viewHeight, bool online);

} // namespace ApexLevel
