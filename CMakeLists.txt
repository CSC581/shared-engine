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

# SDL-free time module (clocks, timelines, per-consumer deltas). Its own
# library rather than a folder inside engine-geometry: the rule that it must
# never depend on SDL is then enforced by the link line rather than by care,
# and a headless server can link the clocks on their own.
add_library(engine-time STATIC
    src/RealTimeClock.cpp
    src/Timeline.cpp
    src/DeltaTimer.cpp
)

target_include_directories(engine-time PUBLIC include)

find_package(Threads REQUIRED)
target_link_libraries(engine-time PUBLIC Threads::Threads)

# SDL-free geometry code (Entity, Physics, Collision), so tests built on it
# stay independently testable with no SDL dependency.
add_library(engine-geometry STATIC
    src/Entity.cpp
    src/Physics.cpp
    src/Collision.cpp
)

target_include_directories(engine-geometry PUBLIC include)

# Entity::update takes a FrameTime, so the clocks come with the geometry. Still
# no SDL on this link line.
target_link_libraries(engine-geometry PUBLIC engine-time)

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

# The whole time story in one binary: Timeline and DeltaTimer semantics, the
# concurrent case, and the seam with the pre-existing Entity/Game API, where
# the FrameTime overloads sit beside the float ones rather than replacing them.
# Links engine rather than engine-time because that last group needs Entity and
# Game; engine-time itself still has no SDL on its own link line.
add_executable(timeline-tests tests/TimelineTests.cpp)
target_link_libraries(timeline-tests PRIVATE engine)

add_test(NAME timeline-tests COMMAND timeline-tests)

# Verifies the vendored ZeroMQ import stays wired up. Links cppzmq-static so
# the test binary is self-contained and does not depend on a shared libzmq
# sitting next to it (which Windows, having no rpath, would not find).
add_executable(zmq-smoke-tests tests/ZmqSmokeTests.cpp)
target_link_libraries(zmq-smoke-tests PRIVATE cppzmq-static)

add_test(NAME zmq-smoke-tests COMMAND zmq-smoke-tests)

# Dear ImGui, for the timeline sandbox and nothing else. Fetched rather than
# vendored as a submodule: it is a development tool for one target, not a
# dependency of the engine or of any game, and the engine library must never
# link it. ImGui ships no CMakeLists of its own, so the static library is
# declared here from its sources.
include(FetchContent)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# The same SDL3 target the engine uses: never a second copy of SDL.
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# Interactive bench for the time module. Implements Game like any other game,
# with no privileged access to the engine.
add_executable(timeline-sandbox sandbox/TimelineSandbox.cpp)
target_link_libraries(timeline-sandbox PRIVATE engine imgui)

# Apex Ascent executable
add_executable(apex-ascent individual-games/harsha/apexAscent.cpp)
target_link_libraries(apex-ascent PRIVATE engine)

# Golf executable. Also builds standalone through
# individual-games/skim253/build.sh, which predates this target; the target
# exists so a clean configure covers this game like every other one.
add_executable(golf-game individual-games/skim253/main.cpp)
target_link_libraries(golf-game PRIVATE engine)

# Starfall Salvage executable
add_executable(starfall-salvage
    individual-games/vanaja/starfall-salvage/src/main.cpp
    individual-games/vanaja/starfall-salvage/src/StarfallSalvageGame.cpp
)

target_include_directories(starfall-salvage PRIVATE
    individual-games/vanaja/starfall-salvage/include
)

target_link_libraries(starfall-salvage PRIVATE engine)
