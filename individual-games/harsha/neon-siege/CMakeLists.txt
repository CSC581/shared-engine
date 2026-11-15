set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_library(neon-rules STATIC src/NeonRules.cpp)
target_include_directories(neon-rules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_executable(neon-rules-tests tests/NeonRulesTests.cpp)
target_link_libraries(neon-rules-tests PRIVATE neon-rules)
add_test(NAME neon-rules-tests COMMAND neon-rules-tests)

add_library(neon-gameplay STATIC src/NeonStorage.cpp src/NeonArena.cpp)
target_link_libraries(neon-gameplay PUBLIC neon-rules engine-geometry)
add_executable(neon-storage-tests tests/NeonStorageTests.cpp)
target_link_libraries(neon-storage-tests PRIVATE neon-gameplay)
add_test(NAME neon-storage-tests COMMAND neon-storage-tests)

add_executable(neon-arena-tests tests/NeonArenaTests.cpp)
target_link_libraries(neon-arena-tests PRIVATE neon-gameplay)
add_test(NAME neon-arena-tests COMMAND neon-arena-tests)
set_tests_properties(neon-arena-tests PROPERTIES TIMEOUT 60)

add_library(neon-ui STATIC src/NeonGame.cpp src/NeonGameRender.cpp)
target_link_libraries(neon-ui PUBLIC neon-gameplay engine)
add_executable(neon-siege src/neonSiege.cpp)
target_link_libraries(neon-siege PRIVATE neon-ui)
add_executable(neon-ui-tests tests/NeonUiTests.cpp)
target_link_libraries(neon-ui-tests PRIVATE neon-ui)
add_test(NAME neon-ui-tests COMMAND neon-ui-tests)
add_test(NAME neon-smoke COMMAND neon-siege --smoke)
set_tests_properties(neon-ui-tests neon-smoke PROPERTIES TIMEOUT 60
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software")

add_executable(neon-allocation-probe-tests tests/AllocationProbeTests.cpp benchmarks/AllocationProbe.cpp)
target_include_directories(neon-allocation-probe-tests PRIVATE benchmarks)
add_test(NAME neon-allocation-probe-tests COMMAND neon-allocation-probe-tests)

foreach(neon_memory_kind IN ITEMS benchmark profile)
    add_executable(neon-siege-memory-${neon_memory_kind} benchmarks/NeonMemoryBenchmark.cpp)
    target_include_directories(neon-siege-memory-${neon_memory_kind} PRIVATE "${CMAKE_SOURCE_DIR}/benchmarks")
    target_link_libraries(neon-siege-memory-${neon_memory_kind} PRIVATE neon-gameplay)
    add_test(NAME neon-memory-${neon_memory_kind}-quick
        COMMAND neon-siege-memory-${neon_memory_kind} --quick
            --runs "${CMAKE_BINARY_DIR}/neon-memory-${neon_memory_kind}-quick-runs.csv"
            --summary "${CMAKE_BINARY_DIR}/neon-memory-${neon_memory_kind}-quick-summary.csv")
    set_tests_properties(neon-memory-${neon_memory_kind}-quick PROPERTIES TIMEOUT 60)
endforeach()
target_sources(neon-siege-memory-profile PRIVATE benchmarks/AllocationProbe.cpp)
target_compile_definitions(neon-siege-memory-profile PRIVATE NEON_ALLOCATION_PROFILE)
