# Building, testing and CI

Everything lives in `server/`. Two equivalent front doors:

| | Makefile (quickest on a Mac) | CMake (Windows, Linux, macOS, CI) |
|---|---|---|
| build + test | `make test` | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure` |
| server | `make server` → `build/w2f_server` | target `w2f_server` |
| demo match | `make demo SEED=7` | target `w2f_demo` |

Debug builds use AddressSanitizer (+ UndefinedBehaviorSanitizer on clang / gcc) by default; `-DW2F_SANITIZE=OFF` turns them off,
`-DW2F_WERROR=ON` makes warnings errors, `-DW2F_UE_COMPAT=ON` builds without C++ exceptions and RTTI, the way Unreal Engine compiles game code.
CMake needs 3.16+; on Windows use the "x64 Native Tools" prompt (or Visual Studio's CMake integration) with Ninja.

## What CI checks (`.github/workflows/build.yml`)
* **ubuntu-latest** (clang and gcc), **macos-latest**, **windows-latest** (MSVC): build, then the whole suite (engine + network, several thousand checks) under sanitizers, then the same in Release.
* **Determinism**: each job plays seeded 8-player matches (`scripts/fingerprint.sh`: final state hash, snapshot checksum, spells cast, placements — with the generic shop and with the real 30-champion roster only) in the sanitized build **and** the Release build and requires the two to match; a last job downloads every platform's fingerprint and requires them all to be byte-identical.
* **UE5 flags**: the whole code base, tests included, built and run without exceptions and RTTI.
* `-Werror` is on for clang (the toolchain the code is developed with). gcc and MSVC report warnings without failing until they have been seen clean once (set `werror: "ON"` in the matrix).

## Status of the platforms
macOS (Apple clang, arm64) is what the code is developed and tested on. Linux / Windows / gcc / MSVC are covered by CI only: the Winsock branch of `TcpServer` and the MSVC build have never run on a Windows machine outside CI. Android / iOS are not built yet — the engine is plain C++17 with no platform APIs (only `net/` touches sockets), so it should compile for them unchanged.

## Unreal Engine 5
The engine (`src/`, `include/w2f/`) has no networking, no exceptions, no RTTI, no floating point: it can be compiled as a module of a UE5 project, or run as this standalone server that a UE5 client talks to over the WebSocket protocol (`docs/network-protocol.md`). `make check-isolation` / the `isolation` ctest fail if the engine ever includes a socket header or refers to the network layer.
