# Storage v2 CI note

The Storage v2 side branch extends Observatory CI so the release job runs the repository Python integration/unit tests in addition to C++ `ctest`. This is required because the Storage v2 MVP orchestration, migration, synchronization and retention code is implemented in dependency-free Python tooling.
