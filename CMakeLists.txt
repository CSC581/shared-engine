cmake_minimum_required(VERSION 3.16)

project(shared-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(vendored/SDL EXCLUDE_FROM_ALL)

add_executable(shared-engine
    src/main.cpp
    src/Engine.cpp
)

target_include_directories(shared-engine PRIVATE include)
target_link_libraries(shared-engine PRIVATE SDL3::SDL3)
