#include "Components.hpp"
#include "FrameWorkers.hpp"
#include "World.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

bool nearlyEqual(float actual, float expected, float tolerance = 0.01F)
{
    return std::fabs(actual - expected) < tolerance;
}

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "Test failed: " << message << '\n';
    }

    return condition;
}

FrameTime frame(double seconds)
{
    return FrameTime{seconds, 0};
}

constexpr double kStep = 1.0 / 60.0;

void run(World& world, int frames)
{
    for (int i = 0; i < frames; ++i) {
        world.update(frame(kStep));
    }
}

// --- A small world object model ------------------------------------------------
//
// One factory per kind of object, the way a game defines its objects on top of
// the engine's components. Each builds the object completely and activates it
// last, so World never sees it half-built.

GameObject& makePlatform(World& world, Rect box, Color color)
{
    GameObject& platform = world.create("platform");
    platform.add<Transform>(box.x, box.y, box.width, box.height);
    platform.add<Collider>(Collider::Kind::Solid);
    platform.add<Renderable>(color);
    platform.setActive(true);
    return platform;
}

template <class Path>
GameObject& makeMovingPlatform(World& world, Path path, float speed, float width, float height)
{
    GameObject& platform = world.create("platform");
    platform.add<PathMover>(path, speed);
    platform.get<Transform>()->width = width;
    platform.get<Transform>()->height = height;
    platform.add<Collider>(Collider::Kind::Solid);
    platform.add<Renderable>(Color{ 200, 120, 60 });
    platform.setActive(true);
    return platform;
}

GameObject& makeCrate(World& world, float x, float y, bool solid)
{
    GameObject& crate = world.create("crate");
    crate.add<Transform>(x, y, 20.0F, 20.0F);
    crate.add<Motion>();
    crate.add<Gravity>();
    if (solid) {
        crate.add<Collider>(Collider::Kind::Solid);
    }
    crate.setActive(true);
    return crate;
}

GameObject& makePlayer(World& world, float x, float y, Behavior::UpdateFn controls)
{
    GameObject& player = world.create("player");
    player.add<Transform>(x, y, 20.0F, 30.0F);
    player.add<Gravity>();
    player.add<Collider>(Collider::Kind::Solid);
    player.add<Renderable>(Color{ 240, 200, 40 });
    if (controls) {
        player.add<Behavior>(std::move(controls));
    }
    player.setActive(true);
    return player;
}

// Hidden: no Renderable.
GameObject& makeSpawnPoint(World& world, float x, float y)
{
    GameObject& spawn = world.create("spawn");
    spawn.add<Transform>(x, y, 0.0F, 0.0F);
    spawn.setActive(true);
    return spawn;
}

// Hidden: no Renderable. Death zones and scroll boundaries differ only in tag
// and in what onEnter does.
GameObject& makeZone(World& world, std::string tag, Rect box, Collider::Callback onEnter)
{
    GameObject& zone = world.create(std::move(tag));
    zone.add<Transform>(box.x, box.y, box.width, box.height);
    zone.add<Collider>(Collider::Kind::Trigger).onCollide = std::move(onEnter);
    zone.setActive(true);
    return zone;
}

// --- The object model itself --------------------------------------------------

bool testAttachAndRequire()
{
    bool passed = true;
    World world;

    // Gravity requires Motion, which requires Transform: one add pulls in all
    // three, and each component's owner is the object it was added to.
    GameObject& crate = world.create("crate");
    Gravity& gravity = crate.add<Gravity>();
    passed &= expect(crate.has<Motion>() && crate.has<Transform>(),
                     "require should attach missing dependencies");
    passed &= expect(&gravity.owner() == &crate && &crate.get<Motion>()->owner() == &crate,
                     "every component should point back at its object");
    passed &= expect(crate.components().size() == 3, "the chain should attach exactly three components");

    bool threw = false;
    try {
        crate.add<Motion>();
    } catch (const std::logic_error&) {
        threw = true;
    }
    passed &= expect(threw, "a second component of the same type should be refused");

    passed &= expect(crate.get<Collider>() == nullptr, "get should return null for a missing component");
    passed &= expect(crate.get<Component>() == nullptr, "get should match only the exact type a component was added as");

    // Lookups are per object: another object's components are not visible.
    GameObject& other = world.create("other");
    other.add<Collider>();
    passed &= expect(other.has<Collider>() && !crate.has<Collider>() && other.get<Gravity>() == nullptr,
                     "each object should find only its own components");

    return passed;
}

// A half-built object is invisible to World until the factory activates it.
bool testInactiveUntilActivated()
{
    bool passed = true;
    World world;

    makePlatform(world, { 0.0F, 0.0F, 100.0F, 100.0F }, Color{ 90, 160, 80 });

    int hits = 0;
    GameObject& crate = world.create("crate");
    crate.add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
    crate.add<Motion>(10.0F, 0.0F);
    crate.add<Collider>().onCollide = [&hits](GameObject&, GameObject&) { ++hits; };

    passed &= expect(!crate.isActive(), "a new object should start inactive");

    world.update(frame(1.0));
    passed &= expect(nearlyEqual(crate.get<Transform>()->x, 0.0F) && hits == 0,
                     "an inactive object should be neither updated nor collided");

    crate.setActive(true);
    world.update(frame(1.0));
    passed &= expect(hits > 0, "once active it should take part in collisions");

    return passed;
}

bool testUpdateOrder()
{
    bool passed = true;
    World world;

    // Attached in the worst order on purpose: Motion before Gravity before the
    // Behavior that sets the intent. Priority, not attach order, decides.
    GameObject& body = world.create();
    body.add<Transform>(0.0F, 0.0F, 10.0F, 10.0F);
    body.add<Motion>();
    body.add<Gravity>(100.0F);
    body.add<Behavior>([](GameObject& self, const FrameTime&) { self.get<Motion>()->velocityX = 50.0F; });
    body.setActive(true);

    world.update(frame(0.1));

    const Transform& transform = *body.get<Transform>();
    passed &= expect(nearlyEqual(transform.x, 5.0F),
                     "Behavior should set velocity before Motion integrates it");
    passed &= expect(nearlyEqual(transform.y, 1.0F),
                     "Gravity should add velocity before Motion integrates it");

    return passed;
}

bool testDeferredDestroyAndLookup()
{
    bool passed = true;
    World world;

    GameObject& a = world.create("coin");
    world.create("coin");
    world.create("player");
    const ObjectId id = a.id();

    passed &= expect(world.findAllByTag("coin").size() == 2, "findAllByTag should find every match");
    passed &= expect(world.findByTag("player") != nullptr, "findByTag should find the object");

    world.destroy(id);
    passed &= expect(world.find(id) != nullptr, "destroy should wait for the end of the frame");
    world.flushDestroyed();
    passed &= expect(world.find(id) == nullptr, "flushDestroyed should remove the object");
    passed &= expect(world.objects().size() == 2, "only the destroyed object should go");

    return passed;
}

// onEnter and onExit fire once per overlap, onCollide on every frame of it. A
// destroyed object gets no onExit.
bool testEnterAndExit()
{
    bool passed = true;
    World world;

    int enters = 0;
    int stays = 0;
    int exits = 0;
    Collider& zone = *makeZone(world, "zone", { 100.0F, 0.0F, 50.0F, 50.0F }, nullptr).get<Collider>();
    zone.onEnter = [&enters](GameObject&, GameObject&) { ++enters; };
    zone.onCollide = [&stays](GameObject&, GameObject&) { ++stays; };
    zone.onExit = [&exits](GameObject&, GameObject&) { ++exits; };

    GameObject& body = world.create("body");
    body.add<Transform>(0.0F, 10.0F, 10.0F, 10.0F);
    body.add<Collider>(Collider::Kind::Trigger);
    body.setActive(true);
    float& bodyX = body.get<Transform>()->x;

    run(world, 1);
    passed &= expect(enters == 0 && stays == 0 && exits == 0, "no callback before the overlap");

    bodyX = 110.0F;
    run(world, 3);
    passed &= expect(enters == 1 && stays == 3 && exits == 0,
                     "onEnter should fire once and onCollide every frame of the overlap");

    bodyX = 0.0F;
    run(world, 2);
    passed &= expect(exits == 1 && stays == 3, "leaving should fire onExit once");

    bodyX = 110.0F;
    run(world, 1);
    passed &= expect(enters == 2, "coming back should fire onEnter again");

    world.destroy(body.id());
    run(world, 2);
    passed &= expect(exits == 1, "a destroyed object should not fire onExit");

    return passed;
}

// --- Combining components into the Part 2 objects ------------------------------

// A static platform is Transform + Collider + Renderable: no Motion, so nothing
// moves it. A crate is Transform + Motion + Gravity. Whether the two interact is
// decided only by whether the crate also has a Collider.
bool testCrateAndLedge()
{
    bool passed = true;

    for (bool solidCrate : { false, true }) {
        World world;
        GameObject& ledge = makePlatform(world, { -50.0F, 100.0F, 200.0F, 20.0F }, Color{ 90, 160, 80 });
        GameObject& crate = makeCrate(world, 0.0F, 0.0F, solidCrate);

        run(world, 120);

        const float crateBottom = crate.get<Transform>()->y + 20.0F;
        if (solidCrate) {
            passed &= expect(nearlyEqual(crateBottom, 100.0F, 0.5F), "a crate with a Collider should land on the ledge");
            passed &= expect(nearlyEqual(crate.get<Motion>()->velocityY, 0.0F, 20.0F),
                             "landing should stop the crate's fall");
        } else {
            passed &= expect(crateBottom > 200.0F, "a crate without a Collider should fall through");
        }
        passed &= expect(nearlyEqual(ledge.get<Transform>()->y, 100.0F), "the ledge should never move");
    }

    return passed;
}

// Three static platforms, each a different combination of position, size and
// colour, from the same factory.
bool testStaticPlatforms()
{
    bool passed = true;
    World world;

    const std::vector<std::pair<Rect, Color>> specs{
        { { 0.0F, 400.0F, 300.0F, 40.0F }, { 90, 160, 80 } },
        { { 350.0F, 320.0F, 120.0F, 20.0F }, { 200, 120, 60 } },
        { { 520.0F, 250.0F, 60.0F, 60.0F }, { 120, 120, 200 } },
    };
    for (const auto& [box, color] : specs) {
        makePlatform(world, box, color);
    }

    run(world, 60);

    const auto platforms = world.findAllByTag("platform");
    passed &= expect(platforms.size() == 3, "three platforms should exist");
    for (std::size_t i = 0; i < platforms.size(); ++i) {
        const Transform& transform = *platforms[i]->get<Transform>();
        const Color& color = platforms[i]->get<Renderable>()->color;
        passed &= expect(nearlyEqual(transform.x, specs[i].first.x) &&
                             nearlyEqual(transform.width, specs[i].first.width) &&
                             color.red == specs[i].second.red,
                         "each static platform should keep its own position, size and colour");
    }

    return passed;
}

// Two movement patterns from one component: PathMover with a Linear or a
// Circular path.
bool testMovingPlatforms()
{
    bool passed = true;
    World world;

    GameObject& slider = makeMovingPlatform(world, PathMover::Linear{ 0.0F, 300.0F, 100.0F, 300.0F }, 50.0F, 80.0F, 16.0F);
    GameObject& orbiter = makeMovingPlatform(world, PathMover::Circular{ 400.0F, 200.0F, 40.0F }, 30.0F, 60.0F, 16.0F);

    passed &= expect(nearlyEqual(slider.get<Transform>()->x, 0.0F), "a linear path should start at its first point");
    passed &= expect(slider.get<Motion>()->kinematic && orbiter.get<Motion>()->kinematic,
                     "PathMover should mark its Motion kinematic");

    // 3 s at 50 units/s is 150 units: to the end (100) and 50 back.
    for (int i = 0; i < 30; ++i) {
        world.update(frame(0.1));
    }
    passed &= expect(nearlyEqual(slider.get<Transform>()->x, 50.0F), "a linear path should bounce at its end");
    passed &= expect(slider.get<Motion>()->velocityX < 0.0F, "after the bounce it should move back");
    passed &= expect(nearlyEqual(slider.get<Transform>()->y, 300.0F), "a horizontal path should not drift vertically");

    const Transform& orbit = *orbiter.get<Transform>();
    passed &= expect(nearlyEqual(std::hypot(orbit.x - 400.0F, orbit.y - 200.0F), 40.0F, 0.05F),
                     "a circular path should stay on its radius");

    float direction = 1.0F;
    passed &= expect(nearlyEqual(PathMover::pingPong(10.0F, 13.0F, 0.0F, direction), 7.0F) && direction < 0.0F,
                     "pingPong should bounce off the far end and flip direction");

    return passed;
}

// A player standing on a moving platform pushes against it every frame. The
// player is pushed out; the platform, being kinematic, stays on its path.
bool testPlayerRidesKinematicPlatform()
{
    bool passed = true;
    World world;

    GameObject& platform = makeMovingPlatform(world, PathMover::Linear{ 0.0F, 300.0F, 0.0F, 200.0F }, 20.0F, 100.0F, 16.0F);
    GameObject& player = makePlayer(world, 30.0F, 260.0F, nullptr);

    // The platform rises 20 units/s for 2 s; the player must ride it up.
    run(world, 120);

    const Transform& p = *player.get<Transform>();
    const Transform& deck = *platform.get<Transform>();
    passed &= expect(nearlyEqual(deck.y, 260.0F, 0.5F), "the platform should follow its path, unpushed");
    passed &= expect(nearlyEqual(p.y + p.height, deck.y, 1.0F), "the player should stand on the rising platform");

    return passed;
}

// The full level: a keyboard-driven player, hidden spawn points, a hidden death
// zone that teleports to a spawn point, and a hidden scroll boundary that moves
// the camera. "Hidden" is not a flag: those objects simply have no Renderable.
bool testLevel()
{
    bool passed = true;
    World world;

    float axis = 0.0F;      // stands in for Input::getAxis(A, D)
    bool jumpPressed = false;
    float cameraX = 0.0F;
    int deaths = 0;

    makeSpawnPoint(world, 40.0F, 300.0F);
    makeSpawnPoint(world, 900.0F, 300.0F);

    GameObject& ground = makePlatform(world, { 0.0F, 400.0F, 600.0F, 40.0F }, Color{ 90, 160, 80 });

    GameObject& player = makePlayer(world, 40.0F, 300.0F, [&](GameObject& self, const FrameTime&) {
        Motion& motion = *self.get<Motion>();
        motion.velocityX = axis * 200.0F;
        if (jumpPressed && motion.velocityY == 0.0F) {
            motion.velocityY = -400.0F;
        }
    });

    // Teleport to the first spawn point (the designated one for this level).
    GameObject& deathZone = makeZone(world, "deathzone", { 0.0F, 600.0F, 2000.0F, 100.0F },
        [&](GameObject&, GameObject& other) {
            if (other.tag() != "player") {
                return;
            }
            ++deaths;
            const Transform& point = *world.findByTag("spawn")->get<Transform>();
            other.get<Transform>()->x = point.x;
            other.get<Transform>()->y = point.y;
            other.get<Motion>()->velocityX = 0.0F;
            other.get<Motion>()->velocityY = 0.0F;
        });

    GameObject& boundary = makeZone(world, "boundary", { 500.0F, 0.0F, 10.0F, 600.0F },
        [&](GameObject&, GameObject& other) {
            if (other.tag() == "player") {
                cameraX = other.get<Transform>()->x - 300.0F;
            }
        });

    // Settle on the ground.
    run(world, 60);
    const Transform& body = *player.get<Transform>();
    passed &= expect(nearlyEqual(body.y + body.height, 400.0F, 0.5F), "the player should land on the ground");

    // Jump: leaves the ground, then lands again.
    jumpPressed = true;
    world.update(frame(kStep));
    jumpPressed = false;
    passed &= expect(body.y + body.height < 400.0F, "jump should lift the player off the ground");
    run(world, 90);
    passed &= expect(nearlyEqual(body.y + body.height, 400.0F, 0.5F), "the player should land after a jump");

    // Walk right into the scroll boundary: the camera follows, nothing moves.
    axis = 1.0F;
    run(world, 150);
    passed &= expect(cameraX > 0.0F, "the scroll boundary should move the camera");
    passed &= expect(nearlyEqual(ground.get<Transform>()->x, 0.0F),
                     "scrolling should move the camera, not the scene");

    // Keep walking off the end of the ground until the death zone catches the
    // player, then let go of the key.
    for (int i = 0; i < 600 && deaths == 0; ++i) {
        world.update(frame(kStep));
    }
    axis = 0.0F;
    passed &= expect(deaths == 1, "falling into the death zone should be detected once");
    passed &= expect(nearlyEqual(body.x, 40.0F) && nearlyEqual(body.y, 300.0F),
                     "the death zone should teleport the player to the spawn point");
    run(world, 60);
    passed &= expect(nearlyEqual(body.y + body.height, 400.0F, 0.5F), "after respawning the player should land again");

    // Hidden objects carry no Renderable; visible ones do.
    passed &= expect(!world.findByTag("spawn")->has<Renderable>() && !deathZone.has<Renderable>() &&
                         !boundary.has<Renderable>(),
                     "spawn points, death zones and boundaries should not be drawable");
    passed &= expect(player.has<Renderable>() && ground.has<Renderable>(), "the player and ground should be drawable");

    return passed;
}

// Disjoint tags may update on different FrameWorkers threads; collisions and
// destruction then run on the main thread.
bool testParallelTags()
{
    bool passed = true;
    World world;

    for (int i = 0; i < 50; ++i) {
        makeMovingPlatform(world, PathMover::Linear{ 0.0F, static_cast<float>(i) * 40.0F, 100.0F, static_cast<float>(i) * 40.0F },
                           10.0F, 20.0F, 10.0F);
        GameObject& drifter = world.create("drifter");
        drifter.add<Motion>(5.0F, 0.0F);
        drifter.setActive(true);
    }

    FrameWorkers workers({
        [&world](float dt) { world.updateComponents(frame(dt), "platform"); },
        [&world](float dt) { world.updateComponents(frame(dt), "drifter"); },
    });

    for (int i = 0; i < 10; ++i) {
        workers.runFrame(0.1F);
        world.resolveCollisions();
        world.flushDestroyed();
    }

    for (GameObject* platform : world.findAllByTag("platform")) {
        passed &= expect(nearlyEqual(platform->get<Transform>()->x, 10.0F), "every platform should advance once per frame");
    }
    for (GameObject* drifter : world.findAllByTag("drifter")) {
        passed &= expect(nearlyEqual(drifter->get<Transform>()->x, 5.0F), "every drifter should advance once per frame");
    }

    return passed;
}

// destroy() from update() on two FrameWorkers at once: every request lands and
// is applied by the next flushDestroyed() on the main thread.
bool testParallelDestroy()
{
    World world;
    for (const char* tag : { "left", "right" }) {
        for (int i = 0; i < 200; ++i) {
            GameObject& doomed = world.create(tag);
            doomed.add<Behavior>([&world](GameObject& self, const FrameTime&) { world.destroy(self.id()); });
            doomed.setActive(true);
        }
    }

    FrameWorkers workers({
        [&world](float dt) { world.updateComponents(frame(dt), "left"); },
        [&world](float dt) { world.updateComponents(frame(dt), "right"); },
    });
    workers.runFrame(0.1F);
    world.flushDestroyed();

    return expect(world.objects().empty(), "every object destroyed from parallel updates should be removed");
}

} // namespace

int main()
{
    bool passed = true;

    passed &= testAttachAndRequire();
    passed &= testInactiveUntilActivated();
    passed &= testUpdateOrder();
    passed &= testDeferredDestroyAndLookup();
    passed &= testEnterAndExit();
    passed &= testCrateAndLedge();
    passed &= testStaticPlatforms();
    passed &= testMovingPlatforms();
    passed &= testPlayerRidesKinematicPlatform();
    passed &= testLevel();
    passed &= testParallelTags();
    passed &= testParallelDestroy();

    if (passed) {
        std::cout << "Object model tests passed.\n";
        return 0;
    }

    return 1;
}
