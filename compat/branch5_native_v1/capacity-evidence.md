# Branch 5 pinned capacity evidence

The canonical machine-readable evidence is
`capacity-evidence-01302019.txt`. It binds the frozen branch-5 implementation,
the reviewed compatibility port, and the complete 2019-01-30 trace. Acceptance
authenticates the reviewed file byte-for-byte by its pinned SHA-256. It then
independently validates the runtime-relevant capacities, sampling policy,
prepared-book count, native-range layout and bytes, admission plan, and reserve
against the branch-5 storage-plan, ready, native-range, and final records.
Profiler high-water marks and their conservative bounds remain reviewed
derivation evidence; the harness does not claim to semantically parse every
manifest key.

The trace registers 8,713 unique directory locates before System Event `S`:
8,696 default, six active, six hot, and five ultra-hot. Handling `S` prepares
one book for each of those locates. `META`, which is in branch 5's hot tier,
does not occur in this 2019 directory.

For a book with order capacity `C`, the four fixed vector payloads consume:

```text
order records          32C
free indexes            4C
occupancy bitmap       C/8
local-reference table  64C
total                801C/8
```

Applying the frozen tier capacities to all 8,713 books gives
59,948,531,712 order-vector bytes. The six shared price vectors reported by
the pinned storage plan total 2,456,420,352 bytes. Therefore the System-`S`
native manifest must contain:

```text
prepared_books=8713
native_range_count=6 + 4 * 8713 = 34858
native_range_bytes=59948531712 + 2456420352 = 62404952064
```

The retained trace profile proves a maximum of 42,774 live orders for any
locate, below the default capacity of 65,536. Its global high-water marks are
707,130 active side/price levels and 56,395 active
`(stock_locate, price >> 16)` paths. Including replace's
destination-before-source-removal transient gives conservative shared-pool
bounds:

```text
internal nodes <= 2 * 56395 + 2 = 112792 < 163840
leaves         <= 707130 + 1     = 707131 < 1048576
levels         <= 707130 + 1     = 707131 < 2097152
```

The reference-table term is 64 bytes per configured order-capacity slot
(four 16-byte hash slots), not per currently live order.

The common fixed sampling interval needs at most
`ceil(363118215 / 64) = 5673723` entries, below the pinned sample capacity of
8,388,608.

The baseline admission plan is exactly 64 GiB
(`68,719,476,736` bytes), leaving 6,314,524,672 bytes above the native vector
payload. The separate 16 GiB reserve remains mandatory and is not part of that
64 GiB admission bound. Root objects, timed-sample storage, allocator metadata,
and other runtime memory are not claimed as native-range payload; the reserve
and live memory/NUMA checks cover them separately.
