# Shared Engine, Milestone 3: Game Object Model and Networked Scene

**Team:** Harsha Vardhan Puvvadi, Seojin Kim, Vanaja Agarwal · **CSC 581**
**Github Link:**https://github.com/CSC581/shared-engine/tree/milestone-3

This is our C++17 game engine. It uses SDL3 for the window, input and drawing, and ZeroMQ for networking. Each game keeps its own rules, and any networking goes through the engine. There are tests for each module. This README only covers Milestone 3. Time, threads and the networking foundations were done in Milestone 2 and are left out here, except where Milestone 3 changed them.

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j 4 -- -k   # -k: keep going past the unported golf game
ctest --test-dir build --output-on-failure -E 'multiplayer-tests'
```

## What each part asked for and how we did it

| Part | Milestone Requirements                                                                                                                      | What we built                                                                                                                                                                                                                                                                                                                                                                                         |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1A   | A component-, property- or custom-based object model (no monolithic hierarchy) that new object types can be built from                      | **Component-based.** A `GameObject` is an id, a tag and the components it owns. A new kind of object is a new mix of components made by a factory function, not a new class. `World` owns every object and runs them each frame. The old `Entity` class and global `Physics` are gone                                                                                                                 |
| 1B   | The object model on every network endpoint, up to 4 clients, movement and platforms synced, disconnects handled                             | The server keeps every player and moving platform as a `GameObject` in its own `World`, and moves platforms with the same `PathMover` a client uses offline. On each client, `NetworkWorldSync` creates, moves and destroys objects to match the session, so a player who leaves or times out disappears from every other screen. There is no player cap, and every client gets its own server thread |
| 1C   | A second network data format and performance experiments                                                                                    | A one-frame **binary** format next to the Milestone 2 multipart text format. Both carry the same messages and full snapshots, and the server answers each client in the format it used. On loopback, binary round trips were 2.8×–41× faster, mainly because each one uses 2 ZeroMQ frames instead of up to 2,533. Four experiments are listed under [Part 1C experiments](#part-1c-experiments)      |
| 2    | A game on the object model with players, ≥3 static platforms, ≥2 moving patterns, hidden spawn points, death zones and scrolling boundaries | Each of us ported a game: Apex Ascent (Harsha), Pokemon Hunt (Seojin) and Starfall Salvage (Vanaja). Each game's README describes its objects                                                                                                                                                                                                                                                         |

### How to see each part

The commands assume the build step above has been run.

| Part  | Harsha: Apex Ascent                                                                                                                          | Seojin: Pokemon Hunt                                                         | Vanaja: Starfall Salvage                                                                   |
| ----- | -------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| 1A, 2 | `./build/apex-ascent`, then climb the six rooms                                                                                              | `./build/pokemon-hunt-server`, then `./build/pokemon-hunt --pokemon pikachu` | `./build/starfall-salvage-server`, then `./build/starfall-salvage --name Nova`             |
| 1B    | `./build/apex-network-server`, then `./build/apex-ascent --join` in up to four terminals. Close one and its ghost disappears from the others | the same server, then `./build/pokemon-hunt` in up to four terminals         | the same server, then `./build/starfall-salvage --name Sol` (and `Mira`) in more terminals |
| 1C    | `./build/network-performance-benchmark --quick` (see [Part 1C experiments](#part-1c-experiments))                                            | `./build/pokemon-hunt --format binary --benchmark 3000`                      |                                                                                            |

The engine's own demo also shows 1B: run `./build/network-server`, then `./build/multiplayer-demo --mode client-server` in up to four terminals and move with WASD. Each window shows the measured latency, about 20 ms with four clients.

## Files for this milestone

```text
1A Object model
├── include/Component.hpp, GameObject.hpp       src/GameObject.cpp          component base, object (add/get/require)
├── include/Components.hpp, Color.hpp           src/Components.cpp          the built-in components
├── include/World.hpp                           src/World.cpp               owns objects: update → collisions → destroy
├── include/Collision.hpp                       src/Collision.cpp           Rect and overlap tests
└── include/Engine.hpp                          src/Engine.cpp              renderWorld(): draws Renderables by layer
1B Networked scene
├── include/NetworkServer.hpp                   src/NetworkServer.cpp       server scene kept in a World
├── include/NetworkServerHost.hpp               src/NetworkServerHost.cpp   one thread per client, 16 ms platform tick
├── include/NetworkWorldSync.hpp                src/NetworkWorldSync.cpp    mirrors a session into a client's World
└── include/Multiplayer.hpp                     src/Multiplayer.cpp         session on its own network thread
1C Network formats and experiments
├── include/NetworkDataFormat.hpp                                          text or binary
├── include/BinaryNetworkProtocol.hpp           src/BinaryNetworkProtocol.cpp  one-frame binary codec
├── include/NetworkSnapshotStrategy.hpp                                    full snapshots or binary deltas
├── benchmarks/NetworkPerformanceBenchmark.cpp                             real-stack comparison
└── benchmarks/analyze_network_performance.py                              validation and graphs
```

## API reference

### Objects (`GameObject.hpp`, `World.hpp`)

A game builds each kind of object with a factory function:

```cpp
GameObject& makePlatform(World& world, Rect box, Color color)
{
    GameObject& object = world.create("platform");   // starts inactive
    object.add<Transform>(box.x, box.y, box.width, box.height);
    object.add<Collider>(Collider::Kind::Solid);
    object.add<Renderable>(color).layer = 1;
    object.setActive(true);                           // always last
    return object;
}
```

| Function                                                                    | Meaning                                                                            |
| --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `world.create(tag)`                                                         | A new, inactive object. Inactive objects do not update, collide or draw            |
| `object.add<T>(args...)`                                                    | Attaches a component and returns it. A second component of the same type throws    |
| `object.get<T>()`, `has<T>()`                                               | The component of exactly that type, or `nullptr`                                   |
| `object.require<T>()`                                                       | Gets the component, adding it first if missing. Components call it in `onAttach()` |
| `object.setActive(on)`                                                      | Switches an object in or out of the update, collisions and drawing                 |
| `world.destroy(id)`                                                         | Removes an object at the end of the frame. Safe from any thread                    |
| `world.find(id)`, `findByTag(tag)`, `findAllByTag(tag)`, `findAllWith<T>()` | Look objects up                                                                    |
| `world.update(frameTime)`                                                   | Runs every component, then `resolveCollisions()`, then `flushDestroyed()`          |
| `world.updateComponents(frameTime, tag)`                                    | Updates one tag only, so `FrameWorkers` threads can split the world by tag         |
| `renderWorld(renderer, world, cameraX, cameraY)`                            | Draws every visible `Renderable`, lowest `layer` first                             |

Components cannot be removed. To take a behaviour away, switch it off through one of its fields, or destroy the object and build a new one.

### Components (`Components.hpp`)

| Component         | Gives an object                                               | Settings                                                                  |
| ----------------- | ------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `Transform`       | position and size                                             | `x`, `y`, `width`, `height`                                               |
| `Motion`          | velocity, applied each frame on game time                     | `velocityX`, `velocityY`, `kinematic` (never pushed by collisions)        |
| `Gravity`         | downward acceleration                                         | `acceleration` (default 980)                                              |
| `PathMover`       | movement along a `Linear` (back and forth) or `Circular` path | path and speed, given when it is added. Drives `Motion`                   |
| `Collider`        | collisions: `Solid` (blocks) or `Trigger` (reports only)      | `layers` mask, `onEnter` / `onCollide` (every frame) / `onExit` callbacks |
| `Renderable`      | drawn by `renderWorld()` as a colour or texture               | `color`, `texture`, `visible`, `layer`. No `Renderable` means hidden      |
| `Behavior`        | game logic each frame (e.g. reading the keyboard)             | a function, run before movement                                           |
| `NetworkIdentity` | which network player or platform the object stands for        | `kind`, `id`. Used by the server and `NetworkWorldSync`                   |

A game adds its own components by deriving from `Component` and overriding `onAttach()` and `update()`. Apex, for example, adds `Surface` (ice friction), `Crumble`, `Carrier`, `DeathZone`, `RoomZone` and `WindZone`.

| Part 2 object               | Built from                                                                                     |
| --------------------------- | ---------------------------------------------------------------------------------------------- |
| Player                      | `Transform` + `Motion` + `Gravity` + `Collider` (+ `Renderable` or a sprite drawn by the game) |
| Static platform             | `Transform` + `Collider` + `Renderable`                                                        |
| Moving platform             | the same + `PathMover` (`Linear` or `Circular`)                                                |
| Spawn point (hidden)        | `Transform`, no `Renderable`                                                                   |
| Death zone (hidden)         | `Transform` + `Trigger` collider whose callback sends the player to a spawn point              |
| Scrolling boundary (hidden) | a trigger that moves the camera passed to `renderWorld()`                                      |

### Networked scene (`NetworkWorldSync.hpp`)

`NetworkWorldSync(world, makePlayer, makePlatform)` takes two factories the game writes, one for a remote player and one for a server platform. Each frame, on the main thread:

```cpp
session.update();                         // even while paused
sync.apply(session);                      // create, move or destroy mirrored objects
world.update(time);
session.publishLocalPlayer(x, y, data);   // every frame, even when standing still
```

Mirrored objects should not get `Motion`, because their position comes from the network. `sync.remotePlayer(id)` and `sync.platform(id)` return the object for that id.

### Network formats (`Multiplayer.hpp`, `NetworkClient.hpp`)

| `Config` setting               | Default         | Meaning                                                                                                  |
| ------------------------------ | --------------- | -------------------------------------------------------------------------------------------------------- |
| `clientServerDataFormat`       | `TextMultipart` | Milestone 2 multipart text, or one-frame `Binary`. Binary is rejected in peer-to-peer mode               |
| `clientServerSnapshotStrategy` | `FullSnapshot`  | Complete snapshots, or binary `PropertyDelta`s. Deltas need binary and are rejected in peer-to-peer mode |

| Server setting            | Default | Meaning                                                                             |
| ------------------------- | ------- | ----------------------------------------------------------------------------------- |
| `ServerConfig.maxPlayers` | `0`     | Most players allowed at once. `0` means no limit                                    |
| `ServerConfig.platforms`  | none    | Moving platforms. New in Milestone 3: `shape = Circular` with a centre and `radius` |

Text keeps one ZeroMQ frame per field, so a client from Milestone 2 still works. Binary sends a whole message as one frame: a `SENG` marker, format and protocol versions, then fixed-width big-endian numbers and length-prefixed strings. Both reject malformed, truncated, oversized and wrong-version messages. `NetworkClient(clock, endpoint, format, snapshotStrategy)` also offers `telemetry()` (message, frame and payload-byte counts) and `snapshotTelemetry()` (full, delta and resync counts) for measurements.

### Part 1C experiments

Each experiment ran five times in Release, and the results, metadata and graphs are in `docs/performance-results/`.

| Experiment        | What it compares                                                          | Conditions                                                                         | Key result                                                                                           | Results folder                       |
| ----------------- | ------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------- | ------------------------------------ |
| Main              | Text vs binary round trips through the real client and server             | 1/2/4 clients, 10/100/500 moving platforms, 1,000 round trips per client           | Binary round trips 2.8×–41× faster, payload at most 24% smaller                                      | `20261011T032629Z`                   |
| Mixed formats     | Text and binary clients on one server, vs each format alone               | 2/4 clients, 10/100/500 platforms                                                  | No meaningful slowdown for either format (round-trip ratio 0.86–1.05)                                | `20261013T011546Z-mixed`             |
| Codec             | Encoding and decoding alone, without the network                          | Position and snapshot messages, 0/10/100/500 platforms, 0/128/512 B of player data | Binary encodes 8×–89× and decodes 15×–97× faster, though its messages were larger in every case here | `20261016T232723Z-codec`             |
| Snapshot delivery | Binary full snapshots vs binary property deltas at 60 requests per second | 1/2/4 clients, 10/100/500 platforms, 0/512 B of player data                        | Deltas 26.4%–73.7% smaller but 1.04×–2.15× slower round trips, so full snapshots stay the default    | `20261017T041259Z-snapshot-delivery` |

```bash
./build/network-performance-benchmark --quick --runs /tmp/runs.csv --latency /tmp/latency.csv   # smoke test only
./benchmarks/collect_network_performance.sh      # main experiment, Release evidence
./benchmarks/collect_snapshot_delivery.sh        # snapshot delivery experiment
```

## Design decisions

- **Composition, not inheritance.** Components declare what they need with `require<T>()` (e.g. `Gravity` pulls in `Motion` and `Transform`), and cache the pointer once. Looking up a component is one array read.
- **Fixed update order.** Components run by priority (`Behavior` → `PathMover` → `Gravity` → `Motion` → the rest), so input, then velocity, then position, every frame. The sorted order is cached and rebuilt only when objects or components change.
- **Network threads never touch a game's `World`.** The session's thread leaves the newest snapshot in a mailbox, and the main thread applies it.
- **Platforms move on the server's tick** (16 ms), not once per client request, so their speed does not depend on how many clients are sending.
- **Text stays the default** so Milestone 2 clients keep working, and binary and deltas are opt-in.

**Known limitations:** only players and platforms are synced, remote objects are not smoothed between updates, the engine does not carry a player standing on a moving platform (each game does it), and collisions check every pair of objects. Peer-to-peer is kept working through this milestone and will then be retired.

<div style="page-break-before: always"></div>

## Appendix: how the pieces fit

**A. Building and updating objects (1A).** A factory creates an inactive object, attaches its components and switches it on. Each frame, `World` runs the components in priority order, resolves collisions, removes destroyed objects, and `renderWorld()` draws what is left.

![Object model flow](docs/object-model-flow.svg)

**B. The object model on every endpoint (1B, 1C).** The server keeps players and platforms as objects in its own `World`. Each client mirrors them into its `World` with `NetworkWorldSync`, and either format can be used per client.

![Networked scene flow](docs/networked-scene-flow.svg)

<sub> **Disclaimer:** The team designed and wrote code, but also used LLMs such as Claude Sonnet 5 and ChatGPT 5.6 Terra to help restructure the code and refactor the components when in need.</sub>
