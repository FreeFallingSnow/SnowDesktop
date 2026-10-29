#include "demo/runtime_workspace.h"
#include "demo/artifact_io.h"
#include "diagnostics/debug_profile.h"
#include "desktop/desktop_source.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
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

std::uint64_t LastWrite(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    Require(GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) != FALSE, "source fixture metadata unavailable");
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

void SetLastWrite(const std::filesystem::path& path, std::uint64_t value)
{
    const HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Require(file != INVALID_HANDLE_VALUE, "test timestamp handle unavailable");
    const FILETIME time{static_cast<DWORD>(value), static_cast<DWORD>(value >> 32)};
    const bool written = SetFileTime(file, nullptr, nullptr, &time) != FALSE;
    CloseHandle(file);
    Require(written, "test timestamp update failed");
}

void CopyChecks()
{
    TestDirectory temporary;
    const auto source = temporary.path / L"source";
    const auto parent = temporary.path / L"copies";
    const auto nested = source / L"原有子目录";
    const auto emptyDirectory = source / L"原有空目录";
    std::filesystem::create_directories(nested);
    std::filesystem::create_directory(emptyDirectory);
    std::filesystem::create_directory(parent);
    const auto note = source / L"原始笔记.txt";
    const auto emptyDocument = nested / L"原有空文档.docx";
    const auto hidden = source / L"hidden.txt";
    const auto linkPath = source / L"入口.lnk";
    { std::ofstream file(note, std::ios::binary); file << "original bytes\r\n"; }
    { std::ofstream file(emptyDocument, std::ios::binary); }
    { std::ofstream file(hidden, std::ios::binary); file << "hidden source bytes"; }
    const auto missingTarget = temporary.path / L"unavailable-program.exe";
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persistence;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link->SetPath(missingTarget.c_str())) && SUCCEEDED(link.As(&persistence)) &&
        SUCCEEDED(persistence->Save(linkPath.c_str(), TRUE)), "test must create a real shortcut with an unavailable target");
    persistence.Reset(); link.Reset();
    Require(SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE) != FALSE,
        "hidden test file attribute failed");
    const std::array<std::filesystem::path, 4> originals{note, emptyDocument, hidden, linkPath};
    std::array<std::string, 4> hashes{};
    std::array<std::uint64_t, 4> dates{};
    for (std::size_t index = 0; index < originals.size(); ++index)
    {
        const auto date = 132541956000000000ULL + index * 1234567ULL;
        SetLastWrite(originals[index], date);
        hashes[index] = demo::Sha256(originals[index]);
        dates[index] = LastWrite(originals[index]);
    }
    SetLastWrite(nested, 132541950000000000ULL);
    SetLastWrite(emptyDirectory, 132541940000000000ULL);
    SetLastWrite(source, 132541930000000000ULL);
    const auto sourceRootTime = LastWrite(source);
    const auto nestedTime = LastWrite(nested);
    const auto emptyDirectoryTime = LastWrite(emptyDirectory);
    const auto workspace = demo::PrepareRuntimeWorkspaceFromDesktop(parent, source, "en-US");
    Require(std::filesystem::equivalent(workspace.sourceDesktop, source), "existing-source identity must remain explicit");
    auto manifest = demo::ValidateRuntimeWorkspace(workspace);
    Require(control::Text(manifest, "dataMode") == "copied-existing-desktop" && manifest.Find("fixtures")->array.size() == 6,
        "small real source must stay a six-entry copy without generated replacements");
    for (std::size_t index = 0; index < originals.size(); ++index)
    {
        const auto copied = workspace.desktop / originals[index].lexically_relative(source);
        Require(std::filesystem::is_regular_file(copied) && demo::Sha256(copied) == hashes[index] &&
            std::filesystem::file_size(copied) == std::filesystem::file_size(originals[index]) &&
            LastWrite(copied) == dates[index], "every real file must retain bytes, length and its original date");
        Require(demo::Sha256(originals[index]) == hashes[index] && LastWrite(originals[index]) == dates[index],
            "preparing a copy must leave original file bytes and dates unchanged");
    }
    Require(std::filesystem::file_size(workspace.desktop / L"原有子目录" / L"原有空文档.docx") == 0,
        "original zero-byte documents must stay zero-byte documents");
    Require(LastWrite(source) == sourceRootTime && LastWrite(workspace.desktop) == sourceRootTime &&
        LastWrite(workspace.desktop / L"原有子目录") == nestedTime &&
        LastWrite(workspace.desktop / L"原有空目录") == emptyDirectoryTime,
        "source and copied directory dates must remain exact");
    Require((GetFileAttributesW((workspace.desktop / L"hidden.txt").c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0,
        "copying a desktop must preserve hidden-file presentation");
    Shortcut(workspace.desktop / L"入口.lnk", missingTarget);
    Require(!std::filesystem::exists(missingTarget) &&
        std::filesystem::is_empty(workspace.assets) && !std::filesystem::exists(workspace.desktop / L"Meeting notes.txt"),
        "shortcut targets must not be materialized and no generated artwork or files may be added");
    const auto audit = demo::ReadJson(workspace.root / L"source-desktop.json");
    Require(audit.Find("beforeAfterMatched")->boolean && audit.Find("shortcutsCopiedAsBytes")->boolean &&
        control::Serialize(*audit.Find("before")) == control::Serialize(*audit.Find("after")),
        "source audit must retain matching before/after metadata and opaque shortcut copying");
    const auto general = demo::ReadJson(workspace.activeData / L"SnowDesktop.general.json");
    Require(control::Text(general, "language") == "en-US" && std::filesystem::exists(workspace.desktop / L"原始笔记.txt"),
        "UI locale must not translate or replace original filenames");

    const auto entriesBeforeFailures = std::distance(std::filesystem::directory_iterator(parent), std::filesystem::directory_iterator{});
    Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(parent, source / L"missing"); }, "missing source must fail without fallback");
    Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(parent, note); }, "source must be a real directory");
    Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(temporary.path, source); }, "source inside parent must be rejected");
    const auto nestedParent = source / L"output-inside-source";
    std::filesystem::create_directory(nestedParent);
    Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(nestedParent, source); }, "parent inside source must be rejected");
    std::filesystem::remove(nestedParent);
    Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(source, source); }, "equal source and parent must be rejected");
    Require(std::distance(std::filesystem::directory_iterator(parent), std::filesystem::directory_iterator{}) == entriesBeforeFailures,
        "invalid source preflight must not create substitute workspaces");

    const auto loop = source / L"cycle";
    if (CreateSymbolicLinkW(loop.c_str(), source.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
    {
        Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(parent, source); }, "recursive source links must be rejected before traversal");
        Require(RemoveDirectoryW(loop.c_str()) != FALSE, "test loop link removal must not touch its target");
        const auto sourceAlias = temporary.path / L"source-alias";
        Require(CreateSymbolicLinkW(sourceAlias.c_str(), source.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE, "source alias setup failed");
        Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(parent, sourceAlias); }, "reparse source root must be rejected");
        Require(RemoveDirectoryW(sourceAlias.c_str()) != FALSE, "source alias removal failed");
        const auto parentAlias = temporary.path / L"parent-alias";
        Require(CreateSymbolicLinkW(parentAlias.c_str(), parent.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE, "parent alias setup failed");
        Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(parentAlias, source); }, "reparse parent root must be rejected");
        Require(RemoveDirectoryW(parentAlias.c_str()) != FALSE, "parent alias removal failed");
        const auto fileAlias = source / L"file-alias.txt";
        Require(CreateSymbolicLinkW(fileAlias.c_str(), note.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE,
            "file alias setup failed");
        Reject([&] { demo::PrepareRuntimeWorkspaceFromDesktop(parent, source); }, "reparse source files must be rejected instead of dereferenced");
        Require(DeleteFileW(fileAlias.c_str()) != FALSE, "file alias removal must not touch the original note");
    }
    else
    {
        const DWORD result = GetLastError();
        Require(result == ERROR_PRIVILEGE_NOT_HELD || result == ERROR_INVALID_PARAMETER,
            "unexpected source-link setup failure");
        std::cout << "source-cycle probe unavailable: Windows symbolic-link privilege not enabled\n";
    }

    { std::ofstream changed(workspace.desktop / L"原始笔记.txt", std::ios::app); changed << "owned change"; }
    Reject([&] { demo::ValidateRuntimeWorkspace(workspace); }, "changed owned copy must invalidate the recorded baseline");
    Require(demo::Sha256(note) == hashes[0], "changing the owned copy cannot change the source file");
    const auto reset = demo::PrepareRuntimeWorkspaceFromDesktop(parent, source, "zh-CN");
    Require(reset.id != workspace.id && demo::Sha256(reset.desktop / L"原始笔记.txt") == hashes[0],
        "reset preparation must create a fresh original-data copy without deleting either tree");
    { std::ofstream extra(reset.desktop / L"unexpected.txt"); extra << "extra"; }
    Reject([&] { demo::ValidateRuntimeWorkspace(reset); }, "unrecorded copied-desktop entries must be rejected");
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
        if (argc == 5 && std::wstring_view(argv[1]) == L"--prepare-from")
        {
            const auto workspace = demo::PrepareRuntimeWorkspaceFromDesktop(argv[2], argv[3], demo::Utf8(argv[4]));
            std::cout << control::Serialize(demo::ValidateRuntimeWorkspace(workspace)) << '\n';
            CoUninitialize(); return 0;
        }
        if (argc != 1) throw std::runtime_error("usage: --prepare PARENT zh-CN|en-US | --prepare-from PARENT SOURCE_DESKTOP zh-CN|en-US");
        Checks(); CopyChecks(); CoUninitialize();
        std::cout << "real runtime workspace checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        CoUninitialize(); std::cerr << error.what() << '\n'; return 1;
    }
}
