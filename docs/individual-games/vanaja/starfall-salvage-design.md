# Starfall Salvage: design decisions

Starfall Salvage is a cooperative three-player platformer on the shared engine.
This document records the game-specific decisions behind its port to the
Milestone 3 component object model and its Milestone 4 event-driven features. Shared-engine decisions live in the
engine's own decision record; only what is particular to this game is here.

Each entry says who made the decision (*user* or *agent*), why, what was
rejected and what limitation was accepted.

| ID | Decision | Rationale and rejected alternatives | Accepted limitation |
| --- | --- | --- | --- |
| SS-01 | *(agent)* Build every station object with SDL-free factory functions in `StarfallSalvageLevel.cpp`, compiled into the `starfall-salvage-rules` library | The level and the astronaut's physics can be built and simulated in headless tests. Rejected: building objects inside the SDL game class, which only a running window could exercise | The game class still owns drawing, input and networking |
| SS-02 | *(user)* Use the engine's solid collisions for the astronaut, as Apex Ascent and Pokemon Hunt do | One collision model for every game on the engine. Rejected (agent's first recommendation): keeping the hand-written landing resolver as a component, which felt identical to the old game but kept a second collision model | The engine pushes along the axis of least overlap, so a landing with less of the astronaut over an edge than it sank that frame (at most about 12 px) is pushed off the edge rather than landed |
| SS-03 | *(agent)* Run at 50 loops per second (25/50/100 at 0.5×/1×/2×) and cap the fall speed at 600 px/s | The frame interval is a whole 20 ms, which keeps the pacing tests exact, and the cap limits how far the astronaut sinks into a deck per frame. Rejected: 60 Hz (16.67 ms is not a whole number of nanoseconds or microseconds, so frame deltas drift), the old 25 Hz (up to ~27 px of sink, almost the astronaut's width) and collision substeps on the main thread. *Revised 2026-10-21 (agent):* the loop restarted its timer after every sleep, so oversleeps added up and it ran at 48 Hz; frames now follow a fixed schedule and run at 50 Hz, and a frame more than one interval late restarts the schedule instead of rushing to catch up | Twice the old message rate at each speed; a slow frame is simulated with its real, longer delta |
| SS-04 | *(agent)* Put the platformer rules in a game component, `Astronaut`: input to velocity, double jump, fall cap, landing, drone hits, death zones and shuttle riding | The rules stay with the object they govern and are testable headless. The engine has no grounded flag, so landing is read from the collision callbacks: after the push, feet within 0.5 px of a surface's top mean standing on it. Rejected: a `Behavior` lambda with the state spread across the game class. *Revised for Milestone 4 (agent):* drone hits and death zones moved out of the component into CollisionEvent handlers (SS-15); landing stays here | One local astronaut per world; remote astronauts are plain synced objects |
| SS-05 | *(agent)* Carry riders in the game: after `NetworkWorldSync::apply()`, move the astronaut by its shuttle's change in position, letting go of moves over 32 px and detaching while paused | Synced shuttles have no velocity, and the engine deliberately has no rider carry. Rejected: adding carry to the engine, which would move Apex's and Pokemon Hunt's riders twice (engine decision M3-D04) | Riders move in the same steps as the snapshot poses, which are not smoothed |
| SS-06 | *(user)* Make shuttle 2 a vertical lift and drone 12 circular, so the moving objects use three patterns: horizontal, vertical and circular | Part 2 requires at least two movement patterns; before the port, all five moving objects went sideways. *(agent)* The lift runs between deck 2 and deck 3, replacing the sideways shuttle that bridged them. The drone circles above deck 2, clear of anyone standing on it, relay 2, cargo 3 and lift riders, so only jumps there need timing | Positions were chosen and checked by tests, not yet tuned by playing |
| SS-07 | *(user)* Cut a hull breach into the floor, with a hidden death-zone trigger inside it and a catch-all below the station | Part 2 requires a hidden death zone that collision detection triggers; before the port, the floor spanned the whole station, so nobody could fall. *(agent)* The breach is under deck 2, between spawn points and clear of cargo, so it is reached along the floor and is 80 px wide, an easy jump. Falling in costs no life, matching the old fall rule. Rejected: invisible hazards on the decks, which players could not see coming | One more hazard on the west floor |
| SS-08 | *(agent)* Skip the astronaut's physics and the collision pass while paused or unable to move | The server's shuttles keep moving while a player is paused; skipping the pass keeps the old behaviour that a paused astronaut stays still and cannot be hurt | A paused astronaut on a shuttle can be left behind and fall on resume, as before |
| SS-09 | *(agent)* Draw decks, shuttles, crates, pads and switches through `renderWorld()` and draw detail over them; draw astronauts and drones as multi-part sprites with no `Renderable` | The engine renders the simple shapes in draw-layer order, while sprites with gaps cannot be a single filled rectangle | The bulkhead is drawn by the game so its open state can show as rails |
| SS-10 | *(agent)* Keep drones as server-authored platforms that the game treats as hazards by id | The engine synchronizes only players and platforms (engine decision M3-B07), so a moving hazard must travel as a platform | Adding another kind of networked object needs engine work |
| SS-11 | *(agent)* Give the three kinds of deck their own colour schemes (hull floor, west decks, east wing) | Part 2 asks for static platforms with distinct combinations of position, size and colour; position and size already differed, colour now does too. *Revised 2026-10-21 (agent):* in screenshots the first west-deck blue was hard to tell from the navy hull, so the west decks are now violet, beside navy hull and teal east wing | — |
| SS-12 | *(agent)* Keep every spawn point out of every drone's and shuttle's travel area | Three floor spawns sat inside drone 11's lane, so players with ids 5–7 (common after reconnects) were hit the moment they spawned, and again after every respawn. They moved to the floor east of the lane and onto deck 2. A test now checks every spawn against every moving object's travel | Players beyond the first four start further east or on deck 2 |
| SS-13 | *(agent)* Mark the hull breach with hazard stripes on both edges, a red glow and a "HULL BREACH" sign | The death zone itself stays hidden, but most of the floor is under the HUD bar, so the gap was only a 7 px notch that players could not see coming | — |

## Milestone 4: events, revive, gravity flips, alarms and replay

| ID | Decision | Rationale and rejected alternatives | Accepted limitation |
| --- | --- | --- | --- |
| SS-14 | *(user)* Unique features: co-op revive, gravity flip panels, crew-wide station alarms and a mission debrief replay | Part 2 asks for an application distinct from teammates. Apex already has crumbling platforms, a Shift-chord dash and a death kill cam, so Starfall leans on what it already is: a co-op game. Rejected (agent options): oxygen sharing, linked teleport pads, magnetic boots | More to explain to new players; the HUD carries the prompts |
| SS-15 | *(agent)* Put the event-driven gameplay in an SDL-free `StationEvents` class in the rules library (in `StarfallSalvageLevel.cpp`, so the game adds no source to the shared build) | The whole chain (collision, death, down, revive, spawn, alarms) is tested headless with a hand-stepped clock. Rejected: handlers inside the SDL game class, testable only with a window | The game class still wires input, networking and drawing |
| SS-16 | *(agent)* Landing stays a collider callback; gameplay touches (drones, death zones, crates, gravity panels) are CollisionEvents | Landing must be known within the same collision pass, every frame, before anything reads `grounded`; events are dispatched after the update. Gameplay reactions do not need that timing. Rejected: moving landing to events too, which would be a frame late | Two mechanisms for collisions, split by purpose |
| SS-17 | *(agent)* `SpawnEvent` is the only place the astronaut is placed, and only events this machine raised (origin 0) change it | One code path for respawns, beacon recalls and revives. Remote DeathEvents and ReviveEvents only produce banners, except a revive naming this player | — |
| SS-18 | *(agent)* A drone death costs a life and leaves the astronaut down; with lives left a 10 s beacon recalls it, at 0 lives only a teammate (or R when alone) can | Reviving matters without making solo play impossible. Rejected: no auto-recovery, which would strand a solo player. *Revised 2026-11-09 (user, on agent suggestion):* replaced by SS-29 | A downed player waits up to 10 s |
| SS-19 | *(agent)* Revive with an S + E input chord, held 1.2 s within 60 px of a downed teammate | "Crouch and use" reads naturally and is distinct from Apex's Shift chords. The rescuer raises `ReviveEvent{target}`; the target's own client respawns itself at the rescuer's position, found through `NetworkWorldSync::remotePlayer(origin)` because the bridge removes object ids from remote events. *Revised 2026-11-09 (agent):* Down + E is a second chord with its own name, because the engine reports each chord's presses and releases separately; sharing one name let releasing Down cancel a revive still held with S + E | Needs client-server mode: events are text and client-server only |
| SS-20 | *(agent)* Share DeathEvent, ReviveEvent and StationAlarmEvent over the network; keep cargo, gate, relays, gravity and downed state in the player data string | One-off happenings suit events; continuous state suits snapshots, which a late joiner also receives. Rejected: sending cargo pickups as events, which a player joining later would never see | The data string grew from 6 to 10 characters, so older clients cannot read it |
| SS-21 | *(user)* Gravity flip panels as the signature mechanic; *(agent)* a station ceiling, panels on deck tops and on the ceiling, and a crate hanging over the breach | Ceilings become a second set of routes. The engine's per-object `Gravity` makes flipping a sign change, and collisions already push from any side, so undersides work as floors. The breach crate is reachable by jumping, but the safe route is the ceiling. A 0.4 s cooldown stops panel flicker | Flipped players cannot use relays or gate switches, which sit on deck tops |
| SS-22 | *(user)* No shared-engine changes for this game; *(agent)* station alarms come from an alarm director: the connected player with the lowest id runs the alarm clock and raises `StationAlarmEvent`, which the server relays to everyone | The engine's server only relays player events and cannot raise its own, and the engine is not to be changed. A lowest-id director needs no election messages: every client sees the same player list, and when the director leaves the next lowest takes over with a fresh interval. Rejected: the first version, which added `NetworkServer::raiseEvent` to the engine (reverted); server-authored alarms through a fake platform's position, which would misuse platform state | Alarms follow the director's game time, so they stop while the director is paused or watching a replay; a director started with `--no-alarms` turns them off for everyone |
| SS-23 | *(agent)* An alarm becomes a local AlarmBeginEvent and AlarmEndEvent raised with delays on game time; overlapping alarms of one kind are counted | Delays reuse the EventManager's timeline: a paused player's alarm waits, a slowed one stretches. Counting keeps one alarm's end from cutting another short. Alarm numbers are clamped because they arrive from the network | Players' alarms drift apart by however long each paused |
| SS-24 | *(agent)* Gravity Failure reverses whatever the panels set and disables them; Blackout darkens everything but light around astronauts and relay pads | Reversal keeps the panel state meaningful afterwards. The blackout is drawn as 16 px tiles of translucent darkness, since SDL's renderer has no stencil | The light edge is blocky |
| SS-25 | *(user)* Replay on mission success; *(agent)* always record the last 10 s, play it at 0.5× as a "mission debrief" once per session, plus F9 to F12 for manual clips | Apex replays on death; Starfall replays the team's success. F9 to F12 avoid Apex's F5 to F8. Astronauts and drones have no `Renderable`, so a frame-state hook stores their positions, flips and downed state each frame for the replay to draw | The replay draws decks with the hull trim; after a manual clip is played, rolling recording resumes and the clip is gone; if a stopped F9 clip is still waiting when the mission completes, nothing of that moment was recorded, so the debrief is skipped; if an F9 recording is still running, the debrief plays that clip (up to 60 s), which also ends on the win, because ReplaySystem cannot shorten a recording before it plays. *Revised 2026-11-09 (agent):* the debrief is skipped whenever nothing is recording (a replay playing or just ended, or a stopped F9 clip waiting), so finishing during or just after an F10 replay no longer replays the older frames under a "REPLAY" title |

| SS-26 | *(agent)* Close the loopholes found in review: a ReviveEvent counts only if the rescuer is within 2 × `reviveRange` of the target; only the current alarm director (or this player itself) can set off an alarm; drones raise "stay" collisions so a shield that runs out inside one still ends in a hit; spawns and alarm begin/end events are `EventPriority::High` so a full queue never drops one (a lost alarm end would leave the alarm on for good) | Remote events are untrusted: the server sets `origin` honestly, but the arguments are the sender's. Rejected: trusting the target's own copy of the rescuer's claim alone, which a modified client could fake from anywhere | A rescuer who walks away between pressing and arrival (rare at 1.2 s holds) can see a revive refused |
| SS-27 | *(agent)* Latch mission completion, and keep publishing the relay a player held while it watches a replay | The debrief pauses the player who first sees all three relays held, which let go of its relay; its release could reach the server before the others saw all three held, so they never completed. Rejected: shortening the debrief or delaying it, which only narrows the race | The completion banner stays up once reached |
| SS-28 | *(agent)* `R` restarts a downed player with no lives whenever no teammate can help (none online, or all down), or after `restartWaitSeconds` (20 s) of game time down; manual replay clips keep their last 60 s; `Esc` and `F1` also go through InputEvent | Two downed players at 0 lives could otherwise never recover, and an idle or absent-minded teammate could strand a player for good; an F9 left running grew without limit; every control now uses the event system. Rejected: restarting at once whenever down, which would make reviving pointless. *Revised 2026-11-09 (user, on agent suggestion):* the `R` restart is removed by SS-29; the 60 s clip limit and InputEvent controls stay | A player can skip waiting for a slow rescuer after 20 s |
| SS-29 | *(user, on agent suggestion)* Drop lives and the `R` restart: a drone hit always leaves the astronaut down until a teammate's revive or the 10 s beacon, which always runs | The old rules (lives, a beacon only with lives left, `R` when no teammate could help or after 20 s) needed a "can a teammate help" check and were hard to explain in play. A fixed beacon cannot strand anyone, so the restart cases disappear, and a revive still matters because it saves time and returns the player where the rescuer is, not at the spawn point. Rejected: keeping lives with a longer recall at 0 lives, which keeps a rule with little effect | A drone hit costs only time; the mission has no timer or game over, so hits never end a run |

## Verification

### Milestone 4

`starfall-salvage-tests` also checks, through a real `EventManager` and the
station:
- flipped gravity: falling to the ceiling, standing on it, jumping downward,
  falling back, and respawning with normal gravity
- the breach DeathEvent respawning at once, without going down
- a crate collected once on touch
- a floor panel flipping gravity, walking the ceiling collecting the breach
  crate, and the ceiling panel flipping back down to the floor
- a drone death leaving the astronaut down and frozen, the beacon recall after
  10 s, a remote revive respawning it at the rescuer's side, and a third hit
  still getting the full beacon (no limit on hits)
- holding the revive chord raising exactly one ReviveEvent for the right
  teammate, and none for a teammate who is not down or out of range
- a remote death showing only a banner
- a shield running out inside a drone ending in a hit
- a revive from a rescuer across the station, or from a player the client does
  not know, refused; one from beside the downed player accepted
- an alarm from a player who is not the director ignored, and this player's
  own alarm (as director) accepted
- the director's gravity alarm warning first, flipping gravity after 2 s of game
  time and restoring it after 6 s; a blackout's begin and end; an unknown alarm
  ignored

Breaking the panel handler, removing the revive distance check or accepting
alarms from any sender each makes the matching check fail. The game
changes no shared-engine file. A server with two clients was run to confirm
both connect and see each other.
Game feel, alarm timing and the debrief still need a round played by people.

### Milestone 3


The `starfall-salvage-tests` target checks the Part 2 objects and the
astronaut's physics through the engine's real collision pass:
- at least three distinct static platforms and three colours
- hidden spawn points and death zones, with no spawn point in any drone's or
  shuttle's path
- horizontal, vertical and circular movement
- the circling drone's orbit clear of anyone standing on deck 2 or riding the
  lift, and the lift's travel clear of every deck
- standing still, jumping and double jumping
- landing half over an edge at terminal speed
- the breach respawn, drone damage with invulnerability, shuttle riding and
  riding the lift up and down
- the closed bulkhead blocking and the open one letting the astronaut through

Removing the fall-speed cap makes the edge-landing check fail, the drone's
first placement (centre x 590) fails the orbit check because it would have hit
riders at the lift's left edge, and the original spawn points fail the
spawn-path check.

Running the server with three clients showed every client seeing the other
two, and the two remaining clients dropping to one remote player when the
third closed. Peer-to-peer mode was also run with the server supplying the
shuttles and drones.

A harness that ran the real game loop with scripted keys and saved
screenshots confirmed spawning, standing without drift, walking, jumping,
double jumping, cargo pickup, drone hits with invulnerability, and falling into
the breach without losing a life. It also measured 50 loops per second with
the GPU renderer after the pacing fix (48 before). Game feel still needs a full
round played by people.
