# Project Structure

```text
glamdb/
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── .clang-tidy
├── .editorconfig
├── .gitmodules
├── apps/
│   └── glamdb/
│       ├── CMakeLists.txt
│       └── main.cpp
├── public/
│   └── storage/
├── core/
├── cmake/
│   ├── CompilerOptions.cmake
│   ├── Sanitizers.cmake
│   ├── Quality.cmake
│   └── dependencies/
│       └── sqlite/
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── fault/
│   └── installed_consumer/
├── fixtures/
├── probes/
├── benchmarks/
├── sdk/
├── ui/
├── scripts/
├── docs/
│   ├── PROJECT_STRUCTURE.md
│   ├── ROADMAP.md
│   └── development/
└── third_party/
    ├── arrow/
    ├── iceberg-cpp/
    ├── googletest/
    ├── lock.json
    └── README.md
```

| Path | Purpose |
| --- | --- |
| `public/storage/` | Public C++ API headers. |
| `core/` | Engine implementation and private headers. Keep each component's `.h` and `.cpp` together. |
| `apps/glamdb/` | Application entry point. The current executable exits successfully without output. |
| `cmake/` | Compiler options, sanitizers, quality targets, and dependency recipes. |
| `tests/` | Unit, integration, fault, and installed-library consumer tests. Only the executable smoke test exists today. |
| `fixtures/` | Shared deterministic test inputs and expected outputs. |
| `probes/` | Isolated dependency and platform experiments. |
| `benchmarks/` | Performance and capacity measurements. |
| `sdk/`, `ui/` | Reserved empty directories. |
| `scripts/` | Developer tooling. |
| `docs/development/` | Build and tooling instructions. |
| `docs/ROADMAP.md` | Implementation stages and scope. |
| `third_party/` | Pinned upstream submodules and the direct dependency lock. |

The scaffold leaves engine components unspecified. Transport and SDK dependencies
belong outside the public API and core.

`sdk/` and `ui/` contain no files, including placeholders. Git does not retain
empty directories. Other reserved directories use `.gitkeep`.

Generated files belong in ignored `build/`, `.deps/`, and `artifacts/` directories.
