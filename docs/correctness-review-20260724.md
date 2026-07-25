# Whole-project correctness review — 2026-07-24

Wire behavior was checked against Nasdaq's official
[TotalView-ITCH 5.0](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf),
[MoldUDP64](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf),
and
[BinaryFILE](https://nasdaqtrader.com/content/technicalSupport/specifications/dataproducts/binaryfile.pdf)
specifications.

## Review checklist

1. Check the official ITCH 5.0, MoldUDP64, and BinaryFILE wire contracts.
2. Verify the sender reads exact BinaryFILE records and emits valid bounded
   MoldUDP64 packets with monotonic sequencing, A/B identity, heartbeats, and
   explicit end-of-session behavior.
3. Verify the receiver rejects malformed/truncated frames and packets before
   mutation, merges redundant A/B traffic without double application, detects
   gaps/conflicts, and accepts end-of-session only after complete recovery.
4. Verify exact ITCH type/length, timestamp, lifecycle, locate/ticker identity,
   Price(4), and order-reference semantics.
5. Verify every registered symbol gets an independent book and that
   `A/F/E/C/X/D/U` applies transactionally.
6. Verify full-depth bid/ask price levels, best-price and top-ten ordering,
   aggregate quantity/order count, raw-price scale, and mutation timestamp.
7. Search production code for trace-, symbol-, and benchmark-specific
   shortcuts or capacity defaults.
8. Add boundary, malformed-input, lifecycle, multi-symbol, full-depth,
   redundant-feed, sender-wire, and capacity-provenance tests.
9. Certify a newer official full-day trace and derive a checksum-bound storage
   plan before EC2 replay.
10. State what the project still lacks instead of treating a clean historical
    replay as live-exchange production certification.

## Result

No remaining code-level sender, parser, order-book, price-level, or capacity
correctness defect was identified by this review and the local test suites
after the patches below. The live engine has no branch keyed to a ticker, trace
name, record count, or performance target. `md_engine` has no built-in
deployment book capacity: startup requires an
`astra_book_capacity_evidence_v2` manifest plus exact duplicated capacity
values. Historical 2019 constants remain only in the explicitly named
benchmark/test fixture; neither the live engine nor the acceptance harness has
a pinned-profile admission path. The checksum-bound EC2 engine replay remains
required before production-engine reconstruction of the 2026 corpus is
certified.

The review did find and fix concrete correctness issues before reaching that
result: invalid Price(4) in an Order Executed With Price `C` could be detected
after mutation; the lifecycle rejected the 12 valid post-`E` partial cancels
present in the official 2026 trace; and UDP constructor failures could leak
file descriptors before object construction completed. Each has a focused
regression, including sanitizer coverage for socket ownership.

The completed semantic profile certifies the structure and semantic cleanliness
of the exact BinaryFILE and supplies its sizing observations; focused tests
exercise the production parser and book. Only a successful checksum-bound EC2
engine replay can certify production-engine reconstruction of the full corpus.
Even that result would **not** certify live Nasdaq production readiness; the
limitations section below is part of the result.

## Sender and wire format

- The source reads a two-byte big-endian BinaryFILE record length and then the
  exact ITCH bytes. Type and exact official message length are checked.
- Strict mode requires the BinaryFILE zero-length terminator. The explicit
  SC-plus-physical-EOF mode is allowed only for an independently verified exact
  checksum whose last ITCH record is terminal System Event `C`. The sender does
  not hash-bind that mode itself; the launch procedure must verify the approved
  checksum before selecting it.
- MoldUDP64 uses the exact 10-byte session, 64-bit sequence, 16-bit message
  count, per-message 16-bit length, and unchanged ITCH payload bytes.
- A packet is flushed early so its complete UDP payload never exceeds 1,472
  bytes. This fits a 1,500-byte Ethernet/IPv4 MTU without fragmentation.
- Redundant mode constructs each packet once; line A and the dedicated line-B
  thread send byte-identical datagrams and advance one shared sequence.
- Synthetic batching ends a datagram immediately after System Hours `S` and
  Market Hours `Q`. This makes the optional post-`S` pause and `S`-to-`Q`
  pacing take effect before any following ITCH record is sent.
- The ordinary configured packet rate is a strict upper bound: fractional
  nanosecond intervals round upward and late sends rebase instead of producing
  a catch-up burst. Shared-IP redundant mode rejects equal A/B ports.
- Timestamp replay uses one-message synthetic packets between `S` and `Q`,
  preventing a later ITCH record from being released at the timestamp of an
  earlier record in the same packet. The configured grouping is restored
  after `Q`.
- Startup heartbeats default to zero. Periodic count-zero heartbeats are sent
  on configured idle deadlines that precede the next data send. Completion
  sends the configured repeated MoldUDP64 end-of-session packets with the
  configured minimum interval between successful sends.
- Deadline arithmetic is checked or saturated at monotonic-clock boundaries.
  A source error, short record, invalid type/length, invalid first/terminal
  event or completion contract, send failure, or incomplete completion returns
  failure rather than an apparently clean run. Send accounting is
  transactional across configured lines; a partial failure stops replay and
  suppresses EOS. Known completion transitions directly to EOS without an
  ordinary-rate wait or idle heartbeat. Full daily phase validation is
  performed by the receiver and semantic profiler, not by the sender source.

BinaryFILE does not preserve original Nasdaq MoldUDP64 packet boundaries,
session, multicast addresses, arrival times, or A/B skew. The sender therefore
creates a standards-conforming but synthetic unicast MoldUDP64 envelope around
the exact ITCH records.

## Receiver, sequencing, and packet commit

- Kernel receivers reject truncation instead of forwarding a clamped prefix.
  The DPDK path validates Ethernet/VLAN, IPv4 header/length/fragmentation,
  UDP length, destination address, and destination port before exposing a
  payload.
- Scalar UDP drop telemetry is strict when requested: invalid configuration,
  missing Linux `SO_RXQ_OVFL`, or `setsockopt` failure aborts startup. Linux
  batch mode always requires that option. Clean-run evidence requires enabled
  telemetry and zero counters, not an uninstrumented printed zero.
- The decoder first validates complete Mold framing and every nested ITCH
  type/length. A framing/type/length-invalid datagram cannot partially mutate
  the book. Semantic messages are then applied in wire order; if a later
  message fails semantically, earlier valid messages in that packet remain
  applied and the channel becomes terminal-invalid, requiring a fresh process.
- Session changes and sequence overflow are terminal. Ahead heartbeats mark a
  gap. Buffered same-sequence A/B copies must be byte-identical; a conflict
  cannot overwrite the first packet.
- An ahead end-of-session marker can wait for the redundant line to fill a
  missing range. It becomes acceptable only after terminal ITCH System Event
  `C`, healthy exact sequence recovery, and an empty gap buffer.
- The engine exits unsuccessfully unless channel health is `Good`, phase is
  End of Messages, the gap buffer is empty, live orders are zero, and the
  MoldUDP64 end marker was accepted.

## Parser and per-symbol order books

The exact supported ITCH lengths are:

```text
S12 R39 H25 Y20 L26 V35 W12 K28 J35 h21
A36 F40 E31 C36 X23 D19 U35 P44 Q40 B19 I50 N20 O48
```

Unknown types and exact-length mismatches fail. Every message timestamp must be
below 86,400,000,000,000 ns. The daily lifecycle is exactly
`O -> S -> Q -> M -> E -> C`; emergency events do not move that daily phase.
The Nasdaq specification names Order Delete `D` and Broken Trade `B` as
possible after End of System Hours `E`. The official checksum-pinned
2026-06-12 archive also contains 12 valid partial Order Cancel `X` messages,
interleaved with 431,550 Order Delete `D` messages, from
20:00:00.000320516 through 20:00:00.029784092; terminal System Event `C`
arrives at 20:05. The parser therefore admits existing-order `X` and `D`
teardown, accepts `B` as having no current-L2-book effect, and admits the final
zero-live-order `SC` after `E`, while rejecting adds, executions, replaces,
directories, and other administrative messages. Match-number history is not
retained, so a `B` match number is not cross-validated.

Every valid nonzero Stock Locate registered by `R` gets an independent,
persistent book. Directories received before `S` become empty books at `S`; a
new directory received later in the open lifecycle gets a book immediately.
Locate zero and unregistered locates have no book. Locate/ticker identity is
immutable, duplicate tickers are rejected, and the eight stock bytes in
`A`/`F` must match the registered directory entry.

Nasdaq order references are treated as day-unique, including references of
deleted, fully executed, cancelled, and replaced orders. The CRUD closure is:

- `A`/`F`: add a new referenced order;
- `E`: reduce by executed quantity;
- `C`: validate the execution Price(4), then reduce the resting order;
- `X`: reduce by cancelled quantity without deleting the final quantity;
- `D`: remove the referenced order;
- `U`: atomically remove the old reference and create the new reference at the
  replacement price and quantity.

Missing/duplicate references, side/locate mismatch, over-execution,
over-cancel, invalid Price(4), aggregate overflow/underflow, or capacity
failure does not partially mutate logical state. A parser-visible mutation
failure terminally invalidates the channel.

## Price-level contract for a strategy

Each symbol book retains full internal bid and ask depth across the valid raw
Price(4) domain `0..2,000,000,000`. A level is keyed by `(locate, side,
raw_price)` and contains a 64-bit aggregate quantity and 32-bit live order
count. Bids sort high-to-low and asks low-to-high. A level is removed when its
last order leaves; deeper levels remain and become visible when better levels
leave.

`getTopOfBook()` returns best bid/ask. `getBookUpdate()` returns the best ten
levels per side plus returned depth (0–10), stock locate, quantities, counts,
and the exchange timestamp of the latest successful mutation. The snapshot
depth is not the book's total number of internal levels. Failed mutations do
not advance the timestamp. Price is never converted through floating point in
the book; currency display is `raw_price / 10,000`, and raw zero has a separate
presence flag.

FIFO is not needed for exact aggregate L2 reconstruction because every ITCH
mutation names an order reference. FIFO/queue position is needed for
price-time-priority fill simulation, the strategy's own queue estimate, or a
priority-ordered L3 queue. No strategy callback/publication pipeline is
currently wired; the snapshot/query API is the correct state source but still
needs an integration layer and a hard gate on `ChannelHealth::Good`.

## 2026 full-day corpus and capacity evidence

The official 2026-06-12 trace is recorded in
[`trace-manifest-S061226-v50.txt`](trace-manifest-S061226-v50.txt):

- compressed bytes: `17,894,268,560`
- compressed SHA-256:
  `1f9d35e12120ef37ea22df9f59dc5207fe51b075b6ccf1c9cdfd09fa566dc1d5`
- BinaryFILE bytes: `41,662,444,846`
- BinaryFILE SHA-256:
  `8aab04f1f6e1287ef73acd7405a5f8487b131a5c6a7ae0f5c8d6d134c2f32238`
- first/final records: System Event `O` / System Event `C`
- zero-length terminator: absent; this exact checksum uses explicit
  SC-plus-physical-EOF compatibility

The labels have independent version axes:
`astra_itch_trace_manifest_v1` is the corpus-manifest schema,
`astra_itch_trace_profile-S061226-v2` identifies the frozen local profiler
revision, `nasdaq-itch-20260612-v1` is the capacity-profile identity, and
`astra_book_capacity_evidence_v2` is the engine-admission manifest schema.
The frozen executable hash is build/toolchain-specific; a Linux rebuild needs
fresh profile/evidence binding.

The completed semantic profile is
[`trace-profile-S061226-v50.txt`](trace-profile-S061226-v50.txt), SHA-256
`cb78c0a5a8a1d45373a603f4acbca431863ed0a0c993d2f8a6997cef38188532`.
It reports 1,304,894,064 records, `semantic_clean=1`, no malformed record or
semantic anomaly, and zero final live orders, active levels, and active price
pages. It observed 12,809 registered locates: 12,782 received displayed-order
prices and 27 remained valid symbols with no displayed-order price. Production
book materialization and independence are established by the implementation
contract and focused tests, not by this independent profiler. Production page
handles are monotonic, so all 156,871 lifetime pages remain allocated after
active levels and active pages return to zero. The frozen profiler SHA-256 is
`ad4a67aa75cb727a6d1ec5d6e18a3e199d14a1ba3a8b1efe0a49734fe3ca8826`.
The derived
[`book-capacity-evidence-S061226-v50.txt`](book-capacity-evidence-S061226-v50.txt),
SHA-256
`55f5ba91d10c74ff28da877c3665a97dca69fe1c5a6572f64332ea56c30a5516`,
binds the corpus-manifest SHA-256
`f527fd23274d45535e4028e2500a41b57ba20c898635850ecfe8f19aeb6d5dd8`,
the frozen-profiler SHA-256 above, and the profile-output SHA-256
`cb78c0a5a8a1d45373a603f4acbca431863ed0a0c993d2f8a6997cef38188532`.
It admits 2,382,540,226 direct-order slots, one fallback bucket, and 156,872
price pages, leaving one direct slot and one page beyond the observed corpus.

The matching local
[`book-storage-plan-S061226-v50.txt`](book-storage-plan-S061226-v50.txt),
SHA-256
`9d79fb39de911edcaebdba4761b1b5736105b4d7ee0a367c5a548de54dd76674`,
reports 388,036,034,560 planned book-storage bytes (361.38671875 GiB).
Adding the normal 16 GiB admission reserve requires 405,215,903,744 bytes;
adding the runbook's separate 4 GiB DPDK hugepage reserve reaches
409,510,871,040 bytes before the OS and operational headroom. The local
profiler binary/evidence hash is build-specific, so the target Linux build
must regenerate and retain its own binding and plan output.

## Tests added or strengthened

The review adds tests for exact ITCH/BinaryFILE framing, all lifecycle
boundaries, invalid timestamps and prices, Price(4) extrema, immutable symbol
identity, multi-symbol independent CRUD/levels, day-unique order references,
transactional mutation failures, exact mutation timestamps, full-domain and
cross-page price traversal, top ten plus retained deeper levels, replacement
into an occupied level followed by further CRUD, identical price/side
isolation across two symbols, terminal rollback tombstones, A/B same-sequence
conflicts, redundant gap recovery before EOS, control-schedule clock overflow,
SHA-256 padding boundaries, sender A/B byte identity, strict upper-rate
spacing, direct completion-to-EOS transition, non-duplicated `SS` pause,
per-message timestamp pacing and `SQ` handoff, wrapper configuration,
`md_engine` v2-schema capacity admission, and profiler/evidence provenance.
The loopback integration now drives the real sender into both live UDP lines
of the real `md_engine`, then verifies two-symbol lifecycle/CRUD aggregates,
redundant merge state, price-page accounting, zero live orders, EOS, and the
shutdown directory audit.
The final engine path also performs a cold fixed-domain audit of registered
symbols versus materialized book descriptors and prepared price state, reports
descriptor/slot identity, committed price pages/capacity failures, and refuses
a clean exit on any generic mismatch.

The order book also has a fixed-seed reference-model oracle that compares long
CRUD sequences, not only hand-selected examples.

On 2026-07-24, a clean macOS ARM64 Release configure/build passed all 283
discovered CTest entries, including approved local UDP loopback integration.
A clean Debug build with AddressSanitizer and UndefinedBehaviorSanitizer passed
282/282 entries; the one Release-only entry is the optimized disassembly
contract. The full applications, library, tests, and benchmarks also compile
cleanly with `-Wall -Wextra -Wpedantic -Werror`. Python-backed correctness
tests are configure-required rather than silently omitted.

## Known limitations

- No MoldUDP64 re-request/retransmission client or recovery service.
- No receiver inactivity/heartbeat watchdog and no independent post-merge
  per-line liveness state.
- Buffered redundant copies with the same start sequence are compared, but
  already-processed late duplicates and processed prefixes of differently
  packetized overlaps are not retained for comparison.
- DPDK does not validate IPv4 or UDP checksums, and not every effective DPDK
  setting is printed; the EC2 runbook pins and retains them explicitly.
- Kernel multicast membership does not yet select an explicit interface for a
  multi-ENI deployment.
- The current hashed `GapBuffer` is roughly 2 GiB and has a very large finite
  probe bound; this is not a deterministic low-tail recovery structure.
- The capacity derivation workflow protects content with hashes and exact
  profiler re-execution, but a hostile local actor able to replace input files
  concurrently remains outside its operational threat model.
- No strategy callback/publication bus, own-order model, or queue-position
  model is implemented. `OrderBook::isTradable()` is a state helper, but no
  wired strategy path enforces it as a trading-risk gate.
