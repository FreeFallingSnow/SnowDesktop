#include "runtime_workspace.h"
#include "artifact_io.h"
#include "ui/preview/preview_png_writer.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <charconv>
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
constexpr DWORD CopiedAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
    FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;

struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    void Close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
    ~Handle() { Close(); }
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

void Timestamp(const std::filesystem::path& path, std::uint64_t stamp = FixtureWriteTime)
{
    SafePath(path);
    Handle file;
    file.value = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILETIME time{static_cast<DWORD>(stamp), static_cast<DWORD>(stamp >> 32)};
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

std::uint64_t Unsigned(std::string_view text)
{
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("demo_workspace_invalid_timestamp");
    return value;
}

std::filesystem::path ExistingDirectory(const std::filesystem::path& path)
{
    SafePath(path);
    Handle directory;
    directory.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (directory.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory.value, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("demo_workspace_existing_directory_required");
    const DWORD length = GetFinalPathNameByHandleW(directory.value, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!length) throw std::runtime_error("demo_workspace_directory_identity_unavailable");
    std::wstring name(static_cast<std::size_t>(length) + 1, L'\0');
    const DWORD copied = GetFinalPathNameByHandleW(directory.value, name.data(),
        static_cast<DWORD>(name.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!copied || copied >= name.size()) throw std::runtime_error("demo_workspace_directory_identity_unavailable");
    name.resize(copied);
    if (name.starts_with(L"\\\\?\\UNC\\")) name = L"\\\\" + name.substr(8);
    else if (name.starts_with(L"\\\\?\\")) name.erase(0, 4);
    std::filesystem::path result(name);
    SafePath(result);
    return result;
}

bool Contains(const std::filesystem::path& root, const std::filesystem::path& path)
{
    auto part = path.begin();
    for (const auto& component : root)
    {
        if (part == path.end() || _wcsicmp(component.c_str(), part->c_str()) != 0) return false;
        ++part;
    }
    return true;
}

Value FileRecord(const std::filesystem::path& path, const std::filesystem::path& base)
{
    SafePath(path);
    const bool directory = std::filesystem::is_directory(path);
    if (!directory && !std::filesystem::is_regular_file(path))
        throw std::runtime_error("demo_workspace_unsupported_source_entry");
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("demo_workspace_source_entry_unavailable");
    auto entry = Object({{"path", String(Utf8(path.lexically_relative(base).generic_wstring()))},
        {"type", String(directory ? "directory" : "file")}, {"writeTime100ns", String(std::to_string(WriteTime(path)))},
        {"fileAttributes", Number(attributes & CopiedAttributes)}});
    (void)Relative(Text(entry, "path"));
    if (!directory)
    {
        entry.object.emplace("bytes", Number(static_cast<double>(std::filesystem::file_size(path))));
        entry.object.emplace("sha256", String(Sha256(path)));
    }
    return entry;
}

Value Inventory(const std::filesystem::path& directory)
{
    SafePath(directory);
    std::vector<std::filesystem::path> paths;
    // Test every entry before advancing the recursive iterator, so a junction
    // or symlink is rejected before recursion could enter its target or cycle.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        SafePath(entry.path());
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    auto entries = Array();
    for (const auto& path : paths) entries.array.push_back(FileRecord(path, directory));
    return entries;
}

RuntimeWorkspace CreateWorkspace(const std::filesystem::path& parent, std::string_view locale,
    const std::filesystem::path& source = {})
{
    if (locale != "zh-CN" && locale != "en-US") throw std::runtime_error("demo_workspace_locale_unsupported");
    const auto canonicalParent = ExistingDirectory(parent);
    if (canonicalParent.native().size() > 120) throw std::runtime_error("demo_workspace_parent_too_long");
    RuntimeWorkspace workspace;
    workspace.id = Guid(); workspace.locale = std::string(locale);
    workspace.root = canonicalParent / Wide(workspace.id);
    workspace.desktop = workspace.root / L"Desktop";
    workspace.assets = workspace.root / L"Assets";
    workspace.runtime = workspace.root / L"runtime";
    workspace.normalData = workspace.runtime / L"data";
    workspace.activeData = workspace.runtime / L"data.debug" / L"data";
    workspace.debugControl = workspace.normalData / L"SnowDesktop.debug-profile.json";
    workspace.manifest = workspace.root / L"workspace.json";
    workspace.sourceDesktop = source;
    NewDirectory(workspace.root);
    // Failed preparations retain their exact owned partial directory as
    // evidence. Neither preparation path resets or deletes any source folder.
    NewFile(workspace.root / OwnerFile, Serialize(Object({{"schemaVersion", Number(1)},
        {"kind", String("SnowDesktop.real-runtime-workspace")}, {"id", String(workspace.id)}})));
    for (const auto& path : {workspace.desktop, workspace.assets, workspace.runtime, workspace.normalData,
            workspace.runtime / L"data.debug", workspace.activeData}) NewDirectory(path);
    return workspace;
}

void Finalize(const RuntimeWorkspace& workspace, Value fixtures, const Value& sourceAudit = {})
{
    const auto general = Object({{"softwareDesktopEnabled", Boolean(true)}, {"demoModeEnabled", Boolean(false)},
        {"language", String(workspace.locale)}, {"desktopIdleHideEnabled", Boolean(false)},
        {"doubleClickHideDesktop", Boolean(false)}, {"dockEnabled", Boolean(false)},
        {"forceRestartHotkeyEnabled", Boolean(false)}, {"animationMode", Number(1)},
        {"animationOnBattery", Boolean(false)}, {"animationEnergySaver", Boolean(false)}});
    NewFile(workspace.activeData / L"SnowDesktop.general.json", Serialize(general));
    NewFile(workspace.debugControl, Serialize(Object({{"enabled", Boolean(true)}, {"pendingReset", Boolean(false)},
        {"pendingDesktopChange", Boolean(false)}, {"desktop", PathValue(workspace.desktop)}})));
    auto manifest = Object({{"schemaVersion", Number(1)},
        {"kind", String("real-files-full-runtime")}, {"id", String(workspace.id)}, {"locale", String(workspace.locale)},
        {"root", PathValue(workspace.root)}, {"desktop", PathValue(workspace.desktop)},
        {"runtime", PathValue(workspace.runtime)}, {"normalData", PathValue(workspace.normalData)},
        {"activeData", PathValue(workspace.activeData)}, {"debugControl", PathValue(workspace.debugControl)},
        {"sourceDesktop", PathValue(workspace.sourceDesktop)},
        {"dataMode", String(workspace.sourceDesktop.empty() ? "generated-fixtures" : "copied-existing-desktop")},
        {"fixtures", std::move(fixtures)}, {"runtimeFilesCopied", Boolean(false)}});
    if (!workspace.sourceDesktop.empty())
    {
        const auto auditPath = workspace.root / L"source-desktop.json";
        NewFile(auditPath, Serialize(sourceAudit));
        manifest.object.emplace("sourceAudit", Object({{"path", String("source-desktop.json")},
            {"sha256", String(Sha256(auditPath))}}));
    }
    NewFile(workspace.manifest, Serialize(manifest));
    (void)ValidateRuntimeWorkspace(workspace);
}

void CopyFileBytes(const std::filesystem::path& source, const std::filesystem::path& destination,
    const Value& expected)
{
    SafePath(source); SafePath(destination);
    Handle input, output;
    // .lnk/.url are ordinary files. Opening the data stream never resolves or
    // launches their targets. OPEN_REPARSE_POINT additionally prevents following
    // a symbolic link substituted for the final source file after preflight.
    input.value = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (input.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(input.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw std::runtime_error("demo_workspace_source_file_unavailable");
    const auto* expectedBytes = expected.Find("bytes");
    const std::uint64_t size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    const std::uint64_t stamp = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
        info.ftLastWriteTime.dwLowDateTime;
    if (!expectedBytes || !expectedBytes->IsNumber() || expectedBytes->number != static_cast<double>(size) ||
        stamp != Unsigned(Text(expected, "writeTime100ns")))
        throw std::runtime_error("demo_workspace_source_changed");
    output.value = CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output.value == INVALID_HANDLE_VALUE) throw std::runtime_error("demo_workspace_copy_target_not_new");
    std::array<char, 65536> buffer{};
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            throw std::runtime_error("demo_workspace_source_read_failed");
        if (!read) break;
        DWORD position = 0;
        while (position < read)
        {
            DWORD written = 0;
            if (!WriteFile(output.value, buffer.data() + position, read - position, &written, nullptr) || !written)
                throw std::runtime_error("demo_workspace_copy_write_failed");
            position += written;
        }
    }
    if (!FlushFileBuffers(output.value)) throw std::runtime_error("demo_workspace_copy_flush_failed");
    output.Close();
    Timestamp(destination, stamp);
    if (Sha256(destination) != Text(expected, "sha256")) throw std::runtime_error("demo_workspace_copy_hash_mismatch");
    const DWORD copied = info.dwFileAttributes & CopiedAttributes;
    if (!SetFileAttributesW(destination.c_str(), copied ? copied : FILE_ATTRIBUTE_NORMAL))
        throw std::runtime_error("demo_workspace_copy_attributes_failed");
}
}

RuntimeWorkspace PrepareRuntimeWorkspace(const std::filesystem::path& parent, std::string_view locale)
{
    auto workspace = CreateWorkspace(parent, locale);

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
        fixtures.array.push_back(FileRecord(path, workspace.root));
    }
    Finalize(workspace, std::move(fixtures));
    return workspace;
}

RuntimeWorkspace PrepareRuntimeWorkspaceFromDesktop(const std::filesystem::path& parent,
    const std::filesystem::path& sourceDesktop, std::string_view locale)
{
    const auto canonicalParent = ExistingDirectory(parent);
    const auto source = ExistingDirectory(sourceDesktop);
    if (Contains(canonicalParent, source) || Contains(source, canonicalParent))
        throw std::runtime_error("demo_workspace_source_parent_overlap");
    const auto rootTime = WriteTime(source);
    const auto before = Inventory(source);
    auto workspace = CreateWorkspace(canonicalParent, locale, source);
    auto fixtures = Array();
    for (const auto& entry : before.array)
    {
        const auto relative = Relative(Text(entry, "path"));
        const auto destination = workspace.desktop / relative;
        if (Text(entry, "type") == "directory") NewDirectory(destination);
        else CopyFileBytes(source / relative, destination, entry);
        auto copied = entry;
        copied.object["path"] = String("Desktop/" + Text(entry, "path"));
        fixtures.array.push_back(std::move(copied));
    }
    for (auto it = before.array.rbegin(); it != before.array.rend(); ++it)
        if (Text(*it, "type") == "directory")
        {
            const auto destination = workspace.desktop / Relative(Text(*it, "path"));
            Timestamp(destination, Unsigned(Text(*it, "writeTime100ns")));
            const DWORD copied = static_cast<DWORD>(it->Find("fileAttributes")->number);
            if (!SetFileAttributesW(destination.c_str(), copied ? copied : FILE_ATTRIBUTE_NORMAL))
                throw std::runtime_error("demo_workspace_copy_attributes_failed");
        }
    Timestamp(workspace.desktop, rootTime);
    const auto after = Inventory(source);
    if (WriteTime(source) != rootTime || Serialize(before) != Serialize(after))
        throw std::runtime_error("demo_workspace_source_changed");
    const auto sourceAudit = Object({{"schemaVersion", Number(1)}, {"sourceDesktop", PathValue(source)},
        {"rootWriteTime100ns", String(std::to_string(rootTime))}, {"before", before}, {"after", after},
        {"beforeAfterMatched", Boolean(true)}, {"shortcutsCopiedAsBytes", Boolean(true)},
        {"observation", String("Source bytes, sizes, last-write times and relative inventory matched before and after copying; preparation never opens a source for writing. This does not exclude transient changes by other processes.")}});
    Finalize(workspace, std::move(fixtures), sourceAudit);
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
    const auto* sourceField = manifest.Find("sourceDesktop");
    const bool copiedDesktop = !workspace.sourceDesktop.empty();
    if (Text(manifest, "id") != workspace.id || Text(manifest, "root") != Utf8(workspace.root.wstring()) ||
        Text(manifest, "desktop") != Utf8(workspace.desktop.wstring()) ||
        Text(manifest, "activeData") != Utf8(workspace.activeData.wstring()) ||
        Text(manifest, "runtime") != Utf8(workspace.runtime.wstring()) ||
        Text(manifest, "locale") != workspace.locale ||
        !sourceField || !sourceField->IsString() || sourceField->string != Utf8(workspace.sourceDesktop.wstring()) ||
        Text(manifest, "dataMode") != (copiedDesktop ? "copied-existing-desktop" : "generated-fixtures") ||
        Text(manifest, "kind") != "real-files-full-runtime") throw std::runtime_error("demo_workspace_manifest_mismatch");
    const auto* fixtures = manifest.Find("fixtures");
    if (!fixtures || !fixtures->IsArray() || (!copiedDesktop && fixtures->array.size() != 11))
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
            WriteTime(path) != Unsigned(Text(entry, "writeTime100ns")))
            throw std::runtime_error("demo_workspace_fixture_identity_changed");
        const auto* attributes = entry.Find("fileAttributes");
        if (!attributes || !attributes->IsNumber() ||
            attributes->number != static_cast<double>(GetFileAttributesW(path.c_str()) & CopiedAttributes))
            throw std::runtime_error("demo_workspace_fixture_attributes_changed");
        if (!directory)
        {
            const auto* bytes = entry.Find("bytes");
            if (!bytes || !bytes->IsNumber() || bytes->number != static_cast<double>(std::filesystem::file_size(path)) ||
                Text(entry, "sha256") != Sha256(path)) throw std::runtime_error("demo_workspace_fixture_content_changed");
        }
    }
    if (copiedDesktop)
    {
        if (!workspace.sourceDesktop.is_absolute()) throw std::runtime_error("demo_workspace_source_identity_invalid");
        const auto* reference = manifest.Find("sourceAudit");
        if (!reference || !reference->IsObject() || Text(*reference, "path") != "source-desktop.json")
            throw std::runtime_error("demo_workspace_source_audit_missing");
        const auto auditPath = workspace.root / L"source-desktop.json";
        SafePath(auditPath);
        if (Sha256(auditPath) != Text(*reference, "sha256"))
            throw std::runtime_error("demo_workspace_source_audit_changed");
        const auto audit = ReadJson(auditPath);
        const auto* before = audit.Find("before");
        const auto* after = audit.Find("after");
        const auto* matched = audit.Find("beforeAfterMatched");
        if (Text(audit, "sourceDesktop") != Utf8(workspace.sourceDesktop.wstring()) ||
            !before || !before->IsArray() || !after || !after->IsArray() ||
            !matched || !matched->IsBoolean() || !matched->boolean || Serialize(*before) != Serialize(*after))
            throw std::runtime_error("demo_workspace_source_audit_invalid");
        // Compare the actual copied tree, including unexpected entries, to the
        // frozen source audit. Validation does not revisit or require the live
        // source directory after preparation; it is a resettable frozen copy.
        const auto actual = Inventory(workspace.desktop);
        if (Serialize(actual) != Serialize(*before))
            throw std::runtime_error("demo_workspace_copied_tree_changed");
        auto expected = *before;
        for (auto& entry : expected.array)
            entry.object["path"] = String("Desktop/" + Text(entry, "path"));
        if (Serialize(expected) != Serialize(*fixtures))
            throw std::runtime_error("demo_workspace_source_fixture_mismatch");
        SafePath(workspace.assets);
        if (std::filesystem::directory_iterator(workspace.assets) != std::filesystem::directory_iterator{})
            throw std::runtime_error("demo_workspace_unexpected_generated_assets");
    }
    return manifest;
}
}
