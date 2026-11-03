#pragma once

#include "page_layout_page_actions.h"

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
