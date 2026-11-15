#include "NeonStorage.hpp"
#include "BenchmarkSupport.hpp"
#ifdef NEON_ALLOCATION_PROFILE
#include "AllocationProbe.hpp"
#endif

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace {
constexpr double dt = 1.0 / 240;
struct Hit { Neon::Handle bullet, target; std::uint64_t birth; };
class Workload {
public:
    Workload(Neon::StorageMode mode, std::size_t capacity, bool burst)
        : storage(mode, {{0, 0, 12, 0, capacity * 3 / 4, 0, capacity / 4}}), interval_(burst ? 2 : 30)
    {
        hits_.reserve(capacity * 12);
        for (int i = 0; i < 12; ++i) storage.spawn(Neon::Kind::Enemy, {64.0F + i * 64, 160, 28, 20});
        storage.onContact = [this](Neon::Handle a, Neon::Handle b) {
            const auto* first = storage.get(a); const auto* second = storage.get(b);
            if (first && second && first->kind == Neon::Kind::PlayerShot && second->kind == Neon::Kind::Enemy)
                hits_.push_back({a, b, first->birth});
        };
    }
    void tick()
    {
        storage.beginStep();
        if (frame_ % interval_ == 0) {
            random_ = random_ * 1664525U + 1013904223U;
            const float x = 68.0F + (random_ % 12) * 64;
            storage.spawn(Neon::Kind::PlayerShot, {x, 500, 8, 12}, 0, -600, 1, 2);
            storage.spawn(Neon::Kind::Particle, {x, 500, 3, 3}, 35, -60, 1, 0.4);
        }
        storage.world().updateComponents({dt, frame_ * 1000000LL / 240});
        storage.world().resolveCollisions();
        std::sort(hits_.begin(), hits_.end(), [](const Hit& a, const Hit& b) {
            return std::tie(a.birth, a.target.slot) < std::tie(b.birth, b.target.slot);
        });
        for (const auto& hit : hits_) if (storage.get(hit.bullet) && storage.get(hit.target)) {
            storage.retire(hit.bullet); ++contacts;
        }
        hits_.clear();
        for (std::size_t i = 0; i < storage.slots().size(); ++i) {
            auto h = storage.handle(i); auto* slot = storage.get(h);
            if (!slot || slot->lifetime == 0) continue;
            slot->age += dt;
            if (slot->age + 1e-10 >= slot->lifetime || slot->body->y < -32 || slot->body->y > 752)
                storage.retire(h);
        }
        ++frame_;
    }
    std::uint64_t digest() const { return storage.digest() ^ (contacts * 1099511628211ULL) ^ random_; }
    Neon::Storage storage;
    std::uint64_t contacts = 0;
private:
    int interval_, frame_ = 0;
    std::uint32_t random_ = 42;
    std::vector<Hit> hits_;
};
void verifyParity(std::size_t capacity, bool burst, int frames)
{
    Workload heap(Neon::StorageMode::Heap, capacity, burst), pool(Neon::StorageMode::Pool, capacity, burst), reuse(Neon::StorageMode::Reuse, capacity, burst);
    for (int frame = 0; frame < frames; ++frame) {
        heap.tick(); pool.tick(); reuse.tick();
        if (heap.digest() != pool.digest() || heap.digest() != reuse.digest())
            throw std::runtime_error("Storage modes performed different logical work");
    }
    if (!burst && heap.storage.stats().rejected != 0) throw std::runtime_error("Normal workload exceeded capacity");
    if (burst && heap.storage.stats().rejected == 0 && capacity == 32) throw std::runtime_error("Burst did not exercise exhaustion");
}
}
int main(int argc, char** argv)
{
    try {
#ifdef NEON_ALLOCATION_PROFILE
        const auto options = Bench::parseOptions(argc, argv, "neon-siege-memory-profile", "C++ requested allocations; no timing evidence");
        Bench::Results results({"mode", "transient_capacity", "workload", "phase"},
            {"allocation_requests", "requested_bytes", "live_requested_bytes", "peak_requested_bytes", "accepted", "rejected", "objects_constructed", "components_constructed", "peak_active", "modeled_reserved_pool_slot_bytes"});
#else
        const auto options = Bench::parseOptions(argc, argv, "neon-siege-memory-benchmark", "Uninstrumented headless lifecycle update cost; not game FPS");
        Bench::Results results({"mode", "transient_capacity", "workload"},
            {"setup_us", "update_mean_us", "update_median_us", "update_p95_us", "update_p99_us", "accepted", "rejected", "objects_constructed", "components_constructed", "peak_active", "modeled_reserved_pool_slot_bytes"});
#endif
        const std::vector<std::size_t> capacities = options.quick ? std::vector<std::size_t>{32, 128} : std::vector<std::size_t>{32, 64, 128};
        const int repetitions = options.quick ? 1 : 5;
        const int warmup = options.quick ? 30 : 240;
        const int measured = options.quick ? 180 : 2400;
        const std::array<Neon::StorageMode, 3> modes{{Neon::StorageMode::Heap, Neon::StorageMode::Pool, Neon::StorageMode::Reuse}};
        std::map<std::pair<std::size_t, bool>, std::array<std::uint64_t, 4>> expected;
        for (auto capacity : capacities) for (bool burst : {false, true}) verifyParity(capacity, burst, 480);
        for (int rep = 1; rep <= repetitions; ++rep) for (auto capacity : capacities) for (bool burst : {false, true}) {
            for (int offset = 0; offset < 3; ++offset) {
                const auto mode = modes[(offset + rep - 1) % 3];
                const double modeledBytes = mode == Neon::StorageMode::Heap ? 0 : double((capacity + 12) * sizeof(GameObject));
                std::vector<std::string> key{Neon::modeName(mode), std::to_string(capacity), burst ? "burst" : "normal"};
                std::array<std::uint64_t, 4> finalWork{};
#ifdef NEON_ALLOCATION_PROFILE
                std::optional<Workload> trial;
                AllocationProbe::start();
                trial.emplace(mode, capacity, burst);
                const auto setup = AllocationProbe::counts();
                const auto setupStats = trial->storage.stats();
                for (int i = 0; i < warmup; ++i) trial->tick();
                const auto before = AllocationProbe::counts();
                const auto beforeStats = trial->storage.stats();
                AllocationProbe::resetPeak();
                for (int i = 0; i < measured; ++i) trial->tick();
                const auto after = AllocationProbe::counts();
                const auto afterStats = trial->storage.stats();
                finalWork = {{trial->digest(), trial->contacts, afterStats.accepted, afterStats.rejected}};
                Bench::sink = finalWork[0];
                trial.reset();
                const auto remaining = AllocationProbe::counts().live;
                AllocationProbe::stop();
                if (remaining != 0) throw std::runtime_error("Measured C++ requests remain live after teardown");
                key.push_back("setup");
                results.add(key, rep, {double(setup.requests), double(setup.bytes), double(setup.live), double(setup.peak),
                    double(setupStats.accepted), double(setupStats.rejected), double(setupStats.objectsConstructed),
                    double(setupStats.componentsConstructed), double(setupStats.peakActive), modeledBytes});
                key.back() = "steady";
                results.add(key, rep, {double(after.requests - before.requests), double(after.bytes - before.bytes),
                    double(after.live), double(after.peak), double(afterStats.accepted - beforeStats.accepted),
                    double(afterStats.rejected - beforeStats.rejected), double(afterStats.objectsConstructed - beforeStats.objectsConstructed),
                    double(afterStats.componentsConstructed - beforeStats.componentsConstructed), double(afterStats.peakActive), modeledBytes});
#else
                std::vector<double> durations; durations.reserve(measured);
                const auto start = Bench::nowNs();
                Workload trial(mode, capacity, burst);
                const double setupUs = (Bench::nowNs() - start) / 1e3;
                for (int i = 0; i < warmup; ++i) trial.tick();
                const auto before = trial.storage.stats();
                for (int i = 0; i < measured; ++i) {
                    const auto begin = Bench::nowNs(); trial.tick();
                    durations.push_back(double(Bench::nowNs() - begin));
                }
                Bench::sink = trial.digest();
                const auto after = trial.storage.stats();
                finalWork = {{trial.digest(), trial.contacts, after.accepted, after.rejected}};
                results.add(key, rep, {setupUs, Bench::mean(durations) / 1e3, Bench::percentile(durations, 50) / 1e3,
                    Bench::percentile(durations, 95) / 1e3, Bench::percentile(durations, 99) / 1e3,
                    double(after.accepted - before.accepted), double(after.rejected - before.rejected),
                    double(after.objectsConstructed - before.objectsConstructed), double(after.componentsConstructed - before.componentsConstructed),
                    double(after.peakActive), modeledBytes});
#endif
                const auto cell = std::make_pair(capacity, burst);
                const auto found = expected.find(cell);
                if (found != expected.end() && found->second != finalWork)
                    throw std::runtime_error("Full-duration trials performed different work");
                expected[cell] = finalWork;
            }
        }
        results.write(options); results.print();
    } catch (const std::exception& e) { std::cerr << "Neon memory study: " << e.what() << '\n'; return 1; }
}
