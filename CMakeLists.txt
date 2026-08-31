cmake_minimum_required(VERSION 3.16)

project(shared-engine LANGUAGES CXX)

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

# Test games: extra games on the same engine sources, as reusability checks.
# They are not part of the individual game submission.
#
# Exercises every engine system, including Physics.
add_executable(siege-game games/SiegeGame.cpp)
target_link_libraries(siege-game PRIVATE engine)

# Player dodges falling obstacles and catches falling coins.
add_executable(catch-game games/CatchGame.cpp)
target_link_libraries(catch-game PRIVATE engine)

# Fruit-Ninja-style slicer: keyboard-steered blade cuts tossed fruit, avoids bombs.
add_executable(coin-game games/CoinRunner.cpp)
target_link_libraries(coin-game PRIVATE engine)

add_executable(slice-game games/SliceGame.cpp)
target_link_libraries(slice-game PRIVATE engine)

# Dung beetle's journey: jump the rocks, roll over the dung to grow the ball.
add_executable(beetle-game games/BeetleGame.cpp)
target_link_libraries(beetle-game PRIVATE engine)

# Side-on golf on a course several screens wide, with a scrolling camera.
add_executable(golf-game games/GolfGame.cpp)
target_link_libraries(golf-game PRIVATE engine)

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
