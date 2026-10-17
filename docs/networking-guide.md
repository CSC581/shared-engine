This is an orientation for someone reading the networking code for the first
time. It covers what the pieces are, what happens at each point in a session's
life, and how a game plugs into it.

Everything here is under `include/`, `src/`, `sandbox/` and `tests/`. Nothing in
this guide is needed to *use* the engine — see the README for that.

---

## 1. The shape of it

There are two complete networking architectures, and one interface over both.

```
                 Multiplayer::Session          <- what a game talks to
                   /              \
     ClientServerSession      PeerToPeerSession
            |                    |        \
     Network::NetworkClient      |         Network::WorldStateClient
            |                    |          (read-only world, optional)
     Network::NetworkServerHost  Peer::PeerSession
      (sockets, workers)         |
     Network::NetworkServer      |
      (session/state store)      |
            |                    |
      NetworkProtocol.hpp    PeerProtocol.hpp
            \                   /
          Net::WireFormat, ZmqMessage, Endpoint
                      |
                   ZeroMQ
```

Why two protocols rather than one: the client-server protocol is a
request/reply conversation with an authority — every message is addressed to
the server and every reply is the server's view of the world. Peer messages are
announcements: a peer states something about itself to everybody and nobody
replies. Folding them together would produce a message set where half the
fields are meaningless in either direction.

What they do share is plumbing, in three header-only files:

| Header | Job |
| --- | --- |
| `WireFormat.hpp` | Values to text fields and back. No ZeroMQ, so a protocol can be encoded, decoded and tested with no transport at all. |
| `ZmqMessage.hpp` | Those fields onto a socket and off again. The only place ZeroMQ appears outside the modules that dial sockets. |
| `Endpoint.hpp` | `tcp://host:port` rewriting, so an address bound on `0.0.0.0` becomes one another machine can dial. |

### Suggested reading order

1. **`include/Multiplayer.hpp`** (205 lines) — the interface a game sees, and
   the design argument for why it is this small. Read the comments; they carry
   the reasoning.
2. **`include/WireFormat.hpp`** (125) — how anything gets onto the wire.
3. **`include/NetworkProtocol.hpp`** (113) then **`include/PeerProtocol.hpp`**
   (98) — the two message sets, side by side.
4. **`src/Multiplayer.cpp`** (322) — both architectures implementing the same
   interface. The clearest place to see how differently they work.
5. **`src/PeerSession.cpp`** (534) — the largest and trickiest file: three
   threads, two sockets, and the mesh discovery.
6. **`include/NetworkServerHost.hpp`** then **`src/NetworkServerHost.cpp`** —
   how the server store is put on the network: the handshake, one worker
   thread per client, and shutdown.

`tests/MultiplayerTests.cpp` is worth reading early too: `exerciseAsAGameWould()`
is one function containing the calls a game's frame loop makes, run against both
architectures with no branching inside it. If both pass it, the abstraction is
real rather than two classes sharing a header.

---

## 2. What happens, step by step

### 2.1 The server starts

Hosting is an engine feature: `Network::NetworkServerHost` in `network-core`
(`include/NetworkServerHost.hpp`). A server executable only supplies a
`ServerConfig` (spawn points, platform paths), its command line, and Ctrl-C
handling. `sandbox/NetworkServerMain.cpp`, run as `./build/network-server`, is
the whole of such an executable in about 70 lines:

```cpp
Network::HostConfig hostConfig;              // mode, bind, advertise host
Network::NetworkServerHost host(NetworkDemo::makeServerConfig(), hostConfig);
host.start();                                // throws if the address is taken
// ... wait for Ctrl-C ...
host.stop();                                 // joins every thread it started
```

The host has two modes:

| Mode | Sockets | Used by |
| --- | --- | --- |
| `HostMode::Dedicated` | handshake `REP` + one `REP` worker per client | `network-server` (Sections 2 and 4) |
| `HostMode::Listen` | one `REP` answering every request | a player hosting the platform authority with `--host` (Section 5) |

`NetworkServer` itself stays free of sockets, so its rules are unit-tested by
handing it messages directly; the host is the only part that knows about
ZeroMQ, and its header does not include ZeroMQ either.

In `Dedicated` mode the host:

1. Binds one `REP` socket — the **handshake** socket — at `tcp://*:5555`. This
   is the only well-known address in the system.
2. Starts a thread that calls `NetworkServer::update()` every 16ms. That
   advances the moving platforms on real time and expires players who have gone
   quiet. It runs whether or not any client is asking for anything, because
   platform motion is not driven by request arrival.
3. Loops on the public socket accepting `JOIN` for players and `GET_WORLD` for
   read-only world observers. Only `JOIN` creates a private worker or player ID.

To see Section 4 from the server's side, set `HostConfig::trafficLogInterval`
(`./build/network-server --rates`): the host logs each player's accepted
`POSITION` rate, counted by `NetworkServer::traffic()`, so a client at 2x
shows roughly double the rate of one at 1x. `advertiseHost` must be an
address clients can dial; `*`, `0.0.0.0` and `::` are rejected at
construction.

`Listen` mode skips the workers: its one socket answers `GET_WORLD`, `JOIN`,
`POSITION` and `LEAVE` itself, which is all a listen-server handing platforms
to hybrid peers needs.

The server is headless — no SDL, no window. `network-core` does not link SDL at
all, which is enforced by the link line rather than by discipline.

### 2.2 A client joins

1. Client connects a `REQ` socket to the handshake address and sends
   **`JOIN`** carrying a random 32-hex session token and a display name.
2. Server assigns the next `PlayerId`, picks a spawn point — preferring an
   unoccupied one so join/leave churn cannot stack two players on the same spot
   (`NetworkServer::spawnPlayer`) — and **spawns a dedicated `REP` worker
   thread** bound to an ephemeral port.
3. Server replies **`WELCOME`** with the player id, that private worker's
   address, and a first snapshot.
4. Client reconnects its `REQ` socket to the private address. Everything after
   this point happens there; the handshake socket is free again immediately.

That per-client worker is the Section 4 requirement: a slow client stalls only
its own thread, and the semantics are built from plain `REQ`/`REP` rather than
from `ROUTER`/`DEALER`, which the assignment forbids for this.

If the same session token joins again, the server reuses the existing worker
rather than issuing a second one, so a client that reconnects does not leak a
thread. A worker ends when its client sends `LEAVE`, or when the server
expires its player for inactivity — a crashed client never says goodbye, so
the worker notices on its next idle poll that its session is gone. Either way
the host joins and drops it while it keeps running (`activeWorkers()` reports
how many it holds). A JOIN waits
at most `HostConfig::workerStartTimeout` for its worker to bind, so one worker
that fails to start cannot hold up everybody else's JOIN.

### 2.3 Clients exchange data

**In client-server mode, they don't — the server relays.** Each client sends
**`POSITION`** (its own pose, a monotonically increasing sequence number, and an
opaque blob of its own) and gets back a **`SNAPSHOT`** containing every player
and every platform. The server stores what it is told; it does not validate
positions, so this is trusted-client, not authoritative physics.

The sequence number is what makes the request/reply honest: the server rejects
any position whose sequence does not advance, so a duplicated or reordered
request cannot move a player backwards.

**In peer-to-peer mode, they genuinely do.** See §3.

### 2.4 A client leaves

Three ways, and all three are handled:

| How | What happens |
| --- | --- |
| Clean exit | Client sends `LEAVE`; server replies `GOODBYE`, erases the player, and the worker thread ends and is reaped by the host. |
| Killed / crashed | No message. The server's update thread expires any player unheard from for **3 seconds** (`ServerConfig::inactivityTimeoutTics`); that player's worker then sees its session gone, exits, and is reaped. |
| Server dies | Client's `NetworkClient` retries once a second and reports the failure through `status()`. |

---

## 3. The peer-to-peer path

`include/PeerSession.hpp`, `src/PeerSession.cpp`. This is Section 5, and it is
the part most worth reviewing carefully.

Each peer binds **two** sockets and runs **three** threads:

| Socket | Role |
| --- | --- |
| `PUB` (port P+1) | everything this peer says about itself, fanned out to everyone at once |
| `REP` (port P) | answers introductions |

The second socket exists because PUB/SUB is one-directional. A newcomer that
subscribed to an existing peer would hear that peer without that peer ever
hearing back. `HELLO` fixes that.

### Joining a mesh

1. Both sockets bind; their real addresses are read back and rewritten to the
   advertise host, because an address bound on `0.0.0.0` is correct for
   listening and useless to hand out.
2. The bootstrap thread sends **`HELLO`** (id, name, both endpoints) to each
   configured peer address, over a short-lived `REQ` socket with a 200ms
   timeout — short-lived because a `REQ` that never gets its reply is stuck
   forever, and a peer that is not up yet is the normal case at startup.
3. The receiving peer records the newcomer, queues a subscription to its `PUB`,
   and replies **`ROSTER`** — everyone *it* knows.
4. The newcomer subscribes to each peer in that roster and sends `HELLO` to any
   it has not greeted.

So **one address is enough to reach a mesh of any size**. The test deliberately
bootstraps peer 3 off peer 2 rather than peer 1, so the roster has to travel a
hop for the mesh to close.

### Exchanging player data

`publishLocalPlayer` builds a **`STATE`** — id, name, an incrementing sequence,
position, and the game's own opaque blob — and pushes it out the `PUB` socket.
One send reaches every peer; nobody acknowledges, so a slow peer cannot stall
the sender. **No server sees this message and no server relays it.**

Receivers accept a `STATE` only if its sequence beats the newest one already
held for that peer. Arrival order is not send order, so a stale pose must never
overwrite a fresh one.

If a shared-world authority is configured, the peer also holds a
`WorldStateClient` to it. Every 50ms it sends `GET_WORLD` and receives
`WORLD_STATE` with a world-only revision and shared platforms, but no player
list. It does not `JOIN` or consume a player slot. This is the hybrid design:
common world details from an authority, player data directly between peers.
An unreachable authority is retried on real time; the session retains its last
platform positions until fresh state arrives.

### Leaving

| How | What happens | Measured |
| --- | --- | --- |
| Clean exit | Destructor publishes `LEAVE`, pauses 50ms so `PUB` can flush, then joins its threads. | gone in <250ms |
| Killed / cable pulled | Nothing is sent. Every peer sweeps twice a second and drops anyone unheard from for **5 seconds**, delivering a *synthetic* `LEAVE` so a game has exactly one way to learn a peer is gone. | gone in ~5.2s |

Liveness does **not** depend on the game publishing anything. Each session
announces itself with **`PING`** twice a second on its own thread, and the
subscriber consumes those pings rather than handing them to the game. This
matters: tying liveness to game traffic made a paused game indistinguishable
from a crashed one, and threw it out of the session after five seconds.

The SUB connection to a departed peer is deliberately *not* torn down. ZeroMQ
reconnects a live SUB by itself, so a peer returning on the same address is
heard again.

---

## 4. How an individual game uses it

The whole surface is seven calls. A game never mentions `Network`, `Peer`, ZeroMQ,
joins, handshakes, rosters, snapshots or sequence numbers.

### Opening a session

```cpp
#include "Multiplayer.hpp"

Multiplayer::Config config;
config.mode = Multiplayer::Mode::PeerToPeer;   // or Mode::ClientServer
config.playerName = "ada";
config.peerId = 1;                             // peer-to-peer only; must be unique
config.basePort = 7200;                        // peer-to-peer only; uses P and P+1
config.bootstrapPeers = {"tcp://127.0.0.1:7202"};
config.serverEndpoint = "tcp://127.0.0.1:5555";

auto session = Multiplayer::Session::open(config, engine.realTime());
```

For a binary client-server session, select it explicitly before `open()`:

```cpp
config.mode = Multiplayer::Mode::ClientServer;
config.clientServerDataFormat = Net::DataFormat::Binary;
config.clientServerSnapshotStrategy = Net::SnapshotStrategy::PropertyDelta; // optional
```

Multipart text and full snapshots remain the defaults. Property deltas require
binary client-server mode. A complete welcome snapshot establishes the
baseline; subsequent replies contain changed fields and membership. The client
still exposes a complete `WorldSnapshot`, and requests a complete resync when
its acknowledged baseline does not match the host. Binary and property deltas
are rejected in peer-to-peer mode.

Pass `engine.realTime()`, never `engine.gameTime()`. A session running on a
pausable clock could never reconnect, and could never report the unpause.

`open()` never throws for a network reason. A dead server, a port already in
use, a peer that is not up yet — all arrive as `state()` and `status()`, because
every one of them is something a game wants to draw rather than catch.

### Each frame

```cpp
void MyGame::update(const FrameTime& time, Engine& engine)
{
    session_->update();                        // pump; call even while paused

    movePlayer(time);                          // the game's own simulation
    session_->publishLocalPlayer(x_, y_);      // where this player is
}

void MyGame::render(SDL_Renderer* renderer) const
{
    for (const auto& platform : session_->platforms()) draw(platform);
    for (const auto& player : session_->remotePlayers()) draw(player);
    draw(x_, y_);                              // this player, drawn by the game
}
```

`state()` describes whether the player session itself is usable. In hybrid
peer-to-peer mode, `authorityState()` separately describes the optional source
of shared world objects. This lets a game show a useful message when players
can see one another but the platform authority is still connecting.

`remotePlayers()` never contains the local player, in either mode, so a game
draws them all and its own character without filtering and without drawing
itself twice.

### Sending more than a position

A real player has a score, a facing, health, an animation state. Those travel in
one opaque field the engine never parses:

```cpp
#include "WireFormat.hpp"   // for Net::formatFloat / Net::parseFloat

// sending — this game's own format, understood nowhere else
std::ostringstream out;
out.imbue(std::locale::classic());
out << strokes_ << ' ' << Net::formatFloat(facing_);
session_->publishLocalPlayer(x_, y_, out.str());

// receiving
for (const auto& player : session_->remotePlayers()) {
    std::istringstream in(player.data);
    in.imbue(std::locale::classic());
    int strokes = 0;
    std::string facing;
    if (in >> strokes >> facing) { /* draw them */ }
}
```

Adding a field is a change to the game's own two functions. No protocol version,
no engine header, nothing to rebuild but the game.

Use `Net::formatFloat` / `Net::parseFloat` for numbers rather than
`std::to_string` / `std::stof`. The latter round a float to six significant
figures and follow the global locale, so a machine set to decimal commas writes
`1,5` and every other machine rejects it. `NetworkTests.cpp` has a comma-locale
test because that has already bitten this codebase once.

Up to 512 bytes per player. ZeroMQ frames are length-delimited, so the field may
contain anything — tabs, newlines, packed binary — with nothing to escape.

### Things a game must get right

- **Call `update()` every frame, including while paused.** Otherwise what other
  players are doing stops arriving while this one is not doing anything.
- **Ids must be unique** across a peer-to-peer session. The introduction
  handshake rejects a duplicate ID and reports it through `status()`, but the
  game still needs to choose a different ID before it can join.
- **No ZeroMQ in game code.** Games use `Multiplayer::Session` to play and
  `Network::NetworkServerHost` to host; neither needs a socket. The
  `no-transport-in-games` test fails if any file under `individual-games/`
  uses ZeroMQ directly — if a game needs something the engine does not offer,
  add it to the engine.
- **`--advertise` is required off-localhost.** An address bound on `0.0.0.0` is
  not one another machine can dial.
- **Platforms may be stale.** If the authority becomes unreachable they stop
  moving but stay where they were, and `status()` says so. A frozen platform is
  wrong but coherent; a vanished one drops every player through the floor.

`sandbox/MultiplayerDemo.cpp` is a complete worked example — search its `Game`
class for "Network", "Peer" or "zmq" and there is nothing to find.

---

## 5. What to look at critically

Points where a reviewer's attention is best spent, including the places this
code has already been wrong.

**Thread safety in `PeerSession`.** Three threads share one state mutex. ZeroMQ
sockets are not thread-safe, so each socket is owned by exactly one thread; the
`PUB` socket is the exception and is guarded by its own mutex, which supplies
the memory barrier ZeroMQ requires. Sockets are handed to their thread at
construction, which is itself a barrier. Every thread is joined before any
socket or the context is destroyed.

**Why the greeter and the bootstrapper are separate threads.** Two peers
greeting each other simultaneously would otherwise each be blocked in a request
while the other waited to be answered.

**Everything off the wire is validated.** Ids, names, endpoints, float fields
and blob lengths are all checked before use; duplicate peer IDs are refused
during introduction. `PeerTests` and `NetworkTests`
both contain rejection suites. `tcp://` and `ipc://` are the only transports
accepted, because endpoints are handed straight to `zmq_connect`.

**The PUB slow-joiner problem is real here.** A `PUB` socket silently drops
anything sent before a subscriber has finished connecting. It is why a peer's
own `LEAVE` can be missed at the very start of a session, and why the 5-second
timeout exists as a backstop rather than as the primary mechanism.

**Known limits, stated plainly:**

- Positions are trusted client reports. There is no server-side validation, so
  a modified client can put its player anywhere.
- Session tokens are random but unauthenticated. They prevent accidental
  collision, not impersonation.
- There is no encryption. This is a LAN/classroom design.
- A returning peer is visible in `remotePlayers()` before it is counted in the
  internal roster, because the roster is only re-populated by `HELLO`. This is
  cosmetic — `peerCount()` is not part of the game-facing API and appears only
  in a status string.

---

## 6. Running the tests

```bash
cmake --build build
ctest --test-dir build --output-on-failure     # 10 tests, ~15s
```

`peer-tests`, `multiplayer-tests` and `network-server-host-tests` open real
TCP sockets and run real sessions in-process; they are integration tests, not
mocks. `network-server-host-tests` runs the real host on loopback (distinct
private workers, three real clients, rejoin, LEAVE reaping, listen mode, clean
shutdown), and `multiplayer-tests` drives both architectures through it.

`peer-tests` is the slowest at ~8.7s, and most of that is one case
(`testSilentPeersStayInTheSession`) deliberately waiting out the 5-second
liveness timeout. There is no way to prove something does *not* happen after
five seconds in less than five seconds, and making the timeout configurable so
the test could hurry it along would be a knob that exists for the test rather
than for any game.

For a manual end-to-end check, first start the dedicated authority in one
terminal:

```bash
./build/network-server
```

Then start two peer-to-peer players in separate terminals:

```bash
./build/multiplayer-demo --mode peer-to-peer --id 1 --port 7200
./build/multiplayer-demo --mode peer-to-peer --id 2 --port 7202 --peer tcp://127.0.0.1:7200
```

Both windows should show the other player and the same moving platforms. To
test a listen-server instead, stop `network-server`, add `--host` to the first
peer command, and run the second peer command unchanged.

---

## Performance benchmark methodology

`network-performance-benchmark` compares multipart text with one-frame binary
through the production stack: `NetworkClient`, `NetworkServerHost`, ZeroMQ,
the normal codecs, one host worker per client, and server-owned `PathMover`
platforms. It is headless and does not link SDL.

The default command runs 90 experiments: both formats, 1/2/4 concurrent
clients, 10/100/500 moving platforms, five repetitions, 100 warm-up round
trips and 1,000 measured round trips per client. Text and binary trials for a
cell run next to each other within each repetition so format is not
systematically confounded with machine drift or thermal state.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target network-performance-benchmark -j 4
./build-release/network-performance-benchmark \
  --runs build-release/runs.csv \
  --latency build-release/latency_samples.csv
```

Each client has its own thread. Before timing, every client connects, waits for
the complete player/platform scene and finishes its warm-up. The main thread
then captures server traffic counters and releases every client through one
measurement gate. The interval ends when the last measured reply arrives;
JOIN, startup, scene stabilization, warm-up, LEAVE and shutdown are excluded.
Throughput is aggregate completed round trips divided by that wall time. RTT
is sampled around every individual position request and snapshot reply.

A run fails on a timeout or disconnect, a missing reply, a wrong player or
platform count, a client message-count mismatch, binary traffic with other
than one frame per message, or a per-player server traffic delta other than
the measured iteration count. Payload-byte counters sum ZeroMQ frame contents
and do not estimate transport framing or TCP/IP overhead.

Quick mode keeps the complete 18-cell matrix but uses one repetition, five
warm-up iterations and twenty measured iterations:

```bash
./build/network-performance-benchmark --quick \
  --runs /tmp/network-performance-quick-runs.csv \
  --latency /tmp/network-performance-quick-latency.csv
```

CTest registers this as `network-performance-benchmark-quick`. It validates
the harness, not performance. Canonical evidence must come from the default
matrix in a Release build on an otherwise idle machine.

`runs.csv` contains one aggregate row per repetition. `latency_samples.csv`
contains one nanosecond RTT per client iteration, keyed by run, format, cell,
repetition, client and iteration, plus the format that client used
(`client_format`). Phase 5 creates the timestamped result
directory, validates these files, records metadata and generates summaries and
graphs.

### Canonical results

The canonical Release collection from 2026-10-11 completed all 90 runs with
no correctness failures and validated 210,000 RTT samples. Its reviewable
summary, machine metadata and four SVG graphs are in
`docs/performance-results/20261011T032629Z/`; the much larger raw run and RTT
files remain under `build/performance-results/20261011T032629Z/`.

On the recorded i7-12650H loopback run, binary reduced mean elapsed time in
every matrix cell. The speedup ranged from 2.83x with four clients and ten
platforms to 41.11x with one client and 500 platforms. Encoded payload per
round trip improved as snapshots grew: the 500-platform cells were 23.4% to
23.8% smaller. With four clients and only ten platforms, binary was 3.2%
larger, showing that its large speed improvement in small scenes comes from
using two frames per round trip instead of the text protocol's many multipart
frames, rather than from guaranteed byte reduction.

These values are a single-machine loopback baseline, not universal network
performance claims. Payload counters measure ZeroMQ frame contents and omit
transport framing and TCP/IP overhead; both formats still send identical full
snapshots.

### Mixed-format experiment

The default matrix runs each format alone, so the two formats never share a
server. `--mixed` asks whether they slow each other down when they do. It runs
2 and 4 clients, 10/100/500 platforms and five repetitions, in three
compositions per cell. These are all-text, all-binary, and mixed, where even
client indexes use text and odd ones use binary. The three runs of a cell are
adjacent within each repetition.

```bash
./build-release/network-performance-benchmark --mixed \
  --runs build/performance-results/<ts>-mixed/runs.csv \
  --latency build/performance-results/<ts>-mixed/latency_samples.csv
python3 benchmarks/analyze_mixed_format.py build/performance-results/<ts>-mixed
```

For each format the analyzer compares the RTT of its clients in the mixed run
("mixed") with the all-same-format run of the same total client count
("alone"). It reports the mean of the per-run mean RTTs, their sample variance
and the mixed/alone ratio, and writes `mixed_summary.csv` plus one SVG per
format. CTest registers `network-performance-benchmark-mixed-quick` for the
harness only.

The 2026-10-13 Release collection on an i9-9880H MacBook loopback completed
all 90 runs and is in `docs/performance-results/20261013T011546Z-mixed/`.
Binary clients were not slowed by text clients. Their mixed/alone RTT ratio
was 0.89 to 0.95 in five of six cells. The one ratio above 1.0 (1.05 with two
clients and 500 platforms) is within one run-to-run standard deviation. Text
clients were also slightly faster when mixed (0.86 to 0.97).

Ratios below 1.0 do not mean mixing helps. A text client completes fewer
requests per second than a binary client (1.4 to 12 times fewer here), so replacing binary
neighbors with text neighbors lowers the total request rate the server
sees. In a mixed run the binary clients also finish their iterations first,
and the text clients then run with fewer competitors. The conclusion is only
that a text client puts no extra burden on binary clients sharing its server.
These absolute RTTs come from a different machine than the canonical results
above and must not be compared with them.

### Optional engine diagnostics

The focused collision, component-update, frame-worker, codec, multiplayer-mode
and snapshot-strategy benchmarks are diagnostic tools, not additional
production network-format evidence.
Their quick CTest entries verify that each harness runs; they do not enforce a
performance threshold. Full Release matrices can be collected with:

```bash
./benchmarks/collect_engine_performance.sh
```

The collector checks the expected row counts, five repetitions per summary
cell and finite summary values. These results are not canonical until they are
collected on an idle machine, reviewed and deliberately selected as evidence.

`snapshot-strategy-benchmark` is a feasibility test for the proposed
full-state-versus-property-delta comparison. Full-state measurements use the
production binary codec. The delta side models a specified packet size and
times typed diff/replay work; it does not serialize packets or use ZeroMQ, so
its CPU timings must not be presented as an end-to-end speedup. It starts from
a full baseline, verifies every reconstructed state and reports steady-update
payload savings for player-only, platform-only and all-moving traces.
