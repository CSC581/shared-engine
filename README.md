# Shared Engine, Milestone 2: Time and Networking Foundations

**Team:** Harsha Vardhan Puvvadi, Seojin Kim, Vanaja Agarwal · **CSC 581** · All five sections (1–5) are implemented.

This is our C++17 game engine. It uses SDL3 for the window, input and drawing, and ZeroMQ for networking. Each game keeps its own rules, and any networking goes through the engine. There are relevant tests that checks for each module. This README only covers Milestone 2. Entities, gravity, collision, input and scaling were done in Milestone 1 and are left out here.

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j 4 && ctest --test-dir build --output-on-failure
```

## What each section asked for and how we did it

| §   | Assignment asks for                                                                     | What we built                                                                                                                                                                                                                                                                  |
| --- | --------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 1   | A time system that can pause, speed up (2×) and slow down (0.5×)                        | A `Timeline` clock that counts time on top of another clock. The engine keeps three: **real time** (never stops), **game time** (pausable, scalable) and **loop count**. Objects move by "time since last frame" from game time, so pausing or scaling needs no change to them |
| 2   | A server with no window that keeps ≥3 separate client windows in sync                   | Each client moves its own player and sends the position to the server, and the server replies with everyone's latest positions. Clients can join at any time                                                                                                                   |
| 3   | Game loop split across ≥2 threads, safely                                               | `FrameWorkers` gives a game long-lived worker threads and waits for all of them each frame. Apex runs its platforms on one and its player on the other. The main thread keeps input, collisions and drawing. Shared objects are locked while being read or written             |
| 4   | A server where one slow client does not slow the others, without ZeroMQ's Router/Dealer | The server gives every client its own thread and connection. A slow or paused client only delays its own thread. Moving platforms are run by the server so all clients see them in the same place                                                                              |
| 5   | Peer-to-peer networking                                                                 | **Hybrid.** Players send their positions straight to each other, and a server (separate, or hosted inside one player's game) only supplies the moving platforms. Games switch between client-server and peer-to-peer with one setting                                          |

<!-- ### How to see each section

Each of us built a game on the engine, and each section can be checked by running one of them. The commands assume the build step above has been run.

| §   | Harsha: Apex Ascent                                                                                                                                                   | Seojin: Pokemon Hunter           | Vanaja: _game name_                  |
| --- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------ | ------------------------------------ |
| 1   | `./build/apex-ascent`, then `P` to pause and `1`/`2`/`3` for 0.5×/1×/2×                                                                                                  | _TODO (Seojin): command, what to do_ | _TODO (Vanaja): command, what to do_ |
| 2   | `./build/apex-network-server`, then `./build/apex-ascent --join` in three terminals                                                                                    | _TODO (Seojin)_                      | _TODO (Vanaja)_                      |
| 3   | `./build/apex-ascent`. Platforms and player physics run on two `FrameWorkers` threads (`ps -T -p $(pgrep apex-ascent)` lists them)                                    | _TODO (Seojin)_                      | _TODO (Vanaja)_                      |
| 4   | `./build/apex-network-server --rates`, join three clients, pause one with `P`. The others keep their rate                                                             | _TODO (Seojin)_                      | _TODO (Vanaja)_                      |
| 5   | `./build/apex-ascent --mode peer-to-peer --id 1 --port 7200 --host`, then `--mode peer-to-peer --id 2 --port 7202 --peer tcp://127.0.0.1:7200` (and `--id 3` likewise) | _TODO (Seojin)_                      | _TODO (Vanaja)_                      | -->

## Files for this milestone

```text
§1 Time
├── include/TimeSource.hpp, TimeUnits.hpp   src/RealTimeClock.cpp    "what time is it" interface, real clock, units
├── include/Timeline.hpp                    src/Timeline.cpp         the pausable, scalable clock
├── include/DeltaTimer.hpp, FrameTime.hpp   src/DeltaTimer.cpp       "time since last frame"
└── include/Engine.hpp                      src/Engine.cpp           owns the three clocks, builds FrameTime each frame
§2 Client-server
├── include/NetworkProtocol.hpp             src/NetworkProtocol.cpp  the messages clients and server exchange
├── include/WireFormat.hpp, Endpoint.hpp, ZmqMessage.hpp             message encoding, addresses, ZeroMQ send/receive
├── include/NetworkClient.hpp               src/NetworkClient.cpp    client: join, send position, receive world
└── include/NetworkServer.hpp               src/NetworkServer.cpp    server: who is connected, where everyone is
§3 Threads
├── include/Entity.hpp                      src/Entity.cpp           game objects, safe to update from several threads
└── include/FrameWorkers.hpp                src/FrameWorkers.cpp     splits a game's update across threads, one per task
§4 Asynchronous server
├── include/NetworkServerHost.hpp           src/NetworkServerHost.cpp  one thread per client, platform timer
└── include/SendPacer.hpp                   src/SendPacer.cpp        send rate follows game speed (0.5x halves, 2x doubles)
§5 Peer-to-peer
├── include/PeerProtocol.hpp                src/PeerProtocol.cpp     messages peers exchange
├── include/PeerSession.hpp                 src/PeerSession.cpp      players finding and talking to each other
├── include/WorldStateClient.hpp            src/WorldStateClient.cpp fetches only the moving platforms
└── include/Multiplayer.hpp                 src/Multiplayer.cpp      one simple API over both networking styles
```

## API reference

### Time (`Timeline.hpp`, `DeltaTimer.hpp`, `FrameTime.hpp`)

| Function                                | Parameter    | Type    | Meaning                                                                       |
| --------------------------------------- | ------------ | ------- | ----------------------------------------------------------------------------- |
| `Timeline(anchor, ticSize)`             | `anchor`     | clock   | The clock this one follows (real time, or another timeline)                   |
|                                         | `ticSize`    | integer | How many of the anchor's ticks make one of ours. Bigger = slower. Must be > 0 |
| `setScale(scale)`                       | `scale`      | decimal | Speed multiplier: `0.5` half speed, `1.0` normal, `2.0` double                |
| `pause()`, `unpause()`, `togglePause()` |              |         | Stop / resume the clock. Its reading never jumps backwards                    |
| `isPaused()`, `scale()`                 |              |         | Read back the pause state and speed, e.g. for the window title                |
| `DeltaTimer(source, maxDelta)`          | `maxDelta`   | integer | Longest gap one frame may report, so a freeze does not teleport objects       |
| `FrameTime` (given each frame)          | `dtSeconds`  | decimal | Game seconds since the last frame (0 while paused)                            |
|                                         | `gameTimeUs` | integer | Total game time so far, in microseconds                                       |

### Multiplayer (`Multiplayer.hpp`), what a game uses

A game fills in a `Config`, calls `Session::open(config, engine.realTime(), &engine.gameTime())`, then every frame calls
`update()` and `publishLocalPlayer(x, y, data)` and reads `remotePlayers()` and `platforms()`.
Network failures show up in `status()` instead of crashing.

| `Config` setting            | Default                | Meaning                                                                                                                           |
| --------------------------- | ---------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `mode`                      | `ClientServer`         | `ClientServer` (everyone talks to a server) or `PeerToPeer` (players talk directly)                                               |
| `serverEndpoint`            | `tcp://127.0.0.1:5555` | Server address. In peer-to-peer, where platforms come from (empty = no platforms)                                                 |
| `playerName`                | empty                  | Name other players see                                                                                                            |
| `sendIntervalGameTics`      | `0`                    | Game time between position sends (µs). `0` = send every call. Set it (Apex uses 1/30 s) and 0.5× halves, 2× doubles the send rate |
| `heartbeatIntervalRealTics` | `250 ms`               | Real time between sends while paused, so a paused player isn't timed out. Only used when pacing is on                             |
| `peerId`                    | `1`                    | Peer-to-peer only: this player's number. Each player needs a different one                                                        |
| `basePort`                  | `7100`                 | Peer-to-peer only: this player uses this port and the next one                                                                    |
| `advertiseHost`             | `127.0.0.1`            | Peer-to-peer only: this computer's address as others should dial it (LAN IP across machines)                                      |
| `bootstrapPeers`            | none                   | Peer-to-peer only: the address of any player already in the game                                                                  |

| `Session` call                   | Meaning                                                                                                                  |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `publishLocalPlayer(x, y, data)` | Sends your position. `data` is optional game info (health, score, …), up to 512 bytes                                    |
| `state()`                        | `Connecting`, `Ready` or `Failed`. When `Failed`, `status()` says why                                                    |
| `mode()`                         | The mode in use. `modeName()` and `parseMode()` convert it to and from text                                              |
| `remotePlayers()`                | Every other `Player`, with `id`, `name`, `x`, `y` and `data`                                                             |
| `platforms()`                    | Every server-owned `Platform`, with `id`, `x`, `y`, `width` and `height`. A game may use `id` to tell object kinds apart |

### Threads and send pacing (`FrameWorkers.hpp`, `SendPacer.hpp`)

| Function                                                          | Meaning                                                                                          |
| ----------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `FrameWorkers(tasks)`                                             | Starts one long-lived thread per task. Each task is a function taking `dt`                       |
| `runFrame(dt)`                                                    | Runs every task once in parallel and returns when all have finished                              |
| `SendPacer(&gameTime, gameInterval, realTime, heartbeatInterval)` | Paces sends on game time, with a real-time heartbeat while paused. `Session` uses one internally |
| `shouldSend()`                                                    | True when a send is due. Call it only when the send will really happen                           |

### Running a server (`NetworkServer.hpp`, `NetworkServerHost.hpp`)

Create `NetworkServerHost(serverConfig, hostConfig)`, then call `start()` and later `stop()`.

| `Config` Setting                     | Default        | Meaning                                                                                                                                                                    |
| ------------------------------------ | -------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ServerConfig.spawnPoints`           | none           | Where new players appear (`x`, `y`)                                                                                                                                        |
| `ServerConfig.platforms`             | none           | Moving platforms: start and end point, `speed` (80 units/s), `width` (96), `height` (20)                                                                                   |
| `ServerConfig.maxPlayers`            | `8`            | Most players allowed at once                                                                                                                                               |
| `ServerConfig.inactivityTimeoutTics` | 3 seconds      | A player silent this long is removed                                                                                                                                       |
| `HostConfig.mode`                    | `Dedicated`    | `Dedicated` uses one thread per client (§4). `Listen` answers everyone on one connection, so a slow client delays the others. Apex uses it to host platforms inside a game |
| `HostConfig.bindEndpoint`            | `tcp://*:5555` | Address and port the server listens on                                                                                                                                     |
| `HostConfig.advertiseHost`           | `127.0.0.1`    | This machine's address as clients should reach it                                                                                                                          |
| `HostConfig.tickInterval`            | 16 ms          | How often platforms move                                                                                                                                                   |
| `HostConfig.trafficLogInterval`      | off            | If set, prints each player's accepted position updates per second, once per interval. Needs `HostConfig.log` to be set                                                     |

A game that needs to change replies can skip the host and drive `NetworkServer(clock, serverConfig)`
itself. It calls `handle(message)` for each request and `update()` on a timer, and uses the
`encode…`/`decode…` functions in `NetworkProtocol.hpp` to read or rewrite messages. On the client
side, `NetworkClient(clock, endpoint)` offers `start()`, `poll()`, `submitPosition(x, y, data)`,
`state()` and `snapshot()` for a direct connection outside a `Session`.

### Client-server messages (`NetworkProtocol.hpp`), one reply per request

| Client sends        | With                                 | Server replies | With                                               |
| ------------------- | ------------------------------------ | -------------- | -------------------------------------------------- |
| `JOIN`              | secret token, name                   | `WELCOME`      | player id, private connection address, world state |
| `POSITION`          | id, token, counter, `x`, `y`, `data` | `SNAPSHOT`     | every player's position and every platform's       |
| `LEAVE`             | id, token                            | `GOODBYE`      | nothing                                            |
| `GET_WORLD`         | nothing (peer-to-peer)               | `WORLD_STATE`  | platform positions only                            |
| any invalid request |                                      | `ERROR`        | reason                                             |

## Design decisions

- **Two kinds of time.** Movement uses game time. Networking, input and drawing use real time,
  so a paused client still hears the "unpause" key and stays connected.
- **One thread per client.** A basic ZeroMQ request/reply connection handles one request at a
  time, so the public address only admits new players and gives each a private connection.
- **Games own their rules.** The server only stores and shares positions, so every team game reuses it.

<div style="page-break-before: always"></div>

## Appendix: how the pieces fit

**A. One frame and the clocks behind it (§1).** Each frame runs steps 1–5. Movement uses game
time, which the game can pause or scale. Networking keeps running on real time.

![Frame and time flow](docs/frame-time-flow.svg)

**B. Client-server (§2, §4).** A new client joins through the public socket and is handed its own
worker thread. After that it only talks to that worker. The server moves platforms on its own.

![Client-server flow](docs/client-server-flow.svg)

**C. Peer-to-peer, hybrid (§5).** A new peer asks any running peer for the list of players, then
sends its position straight to them. Platforms optionally come from a server.

![Peer-to-peer flow](docs/peer-hybrid-flow.svg)

<sub> **Disclaimer:** The team designed and wrote code, but also used LLMs such as Claude Sonnet 5 and ChatGPT 5.6 Terra to help restructure the code and refactor the components when in need.</sub>
