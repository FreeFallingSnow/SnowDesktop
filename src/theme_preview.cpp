#include "theme_preview.h"
#include "preview_png_writer.h"
#include "json_value.h"
#include "steam_child_environment.h"
#include <fstream>
#include <thread>

namespace snowdesktop::themes::preview
{
namespace
{
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
bool Fail(std::string& error, const char* value) { error = value; return false; }
}
std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring out = L"\""; unsigned slashes = 0;
    for (wchar_t c : value)
    {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(2 * slashes, L'\\'); return out + L'"';
}
bool Run(const std::filesystem::path& executable, const std::vector<std::wstring>& args,
    std::string& output, unsigned timeoutMs, std::string& error, const std::atomic_bool* cancel)
{
    output.clear();
    if (cancel && cancel->load()) return Fail(error, "cancelled");
    Handle read, write; SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &attributes, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) return Fail(error, "processFailed");
    std::wstring command = QuoteArgument(executable.wstring());
    for (const auto& arg : args) command += L" " + QuoteArgument(arg);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
    Handle input; input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    startup.StartupInfo.hStdInput = input.value;
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> storage(bytes); startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return Fail(error, "processFailed");
    struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } cleanup{startup.lpAttributeList};
    HANDLE inherited[]{write.value, input.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) return Fail(error, "processFailed");
    PROCESS_INFORMATION info{};
    auto environment = BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty()) return Fail(error, "processFailed");
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, environment.data(), executable.parent_path().c_str(), &startup.StartupInfo, &info))
        return Fail(error, "processFailed");
    Handle process{info.hProcess}, thread{info.hThread}; CloseHandle(write.value); write.value = nullptr;
    const auto started = GetTickCount64(); bool done = false;
    for (;;)
    {
        if (cancel && cancel->load()) { error = "cancelled"; break; }
        if (GetTickCount64() - started > timeoutMs) { error = "processTimeout"; break; }
        DWORD available = 0;
        if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buffer[8192]; DWORD received = 0;
            if (ReadFile(read.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &received, nullptr)) output.append(buffer, received);
            if (output.size() > 16 * 1024 * 1024) { error = "processOutputTooLarge"; break; }
            continue;
        }
        if (done) { DWORD code = 1; GetExitCodeProcess(process.value, &code); return code == 0 || Fail(error, "processFailed"); }
        done = WaitForSingleObject(process.value, 10) == WAIT_OBJECT_0;
        if (!done && cancel && cancel->load()) { error = "cancelled"; break; }
        if (!done && GetTickCount64() - started > timeoutMs) { error = "processTimeout"; break; }
    }
    // Only this request's one-shot child is terminated; no application tree.
    TerminateProcess(process.value, 6); WaitForSingleObject(process.value, 5000); return false;
}
std::vector<Part> Parts(const Package& package, std::string_view root, unsigned scope, std::string& error)
{
    if (!Validate(package, error)) return {};
    const auto theme = Resolve(package, root);
    if (!theme) { error = "themeNotFound"; return {}; }
    if (theme->kind != Kind::Global) return {{theme->kind == Kind::QuickPanel ? "quick-navigation" : "popup", std::string(root)}};
    if (!scope || (scope & ~All) || (theme->scopes & scope) != scope) { error = "invalidSelection"; return {}; }
    std::vector<Part> out;
    for (const auto& [bit, component] : std::vector<std::pair<unsigned, std::string>>{
        {Components, "collection"}, {Dock, "dock"}, {StatusBar, "status-bar"}, {Taskbar, "taskbar"}})
        if (scope & bit) out.push_back({component, std::string(root)});
    if (scope == theme->scopes)
    {
        out.push_back({"quick-navigation", theme->quickPanel}); out.push_back({"popup", theme->popup});
    }
    return out;
}
bool SaveCover(const std::filesystem::path& path, widget_preview::Wallpaper image, std::string& error)
{
    if (image.width != kCoverSize || image.height != kCoverSize || image.pixels.size() != kCoverSize * kCoverSize)
        return Fail(error, "previewFailed");
    for (unsigned bits : {8u, 6u, 5u, 4u, 3u, 2u})
    {
        auto pixels = image.pixels;
        if (bits < 8)
            for (auto& p : pixels) { const auto mask = (0xffu << (8 - bits)) & 0xffu; p = 0xff000000u | (p & (mask | mask << 8 | mask << 16)); }
        if (!preview_png::Save(path, kCoverSize, kCoverSize, pixels, error)) return false;
        std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
        if (!ec && size > 0 && size < kCoverMaximumBytes) return true;
    }
    std::error_code ec; std::filesystem::remove(path, ec); return Fail(error, "previewTooLarge");
}
bool Render(const std::filesystem::path& host, const Package& package, std::string_view root,
    unsigned scope, const std::filesystem::path& directory, std::filesystem::path& cover,
    std::string& error, const std::atomic_bool* cancel)
{
    cover.clear(); const auto parts = Parts(package, root, scope, error); if (parts.empty()) return false;
    std::error_code ec;
    // A new directory is mandatory. A failed request can never reuse an older cover.
    if (!std::filesystem::create_directory(directory, ec) || ec) return Fail(error, "writeFailed");
    struct Cleanup { std::filesystem::path path; bool keep = false; ~Cleanup() { if (!keep) { std::error_code ec; std::filesystem::remove_all(path, ec); } } } cleanup{directory};
    const auto snapshot = directory / L"package.snowtheme";
    if (!WritePackage(snapshot, package, error)) return false;
    auto canvas = widget_preview::GenerateWallpaper(kCoverSize, kCoverSize, false);
    const int columns = parts.size() > 1 ? 2 : 1, rows = static_cast<int>((parts.size() + columns - 1) / columns);
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        if (cancel && cancel->load()) return Fail(error, "cancelled");
        const auto partDir = directory / std::to_wstring(index); const auto resultPath = directory / (std::to_wstring(index) + L".json");
        std::string output;
        const auto& part = parts[index];
        if (!Run(host, {L"--native-component-preview", std::wstring(part.component.begin(), part.component.end()), partDir.wstring(),
            L"96", L"en-US", L"dark", L"", L"2048", L"1536", L"32", L"0", L"0", resultPath.wstring(), snapshot.wstring(),
            std::wstring(part.themeId.begin(), part.themeId.end())}, output, 60000, error, cancel)) return false;
        std::ifstream file(resultPath, std::ios::binary); std::string text(std::istreambuf_iterator<char>(file), {}); JsonValue result;
        if (!ParseJson(text, result)) return Fail(error, "previewFailed");
        const auto* ok = result.Find("ok"), *outputs = result.Find("outputs");
        if (!ok || !ok->IsBoolean() || !ok->boolean || !outputs || !outputs->IsArray() || outputs->array.empty()) return Fail(error, "previewFailed");
        const auto& item = outputs->array.front(); const auto* path = item.Find("path");
        if (!path || !path->IsString()) return Fail(error, "previewFailed");
        const auto image = widget_preview::LoadWallpaperImage(std::filesystem::path(std::u8string(path->string.begin(), path->string.end())));
        const auto number = [&](const char* key) { const auto* value = item.Find(key); return value && value->IsNumber() ? static_cast<int>(value->number) : 0; };
        int left = number("placementX"), top = number("placementY"), width = number("componentWidth"), height = number("componentHeight");
        const int margin = 12;
        left = std::max(0, left - margin); top = std::max(0, top - margin);
        width = std::min(image.width - left, width + 2 * margin); height = std::min(image.height - top, height + 2 * margin);
        if (image.pixels.empty() || width <= 0 || height <= 0) return Fail(error, "previewFailed");
        const int cellWidth = kCoverSize / columns - 40, cellHeight = kCoverSize / rows - 40;
        const double scale = std::min(static_cast<double>(cellWidth) / width, static_cast<double>(cellHeight) / height);
        const int w = std::max(1, static_cast<int>(width * scale)), h = std::max(1, static_cast<int>(height * scale));
        const int x = static_cast<int>(index % columns) * kCoverSize / columns + (kCoverSize / columns - w) / 2;
        const int y = static_cast<int>(index / columns) * kCoverSize / rows + (kCoverSize / rows - h) / 2;
        for (int dy = 0; dy < h; ++dy) for (int dx = 0; dx < w; ++dx)
            canvas.pixels[static_cast<std::size_t>(y + dy) * kCoverSize + x + dx] =
                image.pixels[static_cast<std::size_t>(top + static_cast<int>(dy / scale)) * image.width + left + static_cast<int>(dx / scale)];
        std::filesystem::remove_all(partDir, ec); std::filesystem::remove(resultPath, ec);
    }
    const auto destination = directory / L"cover.png";
    if (!SaveCover(destination, std::move(canvas), error)) return false;
    cover = destination; cleanup.keep = true; return true;
}
}
