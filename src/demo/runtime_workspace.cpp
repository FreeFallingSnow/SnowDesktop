#include "runtime_workspace.h"
#include "artifact_io.h"
#include "ui/preview/preview_png_writer.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace snowdesktop::demo
{
namespace
{
using namespace control;
using Microsoft::WRL::ComPtr;
// 2025-01-15 09:00:00 UTC. Stable file dates exercise the normal date grouping.
constexpr std::uint64_t FixtureWriteTime = 133814052000000000ULL;
constexpr wchar_t OwnerFile[] = L".snowdesktop-demo-workspace.json";

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void SafePath(std::filesystem::path path)
{
    if (!path.is_absolute()) throw std::runtime_error("demo_workspace_requires_absolute_path");
    for (; !path.empty();)
    {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                throw std::runtime_error("demo_workspace_path_unavailable");
        }
        else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("demo_workspace_reparse_point");
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
}

void NewDirectory(const std::filesystem::path& path)
{
    SafePath(path);
    if (!CreateDirectoryW(path.c_str(), nullptr))
        throw std::runtime_error("demo_workspace_directory_not_new:" + std::to_string(GetLastError()));
}

void NewFile(const std::filesystem::path& path, std::string_view bytes)
{
    SafePath(path);
    Handle output;
    output.value = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("demo_workspace_file_not_new:" + std::to_string(GetLastError()));
    DWORD written = 0;
    if (bytes.size() > MAXDWORD || !WriteFile(output.value, bytes.data(),
            static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() ||
        !FlushFileBuffers(output.value))
        throw std::runtime_error("demo_workspace_write_failed");
}

void Timestamp(const std::filesystem::path& path)
{
    SafePath(path);
    Handle file;
    file.value = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILETIME time{static_cast<DWORD>(FixtureWriteTime), static_cast<DWORD>(FixtureWriteTime >> 32)};
    if (file.value == INVALID_HANDLE_VALUE || !SetFileTime(file.value, &time, &time, &time))
        throw std::runtime_error("demo_workspace_timestamp_failed");
}

std::uint64_t WriteTime(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        throw std::runtime_error("demo_workspace_fixture_missing");
    return (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
}

std::string Guid()
{
    GUID value{};
    if (FAILED(CoCreateGuid(&value))) throw std::runtime_error("demo_workspace_id_failed");
    wchar_t text[40]{};
    if (!StringFromGUID2(value, text, static_cast<int>(std::size(text))))
        throw std::runtime_error("demo_workspace_id_failed");
    return Utf8(std::wstring_view(text + 1, 36));
}

void NewShortcut(const std::filesystem::path& output, const std::filesystem::path& target)
{
    if (std::filesystem::exists(output)) throw std::runtime_error("demo_workspace_shortcut_not_new");
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link->SetPath(target.c_str())) || FAILED(link->SetWorkingDirectory(target.parent_path().c_str())) ||
        FAILED(link->SetDescription(L"SnowDesktop isolated demonstration files")) ||
        FAILED(link.As(&file)) || FAILED(file->Save(output.c_str(), TRUE)))
        throw std::runtime_error("demo_workspace_shortcut_failed");
}

void NewPicture(const std::filesystem::path& path, bool warm)
{
    SafePath(path);
    if (std::filesystem::exists(path)) throw std::runtime_error("demo_workspace_picture_not_new");
    constexpr int width = 960, height = 640;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            const bool hill = y > 310 + (x - 480) * (x - 480) / 1600;
            const bool foreground = y > 450 - x / 6;
            const bool sun = (x - 710) * (x - 710) + (y - 165) * (y - 165) < 65 * 65;
            unsigned red = static_cast<unsigned>((warm ? 250 : 155) - y * (warm ? 45 : 70) / height);
            unsigned green = static_cast<unsigned>(220 - y * 60 / height);
            unsigned blue = static_cast<unsigned>((warm ? 178 : 249) - y * 45 / height);
            if (hill) { red = warm ? 179u : 72u; green = warm ? 152u : 150u; blue = warm ? 142u : 162u; }
            if (foreground) { red = warm ? 116u : 35u; green = warm ? 107u : 109u; blue = warm ? 115u : 127u; }
            if (sun) { red = 255; green = 243; blue = 209; }
            pixels[static_cast<std::size_t>(y) * width + x] = 0xff000000u | (red << 16) | (green << 8) | blue;
        }
    std::string error;
    if (!preview_png::Save(path, width, height, pixels, error))
        throw std::runtime_error("demo_workspace_picture_failed:" + error);
}

Value PathValue(const std::filesystem::path& path) { return String(Utf8(path.wstring())); }
std::filesystem::path Relative(const std::string& text)
{
    if (text.find('\0') != std::string::npos) throw std::runtime_error("demo_workspace_manifest_path_invalid");
    const auto path = std::filesystem::path(Wide(text));
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        throw std::runtime_error("demo_workspace_manifest_path_invalid");
    for (const auto& part : path)
        if (part == L".." || part == L"." || part.native().find(L':') != std::wstring::npos)
            throw std::runtime_error("demo_workspace_manifest_path_invalid");
    return path;
}
}

RuntimeWorkspace PrepareRuntimeWorkspace(const std::filesystem::path& parent, std::string_view locale)
{
    if (locale != "zh-CN" && locale != "en-US") throw std::runtime_error("demo_workspace_locale_unsupported");
    SafePath(parent);
    if (!std::filesystem::is_directory(parent)) throw std::runtime_error("demo_workspace_parent_missing");
    if (parent.native().size() > 120) throw std::runtime_error("demo_workspace_parent_too_long");
    RuntimeWorkspace workspace;
    workspace.id = Guid(); workspace.locale = std::string(locale);
    workspace.root = parent / Wide(workspace.id);
    workspace.desktop = workspace.root / L"Desktop";
    workspace.assets = workspace.root / L"Assets";
    workspace.runtime = workspace.root / L"runtime";
    workspace.normalData = workspace.runtime / L"data";
    workspace.activeData = workspace.runtime / L"data.debug" / L"data";
    workspace.debugControl = workspace.normalData / L"SnowDesktop.debug-profile.json";
    workspace.manifest = workspace.root / L"workspace.json";
    NewDirectory(workspace.root);
    // The root is never reused or recursively cleaned up on any error. A failed
    // preparation leaves its owned partial directory as diagnosable evidence.
    NewFile(workspace.root / OwnerFile, Serialize(Object({{"schemaVersion", Number(1)},
        {"kind", String("SnowDesktop.real-runtime-workspace")}, {"id", String(workspace.id)}})));
    for (const auto& path : {workspace.desktop, workspace.assets, workspace.runtime, workspace.normalData,
            workspace.runtime / L"data.debug", workspace.activeData}) NewDirectory(path);

    const bool chinese = locale == "zh-CN";
    const auto projects = workspace.desktop / (chinese ? L"项目资料" : L"Projects");
    const auto photos = workspace.desktop / (chinese ? L"旅行照片" : L"Travel photos");
    const auto archive = workspace.desktop / (chinese ? L"资料归档" : L"Archive");
    for (const auto& path : {projects, photos, archive}) NewDirectory(path);
    const auto note = workspace.desktop / (chinese ? L"会议笔记.txt" : L"Meeting notes.txt");
    NewFile(note, chinese ? "\xef\xbb\xbf周五设计讨论\r\n\r\n主题：让桌面操作更清晰。\r\n待办：整理项目文件、挑选旅行照片、检查演示流程。\r\n"
                         : "Friday design meeting\r\n\r\nTopic: a clearer desktop workflow.\r\nTasks: organize project files, select travel photos, review the demonstration.\r\n");
    NewFile(projects / (chinese ? L"项目说明.md" : L"Project brief.md"), chinese
        ? "# 桌面设计项目\n\n这是隔离演示目录中的真实 Markdown 文件。\n\n- 调整集合视图\n- 查看文件详情\n- 归档完成的资料\n"
        : "# Desktop design project\n\nThis is a real Markdown file in the isolated demonstration directory.\n\n- Change the collection view\n- Inspect file details\n- Archive completed work\n");
    NewFile(projects / (chinese ? L"任务清单.csv" : L"Task list.csv"),
        "Task,Owner,Status\r\nLayout,Snow,Ready\r\nFiles,Snow,Ready\r\nDemo,Snow,In progress\r\n");
    NewFile(archive / (chinese ? L"归档说明.txt" : L"Archive notes.txt"),
        "This folder contains demonstration files only.\r\nAll paths are local to this workspace.\r\n");
    NewPicture(photos / (chinese ? L"山间日出.png" : L"Mountain sunrise.png"), true);
    NewPicture(photos / (chinese ? L"湖畔清晨.png" : L"Lakeside morning.png"), false);
    NewPicture(workspace.assets / L"wallpaper.png", false);
    NewShortcut(workspace.desktop / (chinese ? L"打开项目资料.lnk" : L"Open projects.lnk"), projects);

    Value fixtures = Array();
    std::vector<std::filesystem::path> paths;
    for (const auto& root : {workspace.desktop, workspace.assets})
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    // Descendants first so preparing files cannot change their parent's date.
    for (const auto& path : paths) if (!std::filesystem::is_directory(path)) Timestamp(path);
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) if (std::filesystem::is_directory(*it)) Timestamp(*it);
    Timestamp(workspace.desktop); Timestamp(workspace.assets);
    for (const auto& path : paths)
    {
        const bool directory = std::filesystem::is_directory(path);
        auto entry = Object({{"path", String(Utf8(path.lexically_relative(workspace.root).generic_wstring()))},
            {"type", String(directory ? "directory" : "file")}, {"writeTime100ns", String(std::to_string(FixtureWriteTime))}});
        if (!directory)
        {
            entry.object.emplace("bytes", Number(static_cast<double>(std::filesystem::file_size(path))));
            entry.object.emplace("sha256", String(Sha256(path)));
        }
        fixtures.array.push_back(std::move(entry));
    }
    const auto general = Object({{"softwareDesktopEnabled", Boolean(true)}, {"demoModeEnabled", Boolean(false)},
        {"language", String(workspace.locale)}, {"desktopIdleHideEnabled", Boolean(false)},
        {"doubleClickHideDesktop", Boolean(false)}, {"dockEnabled", Boolean(false)},
        {"forceRestartHotkeyEnabled", Boolean(false)}, {"animationMode", Number(1)},
        {"animationOnBattery", Boolean(false)}, {"animationEnergySaver", Boolean(false)}});
    NewFile(workspace.activeData / L"SnowDesktop.general.json", Serialize(general));
    NewFile(workspace.debugControl, Serialize(Object({{"enabled", Boolean(true)}, {"pendingReset", Boolean(false)},
        {"pendingDesktopChange", Boolean(false)}, {"desktop", PathValue(workspace.desktop)}})));
    NewFile(workspace.manifest, Serialize(Object({{"schemaVersion", Number(1)},
        {"kind", String("real-files-full-runtime")}, {"id", String(workspace.id)}, {"locale", String(workspace.locale)},
        {"root", PathValue(workspace.root)}, {"desktop", PathValue(workspace.desktop)},
        {"runtime", PathValue(workspace.runtime)}, {"normalData", PathValue(workspace.normalData)},
        {"activeData", PathValue(workspace.activeData)}, {"debugControl", PathValue(workspace.debugControl)},
        {"fixtures", std::move(fixtures)}, {"runtimeFilesCopied", Boolean(false)}})));
    (void)ValidateRuntimeWorkspace(workspace);
    return workspace;
}

control::Value ValidateRuntimeWorkspace(const RuntimeWorkspace& workspace)
{
    SafePath(workspace.root);
    if (workspace.id.empty() || workspace.root.filename() != Wide(workspace.id) ||
        workspace.desktop != workspace.root / L"Desktop" || workspace.assets != workspace.root / L"Assets" ||
        workspace.runtime != workspace.root / L"runtime" || workspace.normalData != workspace.runtime / L"data" ||
        workspace.activeData != workspace.runtime / L"data.debug" / L"data" ||
        workspace.debugControl != workspace.normalData / L"SnowDesktop.debug-profile.json" ||
        workspace.manifest != workspace.root / L"workspace.json")
        throw std::runtime_error("demo_workspace_paths_mismatch");
    SafePath(workspace.root / OwnerFile); SafePath(workspace.manifest);
    const auto owner = ReadJson(workspace.root / OwnerFile);
    if (Text(owner, "kind") != "SnowDesktop.real-runtime-workspace" || Text(owner, "id") != workspace.id)
        throw std::runtime_error("demo_workspace_owner_mismatch");
    const auto manifest = ReadJson(workspace.manifest);
    if (Text(manifest, "id") != workspace.id || Text(manifest, "root") != Utf8(workspace.root.wstring()) ||
        Text(manifest, "desktop") != Utf8(workspace.desktop.wstring()) ||
        Text(manifest, "activeData") != Utf8(workspace.activeData.wstring()) ||
        Text(manifest, "runtime") != Utf8(workspace.runtime.wstring()) ||
        Text(manifest, "locale") != workspace.locale ||
        Text(manifest, "kind") != "real-files-full-runtime") throw std::runtime_error("demo_workspace_manifest_mismatch");
    const auto* fixtures = manifest.Find("fixtures");
    if (!fixtures || !fixtures->IsArray() || fixtures->array.size() < 8)
        throw std::runtime_error("demo_workspace_fixture_list_invalid");
    std::unordered_set<std::string> seen;
    for (const auto& entry : fixtures->array)
    {
        const auto name = Text(entry, "path");
        const auto relative = Relative(name);
        if (!seen.insert(name).second) throw std::runtime_error("demo_workspace_duplicate_fixture");
        if (*relative.begin() != L"Desktop" && *relative.begin() != L"Assets")
            throw std::runtime_error("demo_workspace_fixture_outside_desktop");
        const auto path = workspace.root / relative;
        SafePath(path);
        const auto type = Text(entry, "type");
        if (type != "file" && type != "directory") throw std::runtime_error("demo_workspace_fixture_type_invalid");
        const bool directory = type == "directory";
        if (directory != std::filesystem::is_directory(path) ||
            (!directory && !std::filesystem::is_regular_file(path)) ||
            WriteTime(path) != FixtureWriteTime || Text(entry, "writeTime100ns") != std::to_string(FixtureWriteTime))
            throw std::runtime_error("demo_workspace_fixture_identity_changed");
        if (!directory)
        {
            const auto* bytes = entry.Find("bytes");
            if (!bytes || !bytes->IsNumber() || bytes->number != static_cast<double>(std::filesystem::file_size(path)) ||
                Text(entry, "sha256") != Sha256(path)) throw std::runtime_error("demo_workspace_fixture_content_changed");
        }
    }
    return manifest;
}
}
