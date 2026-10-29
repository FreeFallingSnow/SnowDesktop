#include "demo/runtime_workspace.h"
#include "demo/artifact_io.h"
#include "diagnostics/debug_profile.h"
#include "desktop/desktop_source.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace snowdesktop;
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<typename Function> void Reject(Function operation, const char* message)
{
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, message);
}

struct TestDirectory
{
    std::filesystem::path parent, path;
    TestDirectory()
    {
        wchar_t temp[MAX_PATH + 1]{};
        Require(GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) != 0, "temp directory unavailable");
        parent = std::filesystem::canonical(temp);
        path = parent / (L"SnowDemoWorkspaceTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        Require(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "test directory must be newly created");
    }
    ~TestDirectory()
    {
        std::error_code error;
        // Only the exact fresh test directory under its original temp parent.
        if (path.is_absolute() && path.parent_path() == parent &&
            path.filename().wstring().find(L"SnowDemoWorkspaceTests-") == 0)
            std::filesystem::remove_all(path, error);
    }
};

void Image(const std::filesystem::path& path)
{
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)), "demonstration image must decode as a real PNG");
    UINT width = 0, height = 0;
    Require(SUCCEEDED(frame->GetSize(&width, &height)) && width == 960 && height == 640,
        "real image dimensions must match fixture data");
}

void Shortcut(const std::filesystem::path& file, const std::filesystem::path& expected)
{
    ComPtr<IShellLinkW> shortcut;
    ComPtr<IPersistFile> persistence;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shortcut))) &&
        SUCCEEDED(shortcut.As(&persistence)) && SUCCEEDED(persistence->Load(file.c_str(), STGM_READ)),
        "fixture shortcut must load through the real ShellLink API");
    wchar_t path[MAX_PATH]{};
    Require(SUCCEEDED(shortcut->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH)) &&
        std::filesystem::path(path) == expected, "shortcut may only target the actual internal folder");
}

void Checks()
{
    TestDirectory temporary;
    const auto sentinel = temporary.path / L"personal-sentinel.txt";
    { std::ofstream file(sentinel); file << "untouched"; }
    const auto first = demo::PrepareRuntimeWorkspace(temporary.path, "zh-CN");
    const auto second = demo::PrepareRuntimeWorkspace(temporary.path, "en-US");
    Require(first.id != second.id && first.root != second.root, "preparation must always produce a fresh UUID workspace");
    Require(std::filesystem::is_regular_file(first.desktop / L"会议笔记.txt") &&
        std::filesystem::is_regular_file(first.desktop / L"项目资料" / L"任务清单.csv") &&
        std::filesystem::is_directory(first.desktop / L"资料归档"), "desktop entries must correspond to real files and directories");
    Require(std::filesystem::is_regular_file(second.desktop / L"Meeting notes.txt") &&
        std::filesystem::is_regular_file(second.desktop / L"Projects" / L"Project brief.md"), "locale must fix real filesystem names");
    Image(first.desktop / L"旅行照片" / L"山间日出.png");
    Image(second.assets / L"wallpaper.png");
    Shortcut(first.desktop / L"打开项目资料.lnk", first.desktop / L"项目资料");
    auto manifest = demo::ValidateRuntimeWorkspace(first);
    Require(manifest.Find("fixtures")->array.size() == 11 && !manifest.Find("runtimeFilesCopied")->boolean,
        "manifest must record all real fixtures without claiming a copied runtime");

    debug_profile::Configuration config;
    std::string error;
    const auto paths = debug_profile::ResolvePaths(first.normalData);
    Require(debug_profile::Read(paths, config, error) && config.enabled &&
        !config.pendingReset && config.desktop == first.desktop && paths.data == first.activeData,
        "full-runtime debug profile must point to the real isolated desktop and data directory");
    Require(debug_profile::Prepare(paths, config, {}, error), "normal debug profile preparation must accept fixture directories");
    const auto previous = debug_profile::runtimeSession;
    debug_profile::runtimeSession = {paths, config, true};
    const auto actualDesktop = desktop_source::Directory();
    debug_profile::runtimeSession = previous;
    Require(actualDesktop == first.desktop.wstring(), "normal desktop enumeration must select the real demonstration path");
    const auto general = demo::ReadJson(first.activeData / L"SnowDesktop.general.json");
    Require(general.Find("softwareDesktopEnabled")->boolean && !general.Find("demoModeEnabled")->boolean &&
        control::Text(general, "language") == "zh-CN" && general.Find("animationMode")->number == 1,
        "full-runtime settings must use real enumeration, a fixed language and live animation");
    Require(!std::filesystem::exists(first.runtime / L"SnowDesktop.runtime-context.json") &&
        !std::filesystem::exists(first.runtime / L"SnowDesktop.exe"), "preparation must not copy private deployment data or claim an executable");
    Reject([&] { demo::PrepareRuntimeWorkspace(temporary.path / L"missing"); }, "missing parent must not be silently created");
    Reject([&] { demo::PrepareRuntimeWorkspace(temporary.path, "system"); }, "system language is nondeterministic");
    auto wrong = first; wrong.manifest = sentinel;
    Reject([&] { demo::ValidateRuntimeWorkspace(wrong); }, "foreign manifest path must be rejected before reading");
    const auto original = manifest;
    manifest.object["fixtures"].array[0].object["path"] = control::String("Desktop/../../personal-sentinel.txt");
    demo::WriteJson(first.manifest, manifest);
    Reject([&] { demo::ValidateRuntimeWorkspace(first); }, "manifest traversal must fail");
    demo::WriteJson(first.manifest, original);
    Require(!demo::ValidateRuntimeWorkspace(first).IsNull(), "restored manifest must retain its file identities");

    const auto originalFile = second.desktop / L"Meeting notes.txt";
    { std::ofstream changed(originalFile, std::ios::app); changed << "changed"; }
    Reject([&] { demo::ValidateRuntimeWorkspace(second); }, "changed real fixture data must invalidate replay preparation");
    const auto junction = temporary.path / L"reparse-parent";
    if (CreateSymbolicLinkW(junction.c_str(), first.root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
    {
        Reject([&] { demo::PrepareRuntimeWorkspace(junction); }, "reparse-point parent must be rejected");
        Require(RemoveDirectoryW(junction.c_str()) != FALSE, "test symlink must be removed without touching its target");
    }
    else
    {
        const DWORD result = GetLastError();
        Require(result == ERROR_PRIVILEGE_NOT_HELD || result == ERROR_INVALID_PARAMETER,
            "unexpected symbolic-link test setup failure");
        std::cout << "reparse-parent probe unavailable: Windows symbolic-link privilege not enabled\n";
    }
    std::string sentinelText;
    { std::ifstream file(sentinel); std::getline(file, sentinelText); }
    Require(sentinelText == "untouched", "preparation and validation must preserve unrelated files");
}
}

int wmain(int argc, wchar_t** argv)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::cerr << "COM initialization failed\n"; return 1; }
    try
    {
        // Manual fixture preparation for full-runtime acceptance. This test-only
        // mode does not launch/copy a runtime or report a regression-test pass.
        if (argc == 4 && std::wstring_view(argv[1]) == L"--prepare")
        {
            const auto workspace = demo::PrepareRuntimeWorkspace(argv[2], demo::Utf8(argv[3]));
            std::cout << control::Serialize(demo::ValidateRuntimeWorkspace(workspace)) << '\n';
            CoUninitialize(); return 0;
        }
        if (argc != 1) throw std::runtime_error("usage: --prepare EXISTING_ABSOLUTE_PARENT zh-CN|en-US");
        Checks(); CoUninitialize();
        std::cout << "real runtime workspace checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        CoUninitialize(); std::cerr << error.what() << '\n'; return 1;
    }
}
