# Native HTTP service (v0)

`apps/server` builds `glamdb`; `apps/cli` builds `glamdb-cli`. The server opens
one existing store through the public C++ facade. Creation remains explicit:

```sh
cmake --preset local
cmake --build --preset local
store_id=$(python3 -c 'import uuid; print(uuid.uuid4())')
./build/local/apps/cli/glamdb-cli create-store --root /tmp/my-glamdb --store-id "$store_id"
export GLAMDB_TOKEN='replace-with-a-random-secret'
./build/local/apps/server/glamdb --root /tmp/my-glamdb --store-id "$store_id" --ui-root "$PWD/ui"
```

Open [the workbench](http://127.0.0.1:8080), enter the token, create a collection,
append rows and query them. Restart with the same root and store ID. No mock
fallback exists. `--port 0` selects a port; the first stdout line reports it as
JSON. Installation places the binaries in `bin` and the UI in `share/glamdb/ui`;
the installed server discovers that UI relative to its executable on Linux.

The offline CLI retains `create-store` and `inspect-store`. Both acquire core
ownership and fail with `STATE_IN_USE` while the server owns the store. Online:

```sh
glamdb-cli request --path /v0/collections
glamdb-cli request --path /v0/collections --body collection.json
glamdb-cli request --path /v0/collections/COLLECTION_UUID/append --body append.json
glamdb-cli request --path /v0/collections/COLLECTION_UUID/scan --body query.json
```

`request` defaults to GET, or POST with `--body FILE` (`-` reads stdin).
`--method`, `--host` and `--port` are explicit. It prints the complete JSON and
exits nonzero for HTTP errors; it never retries. Direct CLI HTTP is loopback-only.
For remote access use the Python SDK with HTTPS through a TLS reverse proxy.

## Contract and ownership

Versioned [JSON Schema wire types](../../fixtures/http/v0.schema.json) define
requests, responses, errors and recovery envelopes. Fixed-schema row types,
UUID semantics and int64 range are additionally checked by the native core.
The executable [v0 route/shape fixtures](../../fixtures/http/v0.json) inventory
every SDK/workbench call, its core operation, request example, status and required
response-field types. `glamdb.native_http` executes them against real storage.
The fixtures are a bounded response-shape vocabulary, not a full JSON Schema
validator. Native `outcome_json_test` separately pins complete recovery/receipt
serialization. The existing [mock contract](HTTP_MOCK_CONTRACT.md) remains a
separate, broader development profile.

| HTTP route (under `/v0`) | Public core operation | Availability |
| --- | --- | --- |
| `GET /capabilities` | Adapter inventory | Native features, predicates, transport limits |
| `GET /collections` | `ListCollections` | Complete bounded list; no pagination |
| `POST /collections` | `CreateCollection` | Fixed-schema append collection |
| `GET /collections:resolve?name=...` | `ResolveCollection` | Exact name |
| `GET /collections/{id}` | `GetCollection` | Identity and current schema |
| `GET /collections/{id}/schema` | `GetCollection` / `Schema` | Stable field IDs |
| `POST /collections/{id}/append` | `AppendDurable` | Synchronous native receipt |
| `GET /collections/{id}/operations/{op}` | `FindAppendOutcome` | Original result or authoritative absence |
| `POST /collections/{id}/scan` | `Scan` | Fixed-view JSON or Arrow IPC |
| `GET .../stats`, `.../snapshots`, `.../files`, `.../files/{file}` | Proposed public metadata methods | `501 UNSUPPORTED_OPERATION` |
| `POST .../explain` | Proposed public explain method | `501 UNSUPPORTED_OPERATION` |

CLI mapping: `create-store` → `CreateStore`, `inspect-store` → `OpenExisting` /
`GetStoreInfo`, `request` → the route/core mapping above.

The transport owns HTTP parsing, codecs, authentication, connection admission
and static UI assets. Core owns store identity/locking, schema validation,
canonical hashes, serialized writes, durable publication, receipts, recovery and
snapshot selection. The adapter never reads catalog tables or enumerates the
warehouse. Metadata/explain must be added through public core methods, not
synthetic values. The UI respects `features` and reports unavailable sections.

`GET /healthz` is public process liveness, **not write readiness**. An opened
engine has completed open/recovery, but core errors may subsequently prevent
writes. `backend=local`, `mock=false` do not imply power-loss qualification;
`durability_validation=designed` remains explicit. Detailed readiness and global
engine resource accounting are separate follow-up contracts.

## Requests and results

Requests use UTF-8 JSON objects with unknown/duplicate fields rejected. Create:
`{collection_id,name,schema:{fields:[{name,type,nullable}]},mode?:"append"}`.
Types: `string`, `bool`, `int64`, `float64`, `timestamp_us_utc`. Core assigns IDs
1..N; supplied field IDs must match. The required string `key` is not unique.
Schema evolution, SQL, upsert and delete are unsupported.

Append: `{operation_id,rows:[{field:value,...}],expected_snapshot?}`. Omitted
precondition means any snapshot; null means no snapshot; a decimal string means
an exact snapshot. Missing nullable row values become null. Int64 and snapshot
IDs **must be decimal strings**, never JSON numbers; floats must be finite and
booleans remain booleans. Timestamps accept RFC3339 offsets and at most six
fractional digits, normalized to UTC with six digits, years 0001–9999.

Scan: `{projection?,predicate?,snapshot_id?,limit?,batch_rows?}`. Omission selects
latest once; null or `"initial"` selects the initial empty view. Historical IDs
are decimal strings. Omitted/null projection defaults to all columns; zero-column projection
is unavailable. Predicates are null/`{op:"true"}` or one comparison
`{op:"eq|ne|lt|le|gt|ge",field,value}` with a correctly typed literal. Other
operators return 501. Limit is an integer 0..100000; batch_rows is 1..1024.
Only TRUE survives NULL filtering. No SQL parser is implied.

JSON scan returns `{schema,snapshot_id,rows,identity,profile}`. Profile currently
contains only the actual `rows_returned` decimal string. It makes no pruning or
physical IO claims. `Accept: application/vnd.apache.arrow.stream` returns a real
schema-bearing Arrow IPC stream, including empty results, with `Content-Length`,
`X-Content-SHA256` and `X-GlamDB-Snapshot-Id`. The entire bounded response is
prepared before sending success headers: an encoding/read error cannot become
an apparently successful truncated result. Larger results fail explicitly;
streaming pagination is a later extension.

Create/append results use the existing native serializer. Append preserves
`committed`, `definitely_not_committed` and `commit_unknown`, phase, original ID,
full receipt/error and recovery context. No 202 or asynchronous admission exists.
A timeout/disconnect/HTTP error alone never proves rollback. Retain the original
ID and payload, query its outcome, and reconcile/reopen when core requires it.
HTTP parsing/codec rejection precedes the core call and returns an error object;
it does not register an operation. No implicit retry occurs in server or CLI.

Errors are `{error:{code,message,retry_hint}}`, with the core retry hint unchanged.
400 invalid request, 401 authentication, 404 authoritative absence, 409 conflict,
412 precondition, 413 oversized body, 415 media type, 429 capacity, 501 unsupported,
503 recovery/dependency/unresolved outcome, 500 unexpected/storage error.
Collections accept `page_size` 1..1000; insufficient size or any cursor returns
501 rather than silently dropping entries or inventing an unstable cursor.

## HTTP service profile

Standalone Asio provides asynchronous sockets, timers and signals. A fixed pool
of four workers calls core; at most 16 connections bound the queue. Saturation
closes new connections before admission. Core retains its single serialized
writer. Header/body timeout defaults to 10 seconds (`--request-timeout-ms`);
response writes have 30 seconds. Header limit: 16 KiB; request/response: 16 MiB;
append: 10000 rows; scan: 100000 rows. JSON depth/node limits also bound parsing.
These transport bounds are not a qualified process-memory or storage budget.

HTTP/1.1 uses Content-Length and closes each response connection. Duplicate
headers, folded headers, Transfer-Encoding, Expect, compressed requests and
pipelined bytes are rejected; keep-alive and chunked requests are not supported.
Use ordinary buffered JSON requests. The server defaults to 127.0.0.1:8080.
`--listen` is explicit; terminate TLS and apply deployment limits at a trusted
reverse proxy for remote access. There is one bearer identity, no user/RBAC layer.
The token comes only from `GLAMDB_TOKEN`, never a URL, log, config response or UI
storage. API responses are uncached. CORS is disabled; the UI uses the same origin.

SIGINT/SIGTERM stop accepting and close incomplete requests; admitted calls finish
and responses drain before Engine::Shutdown releases store ownership. OS IO may
still block; there is no forced rollback or detached writer. Client disconnects
do not cancel publication. Scans are synchronous bounded responses; independent
server-side query cancellation is not advertised.

Static serving allows only index, theme and supported assets beneath `--ui-root`,
rejects traversal and symlinks, and never exposes the store. Administrators must
keep the UI tree trusted and separate from the store.

## Protocol choices

[ClickHouse HTTP](https://clickhouse.com/docs/interfaces/http) provides a useful
cross-language HTTP/format model and documents late streaming error caveats.
GlamDB v0 buffers bounded responses before success headers to avoid that ambiguity.
[Trino's REST protocol](https://trino.io/docs/current/develop/client-protocol.html)
separates query submission, result pages and cancellation; GlamDB advertises only
the synchronous subset its core supports. [PostgreSQL message flow](https://www.postgresql.org/docs/current/protocol-flow.html)
illustrates why requesting cancellation does not prove a write failed. GlamDB
keeps operation identity and durable outcome lookup separate from the connection.

## Validation

```sh
ctest --preset local -R 'glamdb\.(native_http|transport\.|cli)'
# With workbench Python dependencies, also exercise SDK JSON/Arrow decoding:
.venv/bin/python tests/server/native_http_test.py build/local/apps/server/glamdb build/local/apps/cli/glamdb-cli ui
GLAMDB_NATIVE_SERVER="$PWD/build/local/apps/server/glamdb" \
GLAMDB_NATIVE_CLI="$PWD/build/local/apps/cli/glamdb-cli" npm --prefix ui run test:e2e
```

The native suite uses fresh disposable stores, real HTTP, exact scalar checks,
repeat-ID conflicts, fixed snapshots, lost-response lookup, reopen, owner locking,
request framing/auth/limits and UI assets. Mock/client suites remain separate.
Passing these tests does not qualify physical power-loss durability.

The deterministic `glamdb.http_lifecycle` test uses a test-only backend barrier
to disconnect and deliver SIGTERM during publication, verifies that the server
waits, then checks the committed receipt after reopen. It validates lifecycle
coordination; the separate native suite supplies real storage evidence.

Set `GLAMDB_VALIDATE_SCHEMAS=1` when running the native Python suite in an
environment with `jsonschema` to validate each fixture request and actual response
against Draft 2020-12. The standard-library CTest runner always checks route/status
and required response-field types.
