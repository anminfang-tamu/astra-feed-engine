# Performance Improvements To Consider

This is a static code-scan summary focused on making the project closer to a real low-latency market data / HFT-style system. The order below is roughly prioritized by expected impact.

## Recent Runtime Finding: Order Book Data Structure Needs Redesign

The latest dual-feed EC2 runs show that gap recovery itself is not the root problem.

- At `5000 pkt/s` per line with `20` messages per packet, the receiver stayed healthy:
  - `channel_status_name=Good`
  - `line_a_kernel_drops=0`
  - `line_b_kernel_drops=0`
- At `10000 pkt/s` per line, both socket queues overflowed:
  - `line_a_kernel_drops > 0`
  - `line_b_kernel_drops > 0`
  - later packets were buffered, but the missing sequence was dropped by both redundant feeds, so the gap could not recover.
- `OrderBook` creation now touches all owned pages by default. Creating books for the full directory can still get the process killed because about `8713` default-sized books alone are roughly `70 GiB` committed before hot-symbol tiers, parser maps, gap buffer, socket queues, and OS memory.

The practical conclusion is that the current single-thread path is overloaded by the book-update workload:

```text
recv A/B -> duplicate filtering -> MoldUDP decode -> ITCH parse -> order state -> OrderBook update
```

The parser byte decoding is not the main issue. The expensive part is the data structure work behind each book-relevant ITCH message:

- `ItchParser` keeps parser-side order state in `orders_`.
- `ItchParser` keeps execution state in `executions_by_match_`.
- `OrderBook` also keeps an `OrderIdMap`.
- Each `OrderBook` owns large per-symbol arrays for orders, free list, price levels, and order-id indexing.
- Many operations touch multiple large hash/index structures, causing random memory access, cache misses, TLB pressure, and large committed memory when warmed.

The order-book data model should be changed before expecting `10000 pkt/s` per line to be reliable on one thread.

Suggested direction:

- Remove duplicated order state between `ItchParser` and `OrderBook` where possible.
- Consider a single global order-id table mapping `order_id -> symbol/order slot` instead of one large `OrderIdMap` per symbol.
- Store enough order metadata in one place to support execute/cancel/delete/replace/broken-trade without a second hash lookup.
- Avoid touching or allocating full worst-case order-book memory for every listed symbol.
- Use symbol liquidity tiers aggressively, and allocate hot capacity only after evidence that a symbol needs it.
- Consider sharding book updates by symbol only after the single-thread data structure is leaner; threading will not fix excessive random memory pressure by itself.

## 1. Remove Hot-Path Virtual Calls

Current path:

- `MarketDataEngine::run()` calls `source_.next(packet)` through `IMarketDataSource`.
- It calls `publisher_.publish()` through `IPublisher`.
- Decoder, sequencing, book routing, and publishing all sit behind small function calls in the per-message loop.

Relevant files:

- `src/engine/MarketDataEngine.cpp`
- `include/astra/source/IMarketDataSource.hpp`
- `include/astra/publish/IPublisher.hpp`

Why it matters:

Virtual dispatch is small but paid on every message. In a real feed handler, the live path should be concrete enough for the compiler to inline.

Suggested direction:

- Keep interfaces for tests and apps.
- Add a concrete or templated live engine path, for example `MarketDataEngine<UdpReceiver, NullPublisher>`.
- Avoid calling `publisher_.publish()` every message unless there is actual publish work.

## 2. Replace One-Packet-Per-Syscall UDP Receive

Current path:

- `UdpReceiver::next()` uses one `recv()` syscall per packet.
- It timestamps with `nowNs()` after receive.

Relevant file:

- `src/source/UdpReceiver.cpp`

Why it matters:

One syscall per packet will become a bottleneck at high feed rates. Software timestamps from `std::chrono` also add overhead and do not represent true NIC receive time.

Suggested direction:

- Use `recvmmsg()` batching for kernel UDP.
- Add packet batch APIs such as `nextBatch(PacketView* packets, size_t max)`.
- Use kernel/NIC timestamping where available.
- Track receive drops with socket counters such as `SO_RXQ_OVFL`.
- Fill in the currently empty DPDK path for a true kernel-bypass option.

## 3. Redesign Order Book Memory Layout

Current path:

- `OrderBook` uses a fixed mmap-backed `order_pool_`, vector-backed `free_list_` and bid/ask levels, and a fixed mmap-backed `OrderIdMap`.
- Default order capacity is `64k` orders per symbol.
- Hot tiers can grow to `1M` or `4M` orders per symbol.
- Book creation commits/touches these structures by default and can OOM if done for the full symbol universe.

Relevant files:

- `include/astra/book/OrderBook.hpp`
- `src/book/OrderBook.cpp`
- `include/astra/constants/SymbolCapacity.hpp`
- `include/astra/utils/OrderIdMap.hpp`

Why it matters:

The current structure avoids hot-path allocation, but it does so by reserving very large per-symbol arenas. That is good for a small set of hot symbols and bad for the full NASDAQ universe. Touching all pages can commit tens of GiB. Even without touching all pages, the steady-state update path does random probes into large per-symbol structures.

Suggested direction:

- Keep fixed-capacity structures for hot symbols, but do not give every symbol a large worst-case arena.
- Split symbol capacity into smaller default tiers and promote only active symbols.
- Consider compact per-symbol book storage where quiet symbols do not allocate full bid/ask level arrays.
- Consider a shared/global order-id table to avoid one large hash table per symbol.
- Keep `touch` targeted to known hot symbols only.

## 4. Decode Message Fields Once

Current path:

- `MarketDataMessageView` loads each field with `memcpy`.
- Book logic repeatedly calls accessors such as `msg.qty()`, `msg.side()`, `msg.orderId()`, and `msg.price()`.

Relevant files:

- `include/astra/protocol/MarketDataMessageView.hpp`
- `src/codec/BinaryDecoder.cpp`
- `src/book/OrderBook.cpp`

Why it matters:

The current view is safe for packed wire data, but repeated field loads add unnecessary work in the hot path.

Suggested direction:

- Decode once into a normalized stack struct.
- Pass that struct through sequencing, book routing, and book updates.

Example shape:

```cpp
struct DecodedMessage {
  uint64_t seq;
  uint64_t order_id;
  uint64_t price;
  uint32_t symbol_id;
  uint32_t qty;
  MessageType type;
  OrderSide side;
};
```

## 5. Avoid Duplicate `OrderIdMap` Probes On Add

Current path:

- `OrderBook::addOrder()` calls `order_index_.find(msg.orderId())`.
- Later the same function calls `order_index_.insert(order.order_id, order_idx)`.

Relevant files:

- `src/book/OrderBook.cpp`
- `include/astra/utils/OrderIdMap.hpp`

Why it matters:

A valid add currently pays for two hash/probe chains: one to check duplicate order ID, one to insert.

Suggested direction:

- Change `OrderIdMap::insert()` or add a new API that reports duplicate/new in one probe.
- Use that single insert result in `OrderBook::addOrder()`.

## 6. Make `OrderIdMap` Capacity Match Real Symbol Liquidity

Current path:

- Each `OrderBook` owns an `OrderIdMap`.
- Capacity is derived from `kOrderPoolSize * 4`.
- With 16-byte entries, the default `64k` order-capacity book uses about `4 MiB` for the order-id map alone.
- Hot/ultra-hot symbols use much more.

Relevant files:

- `src/book/OrderBook.cpp`
- `include/astra/utils/OrderIdMap.hpp`
- `include/astra/constants/SymbolCapacity.hpp`

Why it matters:

This is acceptable for one or a few active symbols, but it does not scale cleanly to a broad market universe. It also duplicates parser-side order state work.

Suggested direction:

- Make order map capacity configurable per symbol.
- Use liquidity tiers for active symbols.
- Consider one global order-reference map that stores `(symbol_id, order_index)` if the feed guarantees globally unique order refs.
- Keep current low load factor for the most active symbols only.
- Measure whether `ItchParser::orders_` can be merged with or replaced by `OrderBook` state.

## 7. Prebuild Books From Reference Data

Current path:

- `ItchParser::handleStockDirectory()` registers each stock locate and `SS`-time creation calls `BookManager::getOrCreate()`.
- `BookManager::getOrCreate()` creates an `OrderBook` for each directory entry, and `OrderBook` creation touches its pages by default.

Relevant file:

- `src/book/BookManager.cpp`
- `src/replay/itch/ItchParser.cpp`

Why it matters:

Real feed handlers usually know the instrument universe from reference data before live processing starts. Preparing books from Stock Directory is the right direction, but touching every book is too memory-heavy with the current structure.

Suggested direction:

- Load symbol/reference data before starting the engine.
- Build compact symbol IDs.
- Allocate metadata for all expected books during startup.
- Allocate or touch full order storage only for hot symbols.
- Keep full `getOrCreate()` behavior for replay/dev mode only.

## 8. Remove Duplicate Parser/Book Order State

Current path:

- `ItchParser` stores order state in `FixedHashMap<OrderState> orders_`.
- `ItchParser` stores execution state in `FixedHashMap<MatchEntry> executions_by_match_`.
- `OrderBook` stores order id to pool index in `OrderIdMap`.
- Add/execute/replace/delete can touch parser-side state and book-side state for the same logical order.

Relevant files:

- `src/replay/itch/ItchParser.cpp`
- `include/replay/itch/ItchParser.hpp`
- `src/book/OrderBook.cpp`
- `include/astra/utils/FixedHashMap.hpp`
- `include/astra/utils/OrderIdMap.hpp`

Why it matters:

The byte parser is simple. The expensive part is repeated random access to large order-state maps. Keeping the same order state in two places increases cache/TLB pressure and makes one-thread throughput worse.

Suggested direction:

- Decide which component owns order state.
- Let `OrderBook` expose enough metadata for executions and broken-trade reversal, or move book indexing into one shared order table.
- Avoid parser-side `orders_` lookup when the same order must be found again inside `OrderBook`.
- Keep `executions_by_match_` only if broken-trade correctness requires it; otherwise make it optional for performance runs.

## 9. Add Real Benchmarks

Current path:

- Benchmark files exist but are empty.

Relevant files:

- `benchmarks/OrderBookBenchmark.cpp`
- `benchmarks/EndToEndBenchmark.cpp`
- `benchmarks/DecoderBenchmark.cpp`
- `benchmarks/UdpReceiverBenchmark.cpp`

Why it matters:

Performance work needs baselines. Without benchmarks, changes can improve style while hurting throughput or tail latency.

Suggested direction:

- Add order book microbenchmarks for add/modify/delete/trade.
- Add decoder benchmarks for packed wire messages.
- Add end-to-end replay benchmark from packet bytes to book update.
- Track p50, p99, p99.9, max latency, and messages/sec.

## 10. Add A Release/Perf Build Profile

Current path:

- CMake sets warnings but no explicit perf-oriented release options.

Relevant file:

- `CMakeLists.txt`

Suggested direction:

- Use `-O3`, `-DNDEBUG`, and `-march=native` for local performance builds.
- Consider LTO for release binaries.
- Consider PGO after stable benchmark workloads exist.

## Suggested Implementation Order

1. Redesign order-book/order-state storage so each order is indexed once.
2. Reduce default per-symbol memory and make full warm touch targeted to hot symbols only.
3. Remove duplicate `OrderIdMap` probing in `addOrder()`.
4. Add benchmark coverage for book and decoder paths.
5. Increase `SO_RCVBUF` and kernel `rmem_max` to absorb bursts.
6. Add a concrete/inlined live engine path.
7. Add batched UDP receive or a dedicated receive thread.
8. Consider symbol-sharded book-update workers after the single-thread data structure is leaner.
9. Fill in DPDK or another kernel-bypass receive path only after kernel UDP limits are measured.
