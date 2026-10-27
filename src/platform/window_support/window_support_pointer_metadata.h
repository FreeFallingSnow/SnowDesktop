#pragma once

#include "window_support_protocol.h"

namespace snowdesktop::window_support
{
// Private semantic metadata; the low bits retain MSLLHOOKSTRUCT flags. The
// packet layout remains compatible with installed v1/v2 providers.
inline constexpr std::uint32_t PointerMinimizeIntent = 0x80000000u;
inline UINT BeforeNativeMinimizeMessage() noexcept
{
    static const UINT message = RegisterWindowMessageW(L"SnowDesktop.WindowSupport.BeforeMinimize.v1");
    return message;
}
inline bool PressPrecedesObservedMinimize(const protocol::PointerEvent& event,
    const RECT& originalFrame, DWORD observedAt, DWORD now) noexcept
{
    return event.message == WM_LBUTTONDOWN && now - observedAt <= 250 &&
        observedAt - event.time <= 250 && PtInRect(&originalFrame, event.point);
}
}
