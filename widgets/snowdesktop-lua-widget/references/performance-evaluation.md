# Component performance evaluation

Use this workflow when a user asks to measure a component, investigate CPU or
allocation cost, or compare a concrete performance change. Select workloads that
match the question; do not add tracing to every visual preview.

## Check the callable contract

Run the bundled CLI:

```powershell
bin\snowwidget.exe capabilities
```

Require `performanceProfile.schemaVersion == 1`. Keep Lua `apiVersion` at 2.
The capability and the actual returned performance result establish support;
a matching application version alone does not. A copied CLI can use
`--host <SnowDesktop.exe>`, as with previews. An older host that returns only
ordinary preview results must fail performance evaluation, rather than produce
a success claim.

## Run a bounded workload

Lint and validate the package first. Use a package the user authorized you to
evaluate and a new output directory outside its distributable contents. The
development-workspace gate applies when editing package files; profiling reads
the package and writes only the requested artifacts:

```powershell
bin\snowwidget.exe profile <package-directory> <new-output-directory> `
  --seconds 5 --warmup-seconds 1 --workload redraw --interval-ms 16 `
  --scope-cpu --host <SnowDesktop.exe>
```

The command uses the real isolated preview engine, Lua quota allocator,
callbacks, named timers and Direct2D rendering. It does not install the package,
operate the desktop host, alter user layout or publish anything. Normal preview
data envelopes and deterministic preview clock semantics remain in effect;
live HTTP, system providers, desktop interaction and desktop composition are
excluded. The SDK records the actual measurement window.

Choose one of these workloads:

- `idle`: dispatch the component's actual timers and render on invalidation.
  It creates no extra periodic redraw timer. Resource sampling remains 1 Hz.
- `redraw`: request rendering at `--interval-ms` while dispatching actual timers.
  This includes applicable cache behavior; it does not force every cached view
  to rebuild or claim to reproduce pointer interaction.

`--seconds` accepts 1–60, `--warmup-seconds` 0–30, and `--interval-ms` 8–1000.
The interval is a requested application redraw period, not presented FPS.
For focused evidence, check `performanceProfile.moduleFiltering` and
`performanceProfile.inputSampling`, then pass `--modules lua,render` or
`--modules resource`, and optionally `--input-sample-every 4` (1–1000).
Available module groups are `resource`, `lua`, `task`, `frame`, `input`, `dock`,
`popup`, `layout`, `render`, `icon` and `cache`. The widget preset permits them
all; resources and profiler boundary markers always remain enabled. An empty
filter uses the preset default. The CLI requires the actual host result to
confirm any requested non-default selection; an older profile-capable host
must not silently ignore it.

Input sampling uses a session-salted operation hash with a nominal one-in-N
ratio. Nested input scopes and metrics in one producer-thread synchronous
operation group agree. Read the actual observed/selected group counts and
category-filtered probe calls in the manifest and `inputDetailSampling` report.
Owner-filtered calls are separate from category filters and probe losses.
These are capture-active diagnostic group observations, not OS input-event
counts or the fixed SQL slice window. Selection does not guarantee serialization
when trace loss or a capture boundary intervenes; inspect report quality. Do not
multiply sampled counts or treat their latency percentiles as the full input
population. This isolated workload does not generate interactive pointer input;
zero observed input groups is expected unless actual input probes execute.

Use existing preview options for span, DPI, locale, appearance, data state and
storage. Initial package load and warmup precede tracing. PNG readback/encoding
and analysis follow it.

Thread CPU is optional and adds instrumentation cost. Omit `--scope-cpu` when
only resource/idle behavior is required. The fixed official Trace Processor is
resolved and hash-checked locally; its first analysis may download that tool.
Use `--offline` when it is already cached and network use must be excluded.
Capture data is not uploaded.

## Inspect the result before making a claim

The command returns JSON. Retain and read:

- `evaluation.json`: workload configuration, span/DPI/appearance, actual draw
  invocation count, process role, report path and coverage statement;
- `trace.pftrace` and `session.json`: raw SDK evidence, configuration, hashes,
  completion, sampling and quality metadata;
- the reported analysis directory: fixed SQL and CSV, `summary.json` and
  `perfetto_analysis_report.md`;
- `preview.png`: the component's ending appearance, useful for checking the
  workload state, with the same limits as any preview image.

Require a completed native session and inspect `summary.quality.valid`, losses,
scope coverage, missing CPU and `finalWidgetSampleConfirmed`. A successful
process exit is not enough to call data complete. Keep incomplete or failed
evidence; choose a fresh output directory for a subsequent run. Existing
evidence directories are refused and are not overwritten or silently cleaned.

Use the actual resource sample window for process CPU rates and cumulative
allocation deltas. Lua allocation traffic counts successful logical requests,
including realloc old/new sizes. Increasing memory is an observation, not proof
of a leak. Missing samples stay null.

Per-instance and module CPU are inclusive views of their outermost recorded
synchronous scopes. They include native child work, overlap across groups and
must not be added as exclusive CPU. They are not pure Lua VM CPU. Shared image,
font and rendering caches remain shared; do not divide their totals among
instances. GPU counters, when present, describe a process, not widget GPU time.
Application redraws and callbacks do not measure presented FPS or OS wakeups.

## Compare a proposed change

Keep package content, storage, data state, span, DPI, display/power state,
workload, duration, warmup, CPU option and tool versions fixed. For a comparison,
run at least five matched repetitions into separate new directories and retain
the raw values and their spread. Alternate the order of the two versions when
practical. Report known differences and unknown environment/content identity.

Do not call a component efficient solely because it generated a trace, ran
without exceptions or stayed below an invented universal threshold. Explain
which observed work dominates, where samples are missing, and which changes
the evidence supports. For small differences, report noise and uncertainty.
The profiling runtime and SDK contribute to process cost; this command alone
does not prove default-off instrumentation overhead or the full desktop's cost.

When desktop interaction, live service behavior, multi-monitor DPI, GPU
presentation or actual installed-instance behavior matters, provide a separate
targeted runtime verification step and preserve the preview's narrower claim.
