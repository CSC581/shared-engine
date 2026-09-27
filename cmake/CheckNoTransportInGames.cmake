# Fails if any individual game talks to ZeroMQ directly.
#
# Networking mechanics (sockets, handshakes, per-client workers, hosting) are
# engine features: games reach them through Multiplayer::Session and
# Network::NetworkServerHost and supply only configuration. A game that
# includes ZeroMQ has almost always copied engine or sandbox code, and the
# copy drifts from the engine the first time the protocol changes.
#
# Run by CTest as:
#   cmake -DGAMES_DIR=<repo>/individual-games -P CheckNoTransportInGames.cmake
#
# Only code is matched, not prose: a comment that says "ZeroMQ" is fine,
# `zmq::socket_t` or `#include <zmq.hpp>` is not.

if(NOT DEFINED GAMES_DIR OR NOT IS_DIRECTORY "${GAMES_DIR}")
    message(FATAL_ERROR "GAMES_DIR must name the individual-games directory (got '${GAMES_DIR}')")
endif()

set(transport_pattern "zmq::|zmq\\.h|zmq_addon\\.h|ZmqMessage\\.hpp|zmq_[a-z_]+\\(|socket_type::")

file(GLOB_RECURSE game_sources
    "${GAMES_DIR}/*.cpp" "${GAMES_DIR}/*.cc" "${GAMES_DIR}/*.cxx"
    "${GAMES_DIR}/*.hpp" "${GAMES_DIR}/*.hh" "${GAMES_DIR}/*.h" "${GAMES_DIR}/*.inl")

set(offenders "")
foreach(source IN LISTS game_sources)
    file(STRINGS "${source}" hits REGEX "${transport_pattern}")
    if(hits)
        file(RELATIVE_PATH shown "${GAMES_DIR}/.." "${source}")
        string(APPEND offenders "\n  ${shown}:")
        # file(STRINGS) escapes a ';' inside a line as '\;', so each list
        # element is still one source line; unescape it for display.
        foreach(hit IN LISTS hits)
            string(REPLACE "\\;" ";" hit "${hit}")
            string(STRIP "${hit}" hit)
            string(APPEND offenders "\n    ${hit}")
        endforeach()
    endif()
endforeach()

if(offenders)
    message(FATAL_ERROR
        "ZeroMQ used directly in individual-games/:${offenders}\n"
        "Use Multiplayer::Session / Network::NetworkServerHost from network-core, "
        "or add the missing capability to the engine instead of the game.")
endif()

list(LENGTH game_sources checked)
message(STATUS "No direct ZeroMQ use in ${checked} individual-game source files.")
