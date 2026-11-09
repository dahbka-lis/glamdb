# Building

## Requirements

- CMake 3.28 or newer, Ninja, and Python 3.9 or newer.
- C++23 compiler: GCC 14+ or Clang 18+ on Linux. Recent Apple Clang supports
  development builds on macOS.
- LLVM 18 `clang-format` and `clang-tidy` for quality targets.

## Scaffold

```sh
cmake --preset local
cmake --build --preset local
ctest --preset local
./build/local/apps/glamdb/glamdb
```

The executable has a no-op `main`. This build does not fetch or link engine
dependencies. The smoke test verifies that it starts and exits successfully.

Use `release` or `asan-ubsan` in place of `local` for the other build profiles.

```sh
cmake --install build/local
./build/install/local/bin/glamdb
```

The install currently contains the executable. A public library package and its
consumer test will be added with the engine API.

## Quality

```sh
cmake --build --preset local --target format-check tidy-check quality-negative-check
cmake --build --preset local --target format
```

Tools must report LLVM major version 18. Versioned binaries such as
`clang-tidy-18` take precedence over unversioned binaries. Override their paths
with `GLAMDB_CLANG_FORMAT` and `GLAMDB_CLANG_TIDY`.

Checks cover project sources and headers, excluding generated and third-party
files. `tidy-check` uses `compile_commands.json` and rejects missing translation
units. `quality-negative-check` verifies that a formatting violation and a null
dereference in a project header both fail their respective checks.

## Dependencies

See [the dependency guide](../../third_party/README.md). The separate
`deps-local` build installs dependencies into `.deps/install`.

The initial durability qualification targets Linux x86-64 on a dedicated local
ext4 volume. A successful scaffold build, including on macOS, does not validate
engine behavior or durability.
