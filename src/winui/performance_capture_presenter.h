#pragma once
#include "settings/performance_capture_settings.h"
#include <winrt/Microsoft.UI.Xaml.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{
class PerformanceCapturePresenter final
{
public:
    using Localize = std::function<std::wstring(std::string_view)>;
    PerformanceCapturePresenter(Localize localize, const winrt::Microsoft::UI::Xaml::Style& style);
    ~PerformanceCapturePresenter();
    void SetAction(std::function<void(const PerformanceCaptureRequest&)> action);
    void ApplyStatus(const PerformanceCaptureStatus& status);
    void RefreshText();
    winrt::Microsoft::UI::Xaml::FrameworkElement Root() const;
    winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
