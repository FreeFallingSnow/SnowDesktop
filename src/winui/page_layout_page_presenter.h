#pragma once

#include "../page_layout_settings.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::winui
{

struct PageLayoutPageActions
{
    using ConfirmationCompletion = std::function<void(bool confirmed)>;

    std::function<PageLayoutSnapshot()> capture;
    std::function<PageGridChangeImpact(
        const std::wstring& pageId, int columns, int rows)> analyzeGrid;
    std::function<PageLayoutOperationResult(
        std::uint64_t expectedRevision,
        const std::vector<std::wstring>& pageIds)> applyOrder;
    std::function<PageLayoutOperationResult(
        std::uint64_t expectedRevision,
        const std::wstring& pageId, int columns, int rows)> applyGrid;
    std::function<PageLayoutOperationResult(
        std::uint64_t expectedRevision)> addPage;
    std::function<void(
        std::wstring title,
        std::wstring message,
        std::wstring primaryButtonText,
        ConfirmationCompletion completed)> confirm;
};

class PageLayoutPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    PageLayoutPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~PageLayoutPagePresenter();

    PageLayoutPagePresenter(const PageLayoutPagePresenter&) = delete;
    PageLayoutPagePresenter& operator=(const PageLayoutPagePresenter&) = delete;

    void SetActions(PageLayoutPageActions actions);
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        Content() const noexcept;
    void RefreshLocalizedText();
    void Activate();
    void Deactivate() noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
