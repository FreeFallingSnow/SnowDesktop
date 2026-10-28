# Application control IPC v1

SnowDesktop exposes a local JSON-RPC 2.0 endpoint while the normal desktop host
is running. External automation and scene replay use this same operation
contract. CLI, MCP and Skill adapters are not required by the host protocol.
The existing settings-child and elevated-window-helper protocols stay private.

## Element and pointer capabilities

`api.describe.capabilities` advertises `elements:1`, `controlledPointer:1` and
`demoCursor:1`. These are additive capabilities within protocol v1; the original
13 methods retain their parameters and behavior. Clients must inspect capabilities
before using an extension.

- `ui.snapshot {}` returns `snapshotRevision`, `coordinateSpace`, `elements`,
  `coveredSurfaces` and `pendingSurfaces`. Coordinates are physical screen pixels,
  including negative monitor origins. Each element has an opaque ID, parent ID,
  surface, role, name, bounds, clipping rectangle, state and supported actions.
  Password values are omitted. IDs use model identities rather than screen
  coordinates; repeated references to the same item are disambiguated within
  their parent. IDs remain valid only while that model object remains present.
- `ui.perform {elementId,action,ifSnapshotRevision,...}` checks the complete
  current snapshot before applying an action. A stale snapshot, hidden/disabled
  element or unsupported action fails explicitly. Take a new snapshot before
  preparing another action. `setValue` accepts `value` (including the empty
  string); `setRangeValue` and `setScrollOffset` accept finite `number` values.
  Lua semantic actions use the existing accessibility implementation. Widget
  and navigation search fields use their existing editor/model path.
- Pointer actions are `hover`, `click`, `doubleClick`, `contextMenu`, `drag` and
  `scroll`, as advertised by each element. `drag` requires `targetId` for a
  current element supporting `drop`; `scroll` requires a nonzero integer `number`
  in [-12000,12000], using Windows wheel units (120 per standard notch).
  Optional `durationMs` is an integer in [100,10000] (default 400);
  optional `showCursor` renders the demo cursor, trail and press/click feedback.
  Pointer actions return an operation ID and `running` state, then execute
  asynchronously through the normal Windows input path. They can launch or
  modify an item in the same way as its ordinary desktop click or drag; use
  isolated data for unattended demonstrations.
- `ui.pointerState {includeTrace?:boolean}` returns operation ID, status,
  error, source/target IDs and current cursor sample. With `includeTrace:true`,
  it includes up to 4096 timestamped physical-screen samples with button state
  and phase. Terminal states are `completed`, `failed` and `cancelled`.
  When an untagged input interrupts playback, optional `interruption` records
  device, Windows message/flags, injection/integrity flags, event coordinates,
  virtual key (keyboard only), and received/expected extra-info tags. Tags are
  decimal strings to preserve pointer-width values beyond JSON's safe integers.
  The `interrupted_by_user` error also covers unrecognized injected input; it
  alone does not establish that a human operated the device.
- `ui.cancelPointer {}` cancels the current operation and returns its state.
  Cancellation releases buttons owned by playback; an active drag also receives
  Escape. It does not roll back changes already applied by the application.

Playback cancels on observed physical or untagged input, never suppresses that
input, and fails if display geometry changes or the destination is occluded by
another window. Injection is subject to normal Windows integrity rules. State
queries and cancellation remain available while playback is active. Completion
means that the input sequence was delivered; confirm the resulting UI/model
state separately. An operation may fail after delivering part of its input.

Current catalogs cover desktop items, common native widget frames/member items
and search, visible Dock entries, Quick Navigation and Lua accessibility nodes.
The response explicitly lists pending surfaces: additional native chrome/tabs,
popup-specific controls, context menus, settings and external/system windows.
`coveredSurfaces` identifies available adapters, not exhaustive acceptance of
every control or every visual state. Pointer motion follows current target
geometry and uses the same procedural cursor renderer intended for later
capture/offline compositing. Scene configuration and video export are a later
stage and are not provided by these methods.

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
keeping UI/input dispatch available. Capture can run while pointer playback is
active. The format is SDR BGRA8/PNG; HDR tone mapping and video export are not part
of this extension. Frame acquisition times out after five seconds. Unsupported
cursor exclusion fails explicitly instead of silently including a system cursor.

## Connection

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
This registry is the discovery source for later adapters. v1 changes may add
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

The first version provides discovery, observation, placement and common UI
operations. Loading fixture data, virtual-clock control, replay, recording,
creating/removing widgets, arbitrary settings mutation and package publication
are not implemented by this endpoint. A scene runner can later compose these
operations and add explicitly scoped capabilities without editing layout files
behind the running host.
