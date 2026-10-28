# Application control IPC v1

SnowDesktop exposes a local JSON-RPC 2.0 endpoint while the normal desktop host
is running. External automation and scene replay use this same operation
contract. CLI, MCP and Skill adapters are not required by the host protocol.
The existing settings-child and elevated-window-helper protocols stay private.

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
