// Pokemon Hunt's level, collisions and server-side Mewtwo, without a window.
#include "PokemonHuntBoss.hpp"
#include "PokemonHuntLevel.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace PokemonHunt;

constexpr float frame = 1.0F / 60.0F;
constexpr float floorTop = groundY - pokemonSize;

bool expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.01F;
}

// A Pokemon as the game builds one, minus the keyboard and the fight.
GameObject& makeFallingBody(World& world, float x, float y, float velocityX = 0.0F, float velocityY = 0.0F)
{
    GameObject& body = world.create("pokemon");
    body.add<Transform>(x, y, pokemonSize, pokemonSize);
    body.add<Motion>(velocityX, velocityY);
    body.add<Gravity>(gravity);
    body.add<Collider>(Collider::Kind::Solid, hurtLayer | zoneLayer | solidLayer);
    body.setActive(true);
    return body;
}

bool levelIsGameObjects()
{
    World world;
    buildLevel(world);

    const std::vector<GameObject*> platforms = world.findAllByTag("platform");
    bool passed = expect(platforms.size() >= 3, "at least three static platforms");
    for (std::size_t i = 0; i < platforms.size(); ++i) {
        const Transform& a = *platforms[i]->get<Transform>();
        const Renderable* look = platforms[i]->get<Renderable>();
        passed &= expect(look != nullptr && !platforms[i]->has<Motion>(), "static platforms are drawn and never move");
        for (std::size_t j = i + 1; j < platforms.size(); ++j) {
            const Transform& b = *platforms[j]->get<Transform>();
            const Renderable& other = *platforms[j]->get<Renderable>();
            const bool sameColor = look->color.red == other.color.red && look->color.green == other.color.green &&
                                   look->color.blue == other.color.blue;
            passed &= expect(!sameColor && !near(a.x, b.x) && !near(a.width, b.width),
                             "each static platform has its own position, size and colour");
        }
    }

    const std::vector<GameObject*> spawns = world.findAllByTag("spawn");
    passed &= expect(spawns.size() == SpeciesCount, "one spawn point per species");
    for (int kind = 0; kind < SpeciesCount; ++kind) {
        const Transform& spawn = spawnPointFor(world, kind);
        passed &= expect(near(spawn.x, spawnX(kind)) && near(spawn.y, spawnY), "spawn point N is species N's");
        passed &= expect(!spawns[static_cast<std::size_t>(kind)]->has<Renderable>(), "spawn points are hidden");
    }

    GameObject& zone = makeDeathZone(world, {});
    passed &= expect(!zone.has<Renderable>() && zone.get<Collider>()->kind == Collider::Kind::Trigger,
                     "the death zone is a hidden trigger");
    const std::vector<Network::PlatformPath> paths = movingPlatformPaths();
    passed &= expect(paths.size() >= 2 && paths[0].shape != paths[1].shape,
                     "the server moves platforms in two patterns (linear and circular)");
    return passed;
}

// Runs World frames until the body rests on a platform; returns that platform.
GameObject* fall(World& world, GameObject& pokemon)
{
    const Transform& body = *pokemon.get<Transform>();
    for (int i = 0; i < 240 && body.y < windowHeight + 100.0F; ++i) {
        world.update(FrameTime{frame, 0});
        for (const char* tag : {"platform", "moving-platform"}) {
            for (GameObject* platform : world.findAllByTag(tag)) {
                if (pokemon.get<Motion>()->velocityY == 0.0F && standsOn(body, *platform->get<Transform>())) {
                    return platform;
                }
            }
        }
    }
    return nullptr;
}

bool pokemonFallsOntoTheFloor()
{
    World world;
    buildLevel(world);
    GameObject& pokemon = makeFallingBody(world, 450.0F, 100.0F);
    GameObject* standingOn = fall(world, pokemon);
    return expect(standingOn != nullptr && standingOn->tag() == "platform" && near(pokemon.get<Transform>()->y, floorTop),
                  "the collision pass stands a falling Pokemon on a static platform");
}

bool pitsLeadToTheDeathZone()
{
    World world;
    buildLevel(world);
    const Transform& spawn = spawnPointFor(world, Squirtle);
    bool teleported = false;
    makeDeathZone(world, [&](GameObject&, GameObject& other) {
        Transform& body = *other.get<Transform>();
        body.x = spawn.x;
        body.y = spawn.y;
        teleported = true;
    });
    const float pitStart = staticPlatforms[0].x + staticPlatforms[0].width;
    GameObject& pokemon = makeFallingBody(world, pitStart + (staticPlatforms[1].x - pitStart - pokemonSize) * 0.5F, 300.0F);
    for (int i = 0; i < 240 && !teleported; ++i) {
        world.update(FrameTime{frame, 0});
    }
    const Transform& body = *pokemon.get<Transform>();
    return expect(teleported && near(body.x, spawn.x) && near(body.y, spawn.y),
                  "a Pokemon that falls into a pit reaches the death zone and goes back to a spawn point");
}

bool movingPlatformsAreSolid()
{
    World world;
    buildLevel(world);
    GameObject& platform = makeMovingPlatform(world);
    Transform& box = *platform.get<Transform>();
    box.x = 800.0F;
    box.y = 360.0F;
    box.width = 140.0F;
    box.height = 30.0F;
    platform.setActive(true);

    GameObject& faller = makeFallingBody(world, box.x + 20.0F, 250.0F);
    bool passed = expect(fall(world, faller) == &platform, "a falling Pokemon lands on a moving platform");

    GameObject& jumper = makeFallingBody(world, box.x + 80.0F, box.y + box.height - 2.0F, 0.0F, -300.0F);
    world.update(FrameTime{frame, 0});
    passed &= expect(near(jumper.get<Transform>()->y, box.y + box.height) && jumper.get<Motion>()->velocityY == 0.0F,
                     "a Pokemon jumping into a moving platform from below bumps its head");
    return passed;
}

bool staticPlatformsAreWallsAndTheLevelHasEnds()
{
    World world;
    buildLevel(world);
    const StaticPlatform& second = staticPlatforms[1];

    GameObject& inPit = makeFallingBody(world, second.x - 10.0F, groundY + 20.0F, walkSpeed);
    world.update(FrameTime{frame, 0});
    bool passed = expect(near(inPit.get<Transform>()->x, second.x - pokemonSize),
                         "a Pokemon in a pit cannot walk into the floor");

    GameObject& atStart = makeFallingBody(world, -20.0F, floorTop, -walkSpeed);
    GameObject& atEnd = makeFallingBody(world, levelWidth - 20.0F, floorTop, walkSpeed);
    world.update(FrameTime{frame, 0});
    passed &= expect(near(atStart.get<Transform>()->x, 0.0F), "a hidden wall closes the start of the level");
    passed &= expect(near(atEnd.get<Transform>()->x, levelWidth - pokemonSize),
                     "a hidden wall closes the end of the level");
    return passed;
}

void run(Boss& boss, float seconds)
{
    for (float t = 0.0F; t < seconds; t += frame) {
        boss.update(frame);
    }
}

bool bossWalksBetweenTheWalls()
{
    Boss boss({}, 1U);
    for (int i = 0; i < 60 * 30; ++i) {
        boss.update(frame);
        if (boss.x() < bossMinX - 0.01F || boss.x() > bossMaxX + 0.01F) {
            return expect(false, "Mewtwo left the walls at x=" + std::to_string(boss.x()));
        }
        if (boss.y() > groundY - bossHeight + 0.01F) {
            return expect(false, "Mewtwo sank through the floor");
        }
    }
    return expect(!boss.fainted(), "nobody hit Mewtwo, so it is still standing");
}

bool bossWalksAcrossAPit()
{
    Boss boss({}, 5U);
    const float pitStart = staticPlatforms[0].x + staticPlatforms[0].width;
    boss.walkFrom(pitStart - bossWidth, false);
    run(boss, 1.5F);
    return expect(boss.x() > staticPlatforms[1].x && near(boss.y(), groundY - bossHeight) &&
                      boss.hp() == bossMaxHp,
                  "Mewtwo walks across a pit without falling in");
}

bool bossJumpsAndLands()
{
    Boss boss({}, 2U);
    const float floorY = groundY - bossHeight;
    boss.jump();
    run(boss, 0.3F);
    bool passed = expect(boss.y() < floorY - 50.0F, "Mewtwo rises after jumping");
    run(boss, 1.2F);
    passed &= expect(near(boss.y(), floorY), "Gravity brings Mewtwo back to the floor");
    return passed;
}

bool shadowBallsFlyOffAndDisappear()
{
    Boss boss({}, 3U);
    boss.fireShadowBall();
    bool passed = expect(boss.shadowBallCount() >= 1, "a shadow ball is a GameObject in Mewtwo's World");
    bool firstOnWire = false;
    for (const Multiplayer::Platform& object : boss.objects()) {
        firstOnWire = firstOnWire || object.id == shadowBallIdBase;
    }
    passed &= expect(firstOnWire, "the shadow ball goes out in the boss world");

    run(boss, 4.0F);
    bool firstGone = true;
    for (const Multiplayer::Platform& object : boss.objects()) {
        firstGone = firstGone && object.id != shadowBallIdBase;
    }
    passed &= expect(firstGone, "a shadow ball that leaves the screen is destroyed");
    return passed;
}

bool damageFaintsAndRespawns()
{
    Boss boss({}, 4U);
    boss.reportDamage(1, 10);
    boss.reportDamage(1, 10);
    bool passed = expect(boss.hp() == bossMaxHp - 10, "only new damage comes off the HP");

    boss.reportDamage(2, bossMaxHp);
    passed &= expect(boss.fainted() && boss.objects().size() == 1, "a fainted Mewtwo sends only its status");

    run(boss, bossRespawnSeconds + 0.1F);
    passed &= expect(!boss.fainted() && boss.hp() == bossMaxHp, "Mewtwo comes back at full health");
    run(boss, 2.0F);
    passed &= expect(near(boss.y(), groundY - bossHeight), "the respawned Mewtwo drops onto the floor");
    return passed;
}

} // namespace

int main()
{
    bool passed = true;
    passed &= levelIsGameObjects();
    passed &= pokemonFallsOntoTheFloor();
    passed &= pitsLeadToTheDeathZone();
    passed &= movingPlatformsAreSolid();
    passed &= staticPlatformsAreWallsAndTheLevelHasEnds();
    passed &= bossWalksBetweenTheWalls();
    passed &= bossWalksAcrossAPit();
    passed &= bossJumpsAndLands();
    passed &= shadowBallsFlyOffAndDisappear();
    passed &= damageFaintsAndRespawns();

    if (passed) {
        std::cout << "pokemon-hunt-tests: all checks passed\n";
    }
    return passed ? 0 : 1;
}
