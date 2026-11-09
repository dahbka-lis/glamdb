# GlamDB

An open-source columnar key-value storage engine in early development.

The repository currently contains a C++23 build scaffold and a no-op executable.
The first engine milestone is durable local append; key-value operations follow.

```sh
cmake --preset local
cmake --build --preset local
ctest --preset local
```

- [Project structure](docs/PROJECT_STRUCTURE.md)
- [Build and quality checks](docs/development/BUILDING.md)
- [Dependencies](third_party/README.md)
- [Implementation stages](docs/ROADMAP.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
