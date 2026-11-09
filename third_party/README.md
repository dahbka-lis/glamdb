# Dependencies

| Dependency | Version | Source |
| --- | --- | --- |
| Arrow / Parquet | 25.0.0 | `arrow/` submodule; Parquet is part of Arrow. |
| Iceberg C++ | 0.4.0 | `iceberg-cpp/` submodule. |
| GoogleTest / GoogleMock | 1.17.0 | `googletest/` submodule. |
| SQLite | 3.54.1 | Official amalgamation archive. |

Exact commits, URLs, and SQLite checksums are recorded in [lock.json](lock.json).
SQLite's archive was checked against the SHA3-256 published on its
[download page](https://www.sqlite.org/download.html).

```sh
git submodule update --init --recursive
cmake --preset deps-local
cmake --build --preset deps-local --parallel 2
```

This explicitly builds and installs dependencies into `.deps/install`. It needs
network access for SQLite and upstream transitive dependencies, plus the build
prerequisites of Arrow and Iceberg, including zlib development files. Set
`GLAMDB_DEPENDENCY_JOBS` when configuring to change parallelism within each
dependency build; the default is two.

The normal application build does not require this step yet. Check the local
submodule revisions without building dependencies:

```sh
cmake -P cmake/dependencies/VerifySubmodules.cmake
```

Arrow enables IPC, compute, filesystem, Parquet, JSON, and compression codecs.
Iceberg uses the same installed Arrow/Parquet and the pinned SQLite library.
SQL catalog support is enabled; REST, Hive, and S3 are disabled. CMake 3.28 is
required by Iceberg's built-in SQL connector.

The lock covers direct dependencies. Transitive dependency recipes belong to the
pinned upstream sources; a complete Linux toolchain and integration receipt is
part of G1. A configured superbuild is not evidence of an installed engine API
or a passing dependency integration probe.

To update a submodule, change its checkout and matching lock entry together.
For SQLite, verify the upstream archive checksum and update the version, URL,
hashes, and source ID. Preserve upstream licenses and notices in the submodules.
