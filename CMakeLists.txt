cmake_minimum_required(VERSION 3.16)

project(shared-engine LANGUAGES CXX)

# ZeroMQ (C) + cppzmq (C++ headers / cmake target)
set(ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)          # libzmq sometimes uses this
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
set(WITH_DOCS OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(vendored/libzmq EXCLUDE_FROM_ALL)

# cppzmq's CMake does find_package(ZeroMQ). When libzmq is already in-tree,
# recent cppzmq also detects TARGET libzmq / libzmq-static. If configure
# still fails, set these before add_subdirectory(cppzmq):
#   set(ZeroMQ_FOUND ON)
#   set(ZeroMQ_LIBRARY libzmq)          # or libzmq-static
#   set(ZeroMQ_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendored/libzmq/include")

add_subdirectory(vendored/cppzmq EXCLUDE_FROM_ALL)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# Example: only for a networked game / tool
target_link_libraries(some-target PRIVATE cppzmq)
# cppzmq usually pulls libzmq transitively; if not:
# target_link_libraries(some-target PRIVATE cppzmq libzmq)