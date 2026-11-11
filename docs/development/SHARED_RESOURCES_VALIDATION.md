# Shared resource validation

Recorded on 2026-11-11 for [issue #99](https://github.com/glamourdb/glamdb/issues/99).
Environment: Ubuntu 24.04.4, Linux x86-64, Clang 18.1.3, Debug builds,
pinned libc++ 18 and repository dependencies. The test filesystem reports
`ext2/ext3` through `stat -f`; this is not the dedicated G4 qualification volume.
The final tree includes official main through `85bd283`.

## Checks

| Check | Result |
| --- | --- |
| Storage-enabled build and full native CTest suite | PASS: 607 passed, two skipped, one disabled; 610 discovered |
| Resource owner/pool, shared-store, native route, file metadata and HTTP profile tests under ASan/UBSan/LSan | PASS: 24 tests, leak detection enabled |
| Storage-disabled resource tests and installed public consumer | PASS: eleven tests |
| LLVM 18 formatting | PASS: 203 project files |
| LLVM 18 static analysis with configured warnings as errors | PASS: all 22 changed translation units, including project headers |
| Quality negative checks and `git diff --check` | PASS |

The skipped cases are the standalone `CapacityRecoveryChild.RejectsOverBudgetStore`
and `RecoveryChildProcess.ReopensPublishedRoot` subprocess helpers. The disabled
case is `TerminalRetryCost.CoordinatorWorkScalesWithStoreHistory`. No new skips
or disabled cases were added. Full-repository static analysis, full sanitizer
suites, TSan and MSan were not run locally.

An earlier parallel run during static analysis failed the unchanged metadata
serialization timing ratio (3.34 against a limit of 3). Three isolated repeats
passed with ratios 1.90, 2.01 and 1.87; the final full suite also passed without
changing that test.

Commands, with the normal and instrumented build directories configured as
described in [BUILDING.md](BUILDING.md):

```sh
cmake --build build/local --parallel 2
ctest --test-dir build/local --output-on-failure --parallel 2
ctest --test-dir build/issue-43-asan --output-on-failure -V \
  -R 'ResourceOwner|ResourcePool|MetadataResources|SharedResources|SharedPressure|TFileMetadataTest|glamdb.resource_profile'
ctest --test-dir build/nostorage --output-on-failure \
  -R 'ResourceOwner|ResourcePool|MetadataResources|^glamdb.installed_consumer$'
cmake --build build/local --target format-check quality-negative-check
ctest --test-dir build/local --output-on-failure -V -R '^glamdb.resource_profile$'
```

The instrumented build enables address and undefined-behavior sanitizers;
LeakSanitizer runs with its default Linux leak detection. Static analysis uses
`clang-tidy-18`, `build/local/compile_commands.json`, the checked-in
`.clang-tidy`, the project header filter and `--warnings-as-errors=*` for each
changed C++ translation unit.

## Behavior and independent review

Owner tests check concurrent admission, separate protected classes, shared
lease lifetime, same-device byte/inode competition and overflow refusal. Pool
tests check upstream refusal, retained old buffers on failed reallocation and
the simultaneous old/replacement charge. Zero-size reallocation refusal also
retains the old buffer and its reservation. Real-store tests check cross-database
pressure, pre-binding refusal, original receipt reuse, uncertain append/create
reconciliation, retained slices/C Data exports and original metadata results.
All reservations return to zero after their final owners disappear.

A fresh independent reviewer inspected the patch and surrounding code read-only.
The review found three issues, all corrected before publication:

- Staged-object destruction could refund a candidate root before an uncertain
  COMMIT was classified. Only confirmed publication/rollback now releases it;
  append and create recover while ordinary classes are full.
- Returned metadata outlived its working reservation. Original DTOs now retain
  capacity-sized leases, and an extracted explanation schema retains its lease.
- HTTP response members could outlive their reservation during destruction.
  The reservation now outlives the response payload and headers.

The first two findings were reproduced by failing tests before the fixes. The
final tests pass with sanitizers. The reviewer found no critical issues; broader
envelope calibration and physical fault qualification remain explicit limits.
A scoped independent review covers the later integration of snapshot file APIs
from main. Their listing and file-detail DTOs retain separate capacity-sized
leases for file metrics, bounds and row-group/column data; a new regression
reproduced their missing charges after close. This review found that many small
bounds allocations needed per-allocation overhead beyond the fixed allowance.
An 8,192-bound refusal test reproduced it; retained charges now include allocator
headers and rounding per allocation.

Final self-inspection also reproduced and fixed premature release on a rejected
zero-size reallocation; its regression checks both refusal and successful release.

## Measured HTTP profile

`glamdb.resource_profile` creates three managed databases with one collection
each. Every collection receives two 2,000-row appends, including duplicate keys
and 128-byte string payloads. Three writers and three scanners overlap; final
scans check all 4,000 rows per database and retries check original receipts.
The service reconciles, shuts down and reopens in a fresh process with the same
limits. A third process fills data, metadata and ordinary control reservations,
then verifies outcome lookup, readiness and reconciliation for every database.

One normal run recorded these reservation high-water marks, in bytes:

| Process phase | Data | Metadata | Control | Completion | Recovery |
| --- | ---: | ---: | ---: | ---: | ---: |
| Concurrent load | 1,380,233,782 | 2,126,693,376 | 81,920 | 4,588,556 | 2,432,696,320 |
| Fresh-process reopen | 404,468,630 | 1,053,038,336 | 49,152 | 262,144 | 2,432,696,320 |
| Ordinary classes full | 2,147,483,648 | 8,589,934,592 | 16,777,216 | 4,588,670 | 2,432,696,320 |

Actual Arrow allocator peaks and process RSS are measured separately from those
conservative reservations:

| Process phase | Arrow peak bytes | Peak RSS bytes | Pending device bytes peak | Pending inodes peak |
| --- | ---: | ---: | ---: | ---: |
| Concurrent load | 1,238,016 | 89,026,560 | 3,012,558,848 | 240 |
| Fresh-process reopen | 417,792 | 76,124,160 | 2,475,687,936 | 112 |
| Ordinary classes full | 32,832 | 33,972,224 | 2,475,687,936 | 112 |

The load returned nine retryable `SERVER_BUSY` admissions and completed after
retrying them. A 5 ms sampler observed physical tree peaks of 401,408 bytes and
49 entries, including an 8,192-byte recovery workspace. Sampling may miss shorter
peaks; it is not a physical upper bound. The Arrow metric is the default upstream
pool's high-water allocation count; RSS includes non-Arrow memory and allocator
retention. Every process reported zero remaining memory/device/inode reservations
after shutdown.

The same profile passed under ASan/UBSan/LSan. That run recorded peak RSS of
346,550,272 bytes during load, 193,404,928 during reopen and 136,794,112 with
ordinary classes full. Sanitizer overhead makes these separate measurements.
Scheduling changes reservation peaks and retry counts between runs.

## Limits

This small campaign exercises shared admission and lifetimes; it does not
certify an RSS ceiling for every valid or corrupt input. Non-Arrow dependency
work uses deliberately coarse envelopes. Broader workloads and dependency
changes require recalibration. Caller-created copies of inputs or metadata DTOs
belong to the embedding application's budget.

Device tests use deterministic reservation caps on real filesystem paths.
Existing injected I/O/fault suites pass, but no dedicated physical full-volume,
inode-exhaustion or power-loss campaign was run. External writers can still
cause ENOSPC after admission. No G4 qualification or network durability claim
follows from these results; [G4 remains NOT RUN](../ROADMAP.md#current-status).
