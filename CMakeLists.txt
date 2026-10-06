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

# SDL-free frame workers (Section 3): persistent threads that split a game's
# per-frame update, with a barrier so the main thread reads their results
# race-free. Its own library so a test can link it without SDL.
add_library(engine-threads STATIC
    src/FrameWorkers.cpp
)

target_include_directories(engine-threads PUBLIC include)
target_link_libraries(engine-threads PUBLIC Threads::Threads)

# SDL-free networking: protocol, session/state store, the host that puts that
# store on the network (handshake, per-client workers, shutdown), and the
# non-blocking ZeroMQ client adapter. A headless server can link this without
# the window engine.
add_library(network-core STATIC
    src/NetworkProtocol.cpp
    src/NetworkServer.cpp
    src/NetworkServerHost.cpp
    src/NetworkClient.cpp
    src/WorldStateClient.cpp
)

target_include_directories(network-core PUBLIC include)
# engine-geometry for PathMover::pingPong, the bounce rule the server's moving
# platforms share with the PathMover component.
target_link_libraries(network-core PUBLIC engine-time engine-geometry cppzmq-static)

# SDL-free peer-to-peer module (Section 5): peer protocol and mesh transport.
# A separate library from network-core rather than an addition to it — the two
# speak different protocols, and a peer links the server's client only when it
# wants a shared-world authority, which the hybrid design does.
add_library(peer-core STATIC
    src/PeerProtocol.cpp
    src/PeerSession.cpp
)

target_include_directories(peer-core PUBLIC include)
target_link_libraries(peer-core PUBLIC cppzmq-static Threads::Threads)

# One multiplayer interface over both networking architectures, so a game
# chooses between client-server and peer-to-peer by filling in a config rather
# than by being rewritten. Links both modules because it implements the same
# interface twice, once on each; a game links this instead of either.
add_library(multiplayer STATIC
    src/Multiplayer.cpp
    src/NetworkWorldSync.cpp
    src/SendPacer.cpp
)

target_include_directories(multiplayer PUBLIC include)
# engine-geometry for NetworkWorldSync, which mirrors a session into a World.
target_link_libraries(multiplayer PUBLIC network-core peer-core engine-geometry)

# SDL-free game object model (GameObject, Component, the built-in components,
# World) and the box geometry it collides with. No SDL on this link line, so a
# headless server and the tests can use the same objects as the clients.
add_library(engine-geometry STATIC
    src/Collision.cpp
    src/GameObject.cpp
    src/Components.cpp
    src/World.cpp
)

target_include_directories(engine-geometry PUBLIC include)

# Components update on a FrameTime, so the clocks come with the object model.
target_link_libraries(engine-geometry PUBLIC engine-time)

# SDL-dependent engine core (window, renderer, main loop, input). Publicly
# links engine-geometry and SDL3 so any target linking `engine` gets both
# transitively with a single target_link_libraries call.
add_library(engine STATIC
    src/Engine.cpp
    src/Input.cpp
)

target_include_directories(engine PUBLIC include)
target_link_libraries(engine PUBLIC SDL3::SDL3 engine-geometry engine-threads)

enable_testing()

# The object model: attaching and requiring components, update order, deferred
# destruction, and every Part 2 object built from components in a small level.
# Links engine-threads to check that disjoint tags update on FrameWorkers.
add_executable(object-model-tests tests/ObjectModelTests.cpp)
target_link_libraries(object-model-tests PRIVATE engine-geometry engine-threads)

add_test(NAME object-model-tests COMMAND object-model-tests)

add_executable(input-tests tests/InputTests.cpp)
target_link_libraries(input-tests PRIVATE engine)

add_test(NAME input-tests COMMAND input-tests)

add_executable(frame-workers-tests tests/FrameWorkersTests.cpp)
target_link_libraries(frame-workers-tests PRIVATE engine-threads)

add_test(NAME frame-workers-tests COMMAND frame-workers-tests)

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

add_executable(network-tests tests/NetworkTests.cpp)
target_link_libraries(network-tests PRIVATE network-core engine-geometry)

add_test(NAME network-tests COMMAND network-tests)

# The real NetworkServerHost on loopback: handshake -> private worker, rejoin,
# LEAVE reaping, GET_WORLD without joining, listen mode, and clean shutdown.
add_executable(network-server-host-tests tests/NetworkServerHostTests.cpp)
target_link_libraries(network-server-host-tests PRIVATE network-core)

add_test(NAME network-server-host-tests COMMAND network-server-host-tests)

# Guardrail: individual games reach networking through the engine
# (Multiplayer::Session, NetworkServerHost), never ZeroMQ directly. Copies of
# engine code in a game drift from it the first time the protocol changes.
add_test(NAME no-transport-in-games
    COMMAND ${CMAKE_COMMAND}
        "-DGAMES_DIR=${CMAKE_SOURCE_DIR}/individual-games"
        -P "${CMAKE_SOURCE_DIR}/cmake/CheckNoTransportInGames.cmake")

# Section 5: peer message parsing and a real three-peer TCP mesh.
add_executable(peer-tests tests/PeerTests.cpp)
target_link_libraries(peer-tests PRIVATE peer-core)

add_test(NAME peer-tests COMMAND peer-tests)

# The engine's multiplayer choice: the same game-level sequence of calls run
# against both architectures, so "a game can switch by changing a config" is
# checked rather than asserted.
add_executable(multiplayer-tests tests/MultiplayerTests.cpp)
target_link_libraries(multiplayer-tests PRIVATE multiplayer)

add_test(NAME multiplayer-tests COMMAND multiplayer-tests)

# Part 1B, client side: a session mirrored into a World. Players and platforms
# appear, follow the session and leave the scene when it stops reporting them.
add_executable(network-world-sync-tests tests/NetworkWorldSyncTests.cpp)
target_link_libraries(network-world-sync-tests PRIVATE multiplayer)

add_test(NAME network-world-sync-tests COMMAND network-world-sync-tests)

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

# Moodle ThreadExample (Project 2 Section 3 baseline): mutex + condition_variable
# + join, with no engine/SDL. Source kept under sandbox/ so docs/ stays reference.
add_executable(thread-example sandbox/ThreadExampleMain.cpp)
target_link_libraries(thread-example PRIVATE Threads::Threads)

# Section 3 M3/M4: Game loop with platform vs character worker threads
# (M3 spawn+join evolved into M4 persistent threads + CV barrier).
add_executable(thread-loop-sandbox sandbox/ThreadLoopSandbox.cpp)
target_link_libraries(thread-loop-sandbox PRIVATE engine Threads::Threads)

# Standalone Section 2 proof. These do not modify or build into any person's
# individual game: the server is headless and the client is a small SDL demo.
add_executable(network-server sandbox/NetworkServerMain.cpp)
target_include_directories(network-server PRIVATE sandbox)
target_link_libraries(network-server PRIVATE network-core Threads::Threads)

add_executable(network-client sandbox/NetworkClientDemo.cpp)
target_link_libraries(network-client PRIVATE engine network-core)

# The engine's multiplayer choice, demonstrated: one game, one --mode flag,
# either architecture. Links `multiplayer` and neither networking module
# directly, which is the point being made.
add_executable(multiplayer-demo sandbox/MultiplayerDemo.cpp)
target_link_libraries(multiplayer-demo PRIVATE engine multiplayer)

# Apex Ascent executable (gameplay + game-local animation module).
add_executable(apex-ascent
    individual-games/harsha/src/apexAscent.cpp
    individual-games/harsha/src/ApexGame.cpp
    individual-games/harsha/src/ApexGameRender.cpp
    individual-games/harsha/src/ApexOptions.cpp
    individual-games/harsha/src/ApexPresence.cpp
    individual-games/harsha/src/animation/PlayerAnimation.cpp
)
target_include_directories(apex-ascent PRIVATE individual-games/harsha/include)
target_link_libraries(apex-ascent PRIVATE engine multiplayer)

# Headless Apex world server (tower coords). Separate from sandbox network-server.
add_executable(apex-network-server individual-games/harsha/src/ApexNetworkServerMain.cpp)
target_include_directories(apex-network-server PRIVATE individual-games/harsha/include)
target_link_libraries(apex-network-server PRIVATE network-core Threads::Threads)

# Golf executable. Also builds standalone through
# individual-games/skim253/build.sh, which predates this target; the target
# exists so a clean configure covers this game like every other one.
add_executable(golf-game individual-games/skim253/main.cpp)
target_link_libraries(golf-game PRIVATE engine)

# Pokemon Hunt (Milestone 2): each client is a Pokemon, the headless server
# owns the monster.
add_executable(pokemon-hunt individual-games/skim253/pokemon-hunt/PokemonHunt.cpp)
target_link_libraries(pokemon-hunt PRIVATE engine multiplayer)
add_custom_command(TARGET pokemon-hunt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/individual-games/skim253/pokemon-hunt/media
        $<TARGET_FILE_DIR:pokemon-hunt>/media/pokemon-hunt
    COMMENT "Copying Pokemon Hunt sprites next to pokemon-hunt"
)

add_executable(pokemon-hunt-server individual-games/skim253/pokemon-hunt/PokemonHuntServer.cpp)
target_link_libraries(pokemon-hunt-server PRIVATE multiplayer engine-geometry Threads::Threads)

# Runtime art next to the binary so SDL_GetBasePath() + "media/apex-ascent/"
# works regardless of the caller's working directory.
add_custom_command(TARGET apex-ascent POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:apex-ascent>/media/apex-ascent
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/individual-games/harsha/media/Spritesheets/Spritesheets
        $<TARGET_FILE_DIR:apex-ascent>/media/apex-ascent
    COMMENT "Copying Apex Ascent player sprites next to apex-ascent"
)

# Starfall Salvage's cooperative Project 2 iteration. The server is headless
# and owns this game's shuttle and drone paths.
add_library(starfall-salvage-rules STATIC
    individual-games/vanaja/src/StarfallSalvageRules.cpp
)
target_include_directories(starfall-salvage-rules PUBLIC
    individual-games/vanaja/include
)
target_link_libraries(starfall-salvage-rules PUBLIC engine-geometry)

add_executable(starfall-salvage
    individual-games/vanaja/src/starfallSalvage.cpp
    individual-games/vanaja/src/StarfallSalvageGame.cpp
)
target_link_libraries(starfall-salvage PRIVATE engine multiplayer starfall-salvage-rules)

add_executable(starfall-salvage-server
    individual-games/vanaja/src/StarfallSalvageServer.cpp
)
target_link_libraries(starfall-salvage-server PRIVATE network-core)
target_include_directories(starfall-salvage-server PRIVATE
    individual-games/vanaja/include
)

add_executable(starfall-salvage-tests
    individual-games/vanaja/tests/StarfallSalvageTests.cpp
)
target_link_libraries(starfall-salvage-tests PRIVATE starfall-salvage-rules multiplayer)
add_test(NAME starfall-salvage-tests COMMAND starfall-salvage-tests)
