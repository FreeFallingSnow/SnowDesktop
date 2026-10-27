# Perfetto SDK

This directory vendors the unmodified C++ SDK amalgamation from the official
Perfetto v58.2 release (source commit `add693d8b338ba9599dbcbc3e300b1ab8c000897`).
The SDK is copyright The Android Open Source Project, licensed under Apache 2.0;
the complete license is in `LICENSE`. Upstream copyright notices remain in both
amalgamated files.

- Release: https://github.com/google/perfetto/releases/tag/v58.2
- Archive: https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip
- Archive SHA256: `e56126ff81f0f914d35fe03c72419d147acf2890f36fbda7d4558fd5b84c4b09`
- `perfetto.h` SHA256: `df97880d56c765a4c51ffe7fd2c71ce4fbb3164677d81185e8488c1516e87470`
- `perfetto.cc` SHA256: `cfd4d552272f37da310d43c96c418788567d12174017e2fce061797297391e4c`

`cmake/SnowDesktop.Perfetto.cmake` verifies the upstream bytes and generates the
compiler input under the build directory. It contains two reviewed MSVC C4267
adaptations: keep a chunk index as `size_t` to match the producer and consumer
APIs, and explicitly represent the at-most-five-byte uint32 varint length as
`uint32_t`. These conversions do not change the trace wire format. The generated
compiler copy is rewritten only when its contents change. No compiler warning
is disabled, and the files vendored here retain the official release hashes.

Runtime capture, analysis and regression entry points are documented in
[PERFORMANCE.md](../../src/diagnostics/PERFORMANCE.md).
