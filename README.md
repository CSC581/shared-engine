# Shared Engine, Milestone 3: Game Object Model and Networked Scene

**Team:** Harsha Vardhan Puvvadi, Seojin Kim, Vanaja Agarwal · **CSC 581**

This is our C++17 game engine. It uses SDL3 for the window, input and drawing, and ZeroMQ for networking. Each game keeps its own rules, and any networking goes through the engine. This README covers Milestone 3; the API reference at the end also covers the time and networking modules from Milestone 2.

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j 4 -- -k   # -k: keep going past the unported games
ctest --test-dir build --output-on-failure -E starfall
```

> The individual games and two sandbox tools (`timeline-sandbox`, `thread-loop-sandbox`) still use Milestone 2's `Entity` class and are being ported, so a full build currently reports errors for those targets only. Every engine library, the demo programs and all 13 engine test suites build and pass.

## Milestone 3 progress

| Part | Assignment asks for | Status | What we built |
| ---- | ------------------- | ------ | ------------- |
| 1A | A component-, property- or custom-based object model (no monolithic hierarchy) that new object types can be built from | Done | A `GameObject` is an id, a tag and the components it owns. A new kind of object is a new mix of components, not a new class. `World` owns all objects and runs each frame |
| 1B | The object model on every network endpoint; up to 4 clients; movement and platforms synced; disconnects handled | Done | The server keeps players and platforms as `GameObject`s. `NetworkWorldSync` mirrors each client's session into its `World`, removing a player's object when they leave or time out. Every endpoint is multithreaded and there is no player cap. Measured keyboard-to-other-screen latency: ~20 ms with 4 clients |
| 1C | A second network data format and performance experiments | Not started | |
| 2 | Individual games built on the engine | In progress | Games are being ported to the object model |

### Components

| Component | Gives an object | Notes |
| --------- | --------------- | ----- |
| `Transform` | position and size | every other component requires it |
| `Motion` | velocity, applied each frame on game time | `kinematic` objects are never pushed by collisions |
| `Gravity` | constant downward acceleration | per object, default 980 |
| `PathMover` | movement along a `Linear` (back and forth) or `Circular` path | drives `Motion`, so riders can read its velocity |
| `Collider` | collisions: `Solid` (blocks) or `Trigger` (reports only) | `onEnter` / `onCollide` (every frame) / `onExit` callbacks; layer masks |
| `Renderable` | drawn by `renderWorld()` as a colour or texture | `layer` sets draw order; no `Renderable` = hidden |
| `Behavior` | game logic each frame (e.g. reading the keyboard) | runs before movement |
| `NetworkIdentity` | which network player or platform the object stands for | used by the server and `NetworkWorldSync` |

| Part 2 object | Built from |
| ------------- | ---------- |
| Player | `Transform` + `Gravity` + `Collider` + `Renderable` + `Behavior` |
| Static platform | `Transform` + `Collider` + `Renderable` |
| Moving platform | the same + `PathMover` (`Linear` or `Circular`) |
| Spawn point (hidden) | `Transform` only |
| Death zone (hidden) | `Transform` + `Trigger` collider whose callback teleports to a spawn point |
| Side scrolling | a hidden trigger that moves the camera passed to `renderWorld()` |

### Try 1B

```bash
./build/network-server                              # headless server, no player limit
./build/multiplayer-demo --mode client-server       # in up to 4 terminals
```

Move with WASD. Each window shows the measured latency. Close one window and its player disappears from the others. `--mode peer-to-peer` runs the same game over the peer mesh (see `--help`).

### Files for Milestone 3

```text
1A Object model
├── include/Component.hpp, GameObject.hpp       src/GameObject.cpp     component base, object (add/get/require)
├── include/Components.hpp, Color.hpp           src/Components.cpp     the built-in components
├── include/World.hpp                           src/World.cpp          owns objects; update → collisions → destroy
└── include/Engine.hpp                          src/Engine.cpp         renderWorld(): draws Renderables by layer
1B Networked scene
├── include/NetworkServer.hpp                   src/NetworkServer.cpp  server scene kept in a World
├── include/NetworkWorldSync.hpp                src/NetworkWorldSync.cpp  mirrors a session into a client's World
└── include/Multiplayer.hpp                     src/Multiplayer.cpp    client-server session on its own network thread
```

### Design decisions

- **Composition, not inheritance.** Components declare what they need with `require<T>()` (e.g. `Gravity` pulls in `Motion` and `Transform`). Lookups are one array read per type.
- **Fixed update order.** Components run by priority (`Behavior` → `PathMover` → `Gravity` → `Motion`), so input, then velocity, then position, every frame. The sorted order is cached and rebuilt only when objects or components change.
- **The network protocol did not change.** Both ends now keep the scene in a `World`. Network threads never touch a `World`: the server works on it under its lock, and clients hand data over through `Session::update()` on the main thread.
- **Platforms move on the server's tick** (60 Hz), not once per client request, so the server's work does not grow with the number of clients.
- **Threads:** `World::destroy()` is safe from any thread. Creating objects is main-thread only.

**Known limitations:** only players and platforms are synced; remote objects are not smoothed between updates; a player standing on a sideways-moving platform is not carried; collisions check every pair of objects.

<div style="page-break-before: always"></div>

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
| `ServerConfig.platforms`             | none           | Moving platforms: start and end point (or `shape = Circular` with a centre and `radius`), `speed` (80 units/s), `width` (96), `height` (20)                               |
| `ServerConfig.maxPlayers`            | `0`            | Most players allowed at once; `0` means no limit                                                                                                                           |
| `ServerConfig.inactivityTimeoutTics` | 3 seconds      | A player silent this long is removed                                                                                                                                       |
| `HostConfig.mode`                    | `Dedicated`    | `Dedicated` uses one thread per client. `Listen` answers everyone on one connection, so a slow client delays the others. Apex uses it to host platforms inside a game |
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

<div style="page-break-before: always"></div>

## Appendix: how the pieces fit

**A. One frame and the clocks behind it.** Each frame runs steps 1–5. Movement uses game
time, which the game can pause or scale. Networking keeps running on real time.

![Frame and time flow](docs/frame-time-flow.svg)

**B. Client-server.** A new client joins through the public socket and is handed its own
worker thread. After that it only talks to that worker. The server moves platforms on its own.

![Client-server flow](docs/client-server-flow.svg)

**C. Peer-to-peer, hybrid.** A new peer asks any running peer for the list of players, then
sends its position straight to them. Platforms optionally come from a server.

![Peer-to-peer flow](docs/peer-hybrid-flow.svg)

<sub> **Disclaimer:** The team designed and wrote code, but also used LLMs such as Claude Sonnet 5 and ChatGPT 5.6 Terra to help restructure the code and refactor the components when in need.</sub>
