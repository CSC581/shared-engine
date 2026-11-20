#include "GardenDefenseMatch.hpp"
#include "GardenDefenseRules.hpp"

#include "Components.hpp"
#include "EventManager.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cmath>
#include <iostream>
#include <vector>

#define EXPECT(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

using namespace GardenDefense;

constexpr std::int64_t frameNs = kNsPerSec / loopHz;

// A match on a hand-stepped game timeline, run the way Engine::run runs it:
// update, then dispatch.
struct Harness {
    ManualClock real;
    Timeline gameTime{real, kNsPerUs};
    EventManager events{gameTime};
    Match match{events, 7};

    Harness() { events.dispatch(); }

    // `quiet` removes the seeds the pests drop, so a test is not decided by
    // where they happen to fall.
    void step(double seconds, bool quiet = true)
    {
        const auto frames = static_cast<int>(std::lround(seconds * static_cast<double>(loopHz)));
        for (int i = 0; i < frames; ++i) {
            if (quiet) {
                for (const auto& object : match.world().objects()) {
                    if (object->tag() == "seed") {
                        object->setActive(false);
                        match.world().destroy(object->id());
                    }
                }
            }
            const std::int64_t before = gameTime.now();
            real.advance(frameNs);
            const std::int64_t after = gameTime.now();
            match.update(FrameTime{static_cast<double>(after - before) / kGameTicsPerSecond, after});
            events.dispatch();
        }
    }

    // A seed right on top of the can.
    void seedOnCan()
    {
        const Transform& can = *match.can().get<Transform>();
        GameObject& seed = match.world().create("seed");
        seed.add<Transform>(can.x + 10.0F, can.y + 4.0F, seedWidth, seedHeight);
        seed.add<Collider>(Collider::Kind::Trigger, seedLayer);
        seed.setActive(true);
    }

    void killCan()
    {
        events.raise(Event(Events::Death).set("object", match.can().id()).set("cause", "seed"));
        events.dispatch();
    }

    void sprayEveryPest()
    {
        for (const auto& object : match.world().objects()) {
            if (object->isActive() && object->tag() == "pest") {
                events.raise(Event(Events::Death).set("object", object->id()).set("cause", "sprayed"));
            }
        }
        events.dispatch();
    }

    std::vector<Transform*> pests()
    {
        std::vector<Transform*> result;
        for (const auto& object : match.world().objects()) {
            if (object->isActive() && object->tag() == "pest") {
                result.push_back(object->get<Transform>());
            }
        }
        return result;
    }
};

int checkRules()
{
    EXPECT(pointsForRow(0) == 40);
    EXPECT(pointsForRow(rows - 1) == 10);

    // Moves sideways inside the margins.
    March step = marchStep(100.0F, 500.0F, 1.0F, 50.0F, 0.1F);
    EXPECT(step.dx == 5.0F && step.dy == 0.0F && step.direction == 1.0F);
    // Stops at the right margin, steps down and turns around.
    step = marchStep(300.0F, width - edgeMargin - 2.0F, 1.0F, 50.0F, 0.1F);
    EXPECT(std::abs(step.dx - 2.0F) < 1e-4F && step.dy == stepDown && step.direction == -1.0F);
    // The same at the left margin.
    step = marchStep(edgeMargin + 1.0F, 400.0F, -1.0F, 50.0F, 0.1F);
    EXPECT(std::abs(step.dx + 1.0F) < 1e-4F && step.dy == stepDown && step.direction == 1.0F);
    // A paused frame at the edge does not step down again.
    step = marchStep(300.0F, width - edgeMargin, 1.0F, 50.0F, 0.0F);
    EXPECT(step.dx == 0.0F && step.dy == 0.0F && step.direction == 1.0F);

    // Faster as pests fall, and faster again in wave 2.
    const int total = rows * columns;
    EXPECT(marchSpeed(total, total, 1) == baseMarchSpeed);
    EXPECT(marchSpeed(1, total, 1) > marchSpeed(total / 2, total, 1));
    EXPECT(marchSpeed(total, total, 2) > marchSpeed(total, total, 1));
    EXPECT(seedInterval(2, 0.5F) < seedInterval(1, 0.5F));
    return 0;
}

int checkStart()
{
    Harness h;
    EXPECT(h.match.world().objectCapacity() == objectCapacity);
    EXPECT(h.match.pestsAlive() == rows * columns);
    EXPECT(h.match.canUp());
    EXPECT(h.match.lives() == startingLives);
    EXPECT(h.match.state() == Match::State::Playing);
    return 0;
}

int checkMarchAndPause()
{
    Harness h;
    const float startX = h.pests().front()->x;
    h.step(0.5);
    EXPECT(h.pests().front()->x > startX);

    // While paused, game time stands still: nothing marches.
    h.gameTime.togglePause();
    const float pausedX = h.pests().front()->x;
    h.step(0.5);
    EXPECT(h.pests().front()->x == pausedX);
    h.gameTime.togglePause();

    // Long enough to reach an edge: the formation steps down.
    const float startY = h.pests().front()->y;
    h.step(8.0);
    EXPECT(h.pests().front()->y > startY);
    return 0;
}

int checkSprayScores()
{
    Harness h;
    // Keep one drop in the air until the first pest is hit.
    for (int i = 0; i < 6 * loopHz && h.match.score() == 0; ++i) {
        if (h.match.dropsInFlight() == 0) {
            h.match.fire();
        }
        h.step(1.0 / loopHz);
    }
    // One pest sprayed, worth its row's points.
    EXPECT(h.match.score() >= pointsForRow(rows - 1) && h.match.score() <= pointsForRow(0));
    EXPECT(h.match.score() % 10 == 0);
    EXPECT(h.match.pestsAlive() == rows * columns - 1);
    return 0;
}

int checkDropLimit()
{
    Harness h;
    for (int i = 0; i < maxDrops; ++i) {
        EXPECT(h.match.fire());
    }
    EXPECT(!h.match.fire());
    EXPECT(h.match.dropsInFlight() == maxDrops);
    return 0;
}

int checkSeedCostsLifeAndRespawns()
{
    Harness h;
    // A seed during the respawn shield is blocked.
    h.seedOnCan();
    h.step(1.0 / loopHz, false);
    EXPECT(h.match.lives() == startingLives && h.match.seedsBlocked() == 1);

    h.step(respawnShieldSeconds);
    h.seedOnCan();
    h.step(1.0 / loopHz, false);
    EXPECT(h.match.lives() == startingLives - 1);
    EXPECT(!h.match.canUp());

    // The SpawnEvent was raised with a delay on game time: pausing holds it.
    h.gameTime.togglePause();
    h.step(2.0);
    EXPECT(!h.match.canUp());
    h.gameTime.togglePause();
    h.step(respawnSeconds + 0.05);
    EXPECT(h.match.canUp());
    EXPECT(h.match.shieldSecondsLeft() > 0.0F);
    return 0;
}

int checkUmbrella()
{
    Harness h;
    h.step(respawnShieldSeconds + 0.1);
    EXPECT(h.match.openUmbrella());
    EXPECT(!h.match.openUmbrella());
    h.seedOnCan();
    h.step(1.0 / loopHz, false);
    EXPECT(h.match.lives() == startingLives && h.match.seedsBlocked() == 1);

    // Closed after umbrellaSeconds, then cooling down.
    h.step(umbrellaSeconds);
    EXPECT(!h.match.umbrellaOpen());
    EXPECT(!h.match.openUmbrella());
    h.seedOnCan();
    h.step(1.0 / loopHz, false);
    EXPECT(h.match.lives() == startingLives - 1);

    h.step(umbrellaCooldown);
    EXPECT(h.match.canUp());
    EXPECT(h.match.umbrellaCooldownLeft() == 0.0F);
    EXPECT(h.match.openUmbrella());
    return 0;
}

int checkWavesAndPool()
{
    Harness h;
    h.sprayEveryPest();
    EXPECT(h.match.pestsAlive() == 0);
    EXPECT(h.match.state() == Match::State::WaveCleared);
    EXPECT(h.match.score() == 8 * (40 + 30 + 20 + 10));
    // The bloom asks for more petals than the cosmetic budget allows.
    EXPECT(h.match.cosmeticsSkipped() > 0);
    EXPECT(h.match.world().objects().size() + gameplayReserve <= objectCapacity);

    // Wave 2 is built while the petals are still falling, and still fits:
    // the reserve kept its slots free.
    h.step(waveBreakSeconds + 0.05);
    EXPECT(h.match.state() == Match::State::Playing);
    EXPECT(h.match.wave() == 2);
    EXPECT(h.match.pestsAlive() == rows * columns);
    EXPECT(h.pests().front()->y >= formationTop + waveTwoDrop);
    for (int i = 0; i < maxDrops; ++i) {
        EXPECT(h.match.fire());
    }
    EXPECT(h.match.world().objects().size() <= objectCapacity);

    h.sprayEveryPest();
    EXPECT(h.match.state() == Match::State::Won);
    return 0;
}

int checkOverrun()
{
    Harness h;
    for (Transform* pest : h.pests()) {
        pest->y = gardenLineY - pestHeight + 2.0F;
    }
    h.step(1.0 / loopHz);
    EXPECT(h.match.state() == Match::State::Lost);
    EXPECT(h.match.lives() == 0);
    EXPECT(!h.match.canUp());
    return 0;
}

int checkRestartDropsStaleSpawn()
{
    Harness h;
    h.killCan();
    EXPECT(h.match.lives() == startingLives - 1);
    h.step(0.5);
    h.match.restart();
    h.events.dispatch();
    EXPECT(h.match.lives() == startingLives && h.match.canUp());
    EXPECT(h.match.pestsAlive() == rows * columns);

    // The first death's respawn, due now, belongs to the old match.
    h.killCan();
    h.step(0.6);
    EXPECT(!h.match.canUp());
    h.step(0.5);
    EXPECT(h.match.canUp());
    return 0;
}

} // namespace

int main()
{
    const int failures = checkRules() + checkStart() + checkMarchAndPause() + checkSprayScores() +
                         checkDropLimit() + checkSeedCostsLifeAndRespawns() + checkUmbrella() +
                         checkWavesAndPool() + checkOverrun() + checkRestartDropsStaleSpawn();
    if (failures != 0) {
        std::cerr << failures << " Garden Defense check(s) failed\n";
        return 1;
    }
    std::cout << "All Garden Defense checks passed\n";
    return 0;
}
