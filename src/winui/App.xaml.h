#pragma once

#include "App.xaml.g.h"

#include <winrt/Microsoft.UI.Xaml.Hosting.h>

namespace winrt::SnowDesktop::implementation
{
struct App : AppT<App>
{
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void OnLaunched(
        winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    void Close() noexcept;

private:
    winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager
        windowsXamlManager_{nullptr};
};
}
