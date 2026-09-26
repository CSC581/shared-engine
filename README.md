# Shared Engine

A small C++17 game-engine foundation built with SDL3.

`Engine` owns SDL setup, the window, renderer, main loop, timing, and render
scaling. A game implements `Game` (`handleInput`, `update`, and `render`) and
is passed to `Engine::run()`. `Entity`, `Physics`, `Input`, and `Collision`
provide the reusable game-building tools.

## Milestone 1 Coverage

| Milestone task | Shared-engine implementation | Where to find it |
| --- | --- | --- |
| 1. Core graphics setup | Initializes SDL3, creates a resizable window and renderer, runs the game loop, clears the screen blue, presents each frame, and cleans up SDL resources. | `include/Engine.hpp`, `src/Engine.cpp` |
| 2. Entity system | A generic `Entity` stores position, width, height, and velocity. It updates its position and exposes a bounding rectangle. | `include/Entity.hpp`, `src/Entity.cpp` |
| 3. Physics | `Physics::setGravity()` configures gravity and `Physics::applyGravity()` adds downward velocity to selected entities. | `include/Physics.hpp`, `src/Physics.cpp` |
| 4. Input | Uses `SDL_GetKeyboardState` and provides held, just-pressed, and just-released key queries. | `include/Input.hpp`, `src/Input.cpp` |
| 5. Collision | Generic axis-aligned bounding-box (AABB) overlap detection works on `Entity` or `Rect` values. | `include/Collision.hpp`, `src/Collision.cpp` |
| 6. Scaling (CSC 581) | Provides constant/pixel scaling and proportional scaling. `F1` toggles between modes by default. | `include/Engine.hpp`, `src/Engine.cpp` |

## Architecture

```text
Engine     -> SDL setup, window, renderer, loop, timing, clear, present, scaling
Game       -> game-specific rules, objects, input handling, and rendering
Entity     -> position, size, velocity, and movement
Physics    -> configurable gravity
Input      -> keyboard state queries
Collision  -> overlap and separation calculations
Timeline   -> pausable, rescalable clocks the simulation runs on
DeltaTimer -> per-consumer "time since I last looked"
Network    -> SDL-free protocol, player-state server, non-blocking client
Peer       -> SDL-free peer mesh: introductions, rosters, direct player data
Multiplayer-> one interface over both, so a game picks its architecture
```

The two networking modules speak different protocols but share their plumbing,
in three header-only pieces that keep the layers apart:

| Header | Job |
| --- | --- |
| `WireFormat` | Values to text fields and back. No ZeroMQ, so a protocol can be encoded, decoded and tested with no transport at all. |
| `ZmqMessage` | Those fields onto a ZeroMQ socket and off again. The only place ZeroMQ appears outside the modules that dial sockets. |
| `Endpoint` | `tcp://host:port` rewriting, so an address bound on `0.0.0.0` becomes one another machine can dial. |


The intended order for each frame is:

```text
input -> gravity (selected entities) -> entity update -> collision -> render
```

## Build

SDL3 and ZeroMQ are Git submodules under `vendored/`. On a new clone, fetch
them first:

```bash
git submodule update --init --recursive
```

Configure and build from the project folder:

```bash
cmake -S . -B build
cmake --build build
```

Run the automated checks with:

```bash
ctest --test-dir build --output-on-failure
```

## Key Features

### Entity And Physics

An `Entity` has a position, size, and velocity. Calling
`entity.update(deltaTime)` moves it using that velocity.

```cpp
Entity player(100.0F, 200.0F, 32.0F, 32.0F);
player.setVelocity(200.0F, 0.0F);
player.update(deltaTime);
```

Gravity is opt-in. A game chooses which objects should fall:

```cpp
Physics::setGravity(980.0F);
Physics::applyGravity(player, deltaTime);
```

### Input

The engine updates keyboard state once each frame. Games can query keys without
reading SDL events directly.

```cpp
if (Input::isKeyPressed(SDL_SCANCODE_A)) {
    player.setVelocityX(-speed);
}

if (Input::isKeyJustPressed(SDL_SCANCODE_SPACE)) {
    // Start a jump, shoot, or open a menu.
}
```

Available queries are `isKeyPressed`, `isKeyJustPressed`, and
`isKeyJustReleased`.

### Collision

Collision is SDL-free AABB geometry. It reports whether rectangles overlap;
the game decides the response.

```cpp
if (player.collidesWith(hazard)) {
    // The game decides what a hazard collision means.
}
```

`Collision::resolve(moving, blocker)` is available when a moving entity should
be pushed out of a platform or wall. `getIntersection` and `getSeparation`
provide more detailed collision information when needed.

### Scaling

Press `F1` while a game is running, then resize the window to compare modes.

| Mode | Behavior |
| --- | --- |
| Constant / pixel scaling | One game unit equals one screen pixel. Resizing can reveal more or less of the game world. |
| Proportional scaling | The design resolution scales uniformly and keeps its aspect ratio. Unused space may appear at the sides or top and bottom. |

Use `Engine::setScaleToggleKey()` to change or disable the default `F1` key.

### Timelines

The engine measures time on three scales, and owns a clock for each:

| Scale | Clock | Counts |
| --- | --- | --- |
| Real time | `realTime()` | Nanoseconds off `steady_clock`. Never pauses, never scales. |
| Game time | `gameTime()` | Game microseconds. Pausable and rescalable; the simulation runs on this. |
| Loop iterations | `loopTime()` | One tic per pass through the main loop, however long that pass took. |

Each frame the engine builds a `FrameTime` off the game timeline -- game
seconds elapsed, plus absolute game time -- and hands it down.

Everything whose motion comes from that `FrameTime` can therefore be frozen or
stretched by one call, with no cooperation from any of it:

```cpp
engine.gameTime().togglePause();  // freezes everything on game time at once
engine.gameTime().setScale(0.5);  // half speed, with no jump in position
```

Entities stay time-agnostic -- they own no clock and ask none what time it is,
they are simply told how far to move:

```cpp
void MyGame::update(const FrameTime& time, Engine&)
{
    player_.update(time);                                  // velocity * dt
    const double t = time.gameTimeUs / 1'000'000.0;        // absolute, no drift
    platform_.setPosition(originX + amplitude * std::sin(t), platformY);
}
```

A game that only implements the older `update(float deltaTime, Engine&)` keeps
working untouched; the engine forwards to it.

Timelines nest. Anchoring one to another gives a clock that inherits the
parent's pauses and multiplies its scale, which is what a slow-motion layer or
a per-client loop speed is made of:

```cpp
Timeline childTime(engine.gameTime(), 1000);  // anchor, tics of the anchor
DeltaTimer childTimer(childTime, 250);        // one per consumer
```

`engine.realTime()` is never paused and never scaled -- anchor to it for menu
animation or anything that has to keep running while the game is frozen. Input
polling and rendering run on real time for the same reason: a loop that waited
on a paused timeline could never read the key that unpauses it.

`engine.loopTime()` counts frames rather than seconds, and takes the same
pause, tic size and scale as the others -- a tic size of 2 is one tic every
second iteration. Anchor to it when a simulation has to advance per frame
rather than per second and reach the same state on every machine however fast
each one runs: lockstep peer-to-peer sync, a reproducible replay, a fixed-step
physics tick.

The module (`TimeSource`, `Timeline`, `DeltaTimer`, `FrameTime`) has no SDL
dependency and no global state, so a headless server can link `engine-time` on
its own. Every `Timeline` method is thread-safe.

### Networking

Headless ZeroMQ server plus SDL clients, separate from the individual games.
Each client simulates its own character and submits its position; the server
stores membership and poses and replies with a world snapshot (other players
plus server-owned moving platforms). Local movement uses game time; network
I/O and platforms use real time. A JOIN handshake hands each client a private
REP worker so one slow client does not stall the others (no Router/Dealer).
Demo layout lives in `sandbox/NetworkDemoConfig.hpp`. Network protocol version 6 —
rebuild server and clients together.

```bash
./build/network-server
./build/network-client   # repeat in other terminals
```

`WASD` or arrows move. `P` pauses this client; `1` / `2` / `3` set 0.5× / 1× /
2×. Defaults: `tcp://*:5555` / `tcp://127.0.0.1:5555`. Optional endpoints and
advertise host (required when the client is on another machine):

```bash
./build/network-server 'tcp://*:6000'
./build/network-server tcp://*:5555 --advertise 192.168.1.10
./build/network-client tcp://192.168.1.10:5555
```

### Choosing A Network Architecture

A game does not have to pick between client-server and peer-to-peer at the time
it is written. `Multiplayer::Session` is one interface implemented over both,
so the choice is a field in a config:

```cpp
Multiplayer::Config config;
config.mode = Multiplayer::Mode::PeerToPeer;   // or Mode::ClientServer
auto session = Multiplayer::Session::open(config, engine.realTime());
```

and the rest of the game reads the same either way:

```cpp
session->update();                          // once a frame, pause or no pause
session->publishLocalPlayer(x, y);          // where my player is
for (const auto& player : session->remotePlayers()) { draw(player); }
for (const auto& platform : session->platforms()) { draw(platform); }
```

That is the whole surface. No join, no handshake, no roster, no snapshot, no
sequence numbers, no sockets — those belong to an architecture, and the point
is that the game is not written against one. `remotePlayers()` never contains
the local player in either mode, so a game draws them all and its own character
without filtering.

#### A Player Is More Than A Position

A game's player has a score, a facing, health, an animation state. Those are
carried as attributes: string fields the engine relays and never interprets.

```cpp
Multiplayer::AttributeWriter fields;
fields.addInt(score_).addFloat(facing_).addBool(carryingFlag_);
session->publishLocalPlayer(x_, y_, fields.fields());
```

and on the other side:

```cpp
Multiplayer::AttributeReader fields(player.attributes);
std::int64_t score = 0;
if (fields.readInt(score) && fields.readFloat(facing)) { /* draw them */ }
```

They are strings because the wire is strings, and because an engine with a type
for them would be an engine that knows what one game's fields mean. A game adds
a field by changing its own encode and decode; the server, the peer module and
this interface do not change with it. `AttributeWriter` exists because the
obvious way to build those strings is wrong twice over: `std::to_string(float)`
rounds to six significant figures, and both it and `std::stof` follow the global
locale, so a machine set to decimal commas writes `1,5` and every other machine
rejects it.

Both architectures carry a player's name and up to `Net::maxAttributeCount`
fields of `Net::maxAttributeLength` printable characters. Reading is total: a
reader that runs past the end, or meets a field that is not the type asked for,
returns false rather than inventing a value — the data came from another
machine, so a game has to be able to decide what to do about nonsense.

| Mode | Players travel | Shared objects come from | Needs |
| --- | --- | --- | --- |
| `ClientServer` | via the server | the server | a `network-server` process |
| `PeerToPeer` | peer to peer, directly | an authority, or nowhere | a peer address to bootstrap from |

If the authority becomes unreachable, the world objects it sent stop moving but
stay where they were, and `status()` says they are no longer being refreshed. A
level does not cease to exist because a server blinked, and a game whose floor
vanished would drop every player through the world. Remote players are the
opposite case and do disappear: a player nobody is steering any more is a
ghost.

Pass `--host` in peer-to-peer mode to carry the authority in this process
instead of running `network-server` separately.

In `PeerToPeer` the server is optional and owns world objects only: point every
peer at one for moving platforms and the players still go peer to peer, or
leave `serverEndpoint` empty for a session with no server process anywhere.
Hybrid peers use a read-only `WorldStateClient` to fetch platforms; they do not
join the server as players or send it their positions.
Identity differs the way the architectures do — the server assigns an id, while
peers choose their own and must not collide.

```bash
./build/network-server                                    # client-server needs this
./build/multiplayer-demo --mode client-server
./build/multiplayer-demo --mode client-server             # again, for a second player

./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200
./build/multiplayer-demo --mode peer-to-peer --id 2 --port 7202 --peer tcp://127.0.0.1:7200
./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200 --server none   # no server at all
```

### Peer-To-Peer

Peers talk to each other directly. `peer-core` is a separate library from
`network-core`: the client-server protocol is a conversation with an authority,
while peer messages are announcements nobody replies to, so they are different
protocols rather than one protocol with half its fields unused.

| Piece | Job |
| --- | --- |
| `PeerProtocol` | `HELLO` / `ROSTER` / `STATE` / `LEAVE`, encoded as text fields. |
| `PeerSession` | The mesh. A `PUB` socket to announce on and a `REP` socket to be introduced on, with background threads for each. |

Every peer runs the same code and binds two consecutive ports: one to be
introduced on, one to broadcast on. A joining peer sends `HELLO` to any single
peer already running and gets that peer's whole roster back, so one address is
enough to reach a mesh of any size — the rest introduce themselves.

This is the hybrid design Section 5 asks for: player data goes straight from
peer to peer and never through a server, while a shared authority owns the
moving platforms so they are in the same place on every screen. That authority
can be a separate `network-server` process, or one of the players can carry it
with `--host` (a listen-server). Each peer keeps its own game timeline, so `P`
and `1`/`2`/`3` change that peer's speed and nobody else's.

```bash
# dedicated authority
./build/network-server
./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200
./build/multiplayer-demo --mode peer-to-peer --id 2 --port 7202 --peer tcp://127.0.0.1:7200

# or one of the players hosts it instead
./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200 --host
```

Peers decide someone is gone by hearing nothing from them, so a session
announces itself with `PING` twice a second whatever the game is doing. That
keeps liveness out of the game: a paused game, a game between levels and a
player standing still are all still in the session, and `publishLocalPlayer`
can be called as often or as rarely as the game likes. A peer that has said
nothing for five seconds — ten missed announcements — is presumed gone.

Leaving is announced with `LEAVE` and noticed within a frame; the timeout is
the backstop for the cases that announce nothing, a killed process or a pulled
cable. Measured: a clean exit disappears in under 250ms, a `SIGKILL`ed peer
after 5.2s.

`--advertise HOST` is required when the peers are on different machines, for
the same reason the server needs it: an address bound on `0.0.0.0` is not one
another machine can dial. Duplicate peer IDs are rejected during introduction.
Peer protocol version 4 — rebuild all peers together.

### The Timeline Sandbox

`timeline-sandbox` is an interactive bench for all of the above: pause, scale
and tic size sliders, a child timeline, an adjustable frame-delta clamp and
frame delay, and a graph of recent frame deltas.

```bash
cmake --build build --target timeline-sandbox
./build/timeline-sandbox            # --frames N runs N frames and exits
```

`P` pauses, `1`/`2`/`3` select 0.5x, 1.0x and 2.0x, `WASD` moves. It is the only
target that depends on Dear ImGui; the engine library must not.

### Multiple Keys At Once

`Input::update()` copies the whole keyboard every frame (`src/Input.cpp`), so
every key is an independent slot and any number of them can read as held in the
same frame. Diagonal movement, run-while-jumping, modifier combos and two
players sharing one keyboard all work without special handling.

Helpers for reading several keys together:

- `areAllKeysPressed({a, b, c})` — every listed key is held right now, for
  chords and modifier combos.
- `isAnyKeyPressed({a, b})` — at least one of them is held.
- `getAxis(negativeKey, positiveKey)` — `-1` / `0` / `+1` for an opposed pair;
  holding both cancels out. Two calls give a full 8-way direction.
- `pressedKeyCount()` and `getPressedKeys()` — how many, and which, keys are
  held this frame. Useful for debug readouts.

```cpp
// 8-way movement plus a sprint modifier: up to three keys at once.
const float speed = Input::isKeyPressed(SDL_SCANCODE_LSHIFT) ? sprintSpeed : walkSpeed;
player.setVelocity(Input::getAxis(SDL_SCANCODE_A, SDL_SCANCODE_D) * speed,
                   Input::getAxis(SDL_SCANCODE_W, SDL_SCANCODE_S) * speed);
```

A very brief press and release that happens entirely between two frames may not
be visible to a polling input system. If a specific physical combination never
appears in `getPressedKeys()`, that is keyboard ghosting in the hardware rather
than an engine limitation; most non-gaming keyboards drop the third
simultaneous key in some rows.
