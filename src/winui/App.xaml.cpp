#include "pch.h"

#include "App.xaml.h"
#include "../app_font.h"
#include "../data_paths.h"
#include "../general_settings.h"
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace winrt::SnowDesktop::implementation
{
App::App()
    : windowsXamlManager_(
          winrt::Microsoft::UI::Xaml::Hosting::WindowsXamlManager::
              InitializeForCurrentThread())
{
    // Load the compiled application resources before any Island content is
    // constructed.  Without this call WinUI controls cannot resolve the
    // XamlControlsResources declared in App.xaml.
    GeneralSettings settings;
    if (LoadGeneralSettings(GetGeneralSettingsPath().c_str(), settings))
        snowdesktop::app_fonts::Select(settings.font, std::filesystem::path(GetExecutableDirectoryPath()) / L"Assets", GetDataDirectoryPath());
    namespace mrt = winrt::Microsoft::Windows::ApplicationModel::Resources;
    ResourceManagerRequested([this](const auto&, const auto& args) {
        fontResourceManager_ = mrt::ResourceManager{};
        fontResourceManager_.ResourceNotFound([](const auto&, const mrt::ResourceNotFoundEventArgs& missing) {
            const auto file = snowdesktop::app_fonts::ResolveXamlResource(missing.Name().c_str(), GetDataDirectoryPath());
            if (!file.empty()) missing.SetResolvedCandidate(mrt::ResourceCandidate(mrt::ResourceCandidateKind::FilePath, file.wstring()));
        });
        args.CustomResourceManager(fontResourceManager_);
    });
    InitializeComponent();
    const winrt::Microsoft::UI::Xaml::Media::FontFamily family{snowdesktop::app_fonts::XamlFamily()};
    Resources().Insert(winrt::box_value(L"ContentControlThemeFontFamily"), family);
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
