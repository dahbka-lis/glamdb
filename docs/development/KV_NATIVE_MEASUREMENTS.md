# Native KV measurements for issue #101

Both final-schema native 4/16 GiB campaigns passed build, clustered mutation,
full-row lookup, historical reads, durable no-ops, compaction and fresh-process
reopen with exact retries. On the actual 17,179,905,360-byte decoded fixture, a
1,000-row clustered update rewrote two files totaling 17,009,142 bytes in
705.118 ms, with 68,005,616 requested Parquet read-plus-write bytes. Peak
recorded process RSS across both campaigns was 123,006,976 bytes.

This benchmark exercises the public `Engine` facade, catalog, bounded KV
producer and durable Parquet/Iceberg publication. It complements the earlier
[layout component experiment](KV_LAYOUT_MEASUREMENTS.md). These development-host
measurements do not qualify engine durability or the 1 TiB campaign.

## Fixture and measurement scope

The [native benchmark](../../benchmarks/kv_mode.cpp) generates required UTF-8 keys
and 4,096-byte deterministic printable payloads plus an `int64` version. It uses
native uncompressed Parquet with dictionaries disabled. The source replays in
bounded 1,024-row batches, with approximately 256 MiB decoded input per operation;
both passes use the production bulk normalization and hashing path. Creation
used 17/65 independent bounded operations, including the final partial operation.

Rows round upward to the requested decoded size. The recorded decoded count is
actual Arrow `TotalBufferSize` across one complete source pass, including string
offsets. Current physical Parquet bytes are separate. Immutable historical files
remain retained; filesystem counts and sizes agree with all added-file receipts,
and every retained primary Parquet file is below 32 MiB. Both final catalogs
have identical checked schema fingerprints and pass SQLite `quick_check`.

Each build, exercise and reopen phase uses a fresh process. The exercise holds a
historical cursor, replaces 1,000 clustered full rows, verifies every replacement
value through ordered lookup with a miss and repeated hit, verifies old full
values at the retained snapshot, and records durable unchanged-upsert and
missing-delete no-ops. A second collection is reduced from four small live files
to one with value-preserving compaction. Reopen validates latest and historical
full values, row counts, exact mutation retry and exact no-op retry. No mutation
scratch files remain after shutdown.

An `ILocalFS` wrapper counts requested and accepted external-file reads/writes,
and counts Parquet requests and file syncs separately. Counts include native
publication and recovery verification, including repeated reads. They describe
file operations, not cold device traffic. Catalog SQLite and the independently
bounded mutation-scratch SQLite VFS are excluded. The producer enforces an 8 GiB
attempt I/O/output ceiling plus a separate 16 GiB scratch-I/O ceiling; scratch
main pages are capped at 4 GiB. Peak RSS comes from `getrusage`. Resource-owner
memory peaks in raw evidence are conservative admitted envelopes, not RSS.

OS caches were not dropped. The two fixture creation processes ran concurrently;
development compilation and functional checks could also overlap. Exercise and
reopen phases ran separately. Each latency is one observed sample, so no native
percentile or dedicated-device claim is made. The earlier layout experiment
provides repeated component samples. Both fixtures used the final frozen catalog
schema. Checked-result and static-analysis cleanup preceded exercise/reopen;
producer layout and schema stayed the same. Raw evidence retains both artifact
hashes.

## Results

| Metric | Requested 4,096 MiB | Requested 16,384 MiB |
|---|---:|---:|
| Actual decoded source bytes | 4,294,978,412 | 17,179,905,360 |
| Rows | 1,039,441 | 4,157,762 |
| Bounded build operations | 17 | 65 |
| Current Parquet bytes after build | 4,316,408,668 | 17,265,626,695 |
| Current files after build | 508 | 2,031 |
| Retained primary Parquet bytes after exercise | 4,402,149,685 | 17,555,464,672 |
| Build time (s) | 243.172 | 975.102 |
| Clustered update files removed / added | 2 / 2 | 2 / 2 |
| Clustered update bytes removed / added | 17,009,142 / 17,009,142 | 17,009,142 / 17,009,142 |
| Clustered update requested Parquet reads / writes | 50,996,474 / 17,009,142 | 50,996,474 / 17,009,142 |
| Clustered update time (ms) | 476.064 | 705.118 |
| Ordered full-row lookup time (ms) | 42.055 | 52.273 |
| Compaction files removed / added | 4 / 1 | 4 / 1 |
| Compaction time (ms) | 35.261 | 72.151 |
| Fresh reopen time (ms) | 2,718.433 | 9,859.532 |
| Fresh reopen requested Parquet reads | 4,470,969,469 | 17,650,862,921 |
| Peak RSS: build / exercise / reopen (B) | 114,073,600 / 118,370,304 / 57,753,600 | 115,531,776 / 123,006,976 / 63,062,016 |

Raw [operation receipts, artifact hashes and I/O measurements](kv_native_measurements.json)
retain every bounded build operation and all phase results. File sync counts
show that normal durable publication paths ran; they do not simulate power loss.

## Reproduction

Configure the pinned native Release build with storage and tests enabled, then
build `glamdb_kv_mode_bench`. Use a fresh disposable root and ledger for each
size. For example:

```sh
build/benchmarks/glamdb_kv_mode_bench build /tmp/kv-4096 /tmp/kv-4096.json 4096
build/benchmarks/glamdb_kv_mode_bench exercise /tmp/kv-4096 /tmp/kv-4096.json
build/benchmarks/glamdb_kv_mode_bench reopen /tmp/kv-4096 /tmp/kv-4096.json
```

Repeat with `16384` and fresh paths. Allow space for retained history and bounded
scratch, then remove only those disposable roots after all reopen checks. The
2026-11-12 run used Linux 5.4 x86-64, ext4, a KVM AMD EPYC 7713 host with 48
visible vCPUs and approximately 120 GiB RAM, no swap, Clang 18/libc++ and the
repository's pinned Arrow 25/Parquet/Iceberg/SQLite dependencies. Generated
fixture roots were removed after verification; their ledgers remain in the raw
repository evidence.
