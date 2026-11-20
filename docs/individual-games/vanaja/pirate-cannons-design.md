# Pirate Cannons: design decisions

Pirate Cannons is a Gorillas-style artillery game against the computer on the
shared engine, built for Milestone 5. This document records the
game-specific decisions; shared-engine decisions live in the engine's own
decision record.

Each entry says who made the decision (*user* or *agent*), why, what was
rejected and what limitation was accepted.

| ID | Decision | Rationale and rejected alternatives | Accepted limitation |
| --- | --- | --- | --- |
| PC-01 | *(user)* A Gorillas game with pirate ships, one player against the computer | Section 4 asks for another game that is not a platformer; turn-based artillery plays very differently from Garden Defense. Rejected: two players on one keyboard, and offering both modes | The computer's aim is the main source of challenge |
| PC-02 | *(agent)* The computer searches angles and powers with `simulate()`, which steps the flight in the same order the World runs it (wind as a `Behavior`, then `Gravity`, then `Motion`), and picks the shot that comes down nearest a target beside your ship | Matching the World's update order means an aimed shot lands in the real game too; a test fires an aimed shot through the World and checks that it hits. Rejected: the closed-form range formula, which ignores wind and the island | The search runs once per computer turn, about 3,000 short simulations |
| PC-03 | *(agent)* The computer aims at a point beside your ship, 140 px off at first and 35 px closer after each miss (never under 20 px) | A perfect computer would win every exchange; a fixed error would never improve. Rejected: random noise on angle and power, whose effect on the landing point depends on the shot | It may need several turns before it can hit |
| PC-04 | *(user)* The chord is Left + Right + Space, which fires a chain shot that splits into three at its peak, twice per match | All three keys already set the power or fire; together the power change cancels out. *(agent)* Key events arrive before chord events in a frame, so `Space` is ignored while Left and Right are both held (`plainShotAllowed`), and the chord fires instead | Pressing Space before the other two fires a normal shot |
| PC-05 | *(agent)* One pooled `World` of 96 slots holds every object; splinters, splashes and feathers use `tryCreate()` and only while more than `gameplayReserve` (16) slots are free | The reserve covers two ships, the island, the sea, two rounds' gulls (the old ones wait for the next flush) and a chain shot with its three splits, so gameplay objects always fit. A sinking ship asks for 90 splinters, more than the cosmetic budget, so the skipping shows during play | Cosmetics are skipped near the limit; `F3` shows how many |
| PC-06 | *(agent)* Gulls fly on `PathMover` paths, two straight and two circular, and a hit gull is gone until the next round | Moving obstacles change which shots are safe and reuse the engine's shared path movement in both of its shapes | Gull positions make some shots luck-dependent |
| PC-07 | *(agent)* A cannonball is deactivated the moment it hits something; ships are created before the sea | One ball can touch a ship and the sea in the same frame. Deactivating stops a second hit counting, and the World reports pairs in creation order, so a hit at the waterline counts as a ship hit | — |
| PC-08 | *(agent)* A turn ends when no cannonball is left in the air; the next round is a SpawnEvent raised with a delay on game time, tagged with the match generation | Works the same for one ball and for a chain shot's three. The delay waits while paused, and `restart()` drops a round queued by the old match | — |
| PC-09 | *(agent)* The loop is paced at 125 Hz in `handleInput`, as in Garden Defense | `Engine::run` has no frame cap, and the computer's simulation uses the same 8 ms step | A frame that runs long takes a longer step than the simulation assumed |
