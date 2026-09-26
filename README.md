# Shared Engine

A reusable C++17 game-engine foundation built with SDL3 and ZeroMQ. The shared
code supplies a window and game loop, entities, physics, input, collision,
timekeeping, and networking. Applications supply their own rules and artwork.

A game implements `Game::handleInput()`, `Game::update()`, and `Game::render()`,
then passes itself to `Engine::run()`. The engine does not contain a particular
player, level, score, or win condition.

## Feature Map

### Project 2 (Milestone 2)

| Section | Shared-engine feature or demonstration | Main files |
| --- | --- | --- |
| 1. Time | Monotonic real-time source, pausable and rescalable timelines, per-consumer delta timers, and `FrameTime` for game movement. The engine also exposes a loop-iteration clock. | `include/TimeSource.hpp`, `include/Timeline.hpp`, `include/DeltaTimer.hpp`, `include/FrameTime.hpp`, `src/Engine.cpp` |
| 2. Networking | SDL-free protocol and nonblocking ZeroMQ client. A headless server keeps player sessions, positions, and server-owned moving platforms. | `include/NetworkProtocol.hpp`, `include/NetworkClient.hpp`, `include/NetworkServer.hpp`, `src/Network*.cpp` |
| 3. Multithreaded update example | A sandbox runs moving-platform and character updates on persistent worker threads, synchronizing each frame with a mutex and condition variable. This is a demonstration, not a thread scheduler inside `Engine::run()`. | `sandbox/ThreadLoopSandbox.cpp` |
| 4. Concurrent clients | The dedicated server gives joined players separate request/reply workers; one slow client need not block another. Demo clients can independently pause or scale their own game time. | `sandbox/NetworkServerMain.cpp`, `sandbox/MultiplayerDemo.cpp` |
| 5. Peer-to-peer and hybrid mode | Peers discover one another and send player state directly. An optional read-only world client obtains moving-platform state from a dedicated or player-hosted authority without joining its player roster. | `include/PeerSession.hpp`, `include/WorldStateClient.hpp`, `include/Multiplayer.hpp`, `src/PeerSession.cpp`, `src/WorldStateClient.cpp`, `src/Multiplayer.cpp` |

### Milestone 1 Foundations

| Task | Shared-engine feature | Main files |
| --- | --- | --- |
| 1. Engine setup and rendering | SDL3 initialization, a resizable window and renderer, the main loop, screen clear, frame presentation, and cleanup. The game chooses its window title and design size. | `include/Engine.hpp`, `src/Engine.cpp`, `include/Game.hpp` |
| 2. Entity | Position, dimensions, velocity, time-based movement, and a bounding rectangle. | `include/Entity.hpp`, `src/Entity.cpp` |
| 3. Physics | Configurable gravity applied only to entities selected by the game. | `include/Physics.hpp`, `src/Physics.cpp` |
| 4. Input | Polls the SDL keyboard once per frame and exposes held, just-pressed, and just-released keys, plus multi-key helpers. | `include/Input.hpp`, `src/Input.cpp` |
| 5. Collision | SDL-free rectangle overlap, intersection, separation, and resolution. The game decides what a collision means. | `include/Collision.hpp`, `src/Collision.cpp` |
| 6. Scaling | Constant/pixel and proportional/aspect-preserving rendering modes. `F1` toggles them by default; games may rebind or disable the key. | `include/Engine.hpp`, `src/Engine.cpp` |

## How The Pieces Fit

```text
Application -> Game + Engine -> SDL window, input, rendering, FrameTime
            -> Entity, Physics, Collision -> application-controlled movement
            -> Multiplayer::Session (optional)
               -> ClientServer: NetworkClient <-> NetworkServer
               -> PeerToPeer:  PeerSession <-> other peers
                               WorldStateClient <-> optional platform authority
```

`engine-time`, `engine-geometry`, `network-core`, and `peer-core` do not link
SDL. The windowed `engine` library adds SDL on top, while the dedicated
`network-server` uses the networking and time libraries without a window.
The threading example is a sandbox game using the ordinary `Game` interface;
it does not make every game multithreaded.

The public APIs are in `include/`, their implementations are in `src/`,
runnable examples are in `sandbox/`, and automated checks are in `tests/`.
For networking internals, see
[docs/networking-guide.md](docs/networking-guide.md).

## Build And Test

The SDL3 and ZeroMQ dependencies are Git submodules. From a new clone, run:

```bash
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

CMake also fetches Dear ImGui for the timeline sandbox during the first
configuration, so that first configure needs internet access unless the
dependency is already cached. Dear ImGui is not linked into the shared engine.
The test suite covers entities and physics, input, collision, timelines,
ZeroMQ, the network client/server, the peer mesh, and both multiplayer modes.

## Using The Engine

### Section 1: Time

`engine.realTime()` counts monotonic nanoseconds for networking and other
always-running work. `engine.gameTime()` is a timeline for simulation, and
`engine.loopTime()` counts loop iterations. The engine passes a `FrameTime` to
the game each frame with elapsed game seconds (`dtSeconds`) and absolute game
microseconds (`gameTimeUs`). `Entity::update()` accepts either `FrameTime` or a
float delta time, so movement can use elapsed time rather than a fixed distance
per frame.

Use `engine.gameTime().togglePause()` to pause the simulation and
`engine.gameTime().setScale(0.5)` or `engine.gameTime().setScale(2.0)` to
change its speed. While paused, game-time deltas become zero, but real time
keeps advancing so input, rendering, and network retries can continue.
`Timeline` also supports adjustable tic size and child timelines. Each
`DeltaTimer` measures elapsed tics for its own consumer and can cap a long
frame delta. `loopTime()` records iterations; it does not set the frame rate.

### Section 2: Client-Server Networking

`NetworkClient` connects to a headless `NetworkServer` over ZeroMQ. Start the
client and call `poll()` each frame. It first sends `JOIN`; the server assigns
a player ID and returns an initial world snapshot. The client then uses
`submitPosition(x, y)` to report its locally calculated position, and
`snapshot()` provides the latest positions of connected players and moving
platforms. A demo client sends its position even when it is not moving so
the server knows it is still present.

`poll()` checks for replies without stopping the game loop. The server owns
membership and platform motion, but character controls and movement stay in
the client. Connection timeouts and retries use real time, so they still work
when the client's game timeline is paused.

### Section 3: Multithreaded Updates

`thread-loop-sandbox` demonstrates a loop with two persistent worker threads:
one updates a moving platform and the other updates a character. SDL input and
rendering stay on the main thread. At the start of a frame, the main thread
provides the frame delta and wakes both workers. It waits for both updates to
finish before resolving collisions and drawing the new positions. A mutex and
condition variable coordinate that work, and both workers are joined on
shutdown. This threaded loop is in the sandbox, not built into `Engine::run()`.

### Section 4: Concurrent Clients And Shared Platforms

The dedicated `network-server` accepts a player's `JOIN` on a public endpoint
and returns a private request/reply endpoint in `WELCOME`. Subsequent position
updates use that player's worker, so another player's slow exchange does not
hold up its replies. The server protects its shared session state while the
workers run concurrently.

A separate server thread advances moving platforms on real time about every
16 ms. Clients receive those platform positions in snapshots. In the
`multiplayer-demo`, each window can pause or scale its own game timeline with
`P` or `1`/`2`/`3` while continuing to poll the network and display the
server's platforms.

### Section 5: Peer-To-Peer And Hybrid Mode

`Multiplayer::Session` provides one game-facing API for client-server and
peer-to-peer play. Select a mode in `Multiplayer::Config`, call `update()` each
frame, publish the local player's position, and read `remotePlayers()` and
`platforms()`:

```cpp
Multiplayer::Config config;
config.mode = Multiplayer::Mode::PeerToPeer;
config.peerId = 1;
config.basePort = 7200;
auto session = Multiplayer::Session::open(config, engine.realTime());

session->update();
session->publishLocalPlayer(x, y, "health=3");
for (const auto& player : session->remotePlayers()) {
    // Read player.x, player.y, and optional player.data.
}
```

In peer-to-peer mode, a new `PeerSession` contacts a known peer and receives
the other peers' addresses. Peers then announce their player positions and
optional `data` directly to one another; `remotePlayers()` exposes the latest
received state. Peers also send presence messages, so a player who is paused
or standing still remains in the session.

If `config.serverEndpoint` names an authority, `WorldStateClient` requests
shared moving-platform positions separately. It does not send the peer's
player position to that server or register it as a server player.
`session->platforms()` returns the last received platform positions, while
`authorityState()` reports whether those updates are available. Set
`config.serverEndpoint` to an empty string to run without a platform
authority. Set the mode to `ClientServer` instead to use the same session
interface with the centralized server. Rebuild the server and clients
together after a protocol change.

## Run The Demonstrations

Start a separate terminal for each command that runs a server or game window.

| Demonstration | Command | What it shows |
| --- | --- | --- |
| Timeline controls | `./build/timeline-sandbox` | Pause, time scale, nested timeline, and frame delta controls. |
| Two update workers | `./build/thread-loop-sandbox` | Character and platform updates synchronized with the render loop. |
| Basic client-server | `./build/network-server`, then `./build/network-client` in three terminals | Three windows sharing player positions and moving platforms. |

To run the same multiplayer demo in client-server mode, start the server and
then launch `./build/multiplayer-demo --mode client-server` twice. To run it in
hybrid peer-to-peer mode, start the server and then launch these in separate
terminals:

```bash
./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200
./build/multiplayer-demo --mode peer-to-peer --id 2 --port 7202 --peer tcp://127.0.0.1:7200
```

The first peer can host the platform authority itself instead of using a
separate server: add `--host` to its command and do not start
`network-server`. For a serverless peer demonstration, pass `--server none`
to both peers; they can exchange player positions but have no shared
server-owned platforms.

`WASD` or arrow keys move in the networking demos. `P` pauses that client's
game time, and `1`, `2`, and `3` choose 0.5x, 1x, and 2x speed. On different
computers, use a reachable server address and set `--advertise HOST` for the
dedicated server and each peer. The private worker ports also need to be
reachable through any firewall; see `./build/multiplayer-demo --help`.
