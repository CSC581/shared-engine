// Builds the Apex tower as GameObjects. Each kind of object has one factory
// function, so what makes a platform or a mover is written down once.

#pragma once

#include "ApexComponents.hpp"
#include "ApexNetworkConfig.hpp"
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

// The platform kind a mover style stands for.
ApexPlatform::Kind kindFor(ApexNetwork::MoverStyle style);

// A platform that rides its path on its own (offline only; online the server
// owns movers). Carrier lets a climber standing on it be carried.
GameObject& makeMover(World& world, const ApexNetwork::MoverSpec& spec);

// A hidden spawn point: a trigger the size of a climber, standing where a
// climber spawns. Standing in it (grounded) activates a checkpoint.
GameObject& makeSpawnMarker(World& world, float x, float y, int room, int slot);

// A hidden wind region: trigger collider, no Renderable.
GameObject& makeWindZone(World& world, float x, float y, float width, float height, float amplitude,
                         float period);

// The local climber. No Renderable: ApexGame draws the animated sprite itself.
GameObject& makePlayer(World& world, float x, float y, float width, float height, float gravity);

// Online factories for NetworkWorldSync. They only attach components:
// apply() sets the pose, adds NetworkIdentity and activates the object.
//
// Another climber: a Transform to draw the ghost sprite at, nothing else. No
// Collider, so climbers pass through each other (see design §10).
GameObject& makeRemotePlayer(World& world, float width, float height);
// A server-owned mover. No Motion: its position belongs to the network, and
// without Motion the local collision pass never pushes it.
GameObject& makeServerPlatform(World& world, std::uint32_t id);

// The whole tower. Online, movers are left out: NetworkWorldSync creates them
// from server snapshots instead.
void buildLevel(World& world, float worldWidth, float worldHeight, float viewHeight, bool online);

} // namespace ApexLevel
