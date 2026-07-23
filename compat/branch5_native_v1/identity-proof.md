# Branch 5 compatibility proof

Pinned implementation: `324d81a15ee52cc72f68873a1ced122923406df2`,
whose Git tree is `cf27ee8d6f3a85c034c0a289d316b1ee6670dc96`.

The final patch SHA-256 is
`914fde914dca97def9964059581ae8ce98dfce7a24b8d7659a183231a4093f7d`.
It was applied to a fresh pinned checkout, committed as one clean child, and
produced reviewed Git tree
`492938730e6db91e84bdb1f8e25152536e81dbc0`. The native replay source SHA-256
was `7e4c0d994bbb10f11cf07d9339074d3239f9ba3bafcabf990d0e6aa06621fc76`.

That clean tree was compiled on arm64 macOS with Apple clang 21.0.0. The
resulting functional-proof binary SHA-256 was
`4b994a81a23210db1c31fc6ab39a55246a23b36355f266585f1a914c5766a1db`.
This host is suitable for functional proof only; its timing is not EC2 x86
acceptance evidence.

The 11-record all-mutations trace completed with:

```text
records=11
bytes=324
book_messages=8
applied_book_mutations=8
prelude_records=2
prelude_bytes=55
final_live_orders=0
phase=7
semantic_mutation_digest_schema=applied_itch_book_semantics_v1_fnv1a64le
semantic_mutation_digest=12141299839370961608
```

That digest is exactly the branch-6 identity-test constant, including raw-domain
endpoints `0` and `UINT32_MAX`, and is independent of branch 5's physical
local-index/radix layout.

The separate lifecycle trace completed with:

```text
records=8
book_messages=3
applied_book_mutations=3
final_live_orders=0
phase=7
late_r_after_ss=PASS
order_e_after_system_e=PASS
```

The native manifest writer was compiled against the pinned implementation and
produced the expected `6 + 4 * 1 = 10` exact vector spans for one prepared
book. Its header count, summed bytes, and FNV digest were nonzero, and a second
create at the same path is rejected by `O_EXCL`.

The current schema-aware hot-path verifier reported version 2 PASS for
`branch5_native_v1`, selecting 78 functions with zero forbidden targets,
indirect calls, or lock-prefixed instructions.

The generic branch-6 identity command is intentionally not reused for this
binary: it supplies redesign-only capacity-profile options, which branch 5
rejects. The compatibility-specific identity test applies the same logical
mutation and lifecycle contract with branch-5-native capacity arguments. No
dummy redesign ranges were added; `branch5_native_v1` has its own honest
residency policy.
