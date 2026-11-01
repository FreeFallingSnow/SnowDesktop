# Input, capture and rendering

These extensions share the application control endpoint. Discover support using `api.describe` before using them.

## Private input

1. Get an exact monitor ID and physical screen bounds from `capture.monitors`.
2. Start `input.begin {monitorId,x?,y?,showCursor?}`; x and y must be supplied together. Retain the returned `sessionId`.
3. Inspect `ui.snapshot` for currently visible native element bounds. Coordinates are physical screen pixels, including negative monitor origins. Refresh after layout, menu, popup or search changes. Wait for popup animations to settle before resolving targets.
4. Queue `input.pointer` events: `move` with integer x/y; `down` or `up` with `button:"left"|"right"|"middle"`; `wheel` with nonzero integer `delta` in [-12000,12000]. Clicks are move/down/up; drags move while a button is held. Only supply fields belonging to that event type.
5. Use `input.keyboard` with `type:"down"|"up"` and a supported Windows virtual-key `key`, or `type:"text"` and Unicode `text` (maximum 4096 UTF-8 bytes). Input goes to the current application-local logical focus.
6. Poll `input.state` until `pendingEvents` is zero, check errors, then verify the intended product result. `includeTrace:true` requests the bounded delivered-message trace.
7. Always call `input.end {sessionId}` in cleanup, including after failure. It cancels queued presses/drops; it does not undo completed actions.

Private input does not move the OS cursor or control external applications. Physical input to native SnowDesktop hosts is ignored during the private session, while other apps continue receiving it. Do not leave the session active. Unsupported surfaces fail without falling back to system input. Settings-process input is pending (`settingsPrivateInput:0`); its UIA catalog is read-only. `ui.perform`, `ui.pointerState`, `ui.cancelPointer`, UIA Invoke/SetValue and arbitrary HWND dispatch are not part of this device.

`ui.dismissMenu` closes an active modern menu once the private event queue has drained. Menu IDs are session-scoped, and names/values are separate fields. Individual component-preview button geometry and some popup/system surfaces remain explicitly pending in the catalog.

## Screen capture

- `capture.screenshot {monitorId?,includeSystemCursor?}` starts a PNG task. Omitting the monitor selects the primary display; cursor inclusion defaults to false.
- `capture.record {monitorId?,includeSystemCursor?,durationMs,fps}` records a real monitor as H.264 MP4. Duration: 100..120000 milliseconds; frame rate: 1..60; actual even dimensions up to 3840x2160. Wait for `captureStartedUnixMs>0` and at least one output frame before beginning the demonstrated action.
- Poll `capture.state {id}` until `completed`, `failed` or `cancelled`. `capture.cancel {id}` requests cancellation; continue observing the terminal state. Only the latest capture task is queryable.

Capture includes all visible content on the selected monitor, including other applications. Confirm the intended screen and content with the user's task before recording. Outputs use fresh UUID directories under the active data directory's `Automation/Captures/`. The client cannot choose an arbitrary output filename. State includes paths, timestamps, frame statistics and provenance; cancellation/failure may retain partial output and diagnostics.

## Fixed-data exports

`preview.describe` returns scene schema, supported actions, bounds and a complete example. Use that result to construct `preview.render {scene}`, then poll `preview.state {id}`; cancel with `preview.cancel {id}`. Scenes use fixture data and virtual time; they do not edit the live desktop or prove end-to-end user interaction. Supported native surfaces include collections, file categories, folder mappings, popups, Dock, status bar and Quick Navigation. Output is limited to 3840x2160, 30 seconds and 60 fps, with a further work budget. Results live under `Automation/Demos/` and include scene, poster, sampled frames, silent MP4 and manifest. Arbitrary external programs, personal-file operations and Lua packages are outside this scene schema.
