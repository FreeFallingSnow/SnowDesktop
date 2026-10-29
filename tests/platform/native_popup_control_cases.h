#pragma once

namespace native_popup_control_cases
{
inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto directory = profile.root / L"actual-popup-files";
    std::filesystem::create_directories(directory);
    const std::vector<std::filesystem::path> files{directory / L"Project notes.txt", directory / L"Budget.csv"};
    for (const auto& file : files) { std::ofstream stream(file); stream << "Actual popup catalog fixture\n"; }
    unsigned scenes = 0;
    for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories})
        for (const bool list : {false, true})
            for (const bool negativeOrigin : {false, true})
            {
                auto& app = profile.Model(false, profile.root / (L"catalog-" + std::to_wstring(scenes++)));
                NativeDragIntegrationAccess::CheckPopupControlCatalog(app, files, type, list, negativeOrigin);
            }
    std::cout << scenes << " native popup catalogs preserve real membership and use current grid/list hit geometry\n";
}
}
