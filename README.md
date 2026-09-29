# Shared Engine — Project 2: Time and Networking Foundations

**Team:** Harsha Vardhan Puvvadi, Seojin Kim, Vanaja Agarwal · **CSC 581** · All five sections (1–5) are implemented.

A reusable C++17 game engine built on SDL3 (window, input, drawing) and ZeroMQ (networking).
Each game supplies its own input, update and draw code; the engine holds no game rules.
Milestone 1 features (entities, gravity, collision, input, scaling) are not repeated here.

```bash
git submodule update --init --recursive
cmake -S . -B build && cmake --build build -j 4 && ctest --test-dir build --output-on-failure
```

## What each section asked for and how we did it

| § | Assignment asks for | What we built | How to see it |
| --- | --- | --- | --- |
| 1 | A time system that can pause, speed up (2×) and slow down (0.5×) | A `Timeline` clock that counts time on top of another clock. The engine keeps three: **real time** (never stops), **game time** (pausable, scalable) and **loop count**. Objects move by "time since last frame" from game time, so pausing or scaling needs no change to them | `./build/timeline-sandbox`; in `network-client` press `P` to pause, `1`/`2`/`3` for 0.5×/1×/2× |
| 2 | A server with no window that keeps ≥3 separate client windows in sync | Each client moves its own player and sends the position to the server; the server replies with everyone's latest positions. Clients can join at any time | `./build/network-server`, then three `./build/network-client` (move with `WASD`) |
| 3 | Game loop split across ≥2 threads, safely | Two long-lived worker threads (one moves platforms, one moves the player). The main thread waits for both before checking collisions and drawing. Shared objects are locked while being read or written | `./build/thread-loop-sandbox` |
| 4 | A server where one slow client does not slow the others, without ZeroMQ's Router/Dealer | The server gives every client its own thread and connection. A slow or paused client only delays its own thread. Moving platforms are run by the server so all clients see them in the same place | `./build/network-server --rates` prints each client's update rate |
| 5 | Peer-to-peer networking | **Hybrid:** players send their positions straight to each other; a server (separate, or hosted inside one player's game) only supplies the moving platforms. Games switch between client-server and peer-to-peer with one setting | `./build/multiplayer-demo --mode peer-to-peer` (add `--host` to host the platforms) |

Automated tests cover every section (`ctest`), plus one that fails if a game uses ZeroMQ directly.

## Files for this milestone

```text
include/    (public headers; the matching .cpp files are in src/)
├── TimeSource.hpp, TimeUnits.hpp    §1 the basic "what time is it" interface and unit constants
├── Timeline.hpp                     §1 the pausable, scalable clock
├── DeltaTimer.hpp, FrameTime.hpp    §1 "time since last frame", given to the game each frame
├── Entity.hpp                       §3 game objects, now safe to update from several threads
├── NetworkProtocol.hpp (+ WireFormat, Endpoint)  §2 the messages clients and server exchange
├── NetworkClient.hpp                §2 client side: join, send position, receive world
├── NetworkServer.hpp                §2 server side: who is connected, where everyone is
├── NetworkServerHost.hpp            §4 runs the server: one thread per client, platform timer
├── PeerProtocol.hpp, PeerSession.hpp  §5 players finding and talking to each other directly
├── WorldStateClient.hpp             §5 fetches only the moving platforms for peer-to-peer
└── Multiplayer.hpp                  §5 one simple API over both networking styles
sandbox/    small demo programs, one per section (TimelineSandbox, NetworkServerMain, …)
tests/      automated tests for timelines, networking, peer-to-peer and multiplayer
```

## API reference

### Time (`Timeline.hpp`, `DeltaTimer.hpp`, `FrameTime.hpp`)

| Function | Parameter | Type | Meaning |
| --- | --- | --- | --- |
| `Timeline(anchor, ticSize)` | `anchor` | clock | The clock this one follows — real time, or another timeline |
| | `ticSize` | integer | How many of the anchor's ticks make one of ours. Bigger = slower. Must be > 0 |
| `setScale(scale)` | `scale` | decimal | Speed multiplier: `0.5` half speed, `1.0` normal, `2.0` double |
| `pause()`, `unpause()`, `togglePause()` | — | — | Stop / resume the clock. Its reading never jumps backwards |
| `DeltaTimer(source, maxDelta)` | `maxDelta` | integer | Longest gap one frame may report, so a freeze does not teleport objects |
| `FrameTime` (given each frame) | `dtSeconds` | decimal | Game seconds since the last frame (0 while paused) |
| | `gameTimeUs` | integer | Total game time so far, in microseconds |

### Multiplayer (`Multiplayer.hpp`) — what a game uses

A game fills in a `Config`, calls `Session::open(config, engine.realTime())`, then every frame calls
`update()` and `publishLocalPlayer(x, y, data)` and reads `remotePlayers()` and `platforms()`.
Network failures show up in `status()` instead of crashing.

| `Config` setting | Default | Meaning |
| --- | --- | --- |
| `mode` | `ClientServer` | `ClientServer` (everyone talks to a server) or `PeerToPeer` (players talk directly) |
| `serverEndpoint` | `tcp://127.0.0.1:5555` | Server address. In peer-to-peer, where platforms come from (empty = no platforms) |
| `playerName` | empty | Name other players see |
| `peerId` | `1` | Peer-to-peer only: this player's number; each player needs a different one |
| `basePort` | `7100` | Peer-to-peer only: this player uses this port and the next one |
| `advertiseHost` | `127.0.0.1` | Peer-to-peer only: this computer's address as others should dial it (LAN IP across machines) |
| `bootstrapPeers` | none | Peer-to-peer only: the address of any player already in the game |

`publishLocalPlayer(x, y, data)`: `x`, `y` are the player's position; `data` is optional extra
text the game wants to share (health, score, …), up to 512 bytes. The engine passes it along unread.

### Running a server (`NetworkServer.hpp`, `NetworkServerHost.hpp`)

Create `NetworkServerHost(serverConfig, hostConfig)`, then call `start()` and later `stop()`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ServerConfig.spawnPoints` | none | Where new players appear (`x`, `y`) |
| `ServerConfig.platforms` | none | Moving platforms: start and end point, `speed` (80 units/s), `width` (96), `height` (20) |
| `ServerConfig.maxPlayers` | `8` | Most players allowed at once |
| `ServerConfig.inactivityTimeoutTics` | 3 seconds | A player silent this long is removed |
| `HostConfig.mode` | `Dedicated` | `Dedicated`: one thread per client (§4). `Listen`: one simple connection, for serving platforms |
| `HostConfig.bindEndpoint` | `tcp://*:5555` | Address and port the server listens on |
| `HostConfig.advertiseHost` | `127.0.0.1` | This machine's address as clients should reach it |
| `HostConfig.tickInterval` | 16 ms | How often platforms move |
| `HostConfig.trafficLogInterval` | off | If set, prints how many updates each client sends per interval |

### Client–server messages (`NetworkProtocol.hpp`) — one reply per request

| Client sends | With | Server replies | With |
| --- | --- | --- | --- |
| `JOIN` | secret token, name | `WELCOME` | player id, the client's private connection address, world state |
| `POSITION` | id, token, counter, `x`, `y`, `data` | `SNAPSHOT` | every player's position and every platform's |
| `LEAVE` | id, token | `GOODBYE` | — |
| `GET_WORLD` (peer-to-peer) | — | `WORLD_STATE` | platform positions only |
| any invalid request | — | `ERROR` | reason |

## Design decisions

- **Two kinds of time.** Movement uses game time; networking, input and drawing use real time,
  so a paused client still hears the "unpause" key and stays connected.
- **One thread per client.** A basic ZeroMQ request/reply connection handles one request at a
  time, so the public address only admits new players and gives each a private connection.
- **Games own their rules.** The server only stores and shares positions, so all three team
  games reuse the same server, and switching client-server ↔ peer-to-peer is one setting.

Diagrams are in the appendix below. More detail: [docs/networking-guide.md](docs/networking-guide.md).

<div style="page-break-before: always"></div>

## Appendix: how the pieces fit

**A. One frame and the clocks behind it (§1).** Each frame runs steps 1–5. Movement uses game
time, which the game can pause or scale; networking keeps running on real time.

![Frame and time flow](docs/frame-time-flow.svg)

**B. Client-server (§2, §4).** A new client joins through the public socket and is handed its own
worker thread; after that it only talks to that worker. The server moves platforms on its own.

![Client-server flow](docs/client-server-flow.svg)

**C. Peer-to-peer, hybrid (§5).** A new peer asks any running peer for the list of players, then
sends its position straight to them. Platforms optionally come from a server.

![Peer-to-peer flow](docs/peer-hybrid-flow.svg)

<sub>LLM assistants were used to help navigate the code structure and draft docs; the team reviewed all design and code.</sub>
