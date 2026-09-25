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
The private wire format is not a Microsoft compatibility contract. Unknown
payloads are rejected; native notification-area access remains available.

Other modules may consult this pinned source for system controls. Any further
adaptation must be recorded here; its individual dependency licenses also apply.
