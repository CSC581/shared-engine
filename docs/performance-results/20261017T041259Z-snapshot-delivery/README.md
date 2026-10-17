# Binary Full Snapshot versus Property Delta Results

## 1. Decision

Property-delta delivery remains an explicit binary client-server option. Full snapshots remain the default. The delta implementation passed every correctness and recovery check and reduced encoded payload in every measured condition, but it increased mean round-trip latency in every condition. The trade-off is useful when bandwidth or unchanged opaque player data matters, but the measurements do not justify applying it to every game automatically.

## 2. Method

The Release benchmark used the production `NetworkClient`, `NetworkServerHost`, ZeroMQ sockets, binary codecs, per-client worker threads and moving platforms on an Intel Core i7-12650H. Each client offered 60 sequential position requests per second. The matrix covered binary full snapshots and binary property deltas, 1/2/4 clients, 10/100/500 platforms, 0/512-byte opaque player data and five repetitions. Each run excluded join, scene stabilization and 100 warm-up updates, then measured 1,000 updates per client.

The collector validated 180 runs and 420,000 RTT samples. Every measured update received one reply, used one request and reply frame, matched the server traffic counter, preserved the expected player and platform counts, and returned the submitting player's exact position and opaque data. No client disconnected or reported a baseline mismatch. Payload counts include ZeroMQ frame contents only. The baseline-memory value is the encoded full-snapshot size per client, not allocator-resident memory.

## 3. Results

Property deltas reduced encoded request-plus-reply payload per update by **26.4% to 73.7%**. The smallest reduction occurred with one client, ten platforms and no opaque data, where payload fell from 315.0 to 231.9 bytes. The largest reduction occurred with four clients, ten platforms and 512-byte player data, where payload fell from 2,955.2 to 776.9 bytes. This pattern is consistent with the delta omitting unchanged copies of every player's opaque data.

Both strategies sustained the offered rate. Cell means ranged from 60.051 to 60.057 updates per second per client. Property deltas therefore did not cause missed work or saturation at the tested load.

Delta construction, serialization, decoding and reconstruction increased mean RTT by **3.7% to 115.0%** relative to full binary snapshots. In the four-client, 500-platform, zero-data cell, mean RTT increased from 0.804 to 1.729 ms and p95 increased from 1.304 to 2.478 ms. The relative increase is material, but the absolute mean remained below 2 ms and well below the 16.67 ms request period. The experiment measures the combined end-to-end cost and does not isolate diff construction from encoding or client-side application.

The encoded baseline proxy ranged from 251 to 12,180 bytes per client. Host baseline storage therefore grows with both client count and world size, as expected from the O(clients × world size) design. These byte values should not be reported as resident heap usage.

## 4. Recovery evidence

The separate recovery matrix forced a complete resynchronization every 30 measured updates. Its 36 runs and 10,080 RTT samples completed without failures. Delta clients sent 126 resynchronization requests, received 126 complete snapshots, applied 4,914 ordinary deltas and reported zero baseline mismatches. Deterministic host tests separately cover stale acknowledgements and rejoin baseline resets.

## 5. Findings and limits

1. **Property deltas are correct under the measured production path.** All steady-state and forced-recovery runs passed, and mixed full/delta operation plus disconnect handling remain covered by integration tests.
2. **Property deltas trade CPU latency for bandwidth.** Every cell transferred fewer encoded bytes, while every cell had a higher mean RTT.
3. **Opaque player data changes the value proposition.** Four clients, ten platforms and 512-byte player data produced the largest measured reduction at 73.7% with only a 6.6% mean-RTT increase.
4. **Large moving scenes expose delta-processing cost.** Four clients and 500 platforms produced a 2.15× mean-RTT ratio, although the absolute delta mean remained 1.729 ms and the offered 60 Hz rate was sustained.
5. **Full snapshots should remain the default.** Games should opt into deltas when their bandwidth profile resembles the favorable cells and accept the per-session baseline memory and additional processing.

These results are a single-machine loopback baseline. They do not measure WAN loss, transport framing, resident memory, energy, or isolated diff/apply CPU time. Raw runs and latency samples remain under `build/performance-results/20261017T041259Z-snapshot-delivery/`.
