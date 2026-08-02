// SPDX-FileCopyrightText: TranslucentTB contributors
// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// Contains modified portions derived from TranslucentTB's undocumented
// Explorer interface declarations at commit
// 322e2b7395a51975150126276308b415970e080b.

#pragma once

#include <windows.h>
#include <unknwn.h>

// Undocumented Explorer interfaces used by TranslucentTB to observe Task View.
// They are queried only inside Explorer and failure is treated as an optional
// detector being unavailable.
inline constexpr CLSID kImmersiveShellClsid = {
    0xC2F03A33, 0x21F5, 0x47FA,
    { 0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39 }
};
inline constexpr GUID kMultitaskingViewVisibilityServiceSid = {
    0x785702DD, 0xB8EF, 0x469F,
    { 0x8C, 0x19, 0xE9, 0x1B, 0x5F, 0x4C, 0xA5, 0x64 }
};

enum MultitaskingViewType : INT
{
    MultitaskingViewNone = 0x0,
    MultitaskingViewAltTab = 0x1,
    MultitaskingViewTaskView = 0x2,
    MultitaskingViewSnapAssist = 0x4,
    MultitaskingViewAny = 0xF
};

MIDL_INTERFACE("c59a7a3c-0676-4526-8192-5d0bf9b89b95")
IMultitaskingViewVisibilityNotification : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE MultitaskingViewShown(
        MultitaskingViewType flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE MultitaskingViewDismissed(
        MultitaskingViewType flags) = 0;
};

MIDL_INTERFACE("ac11cda3-1601-4ad7-a40e-fe2ced187307")
IMultitaskingViewVisibilityService : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(
        MultitaskingViewType flags, MultitaskingViewType* visibleFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Register(
        IMultitaskingViewVisibilityNotification* notification,
        DWORD* cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unregister(DWORD cookie) = 0;
};
