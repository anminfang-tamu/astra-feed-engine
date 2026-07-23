# Branch 5 native acceptance port

This package ports the pinned branch-5 implementation at commit
`324d81a15ee52cc72f68873a1ced122923406df2` to the cross-layout replay
contract without changing its order-book mutation design. It deliberately does
not create branch-6-style direct-order or price-page mappings.

## Apply the port

From the redesign repository, create a separate named worktree. The fixed patch
digest and resulting tree make this fail closed before performance evidence is
collected:

```bash
REPO_ROOT="$(git rev-parse --show-toplevel)"
BASELINE_ROOT="$(dirname "${REPO_ROOT}")/astra-feed-engine-branch5-native-v1"
BASELINE_BRANCH=5-to-add-numa-acceptance
PINNED_BRANCH5=324d81a15ee52cc72f68873a1ced122923406df2
PORT_PATCH="${REPO_ROOT}/compat/branch5_native_v1/branch5-native-port.patch"
BRANCH5_CAPACITY_EVIDENCE="${REPO_ROOT}/compat/branch5_native_v1/capacity-evidence-01302019.txt"

test "$(sha256sum "${PORT_PATCH}" | awk '{print $1}')" = \
  914fde914dca97def9964059581ae8ce98dfce7a24b8d7659a183231a4093f7d
test "$(sha256sum "${BRANCH5_CAPACITY_EVIDENCE}" | awk '{print $1}')" = \
  05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282
git cat-file -e "${PINNED_BRANCH5}^{commit}"
git worktree add -b "${BASELINE_BRANCH}" "${BASELINE_ROOT}" \
  "${PINNED_BRANCH5}"
git -C "${BASELINE_ROOT}" apply --check "${PORT_PATCH}"
git -C "${BASELINE_ROOT}" apply --index "${PORT_PATCH}"
git -C "${BASELINE_ROOT}" commit \
  -m 'Add branch5 native acceptance compatibility port'
test "$(git -C "${BASELINE_ROOT}" rev-parse HEAD^)" = "${PINNED_BRANCH5}"
test "$(git -C "${BASELINE_ROOT}" rev-parse 'HEAD^{tree}')" = \
  492938730e6db91e84bdb1f8e25152536e81dbc0
bash "${BASELINE_ROOT}/scripts/build_branch5_native_replay.sh"
```

The patch adds only read-only correctness/residency accessors plus a complete
native replay target:

- `Branch5ReplayBackend.hpp` exposes logical order/level state to the replay
  digester. It uses the stock locate carried by every ITCH order message and
  performs no auxiliary allocation or global lookup.
- `Branch5NativeStorage.hpp` enumerates the real fixed `std::vector` payloads
  and writes the exclusive-create native range manifest.
- `Branch5ItchBookReplayBenchmark.cpp` implements the common CLI, fixed sample
  schedule, physical and layout-independent digests, System-S ready gate, and
  native output schema.
- `Branch5ReplayTime.cpp` is the same serialized RDTSCP/portable timing
  implementation used by the redesign replay.
- `build_branch5_native_replay.sh` configures and builds the Release CMake
  target deterministically from the one reviewed compatibility commit directly
  above the pinned commit.

The compatibility changes must be one clean commit whose parent is the pinned
branch-5 commit. This preserves the acceptance harness's clean-worktree rule;
the baseline commit passed to the comparator is the reviewed compatibility
commit, while its parent proves the pinned implementation base. The build
script rejects a dirty tree, an extra commit, a different parent, or any changed
path outside the fixed port path set.

The reviewed port's complete Git tree is
`492938730e6db91e84bdb1f8e25152536e81dbc0`. The acceptance harness captures
the parent and tree both before and after all runs; branch-5 artifacts are
rejected unless the parent is the pinned implementation commit and the tree is
this exact value.

The script prints the resulting
`build/branch5-native-release/benchmarks/astra_itch_book_replay_benchmark`
path. That prebuilt file is only the proposed baseline input. Before acceptance,
the universal harness exports the reviewed compatibility commit, configures a
fresh benchmark-only CMake graph, clean-builds the same target, and requires the
proposed and independently built executable bytes to match exactly. Thus the
branch-5 flow remains supported without trusting a stale compatibility build
directory.

## Pinned full-trace baseline

The full-trace baseline must use all four canonical capacities together:

```text
default_order_capacity=65536
price_internal_node_capacity=163840
price_leaf_capacity=1048576
price_level_capacity=2097152
```

They are policy values, not tuning knobs; partial or different overrides are
invalid. The canonical
[`capacity-evidence-01302019.txt`](capacity-evidence-01302019.txt), SHA-256
`05f21a7c0db648028feb2cc006440ae5fb4431fa1f3685bc1404ddad610b4282`,
is authoritative; [`capacity-evidence.md`](capacity-evidence.md) retains its
derivation. With the pinned trace, the ready manifest has 8,713 prepared books,
34,858 ranges, and 62,404,952,064 native bytes. A 64 GiB `--planned-bytes`
admission bound covers that manifest, while the harness's default 16 GiB
reserve remains additional.

Set `CPU` and `NODE` to the selected isolated CPU and its local memory node,
then invoke the final harness from the redesign repository. `UINT64_MAX`
ceilings intentionally make latency non-gating for branch 5; the retained
distributions become the comparator baseline:

```bash
"${REPO_ROOT}/scripts/run_order_book_acceptance.sh" \
  --binary "${BASELINE_ROOT}/build/branch5-native-release/benchmarks/astra_itch_book_replay_benchmark" \
  --expect-hot-arena-schema branch5_native_v1 \
  --trace "${REPO_ROOT}/data/itch/unzipped/01302019.NASDAQ_ITCH50" \
  --cpu "${CPU}" \
  --numa-node "${NODE}" \
  --expect-records 368366634 \
  --expect-bytes 11245883092 \
  --default-order-capacity 65536 \
  --price-node-capacity 163840 \
  --price-leaf-capacity 1048576 \
  --price-level-capacity 2097152 \
  --sample-capacity 8388608 \
  --planned-bytes 68719476736 \
  --reserve-bytes 17179869184 \
  --max-p50-ns 18446744073709551615 \
  --max-p99-ns 18446744073709551615 \
  --max-p99-9-ns 18446744073709551615 \
  --correctness-digest
```

The harness must report `PASS`, but that status authenticates the run and its
evidence; it does not claim that branch 5 met a performance ceiling. Use the
cross-artifact comparator in the top-level README to apply the redesign's p50
target and branch-5-relative tail gates.

## Universal replay integration

The backend-specific replay drivers implement the same output contract,
sampling schedule, and lifecycle gates. The branch-5 driver identifies
`Branch5ReplayBackend::hotArenaSchema()` as `branch5_native_v1` and consumes
this startup prelude:

1. Consume directory/admin records through System Event `S`. Reject any book
   mutation before `readyForTimedReplay()` becomes true.
2. At that boundary all registered books have been constructed, their fixed
   vectors have been page-walked, and the book universe is sealed.
3. Write the path supplied by `--native-range-manifest=PATH`, emit the ready
   record, and wait at the common start gate.
4. Continue with the same fixed sampling schedule. All logical observation is
   conditional on digest mode and occurs after the ending timestamp.

The manifest grammar is deterministic:

```text
branch5_native_ranges schema=branch5_native_ranges_v1 count=N bytes=N digest=N
branch5_native_range ordinal=0 kind=price_nodes locate=0 base=N bytes=N
...
```

Rows are six shared price vectors in this exact order:
`price_nodes`, `price_leaves`, `price_levels`, `price_free_nodes`,
`price_free_leaves`, and `price_free_levels`. They are followed by ascending
stock locate, with `order_records`, `order_free_indices`, `order_occupancy`,
and `order_ref_entries` for each prepared book. Therefore
`native_range_count == 6 + 4 * prepared_books`.

`native_range_digest` is FNV-1a-64. It starts with the raw ASCII domain
`branch5_native_ranges_v1`; each row adds the kind as a little-endian `uint64`
length plus ASCII bytes, then locate, base, and bytes as little-endian
`uint64` values. Ordinals are zero-based and are not hashed.

The storage-plan record for this schema contains:

```text
system_page_bytes price_nodes_bytes price_leaves_bytes price_levels_bytes
price_free_nodes_bytes price_free_leaves_bytes price_free_levels_bytes
planned_price_pool_bytes default_order_capacity
price_internal_node_capacity price_leaf_capacity price_level_capacity prefault
```

Ready and final records repeat the explicit manifest path/schema/count/bytes/
digest and include `native_prefault_complete=1`, `book_universe_sealed=1`, and
`prepared_books=N`. Branch-6 `mapped_array`, direct/fallback order, descriptor,
and eleven redesign-arena fields do not exist under this schema.

Acceptance policy remains strict: prefault must be complete, post-warmup minor
and major faults must be zero, swap must be zero, and at least 99% of anonymous
pages must reside on the selected NUMA node. Exact 2 MiB arena alignment and
THP coverage apply only to `redesign_v1`, not to the native allocator/vector
layout.

## Proofs

`verify_identity.py` creates two traces and checks only layout-independent
behavior. Backend-specific small-capacity arguments follow `--`; a native
manifest argument may use `{manifest}`, which is replaced with a unique path
for each replay:

```bash
python3 compat/branch5_native_v1/verify_identity.py /path/to/branch5-replay \
  --expect-hot-arena-schema=branch5_native_v1 -- \
  --default-order-capacity=16 \
  --price-node-capacity=16 \
  --price-leaf-capacity=16 \
  --price-level-capacity=16 \
  '--native-range-manifest={manifest}'
```

The 11-record trace covers A/F/E/C/X/D/U and prices `0`, `0xffffffff`, and
`0x7fff0001`. The lifecycle trace proves that an identical `R` is accepted
after System Event `S`, and that Order Executed `E` remains independent and is
applied after System Event `E`.

See `identity-proof.md` for the local compiled proof. It is a compatibility
proof, not a substitute for the required repeated EC2 performance and NUMA
acceptance runs.

For strict A/B fairness, both replay executables emit their ready/start gate at
the same post-System-S boundary. Ready and final records expose
`prelude_records` and `prelude_bytes`; the comparator requires exact equality
between branch 5 and the redesign for every measured and correctness replay.

The sealed-universe flag is a constraint of this historical branch-5 adapter.
It does not describe branch 6, whose startup-preallocated descriptors allow
both repeated and genuinely new `R` messages after `SS`.
