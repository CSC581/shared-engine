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
