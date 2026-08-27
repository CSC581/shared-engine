cmake_minimum_required(VERSION 3.16)

project(shared-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(vendored/SDL EXCLUDE_FROM_ALL)

add_executable(shared-engine
    src/main.cpp
    src/Engine.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(shared-engine PRIVATE include)
target_link_libraries(shared-engine PRIVATE SDL3::SDL3)

enable_testing()

add_executable(entity-physics-tests
    tests/EntityPhysicsTests.cpp
    src/Entity.cpp
    src/Physics.cpp
)

target_include_directories(entity-physics-tests PRIVATE include)

add_test(NAME entity-physics-tests COMMAND entity-physics-tests)
