# Third-Party Notices

The C++ targets use the C++ standard library, the selected
operating-system/compiler toolchain, and this system library:

- **SQLite** (`SQLite3`), public domain; used for embedded local indexing and
  querying. SQLite is actively maintained. Major concerns are the normal
  security considerations for database files and a native library; it has no
  restrictive licence. SQLite source is not vendored or redistributed here.

Registry collection also invokes these explicit system executables without a
shell:

- **curl** (`/usr/bin/curl`), curl licence (MIT-style); used as the maintained
  HTTP/TLS client. curl is actively maintained. The collector disables
  automatic redirects, bounds request time and response size, and validates
  every redirect itself. Operators must keep their distribution's curl and TLS
  packages patched. The executable is not redistributed here.
- **OpenSSL** (`/usr/bin/openssl`), Apache License 2.0 for current OpenSSL 3
  releases; used only for SHA-256. OpenSSL is actively maintained. Operators
  must provide a maintained OpenSSL 3 command and apply security updates. The
  executable is not redistributed here.

Offline CLI tests use Python 3's standard library to host a loopback fixture
server. Python is PSF-licensed and is a test-only dependency.

No third-party source code is included in this repository. CMake, Ninja,
compilers, SQLite, curl, OpenSSL, and Python are not redistributed by this
project and retain their respective licences. Reassess this file before adding
or distributing any dependency, generated component, vendored asset,
container base image, or external schema.
