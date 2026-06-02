# Performance Improvements To Consider

This is a static code-scan summary focused on making the project closer to a real low-latency market data / HFT-style system. The order below is roughly prioritized by expected impact.

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

## 3. Preallocate Order Book Memory Before Processing

Current path:

- `OrderBook::ensureLevelStorage()` lazily resizes bid/ask level vectors on the first add.
- `OrderBook::allocateOrder()` grows `order_pool_` with `push_back()`.
- `OrderBook::freeOrder()` may grow `free_list_`.

Relevant files:

- `include/astra/book/OrderBook.hpp`
- `src/book/OrderBook.cpp`

Why it matters:

Dynamic allocation and exception paths should not exist in the live message path. Even rare allocations create tail-latency spikes.

Suggested direction:

- Pre-size bid/ask levels in the constructor or warmup phase.
- Pre-size `order_pool_` to `kOrderPoolSize`.
- Pre-size `free_list_` to `kOrderPoolSize`.
- Populate the free list up front and remove hot-path `try/catch` allocation handling.

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
- With 16-byte entries, this is roughly 64 MiB per symbol for the order ID map alone.

Relevant files:

- `src/book/OrderBook.cpp`
- `include/astra/utils/OrderIdMap.hpp`

Why it matters:

This is acceptable for one or a few active symbols, but it does not scale to a broad market universe.

Suggested direction:

- Make order map capacity configurable per symbol.
- Use liquidity tiers for active symbols.
- Consider one global order-reference map that stores `(symbol_id, order_index)` if the feed guarantees globally unique order refs.
- Keep current low load factor for the most active symbols only.

## 7. Prebuild Books From Reference Data

Current path:

- `BookManager::getOrCreateOrderBook()` dynamically resizes `book_by_symbol_id_`.
- It allocates an `OrderBook` on the first add for a symbol.

Relevant file:

- `src/book/BookManager.cpp`

Why it matters:

Real feed handlers usually know the instrument universe from reference data before live processing starts. Dynamic book creation in the hot path creates avoidable allocation and branching.

Suggested direction:

- Load symbol/reference data before starting the engine.
- Build compact symbol IDs.
- Allocate all expected books during startup.
- Keep `getOrCreateOrderBook()` for replay/dev mode only.

## 8. Separate Replay Parser Optimization From Live Feed Optimization

Current path:

- `ItchParser` uses `std::unordered_map` for order state and execution state.
- `SymbolTable` constructs temporary `std::string` values for lookups.
- Replay source uses a `std::deque` for pending messages.

Relevant files:

- `src/replay/itch/ItchParser.cpp`
- `include/replay/itch/ItchParser.hpp`
- `src/replay/SymbolTable.cpp`
- `src/replay/itch/ItchReplaySource.cpp`

Why it matters:

This is fine for correctness-first replay, but max-speed replay can become CPU and allocation heavy.

Suggested direction:

- Reserve hash maps up front when replay file size or expected order count is known.
- Use a fixed 8-byte symbol key instead of repeatedly constructing strings.
- Replace replay `unordered_map` with a flat/open-addressing map if replay throughput matters.
- Replace `std::deque` pending queue with a tiny fixed ring buffer; replace events only emit one or two messages.

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

1. Preallocate order book memory.
2. Decode packet fields once into a normalized message struct.
3. Remove duplicate `OrderIdMap` probing in `addOrder()`.
4. Add benchmark coverage for book and decoder paths.
5. Add a concrete/inlined live engine path.
6. Add batched UDP receive.
7. Fill in DPDK or another kernel-bypass receive path.
8. Optimize replay parser data structures after the live path is measured.
