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
