#pragma once

#include "diagnostics/debug_profile.h"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace native_drag_commit_cases
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
inline std::string context;

inline void Require(bool value, const std::string& message)
{
    if (!value) throw std::runtime_error(message + " [" + context + "]");
}
inline std::string Read(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    Require(static_cast<bool>(stream), "read actual native model fixture file");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class Profile
{
    bool comInitialized_ = false;
    fs::path parent_;
public:
    fs::path root;
    std::vector<DesktopApp*> apps;
    Profile()
    {
        try
        {
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize native model STA");
        comInitialized_ = true;
        GUID guid{};
        Require(SUCCEEDED(CoCreateGuid(&guid)), "create isolated native model identity");
        wchar_t text[40]{};
        StringFromGUID2(guid, text, 40);
        parent_ = fs::weakly_canonical(fs::temp_directory_path());
        root = parent_ / (std::wstring(L"SnowDesktopNative-") + text);
        fs::create_directories(root);
        namespace profile = snowdesktop::debug_profile;
        const auto paths = profile::ResolvePaths(root / L"normal-data");
        profile::Configuration config;
        config.enabled = true; config.desktop = paths.defaultDesktop;
        std::string error;
        Require(profile::Write(paths, config, error) && profile::Initialize(paths.normalData, {}, error),
            "activate actual debug-profile storage inside the isolated native fixture");
        Access::completionWindow = CreateWindowExW(0, L"STATIC", L"Native model completion", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(Access::completionWindow != nullptr, "create message-only native model notification owner");
        }
        catch (...) { Cleanup(); throw; }
    }
    ~Profile() { Cleanup(); }
    void Cleanup() noexcept
    {
        for (auto* app : apps) Access::StopModel(*app);
        if (Access::completionWindow) DestroyWindow(Access::completionWindow);
        Access::completionWindow = nullptr;
        // Keep the process's profile isolated through detached provider teardown.
        // The owner graph is not Run() initialized and must not shut down Shell.
        std::error_code error;
        const auto actual = fs::weakly_canonical(root, error);
        if (!error && !root.empty() && actual.parent_path() == parent_ &&
            actual.filename().wstring().starts_with(L"SnowDesktopNative-")) fs::remove_all(actual, error);
        if (comInitialized_) CoUninitialize();
    }
    DesktopApp& Model(bool secondary, const fs::path& layout)
    {
        auto& app = Access::Scene(secondary);
        apps.push_back(&app);
        fs::create_directories(layout);
        Access::ConfigureModel(app, layout);
        return app;
    }
};

inline void CheckModel(DesktopApp& app, const std::vector<std::wstring>& keys,
    const DropPreviewList& observed, int source, std::size_t count)
{
    const auto& items = Access::Items(app);
    Require(items.size() == 2 && Access::Pages(app).size() == 1, "native commit and rebuild preserve every item and the page");
    Require(observed.landings.size() == count, "native accepted preview includes exactly the captured source identities");
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto item = std::find_if(items.begin(), items.end(), [&](const auto& value) { return value.layoutKey == keys[i]; });
        Require(item != items.end(), "native commit and reload preserve stable file identity");
        const auto landing = std::find_if(observed.landings.begin(), observed.landings.end(),
            [&](const auto& value) { return value.sourceIndex == i; });
        Require(landing != observed.landings.end() && item->gridCell.pageId == landing->cell.pageId &&
            item->gridCell.column == landing->cell.column && item->gridCell.row == landing->cell.row,
            "actual native commit and reload must fulfill the previously published placement for each source");
        Require(item->gridCell.pageId == L"__page:1" && item->gridCell.column >= 0 && item->gridCell.column < 4 &&
            item->gridCell.row >= 0 && item->gridCell.row < 3 && item->bounds.right > item->bounds.left,
            "actual LayoutItems must publish a valid visible frame after committing the placement");
        const auto frame = Access::SlotFrame(app, item->gridCell.column, item->gridCell.row);
        Require(EqualRect(&frame, &item->bounds) && Access::HasVisibleIcon(app, *item),
            "actual native layout and rebuild must publish the committed file in its corresponding runtime frame");
        if (!source)
            Require(item->gridCell.column == static_cast<int>(i) * 2 + 1 && item->gridCell.row == 1,
                "unobstructed native desktop translation preserves the literal group-relative cells");
        if (count == 1)
            Require(item->gridCell.column == 1 && item->gridCell.row == 1,
                "a native single-source drop must reach the requested unoccupied cell");
    }
    if (count == 2)
        Require(items[0].gridCell.column != items[1].gridCell.column || items[0].gridCell.row != items[1].gridCell.row,
            "native multi-item commit and reload cannot overlap accepted ordinary file cells");
    else if (!source)
        Require(items[1].gridCell.column == 2 && items[1].gridCell.row == 0,
            std::string("native single-item commit preserves the unrelated item's return position; actual=") +
                std::to_string(items[1].gridCell.column) + "," + std::to_string(items[1].gridCell.row));
    const auto& widgets = Access::Widgets(app);
    if (source)
    {
        Require(widgets.size() == 1 && widgets[0].id == L"native-origin" &&
            widgets[0].itemKeys.size() == 2 - count && !widgets[0].autoCollect &&
            widgets[0].gridCell.pageId == L"__page:1" && widgets[0].gridCell.column == 0 && widgets[0].gridCell.row == 0 &&
            widgets[0].gridSpan.columns == 1 && widgets[0].gridSpan.rows == 1,
            "native ownership removal and reload preserve the source widget and only untransferred members");
        if (count == 1)
            Require(widgets[0].itemKeys[0] == keys[1] && !Access::HasVisibleIcon(app, items[1]),
                "native ownership removal must retain the untransferred identity and order without exposing a desktop duplicate");
    }
    else Require(widgets.empty(), "native desktop translation must not create a component");
}

inline void Run()
{
    Profile profile;
    std::size_t cases = 0;
    for (const bool secondary : {false, true})
        for (const int source : {0, 1, 2})
            for (const std::size_t count : {1u, 2u})
            {
                const auto scenario = "source=" + std::to_string(source) + " count=" + std::to_string(count) +
                    " secondary=" + std::to_string(secondary);
                context = scenario + " phase=initial";
                const auto directory = profile.root / (L"case-" + std::to_wstring(cases++));
                fs::create_directories(directory);
                const auto sourceDirectory = snowdesktop::debug_profile::Current().configuration.desktop;
                std::vector<fs::path> files{sourceDirectory / L"A.txt", sourceDirectory / L"B.txt"};
                for (std::size_t i = 0; i < files.size(); ++i)
                { std::ofstream stream(files[i], std::ios::binary); stream << "native-file-" << i; }
                auto& app = profile.Model(secondary, directory / L"layout");
                Access::PopulateModel(app, files, source);
                Require(Access::Save(app), "actual native initial model must save before dragging");
                std::vector<std::wstring> keys;
                for (const auto& item : Access::Items(app)) keys.push_back(item.layoutKey);
                Access::BeginModel(app, source, count);
                const auto observed = Access::ObserveModelPreview(app, {secondary ? -650L : 150L, 150});
                Require(Access::Target(app) == Access::Desktop(app) && observed.targetKind == DropTargetKind::Desktop,
                    "actual native model must resolve and plan the requested desktop target");
                Access::DropModel(app);
                context = scenario + " phase=committed";
                CheckModel(app, keys, observed, source, count);
                Require(Access::Save(app), "explicit synchronous boundary flushes the committed native layout");
                const auto saved = Read(Access::LayoutPath(app));
                for (const int guard : {0, 1, 2})
                {
                    Require(!Access::GuardSave(app, guard) && Read(Access::LayoutPath(app)) == saved,
                        "actual host save guard must preserve the existing layout during incomplete or pending reload state");
                }
                auto& loaded = profile.Model(secondary, directory / L"layout");
                context = scenario + " phase=restore";
                Access::Load(loaded);
                Require(!Access::Ready(loaded) && !Access::Save(loaded) && Read(Access::LayoutPath(loaded)) == saved,
                    "loading disk records alone must not allow an incomplete native model to overwrite the saved layout");
                Access::ReadSnapshot(loaded, files, false);
                Require(!Access::Ready(loaded) && !Access::Save(loaded), "partial native enumeration must keep the layout write barrier");
                Access::ReadSnapshot(loaded, files, true);
                Require(Access::Ready(loaded), "actual native completed snapshot must publish model readiness");
                CheckModel(loaded, keys, observed, source, count);
                Require(Access::Save(loaded), "actual restored native model must save after completing enumeration and layout");
                for (std::size_t i = 0; i < files.size(); ++i)
                    Require(Read(files[i]) == "native-file-" + std::to_string(i),
                        "native logical translation and ownership changes must preserve actual source file bytes");
                Access::StopModel(app); Access::StopModel(loaded);
                for (const auto& file : files) fs::remove(file);
            }
    std::cout << cases << " actual native commit, layout, save-guard and disk/model restoration scenes passed\n";
}
}
