#pragma once
#include "status_bar_input_method_identity.h"
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::status_bar_input_method::native_menu
{
using Microsoft::WRL::ComPtr;

// The shell localizes this accessible name independently of our UI language.
// Resolve the OS resource rather than maintaining translated match strings or
// assuming that every SystemTrayIcon/NormalButton is the input mode button.
inline std::wstring ModeButtonName()
{
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    for (const auto package : {L"MicrosoftWindows.Client.Core", L"MicrosoftWindows.Client.CBS"})
    {
        const std::wstring resource = std::wstring(L"@{") + windows + L"\\SystemApps\\" +
            package + L"_cw5n1h2txyewy\\resources.pri?ms-resource://" + package +
            L"/SystemTray/Resources/AutomationName_ImeButton}";
        wchar_t name[256]{};
        if (SUCCEEDED(SHLoadIndirectString(resource.c_str(), name, static_cast<UINT>(std::size(name)), nullptr)) && name[0])
            return name;
    }
    return {};
}

inline bool HasModernTaskbar()
{
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    return taskbar && FindWindowExW(taskbar, nullptr,
        L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr);
}

inline ComPtr<IUIAutomationCondition> Property(IUIAutomation* automation, PROPERTYID id, const wchar_t* text)
{
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(text);
    ComPtr<IUIAutomationCondition> result;
    if (value.bstrVal) automation->CreatePropertyCondition(id, value, &result);
    VariantClear(&value);
    return result;
}

inline HRESULT FindModeButton(IUIAutomation* automation, HWND taskbar, std::wstring_view name,
    ULONGLONG deadline, std::stop_token stop, ComPtr<IUIAutomationElement>& result)
{
    result.Reset();
    if (!automation || !taskbar || name.empty()) return E_INVALIDARG;
    auto id = Property(automation, UIA_AutomationIdPropertyId, L"SystemTrayIcon");
    auto type = Property(automation, UIA_ClassNamePropertyId, L"SystemTray.NormalButton");
    ComPtr<IUIAutomationCondition> condition;
    if (!id || !type) return E_OUTOFMEMORY;
    HRESULT hr = automation->CreateAndCondition(id.Get(), type.Get(), &condition);
    if (FAILED(hr)) return hr;

    // The hidden taskbar root omits its XAML descendants from UIA. Query its
    // existing XAML bridge directly; never reveal the taskbar or scan desktop
    // applications. Older shell versions expose the subtree at the root.
    std::vector<HWND> roots{taskbar};
    EnumChildWindows(taskbar, [](HWND window, LPARAM data) -> BOOL {
        wchar_t typeName[96]{};
        GetClassNameW(window, typeName, static_cast<int>(std::size(typeName)));
        if (wcscmp(typeName, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0)
        {
            auto& values = *reinterpret_cast<std::vector<HWND>*>(data);
            if (values.size() < 4) values.insert(values.begin(), window);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&roots));
    DWORD shellProcess = 0;
    GetWindowThreadProcessId(taskbar, &shellProcess);
    for (HWND window : roots)
    {
        if (stop.stop_requested() || GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(window, &root))) continue;
        ComPtr<IUIAutomationElementArray> elements;
        if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &elements))) continue;
        int count = 0;
        if (FAILED(elements->get_Length(&count))) continue;
        for (int index = 0; index < std::min(count, 32); ++index)
        {
            if (stop.stop_requested() || GetTickCount64() >= deadline) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            ComPtr<IUIAutomationElement> item;
            if (FAILED(elements->GetElement(index, &item))) continue;
            int process = 0;
            if (FAILED(item->get_CurrentProcessId(&process)) || static_cast<DWORD>(process) != shellProcess) continue;
            BSTR accessibleName = nullptr;
            hr = item->get_CurrentName(&accessibleName);
            const bool matches = SUCCEEDED(hr) && accessibleName &&
                MatchesModeButton({accessibleName, SysStringLen(accessibleName)}, name);
            SysFreeString(accessibleName);
            if (matches) { result = std::move(item); return S_OK; }
        }
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

// Worker/MTA only. UIA owns the native button's XAML menu and its lifetime.
// Provider timeouts and a search deadline bound shell restart/hang failures.
inline HRESULT ShowContextMenu(std::wstring_view name, ULONGLONG deadline, std::stop_token stop,
    HWND foreground, DWORD inputThread, HKL layout)
{
    ComPtr<IUIAutomation> automation;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(hr)) return hr;
    ComPtr<IUIAutomation2> timeouts;
    if (SUCCEEDED(automation.As(&timeouts)))
    {
        timeouts->put_ConnectionTimeout(300);
        timeouts->put_TransactionTimeout(300);
    }
    ComPtr<IUIAutomationElement> button;
    hr = FindModeButton(automation.Get(), FindWindowW(L"Shell_TrayWnd", nullptr), name, deadline, stop, button);
    if (FAILED(hr)) return hr;
    if (stop.stop_requested() || GetTickCount64() >= deadline || !foreground ||
        GetForegroundWindow() != foreground || !inputThread || GetKeyboardLayout(inputThread) != layout)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    ComPtr<IUIAutomationElement3> context;
    hr = button.As(&context);
    return FAILED(hr) ? hr : context->ShowContextMenu();
}
}
