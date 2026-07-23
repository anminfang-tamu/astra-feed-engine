# Historical performance audit

This file is retained only as a record of the review that motivated branch
`6-redesign-order-book-data-structure`. The authoritative architecture,
correctness invariants, storage plan, and acceptance procedure are in
[`docs/order-book-redesign.md`](docs/order-book-redesign.md).

## Findings resolved by the redesign

| Historical finding | Current resolution |
| --- | --- |
| A centered 65,536-tick price array rejected far valid prices. | `PriceLevelStore` splits the complete `uint32_t` price key into `page_index` and `level_index`; there is no configurable price window. |
| Parser-side order state and per-book order maps duplicated lookups and ownership. | One process-wide `OrderTable` owns each live order exactly once. The direct tier indexes by order reference; the 64-bit fallback has a fixed four-slot probe bound. |
| Per-book order pools, hashes, FIFO links, and price arrays produced a large scattered working set. | Books are small preallocated descriptors over shared fixed arenas. Aggregate L2 processing needs no FIFO links. |
| Price lookup used multiple dependent tree/pool translations. | A flat `(stock_locate, page_index)` root selects one dense 2 MiB price page; existing-order mutations cache the page handle and level index in a 16-byte `OrderState`. |
| Add and replace could partially mutate state when a destination failed. | Order and price resources are preflighted and mutation failures are typed and fail closed. |
| Aggregate quantity used a 32-bit representation. | Every price level stores a 64-bit aggregate quantity. |
| Book creation and mutation could allocate or fault unpredictably. | All capacities are fixed at startup; named arenas can be prefaulted and NUMA-verified before the feed is released. |
| There was no representative order-book performance gate. | Synthetic bounded-work benchmarks, full ITCH replay, semantic and physical digests, a disassembly audit, and a controlled branch-6 acceptance harness are checked in. |
| Session/lifecycle handling could stop before late directory or cleanup traffic. | Repeated and new `R` messages remain supported after System Hours start; order `E/C` remain independent of system-event `E`; only system-event `C` and a subsequent exact Mold end marker terminate normally. |

## Work still requiring production evidence

- Run at least five fresh branch-6 processes through the acceptance harness on
  the same isolated x86 EC2 core and NUMA node. Every run must meet the 150 ns
  p50 target and explicitly approved absolute p99/p99.9 ceilings.
- Build deployment capacities from several representative sessions, retain
  the profiler evidence, add explicit headroom, and validate the resulting
  memory plan at startup. The checked-in 2019 trace profile is an acceptance
  fixture, not a universal production capacity forecast.
- Validate the complete live transport path separately. `recvmmsg`, socket
  buffering, redundant-feed recovery, and any future DPDK path are outside
  the book-only latency distribution and must not be conflated with it.
- Retain separate `perf stat` evidence for cache/TLB misses, faults, context
  switches, and migrations. Instrumentation runs are diagnostic and are not
  members of the accepted latency population.

The historical 310 ns observation motivated the redesign, but it is not a
like-for-like repository result. The controlled branch-6 run establishes the
current implementation's performance.
