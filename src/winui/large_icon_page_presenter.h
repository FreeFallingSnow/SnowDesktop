#pragma once
#include "../large_icon_settings.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <memory>

namespace snowdesktop::winui
{
class LargeIconPagePresenter
{
public:
    LargeIconPagePresenter(std::function<std::wstring(std::string_view)> localize,
        winrt::Microsoft::UI::Xaml::Style style, LargeIconSettingsAction action);
    ~LargeIconPagePresenter();
    winrt::Microsoft::UI::Xaml::Controls::StackPanel Content() const;
    void Activate(std::wstring key);
    void Deactivate();
    void RefreshLocalizedText();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
