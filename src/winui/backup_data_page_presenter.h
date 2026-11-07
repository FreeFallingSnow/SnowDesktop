#pragma once

#include "backup_data_page_model.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::winui
{/**
 * Cached native WinUI 3 presentation for the BackupAndData route.
 *
 * The presenter performs no file I/O, migration, shell launch, restart or
 * private background work. It only emits typed host requests. Host-published
 * snapshots drive lists, ProgressRing, cancellation state and InfoBar output.
 */
class BackupDataPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    BackupDataPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~BackupDataPagePresenter();

    BackupDataPagePresenter(const BackupDataPagePresenter&) = delete;
    BackupDataPagePresenter& operator=(
        const BackupDataPagePresenter&) = delete;
    BackupDataPagePresenter(BackupDataPagePresenter&&) = delete;
    BackupDataPagePresenter& operator=(BackupDataPagePresenter&&) = delete;

    void SetActions(BackupDataPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        Content() const noexcept;

    /** Returns false when the snapshot is uninitialized, stale or closed. */
    [[nodiscard]] bool ApplySnapshot(
        const BackupDataPageSnapshot& snapshot);

    void RefreshLocalizedText();
    void Activate() noexcept;
    void Deactivate() noexcept;

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;

    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept;

    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
