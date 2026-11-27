#pragma once

#include <Windows.h>
#include <functional>

namespace snowdesktop::touch_ole_drag
{
// A successful start delivers exactly one callback. True means the OLE
// source can start now. False means the pending handoff was canceled.
bool StartDrop(HWND owner, POINT screen, std::function<void(bool)> complete);
bool IsRelayMouseMessage(UINT message) noexcept;
void CancelPending() noexcept;
bool ShouldCancel(bool escapePressed) noexcept;
}
