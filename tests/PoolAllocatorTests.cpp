#include "PoolAllocator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what, int line)
{
    if (!ok) {
        std::cerr << "line " << line << ": " << what << '\n';
        ++failures;
    }
}
#define CHECK(x) check((x), #x, __LINE__)

template <class Exception, class Action>
void throws(Action action)
{
    bool expected = false;
    try {
        action();
    } catch (const Exception&) {
        expected = true;
    } catch (...) {
    }
    CHECK(expected);
}

struct Counts {
    int attempts = 0;
    int constructed = 0;
    int destroyed = 0;
};

struct ConstructionFailure : std::runtime_error {
    ConstructionFailure() : std::runtime_error("constructor failed") {}
};

struct Tracked {
    Counts& counts;
    int value;

    Tracked(Counts& counts, int value, bool fail = false) : counts(counts), value(value)
    {
        ++counts.attempts;
        if (fail) {
            throw ConstructionFailure{};
        }
        ++counts.constructed;
    }
    ~Tracked() noexcept { ++counts.destroyed; }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&&) = delete;
    Tracked& operator=(Tracked&&) = delete;
};

struct MoveOnlyArgument {
    std::unique_ptr<int> value;
    explicit MoveOnlyArgument(std::unique_ptr<int> value) : value(std::move(value)) {}
};

struct Bytes {
    unsigned char values[3]{};
};

struct alignas(256) Wide {
    int value = 17;
};

static_assert(!std::is_copy_constructible_v<PoolAllocator<int>>);
static_assert(!std::is_copy_assignable_v<PoolAllocator<int>>);
static_assert(!std::is_move_constructible_v<PoolAllocator<int>>);
static_assert(!std::is_move_assignable_v<PoolAllocator<int>>);
static_assert(std::is_nothrow_destructible_v<PoolAllocator<Tracked>>);

void reservationDoesNotConstructObjects()
{
    Counts counts;
    {
        const PoolAllocator<Tracked> pool(7);
        CHECK(pool.capacity() == 7);
        CHECK(pool.size() == 0);
        CHECK(counts.attempts == 0);
    }
    CHECK(counts.destroyed == 0);
}

void capacityOneAndExhaustion()
{
    Counts counts;
    PoolAllocator<Tracked> pool(1);
    Tracked* first = pool.create(counts, 42);
    CHECK(first != nullptr);
    if (first == nullptr) return;
    CHECK(first->value == 42);
    CHECK(pool.size() == 1);
    CHECK(pool.create(counts, 99) == nullptr);
    CHECK(pool.create(counts, 99, true) == nullptr);
    CHECK(counts.attempts == 1);
    CHECK(pool.capacity() == 1);
    CHECK(pool.size() == 1);
    CHECK(first->value == 42);
    pool.destroy(first);
    CHECK(pool.size() == 0);
    CHECK(counts.destroyed == 1);
}

void everySlotIsDistinctAndStable()
{
    for (const std::size_t capacity : {2U, 7U, 64U}) {
        PoolAllocator<int> pool(capacity);
        std::vector<int*> objects;
        std::set<int*> unique;
        for (std::size_t i = 0; i < capacity; ++i) {
            int* object = pool.create(static_cast<int>(i));
            CHECK(object != nullptr);
            if (object == nullptr) return;
            objects.push_back(object);
            CHECK(unique.insert(object).second);
            CHECK(pool.size() == i + 1);
            for (std::size_t j = 0; j <= i; ++j) {
                CHECK(*objects[j] == static_cast<int>(j));
            }
        }
        CHECK(pool.create(123) == nullptr);
    }
}

void defaultConstructionAndMoveOnlyArguments()
{
    PoolAllocator<int> integers(1);
    int* zero = integers.create();
    CHECK(zero != nullptr);
    if (zero != nullptr) CHECK(*zero == 0);

    PoolAllocator<MoveOnlyArgument> objects(1);
    auto argument = std::make_unique<int>(73);
    MoveOnlyArgument* object = objects.create(std::move(argument));
    CHECK(argument == nullptr);
    CHECK(object != nullptr);
    if (object != nullptr) CHECK(*object->value == 73);
    auto unconsumed = std::make_unique<int>(19);
    CHECK(objects.create(std::move(unconsumed)) == nullptr);
    CHECK(unconsumed != nullptr);
}

template <class T>
void checkAlignment()
{
    PoolAllocator<T> pool(5);
    std::vector<T*> objects;
    for (int i = 0; i < 5; ++i) {
        T* object = pool.create();
        CHECK(object != nullptr);
        if (object == nullptr) return;
        CHECK(reinterpret_cast<std::uintptr_t>(object) % alignof(T) == 0);
        objects.push_back(object);
    }
    CHECK(pool.create() == nullptr);
    for (T* object : objects) pool.destroy(object);
    for (int i = 0; i < 5; ++i) {
        T* object = pool.create();
        CHECK(object != nullptr);
        if (object == nullptr) return;
        CHECK(reinterpret_cast<std::uintptr_t>(object) % alignof(T) == 0);
    }
}

void alignmentForSmallAndOveralignedTypes()
{
    checkAlignment<unsigned char>();
    checkAlignment<Bytes>();
    checkAlignment<long double>();
    checkAlignment<Wide>();
}

void tinySlotsDoNotOverwriteLiveNeighbors()
{
    PoolAllocator<unsigned char> bytes(5);
    PoolAllocator<Bytes> triples(5);
    std::vector<unsigned char*> single;
    std::vector<Bytes*> triple;
    for (unsigned char i = 0; i < 5; ++i) {
        auto* a = bytes.create(static_cast<unsigned char>(10 + i));
        auto* b = triples.create();
        CHECK(a != nullptr && b != nullptr);
        if (a == nullptr || b == nullptr) return;
        for (unsigned char j = 0; j < 3; ++j) b->values[j] = 20 + i * 3 + j;
        single.push_back(a);
        triple.push_back(b);
    }
    for (const int i : {1, 3}) {
        bytes.destroy(single[i]);
        triples.destroy(triple[i]);
    }
    const auto checkSurvivors = [&] {
        for (const int i : {0, 2, 4}) {
            CHECK(*single[i] == 10 + i);
            for (int j = 0; j < 3; ++j) CHECK(triple[i]->values[j] == 20 + i * 3 + j);
        }
    };
    checkSurvivors();
    for (int i = 0; i < 2; ++i) {
        CHECK(bytes.create(static_cast<unsigned char>(99)) != nullptr);
        CHECK(triples.create() != nullptr);
    }
    checkSurvivors();
}

void constructorFailureRollsBackSlot()
{
    Counts counts;
    {
        PoolAllocator<Tracked> pool(2);
        Tracked* survivor = pool.create(counts, 10);
        CHECK(survivor != nullptr);
        if (survivor == nullptr) return;
        for (int i = 0; i < 3; ++i) {
            throws<ConstructionFailure>([&] { pool.create(counts, 20, true); });
            CHECK(pool.size() == 1);
            CHECK(pool.capacity() == 2);
            CHECK(survivor->value == 10);
            CHECK(counts.destroyed == 0);
        }
        Tracked* replacement = pool.create(counts, 30);
        CHECK(replacement != nullptr);
        CHECK(pool.size() == 2);
        CHECK(counts.constructed == 2);
        CHECK(pool.create(counts, 40) == nullptr);
    }
    CHECK(counts.destroyed == 2);
}

void teardownDestroysOnlyLiveObjects()
{
    Counts counts;
    {
        PoolAllocator<Tracked> pool(4);
        auto* released = pool.create(counts, 1);
        CHECK(released != nullptr);
        if (released == nullptr) return;
        CHECK(pool.create(counts, 2) != nullptr);
        pool.destroy(released);
        CHECK(counts.destroyed == 1);
        CHECK(pool.create(counts, 3) != nullptr);
        CHECK(pool.size() == 2);
    }
    CHECK(counts.constructed == 3);
    CHECK(counts.destroyed == 3);
}

void repeatedCapacityOneReuse()
{
    Counts counts;
    {
        PoolAllocator<Tracked> pool(1);
        std::uintptr_t address = 0;
        for (int i = 0; i < 256; ++i) {
            Tracked* object = pool.create(counts, i);
            CHECK(object != nullptr);
            if (object == nullptr) return;
            if (i == 0) address = reinterpret_cast<std::uintptr_t>(object);
            CHECK(reinterpret_cast<std::uintptr_t>(object) == address);
            CHECK(object->value == i);
            pool.destroy(object);
            CHECK(pool.size() == 0);
            CHECK(counts.destroyed == i + 1);
        }
    }
    CHECK(counts.constructed == counts.destroyed);
}

void arbitraryReleaseOrderPreservesSurvivors()
{
    PoolAllocator<int> pool(5);
    std::vector<int*> objects;
    for (int i = 0; i < 5; ++i) {
        auto* object = pool.create(i);
        CHECK(object != nullptr);
        if (object == nullptr) return;
        objects.push_back(object);
    }
    for (const int i : {3, 0, 2}) pool.destroy(objects[i]);
    CHECK(pool.size() == 2);
    CHECK(*objects[1] == 1);
    CHECK(*objects[4] == 4);
    std::set<int*> live{objects[1], objects[4]};
    for (int i = 0; i < 3; ++i) {
        int* object = pool.create(50 + i);
        CHECK(object != nullptr);
        CHECK(live.insert(object).second);
    }
    CHECK(pool.size() == 5);
    CHECK(pool.create(99) == nullptr);
    CHECK(*objects[1] == 1 && *objects[4] == 4);
}

void invalidReleaseDoesNotCorruptPool()
{
    Counts counts;
    PoolAllocator<Tracked> pool(1);
    PoolAllocator<Tracked> other(1);
    Tracked stack(counts, 1);
    Tracked* own = pool.create(counts, 2);
    Tracked* foreign = other.create(counts, 3);
    CHECK(own != nullptr && foreign != nullptr);
    if (own == nullptr || foreign == nullptr) return;
    throws<std::invalid_argument>([&] { pool.destroy(nullptr); });
    throws<std::invalid_argument>([&] { pool.destroy(&stack); });
    throws<std::invalid_argument>([&] { pool.destroy(foreign); });
    CHECK(pool.size() == 1 && other.size() == 1);
    CHECK(own->value == 2 && foreign->value == 3 && stack.value == 1);
    CHECK(counts.destroyed == 0);
    pool.destroy(own);
    throws<std::invalid_argument>([&] { pool.destroy(own); });
    CHECK(counts.destroyed == 1);
    CHECK(pool.size() == 0);
    CHECK(pool.create(counts, 4) != nullptr);
    CHECK(pool.create(counts, 5) == nullptr);
}

void interiorPointerIsRejectedWithoutDereference()
{
    static_assert(alignof(Bytes) == 1 && sizeof(Bytes) >= 3);
    PoolAllocator<Bytes> pool(1);
    Bytes* object = pool.create();
    CHECK(object != nullptr);
    if (object == nullptr) return;
    auto* interior = reinterpret_cast<Bytes*>(reinterpret_cast<unsigned char*>(object) + 1);
    throws<std::invalid_argument>([&] { pool.destroy(interior); });
    CHECK(pool.size() == 1);
    object->values[0] = 9;
    CHECK(object->values[0] == 9);
    pool.destroy(object);
    CHECK(pool.size() == 0);
}

void invalidCapacitiesAreRejected()
{
    throws<std::invalid_argument>([] { PoolAllocator<int> pool(0); });
    constexpr auto impossible = std::numeric_limits<std::size_t>::max() / sizeof(Wide) + 1;
    throws<std::length_error>([] { PoolAllocator<Wide> pool(impossible); });
}

void independentPoolsDoNotShareCapacity()
{
    PoolAllocator<int> first(1);
    PoolAllocator<int> second(2);
    auto* a = first.create(1);
    auto* b = second.create(2);
    CHECK(a != nullptr && b != nullptr);
    if (a == nullptr || b == nullptr) return;
    CHECK(first.create(3) == nullptr);
    CHECK(second.create(4) != nullptr);
    first.destroy(a);
    CHECK(first.size() == 0 && second.size() == 2);
    CHECK(*b == 2);
}

void mixedOperationsMatchIndependentLiveSet()
{
    constexpr std::size_t capacity = 7;
    PoolAllocator<int> pool(capacity);
    std::vector<std::pair<int*, int>> live;
    std::mt19937 random(581);
    for (int step = 0; step < 1000; ++step) {
        if (live.empty() || random() % 3 != 0) {
            int* created = pool.create(step);
            if (live.size() == capacity) {
                CHECK(created == nullptr);
            } else {
                CHECK(created != nullptr);
                if (created == nullptr) return;
                live.emplace_back(created, step);
            }
        } else {
            const auto index = random() % live.size();
            pool.destroy(live[index].first);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        }
        CHECK(pool.size() == live.size());
        std::set<int*> addresses;
        for (const auto& [object, expected] : live) {
            CHECK(addresses.insert(object).second);
            CHECK(*object == expected);
        }
    }
    for (const auto& [object, expected] : live) {
        (void)expected;
        pool.destroy(object);
    }
    CHECK(pool.size() == 0);
    for (std::size_t i = 0; i < capacity; ++i) CHECK(pool.create(7) != nullptr);
    CHECK(pool.create(8) == nullptr);
}
} // namespace

int main()
{
    const std::pair<const char*, void (*)()> cases[] = {
        {"reservation does not construct", reservationDoesNotConstructObjects},
        {"capacity one and exhaustion", capacityOneAndExhaustion},
        {"distinct stable slots", everySlotIsDistinctAndStable},
        {"default construction and forwarding", defaultConstructionAndMoveOnlyArguments},
        {"small and over-aligned types", alignmentForSmallAndOveralignedTypes},
        {"tiny slots preserve live neighbors", tinySlotsDoNotOverwriteLiveNeighbors},
        {"constructor failure rollback", constructorFailureRollsBackSlot},
        {"teardown destroys live objects", teardownDestroysOnlyLiveObjects},
        {"repeated single-slot reuse", repeatedCapacityOneReuse},
        {"arbitrary release order", arbitraryReleaseOrderPreservesSurvivors},
        {"invalid release preserves state", invalidReleaseDoesNotCorruptPool},
        {"interior pointer rejection", interiorPointerIsRejectedWithoutDereference},
        {"invalid capacities", invalidCapacitiesAreRejected},
        {"independent pools", independentPoolsDoNotShareCapacity},
        {"mixed operations match live-set oracle", mixedOperationsMatchIndependentLiveSet},
    };
    for (const auto& [name, run] : cases) {
        const int before = failures;
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << name << ": " << error.what() << '\n';
            ++failures;
        }
        std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
    }
    return failures == 0 ? 0 : 1;
}
