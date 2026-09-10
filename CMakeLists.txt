cmake_minimum_required(VERSION 3.16)

project(shared-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ZeroMQ (libzmq, C++ sources w/ C ABI) + cppzmq (header-only C++ binding).
# libzmq's own knobs, forced off so vendoring stays quiet and fast.
# NOTE: libzmq's test switch is BUILD_TESTS (generic name, ours is separate);
# it does not read a ZMQ_BUILD_TESTS variable.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Order matters: libzmq first so cppzmq sees TARGET libzmq in-tree and skips
# find_package(ZeroMQ) entirely (no system ZeroMQ install required).
add_subdirectory(vendored/libzmq EXCLUDE_FROM_ALL)
add_subdirectory(vendored/cppzmq EXCLUDE_FROM_ALL)

add_subdirectory(vendored/SDL EXCLUDE_FROM_ALL)

# SDL-free geometry code (Entity, Physics, Collision), so tests built on it
# stay independently testable with no SDL dependency.
add_library(engine-geometry STATIC
    src/Entity.cpp
    src/Physics.cpp
    src/Collision.cpp
)

target_include_directories(engine-geometry PUBLIC include)

# SDL-dependent engine core (window, renderer, main loop, input). Publicly
# links engine-geometry and SDL3 so any target linking `engine` gets both
# transitively with a single target_link_libraries call.
add_library(engine STATIC
    src/Engine.cpp
    src/Input.cpp
)

target_include_directories(engine PUBLIC include)
target_link_libraries(engine PUBLIC SDL3::SDL3 engine-geometry)

enable_testing()

add_executable(entity-physics-tests tests/EntityPhysicsTests.cpp)
target_link_libraries(entity-physics-tests PRIVATE engine-geometry)

add_test(NAME entity-physics-tests COMMAND entity-physics-tests)

add_executable(input-tests tests/InputTests.cpp)
target_link_libraries(input-tests PRIVATE engine)

add_test(NAME input-tests COMMAND input-tests)

add_executable(collision-tests tests/CollisionTests.cpp)
target_link_libraries(collision-tests PRIVATE engine-geometry)

add_test(NAME collision-tests COMMAND collision-tests)

# Verifies the vendored ZeroMQ import stays wired up. Links cppzmq-static so
# the test binary is self-contained and does not depend on a shared libzmq
# sitting next to it (which Windows, having no rpath, would not find).
add_executable(zmq-smoke-tests tests/ZmqSmokeTests.cpp)
target_link_libraries(zmq-smoke-tests PRIVATE cppzmq-static)

add_test(NAME zmq-smoke-tests COMMAND zmq-smoke-tests)

# Apex Ascent executable
add_executable(apex-ascent individual-games/harsha/apexAscent.cpp)
target_link_libraries(apex-ascent PRIVATE engine)

# Starfall Salvage executable
add_executable(starfall-salvage
    individual-games/vanaja/starfall-salvage/src/main.cpp
    individual-games/vanaja/starfall-salvage/src/StarfallSalvageGame.cpp
)

target_include_directories(starfall-salvage PRIVATE
    individual-games/vanaja/starfall-salvage/include
)

target_link_libraries(starfall-salvage PRIVATE engine)
