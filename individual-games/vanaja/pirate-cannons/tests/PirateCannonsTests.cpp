#include "PirateCannonsMatch.hpp"
#include "PirateCannonsRules.hpp"

#include "Components.hpp"
#include "EventManager.hpp"
#include "TimeSource.hpp"
#include "TimeUnits.hpp"
#include "Timeline.hpp"
#include "World.hpp"

#include <cmath>
#include <iostream>

#define EXPECT(condition) do { \
    if (!(condition)) { \
        std::cerr << "FAILED: " #condition " at line " << __LINE__ << '\n'; \
        return 1; \
    } \
} while (false)

namespace {

using namespace PirateCannons;

constexpr std::int64_t frameNs = kNsPerSec / loopHz;

// A match on a hand-stepped game timeline, run the way Engine::run runs it:
// update, then dispatch.
struct Harness {
    ManualClock real;
    Timeline gameTime{real, kNsPerUs};
    EventManager events{gameTime};
    Match match{events, 11};

    Harness() { events.dispatch(); }

    void step(double seconds)
    {
        const auto frames = static_cast<int>(std::lround(seconds * static_cast<double>(loopHz)));
        for (int i = 0; i < frames; ++i) {
            const std::int64_t before = gameTime.now();
            real.advance(frameNs);
            const std::int64_t after = gameTime.now();
            match.update(FrameTime{static_cast<double>(after - before) / kGameTicsPerSecond, after});
            events.dispatch();
        }
    }

    // Steps until `done` holds, for at most `seconds`.
    template <class Done>
    bool stepUntil(Done done, double seconds = 10.0)
    {
        for (int i = 0; i < static_cast<int>(seconds * static_cast<double>(loopHz)); ++i) {
            if (done()) {
                return true;
            }
            step(1.0 / static_cast<double>(loopHz));
        }
        return done();
    }

    // Scares the gulls off, so a test's shot is not decided by them.
    void calm()
    {
        for (const auto& object : match.world().objects()) {
            if (object->tag() == "gull") {
                object->setActive(false);
                match.world().destroy(object->id());
            }
        }
    }

    // Sets the player's cannon to `target` through the aiming controls.
    void aim(Shot target)
    {
        const float turn = target.angle - match.shot(Side::Player).angle;
        match.adjustAim(turn >= 0.0F ? 1.0F : -1.0F, 0.0F, std::abs(turn) / angleRate);
        const float push = target.power - match.shot(Side::Player).power;
        match.adjustAim(0.0F, push >= 0.0F ? 1.0F : -1.0F, std::abs(push) / powerRate);
    }

    // A cannonball fired by `shooter`, already touching `target`'s ship.
    void ballOn(Side target, Side shooter)
    {
        const Rect hull = shipBounds(target);
        GameObject& ball = match.world().create("ball");
        ball.add<Transform>(hull.x + hull.width * 0.5F, hull.y + 4.0F, ballSize, ballSize);
        ball.add<Collider>(Collider::Kind::Trigger, ballLayers);
        ball.add<Cannonball>(shooter, false);
        ball.setActive(true);
        step(1.0 / static_cast<double>(loopHz));
    }

    int count(const char* tag) const
    {
        int total = 0;
        for (const auto& object : match.world().objects()) {
            total += object->isActive() && object->tag() == tag ? 1 : 0;
        }
        return total;
    }
};

int checkRules()
{
    const Velocity right = launchVelocity({45.0F, 100.0F}, Side::Player);
    EXPECT(std::abs(right.x - 70.71F) < 0.01F && std::abs(right.y + 70.71F) < 0.01F);
    const Velocity left = launchVelocity({45.0F, 100.0F}, Side::Cpu);
    EXPECT(left.x < 0.0F && std::abs(left.x + right.x) < 1e-4F);

    const Shot clamped = clampShot({95.0F, 50.0F});
    EXPECT(clamped.angle == maxAngle && clamped.power == minPower);

    // With no wind and no island in the way, the flight matches the range
    // formula v^2 sin(2a) / g to within a frame's travel.
    const float x0 = 10.0F;
    const float y0 = 100.0F;
    const Velocity v = launchVelocity({45.0F, 300.0F}, Side::Player);
    const Flight flight = simulate(x0, y0, v, 0.0F, y0 + ballSize);
    const float range = 300.0F * 300.0F / gravity;
    EXPECT(!flight.hitIsland);
    EXPECT(std::abs((flight.x - x0) - range) < range * 0.02F);
    // A following wind carries the ball further.
    EXPECT(simulate(x0, y0, v, 40.0F, y0 + ballSize).x > flight.x);
    // A flat shot from the player's ship meets the island on its way down.
    EXPECT(simulate(muzzleX(Side::Player), muzzleY(), launchVelocity({15.0F, 450.0F}, Side::Player), 0.0F,
                    seaY).hitIsland);

    // The computer's best shot clears the island and comes down on target.
    const Rect hull = shipBounds(Side::Player);
    const float target = hull.x + hull.width * 0.5F;
    for (const float wind : {-maxWind, 0.0F, 35.0F}) {
        const Shot shot = aimAt(Side::Cpu, target, wind);
        const Flight aimed = simulate(muzzleX(Side::Cpu), muzzleY(), launchVelocity(shot, Side::Cpu), wind, shipY + 8.0F);
        EXPECT(!aimed.hitIsland);
        EXPECT(std::abs(aimed.x + ballSize * 0.5F - target) < hull.width * 0.5F);
    }

    EXPECT(aimError(0) == cpuStartError);
    EXPECT(aimError(1) < aimError(0));
    EXPECT(aimError(100) == cpuMinError);
    EXPECT(rollWind(0.0F) == -maxWind && rollWind(1.0F) == maxWind);
    EXPECT(plainShotAllowed(true, false) && plainShotAllowed(false, false));
    EXPECT(!plainShotAllowed(true, true));
    return 0;
}

int checkStart()
{
    Harness h;
    EXPECT(h.match.world().objectCapacity() == objectCapacity);
    EXPECT(h.match.round() == 1);
    EXPECT(h.match.turn() == Side::Player);
    EXPECT(h.match.state() == Match::State::Aiming);
    EXPECT(h.match.hp(Side::Player) == shipHp && h.match.hp(Side::Cpu) == shipHp);
    EXPECT(h.count("gull") == gulls);
    return 0;
}

int checkAimedShotHitsInTheWorld()
{
    // The rules' flight model and the real one (Behavior wind, Gravity,
    // Motion) agree well enough that an aimed shot lands.
    Harness h;
    h.calm();
    const Rect hull = shipBounds(Side::Cpu);
    h.aim(aimAt(Side::Player, hull.x + hull.width * 0.5F, h.match.wind()));
    EXPECT(h.match.fire(false));
    EXPECT(h.match.state() == Match::State::Flying);
    EXPECT(!h.match.fire(false));
    EXPECT(h.stepUntil([&] { return h.match.state() != Match::State::Flying; }));
    EXPECT(h.match.hp(Side::Cpu) == shipHp - 1);
    EXPECT(h.match.turn() == Side::Cpu && h.match.state() == Match::State::CpuAiming);
    return 0;
}

int checkCpuTurn()
{
    Harness h;
    h.calm();
    EXPECT(h.match.fire(false));
    EXPECT(h.stepUntil([&] { return h.match.state() == Match::State::CpuAiming; }));
    // The computer thinks, fires, and the turn comes back.
    h.step(cpuThinkSeconds * 0.5);
    EXPECT(h.match.state() == Match::State::CpuAiming);
    EXPECT(h.stepUntil([&] { return h.match.state() == Match::State::Flying; }));
    EXPECT(h.stepUntil([&] { return h.match.state() == Match::State::Aiming; }));
    EXPECT(h.match.turn() == Side::Player);
    // A miss counts towards the next aim's smaller error; a hit does not.
    EXPECT(h.match.cpuMisses() == (h.match.hp(Side::Player) == shipHp ? 1 : 0));
    return 0;
}

int checkRoundsAndPool()
{
    Harness h;
    for (int i = 0; i < shipHp; ++i) {
        h.ballOn(Side::Cpu, Side::Player);
    }
    EXPECT(h.match.state() == Match::State::RoundOver);
    EXPECT(h.match.roundsWon(Side::Player) == 1);
    EXPECT(h.match.sinking() == Side::Cpu);
    // The sinking ship throws more splinters than the cosmetic budget.
    EXPECT(h.match.cosmeticsSkipped() > 0);
    EXPECT(h.match.world().objects().size() + gameplayReserve <= objectCapacity);

    // The next round waits on game time: pausing holds it.
    h.gameTime.togglePause();
    h.step(roundBreakSeconds + 1.0);
    EXPECT(h.match.round() == 1);
    h.gameTime.togglePause();
    h.step(roundBreakSeconds + 0.05);
    EXPECT(h.match.round() == 2);
    EXPECT(h.match.hp(Side::Cpu) == shipHp);
    EXPECT(h.match.turn() == Side::Cpu && h.match.state() == Match::State::CpuAiming);
    EXPECT(h.count("gull") == gulls);

    for (int i = 0; i < shipHp; ++i) {
        h.ballOn(Side::Cpu, Side::Player);
    }
    EXPECT(h.match.state() == Match::State::MatchOver);
    EXPECT(h.match.roundsWon(Side::Player) == roundsToWin);

    h.match.restart();
    h.events.dispatch();
    EXPECT(h.match.round() == 1 && h.match.roundsWon(Side::Player) == 0);
    EXPECT(h.match.state() == Match::State::Aiming);
    EXPECT(h.match.chainShotsLeft() == chainShotsPerMatch);
    return 0;
}

int checkChainShot()
{
    Harness h;
    h.calm();
    h.aim({60.0F, 450.0F});
    EXPECT(h.match.fire(true));
    EXPECT(h.match.chainShotsLeft() == chainShotsPerMatch - 1);
    EXPECT(h.match.ballsInFlight() == 1);
    // Splits in three once it starts to come down.
    EXPECT(h.stepUntil([&] { return h.match.ballsInFlight() == 3; }, 3.0));
    EXPECT(h.stepUntil([&] { return h.match.state() == Match::State::Aiming; }));

    EXPECT(h.match.fire(true));
    EXPECT(h.match.chainShotsLeft() == 0);
    EXPECT(h.stepUntil([&] { return h.match.state() == Match::State::Aiming; }));
    EXPECT(!h.match.fire(true));
    EXPECT(h.match.fire(false));
    return 0;
}

int checkRestartDropsStaleRound()
{
    Harness h;
    for (int i = 0; i < shipHp; ++i) {
        h.ballOn(Side::Cpu, Side::Player);
    }
    EXPECT(h.match.state() == Match::State::RoundOver);
    h.match.restart();
    h.events.dispatch();
    // The old match's round 2, due now, is dropped.
    h.step(roundBreakSeconds + 0.1);
    EXPECT(h.match.round() == 1);
    EXPECT(h.match.roundsWon(Side::Player) == 0);
    return 0;
}

} // namespace

int main()
{
    const int failures = checkRules() + checkStart() + checkAimedShotHitsInTheWorld() + checkCpuTurn() +
                         checkRoundsAndPool() + checkChainShot() + checkRestartDropsStaleRound();
    if (failures != 0) {
        std::cerr << failures << " Pirate Cannons check(s) failed\n";
        return 1;
    }
    std::cout << "All Pirate Cannons checks passed\n";
    return 0;
}
