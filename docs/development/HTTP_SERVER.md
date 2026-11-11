# Native HTTP service

`apps/server` builds `glamdb`; `apps/cli` builds `glamdb-cli`. The server manages named databases through the public C++ facade. Start with an
empty data directory; create and administer databases through HTTP or the UI:

```sh
cmake --preset local
cmake --build --preset local
export GLAMDB_TOKEN='replace-with-a-random-secret'
./build/local/apps/server/glamdb --data-dir /tmp/my-glamdb --ui-root "$PWD/ui"
```

Open [the workbench](http://127.0.0.1:8080), enter the token, create a database,
then create collections, append rows and query them. The database screen also
opens, closes and reconciles databases. Restart with the same data directory. No mock
fallback exists. `--port 0` selects a port; the first stdout line reports it as
JSON. Installation places the binaries in `bin` and the UI in `share/glamdb/ui`;
the installed server discovers that UI relative to its executable on Linux.

Existing standalone stores can still be served with `--root ABSOLUTE_PATH
--store-id UUID`, mutually exclusive with `--data-dir`. This compatibility mode
uses unscoped collection routes and does not offer database management.

`glamdb-cli` is a standalone network client with DNS and verified HTTPS; it does
not link the engine or storage libraries. `create-store --name analytics` calls
the managed database API. The optional `glamdb-admin` contains offline
`create-store`/`inspect-store`, which acquire core ownership and fail with
`STATE_IN_USE` while the server owns the store. See the [client guide](../../apps/cli/README.md)
for its independent build and TLS configuration. Online:

```sh
glamdb-cli request --path /databases
# database.json: {"name":"analytics"}; response includes database_id
# Retrying the same name returns the same database.
glamdb-cli request --path /databases --body database.json
glamdb-cli request --path /databases/DATABASE_UUID/collections --body collection.json
glamdb-cli request --path /databases/DATABASE_UUID/collections/COLLECTION_UUID/append --body append.json
glamdb-cli request --path /databases/DATABASE_UUID/collections/COLLECTION_UUID/scan --body query.json
```

`request` defaults to GET, or POST with `--body FILE` (`-` reads stdin).
`--method`, `--url`, `--host` and `--port` are explicit. It writes the exact response
bytes and exits nonzero for HTTP, transport or output failures; it never retries.
`--accept application/vnd.apache.arrow.stream --output result.arrow --headers headers.json`
retains binary Arrow and cursor/checksum metadata. HTTP is loopback-only; remote
CLI and SDK access use verified HTTPS through a TLS reverse proxy.

## Database lifecycle

| HTTP route | Behavior |
| --- | --- |
| `GET /databases` | List registered identities, names, states and errors |
| `POST /databases` | Create/open `{name,database_id?:UUID,limits?:object}`; returns database info |
| `GET /databases/{id}` | Database info and store profile while open |
| `POST /databases/{id}/open` | Open existing data and run core recovery |
| `POST /databases/{id}/close` | Stop new calls, drain admitted calls and shut down |
| `POST /databases/{id}/reconcile` | Run core reconciliation for an open engine |

Close/reconcile take `{}`; open accepts `{limits?:object}`. All routes require the bearer token. Collection
routes in the next table are relative to `/databases/{id}` in managed mode;
`GET /capabilities` at the server root advertises `features.database_management`.
A database response contains `{database_id,name,state,limits,store?,error?}`; states are
`open`, `closed`, `closing`, `error`. Lists contain `{items,next_cursor:null}`.
Names are unique, case-sensitive UTF-8 (1–128 bytes, no NUL or path separators).
At most 64 databases are registered. Paths are generated from UUIDs under
`--data-dir/databases`; clients cannot provide filesystem paths. Database and
collection identities are separate, and collection/operation IDs are database scoped.

Creation reserves the name and UUID durably before calling `CreateStore`.
Repeated creation with the same name and optional matching UUID returns or
reopens that database; a conflicting name/UUID returns 409. A lost response can
be resolved by listing databases or repeating the same request. There are no
automatic client retries. An unfinished reservation without a store can be
completed only by explicit creation. `open` never creates anything. An incomplete
store, or a missing initialized store, remains an error and is never replaced.
There is no drop/import/rename endpoint in this release.

Optional `limits` configure the public engine limits. Creation merges omitted
fields with defaults; an omitted limits object on retry preserves the existing
configuration. An explicit conflicting creation configuration returns 409.
Open merges partial settings with persisted values; changing values requires a
closed database. Configuration and open intent are durable before opening the
engine. If a lower limit prevents reopening existing data, close the errored
entry and open again with sufficient limits; data is never discarded.

| Limit | Range | Default |
| --- | --- | --- |
| `max_operations` | 1–4096 | 4096 |
| `max_collections` | 1–8 | 8 |
| `max_catalog_bytes` | 65536–268435456 | 268435456 |
| `max_pending_calls` | 1–16 | 16 |
| `max_owned_input_bytes` | 1–134217728 | 134217728 |
| `writer_wait_ms` | 0–5000 | 5000 |
| `append_deadline_ms` | 1–60000 | 60000 |

Values are JSON integers; booleans, fractions, unknown fields and values outside
these bounds are rejected before reserving a database. Every database response
includes normalized limits, including closed/error states. The service registry
upgrades transactionally from version 1 to 2, supplying defaults to existing
entries. Older server binaries reject version 2; core store formats are unchanged.

A separate `registry.sqlite` records identities and intended open state, using
verified SQLite DELETE/EXTRA settings. The registry does not contain collection
catalogs or data. A failed registry write blocks lifecycle mutations until restart,
since its commit may be uncertain; it never triggers speculative store creation.
A missing registry in a nonempty directory fails startup. Each service directory
has one process owner; each core store retains its own identity and owner lock.
Use an absolute canonical data path with an existing parent on trusted local disk.

Open databases reopen on restart; explicitly closed databases stay closed. A
failed store open is listed as an error while other databases remain usable.
Close persists the intent to remain closed before shutdown. Failed shutdown
retains the engine and its ownership in `closing`; fix the reported storage issue
and retry close before reopening. Reconciliation does not undo shutdown. A client
timeout does not roll back lifecycle changes: inspect state or retry the same
action. Per-database leases drain accepted calls and reject new calls during
close/reconcile; unrelated databases remain usable within the fixed worker pool.
Database info/list requests wait for concurrent lifecycle changes. Failed core
reconciliation preserves outcome lookup while core readiness rejects unsafe work.
The workbench clears query results, schema caches and mutation drafts on database
switch, and ignores replies from the previous session.

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

| HTTP route | Public core operation | Availability |
| --- | --- | --- |
| `GET /capabilities` | Adapter inventory | Native features, predicates, transport limits |
| `GET /collections` | `ListCollections` | Complete bounded list; no pagination |
| `POST /collections` | `CreateCollection` | Fixed-schema append collection |
| `GET /collections:resolve?name=...` | `ResolveCollection` | Exact name |
| `GET /collections/{id}` | `GetCollection` | Identity and current schema |
| `GET /collections/{id}/schema` | `GetCollection` / `Schema` | Stable field IDs |
| `POST /collections/{id}/append` | `AppendDurable` | Synchronous native receipt |
| `GET /collections/{id}/operations/{op}` | `FindAppendOutcome` | Original result or authoritative absence |
| `POST /collections/{id}/scan/open` | `Scan` | Resolve a fixed view and return a cursor without consuming rows |
| `POST /collections/{id}/scan` | `Scan` / `ScanCursor::Next` | First page or cursor continuation, JSON or Arrow IPC |
| `POST /collections/{id}/scan/cancel` | `ScanCursor::Cancel` | Release a cursor and its retained resources |
| `GET .../stats`, `.../snapshots`, `.../files`, `.../files/{file}` | Proposed public metadata methods | `501 UNSUPPORTED_OPERATION` |
| `POST .../explain` | Proposed public explain method | `501 UNSUPPORTED_OPERATION` |

CLI mapping: `create-store` → `POST /databases`; `request` exposes every route
in the tables above. Offline `glamdb-admin` uses `CreateStore` / `OpenExisting`.

The transport owns HTTP parsing, codecs, authentication, connection admission
and static UI assets. Core owns store identity/locking, schema validation,
canonical hashes, serialized writes, durable publication, receipts, recovery and
snapshot selection. The adapter never reads core catalog tables or enumerates the
warehouse; its separate registry stores only database lifecycle metadata. Metadata/explain must be added through public core methods, not
synthetic values. The UI respects `features` and reports unavailable sections.

`GET /healthz` is public process liveness, **not write readiness**. An opened
engine has completed open/recovery, but core errors may subsequently prevent
writes. `backend=local`, `mock=false` do not imply power-loss qualification;
`durability_validation=designed` remains explicit. Database lifecycle state is not a per-write readiness guarantee. Global engine
resource accounting remains a separate follow-up contract.

## Requests and results

Requests use UTF-8 JSON objects with unknown/duplicate fields rejected. Create:
`{collection_id,name,schema:{fields:[{name,type,nullable}]},mode?:"append"}`.
Types: `string`, `bool`, `int64`, `float64`, `timestamp_us_utc`. Core assigns IDs
1..N; supplied field IDs must match. A field named `key`, if supplied, must be a
non-nullable string; its values are not unique.
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
operators return 501. Limit is a nonnegative int64 integer; batch_rows is 1..65536.
Only TRUE survives NULL filtering. No SQL parser is implied.

JSON scan returns `{schema,snapshot_id,rows,identity,profile,next_cursor}`. The
snapshot is the resolved selected view, including null for the initial empty
view; `identity` is the core catalog identity captured when planning the scan.
Profile contains only the actual page `rows_returned` decimal string, without
pruning or physical IO claims. `Accept: application/vnd.apache.arrow.stream`
returns a schema-bearing Arrow IPC page, including empty results, with
`Content-Length`, `X-Content-SHA256`, `X-GlamDB-Snapshot-Id`, and an optional
`X-GlamDB-Next-Cursor`. Each page is fully prepared before success headers, so a
read/encoding failure cannot become a successful partial page.

For complete reads, continue with `POST .../scan {"cursor":"TOKEN"}` and the same
Accept header until JSON `next_cursor` is null or the Arrow cursor header is
absent. Each page has at most 10000 rows and targets 4 MiB; a single larger row is
allowed if its complete response fits 16 MiB. Total rows are not capped by the
page size. Writes after scan creation never enter that scan's fixed view.

For retry-safe reading from the first page, first call `POST .../scan/open` with
the ordinary scan body and chosen Accept header. Its JSON response contains
`{cursor,schema,snapshot_id,identity}` and consumes no rows. Use that cursor for
the first page. The last consumed cursor replays exactly the same response;
advancing invalidates older cursors. A lost response to ordinary `/scan` without
a cursor cannot be replayed: opening again creates a new scan. The SDK's
`scan_pages()` uses the explicit open protocol.

`POST .../scan/cancel {"cursor":"TOKEN"}` accepts either the next or most recently
consumed cursor and returns `{"cancelled":true}`. Tokens are scoped to database,
collection and representation; stale or missing tokens return 404, never restart
queries. Eight sessions are available across the server. Idle sessions expire
after 60 seconds (checked every second); completed replay caches may be evicted
to admit new scans. A failed page invalidates its session. Close, reconciliation
and shutdown release sessions after admitted calls drain. Cancellation releases
between page calls; it does not interrupt an active filesystem read.

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
append: 10000 rows. Scan responses are bounded pages. JSON depth/node limits also bound parsing.
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
do not cancel publication. Scan pages are synchronous bounded responses; cursor
cancellation releases the remaining read session.

Static serving allows only index, theme and supported assets beneath `--ui-root`,
rejects traversal and symlinks, and never exposes the store. Administrators must
keep the UI tree trusted and separate from the store.

## Protocol choices

[ClickHouse HTTP](https://clickhouse.com/docs/interfaces/http) provides a useful
cross-language HTTP/format model and documents late streaming error caveats.
GlamDB buffers bounded pages before success headers to avoid that ambiguity.
[Trino's REST protocol](https://trino.io/docs/current/develop/client-protocol.html)
separates query submission, result pages and cancellation; GlamDB exposes these
for its fixed-view scan cursors. [PostgreSQL message flow](https://www.postgresql.org/docs/current/protocol-flow.html)
illustrates why requesting cancellation does not prove a write failed. GlamDB
keeps operation identity and durable outcome lookup separate from the connection.

## Validation

```sh
GLAMDB_REQUIRE_NATIVE_CLIENTS=1 GLAMDB_VALIDATE_SCHEMAS=1 \
ctest --preset local -R 'glamdb\.(native_http|scan_http|database_limits|transport\.|routes\.|cli)'
# With workbench Python dependencies, also exercise SDK JSON/Arrow decoding:
.venv/bin/python tests/server/native_http_test.py build/local/apps/server/glamdb build/local/apps/cli/glamdb-cli ui
GLAMDB_NATIVE_SERVER="$PWD/build/local/apps/server/glamdb" \
npm --prefix ui run test:e2e
```

The native suite starts with an empty service directory and creates databases
through HTTP without an offline CLI. It covers isolation, close/open/reconcile,
restart and process crashes, missing-store preservation, real HTTP, exact scalar checks,
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

`glamdb.routes.Databases.*` additionally checks concurrent name reservations,
request draining across databases, failed-close ownership and retry, incomplete
creation, and registry COMMIT errors before/after rollback-journal unlink. These
are deterministic failure-model tests, not physical power-loss qualification.

Native CI installs hash-pinned SDK, Arrow and JSON Schema dependencies and requires
these checks without silent import skips. Its normal profile also builds the CLI
without engine dependencies and runs the native browser profile. Run the same gate
with `scripts/check-native-http.sh build/local build/client/glamdb-cli`, selecting
its Python environment through `GLAMDB_NATIVE_PYTHON` if needed.

`glamdb.native_http_fault` uses the production managed HTTP service and real
storage with a test-only SQLite VFS control channel. It covers faults before/after
create, append and registry COMMIT, failed recovery fencing, failure recording,
producer path corruption, and close waiting for an admitted native append.
`glamdb.scan_http` covers fixed-view pages beyond 100000 rows and 16 MiB, response
replay, scope, cancellation, expiry, resource capacity, and real read corruption.
These deterministic checks are separate from physical power-loss qualification.
