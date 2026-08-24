#include "pch.h"

#include "App.xaml.h"

namespace winrt::SnowDesktop::implementation
{
App::App()
    : windowsXamlManager_(
          winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager::
              InitializeForCurrentThread())
{
}

App::~App()
{
    Close();
}

void App::OnLaunched(
    winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
{
    // SnowDesktop owns the Win32 window and attaches XAML content explicitly.
}

void App::Close() noexcept
{
    if (!windowsXamlManager_)
        return;

    try
    {
        windowsXamlManager_.Close();
    }
    catch (...)
    {
        // Shutdown is best-effort and must remain noexcept during process exit.
    }
    windowsXamlManager_ = nullptr;
}
}
