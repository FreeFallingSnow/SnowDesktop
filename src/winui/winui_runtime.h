#pragma once

#include <windows.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>
#include <string>

namespace snowdesktop::winui
{
/**
 * Owns the WinUI 3 objects that must live on SnowDesktop's STA UI thread.
 *
 * Initialize and Shutdown must run on the same thread. The host remains
 * responsible for initializing that thread as an STA (SnowDesktop already
 * does this with OleInitialize) and for dispatching its normal Win32 loop.
 */
class WinUiRuntime final
{
public:
    WinUiRuntime();
    ~WinUiRuntime();

    WinUiRuntime(const WinUiRuntime&) = delete;
    WinUiRuntime& operator=(const WinUiRuntime&) = delete;
    WinUiRuntime(WinUiRuntime&&) = delete;
    WinUiRuntime& operator=(WinUiRuntime&&) = delete;

    [[nodiscard]] bool Initialize() noexcept;
    void Shutdown() noexcept;

    /** Attach a full-window XAML Island to an existing top-level HWND. */
    [[nodiscard]] bool Attach(
        HWND parentWindow,
        const winrt::Microsoft::UI::Xaml::UIElement& content) noexcept;
    void Detach() noexcept;

    /**
     * Enable or clear the Island-owned Mica system backdrop.
     *
     * Enabling must be requested by the HWND host from a later message-loop
     * turn after Attach completes. Creating a system backdrop synchronously
     * while the Island is attaching can wait on a DispatcherQueue that has
     * not begun pumping yet.
     */
    [[nodiscard]] bool SetSystemBackdropEnabled(bool enabled) noexcept;

    /** Resize the Island to the current client rectangle of its parent. */
    void ResizeToClient() noexcept;

    /**
     * Call before accelerator translation/TranslateMessage in the host loop.
     * A true result means WinUI consumed the message.
     */
    [[nodiscard]] bool PreTranslateMessage(MSG* message) noexcept;

    /**
     * Call after accelerator translation and before TranslateMessage so Tab
     * and Shift+Tab can enter the Island from Win32 child controls.
     */
    [[nodiscard]] bool ProcessTabNavigation(MSG* message) noexcept;

    /** Forward WM_SIZE/WM_DPICHANGED/WM_ACTIVATE/WM_NCDESTROY here. */
    void HandleWindowMessage(
        UINT message, WPARAM wParam, LPARAM lParam) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] bool IsAttached() const noexcept;
    [[nodiscard]] HWND ParentWindow() const noexcept;
    [[nodiscard]] HWND IslandWindow() const noexcept;
    [[nodiscard]] const std::wstring& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
