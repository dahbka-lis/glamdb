# Roadmap

The first milestone is an embedded C++23 columnar append store using Arrow,
Parquet, Iceberg, and SQLite. Key-value semantics follow after local durability
has been verified. Append preserves duplicate keys.

| Stage | Deliverable |
| --- | --- |
| G0 | Public contracts, fixtures, and environment limits. |
| G1 | Dependency build, I/O, writer, catalog, and memory probes. |
| G2 | Create, append, close, reopen in a new process, and read a fixed snapshot. |
| G3 | Recovery, fault injection, and lifetime validation. |
| G4 | Linux durability, capacity, installed-package, and quality qualification. |
| K | Key-value updates, deletes, and workload validation. |
| N | HTTP adapter, network SDK, and browser UI. |
| S | Object-storage integration and separate durability qualification. |

The core owns writes and commits. Durable data and metadata files precede the
outer SQLite transaction commit and the committed response. Readers hold a fixed
snapshot and retain the resources needed by their cursors and buffers.

This checkout currently provides build scaffolding only. Engine APIs, probes,
and acceptance gates are not implemented. Each stage requires its own tests
and recorded results before its guarantees can be claimed.

- [Repository structure](PROJECT_STRUCTURE.md)
- [Build and quality checks](development/BUILDING.md)
- [Dependency pins and build](../third_party/README.md)
