#pragma once
#include "icons/icon_library.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
class IconLibraryPagePresenter
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    IconLibraryPagePresenter(Localize localize, IconLibraryAction action);
    ~IconLibraryPagePresenter();
    winrt::Microsoft::UI::Xaml::Controls::StackPanel Content() const;
    void Activate(std::wstring target = {});
    void Deactivate();
    void RefreshLocalizedText();
    void FitViewport(winrt::Microsoft::UI::Xaml::Controls::ScrollViewer viewport, double bottomInset);
    static std::function<void()> Pick(winrt::Microsoft::UI::Xaml::XamlRoot root, Localize localize,
        IconLibraryAction action, std::string selected, std::function<void(std::string)> completed);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
