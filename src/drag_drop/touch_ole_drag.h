#pragma once

#include <Windows.h>
#include <functional>

namespace snowdesktop::touch_ole_drag
{
// The callback runs from the relay's native button-down. It reports whether
// the original local drag is still active after OLE returns.
bool Start(HWND owner, UINT32 pointerId, POINT screen, std::function<bool(POINT)> runOle);
bool IsRelayMouseMessage(UINT message) noexcept;
bool Owns(HWND owner, UINT32 pointerId) noexcept;
bool Update(POINT screen, bool released) noexcept;
bool Cancel() noexcept;
bool ShouldCancel(bool escapePressed) noexcept;
}
