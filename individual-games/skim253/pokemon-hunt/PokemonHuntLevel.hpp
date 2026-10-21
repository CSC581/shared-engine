#pragma once

#include "Components.hpp"
#include "PokemonHuntConfig.hpp"
#include "PokemonHuntWorld.hpp"
#include "World.hpp"

#include <cmath>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

// The client's level as GameObjects: Solid platforms and hidden walls (no
// Motion, so collisions push the Pokemon out of them), hidden spawn points and
// a hidden death zone.
namespace PokemonHunt {

constexpr int levelLayer = 0;

// Collider masks: what hurts, what a shot hits, hidden zones, and the ground.
constexpr std::uint32_t hurtLayer = 1U;
constexpr std::uint32_t shotLayer = 2U;
constexpr std::uint32_t zoneLayer = 4U;
constexpr std::uint32_t solidLayer = 8U;

// The floor, cut by two pits.
struct StaticPlatform {
    float x;
    float width;
    Color color;
};
constexpr StaticPlatform staticPlatforms[] = {
    {0.0F, 900.0F, Palette::floor},
    {970.0F, 730.0F, Palette::stoneFloor},
    {1770.0F, 630.0F, Palette::mossFloor},
};

constexpr bool pitsFitPokemonButNotMewtwo()
{
    for (std::size_t i = 1; i < std::size(staticPlatforms); ++i) {
        const float pit = staticPlatforms[i].x - (staticPlatforms[i - 1].x + staticPlatforms[i - 1].width);
        if (pit <= pokemonSize || pit >= bossWidth) {
            return false;
        }
    }
    return true;
}
static_assert(pitsFitPokemonButNotMewtwo(), "pits must be wider than a Pokemon and narrower than Mewtwo");

inline GameObject& makeStaticPlatform(World& world, const StaticPlatform& platform)
{
    GameObject& block = world.create("platform");
    block.add<Transform>(platform.x, groundY, platform.width, windowHeight - groundY);
    block.add<Renderable>(platform.color).layer = levelLayer;
    block.add<Collider>(Collider::Kind::Solid, solidLayer);
    block.setActive(true);
    return block;
}

// Hidden, just past one end of the level.
inline GameObject& makeWall(World& world, float x)
{
    constexpr float thickness = 100.0F;
    GameObject& wall = world.create("wall");
    wall.add<Transform>(x < 0.0F ? -thickness : x, -windowHeight, thickness, 3.0F * windowHeight);
    wall.add<Collider>(Collider::Kind::Solid, solidLayer);
    wall.setActive(true);
    return wall;
}

// NetworkWorldSync's factory; the server owns the position, so no Motion.
inline GameObject& makeMovingPlatform(World& world)
{
    GameObject& platform = world.create("moving-platform");
    platform.add<Transform>();
    platform.add<Renderable>(Palette::movingPlatform).layer = levelLayer;
    platform.add<Collider>(Collider::Kind::Solid, solidLayer);
    return platform;
}

// Hidden: no Renderable.
inline GameObject& makeSpawnPoint(World& world, float x, float y)
{
    GameObject& spawn = world.create("spawn");
    spawn.add<Transform>(x, y, pokemonSize, pokemonSize);
    spawn.setActive(true);
    return spawn;
}

// Hidden, below the pits and wider than the level.
inline GameObject& makeDeathZone(World& world, Collider::Callback onFall)
{
    constexpr float overhang = 500.0F;
    GameObject& zone = world.create("death-zone");
    zone.add<Transform>(-overhang, windowHeight + 40.0F, levelWidth + 2.0F * overhang, 400.0F);
    zone.add<Collider>(Collider::Kind::Trigger, zoneLayer).onCollide = std::move(onFall);
    zone.setActive(true);
    return zone;
}

// Spawn points are created in species order: spawn point N is species N's.
inline void buildLevel(World& world)
{
    for (const StaticPlatform& platform : staticPlatforms) {
        makeStaticPlatform(world, platform);
    }
    makeWall(world, -1.0F);
    makeWall(world, levelWidth);
    for (int kind = 0; kind < SpeciesCount; ++kind) {
        makeSpawnPoint(world, spawnX(kind), spawnY);
    }
}

inline const Transform& spawnPointFor(World& world, int kind)
{
    const std::vector<GameObject*> spawns = world.findAllByTag("spawn");
    if (kind < 0 || static_cast<std::size_t>(kind) >= spawns.size()) {
        throw std::out_of_range("no spawn point for this species");
    }
    return *spawns[static_cast<std::size_t>(kind)]->get<Transform>();
}

// Grounded check (the engine has none): after the collision pass a resting
// body's feet sit exactly on the platform's top.
inline bool standsOn(const Transform& body, const Transform& platform)
{
    return std::fabs(body.y + body.height - platform.y) < 0.5F && body.x < platform.x + platform.width &&
           platform.x < body.x + body.width;
}

} // namespace PokemonHunt
