# Starfall Salvage: Milestone 3 design decisions

Starfall Salvage is a cooperative three-player platformer on the shared engine.
This document records the game-specific decisions behind its port to the
Milestone 3 component object model. Shared-engine decisions live in the
engine's own decision record; only what is particular to this game is here.

Each entry says who made the decision (*user* or *agent*), why, what was
rejected and what limitation was accepted.

| ID | Decision | Rationale and rejected alternatives | Accepted limitation |
| --- | --- | --- | --- |
| SS-01 | *(agent)* Build every station object with SDL-free factory functions in `StarfallSalvageLevel.cpp`, compiled into the `starfall-salvage-rules` library | The level and the astronaut's physics can be built and simulated in headless tests. Rejected: building objects inside the SDL game class, which only a running window could exercise | The game class still owns drawing, input and networking |
| SS-02 | *(user)* Use the engine's solid collisions for the astronaut, as Apex Ascent and Pokemon Hunt do | One collision model for every game on the engine. Rejected (agent's first recommendation): keeping the hand-written landing resolver as a component, which felt identical to the old game but kept a second collision model | The engine pushes along the axis of least overlap, so a landing with less of the astronaut over an edge than it sank that frame (at most about 12 px) is pushed off the edge rather than landed |
| SS-03 | *(agent)* Run at 50 loops per second (25/50/100 at 0.5×/1×/2×) and cap the fall speed at 600 px/s | Every frame is exactly 20 ms of game time, which keeps the pacing tests exact, and the cap limits how far the astronaut sinks into a deck per frame. Rejected: 60 Hz (16.67 ms is not a whole number of nanoseconds or microseconds, so frame deltas drift), the old 25 Hz (up to ~27 px of sink, almost the astronaut's width) and collision substeps on the main thread | Twice the old message rate at each speed |
| SS-04 | *(agent)* Put the platformer rules in a game component, `Astronaut`: input to velocity, double jump, fall cap, landing, drone hits, death zones and shuttle riding | The rules stay with the object they govern and are testable headless. The engine has no grounded flag, so landing is read from the collision callbacks: after the push, feet within 0.5 px of a surface's top mean standing on it. Rejected: a `Behavior` lambda with the state spread across the game class | One local astronaut per world; remote astronauts are plain synced objects |
| SS-05 | *(agent)* Carry riders in the game: after `NetworkWorldSync::apply()`, move the astronaut by its shuttle's change in position, letting go of moves over 32 px and detaching while paused | Synced shuttles have no velocity, and the engine deliberately has no rider carry. Rejected: adding carry to the engine, which would move Apex's and Pokemon Hunt's riders twice (engine decision M3-D04) | Riders move in the same steps as the snapshot poses, which are not smoothed |
| SS-06 | *(user)* Make shuttle 2 a vertical lift and drone 12 circular, so the moving objects use three patterns: horizontal, vertical and circular | Part 2 requires at least two movement patterns; before the port, all five moving objects went sideways. *(agent)* The lift runs between deck 2 and deck 3, replacing the sideways shuttle that bridged them. The drone circles above deck 2, clear of anyone standing on it, relay 2, cargo 3 and lift riders, so only jumps there need timing | Positions were chosen and checked by tests, not yet tuned by playing |
| SS-07 | *(user)* Cut a hull breach into the floor, with a hidden death-zone trigger inside it and a catch-all below the station | Part 2 requires a hidden death zone that collision detection triggers; before the port, the floor spanned the whole station, so nobody could fall. *(agent)* The breach is under deck 2, between spawn points and clear of cargo, so it is reached along the floor and is 80 px wide, an easy jump. Falling in costs no life, matching the old fall rule. Rejected: invisible hazards on the decks, which players could not see coming | One more hazard on the west floor |
| SS-08 | *(agent)* Skip the astronaut's physics and the collision pass while paused or unable to move | The server's shuttles keep moving while a player is paused; skipping the pass keeps the old behaviour that a paused astronaut stays still and cannot be hurt | A paused astronaut on a shuttle can be left behind and fall on resume, as before |
| SS-09 | *(agent)* Draw decks, shuttles, crates, pads and switches through `renderWorld()` and draw detail over them; draw astronauts and drones as multi-part sprites with no `Renderable` | The engine renders the simple shapes in draw-layer order, while sprites with gaps cannot be a single filled rectangle | The bulkhead is drawn by the game so its open state can show as rails |
| SS-10 | *(agent)* Keep drones as server-authored platforms that the game treats as hazards by id | The engine synchronizes only players and platforms (engine decision M3-B07), so a moving hazard must travel as a platform | Adding another kind of networked object needs engine work |
| SS-11 | *(agent)* Give the three kinds of deck their own colour schemes (hull floor, west decks, east wing) | Part 2 asks for static platforms with distinct combinations of position, size and colour; position and size already differed, colour now does too | — |

## Verification

The `starfall-salvage-tests` target checks the Part 2 objects and the
astronaut's physics through the engine's real collision pass:
- at least three distinct static platforms and three colours
- hidden spawn points and death zones
- horizontal, vertical and circular movement
- the circling drone's orbit clear of anyone standing on deck 2 or riding the
  lift, and the lift's travel clear of every deck
- standing still, jumping and double jumping
- landing half over an edge at terminal speed
- the breach respawn, drone damage with invulnerability, and shuttle riding

Removing the fall-speed cap makes the edge-landing check fail, and the
drone's first placement (centre x 590) fails the orbit check: it would have
hit riders at the lift's left edge.

Running the server with three clients showed every client seeing the other
two, and the two remaining clients dropping to one remote player when the
third closed. Peer-to-peer mode was also run with the server supplying the
shuttles and drones. Feel and visuals still need a full round played on
screen.
