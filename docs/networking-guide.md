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
     Network::NetworkClient      |         Network::NetworkClient
            |                    |          (platforms only, optional)
     Network::NetworkServer   Peer::PeerSession
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

`tests/MultiplayerTests.cpp` is worth reading early too: `exerciseAsAGameWould()`
is one function containing the calls a game's frame loop makes, run against both
architectures with no branching inside it. If both pass it, the abstraction is
real rather than two classes sharing a header.

---

## 2. What happens, step by step

### 2.1 The server starts

`sandbox/NetworkServerMain.cpp`, run as `./build/network-server`.

1. Binds one `REP` socket — the **handshake** socket — at `tcp://*:5555`. This
   is the only well-known address in the system.
2. Starts a thread that calls `NetworkServer::update()` every 16ms. That
   advances the moving platforms on real time and expires players who have gone
   quiet. It runs whether or not any client is asking for anything, because
   platform motion is not driven by request arrival.
3. Loops on the handshake socket accepting `JOIN` and nothing else.

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
thread.

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
| Clean exit | Client sends `LEAVE`; server replies `GOODBYE`, erases the player, and the worker thread ends. |
| Killed / crashed | No message. The server's update thread expires any player unheard from for **3 seconds** (`ServerConfig::inactivityTimeoutTics`). |
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
`NetworkClient` to it — but only for platforms. Its reply carries a player list
and that list is deliberately discarded. This is the hybrid design: common world
details from an authority, player data peer to peer.

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
ctest --test-dir build --output-on-failure     # 8 suites, ~15s
```

`peer-tests` and `multiplayer-tests` both open real TCP sockets and run real
sessions in-process; they are integration tests, not mocks.

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
