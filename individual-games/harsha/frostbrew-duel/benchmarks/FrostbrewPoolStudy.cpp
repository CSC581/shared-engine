#include "FrostbrewArena.hpp"
#include "Components.hpp"
#include "TimeSource.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace Frostbrew;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
struct Digest {
    std::uint64_t value = 14695981039346656037ULL;
    void add(std::uint64_t number) {
        for (int i = 0; i < 8; ++i) {
            value ^= number & 255;
            value *= 1099511628211ULL;
            number >>= 8;
        }
    }
    void addFloat(float number) {
        std::uint32_t bits;
        std::memcpy(&bits, &number, sizeof(bits));
        add(bits);
    }
};
struct Result {
    PoolUsage usage;
    std::uint64_t gameplay, visuals;
    std::size_t ticks, completed, resets;
};

Result run(std::size_t capacity, std::uint32_t seed, bool forced)
{
    ManualClock clock;
    EventManager events(clock);
    Arena arena(events, capacity, seed);
    Digest gameplay, visuals;
    std::size_t ticks = 0, completed = 0, resets = 0;
    const auto sample = [&] {
        gameplay.add(static_cast<unsigned>(arena.match().phase()));
        gameplay.add(arena.match().player());
        gameplay.add(arena.match().health(0));
        gameplay.add(arena.match().health(1));
        for (const auto& object : arena.world().objects()) {
            const auto* body = object->get<Transform>();
            auto& digest = object->tag() == "particle" ? visuals : gameplay;
            digest.add(object->tag() == "penguin" ? 1 : object->tag() == "flask" ? 2 : 3);
            digest.addFloat(body->x); digest.addFloat(body->y);
            if (const auto* motion = object->get<Motion>()) {
                digest.addFloat(motion->velocityX); digest.addFloat(motion->velocityY);
            }
        }
        gameplay.add(255); visuals.add(255);
    };
    constexpr std::array<std::array<Ingredient, 2>, 6> pairs{{
        {Ingredient::Ember, Ingredient::Ember}, {Ingredient::Ember, Ingredient::Stone},
        {Ingredient::Ember, Ingredient::Growth}, {Ingredient::Stone, Ingredient::Stone},
        {Ingredient::Stone, Ingredient::Growth}, {Ingredient::Growth, Ingredient::Growth}
    }};
    for (int shot = 0; shot < (forced ? 16 : 36); ++shot) {
        if (arena.match().phase() == Phase::GameOver) { arena.reset(seed + ++resets); }
        const auto& pair = pairs[forced ? 2 : shot % pairs.size()];
        arena.aim(static_cast<float>(15 + (shot * 17 + seed % 71) % 71),
                  static_cast<float>(200 + (shot * 73 + seed % 501) % 501));
        if (arena.fire(pair[0], pair[1])) {
            int steps = 0;
            while (arena.match().phase() == Phase::Resolving && steps++ < 4000) {
                if (forced) {
                    // Artificially synchronize real impacts; this is a stress fixture, not ordinary play.
                    for (auto* object : arena.world().findAllByTag("flask")) {
                        auto* body = object->get<Transform>();
                        body->x = 595;
                        body->y = arena.terrain().support(595, 10);
                        auto* motion = object->get<Motion>();
                        motion->velocityX = 0; motion->velocityY = 0;
                        object->get<Gravity>()->acceleration = 0;
                    }
                }
                clock.advance(4167);
                arena.update(FrameTime{Arena::Step, clock.now()});
                ++ticks;
                sample();
            }
            require(arena.match().phase() != Phase::Resolving, "Shot exceeded resolution limit");
            require(arena.projectileCount() == 0, "Resolved shot retained a projectile");
            ++completed;
        }
        for (const auto height : arena.terrain().heights()) gameplay.addFloat(height);
        for (std::size_t player = 0; player < 2; ++player)
            for (unsigned potion = 0; potion < 6; ++potion)
                gameplay.add(arena.match().discovered(player, static_cast<Potion>(potion)));
        require(arena.poolUsage().peakObjects <= capacity, "Occupancy exceeded capacity");
    }
    const auto usage = arena.poolUsage();
    require(usage.peakProjectiles <= 8 && usage.peakParticles <= 64, "Object class bound exceeded");
    require(usage.particleRejected == 0, "Particle reserve failed to protect allocation");
    if (capacity >= 83) {
        require(usage.primaryRejected == 0 && usage.secondaryRejected == 0, "Gameplay rejected at sufficient capacity");
        require(usage.particleReserveSkipped == 0, "Cosmetics curtailed at sufficient capacity");
    }
    return {usage, gameplay.value, visuals.value, ticks, completed, resets};
}
} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 3 && std::string(argv[1]) == "--output", "Usage: frostbrew-pool-study --output results.csv");
        std::ofstream csv(argv[2]);
        require(bool(csv), "Cannot open CSV output");
        csv << "workload,seed,capacity,slot_storage_bytes,peak_objects,peak_projectiles,peak_particles,primary_spawned,secondary_spawned,particles_spawned,primary_rejected,secondary_rejected,particle_limit_skipped,particle_reserve_skipped,particle_rejected,ticks,completed_shots,automatic_resets,gameplay_digest,visual_digest,gameplay_matches_128,visuals_match_128\n";
        for (const bool forced : {false, true}) for (const std::uint32_t seed : {7U, 42U, 2026U}) {
            const Result baseline = run(128, seed, forced);
            for (const std::size_t capacity : {128U, 96U, 83U, 82U, 81U, 80U, 74U, 64U, 32U, 10U, 3U, 2U}) {
                const auto result = capacity == 128 ? baseline : run(capacity, seed, forced);
                const auto& u = result.usage;
                const bool gameplayMatches = result.gameplay == baseline.gameplay && result.ticks == baseline.ticks &&
                    result.completed == baseline.completed && result.resets == baseline.resets;
                const bool visualsMatch = result.visuals == baseline.visuals;
                if (capacity >= 83) require(gameplayMatches && visualsMatch, "Sufficient capacity changed the workload");
                if (forced && capacity >= 83) require(u.peakObjects == 74, "Stress fixture did not reach the structural bound");
                if (capacity == 2) require(result.completed == 0, "Full penguin-only pool admitted a shot");
                if (capacity == 3) require(u.secondaryRejected > 0, "Tiny pool did not exercise child rejection");
                csv << (forced ? "forced-impact" : "varied-recipes") << ',' << seed << ',' << capacity << ','
                    << capacity * sizeof(GameObject) << ',' << u.peakObjects << ',' << u.peakProjectiles << ',' << u.peakParticles << ','
                    << u.primarySpawned << ',' << u.secondarySpawned << ',' << u.particlesSpawned << ','
                    << u.primaryRejected << ',' << u.secondaryRejected << ',' << u.particleLimitSkipped << ','
                    << u.particleReserveSkipped << ',' << u.particleRejected << ',' << result.ticks << ','
                    << result.completed << ',' << result.resets << ',' << result.gameplay << ',' << result.visuals << ','
                    << gameplayMatches << ',' << visualsMatch << '\n';
            }
        }
        csv.close();
        require(bool(csv), "CSV output failed");
        std::cout << "Frostbrew pool study: 72 conditions validated; " << argv[2] << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Frostbrew pool study: " << error.what() << '\n';
        return 1;
    }
}
