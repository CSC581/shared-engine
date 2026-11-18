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
add_executable(frostbrew-pool-usage-tests tests/FrostbrewPoolUsageTests.cpp)
target_link_libraries(frostbrew-pool-usage-tests PRIVATE frostbrew-gameplay)
add_test(NAME frostbrew-pool-usage-tests COMMAND frostbrew-pool-usage-tests)

add_library(frostbrew-ui STATIC
    src/FrostbrewGame.cpp
    src/FrostbrewGameRender.cpp
)
target_link_libraries(frostbrew-ui PUBLIC frostbrew-gameplay engine)
add_executable(frostbrew-duel src/frostbrewDuel.cpp)
target_link_libraries(frostbrew-duel PRIVATE frostbrew-ui)
add_executable(frostbrew-ui-tests tests/FrostbrewUiTests.cpp)
target_link_libraries(frostbrew-ui-tests PRIVATE frostbrew-ui)
add_test(NAME frostbrew-ui-tests COMMAND frostbrew-ui-tests)
set_tests_properties(frostbrew-ui-tests PROPERTIES TIMEOUT 30)
foreach(frostbrew_target frostbrew-duel frostbrew-ui-tests)
    add_custom_command(TARGET ${frostbrew_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/individual-games/harsha/media/Spritesheets/Spritesheets
            $<TARGET_FILE_DIR:${frostbrew_target}>/media/frostbrew-duel
    )
endforeach()
add_test(NAME frostbrew-smoke COMMAND frostbrew-duel --smoke)
set_tests_properties(frostbrew-smoke PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software" TIMEOUT 60)
add_test(NAME frostbrew-assets-tests
    COMMAND ${CMAKE_COMMAND}
        "-DGAME=$<TARGET_FILE:frostbrew-duel>"
        "-DART=${CMAKE_SOURCE_DIR}/individual-games/harsha/media/Spritesheets/Spritesheets"
        "-DWORK=${CMAKE_BINARY_DIR}/frostbrew-assets-check"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/CheckFrostbrewAssets.cmake")
set_tests_properties(frostbrew-assets-tests PROPERTIES TIMEOUT 60)
