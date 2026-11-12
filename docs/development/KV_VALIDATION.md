# KV validation and independent review

The implementation for issue #101 preserves the synchronous append contract and
uses the same coordinator, durable file writer, exact-base closure and SQLite
publication authority. The [KV guide](KV_MODE.md) defines the supported profile;
the [layout measurements](KV_LAYOUT_MEASUREMENTS.md) and
[native measurements](KV_NATIVE_MEASUREMENTS.md) record separate experiments.

## Independent review

A separate reviewer examined the whole change and ran public-API reproductions
without editing the implementation. Six findings were corrected and checked:

| Finding | Correction and regression evidence |
| --- | --- |
| A canceled exact migration retry hid its committed population receipt. | Owner-proven terminal replay precedes source reads, cancellation and new budgets; canceled, 1 ms timeout and pressured-memory retries return the original receipt. |
| Public compaction accepted 16 MiB while the backend accepted only 8 MiB. | One 16 MiB upper bound, boundary test and conflicting-options exact retry. |
| SDK/UI accepted impossible upsert physical counters. | Matching native arithmetic and shared invalid receipt vectors; compact no-ops also require zero input rows. |
| An unrelated write could claim a canceled migration's population UUID. | Creation reserves that UUID, binding atomically links its payload digest, and replay checks durable ownership and operation provenance. |
| A busy migration lookup fabricated an unknown receipt with an empty digest. | A blocked identity lookup returns a distinct resolution error; genuine pending populations retain their original validated context. |
| A reentrant replay could read an unconfirmed catalog transaction. | Replay refuses publication/transaction reentry, with a transaction callback regression. |

Independent native reprobes verified original receipt replay, reserved UUID
protection, corrected target rows, busy lookup classification, and policy/source
view conflicts. This is a bounded review; it is not a proof that every possible
bug or durability failure has been excluded.

## Invariants exercised

| Contract | Executable coverage |
| --- | --- |
| Immutable mode; append compatibility | Mode/create retries, raw append refusal without consuming an ID, old receipt byte fixtures, existing ordinary/bulk suites. |
| Full-row uniqueness | Last input wins across chunks, non-first UTF-8 keys, all seven scalar families, omitted nullable fields, duplicate/missing deletes, deleting every live file. |
| Snapshot isolation | Untouched range descriptors, retained old rows, held cursors, interval membership, initial empty views and fresh-process reopen. |
| No-op authority | Unchanged upsert, missing/empty delete and compaction; exact original root proof, permanent receipts, no phantom history and lost-COMMIT/rollback recovery. |
| Changed publication | Six real upsert/delete/compaction lost-COMMIT and rollback cases, exact receipts and fresh-process production reopen; shared external-file fault matrix remains enabled. |
| Migration | Explicit policies, fixed source, chronological cross-chunk/full-row FirstWins/LastWins, Reject failures, creation/population uncertainty, preexisting-target refusal and population provenance. |
| Bounded work and ownership | Read requested-byte charging including partial/failed reads, cancellation/deadline latching, result lifetime, retained capacity refusal, scratch page/I/O limits and collision-safe cleanup. |
| Native clients | HTTP, SDK, CLI and browser mode/operation parity, exact saved retries, all wire scalars, historical lookup, receipt export/import/reload and native restart. |

## Architecture critique

Sorted disjoint full-key ranges meet the measured locality target and avoid a
new per-key index. An index could reduce candidate discovery but cannot reduce
the files physically containing scattered updates. Overlapping sorted runs would
need a different read/merge and tombstone contract. They were measured as a
baseline, rather than silently mixed with this visibility model.

Copy-on-write rewrites surviving values in touched ranges and retains immutable
history. Bounded scatter may refuse; compaction can increase physical retained
usage and does not reclaim snapshots. The conservative four-to-one contiguous
compactor can leave fragmentation. A later multi-output compactor, richer range
directory, or delayed scratch reservation after proving a no-op each needs its
own resource and recovery contract.

The component p95/p99 values have five samples and equal the observed maximum;
OS caches were not flushed. Native runs use a shared development host and one
exercise sample per size, with raw I/O and RSS. These are measurements, not
random-write latency guarantees. Physical power loss, 1 TiB qualification,
retention/GC, remote storage and multiwriter operation remain outside this work.

## Recorded checks before upstream integration

On the feature implementation over reference `37b58fa`:

- Full Release CTest: 1,159 enabled entries, zero failures, including native HTTP,
  faults, admin, installed consumer and tooling. Twelve standalone child or
  opt-in measurement entries skipped; four existing manual campaigns disabled.
- Installed consumer with storage disabled: passed independent write/reopen
  processes and explicit dependency-error expectations.
- Python SDK: 919 passed. UI unit: 251 passed. Native Chromium: 13 passed.
- Actual native KV HTTP/SDK/CLI/restart scenario: passed every scalar, precise
  timestamps, canonical prepared identity and saved no-op retry.
- Both 4/16 GiB component pairs and native campaigns completed; raw evidence and
  scope are linked above.
- Combined ASan/UBSan: 167 unit, 202 storage, 23 mock and 102 selected vertical
  cases passed, with three expected standalone child/measurement skips. The
  selected native set includes KV, bulk, fault, snapshot and input-lifetime tests.
  Leak detection remained enabled and no sanitizer error was suppressed.
- LLVM 18 formatting passed for all 313 project files; quality negative checks
  rejected deliberate format/analyzer violations. New KV units and changed
  runtime units passed individual tidy checks. The full-project tidy run also
  reports existing diagnostics in transport, pruning, verifier, vendor code and
  legacy tests (including the existing write-settings suite).

The native dependency prefix uses Clang 18/libc++ and pinned Arrow 25. For the
combined ASan/UBSan profile with that uninstrumented prefix, follow the existing
[documented compatibility setup](BUILDING.md): `_LIBCPP_HAS_NO_ASAN` and static
libc++ linkage. Allocation bounds, use-after-free checks and UBSan remain enabled;
no runtime sanitizer-error suppression is used.
