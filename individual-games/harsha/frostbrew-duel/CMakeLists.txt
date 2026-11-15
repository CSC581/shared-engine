set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Frostbrew Duel game rules; gameplay consumes the existing engine unchanged.
add_library(frostbrew-rules STATIC
    src/FrostbrewRules.cpp
    src/FrostbrewTerrain.cpp
)
target_include_directories(frostbrew-rules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_executable(frostbrew-rules-tests tests/FrostbrewRulesTests.cpp)
target_link_libraries(frostbrew-rules-tests PRIVATE frostbrew-rules)
add_test(NAME frostbrew-rules-tests COMMAND frostbrew-rules-tests)

add_library(frostbrew-gameplay STATIC src/FrostbrewArena.cpp)
target_link_libraries(frostbrew-gameplay PUBLIC frostbrew-rules engine-geometry)
add_executable(frostbrew-integration-tests tests/FrostbrewIntegrationTests.cpp)
target_link_libraries(frostbrew-integration-tests PRIVATE frostbrew-gameplay)
add_test(NAME frostbrew-integration-tests COMMAND frostbrew-integration-tests)
set_tests_properties(frostbrew-integration-tests PROPERTIES TIMEOUT 60)
