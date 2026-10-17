#pragma once
#include "icons/icon_appearance.h"
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <functional>
#include <memory>

namespace snowdesktop::winui
{
class IconAppearancePresenter
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    IconAppearancePresenter(Localize localize, IconAppearanceAction action);
    ~IconAppearancePresenter();
    winrt::Microsoft::UI::Xaml::Controls::StackPanel Content() const;
    void Activate(std::wstring target = {});
    void Deactivate();
    void ApplyGlobalStyle(const IconBeautifySettings& global);
    void RefreshLocalizedText();
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
