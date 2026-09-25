# YASB references

- Upstream: https://github.com/amnweb/yasb
- Pinned commit: `d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f`
- License: MIT; the complete upstream notice, including the original project's
  attribution, is retained in `LICENSE` and shipped with SnowDesktop.

`src/taskbar_hook/tray_protocol.h` adapts the private Explorer wire structures
from `src/core/widgets/services/systray/hook/trayhook.cpp` and
`src/core/utils/win32/structs.py`. Tray callback behavior was cross-checked with
`src/core/widgets/services/systray/systray_widget.py` and Microsoft's
[NOTIFYICONDATA documentation](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw).

SnowDesktop's collector, bounded queue, shared memory protocol, lifecycle and
host presentation are separate implementations. In particular, icon pixels and
IPC are processed on a worker, with no pipe waits in Explorer's window thread.
The private wire format is not a Microsoft compatibility contract. Like the
pinned YASB receiver, the decoder accepts a known prefix with opaque trailing
bytes (1484-byte packets were observed on Windows 11). SnowDesktop caps the
envelope at 8192 bytes, copies at most the known structure, and validates field
bounds before use. Truncated, oversized or unsupported-operation packets are
rejected; native notification-area access remains available.

The `Shell_NotifyIconGetRect` compatibility reply is an origin followed by a
width/height pair. Returning a second corner would offset native app menus.
An opt-in real-Explorer regression is available through
`scripts/test.bat name "^tray_live_integration$"` after closing SnowDesktop.
It uses the production collector and service with a separate hidden fixture
process, validating registration, dynamic icons, hidden state, rectangle lookup,
v4/legacy callbacks, removal, teardown and reconnect. It only invokes its own
fixture icon. The test broadcasts `TaskbarCreated` and keeps a temporary Hook
copy pinned until Explorer exits; it never restarts Explorer itself. No Explorer
session or an active desktop host is reported as an environment skip, not a pass.

`src/system_control_bluetooth.cpp` adapts the KS reconnect/disconnect approach
and Bluetooth battery property identification from
`src/core/widgets/services/bluetooth/bluetooth_audio.py` and `bluetooth_api.py`.
It resolves devices by their system identity and MAC, never by display name.
`src/system_control_power.cpp` references the mode GUIDs and dynamic API support
checks from `src/core/widgets/services/power_mode/power_mode_api.py`.
The native implementations use Windows SDK declarations and do not distribute
YASB's Python dependencies. Device execution, cancellation and state readback
are implemented in SnowDesktop's shared control service.
