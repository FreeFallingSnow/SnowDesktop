# Application control IPC v1 (unreleased)

The complete, normally running SnowDesktop host exposes one local JSON-RPC
endpoint. Scene replay and future Skill adapters use the same public pipe.
Settings-child and privileged-helper IPC remain private. This contract has not
been released; the former system-input playback endpoints have been removed.

## Application-private pointer

Capability `privatePointer:1` provides a software input device owned by the
application UI thread. It never moves the OS cursor, injects `SendInput`, or takes
system capture/focus. Normal physical operation keeps its existing Win32 path
when there is no private session. During a session, SnowDesktop native hosts
ignore physical pointer/keyboard input; other applications receive it normally.

- `input.begin {monitorId,x?,y?,showCursor?}` binds a fresh session to one exact
  entry from `capture.monitors`. Coordinates are physical screen pixels, including
  negative monitor origins. Supply x/y together, or start at the monitor center.
  `showCursor` defaults to true. The returned `sessionId` identifies this lifetime.
- `input.pointer {sessionId,type,...}` queues one raw event. `type:"move"` needs
  integer x/y; `"down"` or `"up"` needs `button:"left"|"right"|"middle"`;
  `"wheel"` needs a nonzero integer `delta` in [-12000,12000]. Fields from another
  event type are rejected. Build a click with move/down/up; build a drag by moving
  while a button is held. Repeated presses use normal double-click thresholds.
- `input.keyboard {sessionId,type,key?,text?}` queues private keyboard input for
  the current logical focus. `type:"down"|"up"` needs a supported Windows virtual
  key code (including Ctrl, Shift, Alt, navigation/editing keys and letters).
  `type:"text"` needs a nonempty valid Unicode `text` string of at most 4096 UTF-8 bytes
  without NUL. It delivers ordinary WM_CHAR messages; it does not paste via the
  clipboard, set a search value or rename an object directly. Hold Ctrl through
  the pointer release when demonstrating group creation, then release the key.
  Missing or changed logical focus fails rather than typing into another host.
  Capability `privateKeyboard:1` advertises this extension.
- `input.state {includeTrace?:boolean}` returns coordinates/buttons, logical
  hover/capture/focus flags, sequence, pendingEvents and errors. A bounded trace
  records the actual messages delivered to real application windows. Events are
  asynchronous: wait for pendingEvents=0, inspect error and verify product state.
  Input delivery alone does not establish that the intended business action ran.
- `input.end {sessionId}` cancels queued input and native presses without sending
  an accidental click/drop, clears hover/capture and hides the decorative cursor.
  It does not roll back a completed action. Always end a recording session.

Presentation uses an independent monotonic clock while a session is idle, so
trails and click rings fade without inventing input samples. Embedded native
editors retain their actual offscreen HWND and renderer; private capture maps
screen coordinates through their visible owner/frame, and focus uses their
ordinary native focus messages without changing the system keyboard queue.

`app.getState` identifies `runtimeMode:"complete-desktop-host"`, the effective
`dataDirectory` and `desktopDirectory`, `debugProfile` and `demoModeEnabled`.
Record these together with the authenticated server EXE identity when validating
real-file demonstrations. A test utility can prepare a copy of an existing demo
desktop; it must not substitute generated preview fixtures for the article data.

The private device routes coordinates through the real desktop, widget, popup,
Quick Navigation, Dock, modern menus and their product component-preview window
procedures. Existing native hit
testing and control handlers decide which control responds. IPC does not accept
an element command, arbitrary HWND or external target. Hover state, button state,
logical capture/focus and timers read the same private device. The demo cursor
only visualizes those delivered events. Moving to an unsupported region never
falls back to operating the system mouse. Monitor geometry changes end the
session with an explicit error. Settings-process input is a separate pending
adapter; its read-only catalog is not proof of pointer support.

The preview companion is the exact HWND supplied by the active menu on the
owner UI thread. It receives the same raw move/down/up input and normal preview
option/apply handlers; class-name discovery cannot add another input target.
`ui.snapshot` reports its physical window rectangle as
`component-preview/active` (`surface:"component-preview"`). The current catalog
marks `component-preview-button-catalog` pending: individual preview button
geometry is not exported yet. A replay may use the actual captured window to
choose coordinates within that rectangle. This is the product's interactive
preview before adding a real component, rather than an offline export renderer.

## Element discovery

`ui.snapshot {}` returns snapshotRevision, coordinateSpace, coveredSurfaces,
pendingSurfaces and visible native element geometry/state. Opaque IDs identify
models for inspection; actions describe pointer affordances. Use the current
visible rectangle to choose a coordinate, then send raw private pointer events.
The removed `ui.perform`, `ui.pointerState` and `ui.cancelPointer` are unavailable.

While the production rename editor is visible, `rename/editor` has
`surface:"rename"`, `role:"edit"`, its actual physical window rectangle,
current text and logical focus. Click that rectangle, then use raw
`input.keyboard` events to edit; this catalog does not provide a value setter
or a rename command. Values longer than 4096 UTF-16 units are marked by
`rename-editor-value-truncated` in `pendingSurfaces`.
The exact live rename HWND is accepted only on the application's own process
and UI thread and only while it is a known interactive text host. A desktop
embedded below Explorer's WorkerW can have that popup's native owner promoted
to the Shell; this does not make WorkerW an input target.

`nativeWidgetControls:1` covers native view/date buttons, category/source tabs,
detail headers/dividers and common frame controls. Geometry comes from the real
renderer and widget hit test. `nativeControlKey` is independent of translations.
Catalog coverage does not claim acceptance of every control or every state.
Native status-bar/system panels, extra popup/search-result controls, system and
external windows remain explicit coverage boundaries.

## Monitor screenshots

`api.describe.capabilities.monitorCapture:1` advertises the asynchronous screenshot
extension. Runtime availability still depends on Windows Graphics Capture, the
interactive session and graphics support.

- `capture.monitors {}` returns monitor IDs, primary flags and physical screen
  rectangles. It does not activate or rearrange windows.
- `capture.screenshot {monitorId?:string,includeSystemCursor?:boolean}` starts one
  screenshot task. Omit `monitorId` to select the primary monitor, or use an exact
  ID from `capture.monitors`. `includeSystemCursor` defaults to false; a visible
  SnowDesktop demo-cursor layer remains part of the composed screen. This avoids
  adding the hardware cursor a second time. It captures the selected monitor's
  complete composed desktop, including other visible application windows.
- `capture.state {id}` returns the latest task's status, error, monitor/bounds,
  dimensions, start/finish Unix milliseconds, frame timestamp in 100ns units,
  source process ID and PNG path. Timestamps exceeding JSON integer precision are
  decimal strings. Poll until `completed`, `failed` or `cancelled`. Only the latest
  task's state is retained; completed files remain available on disk.
- `capture.cancel {id}` requests cancellation of the current task. It is safe to
  call after completion. Poll state for the terminal outcome. A PNG already
  atomically written during cancellation may remain in that task's directory.

Outputs go into a newly created UUID directory under the current data directory's
`Automation/Captures/`; callers cannot supply a destination filename or overwrite
an existing capture. GPU readback and PNG encoding run on a windowless MTA worker,
keeping UI/input dispatch available. Capture can run during a private input
session. Screenshots use SDR BGRA8/PNG. Frame acquisition times out after five seconds. Unsupported
cursor exclusion fails explicitly instead of silently including a system cursor.

## Real monitor video

Capability `monitorRecording:1` adds `capture.record {monitorId?,includeSystemCursor?,durationMs,fps}`.
Duration is 100..120000 real milliseconds; rate is 1..60 fps. The complete selected
monitor is captured at its actual, even pixel dimensions up to 3840x2160. There is
one WGC capture session, real DWM composition and no preview-model renderer. Wait
for `captureStartedUnixMs>0` and at least one output frame before starting actions.

Use `capture.state` and `capture.cancel` as for screenshots. Recording state adds
`kind:"recording"`, `source:"wgc-monitor"`, frames/capturedFrames/duplicatedFrames/
discardedFrames, first/last WGC frame timestamps, actual elapsed time, encoded
duration and manifest/frame-map paths. Integer counts and 100ns timestamps are
decimal strings. Each output frame maps to a real acquired frame in frames.csv;
repeated frames are explicit. The CFR length can round up by one frame. Capture
waits for the full requested real duration, independently of that rounding.

The UUID output directory contains H264 MP4 and provenance. Cancellation or
failure retains diagnostics/partial output without reporting completed. Changed
monitor geometry, a closed capture source or an encoder falling behind the real
clock fails explicitly. Capture includes visible external content on the chosen
monitor: prepare the isolated real desktop and verify the target screen first.

## Modern menus

`capabilities.menus:1` adds visible modern popup menus to the element catalog.
`surface:"menu"` elements include each popup pane, ordinary/quick/inline commands,
submenu entries and text fields. Bounds and clipping use the menu controller's
actual painted item rectangles, scroll offsets and viewport. IDs include a menu
session and command/model reference; take a new snapshot after filtering or
opening a cascade. Closing and reopening a menu creates a new session identity.

Use advertised `click` and `hover` actions for menu items, and `scroll` on a popup
pane. Submenu entries expose `expanded`; wait for the cascade before querying its
items. Menu text editing requires the corresponding private keyboard adapter.

`ui.dismissMenu {}` closes the active modern menu without selecting a command.
Wait for the private pointer event queue to drain first; pending input returns `busy`.
`app.getState.menuOpen` indicates whether a modern menu is still active. Query and
pointer dispatch continue through the menu's existing nested event pump. Native
system-menu fallbacks and external dialogs remain outside this menu adapter.

## Connection

### Offline preview composition (not a complete running desktop)

Capability `previewRender: 1` adds `preview.describe {}`, `preview.render {scene}`,
`preview.state {id}` and `preview.cancel {id}` to this endpoint. `preview.describe` returns
the supported schema, bounds, surface/action lists and an editable complete
example. Scene schema version 1 is independent of the pipe protocol and Lua API.

A scene fixes locale, appearance, clock text, canvas, frame rate, named fixture
items, surface layout and an ordered timeline. Supported native surfaces are
collections, file categories, folder mappings, collection popups, Dock, status
bar and Quick Navigation. Fixture items contain display text, glyph/color and
category data. Popup and navigation layouts preserve their native aspect ratio
and fit within the configured scene rectangles; other widgets lay out directly
in their configured bounds. Fixture items do not reference personal files. Export does not load or
replace the live desktop layout. `locale:"system"` is rejected; the chosen
language must be installed in the host. Canvas dimensions are even, up to
3840x2160; duration is at most 30 seconds, 60fps, with a further work budget.

Timeline actions use virtual milliseconds: cursor movement/press/release,
surface show/hide/move/resize, grid/list presentation, search text and category.
They update the private native render model. These exports demonstrate native
presentation with fixed data; live end-to-end operation checks use `input.pointer`
and observable product state. The render clock advances by frame index, with
native popup motion and the same pointer interpolation/cursor artwork. Idle
frames continue animating trails and click rings. Arbitrary programs, shell
file operations, external windows and arbitrary Lua packages are outside the
offline scene schema.

Capability `demoElementTargets: 1` extends `cursor.move` with
`{atMs,action:"cursor.move",durationMs,target:"work",element:"view"}` instead of
`x,y`. `target` names a collection/files/mapping surface; `element` is its native
control key, such as `view`, `search`, `open`, `category/documents` or
`header/name`. The worker resolves its visible hit-tested center at arrival time
using the configured layout and presentation state, then records the endpoint in
manifest `resolvedTargets`. Unknown, hidden or unavailable controls fail the
export; coordinates are not guessed. Existing XY moves remain supported. A
collection's optional `scrollContainer:false` selects its native mosaic/Show all
presentation; the default remains `true` for existing scenes. These changes are
additive within scene schema 1 and do not alter Lua APIs.

`surface.view` follows native presentation: scrolling collections, files,
mapping and popups use their list/grid view; navigation uses its native collapsed
list versus expanded grid (native search rules still apply). A mosaic collection
has no list toggle, so `surface.view` on `scrollContainer:false` is rejected.
Interactive scene previews use the configured pointer for native button, tab
and search-box feedback; static noninteractive previews keep that feedback off.

Rendering runs in an owned instance of the existing private preview host. The
live host returns immediately with a task ID; poll state for `completedFrames`,
`frames`, terminal status and a result manifest. Only one export runs at a time;
only the latest task remains queryable. Cancel terminates that owned worker job,
preserving earlier outputs. Failed/cancelled directories retain diagnostic or
partial files and never count as a completed export.

Each run creates a new UUID directory under `data/Automation/Demos/` containing
`scene.json`, a native PNG `poster.png`, sampled frame PNGs, `video.mp4`, progress
and result JSON. The manifest records canvas/rate/frame count, host EXE and scene
SHA256, output hashes and sampled-frame times. H264 output is SDR BT.709 with no
audio; Media Foundation is loaded only for export. Missing media components are
reported as an export failure without adding a desktop-startup dependency. PNG
pixel repeatability must be checked on the actual host/font/graphics environment;
the manifest does not claim identical MP4 bytes across codecs or machines.

### Settings window automation

Capability `settingsAutomation: 1` adds `ui.settingsRefresh {}` and
`ui.settingsState {id}`. Open settings normally, request a refresh, and poll its
task until `completed`. Then `ui.snapshot` includes the cached `settings` surface.
Refresh is read-only with respect to product settings. It replaces the previous
settings catalog; one query/action may run at a time. Only the latest task ID is
retained. `elementCount`, `truncated` and timestamps describe the query boundary.
The catalog is limited to 256 controls, 32 levels and a five-second traversal
budget, with UIA connection/transaction timeouts. A partial query is explicit.

The UIA worker currently provides read-only geometry and state for the owned
settings-process window. Direct Invoke/SetValue automation has been removed.
Settings mouse demonstration must use the settings process's own input adapter;
a UIA pattern action is not a substitute for a hover/press/release sequence.
This adapter is pending and must be validated on the actual XAML island.

The Windows named pipe is
`\\.\pipe\SnowDesktop.Control.v1.<sessionId>.<userSid>`.
Resolve `sessionId` and `userSid` from the caller's Windows process token.
Only a client with the same user SID, session and integrity level is accepted.
Remote pipe clients are rejected. The endpoint does not invoke an elevated
helper or provide arbitrary program execution or file-operation verbs.

Each connection carries one request and one response:

1. Send an unsigned 32-bit little-endian byte length, then that many UTF-8 JSON
   bytes. Maximum payload is 1 MiB; the JSON must not include a BOM.
2. Read the same framing for the JSON-RPC response.
3. Send one acknowledgement byte after consuming the complete response, then
   close the connection. This prevents pipe disconnection from discarding an
   unread buffered response. The acknowledgement value is reserved; use `6`.

Use identification-level security quality of service when opening a pipe.
Before sending requests to an independently discovered endpoint, verify the
server process identity and installed SnowDesktop binary; a pipe name alone
does not authenticate a server to its client.

The host processes one connection at a time. Idle/incomplete reads, writes and
queued UI requests have five-second deadlines. A queued request that expires
does not execute later. A disconnect or timeout after execution starts can have
an unknown outcome: query the state before retrying, especially a layout edit.
There is no automatic retry or exactly-once guarantee across connections.

## Requests and results

```json
{"jsonrpc":"2.0","id":"discover-1","method":"api.describe","params":{}}
```

`id` is a string of at most 128 bytes or an integer in the exact JSON safe-integer
range. Notifications and batches are not supported. Parameters are named JSON
objects; unknown fields are rejected. JSON-RPC replies contain either `result`
or `error`; domain failures carry a stable string at `error.data.code`.

`api.describe` returns `protocolVersion`, frame limits and supported operations.
This registry is the discovery source for later adapters. After the first public release, v1 changes may add
methods or optional result fields; existing meanings remain stable. Incompatible
changes require a new protocol major version and endpoint name. This API version
is independent of the Lua component `apiVersion`.

| Method | Parameters | Behavior |
| --- | --- | --- |
| `api.describe` | `{}` | Protocol and method discovery |
| `app.getState` | `{}` | Host PID, locale, desktop visibility, popup/navigation state and animation flags |
| `desktop.getLayout` | `{}` | Active page geometry, widgets and `layoutRevision` |
| `widgets.list` | `{}` | Same layout snapshot, including widget IDs and placement |
| `widgets.get` | `{id}` | One widget and the current layout revision |
| `widgets.place` | `{id,pageId,column,row,columns,rows,ifLayoutRevision}` | Move/resize using the existing placement and displacement rules |
| `widgets.open` | `{id}` | Request a standalone native storage widget's popup |
| `widgets.close` | `{}` | Request closing the current storage popup |
| `navigation.open` | `{}` | Open Quick Navigation |
| `navigation.close` | `{}` | Close Quick Navigation |
| `settings.open` | `{page?}` or `{widgetId}` | Open an allowed settings page or an existing widget's settings |
| `dock.show` | `{}` | Request revealing the floating Dock using existing policy |
| `dock.hide` | `{}` | Close floating Dock presentations; does not disable the persistent Dock |

Settings `page` values are `general`, `desktop`, `widgets`, `backup-and-data`,
`about`, `dock`, `taskbar`, `quick-navigation`, `theme-manager`, `status-bar`,
`appearance-widgets`, `appearance-desktop-icons`,
`appearance-icon-beautification`, `widget-behavior`, and
`animation-performance`. They follow the existing `SettingsPageKey` contract. Omitted
`page` opens General. Debug/developer and direct advanced-function routes are
not exposed by this method. Existing feature availability checks still apply.

UI actions return `status: "accepted"`: this acknowledges the request, not the
completion of an animation or a successful screen recording. Poll
`app.getState` with a bounded deadline to observe presentation readiness and
popup/navigation animation completion. `dock.show` remains subject to the
existing fullscreen and display policies.

Layout placement returns `status: "applied"`, the resulting layout and
`persistence: "queued"`. This is an in-memory result; asynchronous persistence
has not been acknowledged as durable. The operation may displace loose desktop
items according to normal rules. A collision with another widget is rejected.
Coordinates and spans are integral grid cells, not screen pixels.

Always obtain the current `layoutRevision` immediately before a placement and
send it as `ifLayoutRevision`. It is an opaque content identity, not a numeric
counter. Manual changes to relevant page/widget/item placement invalidate old
requests with `stale_layout`. The identity is not a security token. Grouped
children cannot be placed independently. Live drags and context menus reject
UI changes with `busy`.
Close storage popups and Quick Navigation before changing widget placement.

## Execution and lifetime

Pipe I/O runs on a worker; all host operations execute on the owning UI thread.
The endpoint starts after host service initialization and stops before settings,
widgets and windows are torn down. Preview-export processes do not start it.
Failure to acquire the endpoint is logged without disabling the desktop.

The endpoint provides live discovery/operations plus isolated native render
exports. Fixture data and virtual clocks belong to the private renderer; they
do not replace the live user profile. Creating/removing live widgets, general
settings mutation outside the native input path, external application control
and package publication remain outside this endpoint's current contract.
