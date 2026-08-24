#pragma once

#include "../widget_settings_service.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

/**
 * Host callbacks for diagnostics and mutation feedback emitted by the
 * declarative widget-settings presenter.
 */
struct WidgetSettingsPresenterCallbacks
{
    std::function<void(
        std::string settingKey,
        widget_runtime::WidgetSettingMutationResult result)>
        mutationCompleted;
    std::function<void(
        std::wstring widgetId,
        std::string settingKey,
        std::string diagnosticCode)>
        diagnostic;
};

/**
 * Thread-safe callbacks suitable for WidgetSettingsService::SetEventCallbacks.
 *
 * Each callback captures only a shutdown-aware bridge. It queues the immutable
 * service hint to the presenter's DispatcherQueue, where the authoritative
 * snapshot is read again. A callback retained past Close() becomes a no-op.
 */
struct WidgetSettingsEventDispatchers
{
    widget_runtime::WidgetSettingsService::SnapshotChangedCallback
        snapshotChanged;
    widget_runtime::WidgetSettingsService::SearchCompletedCallback
        searchCompleted;
};

/**
 * Cached WinUI 3 frontend for one widget instance's declarative v2 settings.
 *
 * Secret values, filesystem handles, and logical references never pass through
 * an ordinary string mutation. Every mutation is guarded by widget identity,
 * generation, and the latest accepted revision. Dynamic visibility, enabled
 * state, validation, groups, presets, and reset behavior come exclusively from
 * WidgetSettingsSnapshot.
 */
class WidgetSettingsPresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    WidgetSettingsPresenter(
        widget_runtime::WidgetSettingsService& service,
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~WidgetSettingsPresenter();

    WidgetSettingsPresenter(const WidgetSettingsPresenter&) = delete;
    WidgetSettingsPresenter& operator=(
        const WidgetSettingsPresenter&) = delete;
    WidgetSettingsPresenter(WidgetSettingsPresenter&&) = delete;
    WidgetSettingsPresenter& operator=(WidgetSettingsPresenter&&) = delete;

    void SetCallbacks(WidgetSettingsPresenterCallbacks callbacks);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        Content() const noexcept;

    /**
     * Accepts an authoritative service snapshot on the UI thread. Stale
     * revisions and snapshots which no longer match the service are ignored.
     */
    [[nodiscard]] bool ApplySnapshot(
        const widget_runtime::WidgetSettingsSnapshot& snapshot);

    /** Returns callbacks which safely marshal service events to the UI thread. */
    [[nodiscard]] WidgetSettingsEventDispatchers
        EventDispatchers() const;

    void RefreshLocalizedText();
    void Activate() noexcept;
    void Deactivate() noexcept;

    /**
     * Commits pending text/password edits before route or window close.
     * A persistence failure remains dirty and is returned to the host so the
     * reusable settings window can stay open for retry.
     */
    [[nodiscard]] widget_runtime::WidgetSettingMutationResult
        FlushPendingEdits();

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view settingKey) const noexcept;

    [[nodiscard]] std::wstring_view WidgetId() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept;

    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
