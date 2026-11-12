# CLI data and inspection validation

Issue #52 was implemented against upstream revision
`135651d596e4efd4e477b6f42d3f7eedcd711e5e`, including the final PR #19 baseline
and completed native server, codec and route dependencies (#48, #49, #51).
The branch was subsequently rebased onto `ad323022e4f9f553047f5c061b713715b1d41c43`
to retain concurrent client-conformance and capacity documentation; its runtime code is
unchanged. CLI test selection for MSan is described below. Validation below was
executed on Linux x86-64 with Clang 18 and the repository's
pinned SQLite/Arrow/Parquet/Iceberg installation on 2026-11-12.

The full native build enables `GLAMDB_BUILD_ADMIN=ON`. The network CLI also
builds independently from `apps/cli`, with its existing OpenSSL/CLI11/Asio/JSON
dependencies and no engine installation.
MSan selects the ordinary standalone client for network requests and explicitly
skips the native admin HTTP scenario, because system OpenSSL is uninstrumented.
The other seven scenarios still exercise the instrumented offline admin and core;
normal, ASan, UBSan and TSan profiles include admin HTTP coverage.

Native integration (`glamdb.cli_data`) compares offline facade calls with HTTP
calls through both executables. Its eight tests cover explicit creation and
missing stores, collection/schema/name lookup, append and original receipt lookup,
precise int64 and timestamp boundaries, duplicate rows, initial/historical scans,
stable projection field IDs and zero-column cardinality, snapshot/file inspection,
historical file membership rejection, restart and exact retries, managed database
routes, exclusive server ownership, stdout failure after commit, multi-page scans,
and rejection without partial output above the 10,000-row result bound. A native
16-row result (16,776,376 bytes with the original review fixture) exercises
completed-result accounting just below 16 MiB.

Protocol evidence is separate: `glamdb.cli_network` runs real localhost HTTP/TLS
exchanges, including literal native outcome fixtures, first-page and continuation
disconnects, changed views, missing rows, repeated cursors, cancellation attempts,
and snapshot/file continuation parameters. Its native lifecycle case additionally
uses the production server. Literal fixture responses do not establish native
commit or recovery behavior.

`glamdb.admin_result` tests use complete literal outcomes and an explicit failed
shutdown status. They verify that shutdown failure preserves the response and
unresolved exit 3, retains a committed receipt with exit 4, and that output failure
cannot downgrade an unresolved outcome. These are adapter control-flow tests;
they do not inject an actual simultaneous native uncertain commit and failed
shutdown. Native core uncertainty and recovery retain their separate fault suites.

A separate read-only review checked the implementation, critiqued its design and
considered failure cases and invariants. It found three issues, each reproduced
by a failing regression before correction: shutdown obscuring an operation
response, repeated page envelopes counted against the final result, and scan
disconnects classified as unresolved writes. The final implementation preserves
outcomes, charges one output envelope, and treats failed scans as read failures.

Complete bounded JSON was retained over a page-at-a-time default: it avoids
successful partial output, while the existing `request` command remains available
for larger explicit cursor and Arrow exports. Shared grammar preserves the
standalone client boundary; shared metadata serializers preserve wire precision
and file provenance. No core publication logic or persistent format changes.

Executed checks:

- Full native Debug build with `GLAMDB_BUILD_ADMIN=ON`: PASS.
- `GLAMDB_REQUIRE_NATIVE_CLIENTS=1 GLAMDB_VALIDATE_SCHEMAS=1 ctest --test-dir
  build/local --parallel 4 --output-on-failure`: zero failures out of 765 enabled
  tests (767 discovered; two disabled, seven expected child/manual skips).
- Standalone `cmake -S apps/cli -B build/client52 -G Ninja
  -DCMAKE_CXX_COMPILER=clang++-18`, build and native/wire suites: PASS
  (eight native scenarios and 19 wire/TLS tests).
- `cmake --build build/local --target format-check quality-negative-check`: PASS.
- `clang-tidy-18 -p build/local` on all eight changed C++ translation units: PASS.
- After rebasing, the updated upstream tooling and CLI/outcome selection passed
  all seven selected CTest cases. The final CLI/outcome selection passed all six
  cases after separating native admin HTTP from the MSan profile.
- CTest with `GLAMDB_TEST_CLI_EXECUTABLE` pointing to the standalone client:
  all six selected CLI/outcome tests PASS; the new integration test honors the
  same client override used by sanitizer CI.

Physical power-loss,
qualification-scale RSS, and local sanitizer campaigns are outside this adapter
validation; the roadmap's G4 status remains **NOT RUN**.
