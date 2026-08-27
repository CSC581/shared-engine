cmake_minimum_required(VERSION 3.16)

project(shared-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(vendored/SDL EXCLUDE_FROM_ALL)

add_executable(shared-engine
    src/CoinRunner.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(shared-engine PRIVATE include)
target_link_libraries(shared-engine PRIVATE SDL3::SDL3)

# Test games: extra games on the same engine sources, as reusability checks.
# They are not part of the individual game submission.
#
# Exercises every engine system, including Physics.
add_executable(siege-game
    tests/SiegeGame.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(siege-game PRIVATE include)
target_link_libraries(siege-game PRIVATE SDL3::SDL3)

# Player dodges falling obstacles and catches falling coins.
add_executable(catch-game
    tests/CatchGame.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(catch-game PRIVATE include)
target_link_libraries(catch-game PRIVATE SDL3::SDL3)

# Fruit-Ninja-style slicer: keyboard-steered blade cuts tossed fruit, avoids bombs.
add_executable(slice-game
    tests/SliceGame.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(slice-game PRIVATE include)
target_link_libraries(slice-game PRIVATE SDL3::SDL3)

# Dung beetle's journey: jump the rocks, roll over the dung to grow the ball.
add_executable(beetle-game
    tests/BeetleGame.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(beetle-game PRIVATE include)
target_link_libraries(beetle-game PRIVATE SDL3::SDL3)

# Side-on golf on a course several screens wide, with a scrolling camera.
add_executable(golf-game
    tests/GolfGame.cpp
    src/Engine.cpp
    src/Input.cpp
    src/Collision.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(golf-game PRIVATE include)
target_link_libraries(golf-game PRIVATE SDL3::SDL3)

enable_testing()

add_executable(entity-physics-tests
    tests/EntityPhysicsTests.cpp
    src/Entity.cpp
    src/Physics.cpp
    src/Collision.cpp
)

target_include_directories(entity-physics-tests PRIVATE include)

add_test(NAME entity-physics-tests COMMAND entity-physics-tests)

add_executable(input-tests
    tests/InputTests.cpp
    src/Input.cpp
    src/Entity.cpp
    src/Collision.cpp
)

target_include_directories(input-tests PRIVATE include)
target_link_libraries(input-tests PRIVATE SDL3::SDL3)

add_test(NAME input-tests COMMAND input-tests)

add_executable(collision-tests
    tests/CollisionTests.cpp
    src/Collision.cpp
    src/Entity.cpp
)

target_include_directories(collision-tests PRIVATE include)

add_test(NAME collision-tests COMMAND collision-tests)
