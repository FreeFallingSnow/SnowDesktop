# snowwidget GPU diagnostics

`gpu-diagnostics` captures Windows GPU counters using the same `WidgetGpuSampler`
implementation as SnowDesktop. It runs as a separate, temporary sampler, so its
timestamps and intervals are its own. Use it alongside a same-period observation
of the affected adapter in Task Manager or the status bar.

```bat
snowwidget capabilities
snowwidget gpu-diagnostics gpu-sample.jsonl --samples 11 --interval-ms 1000
```

Check that `capabilities.commands` contains `gpu-diagnostics` before invoking it.
The optional `gpuDiagnostics` object advertises `schemaVersion: 1` and
`format: "jsonl"`. This is an additive CLI command: `protocolVersion` and Lua
`apiVersion` remain 2. It does not require a Lua manifest or a higher
`minHostVersion`; older tools, including early builds with the same version
number, lack the command. Obtain the CLI bundled with the candidate being
investigated and retain its recorded executable hash.

The output path must name a **new file** in an existing directory. Creation is
exclusive, so an existing capture is preserved. Defaults are 11 samples and a
1000 ms wait between samples; the first sample normally establishes the usage
baseline. `--samples` accepts 2–120 and `--interval-ms` accepts 250–5000. The total
requested waits must not exceed 120 seconds. OS collection time is additional.
Output is capped at 64 MiB. Duplicate, unknown and malformed options are rejected
before sampling.

Each UTF-8 JSONL record occupies one line:

| `kind` | Contents |
| --- | --- |
| `metadata` | Schema version, tool version and SHA-256, Windows build, architecture, requested samples/interval and output limit |
| `sample` | Unix milliseconds, PDH FILETIME, actual interval and sampling time, collection/topology status, resource reuse counts, adapter snapshots, engine totals and raw/formatted counter rows |
| `summary` | Completion state, captured count, count of intervals with usable usage, and any output-limit reason |

Adapter identity uses the full session LUID, independent of display name and
enumeration order. Each adapter has separate usage, dedicated-memory and
shared-memory validity. Invalid or warming usage is `null`; valid idle usage is
numeric zero. Missing memory usage is `null`; measured zero bytes is `"0"`.
Engine entries preserve physical/index identity, optional empty type label,
process sample count, unclamped sum and bounded usage. The adapter percentage is
the busiest engine after summing processes that share that engine.

To preserve integer precision in JSON consumers, LUIDs, FILETIMEs, byte counts
and raw signed 64-bit counter values are **decimal strings**. Percentages,
millisecond/microsecond durations, counts and unsigned 32-bit status codes are
numbers. Non-finite formatted values become `null`; their original PDH status
and raw rows remain present. Interpret each formatted row according to its
counter path: utilization uses `percent`, memory uses `bytes`.

Exit 0 means the requested capture was written, including when Windows reports
unavailable counters. Inspect `usableUsageSamples`, per-adapter validity and
status codes to judge measurement availability. Exit 2 means invalid arguments;
exit 1 means an output, capture or size-limit failure. The short stdout result
describes capture completion. Each sample is written immediately. If interrupted
with Ctrl+C or terminated, keep complete lines; a missing successful `summary`
marks a partial capture, and an incomplete last line is not a record.

The capture contains GPU models, session identities and process IDs embedded in
counter-instance names. For an AMD discrepancy, preserve the original file,
affected adapter identity and the timestamped comparison. A capture from another
vendor or a different period cannot establish whether the AMD problem is fixed.

The regression suite checks bounds, JSON validity, exact 64-bit values, invalid
versus zero readings and exclusive output creation. Real hardware collection
is a separate explicit validation; hardware absence is recorded as data rather
than replaced with a fixture.
