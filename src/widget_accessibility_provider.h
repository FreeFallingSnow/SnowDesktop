#pragma once

#include "widget_accessibility_snapshot.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

struct IRawElementProviderSimple;

namespace snowdesktop
{
class WidgetAccessibilityProviderHost final
{
public:
    using SnapshotProvider = std::function<
        std::vector<LuaWidgetAccessibilitySnapshot>()>;
    using FocusProvider = std::function<bool(
        const std::wstring& widgetId, const std::string& nodeKey)>;

    WidgetAccessibilityProviderHost(
        SnapshotProvider snapshotProvider,
        FocusProvider focusProvider);
    ~WidgetAccessibilityProviderHost();

    WidgetAccessibilityProviderHost(
        const WidgetAccessibilityProviderHost&) = delete;
    WidgetAccessibilityProviderHost& operator=(
        const WidgetAccessibilityProviderHost&) = delete;

    bool AttachWindow(HWND window);
    void DetachWindow(HWND window) noexcept;
    bool TryHandleGetObject(HWND window, WPARAM wParam, LPARAM lParam,
        LRESULT& result) const noexcept;

    // Borrowed pointer for focused contract tests. Production callers use
    // TryHandleGetObject so UI Automation owns the marshalled reference.
    IRawElementProviderSimple* RootProvider() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
