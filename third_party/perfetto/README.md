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

`SnowDesktopPerfettoSdk` isolates the static CRT, `/bigobj`, and Windows socket
link dependency. `SnowDesktopPerfetto` owns the finite application categories
and capture-worker session/file boundary. Neither library initializes at process
startup. There is no system tracing daemon, injection, or administrator dependency.

The native regression uses the official Windows x64 trace processor v58.2 to
parse the SDK output and assert its semantics. Install the official Perfetto Skill
and invoke its bundled wrapper once (`python <skill>/bin/trace_processor --version`),
or set `SNOWDESKTOP_TRACE_PROCESSOR` to the native executable for the test process.
The default cache name is `~/.local/share/perfetto/prebuilts/trace_processor_shell-adfa6bad3d72be3b.exe`.
Its SHA256 is `adfa6bad3d72be3ba9b83fa2b17b69fa13b3ab1cad0f42e52b86188bd5f0f997`.
Missing tools fail explicitly rather than omitting the regression.

The first integration candidate intentionally tests this SDK boundary before
replacing the host's existing capture backend. It does not enable two concurrent
product capture paths. Product control/CLI migration follows the Windows-native
fixture results; this candidate alone does not change the host's capture format.
