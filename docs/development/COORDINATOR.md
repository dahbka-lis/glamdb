# Catalog and coordinator MVP

The embedded facade in `public/storage/engine.h` implements explicit store
creation/opening, collection identity/schema lookup, synchronous append outcomes,
operation lookup, reconciliation and bounded shutdown. `glamdb::core` installs
as a CMake package. The CLI provides `create-store` and `inspect-store` with an
absolute root and caller-retained store UUID.

The production durable file backend and scanner are pending. Public factories
can create/open an empty catalog; collection creation reports
`DEPENDENCY_UNAVAILABLE` without a backend. Opening a populated store also
requires its storage validator. Test doubles are confined to `tests/`; they
produce no real Parquet or Avro data. There is no HTTP server, scan API, payload
journal, background job queue, backup or automatic file deletion in this MVP.

## Persistence and retry rules

- One directory lease owns the store across threads/processes. Opening never
  uses SQLite CREATE or repairs missing identity, readiness or catalog files.
- `DurableSqliteCatalogStore` implements Iceberg's public `CatalogStore` with
  one serialized SQLite connection. It checks the locked SQLite source ID and
  actual `DELETE`/`EXTRA` profile. Open validates schema/version, integrity,
  foreign keys, collection registry and operation records.
- The coordinator validates and copies Arrow input into owned normalized rows.
  Field IDs follow fixed schema order from 1; omitted nullable values become
  NULL, duplicate rows remain, float negative zero normalizes, nonfinite floats
  fail, and timestamps are UTC microseconds in years 0001 through 9999. SHA-256
  hashes use versioned canonical bytes, independent of batch chunking.
- A permanent collection-scoped operation binding commits before file work.
  Its exact base includes store/collection/table identities, metadata location,
  optional snapshot, schema and partition-spec IDs. Retrying identical content
  returns the original terminal result before evaluating a new precondition or
  admitting another operation. Different content conflicts.
- Publication owns `BEGIN IMMEDIATE` through final COMMIT. Inner Iceberg
  transactions join that scope. A guard runs before the single INSERT/CAS;
  registry/pointer, snapshot identity and terminal receipt commit atomically.
  Failed inner transactions poison the outer scope even if their error is
  ignored. Ordinary failure records a permanent failed outcome.
- An uncertain binding/publication/failure-recording result closes readiness.
  Reconciliation closes the old handle, reopens authoritative SQLite state,
  validates retained storage in both directions, and commits a real fence
  increment before resolving unfinished bindings. It never replays payloads.
  Missing/corrupt committed state fails closed.
- If a global barrier prevents consulting another operation, append returns
  `commit_unknown` with `recovery.lookup_blocked=true`. Its base is the last
  confirmed collection view and its attempt UUID identifies the lookup call;
  that call binds/publishes nothing. The original unresolved operation remains
  the store's recovery target. This also applies when readiness changes while
  a retry waits for the writer.

Candidate bounds are eight collections, 4096 permanent operations, 256 MiB of
catalog pages, 16 pending calls and 128 MiB aggregate owned input. Per append:
10,000 rows, 16 MiB canonical input, 2 MiB per row, 1 MiB per string and 1024
bytes per key. Writer wait is at most five seconds; append deadline at most
60 seconds. Configuration may lower these bounds. These are admission limits,
not measured simultaneous RSS or full-storage capacity guarantees.

## Storage backend integration

`core/engine/storage_backend.h` is the private integration seam, injected by
`EngineFactory`. Implement `PrepareCreate`, `PrepareAppend` and `VerifyRetained`.
Preparation supplies the actual generated table UUID or complete append receipt;
`Publish` must use the supplied catalog and exact bound base with Iceberg retries
disabled and metadata cleanup disabled. Every new file class must use the same
durable FileIO. Do not substitute the test backend or Iceberg's plain local
FileIO in production.

The staged closure guard must verify the final root and all reachable new
files, durable receipts and sticky errors, reused-base membership and candidate
reopen limits before pointer mutation. `VerifyRetained` must independently match
every committed operation to exactly one retained snapshot/tag and vice versa;
Bound/Failed operations must have none. Validate UUID, schema, ancestry and file
closure. Catalog checks alone cannot establish these storage properties.

## SDK/UI compatibility

Outcome JSON in `apps/glamdb/outcome_json.*` keeps transport dependencies outside
the public API. All int64 quantities are decimal strings; version numbers are
integers. Append replies carry complete receipt/recovery fields. Unknown is
HTTP 503 with `same_id_after_reconcile`. Create success is a direct collection
object, including `current_snapshot_id`, ordered `field_id` values and append
mode, matching the SDK/UI contract in
[PR #4](https://github.com/glamourdb/glamdb/pull/4).

Recommended client adjustments:

- UI create should parse outcome envelopes on error HTTP statuses
  (`allowOutcome: true`) and handle authoritative `definitely_not_committed`
  before status-based recovery. Otherwise a definite 503 failure freezes the
  create UUID/schema as an unresolved request.
- SDK unknown handling should require and validate a complete consistent
  recovery object and error before treating it as a resumable outcome. Retain
  optional extension fields such as `lookup_blocked`.
- Future file listings need a stable `file_id`. Snapshot ordering/pagination
  must use ancestry or sequence, since snapshot IDs are opaque integers.

The immutable reviewed client head is
`ee7da3993d10144cacaa4ad5c5cb629d6db11b0d`. Native JSON fixtures passed six
checks through that unmodified SDK with HTTPX mock transport. This tests wire
shapes and precision; socket-based SDK/UI integration awaits an HTTP adapter.

## Validation evidence

Recorded on Linux x86-64 with Clang 18.1.3/libc++ 18.1.3, Arrow/Parquet 25.0.0,
Iceberg 0.4.0 (`64dc66f159bd62c9817a96d74cbce767268934d0`) and locked SQLite
3.54.1, using disposable stores:

| Executed check | Result |
| --- | --- |
| Normal build + `ctest --test-dir build/coordinator --output-on-failure` | 41/41 PASS |
| Release build + `ctest --test-dir build/coordinator-release --output-on-failure` | 41/41 PASS |
| Debug ASan/UBSan/LeakSanitizer + CTest | 40/40 PASS; installed consumer tested separately above |
| Unmodified SDK parsing native outcome JSON through mock transport | 6/6 PASS |
| LLVM 18 formatting / analyzer | 36 files / 20 translation units PASS |
| Formatting and analyzer negative controls | Both deliberate violations rejected |

The final analyzer pass ran translation units in parallel with the repository's
unchanged checks, header filter, compile database and warnings-as-errors flags.

The sanitizer run enabled leak detection, allocation/deallocation mismatch
checks and halt-on-error, with no diagnostic suppression. It used static
libc++/libc++abi/libunwind because the packaged dynamic exception runtime also
failed a standalone ASan throw/catch control. Independent negative controls
still detected a deliberate mismatch, use-after-free and leak.

Use [BUILDING.md](BUILDING.md) for build, CTest and quality commands. Tests cover
literal canonical hash vectors; A/B/retry-A with exact recovered receipts;
permanent failures; exact metadata-only bases; nested transaction rollback;
lost COMMIT and fence results through a real SQLite VFS; global-barrier races;
fresh-process operation recovery, hot rollback journals and store leases; and
the installed public package.

`glamdb.real_sqlcatalog` additionally uses real Iceberg `StageCreateTable`,
`Transaction::Commit` and `UpdateProperties` with completed JSON roots. It
observes close-before-guard, tentative pointer isolation on a second SQLite
connection, outer rollback, metadata-only stale-base rejection and a separate
process reopening/loading the retained pointer. Its property-only transaction
is deliberately rolled back before receipt insertion; a synthetic append
receipt is never committed. The VFS tests separately exercise terminal receipt
publication. The local test FileIO proves ordering, not file synchronization.

Independent catalog, coordinator and SDK reviews exercised these boundaries;
their confirmed correctness defects have regression tests. Production
Parquet/Avro closure, a real successful append/read, scan semantics, restore,
combined capacity/RSS and OS-cache-loss/power-cut qualification remain NOT RUN.
