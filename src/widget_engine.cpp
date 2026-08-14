/**
 * @file widget_engine.cpp
 * @brief WidgetEngine 类的实现，管理 Lua 小部件的完整生命周期
 *
 * 该文件实现了 WidgetEngine 类及其相关辅助功能，负责：
 * - 将 Lua 脚本加载到 Lua 5.x 沙盒环境中安全执行
 * - 注册绘制 API（draw.*）、系统 API（sys.*）、桌面 API（desktop.*）等 Lua 可调用的 C 函数
 * - 通过 D2D 渲染小部件内容并处理交互事件（点击、菜单、桌面变更等）
 * - 管理小部件的持久化键值存储（localStorage）、文件变更热重载与错误记录
 * - 集成 ImGui 为 Lua 脚本提供 imgui.* 界面控件 API
 * - 读取并解析 .widget.json 清单文件以获取权限、尺寸等元信息
 */

#include "widget_engine.h"
#include "widget_scroll_rules.h"
#include "data_paths.h"
#include "deployment_context.h"
#include "json_value.h"
#include "l10n.h"
#include "system_snapshot.h"
#include "constants.h"
#include "utils.h"
#include "font_cu_rules.h"
#include "search_match.h"
#include "personalization.h"
#include "widget_package.h"
#include "steam_workshop_source.h"
#include "widget_api_registry.h"
#include "widget_preview_context.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")

/**
 * @brief 以二进制模式读取文本文件全部内容
 * @param path 文件路径（宽字符）
 * @return 文件内容的 UTF-8 字符串，读取失败时返回空串
 */
static std::string ReadTextFile(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

/**
 * @brief 将宽字符串（UTF-16）转换为 UTF-8 编码
 * @param w 输入的宽字符串
 * @return UTF-8 编码的 std::string，输入为空时返回空串
 */
static std::string WidgetWideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}

/**
 * @brief 将 UTF-8 字符串转换为本地宽字符串（UTF-16）
 * @param s 输入的 UTF-8 字符串
 * @return UTF-16 编码的 std::wstring，输入为空时返回空串
 */
static std::wstring Utf8ToWideLocal(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), r.data(), n);
    return r;
}

/**
 * @brief 简易 JSON 字符串字段解析器（不依赖第三方库）
 * @param text  待解析的 JSON 文本
 * @param field 目标字段名
 * @param out   输出参数，解析成功后写入字段值
 * @retval true  成功找到并解析出字段值
 * @retval false 字段不存在或解析失败
 * @note 仅支持简单的 \"key\": \"value\" 格式，不处理嵌套对象。
 *       支持 \\n、\\r、\\t、\\"、\\\\ 等转义序列。
 */
static bool JsonReadString(const std::string& text, const char* field, std::string& out)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p + marker.size());
    if (p == std::string::npos) return false;
    p = text.find('"', p + 1);
    if (p == std::string::npos) return false;
    std::string result;
    for (++p; p < text.size(); ++p)
    {
        char ch = text[p];
        if (ch == '"') { out = result; return true; }
        if (ch == '\\' && p + 1 < text.size())
        {
            char esc = text[++p];
            switch (esc)
            {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            default: result.push_back(esc); break;
            }
        }
        else
        {
            result.push_back(ch);
        }
    }
    return false;
}

static bool JsonReadInt(const std::string& text, const char* field, int& out)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p + marker.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    size_t start = p;
    if (p < text.size() && (text[p] == '-' || text[p] == '+')) ++p;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) ++p;
    if (p == start) return false;
    out = std::atoi(text.substr(start, p - start).c_str());
    return true;
}

static std::vector<std::string> JsonReadStringArray(const std::string& text, const char* field)
{
    std::vector<std::string> result;
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return result;
    p = text.find('[', p + marker.size());
    if (p == std::string::npos) return result;
    size_t end = text.find(']', p + 1);
    if (end == std::string::npos) return result;
    for (++p; p < end; ++p)
    {
        if (text[p] != '"') continue;
        std::string value;
        for (++p; p < end; ++p)
        {
            if (text[p] == '"') break;
            if (text[p] == '\\' && p + 1 < end)
                value.push_back(text[++p]);
            else
                value.push_back(text[p]);
        }
        if (!value.empty()) result.push_back(value);
    }
    return result;
}

static snowdesktop::widget::WidgetPackageManager& GetWidgetPackageManager()
{
    static snowdesktop::widget::WidgetPackageManager manager;
    static const bool initialized = [] {
        std::string error;
        return manager.Initialize(error);
    }();
    (void)initialized;
    return manager;
}

static std::unordered_map<std::string,
    std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>>&
GetWidgetPackageSources()
{
    static std::unordered_map<std::string,
        std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>> sources;
    static const bool initialized = [&]
    {
        auto& manager = GetWidgetPackageManager();
        auto builtin =
            std::make_shared<snowdesktop::widget::BuiltinPackageSource>(
                manager);
        sources[builtin->ProviderId()] = builtin;
        auto local =
            std::make_shared<snowdesktop::widget::LocalDirectorySource>(
                manager.Paths().development);
        sources[local->ProviderId()] = local;
        auto steam =
            std::make_shared<snowdesktop::widget::SteamWorkshopSource>();
        sources[steam->ProviderId()] = steam;
        return true;
    }();
    (void)initialized;
    return sources;
}

struct SteamWorkshopPackageAssociationCache
{
    std::mutex mutex;
    std::unordered_map<std::string, std::string> associations;
};

static SteamWorkshopPackageAssociationCache&
GetSteamWorkshopPackageAssociationCache()
{
    static SteamWorkshopPackageAssociationCache cache;
    return cache;
}

static std::wstring ResolveWidgetPath(const std::wstring& packageId)
{
    const auto entry = GetWidgetPackageManager().ResolveEntry(
        WidgetWideToUtf8(packageId));
    return entry ? entry->wstring() : std::wstring{};
}

static bool RecoverWidgetPackage(const std::string& packageId,
    const std::optional<std::string>& preferredVersion = std::nullopt)
{
    auto& manager = GetWidgetPackageManager();
    const auto packages = manager.ListPackages();
    auto tryVersion = [&](const std::string& version) {
        std::string error;
        return manager.Rollback(packageId, version, error);
    };
    if (preferredVersion && tryVersion(*preferredVersion))
        return true;
    std::vector<std::string> candidates;
    bool activeUserPackage = false;
    for (const auto& package : packages)
    {
        if (package.manifest.id != packageId ||
            package.builtin || package.development)
            continue;
        if (package.active) activeUserPackage = true;
        else candidates.push_back(package.manifest.version);
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const std::string& left, const std::string& right)
        {
            return snowdesktop::widget::WidgetPackageValidator::
                IsNewerSemVer(left, right);
        });
    for (const auto& version : candidates)
        if (tryVersion(version)) return true;
    if (activeUserPackage)
    {
        std::string error;
        if (manager.Uninstall(packageId, error))
            return manager.Resolve(packageId).has_value();
    }
    return false;
}

static std::wstring ManifestPathForScriptFile(const std::wstring& fullScriptPath)
{
    if (const auto package =
        GetWidgetPackageManager().ResolveEntryPath(fullScriptPath))
        return (package->root / L"widget.json").wstring();
    const std::filesystem::path packageManifest =
        std::filesystem::path(fullScriptPath).parent_path() / L"widget.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(packageManifest, error))
        return packageManifest.wstring();
    std::wstring path = fullScriptPath;
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (ext && _wcsicmp(ext, L".lua") == 0)
        path.resize(static_cast<size_t>(ext - path.c_str()));
    path += L".widget.json";
    return path;
}

// ── Storage (shared localStorage for Lua widgets) ────────────────
static std::unordered_map<std::string, std::string> g_storage;
static std::wstring g_storagePath;
static snowdesktop::widget_runtime::DiagnosticsLog g_widgetDiagnostics;
static bool StorageWriteWithinQuota(const std::string& prefix,
    const std::string& key, const std::string& value);

static bool IsRemovedPanelEffectSettingKey(const std::string& key)
{
    const size_t separator = key.rfind('.');
    const std::string setting = separator == std::string::npos
        ? key : key.substr(separator + 1);
    return setting == "shadowAlpha" || setting == "shadowBlur" ||
        setting == "shadowOffsetY" || setting == "highlightAlpha" ||
        setting == "noiseAlpha";
}

static std::unordered_map<std::string, std::string>& ActiveStorage()
{
    return snowdesktop::widget_runtime::ActiveStorage(g_storage);
}

using snowdesktop::widget_runtime::DryLoadScope;
using snowdesktop::widget_runtime::PreviewExecutionScope;
using snowdesktop::widget_runtime::StorageOverlayScope;

static std::string EscapeStorageJson(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0x0f]);
                result.push_back(hex[ch & 0x0f]);
            }
            else result.push_back(static_cast<char>(ch));
            break;
        }
    }
    return result;
}

static bool ParseLegacyStorageText(const std::string& text,
    std::unordered_map<std::string, std::string>& output)
{
    // Releases before package format v1 wrote string values without JSON
    // escaping. Recover the same key/value pairs that the former reader
    // accepted, then immediately rewrite them through the safe serializer.
    size_t pos = text.find('{');
    if (pos == std::string::npos) return false;
    while (true)
    {
        pos = text.find('"', pos);
        if (pos == std::string::npos) break;
        const size_t keyEnd = text.find('"', pos + 1);
        if (keyEnd == std::string::npos) break;
        const std::string key = text.substr(pos + 1, keyEnd - pos - 1);
        pos = text.find(':', keyEnd + 1);
        if (pos == std::string::npos) break;
        pos = text.find('"', pos + 1);
        if (pos == std::string::npos) break;
        const size_t valueEnd = text.find('"', pos + 1);
        if (valueEnd == std::string::npos) break;
        if (!key.empty() && !IsRemovedPanelEffectSettingKey(key))
            output[key] = text.substr(pos + 1, valueEnd - pos - 1);
        pos = valueEnd + 1;
    }
    return !output.empty();
}

static bool ParseStorageText(const std::string& text,
    std::unordered_map<std::string, std::string>& output)
{
    JsonValue root;
    std::string parseError;
    if (ParseJson(text, root, &parseError) && root.IsObject())
    {
        for (const auto& [key, value] : root.object)
            if (value.IsString() && !IsRemovedPanelEffectSettingKey(key))
                output.emplace(key, value.string);
        return true;
    }
    return ParseLegacyStorageText(text, output);
}

static bool ReadStorageMap(const std::filesystem::path& path,
    std::unordered_map<std::string, std::string>& output)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    return ParseStorageText(stream.str(), output);
}

static bool SaveStorageFile();

static void LoadStorageFile()
{
    g_storage.clear();
    if (g_storagePath.empty()) return;

    std::error_code ec;
    const bool storageExists =
        std::filesystem::is_regular_file(g_storagePath, ec);
    if (storageExists && !ReadStorageMap(g_storagePath, g_storage))
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t suffix[64]{};
        swprintf_s(suffix, L".corrupt-%04u%02u%02u-%02u%02u%02u.json",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond);
        const std::wstring quarantine = g_storagePath + suffix;
        MoveFileExW(g_storagePath.c_str(), quarantine.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }

    // Built-in loose-file replacement creates this short-lived snapshot before
    // touching package files. Merge only missing keys so current writes win,
    // commit atomically, then consume it. It never enters migrations/.
    const auto pending =
        GetWidgetPackageManager().PendingLegacyStoragePath();
    ec.clear();
    if (!std::filesystem::is_regular_file(pending, ec))
        return;
    std::unordered_map<std::string, std::string> pendingStorage;
    if (!ReadStorageMap(pending, pendingStorage))
        return;
    for (auto& [key, value] : pendingStorage)
        g_storage.try_emplace(std::move(key), std::move(value));
    if (SaveStorageFile())
    {
        ec.clear();
        std::filesystem::remove(pending, ec);
    }
}

static bool SaveStorageFile()
{
    if (g_storagePath.empty()) return false;
    std::vector<std::pair<std::string, std::string>> values(
        g_storage.begin(), g_storage.end());
    std::sort(values.begin(), values.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
    std::ostringstream output;
    output << "{";
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index) output << ',';
        output << "\n  \"" << EscapeStorageJson(values[index].first)
            << "\": \"" << EscapeStorageJson(values[index].second) << '"';
    }
    if (!values.empty()) output << '\n';
    output << "}\n";

    const std::wstring temporary = g_storagePath + L".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    const std::string text = output.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.flush();
    if (!file) return false;
    file.close();
    return MoveFileExW(temporary.c_str(), g_storagePath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

static bool EndsWithLastError(const std::string& key)
{
    constexpr const char* suffix = ".lastError";
    constexpr size_t suffixLen = 10;
    return key.size() >= suffixLen && key.compare(key.size() - suffixLen, suffixLen, suffix) == 0;
}

// ── Drawing API ──────────────────────────────────────────────────
class AsyncShellIconLoader
{
public:
    struct Result
    {
        std::wstring path;
        HBITMAP bitmap = nullptr;
    };

    using ReadyCallback = std::function<void(const std::wstring&)>;

    explicit AsyncShellIconLoader(ReadyCallback readyCallback)
        : readyCallback_(std::move(readyCallback)),
          worker_([this](std::stop_token stopToken) {
              Run(stopToken);
          })
    {
    }

    ~AsyncShellIconLoader()
    {
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
        for (auto& result : completed_)
            if (result.bitmap)
                DeleteObject(result.bitmap);
    }

    void Request(const std::wstring& path, const std::wstring& widgetId)
    {
        if (path.empty()) return;
        {
            std::scoped_lock lock(mutex_);
            if (pending_.contains(path) || requests_.size() >= 128)
                return;
            pending_.insert(path);
            requests_.push_back({ path, widgetId });
        }
        condition_.notify_one();
    }

    std::vector<Result> Drain()
    {
        std::vector<Result> results;
        std::scoped_lock lock(mutex_);
        results.reserve(completed_.size());
        while (!completed_.empty())
        {
            pending_.erase(completed_.front().path);
            results.push_back(std::move(completed_.front()));
            completed_.pop_front();
        }
        return results;
    }

private:
    struct RequestEntry
    {
        std::wstring path;
        std::wstring widgetId;
    };

    void Run(std::stop_token stopToken)
    {
        const HRESULT comResult = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED);
        while (!stopToken.stop_requested())
        {
            RequestEntry request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return stopToken.stop_requested() ||
                        !requests_.empty();
                });
                if (stopToken.stop_requested())
                    break;
                request = std::move(requests_.back());
                requests_.pop_back();
            }

            HBITMAP bitmap = nullptr;
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(
                request.path.c_str(), nullptr, &pidl, 0, nullptr)) &&
                pidl)
            {
                SIZE bitmapSize{};
                bitmap = GetHighResolutionShellIconBitmap(
                    pidl, 0, bitmapSize);
                CoTaskMemFree(pidl);
            }

            {
                std::scoped_lock lock(mutex_);
                completed_.push_back({ request.path, bitmap });
            }
            if (readyCallback_)
                readyCallback_(request.widgetId);
        }
        if (SUCCEEDED(comResult))
            CoUninitialize();
    }

    ReadyCallback readyCallback_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<RequestEntry> requests_;
    std::deque<Result> completed_;
    std::unordered_set<std::wstring> pending_;
    std::jthread worker_;
};

struct D2DState
{
    ID2D1DeviceContext* ctx = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_RECT_F widgetRect{};
    WidgetEngine* engine = nullptr;
    std::string storagePrefix;
    std::wstring currentWidgetId;
    int gridColumns = 1;
    int gridRows = 1;
    int gridCellW = 92;
    int gridCellH = 116;
    int gridGapY = 8;
    int barHeight = 24;
    DWRITE_FONT_WEIGHT itemFontWeight = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    int widgetClipDepth = 0;
    ComPtr<ID2D1Device> bitmapDevice;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> imageCache;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> shellIconCache;
    std::unordered_set<std::wstring> shellIconFailures;
    std::unique_ptr<AsyncShellIconLoader> shellIconLoader;
    ID2D1DeviceContext* brushContext = nullptr;
    std::unordered_map<std::uint32_t, ComPtr<ID2D1SolidColorBrush>> brushCache;
    std::unordered_map<std::uint64_t, ComPtr<IDWriteTextFormat>> textFormatCache;
};

static ID2D1SolidColorBrush* GetCachedBrush(D2DState* state, int color, float alpha = 1.0f)
{
    if (!state || !state->ctx) return nullptr;
    if (state->brushContext != state->ctx)
    {
        state->brushCache.clear();
        state->brushContext = state->ctx;
    }

    const auto alphaByte = static_cast<std::uint32_t>(std::clamp(
        static_cast<int>(std::lround(alpha * 255.0f)), 0, 255));
    const std::uint32_t key =
        (static_cast<std::uint32_t>(color) & 0x00FFFFFFu) | (alphaByte << 24);
    if (auto found = state->brushCache.find(key); found != state->brushCache.end())
        return found->second.Get();

    if (state->brushCache.size() >= 512)
        state->brushCache.clear();

    ComPtr<ID2D1SolidColorBrush> brush;
    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >> 8) & 0xFF) / 255.0f;
    const float b = (color & 0xFF) / 255.0f;
    if (FAILED(state->ctx->CreateSolidColorBrush(
        D2D1::ColorF(r, g, b, alphaByte / 255.0f), &brush)) || !brush)
        return nullptr;
    return state->brushCache.emplace(key, std::move(brush)).first->second.Get();
}

static IDWriteTextFormat* GetCachedTextFormat(D2DState* state, float size,
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
    bool centered = false,
    DWRITE_WORD_WRAPPING wrapping = DWRITE_WORD_WRAPPING_WRAP,
    bool fontAwesome = false,
    bool verticallyCentered = false,
    bool fluent = false)
{
    if (!state || !state->dwrite) return nullptr;
    const auto sizeKey = static_cast<std::uint64_t>(std::clamp(
        static_cast<int>(std::lround(size * 100.0f)), 1, 0xFFFFFF));
    const std::uint64_t key = sizeKey |
        (static_cast<std::uint64_t>(weight) << 24) |
        (static_cast<std::uint64_t>(centered) << 36) |
        (static_cast<std::uint64_t>(wrapping) << 37) |
        (static_cast<std::uint64_t>(fontAwesome) << 40) |
        (static_cast<std::uint64_t>(verticallyCentered) << 41) |
        (static_cast<std::uint64_t>(fluent) << 42);
    if (auto found = state->textFormatCache.find(key); found != state->textFormatCache.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    if (fluent)
    {
        format.Attach(CreateFluentTextFormat(state->dwrite, size));
    }
    else if (fontAwesome)
    {
        format.Attach(CreateFaTextFormat(state->dwrite, size));
    }
    else
    {
        state->dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
    }
    if (!format) return nullptr;
    format->SetTextAlignment(centered
        ? DWRITE_TEXT_ALIGNMENT_CENTER
        : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(centered || verticallyCentered
        ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
        : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    format->SetWordWrapping(wrapping);
    if (state->textFormatCache.size() >= 128)
        state->textFormatCache.clear();
    return state->textFormatCache.emplace(key, std::move(format)).first->second.Get();
}

static bool IsHostStructureSettingKey(const std::string& key);
static bool IsHostAppearanceSettingKey(const std::string& key);

static std::string LuaValueToStorageString(lua_State* L, int index)
{
    index = lua_absindex(L, index);
    if (lua_isboolean(L, index))
        return lua_toboolean(L, index) ? "1" : "0";
    if (lua_isinteger(L, index))
        return std::to_string(static_cast<long long>(lua_tointeger(L, index)));
    if (lua_isnumber(L, index))
    {
        std::ostringstream ss;
        ss << lua_tonumber(L, index);
        return ss.str();
    }
    if (lua_isstring(L, index))
        return lua_tostring(L, index);
    return {};
}

static std::string LuaReadStorageField(lua_State* L, int tableIndex, const char* key)
{
    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, key);
    std::string value = LuaValueToStorageString(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool LuaReadBoolField(lua_State* L, int tableIndex, const char* key, bool defaultValue = false)
{
    tableIndex = lua_absindex(L, tableIndex);
    lua_getfield(L, tableIndex, key);
    bool value = lua_isnil(L, -1) ? defaultValue : (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return value;
}

static LuaWidgetManifest::Setting ReadLuaSettingTable(lua_State* L, int tableIndex)
{
    tableIndex = lua_absindex(L, tableIndex);
    LuaWidgetManifest::Setting setting;
    setting.key = LuaReadStorageField(L, tableIndex, "key");
    setting.label = LuaReadStorageField(L, tableIndex, "label");
    setting.type = LuaReadStorageField(L, tableIndex, "type");
    setting.defaultValue = LuaReadStorageField(L, tableIndex, "default");

    lua_getfield(L, tableIndex, "min");
    if (lua_isnumber(L, -1)) setting.minValue = lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, tableIndex, "max");
    if (lua_isnumber(L, -1)) setting.maxValue = lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, tableIndex, "options");
    if (lua_istable(L, -1))
    {
        int optionsIndex = lua_absindex(L, -1);
        for (int i = 1;; ++i)
        {
            lua_rawgeti(L, optionsIndex, i);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            std::string option = LuaValueToStorageString(L, -1);
            if (!option.empty()) setting.options.push_back(option);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    if (setting.label.empty()) setting.label = setting.key;
    if (setting.type.empty()) setting.type = "text";
    return setting;
}

static void ReadLuaSettingsArray(lua_State* L, int arrayIndex,
    std::vector<LuaWidgetManifest::Setting>& out)
{
    arrayIndex = lua_absindex(L, arrayIndex);
    for (int i = 1;; ++i)
    {
        lua_rawgeti(L, arrayIndex, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (lua_istable(L, -1))
        {
            LuaWidgetManifest::Setting setting = ReadLuaSettingTable(L, -1);
            if (!setting.key.empty() &&
                !IsHostStructureSettingKey(setting.key) &&
                !IsHostAppearanceSettingKey(setting.key))
                out.push_back(std::move(setting));
        }
        lua_pop(L, 1);
    }
}

static std::unordered_map<std::string, std::string> ReadLuaValueMap(lua_State* L, int tableIndex)
{
    std::unordered_map<std::string, std::string> values;
    tableIndex = lua_absindex(L, tableIndex);
    lua_pushnil(L);
    while (lua_next(L, tableIndex) != 0)
    {
        if (lua_isstring(L, -2))
        {
            std::string key = lua_tostring(L, -2);
            if (!IsHostStructureSettingKey(key))
            {
                std::string value = LuaValueToStorageString(L, -1);
                if (!value.empty()) values[key] = value;
            }
        }
        lua_pop(L, 1);
    }
    return values;
}

static LuaWidgetManifest::SettingPreset ReadLuaPresetTable(lua_State* L, int tableIndex)
{
    tableIndex = lua_absindex(L, tableIndex);
    LuaWidgetManifest::SettingPreset preset;
    preset.id = LuaReadStorageField(L, tableIndex, "id");
    preset.label = LuaReadStorageField(L, tableIndex, "label");
    if (preset.label.empty())
        preset.label = LuaReadStorageField(L, tableIndex, "name");
    preset.isDefault = LuaReadBoolField(L, tableIndex, "default") ||
        LuaReadBoolField(L, tableIndex, "isDefault");

    lua_getfield(L, tableIndex, "values");
    if (lua_istable(L, -1))
        preset.values = ReadLuaValueMap(L, -1);
    lua_pop(L, 1);

    if (preset.id.empty()) preset.id = preset.label;
    if (preset.label.empty()) preset.label = preset.id;
    return preset;
}

static void ReadLuaPresetsArray(lua_State* L, int arrayIndex,
    std::vector<LuaWidgetManifest::SettingPreset>& out)
{
    arrayIndex = lua_absindex(L, arrayIndex);
    for (int i = 1;; ++i)
    {
        lua_rawgeti(L, arrayIndex, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (lua_istable(L, -1))
        {
            LuaWidgetManifest::SettingPreset preset = ReadLuaPresetTable(L, -1);
            if (!preset.id.empty() && !preset.label.empty() && !preset.values.empty())
                out.push_back(std::move(preset));
        }
        lua_pop(L, 1);
    }
}

static void ReadLuaDeclaredSettings(lua_State* L, int widgetTableIndex,
    std::vector<LuaWidgetManifest::Setting>& settings,
    std::vector<LuaWidgetManifest::SettingPreset>& presets)
{
    widgetTableIndex = lua_absindex(L, widgetTableIndex);
    lua_getfield(L, widgetTableIndex, "settings");
    if (lua_istable(L, -1))
    {
        int settingsTable = lua_absindex(L, -1);
        lua_getfield(L, settingsTable, "fields");
        if (lua_istable(L, -1))
        {
            ReadLuaSettingsArray(L, -1, settings);
            lua_pop(L, 1);
        }
        else
        {
            lua_pop(L, 1);
            lua_rawgeti(L, settingsTable, 1);
            bool directArray = lua_istable(L, -1);
            lua_pop(L, 1);
            if (directArray)
                ReadLuaSettingsArray(L, settingsTable, settings);
        }

        lua_getfield(L, settingsTable, "presets");
        if (lua_istable(L, -1))
            ReadLuaPresetsArray(L, -1, presets);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, widgetTableIndex, "presets");
    if (lua_istable(L, -1))
        ReadLuaPresetsArray(L, -1, presets);
    lua_pop(L, 1);
}

static D2DState* GetD2D(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__d2d_ptr");
    auto* s = static_cast<D2DState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
}

static std::wstring BoundWidgetId(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_id");
    const char* value = lua_tostring(state, -1);
    const std::wstring result = Utf8ToWideLocal(value ? value : "");
    lua_pop(state, 1);
    return result;
}

static std::string BoundStoragePrefix(lua_State* state)
{
    return WidgetWideToUtf8(BoundWidgetId(state));
}

static void SetWidgetExecutionContext(D2DState* state, const std::wstring& widgetId)
{
    if (!state) return;
    state->currentWidgetId = widgetId;
    state->storagePrefix = WidgetWideToUtf8(widgetId);
    if (state->engine)
        state->engine->ActivateWidgetState(widgetId);
}

class WidgetExecutionContextGuard
{
public:
    WidgetExecutionContextGuard(
        D2DState* state, const std::wstring& widgetId)
        : state_(state)
    {
        if (!state_)
            return;
        context_ = state_->ctx;
        widgetRect_ = state_->widgetRect;
        storagePrefix_ = state_->storagePrefix;
        widgetId_ = state_->currentWidgetId;
        gridColumns_ = state_->gridColumns;
        gridRows_ = state_->gridRows;
        layoutMetrics_ = snowdesktop::widget_runtime::
            CaptureLayoutMetrics(*state_);
        widgetClipDepth_ = state_->widgetClipDepth;
        SetWidgetExecutionContext(state_, widgetId);
    }

    ~WidgetExecutionContextGuard()
    {
        if (!state_)
            return;
        state_->ctx = context_;
        state_->widgetRect = widgetRect_;
        state_->storagePrefix = std::move(storagePrefix_);
        state_->currentWidgetId = std::move(widgetId_);
        state_->gridColumns = gridColumns_;
        state_->gridRows = gridRows_;
        state_->widgetClipDepth = widgetClipDepth_;
        snowdesktop::widget_runtime::RestoreLayoutMetrics(
            *state_, layoutMetrics_, [this]() {
                if (state_->engine)
                {
                    state_->engine->ActivateWidgetState(
                        state_->currentWidgetId);
                }
            });
    }

    WidgetExecutionContextGuard(
        const WidgetExecutionContextGuard&) = delete;
    WidgetExecutionContextGuard& operator=(
        const WidgetExecutionContextGuard&) = delete;

private:
    D2DState* state_ = nullptr;
    ID2D1DeviceContext* context_ = nullptr;
    D2D1_RECT_F widgetRect_{};
    std::string storagePrefix_;
    std::wstring widgetId_;
    int gridColumns_ = 1;
    int gridRows_ = 1;
    snowdesktop::widget_runtime::LayoutMetrics layoutMetrics_;
    int widgetClipDepth_ = 0;
};

static void SetWidgetRectContext(D2DState* state, RECT bounds)
{
    if (!state || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    state->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
}

static int lua_DrawText(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    const char* text = luaL_checkstring(L, 3);
    float size = static_cast<float>(luaL_optnumber(L, 4, 14));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float maxWidth = static_cast<float>(luaL_optnumber(L, 6, 0));
    bool bold = lua_toboolean(L, 7) != 0;
    bool singleLine = lua_toboolean(L, 8) != 0;
    float requestedHeight = static_cast<float>(luaL_optnumber(L, 9, 0));
    float alpha = static_cast<float>(luaL_optnumber(L, 10, 1.0));

    auto* s = GetD2D(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        bold ? DWRITE_FONT_WEIGHT_BOLD : s->itemFontWeight,
        false, maxWidth > 0 && singleLine
            ? DWRITE_WORD_WRAPPING_NO_WRAP
            : DWRITE_WORD_WRAPPING_WRAP);
    if (!format) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    float bx = x + s->widgetRect.left;
    float by = y + s->widgetRect.top;

    if (maxWidth > 0)
    {
        ComPtr<IDWriteTextLayout> layout;
        const float maxHeight = requestedHeight > 0
            ? requestedHeight
            : (singleLine ? std::max(size * 1.35f, size + 4.0f) : 5000.0f);
        s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format, maxWidth, maxHeight, &layout);
        if (layout && (singleLine || requestedHeight > 0))
        {
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(s->dwrite->CreateEllipsisTrimmingSign(format, &ellipsis)) && ellipsis)
            {
                DWRITE_TRIMMING trimming{};
                trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                layout->SetTrimming(&trimming, ellipsis.Get());
            }
        }
        if (layout)
            s->ctx->DrawTextLayout(D2D1::Point2F(bx, by), layout.Get(), brush);
    }
    else
    {
        D2D1_RECT_F rect = { bx, by, bx + 800, by + 200 };
        s->ctx->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format, &rect, brush);
    }
    return 0;
}

static int lua_MeasureText(lua_State* L)
{
    const char* text = luaL_checkstring(L, 1);
    float size = static_cast<float>(luaL_optnumber(L, 2, 14));
    float maxWidth = static_cast<float>(luaL_optnumber(L, 3, 0));
    bool bold = lua_toboolean(L, 4) != 0;

    auto pushSize = [&](float width, float height) {
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, width);
        lua_setfield(L, -2, "width");
        lua_pushnumber(L, height);
        lua_setfield(L, -2, "height");
        return 1;
    };

    auto* s = GetD2D(L);
    if (!s || !s->dwrite)
        return pushSize(0.0f, 0.0f);

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        bold ? DWRITE_FONT_WEIGHT_BOLD : s->itemFontWeight,
        false, DWRITE_WORD_WRAPPING_WRAP);
    if (!format)
        return pushSize(0.0f, 0.0f);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext.data(), wlen);

    ComPtr<IDWriteTextLayout> layout;
    const float layoutWidth = maxWidth > 0.0f ? maxWidth : 4096.0f;
    if (FAILED(s->dwrite->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
        format, layoutWidth, 4096.0f, &layout)) || !layout)
        return pushSize(0.0f, size);

    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return pushSize(metrics.widthIncludingTrailingWhitespace, metrics.height);
}

static int lua_DrawRect(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float w = static_cast<float>(luaL_checknumber(L, 3));
    float h = static_cast<float>(luaL_checknumber(L, 4));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float radius = static_cast<float>(luaL_optnumber(L, 6, 0));
    float alpha = static_cast<float>(luaL_optnumber(L, 7, 1.0));

    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h };
    if (radius > 0)
    {
        D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(rect, radius, radius);
        s->ctx->FillRoundedRectangle(rounded, brush);
    }
    else
    {
        s->ctx->FillRectangle(rect, brush);
    }
    return 0;
}

static int lua_DrawPushClip(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float width = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 3)));
    float height = std::max(0.0f, static_cast<float>(luaL_checknumber(L, 4)));
    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;
    s->ctx->PushAxisAlignedClip(D2D1::RectF(
        s->widgetRect.left + x,
        s->widgetRect.top + y,
        s->widgetRect.left + x + width,
        s->widgetRect.top + y + height),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ++s->widgetClipDepth;
    return 0;
}

static int lua_DrawPopClip(lua_State* L)
{
    auto* s = GetD2D(L);
    if (!s || !s->ctx || s->widgetClipDepth <= 0) return 0;
    s->ctx->PopAxisAlignedClip();
    --s->widgetClipDepth;
    return 0;
}

static int lua_GetTime(lua_State* L)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, st.wYear);   lua_setfield(L, -2, "year");
    lua_pushinteger(L, st.wMonth);  lua_setfield(L, -2, "month");
    lua_pushinteger(L, st.wDay);    lua_setfield(L, -2, "day");
    lua_pushinteger(L, st.wDayOfWeek + 1); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, st.wHour);   lua_setfield(L, -2, "hour");
    lua_pushinteger(L, st.wMinute); lua_setfield(L, -2, "min");
    lua_pushinteger(L, st.wSecond); lua_setfield(L, -2, "sec");
    return 1;
}

static int lua_Notify(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    const char* message = luaL_checkstring(L, 2);
    auto* s = GetD2D(L);
    if (s && s->engine)
    {
        const std::wstring widgetId = BoundWidgetId(L);
        if (!s->engine->RuntimeHasPermission(widgetId, "ui.notify"))
            return luaL_error(L, "widget permission denied: ui.notify");
        s->engine->RuntimeNotify(widgetId, Utf8ToWideLocal(title),
            Utf8ToWideLocal(message));
    }
    return 0;
}

static std::string Sha256File(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, resultSize = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest(32);
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return {};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    char buffer[8192];
    while (file)
    {
        file.read(buffer, sizeof(buffer));
        auto count = file.gcount();
        if (count > 0)
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(count), 0);
    }
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned char byte : digest)
    {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0F]);
    }
    return result;
}

static int CompareVersions(const std::string& left, const std::string& right)
{
    std::istringstream a(left), b(right);
    std::string ap, bp;
    for (int i = 0; i < 4; ++i)
    {
        int av = 0, bv = 0;
        if (std::getline(a, ap, '.')) av = std::atoi(ap.c_str());
        if (std::getline(b, bp, '.')) bv = std::atoi(bp.c_str());
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}

static bool JsonReadDouble(const std::string& text, const char* field, double& out)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p + marker.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    char* end = nullptr;
    out = std::strtod(text.c_str() + p, &end);
    return end != text.c_str() + p;
}

static std::vector<std::string> JsonReadObjectArray(const std::string& text, const char* field)
{
    std::vector<std::string> result;
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return result;
    size_t arrayStart = text.find('[', p + marker.size());
    if (arrayStart == std::string::npos) return result;
    bool inString = false;
    bool escaped = false;
    int arrayDepth = 0;
    int objectDepth = 0;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart; i < text.size(); ++i)
    {
        char ch = text[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') { inString = true; continue; }
        if (ch == '[') ++arrayDepth;
        else if (ch == ']')
        {
            if (--arrayDepth == 0) break;
        }
        else if (ch == '{')
        {
            if (objectDepth++ == 0) objectStart = i;
        }
        else if (ch == '}' && objectDepth > 0)
        {
            if (--objectDepth == 0 && objectStart != std::string::npos)
            {
                result.push_back(text.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }
    return result;
}

static bool JsonReadBool(const std::string& text, const char* field, bool& out)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p + marker.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
    if (text.compare(p, 4, "true") == 0) { out = true; return true; }
    if (text.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
}

static std::string JsonReadObjectField(const std::string& text, const char* field)
{
    std::string marker = std::string("\"") + field + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return {};
    size_t open = text.find('{', p + marker.size());
    if (open == std::string::npos) return {};

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (size_t i = open; i < text.size(); ++i)
    {
        char ch = text[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inString = false;
            continue;
        }
        if (ch == '"') { inString = true; continue; }
        if (ch == '{') ++depth;
        else if (ch == '}' && --depth == 0)
            return text.substr(open, i - open + 1);
    }
    return {};
}

static std::unordered_map<std::string, std::string> JsonReadValueMap(
    const std::string& text, const char* field)
{
    std::unordered_map<std::string, std::string> result;
    std::string object = JsonReadObjectField(text, field);
    if (object.empty()) return result;

    size_t p = 0;
    while (true)
    {
        p = object.find('"', p);
        if (p == std::string::npos) break;
        size_t keyEnd = object.find('"', p + 1);
        if (keyEnd == std::string::npos) break;
        std::string key = object.substr(p + 1, keyEnd - p - 1);
        p = object.find(':', keyEnd + 1);
        if (p == std::string::npos) break;
        ++p;
        while (p < object.size() && std::isspace(static_cast<unsigned char>(object[p]))) ++p;

        std::string value;
        if (p < object.size() && object[p] == '"')
        {
            for (++p; p < object.size(); ++p)
            {
                if (object[p] == '"') { ++p; break; }
                if (object[p] == '\\' && p + 1 < object.size())
                {
                    const char escaped = object[++p];
                    switch (escaped)
                    {
                    case 'n': value.push_back('\n'); break;
                    case 'r': value.push_back('\r'); break;
                    case 't': value.push_back('\t'); break;
                    default: value.push_back(escaped); break;
                    }
                }
                else
                    value.push_back(object[p]);
            }
        }
        else
        {
            size_t start = p;
            while (p < object.size() && object[p] != ',' && object[p] != '}') ++p;
            value = object.substr(start, p - start);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.pop_back();
            if (value == "true") value = "1";
            else if (value == "false") value = "0";
        }
        if (!key.empty()) result[key] = value;
    }
    return result;
}

static const std::unordered_map<std::string, std::string>*
SelectManifestLocale(const LuaWidgetManifest& manifest)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    std::vector<std::string> available;
    available.reserve(manifest.locales.size());
    for (const auto& [code, catalog] : manifest.locales)
    {
        (void)catalog;
        available.push_back(code);
    }
    const std::string selected =
        snowdesktop::localization::ResolveBestLanguage(available, language);
    auto catalog = manifest.locales.find(selected);
    if (catalog != manifest.locales.end()) return &catalog->second;
    catalog = manifest.locales.find("en-US");
    if (catalog != manifest.locales.end())
        return &catalog->second;
    return manifest.locales.empty() ? nullptr : &manifest.locales.begin()->second;
}

static std::string TranslateManifest(const LuaWidgetManifest& manifest,
    const std::string& key, const std::string& fallback = {})
{
    if (const auto* catalog = SelectManifestLocale(manifest))
    {
        auto value = catalog->find(key);
        if (value != catalog->end())
            return value->second;
    }
    auto english = manifest.locales.find("en-US");
    if (english != manifest.locales.end())
    {
        auto value = english->second.find(key);
        if (value != english->second.end())
            return value->second;
    }
    return fallback.empty() ? key : fallback;
}

static void PushWidgetL10nAPI(lua_State* L, const LuaWidgetManifest& manifest);

static bool IsHostStructureSettingKey(const std::string& key)
{
    return key == "cornerRadius" || key == "barHeight";
}

static bool IsHostSharedGlassSettingKey(const std::string& key)
{
    return key == "glassBlurRadius";
}

static bool IsHostAppearanceSettingKey(const std::string& key)
{
    return key == "bg" || key == "border" || key == "alpha" ||
        key == "borderAlpha" || key == "gradientEndA" ||
        IsRemovedPanelEffectSettingKey(key) || key == "glassEnabled" ||
        key == "glassBlurRadius" || key == "acrylicEnabled" ||
        key == "followPersonalization";
}

static std::string FindDeclaredDefaultValue(const LuaWidget& widget, const std::string& key)
{
    auto readPresetValue = [&key](const std::vector<LuaWidgetManifest::SettingPreset>& presets) {
        for (const auto& preset : presets)
        {
            if (!preset.isDefault && preset.id != "default")
                continue;
            auto it = preset.values.find(key);
            if (it != preset.values.end())
                return it->second;
        }
        return std::string{};
    };
    std::string value = readPresetValue(widget.manifest.presets);
    if (!value.empty()) return value;
    value = readPresetValue(widget.scriptPresets);
    if (!value.empty()) return value;

    auto readSettingValue = [&key](const std::vector<LuaWidgetManifest::Setting>& settings) {
        for (const auto& setting : settings)
            if (setting.key == key)
                return setting.defaultValue;
        return std::string{};
    };
    value = readSettingValue(widget.manifest.settings);
    if (!value.empty()) return value;
    return readSettingValue(widget.scriptSettings);
}

static bool RequirePermission(lua_State* L, const char* permission);

static void SetNumberField(lua_State* L, const char* key, lua_Number value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

static void SetBooleanField(lua_State* L, const char* key, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, key);
}

static int lua_SystemCpu(lua_State* L)
{
    if (!RequirePermission(L, "system.read")) return 0;
    auto* s = GetD2D(L);
    CpuSnapshot snapshot = s && s->engine
        ? s->engine->RuntimeGetCpuSnapshot(BoundWidgetId(L)) : CpuSnapshot{};
    lua_createtable(L, 0, 4);
    SetBooleanField(L, "available", snapshot.available);
    SetNumberField(L, "usagePercent", snapshot.usagePercent);
    SetNumberField(L, "logicalProcessors", snapshot.logicalProcessors);
    lua_pushstring(L, snapshot.name.c_str()); lua_setfield(L, -2, "name");
    return 1;
}

static int lua_SystemMemory(lua_State* L)
{
    if (!RequirePermission(L, "system.read")) return 0;
    auto* s = GetD2D(L);
    MemorySnapshot snapshot = s && s->engine
        ? s->engine->RuntimeGetMemorySnapshot(BoundWidgetId(L)) : MemorySnapshot{};
    lua_createtable(L, 0, 5);
    SetBooleanField(L, "available", snapshot.available);
    SetNumberField(L, "totalBytes", static_cast<lua_Number>(snapshot.totalBytes));
    SetNumberField(L, "usedBytes", static_cast<lua_Number>(snapshot.usedBytes));
    SetNumberField(L, "freeBytes", static_cast<lua_Number>(snapshot.freeBytes));
    SetNumberField(L, "usagePercent", snapshot.usagePercent);
    return 1;
}

static int lua_SystemBattery(lua_State* L)
{
    if (!RequirePermission(L, "system.read")) return 0;
    auto* s = GetD2D(L);
    BatterySnapshot snapshot = s && s->engine
        ? s->engine->RuntimeGetBatterySnapshot(BoundWidgetId(L)) : BatterySnapshot{};
    lua_createtable(L, 0, 5);
    SetBooleanField(L, "available", snapshot.available);
    SetNumberField(L, "percent", snapshot.percent);
    SetBooleanField(L, "charging", snapshot.charging);
    SetBooleanField(L, "pluggedIn", snapshot.pluggedIn);
    SetBooleanField(L, "saver", snapshot.saver);
    return 1;
}

static int lua_SystemNetwork(lua_State* L)
{
    if (!RequirePermission(L, "system.read")) return 0;
    auto* s = GetD2D(L);
    NetworkSnapshot snapshot = s && s->engine
        ? s->engine->RuntimeGetNetworkSnapshot(BoundWidgetId(L)) : NetworkSnapshot{};
    lua_createtable(L, 0, 7);
    SetBooleanField(L, "available", snapshot.available);
    SetBooleanField(L, "connected", snapshot.connected);
    SetNumberField(L, "downloadBytesPerSec", static_cast<lua_Number>(snapshot.downloadBytesPerSec));
    SetNumberField(L, "uploadBytesPerSec", static_cast<lua_Number>(snapshot.uploadBytesPerSec));
    SetNumberField(L, "receivedBytes", static_cast<lua_Number>(snapshot.receivedBytes));
    SetNumberField(L, "sentBytes", static_cast<lua_Number>(snapshot.sentBytes));
    return 1;
}

static int lua_SystemGpu(lua_State* L)
{
    if (!RequirePermission(L, "system.read")) return 0;
    auto* s = GetD2D(L);
    GpuSnapshot snapshot = s && s->engine
        ? s->engine->RuntimeGetGpuSnapshot(BoundWidgetId(L)) : GpuSnapshot{};
    lua_createtable(L, 0, 5);
    SetBooleanField(L, "available", snapshot.available);
    lua_pushstring(L, snapshot.name.c_str()); lua_setfield(L, -2, "name");
    SetNumberField(L, "usagePercent", snapshot.usagePercent);
    SetNumberField(L, "vramTotalBytes", static_cast<lua_Number>(snapshot.vramTotalBytes));
    SetNumberField(L, "vramUsedBytes", static_cast<lua_Number>(snapshot.vramUsedBytes));
    return 1;
}

static int lua_MediaCurrent(lua_State* L)
{
    if (!RequirePermission(L, "media.read")) return 0;
    auto* s = GetD2D(L);
    MediaSnapshot snapshot = s && s->engine
        ? s->engine->RuntimeGetMediaSnapshot(BoundWidgetId(L)) : MediaSnapshot{};
    lua_createtable(L, 0, 10);
    SetBooleanField(L, "available", snapshot.available);
    lua_pushstring(L, WidgetWideToUtf8(snapshot.title).c_str()); lua_setfield(L, -2, "title");
    lua_pushstring(L, WidgetWideToUtf8(snapshot.artist).c_str()); lua_setfield(L, -2, "artist");
    lua_pushstring(L, WidgetWideToUtf8(snapshot.album).c_str()); lua_setfield(L, -2, "album");
    lua_pushstring(L, WidgetWideToUtf8(snapshot.sourceApp).c_str()); lua_setfield(L, -2, "sourceApp");
    lua_pushstring(L, snapshot.playbackStatus.c_str()); lua_setfield(L, -2, "playbackStatus");
    SetBooleanField(L, "canPlayPause", snapshot.canPlayPause);
    SetBooleanField(L, "canNext", snapshot.canNext);
    SetBooleanField(L, "canPrevious", snapshot.canPrevious);
    return 1;
}

static int lua_MediaPlayPause(lua_State* L)
{
    if (!RequirePermission(L, "media.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeMediaPlayPause());
    return 1;
}

static int lua_MediaNext(lua_State* L)
{
    if (!RequirePermission(L, "media.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeMediaNext());
    return 1;
}

static int lua_MediaPrevious(lua_State* L)
{
    if (!RequirePermission(L, "media.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeMediaPrevious());
    return 1;
}

static int lua_WidgetSetTimer(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    int intervalMs = static_cast<int>(luaL_checkinteger(L, 2));
    bool repeat = lua_isnoneornil(L, 3) || lua_toboolean(L, 3) != 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine &&
        s->engine->RuntimeSetTimer(BoundWidgetId(L), name ? name : "", intervalMs, repeat));
    return 1;
}

static int lua_WidgetCancelTimer(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine &&
        s->engine->RuntimeCancelTimer(BoundWidgetId(L), name ? name : ""));
    return 1;
}

static int lua_HttpRequest(lua_State* L)
{
    if (!RequirePermission(L, "network.http")) return 0;
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* s = GetD2D(L);
    if (!s || !s->engine) { lua_pushnil(L); return 1; }

    HttpRequestOptions options;
    options.widgetId = BoundWidgetId(L);
    lua_getfield(L, 1, "url");
    options.url = Utf8ToWideLocal(luaL_checkstring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "method");
    if (lua_isstring(L, -1)) options.method = Utf8ToWideLocal(lua_tostring(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "body");
    if (lua_isstring(L, -1)) options.body = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "timeoutMs");
    if (lua_isinteger(L, -1)) options.timeoutMs = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "cacheSeconds");
    if (lua_isinteger(L, -1)) options.cacheSeconds = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1))
    {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0)
        {
            if (lua_isstring(L, -2) && lua_isstring(L, -1))
            {
                options.headers += Utf8ToWideLocal(lua_tostring(L, -2));
                options.headers += L": ";
                options.headers += Utf8ToWideLocal(lua_tostring(L, -1));
                options.headers += L"\r\n";
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    int id = s->engine->RuntimeHttpRequest(BoundWidgetId(L), std::move(options));
    if (id > 0) lua_pushinteger(L, id); else lua_pushnil(L);
    return 1;
}

static int lua_HttpCancel(lua_State* L)
{
    if (!RequirePermission(L, "network.http")) return 0;
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine &&
        s->engine->RuntimeHttpCancel(BoundWidgetId(L), id));
    return 1;
}

static void DrawHostRect(D2DState* state, float x, float y, float width, float height,
    int color, float radius, float alpha)
{
    if (!state || !state->ctx) return;
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color, alpha);
    if (!brush) return;
    D2D1_RECT_F rect = D2D1::RectF(
        state->widgetRect.left + x, state->widgetRect.top + y,
        state->widgetRect.left + x + width, state->widgetRect.top + y + height);
    if (radius > 0)
        state->ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    else
        state->ctx->FillRectangle(rect, brush);
}

static void DrawHostText(D2DState* state, const std::wstring& text,
    float x, float y, float width, float height, float size, int color)
{
    if (!state || !state->ctx || !state->dwrite) return;
    const float scale = CalculateWidgetCellScale(state->gridCellW, state->gridCellH);
    const float scaledSize = std::max(9.0f, size * scale);
    IDWriteTextFormat* format = GetCachedTextFormat(state, scaledSize,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, true, DWRITE_WORD_WRAPPING_NO_WRAP);
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color);
    if (!format || !brush) return;
    state->ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        D2D1::RectF(state->widgetRect.left + x, state->widgetRect.top + y,
            state->widgetRect.left + x + width, state->widgetRect.top + y + height),
        brush);
}

static void DrawHostStrokeRect(D2DState* state, float x, float y, float width, float height,
    int color, float radius, float thickness, float alpha)
{
    if (!state || !state->ctx) return;
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color, alpha);
    if (!brush) return;
    D2D1_RECT_F rect = D2D1::RectF(
        state->widgetRect.left + x, state->widgetRect.top + y,
        state->widgetRect.left + x + width, state->widgetRect.top + y + height);
    if (radius > 0)
        state->ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, thickness);
    else
        state->ctx->DrawRectangle(rect, brush, thickness);
}

static ComPtr<IDWriteTextLayout> CreateHostSingleLineTextLayout(
    D2DState* state, const std::wstring& text, float size,
    float width, float height)
{
    ComPtr<IDWriteTextLayout> layout;
    if (!state || !state->dwrite)
        return layout;
    IDWriteTextFormat* format = GetCachedTextFormat(state,
        std::max(9.0f, size), DWRITE_FONT_WEIGHT_NORMAL, false,
        DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
    if (!format)
        return layout;
    const std::wstring layoutText = text.empty() ? L" " : text;
    if (FAILED(state->dwrite->CreateTextLayout(layoutText.c_str(),
        static_cast<UINT32>(layoutText.size()), format,
        std::max(1.0f, width), std::max(1.0f, height),
        &layout)))
        layout.Reset();
    return layout;
}

static ComPtr<IDWriteTextLayout> CreateHostMultilineTextLayout(
    D2DState* state, const std::wstring& text, float size, float width)
{
    ComPtr<IDWriteTextLayout> layout;
    if (!state || !state->dwrite)
        return layout;
    IDWriteTextFormat* format = GetCachedTextFormat(state,
        std::max(9.0f, size), DWRITE_FONT_WEIGHT_NORMAL, false,
        DWRITE_WORD_WRAPPING_WRAP, false, false);
    if (!format)
        return layout;
    // DirectWrite does not expose an empty final line for a trailing
    // newline. Keep a layout-only space there so the caret can occupy it.
    std::wstring layoutText = text;
    if (layoutText.empty() ||
        layoutText.back() == L'\n' ||
        layoutText.back() == L'\r')
        layoutText.push_back(L' ');
    if (FAILED(state->dwrite->CreateTextLayout(layoutText.c_str(),
        static_cast<UINT32>(layoutText.size()), format,
        std::max(1.0f, width), 100000.0f, &layout)))
        layout.Reset();
    return layout;
}

static bool HostTextIsWhitespaceOnly(const std::wstring& text)
{
    return std::all_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' ||
            ch == L'\n' || ch == L'\v' || ch == L'\f';
    });
}

static void DrawHostTextSelection(D2DState* state,
    IDWriteTextLayout* textLayout, size_t selectionStart,
    size_t selectionEnd, float originX, float originY,
    int color)
{
    if (!state || !textLayout ||
        selectionStart >= selectionEnd ||
        selectionStart >
            static_cast<size_t>(std::numeric_limits<UINT32>::max()))
        return;
    const UINT32 start = static_cast<UINT32>(selectionStart);
    const size_t boundedLength = std::min(
        selectionEnd - selectionStart,
        static_cast<size_t>(
            std::numeric_limits<UINT32>::max() - start));
    if (boundedLength == 0)
        return;

    UINT32 count = 0;
    textLayout->HitTestTextRange(start,
        static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
        nullptr, 0, &count);
    if (count == 0)
        return;
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(textLayout->HitTestTextRange(start,
        static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
        metrics.data(), count, &count)))
        return;
    // DirectWrite may return one metric per glyph/run. Drawing every metric
    // independently makes adjacent selection pieces overlap, especially for
    // proportional text, and produces visible rounded blue boxes while the
    // selection is being dragged. Merge contiguous pieces on each line and
    // draw one flat highlight per line so the selection remains stable.
    std::vector<D2D1_RECT_F> selectionRects;
    selectionRects.reserve(count);
    for (UINT32 i = 0; i < count; ++i)
    {
        const auto& hit = metrics[i];
        const D2D1_RECT_F rect = D2D1::RectF(
            hit.left, hit.top,
            hit.left + std::max(1.5f, hit.width),
            hit.top + std::max(1.0f, hit.height));
        if (!selectionRects.empty())
        {
            auto& previous = selectionRects.back();
            const bool sameLine =
                std::abs(previous.top - rect.top) < 0.5f &&
                std::abs(previous.bottom - rect.bottom) < 0.5f;
            const bool contiguous = rect.left <= previous.right + 0.5f;
            if (sameLine && contiguous)
            {
                previous.left = std::min(previous.left, rect.left);
                previous.right = std::max(previous.right, rect.right);
                previous.top = std::min(previous.top, rect.top);
                previous.bottom = std::max(previous.bottom, rect.bottom);
                continue;
            }
        }
        selectionRects.push_back(rect);
    }

    for (const auto& rect : selectionRects)
    {
        DrawHostRect(state, originX + rect.left,
            originY + rect.top, rect.right - rect.left,
            rect.bottom - rect.top, color, 0.0f, 0.32f);
    }
}

struct HostInputDisplayText
{
    std::wstring text;
    size_t cursor = 0;
    size_t compositionStart = 0;
    size_t compositionLength = 0;
};

static HostInputDisplayText BuildHostInputDisplayText(
    const std::wstring& text, size_t cursor,
    size_t selectionAnchor, const std::wstring& compositionText,
    size_t compositionCursor)
{
    HostInputDisplayText result;
    result.text = text;
    result.cursor = std::min(cursor, result.text.size());
    if (compositionText.empty())
        return result;

    const size_t anchor = std::min(
        selectionAnchor, result.text.size());
    const size_t selectionStart = std::min(result.cursor, anchor);
    const size_t selectionEnd = std::max(result.cursor, anchor);
    result.text.erase(
        selectionStart, selectionEnd - selectionStart);
    result.text.insert(selectionStart, compositionText);
    result.compositionStart = selectionStart;
    result.compositionLength = compositionText.size();
    result.cursor = selectionStart + std::min(
        compositionCursor, compositionText.size());
    return result;
}

static void DrawHostCompositionUnderline(D2DState* state,
    IDWriteTextLayout* textLayout, size_t compositionStart,
    size_t compositionLength, float originX, float originY,
    int color)
{
    if (!state || !textLayout || compositionLength == 0 ||
        compositionStart >
            static_cast<size_t>(std::numeric_limits<UINT32>::max()))
        return;
    const UINT32 start =
        static_cast<UINT32>(compositionStart);
    const size_t boundedLength = std::min(
        compositionLength,
        static_cast<size_t>(
            std::numeric_limits<UINT32>::max() - start));
    if (boundedLength == 0)
        return;

    UINT32 count = 0;
    textLayout->HitTestTextRange(start,
        static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
        nullptr, 0, &count);
    if (count == 0)
        return;
    std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
    if (FAILED(textLayout->HitTestTextRange(start,
            static_cast<UINT32>(boundedLength), 0.0f, 0.0f,
            metrics.data(), count, &count)))
        return;
    for (UINT32 i = 0; i < count; ++i)
    {
        const auto& hit = metrics[i];
        DrawHostRect(state, originX + hit.left,
            originY + hit.top + std::max(1.0f, hit.height - 1.5f),
            std::max(1.5f, hit.width), 1.5f,
            color, 0.0f, 0.92f);
    }
}

static int lua_UiTextInput(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* storageKey = luaL_checkstring(L, 2);
    float x = static_cast<float>(luaL_checknumber(L, 3));
    float y = static_cast<float>(luaL_checknumber(L, 4));
    float width = static_cast<float>(luaL_checknumber(L, 5));
    float height = static_cast<float>(luaL_checknumber(L, 6));
    const int options = lua_istable(L, 7) ? lua_absindex(L, 7) : 0;

    auto numberOption = [&](const char* key, double fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        double result = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };
    auto integerOption = [&](const char* key, int fallback) {
        return static_cast<int>(numberOption(key, fallback));
    };
    auto boolOption = [&](const char* key, bool fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        bool result = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return result;
    };
    auto stringOption = [&](const char* key, const char* fallback) {
        if (!options) return std::string(fallback);
        lua_getfield(L, options, key);
        std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };

    const std::string placeholder = stringOption("placeholder", "");
    const float fontSize = std::clamp(static_cast<float>(numberOption("fontSize", 15.0)), 9.0f, 96.0f);
    const int textColor = integerOption("textColor", 0xFFFFFF);
    const int placeholderColor = integerOption("placeholderColor", 0x94A3B8);
    const int backgroundColor = integerOption("backgroundColor", 0xFFFFFF);
    const int borderColor = integerOption("borderColor", 0xFFFFFF);
    const int focusedBorderColor = integerOption("focusedBorderColor", 0x64A8FF);
    const float backgroundAlpha = std::clamp(
        static_cast<float>(numberOption("backgroundAlpha", 0.05)), 0.0f, 1.0f);
    const float focusedBackgroundAlpha = std::clamp(
        static_cast<float>(numberOption("focusedBackgroundAlpha", 0.12)), 0.0f, 1.0f);
    const float borderAlpha = std::clamp(
        static_cast<float>(numberOption("borderAlpha", 0.12)), 0.0f, 1.0f);
    const float focusedBorderAlpha = std::clamp(
        static_cast<float>(numberOption("focusedBorderAlpha", 0.70)), 0.0f, 1.0f);
    const float radius = std::max(0.0f, static_cast<float>(numberOption("radius", 6.0)));
    const float padding = std::max(0.0f, static_cast<float>(numberOption("padding", 10.0)));
    const float borderThickness = std::max(0.5f,
        static_cast<float>(numberOption("borderThickness", 1.0)));
    const bool selectAll = boolOption("selectAll", false);
    const bool liveUpdate = boolOption("liveUpdate", true);

    auto* s = GetD2D(L);
    std::string value;
    if (s && s->engine && storageKey && *storageKey)
        value = s->engine->RuntimeGetStorageValue(BoundWidgetId(L), storageKey);

    bool focused = false;
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::wstring focusedText;
    std::wstring compositionText;
    size_t compositionCursor = 0;
    if (s && s->engine && id && *id)
    {
        focused = s->engine->RuntimeGetFocusedHostInput(
            BoundWidgetId(L), id, focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);
        if (focused)
            value = WidgetWideToUtf8(focusedText);
    }
    const bool widgetSelected = s && s->engine &&
        s->engine->RuntimeIsWidgetSelected(BoundWidgetId(L));
    const bool drawFocusedBorder = focused && !widgetSelected;
    const HostInputDisplayText focusedDisplay =
        BuildHostInputDisplayText(focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);

    DrawHostRect(s, x, y, width, height, backgroundColor, radius,
        focused ? focusedBackgroundAlpha : backgroundAlpha);
    DrawHostStrokeRect(s, x, y, width, height,
        drawFocusedBorder ? focusedBorderColor : borderColor, radius,
        borderThickness,
        drawFocusedBorder ? focusedBorderAlpha : borderAlpha);
    const bool showingPlaceholder = value.empty() && !focused;
    const std::wstring displayText = focused
        ? focusedDisplay.text
        : Utf8ToWideLocal(showingPlaceholder ? placeholder : value);
    const float innerWidth =
        std::max(1.0f, width - padding * 2.0f);
    ComPtr<IDWriteTextLayout> textLayout =
        CreateHostSingleLineTextLayout(s, displayText,
            fontSize, innerWidth, height);
    if (s && s->ctx)
    {
        const D2D1_RECT_F clip = D2D1::RectF(
            s->widgetRect.left + x + padding,
            s->widgetRect.top + y,
            s->widgetRect.left + x + width - padding,
            s->widgetRect.top + y + height);
        s->ctx->PushAxisAlignedClip(
            clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (focused && compositionText.empty() &&
            selectionAnchor != cursor &&
            textLayout)
        {
            DrawHostTextSelection(s, textLayout.Get(),
                std::min(selectionAnchor, cursor),
                std::max(selectionAnchor, cursor),
                x + padding, y, focusedBorderColor);
        }
        ID2D1SolidColorBrush* textBrush = GetCachedBrush(
            s, showingPlaceholder
                ? placeholderColor : textColor);
        if (textBrush && textLayout)
        {
            s->ctx->DrawTextLayout(
                D2D1::Point2F(
                    s->widgetRect.left + x + padding,
                    s->widgetRect.top + y),
                textLayout.Get(), textBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (focused && focusedDisplay.compositionLength > 0 &&
            textLayout)
        {
            DrawHostCompositionUnderline(s, textLayout.Get(),
                focusedDisplay.compositionStart,
                focusedDisplay.compositionLength,
                x + padding, y, textColor);
        }

        if (focused && textLayout)
        {
            const size_t safeCursor =
                std::min(focusedDisplay.cursor,
                    focusedDisplay.text.size());
            UINT32 hitPosition = 0;
            BOOL trailing = FALSE;
            if (!focusedDisplay.text.empty())
            {
                if (safeCursor >= focusedDisplay.text.size())
                {
                    hitPosition = static_cast<UINT32>(
                        focusedDisplay.text.size() - 1);
                    trailing = TRUE;
                }
                else
                    hitPosition =
                        static_cast<UINT32>(safeCursor);
            }
            float caretX = 0.0f;
            float caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS caretMetrics{};
            if (SUCCEEDED(textLayout->HitTestTextPosition(
                hitPosition, trailing, &caretX, &caretY,
                &caretMetrics)))
            {
                DrawHostRect(s, x + padding + caretX,
                    y + caretY, 1.5f,
                    std::max(caretMetrics.height, fontSize),
                    textColor, 0.0f, 0.98f);
            }
        }
        s->ctx->PopAxisAlignedClip();
    }
    if (s && s->engine && id && *id && storageKey && *storageKey)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Input;
        control.id = id;
        control.storageKey = storageKey;
        control.rect = { static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y)),
            static_cast<LONG>(std::lround(x + width)), static_cast<LONG>(std::lround(y + height)) };
        control.selectAll = selectAll;
        control.liveUpdate = liveUpdate;
        control.fontSize = fontSize;
        control.padding = padding;
        s->engine->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }

    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

static int lua_UiTextArea(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* storageKey = luaL_checkstring(L, 2);
    const float x = static_cast<float>(luaL_checknumber(L, 3));
    const float y = static_cast<float>(luaL_checknumber(L, 4));
    const float width = static_cast<float>(luaL_checknumber(L, 5));
    const float height = static_cast<float>(luaL_checknumber(L, 6));
    const int options = lua_istable(L, 7) ? lua_absindex(L, 7) : 0;

    auto numberOption = [&](const char* key, double fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        const double result = lua_isnumber(L, -1)
            ? lua_tonumber(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };
    auto integerOption = [&](const char* key, int fallback) {
        return static_cast<int>(numberOption(key, fallback));
    };
    auto boolOption = [&](const char* key, bool fallback) {
        if (!options) return fallback;
        lua_getfield(L, options, key);
        const bool result = lua_isnil(L, -1)
            ? fallback : lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return result;
    };
    auto stringOption = [&](const char* key, const char* fallback) {
        if (!options) return std::string(fallback);
        lua_getfield(L, options, key);
        const std::string result = lua_isstring(L, -1)
            ? lua_tostring(L, -1) : fallback;
        lua_pop(L, 1);
        return result;
    };

    const std::string placeholder = stringOption("placeholder", "");
    const float fontSize = std::clamp(static_cast<float>(
        numberOption("fontSize", 15.0)), 9.0f, 96.0f);
    const int textColor = integerOption("textColor", 0xFFFFFF);
    const int placeholderColor =
        integerOption("placeholderColor", 0x94A3B8);
    const int backgroundColor =
        integerOption("backgroundColor", 0xFFFFFF);
    const int borderColor = integerOption("borderColor", 0xFFFFFF);
    const int focusedBorderColor =
        integerOption("focusedBorderColor", 0x64A8FF);
    const float backgroundAlpha = std::clamp(static_cast<float>(
        numberOption("backgroundAlpha", 0.05)), 0.0f, 1.0f);
    const float focusedBackgroundAlpha = std::clamp(static_cast<float>(
        numberOption("focusedBackgroundAlpha", 0.08)), 0.0f, 1.0f);
    const float borderAlpha = std::clamp(static_cast<float>(
        numberOption("borderAlpha", 0.0)), 0.0f, 1.0f);
    const float focusedBorderAlpha = std::clamp(static_cast<float>(
        numberOption("focusedBorderAlpha", 0.35)), 0.0f, 1.0f);
    const float radius = std::max(0.0f,
        static_cast<float>(numberOption("radius", 6.0)));
    const float padding = std::max(0.0f,
        static_cast<float>(numberOption("padding", 8.0)));
    const float borderThickness = std::max(0.5f,
        static_cast<float>(numberOption("borderThickness", 1.0)));
    const bool selectAll = boolOption("selectAll", false);
    const bool liveUpdate = boolOption("liveUpdate", true);
    const bool placeholderWhenWhitespace =
        boolOption("placeholderWhenWhitespace", false);

    auto* s = GetD2D(L);
    std::string value;
    if (s && s->engine && storageKey && *storageKey)
        value = s->engine->RuntimeGetStorageValue(
            BoundWidgetId(L), storageKey);

    bool focused = false;
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::wstring focusedText;
    std::wstring compositionText;
    size_t compositionCursor = 0;
    if (s && s->engine && id && *id)
    {
        focused = s->engine->RuntimeGetFocusedHostInput(
            BoundWidgetId(L), id, focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);
        if (focused)
            value = WidgetWideToUtf8(focusedText);
    }
    const bool widgetSelected = s && s->engine &&
        s->engine->RuntimeIsWidgetSelected(BoundWidgetId(L));
    const bool drawFocusedBorder = focused && !widgetSelected;
    const HostInputDisplayText focusedDisplay =
        BuildHostInputDisplayText(focusedText, cursor,
            selectionAnchor, compositionText,
            compositionCursor);

    DrawHostRect(s, x, y, width, height, backgroundColor, radius,
        focused ? focusedBackgroundAlpha : backgroundAlpha);
    DrawHostStrokeRect(s, x, y, width, height,
        drawFocusedBorder ? focusedBorderColor : borderColor, radius,
        borderThickness,
        drawFocusedBorder ? focusedBorderAlpha : borderAlpha);

    const std::wstring valueText = focused
        ? focusedDisplay.text : Utf8ToWideLocal(value);
    const bool semanticallyEmpty = valueText.empty() ||
        (placeholderWhenWhitespace &&
            HostTextIsWhitespaceOnly(valueText));
    const bool showingPlaceholder = semanticallyEmpty && !focused;
    const std::wstring displayText = showingPlaceholder
        ? Utf8ToWideLocal(placeholder) : valueText;
    const float scrollbarReserve = 8.0f;
    const float innerWidth = std::max(1.0f,
        width - padding * 2.0f - scrollbarReserve);
    ComPtr<IDWriteTextLayout> textLayout =
        CreateHostMultilineTextLayout(s, displayText, fontSize,
            innerWidth);
    DWRITE_TEXT_METRICS textMetrics{};
    if (textLayout)
        textLayout->GetMetrics(&textMetrics);
    const int contentHeight = std::max(
        static_cast<int>(std::ceil(textMetrics.height + padding * 2.0f)),
        static_cast<int>(std::ceil(height)));

    if (s && s->engine && id && *id &&
        storageKey && *storageKey)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Input;
        control.id = id;
        control.storageKey = storageKey;
        control.rect = {
            static_cast<LONG>(std::lround(x)),
            static_cast<LONG>(std::lround(y)),
            static_cast<LONG>(std::lround(x + width)),
            static_cast<LONG>(std::lround(y + height))
        };
        control.selectAll = selectAll;
        control.liveUpdate = liveUpdate;
        control.multiline = true;
        control.fontSize = fontSize;
        control.padding = padding;
        control.contentHeight = contentHeight;
        control.viewportHeight =
            std::max(1, static_cast<int>(std::lround(height)));
        s->engine->RuntimeRegisterHostControl(
            BoundWidgetId(L), std::move(control));
    }

    int scrollOffset = s && s->engine
        ? s->engine->RuntimeGetScrollOffset(
            BoundWidgetId(L), id) : 0;
    ComPtr<IDWriteTextLayout> focusedLayout;
    DWRITE_HIT_TEST_METRICS caretMetrics{};
    float caretX = 0.0f;
    float caretY = 0.0f;
    bool hasCaret = false;
    if (focused)
    {
        focusedLayout = CreateHostMultilineTextLayout(s,
            focusedDisplay.text, fontSize, innerWidth);
        if (focusedLayout)
        {
            const size_t safeCursor =
                std::min(focusedDisplay.cursor,
                    focusedDisplay.text.size());
            UINT32 hitPosition = 0;
            BOOL trailing = FALSE;
            if (!focusedDisplay.text.empty())
            {
                if (safeCursor >= focusedDisplay.text.size() &&
                    (focusedDisplay.text.back() == L'\n' ||
                        focusedDisplay.text.back() == L'\r'))
                {
                    hitPosition = static_cast<UINT32>(
                        focusedDisplay.text.size());
                    trailing = FALSE;
                }
                else if (safeCursor >=
                    focusedDisplay.text.size())
                {
                    hitPosition = static_cast<UINT32>(
                        focusedDisplay.text.size() - 1);
                    trailing = TRUE;
                }
                else
                    hitPosition =
                        static_cast<UINT32>(safeCursor);
            }
            hasCaret = SUCCEEDED(
                focusedLayout->HitTestTextPosition(
                    hitPosition, trailing, &caretX, &caretY,
                    &caretMetrics));
            if (hasCaret && s && s->engine)
            {
                const float caretTop = padding + caretY;
                const float caretBottom = caretTop +
                    std::max(caretMetrics.height, fontSize);
                const float visibleHeight =
                    std::max(1.0f, height - padding * 2.0f);
                int desiredOffset = scrollOffset;
                if (caretTop < scrollOffset)
                    desiredOffset =
                        static_cast<int>(std::floor(caretTop));
                else if (caretBottom >
                    scrollOffset + visibleHeight)
                    desiredOffset = static_cast<int>(std::ceil(
                        caretBottom - visibleHeight));
                s->engine->RuntimeSetScrollOffset(
                    BoundWidgetId(L), id, desiredOffset);
                scrollOffset =
                    s->engine->RuntimeGetScrollOffset(
                        BoundWidgetId(L), id);
            }
        }
    }

    if (s && s->ctx)
    {
        const D2D1_RECT_F clip = D2D1::RectF(
            s->widgetRect.left + x + padding,
            s->widgetRect.top + y + padding,
            s->widgetRect.left + x + width -
                padding - scrollbarReserve,
            s->widgetRect.top + y + height - padding);
        s->ctx->PushAxisAlignedClip(
            clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (focused && compositionText.empty() &&
            selectionAnchor != cursor &&
            focusedLayout)
        {
            DrawHostTextSelection(s, focusedLayout.Get(),
                std::min(selectionAnchor, cursor),
                std::max(selectionAnchor, cursor),
                x + padding, y + padding - scrollOffset,
                focusedBorderColor);
        }
        ID2D1SolidColorBrush* textBrush = GetCachedBrush(
            s, showingPlaceholder
                ? placeholderColor : textColor);
        IDWriteTextLayout* layoutToDraw = focused &&
            focusedLayout ? focusedLayout.Get() :
            textLayout.Get();
        if (textBrush && layoutToDraw)
        {
            s->ctx->DrawTextLayout(
                D2D1::Point2F(
                    s->widgetRect.left + x + padding,
                    s->widgetRect.top + y + padding -
                        scrollOffset),
                layoutToDraw, textBrush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (focused && focusedDisplay.compositionLength > 0 &&
            focusedLayout)
        {
            DrawHostCompositionUnderline(s,
                focusedLayout.Get(),
                focusedDisplay.compositionStart,
                focusedDisplay.compositionLength,
                x + padding, y + padding - scrollOffset,
                textColor);
        }
        if (focused && hasCaret)
        {
            const float cursorHeight = std::max(
                caretMetrics.height, fontSize);
            DrawHostRect(s,
                x + padding + caretX,
                y + padding + caretY - scrollOffset,
                1.5f, cursorHeight, textColor, 0.0f, 0.98f);
        }
        s->ctx->PopAxisAlignedClip();
    }

    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

static int lua_UiFocusInput(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    bool focused = s && s->engine && id && *id &&
        s->engine->RuntimeFocusHostInput(BoundWidgetId(L), id);
    lua_pushboolean(L, focused);
    return 1;
}

static int lua_UiButton(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);
    float x = static_cast<float>(luaL_checknumber(L, 3));
    float y = static_cast<float>(luaL_checknumber(L, 4));
    float width = static_cast<float>(luaL_checknumber(L, 5));
    float height = static_cast<float>(luaL_checknumber(L, 6));
    bool enabled = lua_isnoneornil(L, 7) || lua_toboolean(L, 7) != 0;
    auto* s = GetD2D(L);
    DrawHostRect(s, x, y, width, height, enabled ? 0x3478D4 : 0x555B65, 6, enabled ? 0.95f : 0.55f);
    DrawHostText(s, Utf8ToWideLocal(label), x, y, width, height, 13, 0xFFFFFF);
    if (enabled && s && s->engine)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Button;
        control.id = id;
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        s->engine->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    return 0;
}

static int lua_UiToggle(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);
    float x = static_cast<float>(luaL_checknumber(L, 3));
    float y = static_cast<float>(luaL_checknumber(L, 4));
    float width = static_cast<float>(luaL_checknumber(L, 5));
    float height = static_cast<float>(luaL_checknumber(L, 6));
    bool value = lua_toboolean(L, 7) != 0;
    auto* s = GetD2D(L);
    DrawHostRect(s, x, y, width, height, 0x29323B, 6, 0.95f);
    DrawHostText(s, Utf8ToWideLocal(label), x + 6, y, width - height - 8, height, 12, 0xFFFFFF);
    DrawHostRect(s, x + width - height + 3, y + 3, height - 6, height - 6,
        value ? 0x39B980 : 0x69717A, (height - 6) / 2, 1.0f);
    if (s && s->engine)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Toggle;
        control.id = id;
        control.value = value;
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        s->engine->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    return 0;
}

static int lua_UiProgress(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float width = static_cast<float>(luaL_checknumber(L, 3));
    float height = static_cast<float>(luaL_checknumber(L, 4));
    float value = std::clamp(static_cast<float>(luaL_checknumber(L, 5)), 0.0f, 1.0f);
    int color = static_cast<int>(luaL_optinteger(L, 6, 0x4EA1FF));
    auto* s = GetD2D(L);
    DrawHostRect(s, x, y, width, height, 0x26313A, height / 2, 1.0f);
    DrawHostRect(s, x, y, width * value, height, color, height / 2, 1.0f);
    return 0;
}

static int lua_UiScrollArea(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float width = static_cast<float>(luaL_checknumber(L, 4));
    float height = static_cast<float>(luaL_checknumber(L, 5));
    int contentHeight = static_cast<int>(luaL_checkinteger(L, 6));
    auto* s = GetD2D(L);
    if (s && s->engine)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Scroll;
        control.id = id;
        control.contentHeight = std::max(contentHeight, static_cast<int>(height));
        control.viewportHeight = static_cast<int>(height);
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        s->engine->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    int offset = s && s->engine
        ? s->engine->RuntimeGetScrollOffset(BoundWidgetId(L), id) : 0;
    if (s && s->engine)
    {
        s->engine->RuntimeSetScrollOffset(BoundWidgetId(L), id, offset);
        offset = s->engine->RuntimeGetScrollOffset(BoundWidgetId(L), id);
    }
    lua_pushinteger(L, offset);
    return 1;
}

static int lua_UiVirtualList(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float width = static_cast<float>(luaL_checknumber(L, 4));
    float height = static_cast<float>(luaL_checknumber(L, 5));
    int itemHeight = std::max(1, static_cast<int>(luaL_checkinteger(L, 6)));
    int count = std::max(0, static_cast<int>(luaL_checkinteger(L, 7)));
    const int viewportHeight = std::max(1, static_cast<int>(height));
    auto* s = GetD2D(L);
    if (s && s->engine)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Scroll;
        control.id = id;
        control.contentHeight = count * itemHeight;
        control.viewportHeight = viewportHeight;
        control.rect = { static_cast<LONG>(x), static_cast<LONG>(y),
            static_cast<LONG>(x + width), static_cast<LONG>(y + height) };
        s->engine->RuntimeRegisterHostControl(BoundWidgetId(L), std::move(control));
    }
    int offset = s && s->engine
        ? s->engine->RuntimeGetScrollOffset(BoundWidgetId(L), id) : 0;
    if (s && s->engine)
    {
        s->engine->RuntimeSetScrollOffset(BoundWidgetId(L), id, offset);
        offset = s->engine->RuntimeGetScrollOffset(BoundWidgetId(L), id);
    }
    int first = count == 0 ? 0 : offset / itemHeight + 1;
    int last = count == 0 ? 0 : std::min(count,
        (offset + viewportHeight + itemHeight - 1) / itemHeight);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, first); lua_setfield(L, -2, "first");
    lua_pushinteger(L, last); lua_setfield(L, -2, "last");
    lua_pushinteger(L, offset); lua_setfield(L, -2, "offset");
    return 1;
}

static int lua_UiSetScrollOffset(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    const int offset = std::max(
        0, static_cast<int>(luaL_checkinteger(L, 2)));
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeSetScrollOffset(
            BoundWidgetId(L), id ? id : "", offset);
    return 0;
}

static bool RequirePermission(lua_State* L, const char* permission)
{
    auto* s = GetD2D(L);
    if (!s || !s->engine) return false;
    if (s->engine->RuntimeHasPermission(BoundWidgetId(L), permission))
        return true;
    std::string msg = std::string("Permission denied: ") + permission;
    s->engine->RuntimeRecordError(BoundWidgetId(L), msg);
    luaL_error(L, "%s", msg.c_str());
    return false;
}

static void PushDesktopItem(lua_State* L, const LuaDesktopItemInfo& item)
{
    lua_createtable(L, 0, 6);
    lua_pushstring(L, item.id.c_str()); lua_setfield(L, -2, "id");
    lua_pushstring(L, item.title.c_str()); lua_setfield(L, -2, "title");
    lua_pushstring(L, item.path.c_str()); lua_setfield(L, -2, "path");
    lua_pushstring(L, item.source.c_str()); lua_setfield(L, -2, "source");
    lua_pushstring(L, item.type.c_str()); lua_setfield(L, -2, "type");
    lua_pushboolean(L, item.selected); lua_setfield(L, -2, "selected");
}

static std::wstring ReadLuaPathArg(lua_State* L, int index)
{
    if (lua_istable(L, index))
    {
        lua_getfield(L, index, "path");
        const char* path = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        std::wstring result = Utf8ToWideLocal(path ? path : "");
        lua_pop(L, 1);
        return result;
    }
    const char* path = luaL_checkstring(L, index);
    return Utf8ToWideLocal(path ? path : "");
}

static int lua_WidgetInfo(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_createtable(L, 0, 5);
    if (!s)
        return 1;
    lua_pushstring(L, WidgetWideToUtf8(BoundWidgetId(L)).c_str()); lua_setfield(L, -2, "id");
    lua_pushnumber(L, s->widgetRect.right - s->widgetRect.left); lua_setfield(L, -2, "width");
    lua_pushnumber(L, s->widgetRect.bottom - s->widgetRect.top); lua_setfield(L, -2, "height");
    lua_pushboolean(L, s->engine &&
        s->engine->RuntimeIsWidgetSelected(BoundWidgetId(L)));
    lua_setfield(L, -2, "selected");
    const std::wstring selectedPackageId =
        s->engine
            ? s->engine->RuntimeSelectedWidgetPackageId()
            : std::wstring{};
    lua_pushstring(
        L, WidgetWideToUtf8(selectedPackageId).c_str());
    lua_setfield(L, -2, "selectedPackageId");
    return 1;
}

static int lua_WidgetSetTitle(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeSetWidgetTitle(BoundWidgetId(L), Utf8ToWideLocal(title ? title : ""));
    return 0;
}

static int lua_WidgetOpenSettings(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetSettings(BoundWidgetId(L));
    return 0;
}

static int lua_WidgetInvalidate(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeInvalidateHost(BoundWidgetId(L));
    return 0;
}

static int lua_WidgetLog(lua_State* L)
{
    const char* level = luaL_optstring(L, 1, "info");
    const char* message = luaL_optstring(L, 2, "");
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeAddLog(BoundWidgetId(L), level ? level : "info", message ? message : "");
    return 0;
}

static int lua_WidgetTheme(lua_State* L)
{
    auto* s = GetD2D(L);
    LuaWidgetTheme theme;
    if (s && s->engine)
        theme = s->engine->RuntimeGetWidgetTheme(BoundWidgetId(L));
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, theme.bg); lua_setfield(L, -2, "bg");
    lua_pushinteger(L, theme.border); lua_setfield(L, -2, "border");
    lua_pushnumber(L, theme.alpha); lua_setfield(L, -2, "alpha");
    lua_pushnumber(L, theme.borderAlpha); lua_setfield(L, -2, "borderAlpha");
    lua_pushnumber(L, theme.gradientEndA); lua_setfield(L, -2, "gradientEndA");
    lua_pushnumber(L, theme.cornerRadius); lua_setfield(L, -2, "cornerRadius");
    lua_pushinteger(L, theme.contentTheme); lua_setfield(L, -2, "contentTheme");
    return 1;
}

static int lua_WidgetEditText(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    int x = static_cast<int>(luaL_checknumber(L, 2));
    int y = static_cast<int>(luaL_checknumber(L, 3));
    int w = static_cast<int>(luaL_checknumber(L, 4));
    int h = static_cast<int>(luaL_checknumber(L, 5));
    bool multiline = lua_toboolean(L, 6) != 0;
    auto* s = GetD2D(L);
    if (!s || !s->engine || !key || !*key)
        return 0;

    std::string initial;
    if (lua_isstring(L, 7))
        initial = lua_tostring(L, 7);
    else
        initial = s->engine->RuntimeGetStorageValue(BoundWidgetId(L), key);

    LuaInlineTextEditRequest request;
    request.widgetId = BoundWidgetId(L);
    request.storageKey = key;
    request.text = initial;
    request.localRect = { x, y, x + std::max(1, w), y + std::max(1, h) };
    request.multiline = multiline;
    request.selectAll = lua_isnil(L, 8) ? true : (lua_toboolean(L, 8) != 0);
    request.textColor = static_cast<int>(luaL_optinteger(L, 9, 0x000000));
    request.fontSize = static_cast<float>(luaL_optnumber(L, 10, 15.0));
    request.backgroundColor = static_cast<int>(luaL_optinteger(L, 11, 0xFFFFFF));
    s->engine->RuntimeBeginInlineTextEdit(request);
    return 0;
}

static int lua_DesktopItems(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopItems();
    lua_createtable(L, static_cast<int>(items.size()), 0);
    int i = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int lua_DesktopSelection(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopSelection();
    lua_createtable(L, static_cast<int>(items.size()), 0);
    int i = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int lua_DesktopFind(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    std::wstring query = Utf8ToWideLocal(queryRaw ? queryRaw : "");
    const int requestedLimit = static_cast<int>(
        luaL_optinteger(L, 2, 0));
    const size_t resultLimit = requestedLimit <= 0
        ? std::numeric_limits<size_t>::max()
        : static_cast<size_t>(std::clamp(requestedLimit, 1, 1000));

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopItems();
    if (query.empty())
    {
        const size_t count = std::min(items.size(), resultLimit);
        lua_createtable(L, static_cast<int>(count), 0);
        for (size_t index = 0; index < count; ++index)
        {
            PushDesktopItem(L, items[index]);
            lua_rawseti(L, -2, static_cast<int>(index + 1));
        }
        return 1;
    }

    std::array<std::vector<LuaDesktopItemInfo>,
        kNameSearchNoMatchRank> buckets;
    for (auto& item : items)
    {
        const int rank = NameSearchMatchRank(
            Utf8ToWideLocal(item.title), query);
        if (rank >= 0 && rank < kNameSearchNoMatchRank)
            buckets[static_cast<size_t>(rank)].push_back(
                std::move(item));
    }

    size_t resultCount = 0;
    for (const auto& bucket : buckets)
        resultCount += bucket.size();
    resultCount = std::min(resultCount, resultLimit);
    lua_createtable(L, static_cast<int>(resultCount), 0);
    int i = 1;
    for (const auto& bucket : buckets)
    {
        for (const auto& item : bucket)
        {
            if (static_cast<size_t>(i) > resultCount)
                return 1;
            PushDesktopItem(L, item);
            lua_rawseti(L, -2, i++);
        }
    }
    return 1;
}

static int lua_DesktopFindApplications(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    const std::string query = queryRaw ? queryRaw : "";
    int maxResults = static_cast<int>(
        luaL_optinteger(L, 2, 40));
    maxResults = std::clamp(maxResults, 1, 200);

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s && s->engine
        ? s->engine->RuntimeApplicationSearch(
            query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
    lua_createtable(
        L, static_cast<int>(items.size()), 0);
    int index = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

static void PushCalendarEvent(
    lua_State* L,
    const snowdesktop::calendar::CalendarEvent& event)
{
    lua_createtable(L, 0, 10);
    lua_pushlstring(
        L, event.id.data(), event.id.size());
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, event.revision);
    lua_setfield(L, -2, "revision");
    lua_pushlstring(
        L, event.title.data(), event.title.size());
    lua_setfield(L, -2, "title");
    lua_pushlstring(
        L, event.date.data(), event.date.size());
    lua_setfield(L, -2, "date");
    lua_pushboolean(L, event.allDay);
    lua_setfield(L, -2, "allDay");
    lua_pushinteger(L, event.startMinutes);
    lua_setfield(L, -2, "startMinutes");
    lua_pushinteger(L, event.endMinutes);
    lua_setfield(L, -2, "endMinutes");
    lua_pushlstring(
        L, event.notes.data(), event.notes.size());
    lua_setfield(L, -2, "notes");
    lua_pushinteger(L, event.reminderMinutes);
    lua_setfield(L, -2, "reminderMinutes");
}

static snowdesktop::calendar::CalendarEvent
ReadCalendarEvent(lua_State* L, int index)
{
    const int table = lua_absindex(L, index);
    luaL_checktype(L, table, LUA_TTABLE);
    snowdesktop::calendar::CalendarEvent event;
    auto readString = [&](const char* field) {
        lua_getfield(L, table, field);
        std::string result;
        if (lua_isstring(L, -1))
        {
            size_t length = 0;
            const char* value =
                lua_tolstring(L, -1, &length);
            if (value)
                result.assign(value, length);
        }
        lua_pop(L, 1);
        return result;
    };
    auto readInteger = [&](const char* field, int fallback) {
        lua_getfield(L, table, field);
        const int result = lua_isinteger(L, -1)
            ? static_cast<int>(lua_tointeger(L, -1))
            : fallback;
        lua_pop(L, 1);
        return result;
    };
    event.title = readString("title");
    event.date = readString("date");
    lua_getfield(L, table, "allDay");
    event.allDay =
        lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    event.startMinutes =
        readInteger("startMinutes", 0);
    event.endMinutes =
        readInteger("endMinutes", 0);
    event.notes = readString("notes");
    event.reminderMinutes =
        readInteger("reminderMinutes", -1);
    return event;
}

static void PushCalendarMutation(
    lua_State* L,
    const snowdesktop::calendar::MutationResult& result)
{
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, result.ok);
    lua_setfield(L, -2, "ok");
    lua_pushlstring(
        L, result.id.data(), result.id.size());
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, result.revision);
    lua_setfield(L, -2, "revision");
    lua_pushlstring(
        L, result.error.data(), result.error.size());
    lua_setfield(L, -2, "error");
}

static int lua_CalendarSelectedDate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    auto* state = GetD2D(L);
    const std::string date =
        state && state->engine
        ? state->engine->RuntimeCalendarSelectedDate()
        : std::string();
    lua_pushlstring(L, date.data(), date.size());
    return 1;
}

static int lua_CalendarSetSelectedDate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    size_t length = 0;
    const char* value =
        luaL_checklstring(L, 1, &length);
    auto* state = GetD2D(L);
    lua_pushboolean(
        L, state && state->engine &&
            state->engine->RuntimeCalendarSetSelectedDate(
                std::string(value, length)));
    return 1;
}

static int lua_CalendarDateInfo(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    const char* value = luaL_checkstring(L, 1);
    auto* state = GetD2D(L);
    const auto info = state && state->engine
        ? state->engine->RuntimeCalendarDateInfo(value)
        : std::nullopt;
    if (!info)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, info->year);
    lua_setfield(L, -2, "year");
    lua_pushinteger(L, info->month);
    lua_setfield(L, -2, "month");
    lua_pushinteger(L, info->day);
    lua_setfield(L, -2, "day");
    lua_pushinteger(L, info->weekday);
    lua_setfield(L, -2, "weekday");
    lua_pushinteger(L, info->daysInMonth);
    lua_setfield(L, -2, "daysInMonth");
    return 1;
}

static int lua_CalendarAddDays(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    const char* value = luaL_checkstring(L, 1);
    const int offset = static_cast<int>(
        luaL_checkinteger(L, 2));
    auto* state = GetD2D(L);
    const auto result = state && state->engine
        ? state->engine->RuntimeCalendarAddDays(
            value, offset)
        : std::nullopt;
    if (!result)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(
        L, result->data(), result->size());
    return 1;
}

static int lua_CalendarEvents(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    const char* fromDate =
        luaL_checkstring(L, 1);
    const char* toDate =
        luaL_checkstring(L, 2);
    auto* state = GetD2D(L);
    const auto events = state && state->engine
        ? state->engine->RuntimeCalendarEvents(
            fromDate, toDate)
        : std::vector<
            snowdesktop::calendar::CalendarEvent>{};
    lua_createtable(
        L, static_cast<int>(events.size()), 0);
    int index = 1;
    for (const auto& event : events)
    {
        PushCalendarEvent(L, event);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

static int lua_CalendarCreate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    auto* state = GetD2D(L);
    const auto result = state && state->engine
        ? state->engine->RuntimeCalendarCreate(
            ReadCalendarEvent(L, 1))
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
    PushCalendarMutation(L, result);
    return 1;
}

static int lua_CalendarUpdate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    const char* id = luaL_checkstring(L, 1);
    const int revision = static_cast<int>(
        luaL_checkinteger(L, 2));
    auto* state = GetD2D(L);
    const auto result = state && state->engine
        ? state->engine->RuntimeCalendarUpdate(
            id, revision,
            ReadCalendarEvent(L, 3))
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
    PushCalendarMutation(L, result);
    return 1;
}

static int lua_CalendarRemove(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    const char* id = luaL_checkstring(L, 1);
    auto* state = GetD2D(L);
    const auto result = state && state->engine
        ? state->engine->RuntimeCalendarRemove(id)
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
    PushCalendarMutation(L, result);
    return 1;
}

static int lua_EverythingSearch(lua_State* L)
{
    if (!RequirePermission(L, "everything.search")) return 0;
    const char* queryRaw = luaL_optstring(L, 1, "");
    std::string query = queryRaw ? queryRaw : "";
    int maxResults = static_cast<int>(luaL_optinteger(L, 2, 40));
    maxResults = std::clamp(maxResults, 1, 200);

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s && s->engine
        ? s->engine->RuntimeEverythingSearch(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
    lua_createtable(L, static_cast<int>(items.size()), 0);
    int i = 1;
    for (const auto& item : items)
    {
        PushDesktopItem(L, item);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int lua_DesktopOpen(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeOpenDesktopPath(ReadLuaPathArg(L, 1)));
    return 1;
}

static int lua_DesktopReveal(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeRevealDesktopPath(ReadLuaPathArg(L, 1)));
    return 1;
}

static int lua_DesktopRefresh(lua_State* L)
{
    if (!RequirePermission(L, "desktop.action")) return 0;
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeRefreshDesktop();
    return 0;
}

static int lua_DrawStrokeRect(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 1));
    float y = static_cast<float>(luaL_checknumber(L, 2));
    float w = static_cast<float>(luaL_checknumber(L, 3));
    float h = static_cast<float>(luaL_checknumber(L, 4));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));
    float radius = static_cast<float>(luaL_optnumber(L, 6, 0));
    float thickness = static_cast<float>(luaL_optnumber(L, 7, 1.0));
    float alpha = static_cast<float>(luaL_optnumber(L, 8, 1.0));
    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;
    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;
    D2D1_RECT_F rect = { x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h };
    if (radius > 0)
        s->ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, thickness);
    else
        s->ctx->DrawRectangle(rect, brush, thickness);
    return 0;
}

static int lua_WidgetOpenPanel(lua_State* L)
{
    std::string title;
    int width = 520;
    int height = 620;
    if (lua_istable(L, 1))
    {
        title = LuaReadStorageField(L, 1, "title");
        lua_getfield(L, 1, "width");
        if (lua_isnumber(L, -1))
            width = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 1, "height");
        if (lua_isnumber(L, -1))
            height = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetPanel(
            BoundWidgetId(L), Utf8ToWideLocal(title),
            width, height);
    return 0;
}

static int lua_WidgetClosePanel(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeCloseWidgetPanel(
            BoundWidgetId(L));
    return 0;
}

static void EnsureBitmapCachesForCurrentDevice(D2DState* state)
{
    if (!state || !state->ctx) return;
    ComPtr<ID2D1Device> device;
    state->ctx->GetDevice(&device);
    if (state->bitmapDevice.Get() == device.Get()) return;
    state->bitmapDevice = std::move(device);
    state->imageCache.clear();
    state->shellIconCache.clear();
    state->shellIconFailures.clear();
}

static ID2D1Bitmap1* LoadImageBitmap(D2DState* s, const std::wstring& path)
{
    if (!s || !s->ctx || path.empty()) return nullptr;
    EnsureBitmapCachesForCurrentDevice(s);
    auto it = s->imageCache.find(path);
    if (it != s->imageCache.end()) return it->second.Get();

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory))) || !factory)
        return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder)
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter)
        return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(s->ctx->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap)) || !bitmap)
        return nullptr;
    ID2D1Bitmap1* result = bitmap.Get();
    if (s->imageCache.size() >= 128)
        s->imageCache.clear();
    s->imageCache[path] = bitmap;
    return result;
}

static int lua_DrawImage(lua_State* L)
{
    const char* pathRaw = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float w = static_cast<float>(luaL_checknumber(L, 4));
    float h = static_cast<float>(luaL_checknumber(L, 5));
    float alpha = static_cast<float>(luaL_optnumber(L, 6, 1.0));
    auto* s = GetD2D(L);
    std::wstring path = Utf8ToWideLocal(pathRaw ? pathRaw : "");
    if (!s || !s->engine || path.empty())
        return 0;
    const auto fullPath = s->engine->RuntimeResolvePackageAsset(
        BoundWidgetId(L), path);
    if (!fullPath) return 0;
    ID2D1Bitmap1* bmp = LoadImageBitmap(s, *fullPath);
    if (!s || !s->ctx || !bmp) return 0;
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h);
    s->ctx->DrawBitmap(bmp, dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

static ComPtr<ID2D1Bitmap1> BitmapFromHBitmap(ID2D1DeviceContext* ctx, HBITMAP hbm)
{
    ComPtr<ID2D1Bitmap1> result;
    if (!ctx || !hbm) return result;
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || !bm.bmBits) return result;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(ctx->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(bm.bmWidth), static_cast<UINT32>(bm.bmHeight)),
        bm.bmBits, static_cast<UINT32>(bm.bmWidthBytes), &props, &result)))
        result.Reset();
    return result;
}

static void DrainShellIconResults(D2DState* state)
{
    if (!state || !state->ctx || !state->shellIconLoader)
        return;
    EnsureBitmapCachesForCurrentDevice(state);
    for (auto& result : state->shellIconLoader->Drain())
    {
        if (!result.bitmap)
        {
            state->shellIconFailures.insert(result.path);
            continue;
        }
        ComPtr<ID2D1Bitmap1> bitmap =
            BitmapFromHBitmap(state->ctx, result.bitmap);
        DeleteObject(result.bitmap);
        result.bitmap = nullptr;
        if (!bitmap)
        {
            state->shellIconFailures.insert(result.path);
            continue;
        }
        if (state->shellIconCache.size() >= 512)
            state->shellIconCache.clear();
        state->shellIconCache[result.path] = std::move(bitmap);
    }
}

static std::wstring ReadLuaPreviewIconTitle(
    lua_State* state, int index, const std::wstring& fallback)
{
    if (lua_istable(state, index))
    {
        const int table = lua_absindex(state, index);
        lua_getfield(state, table, "title");
        const char* title = lua_isstring(state, -1)
            ? lua_tostring(state, -1) : nullptr;
        const std::wstring result = Utf8ToWideLocal(title ? title : "");
        lua_pop(state, 1);
        if (!result.empty()) return result;
    }
    std::wstring result = fallback;
    const size_t separator = result.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
        result.erase(0, separator + 1);
    const size_t dot = result.find(L'.');
    if (dot != std::wstring::npos) result.erase(dot);
    return result;
}

static void DrawSimulatedPreviewIcon(D2DState* state,
    const std::wstring& identity, const std::wstring& title,
    float x, float y, float size, float alpha)
{
    if (!state || !state->ctx || size <= 0.0f) return;
    static constexpr int colors[] = {
        0x3B82F6, 0x8B5CF6, 0xEC4899, 0xF97316,
        0x14B8A6, 0x22C55E, 0xEAB308, 0x6366F1,
    };
    std::uint32_t hash = 2166136261u;
    for (wchar_t value : identity)
    {
        hash ^= static_cast<std::uint32_t>(value);
        hash *= 16777619u;
    }
    const int background = colors[hash % std::size(colors)];
    ID2D1SolidColorBrush* fill = GetCachedBrush(
        state, background, std::clamp(alpha, 0.0f, 1.0f));
    ID2D1SolidColorBrush* border = GetCachedBrush(
        state, 0xFFFFFF, std::clamp(alpha * 0.42f, 0.0f, 1.0f));
    ID2D1SolidColorBrush* text = GetCachedBrush(
        state, 0xFFFFFF, std::clamp(alpha, 0.0f, 1.0f));
    const D2D1_RECT_F rect = D2D1::RectF(
        x + state->widgetRect.left, y + state->widgetRect.top,
        x + state->widgetRect.left + size,
        y + state->widgetRect.top + size);
    const float radius = std::max(3.0f, size * 0.22f);
    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(rect, radius, radius);
    if (fill) state->ctx->FillRoundedRectangle(rounded, fill);
    if (border)
        state->ctx->DrawRoundedRectangle(
            rounded, border, std::max(1.0f, size / 24.0f));

    std::wstring letter;
    for (wchar_t value : title)
    {
        if (value != L' ' && value != L'\t' &&
            value != L'\r' && value != L'\n')
        {
            letter.assign(1, value);
            break;
        }
    }
    if (letter.empty()) letter = L"?";
    CharUpperBuffW(letter.data(), static_cast<DWORD>(letter.size()));
    IDWriteTextFormat* format = GetCachedTextFormat(
        state, std::max(11.0f, size * 0.46f),
        DWRITE_FONT_WEIGHT_BOLD, true,
        DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
    if (format && text)
    {
        state->ctx->DrawTextW(letter.c_str(),
            static_cast<UINT32>(letter.size()), format,
            rect, text, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

static int lua_DrawIcon(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    std::wstring path = ReadLuaPathArg(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float size = static_cast<float>(luaL_optnumber(L, 4, 32));
    float alpha = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    auto* s = GetD2D(L);
    if (!s || !s->ctx || path.empty()) return 0;

    if (s->engine && s->engine->IsPreviewOnly())
    {
        DrawSimulatedPreviewIcon(s, path,
            ReadLuaPreviewIconTitle(L, 1, path),
            x, y, size, alpha);
        return 0;
    }

    EnsureBitmapCachesForCurrentDevice(s);
    if (s->shellIconFailures.contains(path))
        return 0;
    auto cached = s->shellIconCache.find(path);
    if (cached == s->shellIconCache.end())
    {
        if (s->shellIconLoader)
            s->shellIconLoader->Request(
                path, BoundWidgetId(L));
        return 0;
    }
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + size, y + s->widgetRect.top + size);
    s->ctx->DrawBitmap(
        cached->second.Get(), dst, alpha,
        D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

// ── WidgetEngine ──────────────────────────────────────────────────
static void* LuaQuotaAllocator(void* userData, void* pointer,
    size_t oldSize, size_t newSize)
{
    auto* quota = static_cast<LuaRuntimeQuota*>(userData);
    if (!quota) return nullptr;
    if (newSize == 0)
    {
        if (pointer)
            quota->memoryBytes = oldSize > quota->memoryBytes
                ? 0 : quota->memoryBytes - oldSize;
        std::free(pointer);
        return nullptr;
    }
    const size_t current = pointer ? oldSize : 0;
    const size_t withoutCurrent = current > quota->memoryBytes
        ? 0 : quota->memoryBytes - current;
    if (newSize > quota->memoryLimit ||
        withoutCurrent > quota->memoryLimit - newSize)
    {
        quota->memoryExceeded = true;
        return nullptr;
    }
    void* result = std::realloc(pointer, newSize);
    if (result)
        quota->memoryBytes = withoutCurrent + newSize;
    return result;
}

WidgetEngine::~WidgetEngine()
{
    Shutdown();
}

bool WidgetEngine::Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory)
{
    previewOnly_ = false;
    d2dContext_ = d2dContext;
    dwriteFactory_ = dwriteFactory;

    // Allocate D2D state
    d2dState_ = new D2DState{};
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    d2dState_->shellIconLoader =
        std::make_unique<AsyncShellIconLoader>(
            [this](const std::wstring& widgetId) {
                RuntimeInvalidateHost(widgetId);
            });

    // Initialize the package registry before loading layouts or menus.
    (void)GetWidgetPackageManager();

    // Init storage path
    g_storagePath = GetDataFilePath(L"SnowDesktop.storage.json");
    LoadStorageFile();
    calendarService_ =
        std::make_unique<
            snowdesktop::calendar::CalendarService>(
            GetDataFilePath(
                L"SnowDesktop.calendar.json"));
    calendarService_->SetChangedCallback(
        [this](const std::string& reason) {
            if (reason == "selection")
                pendingCalendarSelectionChange_ = true;
            else if (reason == "events")
                pendingCalendarEventsChange_ = true;
        });
    calendarService_->SetNotificationCallback(
        [this](
            const snowdesktop::calendar::CalendarEvent& event) {
            if (!notifyCallback_)
                return;
            notifyCallback_(
                _LW("calendar.notification_title"),
                Utf8ToWideLocal(event.title));
        });
    (void)calendarService_->Load();
    systemSnapshotService_ = std::make_unique<SystemSnapshotService>();
    httpService_ = std::make_unique<AsyncHttpService>();
    return true;
}

bool WidgetEngine::InitPreview(
    ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory)
{
    previewOnly_ = true;
    d2dContext_ = d2dContext;
    dwriteFactory_ = dwriteFactory;
    d2dState_ = new D2DState{};
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    // Package metadata and Lua files remain readable.  Shell icon loading,
    // storage, calendar, system snapshots and HTTP workers are deliberately
    // absent in this render-only engine.
    (void)GetWidgetPackageManager();
    return d2dState_ != nullptr;
}

void WidgetEngine::Shutdown()
{
    focusedHostInput_ = {};
    if (d2dState_)
        d2dState_->shellIconLoader.reset();
    for (auto& widget : widgets_)
    {
        if (widget.valid && widget.hostVisible)
            InvokeSimpleCallback(widget, "onHidden");
        if (widget.refreshTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.refreshTimerId);
        if (widget.namedTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.namedTimerId);
    }
    if (systemSnapshotService_)
    {
        systemSnapshotService_->Stop();
        systemSnapshotService_.reset();
        systemSnapshotServiceStarted_ = false;
    }
    if (httpService_)
    {
        httpService_->Stop();
        httpService_.reset();
    }
    calendarService_.reset();
    pendingCalendarSelectionChange_ = false;
    pendingCalendarEventsChange_ = false;
    for (auto& widget : widgets_)
    {
        if (widget.state)
        {
            lua_close(widget.state);
            widget.state = nullptr;
        }
    }
    widgets_.clear();
    delete d2dState_; d2dState_ = nullptr;
}

void WidgetEngine::UnloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    PreviewExecutionScope previewScope(
        widgets_[idx].preview ? &widgets_[idx].previewStorage : nullptr);
    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId)
        focusedHostInput_ = {};
    if (widgets_[idx].hostVisible)
        InvokeSimpleCallback(widgets_[idx], "onHidden");
    if (widgets_[idx].refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].refreshTimerId);
    if (widgets_[idx].namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].namedTimerId);
    if (httpService_) httpService_->CancelWidget(widgetId);
    lua_State* state = widgets_[idx].state;
    if (state)
    {
        luaL_unref(state, LUA_REGISTRYINDEX, widgets_[idx].ref);
        lua_close(state);
        widgets_[idx].state = nullptr;
    }
    widgets_.erase(widgets_.begin() + idx);
    std::erase_if(widgets_, [&widgetId](const LuaWidget& widget) {
        return widget.widgetId == widgetId;
    });

}

void WidgetEngine::DeleteWidgetInstance(const std::wstring& widgetId)
{
    UnloadWidget(widgetId);
    std::string prefix = WidgetWideToUtf8(widgetId) + ".";
    auto& storage = ActiveStorage();
    auto it = storage.begin();
    while (it != storage.end())
    {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
            it = storage.erase(it);
        else
            ++it;
    }
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
}

int WidgetEngine::FindWidget(const std::wstring& widgetId) const
{
    for (int i = 0; i < (int)widgets_.size(); ++i)
    {
        if (widgets_[i].widgetId == widgetId)
            return i;
    }
    return -1;
}

void WidgetEngine::ActivateWidgetState(const std::wstring& widgetId)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].state)
        return;
    const auto& widget = widgets_[index];
    if (d2dState_)
    {
        snowdesktop::widget_runtime::ApplyLayoutMetrics(
            *d2dState_, widget.layoutMetrics);
    }
}

void WidgetEngine::InvokeSimpleCallback(LuaWidget& widget, const char* callbackName)
{
    lua_State* state = widget.state;
    if (!state) return;
    const std::wstring widgetId = widget.widgetId;
    const int widgetRef = widget.ref;
    const RECT bounds = widget.lastBounds;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, bounds);
    lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return; }
    lua_getfield(state, -1, callbackName);
    if (lua_isfunction(state, -1))
    {
        if (snowdesktop::lua_runtime::ProtectedCall(state, 0, 0) != LUA_OK)
        {
            const char* error = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, error ? error : "(callback error)");
            lua_pop(state, 1);
        }
    }
    else
        lua_pop(state, 1);
    lua_pop(state, 1);
}

static int LuaReadOnlyApiWrite(lua_State* state)
{
    return luaL_error(state, "SnowDesktop host API tables are read-only");
}

static void PushReadOnlyProxyForTopTable(lua_State* state)
{
    // Stack before: [..., source]. Stack after: [..., proxy].
    lua_newtable(state);
    lua_newtable(state);
    lua_pushvalue(state, -3);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, LuaReadOnlyApiWrite);
    lua_setfield(state, -2, "__newindex");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);
    lua_remove(state, -2);
}

static void PushReadOnlyGlobal(lua_State* state, const char* name)
{
    lua_getglobal(state, name);
    PushReadOnlyProxyForTopTable(state);
}

void WidgetEngine::PushSafeEnvironment(lua_State* L, const LuaWidget& widget)
{
    static const char* funcs[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "select",
        "tonumber", "tostring", "type", "xpcall"
    };

    lua_newtable(L);
    for (const char* name : funcs)
    {
        lua_getglobal(L, name);
        lua_setfield(L, -2, name);
    }
    for (const char* name : { "string", "table", "math", "utf8", "draw",
        "sys", "layout", "storage", "widget", "desktop", "media", "http",
        "ui", "everything", "calendar" })
    {
        PushReadOnlyGlobal(L, name);
        lua_setfield(L, -2, name);
    }
    PushWidgetL10nAPI(L, widget.manifest);
    PushReadOnlyProxyForTopTable(L);
    lua_setfield(L, -2, "l10n");

    if (widget.permissions.contains("ui.input"))
    {
        PushReadOnlyGlobal(L, "imgui");
        lua_setfield(L, -2, "imgui");
    }

    lua_pushstring(L, WidgetWideToUtf8(widget.widgetId).c_str());
    lua_setfield(L, -2, "widgetId");
}

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& packageId)
{
    if (FindWidget(widgetId) >= 0) return true;
    const std::wstring path = ResolveWidgetPath(packageId);
    if (path.empty())
    {
        RuntimeRecordError(widgetId,
            "Widget package is missing, disabled, or has not been migrated");
        return false;
    }
    if (LoadWidget(path, widgetId))
        return true;
    const std::string id = WidgetWideToUtf8(packageId);
    if (!RecoverWidgetPackage(id))
        return false;
    const std::wstring fallback = ResolveWidgetPath(packageId);
    return !fallback.empty() && fallback != path &&
        LoadWidget(fallback, widgetId);
}

bool WidgetEngine::EnsureWidgetPreviewLoaded(
    const std::wstring& widgetId, const std::wstring& packageId,
    const std::unordered_map<std::string, std::string>& storageOverrides)
{
    if (int index = FindWidget(widgetId); index >= 0)
        return widgets_[index].preview &&
            widgets_[index].packageId == WidgetWideToUtf8(packageId);
    const std::wstring path = ResolveWidgetPath(packageId);
    return !path.empty() && LoadWidget(
        path, widgetId, true, &storageOverrides);
}

bool WidgetEngine::IsPreviewWidget(const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && widgets_[index].preview;
}

bool WidgetEngine::LoadWidget(const std::wstring& path,
    const std::wstring& widgetId, bool preview,
    const std::unordered_map<std::string, std::string>*
        previewStorageOverrides)
{
    std::string source = ReadTextFile(path);
    if (source.empty()) return false;

    LuaWidget pending;
    pending.widgetId = widgetId;
    pending.filePath = path;
    pending.manifest = GetWidgetManifest(path);
    pending.packageId = pending.manifest.packageId;
    pending.preview = preview;
    if (preview)
    {
        const std::string prefix = WidgetWideToUtf8(widgetId) + ".";
        for (const auto& [key, value] : pending.manifest.previewStorage)
            pending.previewStorage[prefix + key] = value;
        if (previewStorageOverrides)
        {
            for (const auto& [key, value] : *previewStorageOverrides)
                pending.previewStorage[prefix + key] = value;
        }
    }
    PreviewExecutionScope previewScope(
        preview ? &pending.previewStorage : nullptr);
    if (const auto package =
        GetWidgetPackageManager().ResolveEntryPath(path))
        pending.packageRoot = package->root;
    else
        pending.packageRoot = std::filesystem::path(path).parent_path();
    if (pending.packageId.empty())
    {
        RuntimeRecordError(widgetId, "Legacy loose Lua scripts cannot run directly");
        return false;
    }
    if (!pending.manifest.signatureValid)
    {
        RuntimeRecordError(widgetId, "Widget signature validation failed");
        return false;
    }
    if (!pending.manifest.minHostVersion.empty() &&
        CompareVersions(SNOWDESKTOP_VERSION, pending.manifest.minHostVersion) < 0)
    {
        RuntimeRecordError(widgetId, "Widget requires SnowDesktop " +
            pending.manifest.minHostVersion + " or newer");
        return false;
    }
    if (const auto package =
        GetWidgetPackageManager().Resolve(pending.packageId))
    {
        switch (snowdesktop::widget::PermissionRuntimeBlockFor(
            package->permissionState))
        {
        case snowdesktop::widget::PermissionRuntimeBlock::PendingConsent:
            RuntimeRecordError(widgetId,
                "Widget permission consent is pending");
            return false;
        case snowdesktop::widget::PermissionRuntimeBlock::Denied:
            RuntimeRecordError(widgetId,
                "Widget permission consent was denied");
            return false;
        case snowdesktop::widget::PermissionRuntimeBlock::None:
            break;
        }
        const std::set<std::string> grantedPermissions(
            package->grantedPermissions.begin(),
            package->grantedPermissions.end());
        for (const auto& permission : pending.manifest.permissions)
            if (grantedPermissions.contains(permission))
                pending.permissions.insert(permission);
    }
    else
    {
        for (const auto& permission : pending.manifest.permissions)
            pending.permissions.insert(permission);
    }

    WidgetExecutionContextGuard loadContext(d2dState_, widgetId);
    auto quota = std::make_unique<LuaRuntimeQuota>();
    lua_State* state = lua_newstate(LuaQuotaAllocator, quota.get());
    if (!state)
    {
        RuntimeRecordError(widgetId, "Cannot allocate isolated Lua state");
        return false;
    }
    std::unique_ptr<lua_State, decltype(&lua_close)> stateGuard(
        state, lua_close);
    luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
    RegisterDrawAPI(state);
    lua_pushlightuserdata(state, d2dState_);
    lua_setfield(state, LUA_REGISTRYINDEX, "__d2d_ptr");
    lua_pushlightuserdata(state, quota.get());
    lua_setfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    lua_pushstring(state, WidgetWideToUtf8(widgetId).c_str());
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_id");
    if (d2dState_)
    {
        d2dState_->currentWidgetId = widgetId;
        d2dState_->storagePrefix = WidgetWideToUtf8(widgetId);
    }
    // Create a sandbox table with only the registered safe API surface.
    PushSafeEnvironment(state, pending);

    // Load the chunk
    if (luaL_loadstring(state, source.c_str()) != LUA_OK)
    {
        const char* err = lua_tostring(state, -1);
        RuntimeRecordError(widgetId, err ? err : "(load error)");
        lua_pop(state, 2);
        return false;
    }

    // Try to set the chunk's first upvalue (_ENV) to sandbox.
    lua_pushvalue(state, -2);
    const char* envName = lua_setupvalue(state, -2, 1);
    if (envName == nullptr)
    {
        // Chunk has no _ENV upvalue - run directly in sandbox via alternative method
        // Pop the chunk, reload with explicit environment
        lua_pop(state, 2);  // pop sandbox copy and chunk, keep sandbox
        // Wrap the source to use the sandbox explicitly
        std::string wrapped = "local _ENV = ...;\n" + source;
        if (luaL_loadstring(state, wrapped.c_str()) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(load error)");
            lua_pop(state, 2);
            return false;
        }
        lua_pushvalue(state, -2);  // push sandbox as argument
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, 1, 0, 1000000,
                std::chrono::milliseconds(100)) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            lua_pop(state, 2);
            return false;
        }
    }
    else
    {
        // Execute the chunk
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, 0, 0, 1000000,
                std::chrono::milliseconds(100)) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            lua_pop(state, 2);
            return false;
        }
    }

    // sandbox now contains the script's globals (name, render, etc.)
    int ref = luaL_ref(state, LUA_REGISTRYINDEX);

    std::string name = "Unnamed";
    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    lua_getfield(state, -1, "name");
    if (lua_isstring(state, -1))
        name = lua_tostring(state, -1);
    lua_pop(state, 1);

    // Read customStyle flag
    bool customStyle = false;
    lua_getfield(state, -1, "useCustomStyle");
    if (!lua_isnil(state, -1))
        customStyle = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    bool followPersonalizationDefault = false;
    lua_getfield(state, -1, "followPersonalizationDefault");
    if (!lua_isnil(state, -1))
        followPersonalizationDefault = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    std::vector<LuaWidgetManifest::Setting> scriptSettings;
    std::vector<LuaWidgetManifest::SettingPreset> scriptPresets;
    ReadLuaDeclaredSettings(state, -1, scriptSettings, scriptPresets);

    const std::string storagePrefix = WidgetWideToUtf8(widgetId) + ".";
    const std::string dataVersionKey = storagePrefix + "__host.dataVersion";
    auto& activeStorage = ActiveStorage();
    int storedDataVersion = 1;
    if (const auto stored = activeStorage.find(dataVersionKey);
        stored != activeStorage.end())
        storedDataVersion = std::max(1, std::atoi(stored->second.c_str()));
    if (pending.manifest.dataVersion < storedDataVersion)
    {
        RuntimeRecordError(widgetId,
            "Widget package dataVersion is older than the instance storage");
        lua_pop(state, 1);
        return false;
    }
    if (pending.manifest.dataVersion > storedDataVersion)
    {
        std::unordered_map<std::string, std::string> migratedStorage =
            activeStorage;
        auto* parentOverlay =
            snowdesktop::widget_runtime::CurrentStorageOverlay();
        bool migrationFailed = false;
        std::string migrationFailure;
        {
            StorageOverlayScope migrationScope(&migratedStorage);
            lua_getfield(state, -1, "migrateStorage");
            if (lua_isfunction(state, -1))
            {
                lua_pushinteger(state, storedDataVersion);
                lua_pushinteger(state, pending.manifest.dataVersion);
                if (snowdesktop::lua_runtime::ProtectedCall(
                        state, 2, 0, 1000000,
                        std::chrono::milliseconds(100)) != LUA_OK)
                {
                    migrationFailed = true;
                    const char* migrationError = lua_tostring(state, -1);
                    migrationFailure = migrationError
                        ? migrationError : "(storage migration error)";
                    lua_pop(state, 1);
                }
            }
            else
                lua_pop(state, 1);
        }
        if (migrationFailed)
        {
            RuntimeRecordError(widgetId,
                "Widget storage migration failed: " + migrationFailure);
            lua_pop(state, 1);
            return false;
        }
        migratedStorage[dataVersionKey] =
            std::to_string(pending.manifest.dataVersion);
        if (parentOverlay)
            *parentOverlay = std::move(migratedStorage);
        else
        {
            g_storage = std::move(migratedStorage);
            SaveStorageFile();
        }
    }
    else if (!activeStorage.contains(dataVersionKey))
    {
        activeStorage[dataVersionKey] =
            std::to_string(pending.manifest.dataVersion);
        if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
    }

    lua_pop(state, 1);  // pop table

    LuaWidget w;
    w.widgetId = widgetId;
    w.packageId = pending.packageId;
    w.packageRoot = pending.packageRoot;
    w.state = stateGuard.release();
    w.quota = std::move(quota);
    w.name = name;
    w.filePath = path;
    w.manifest = pending.manifest;
    w.permissions = pending.permissions;
    w.ref = ref;
    w.valid = true;
    w.customStyle = customStyle;
    w.followPersonalizationDefault = followPersonalizationDefault;
    w.scriptSettings = std::move(scriptSettings);
    w.scriptPresets = std::move(scriptPresets);
    w.preview = preview;
    w.previewStorage = std::move(pending.previewStorage);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
    widgets_.push_back(std::move(w));

    auto& loadedStorage = ActiveStorage();
    const std::string staleErrorKey =
        WidgetWideToUtf8(widgetId) + ".lastError";
    if (loadedStorage.erase(staleErrorKey) > 0 &&
        !snowdesktop::widget_runtime::HasStorageOverlay())
    {
        SaveStorageFile();
    }

    if (!preview && widgets_.back().manifest.refreshIntervalMs > 0 &&
        widgetTimerRequestCallback_)
    {
        UINT_PTR tid = widgetTimerRequestCallback_(widgetId,
            static_cast<UINT>(widgets_.back().manifest.refreshIntervalMs));
        if (tid && !widgets_.empty())
        {
            auto& stored = widgets_.back();
            stored.refreshTimerId = tid;
        }
    }
    return true;
}

void WidgetEngine::RenderAll(ID2D1DeviceContext* context)
{
    d2dState_->ctx = context;
}

bool WidgetEngine::RenderWidgetEditor(const std::wstring& widgetId,
    const std::wstring& widgetName,
    PersonalizationSettings& mainPersonalization,
    bool& sharedGlassSettingsChanged,
    bool& sharedGlassSettingsSaveRequested)
{
    (void)widgetName;
    (void)mainPersonalization;
    sharedGlassSettingsChanged = false;
    sharedGlassSettingsSaveRequested = false;

    int idx = FindWidget(widgetId);
    int ref = (idx >= 0) ? widgets_[idx].ref : LUA_NOREF;
    if (ref == LUA_NOREF) return true;
    lua_State* state = widgets_[idx].state;
    if (!state) return true;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    const std::string editorId = WidgetWideToUtf8(widgetId);
    ImGui::PushID(editorId.c_str());

    const LuaWidget& widget = widgets_[idx];
    std::vector<LuaWidgetManifest::Setting> settings = widget.manifest.settings;
    settings.insert(settings.end(), widget.scriptSettings.begin(), widget.scriptSettings.end());
    std::vector<LuaWidgetManifest::SettingPreset> presets = widget.manifest.presets;
    presets.insert(presets.end(), widget.scriptPresets.begin(), widget.scriptPresets.end());

    const std::string prefix = WidgetWideToUtf8(widgetId) + ".";
    bool storageChanged = false;
    auto getStorage = [&](const std::string& key, const std::string& fallback = std::string{}) {
        auto it = g_storage.find(prefix + key);
        if (it != g_storage.end())
            return it->second;
        std::string defaultValue = RuntimeGetStorageValue(widgetId, key);
        return !defaultValue.empty() ? defaultValue : fallback;
    };
    auto setStorage = [&](const std::string& key, const std::string& value) {
        if (key.empty() || IsHostStructureSettingKey(key) ||
            IsHostSharedGlassSettingKey(key) ||
            IsRemovedPanelEffectSettingKey(key)) return;
        std::string fullKey = prefix + key;
        auto it = g_storage.find(fullKey);
        if (it != g_storage.end() && it->second == value) return;
        g_storage[fullKey] = value;
        storageChanged = true;
    };
    auto applyValues = [&](const std::unordered_map<std::string, std::string>& values) {
        for (const auto& kv : values)
            setStorage(kv.first, kv.second);
    };
    auto flushStorageChanges = [&]() {
        if (!storageChanged) return;
        SaveStorageFile();
        RuntimeInvalidateHost(widgetId);
        storageChanged = false;
    };
    auto whiteTextButton = [](const char* label) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        bool clicked = ImGui::Button(label);
        ImGui::PopStyleColor();
        return clicked;
    };
    auto colorToInt = [](float r, float g, float b) {
        auto toByte = [](float v) {
            return std::clamp(static_cast<int>(std::round(v * 255.0f)), 0, 255);
        };
        return (toByte(r) << 16) | (toByte(g) << 8) | toByte(b);
    };
    auto applyAppearance = [&](const PersonalizationSettings& appearance) {
        setStorage("bg", std::to_string(colorToInt(appearance.widgetBgR,
            appearance.widgetBgG, appearance.widgetBgB)));
        setStorage("border", std::to_string(colorToInt(appearance.widgetBorderR,
            appearance.widgetBorderG, appearance.widgetBorderB)));
        setStorage("alpha", std::to_string(appearance.widgetAlpha));
        setStorage("borderAlpha", std::to_string(appearance.widgetBorderAlpha));
        setStorage("gradientEndA", std::to_string(appearance.gradientEndA));
        setStorage("glassEnabled", appearance.glassEnabled ? "1" : "0");
        setStorage("acrylicEnabled", appearance.acrylicEnabled ? "1" : "0");
    };
    auto parseColor = [](const std::string& value, int fallback) {
        if (value.empty()) return fallback;
        char* end = nullptr;
        long parsed = std::strtol(value.c_str(), &end, 0);
        return end != value.c_str() ? static_cast<int>(parsed) : fallback;
    };
    auto beginEditorRow = [](const char* label, float requestedWidth) {
        const float rowStart = ImGui::GetCursorPosX();
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float rowRight = rowStart + availableWidth;
        const float maximumWidth = std::max(ImGui::GetFrameHeight(), availableWidth * 0.58f);
        const float controlWidth = std::min(requestedWidth, maximumWidth);
        const float controlX = std::max(rowStart, rowRight - controlWidth);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(controlX);
        return controlWidth;
    };
    auto editorCheckbox = [&](const char* label, const char* id, bool* value) {
        beginEditorRow(label, ImGui::GetFrameHeight());
        return ImGui::Checkbox(id, value);
    };
    auto editorButtonWidth = [](const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    };
    constexpr float kEditorControlWidth = 300.0f;
    constexpr float kEditorSliderWidth = kEditorControlWidth;
    auto renderSetting = [&](const LuaWidgetManifest::Setting& setting) {
        if (setting.key.empty() || IsHostStructureSettingKey(setting.key) ||
            IsHostAppearanceSettingKey(setting.key))
            return;
        std::string current = getStorage(setting.key, setting.defaultValue);
        std::string next = current;
        bool changed = false;
        ImGui::PushID(setting.key.c_str());
        if (setting.type == "bool")
        {
            bool value = current == "1" || current == "true";
            if (editorCheckbox(setting.label.c_str(), "##Value", &value))
            {
                next = value ? "1" : "0";
                changed = true;
            }
        }
        else if (setting.type == "int")
        {
            int value = std::atoi(current.c_str());
            ImGui::SetNextItemWidth(beginEditorRow(setting.label.c_str(), kEditorSliderWidth));
            if (ImGui::SliderInt("##Value", &value,
                static_cast<int>(setting.minValue), static_cast<int>(setting.maxValue)))
            {
                next = std::to_string(value);
                changed = true;
            }
        }
        else if (setting.type == "float")
        {
            float value = static_cast<float>(std::atof(current.c_str()));
            ImGui::SetNextItemWidth(beginEditorRow(setting.label.c_str(), kEditorSliderWidth));
            if (ImGui::SliderFloat("##Value", &value,
                static_cast<float>(setting.minValue), static_cast<float>(setting.maxValue)))
            {
                std::ostringstream ss;
                ss << value;
                next = ss.str();
                changed = true;
            }
        }
        else if (setting.type == "color")
        {
            int color = parseColor(current, parseColor(setting.defaultValue, 0xFFFFFF));
            float rgb[3] = {
                ((color >> 16) & 0xFF) / 255.0f,
                ((color >> 8) & 0xFF) / 255.0f,
                (color & 0xFF) / 255.0f
            };
            ImGui::SetNextItemWidth(beginEditorRow(setting.label.c_str(), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x));
            if (ImGui::ColorEdit3("##Value", rgb, ImGuiColorEditFlags_NoInputs))
            {
                next = std::to_string(colorToInt(rgb[0], rgb[1], rgb[2]));
                changed = true;
            }
        }
        else if (setting.type == "select" && !setting.options.empty())
        {
            int selected = 0;
            for (size_t i = 0; i < setting.options.size(); ++i)
                if (setting.options[i] == current) selected = static_cast<int>(i);
            std::vector<std::string> optionLabels;
            optionLabels.reserve(setting.options.size());
            for (size_t i = 0; i < setting.options.size(); ++i)
                optionLabels.push_back(setting.options[i] +
                    "###SettingOption" + std::to_string(i));
            std::vector<const char*> labels;
            labels.reserve(optionLabels.size());
            for (const auto& option : optionLabels)
                labels.push_back(option.c_str());
            ImGui::SetNextItemWidth(beginEditorRow(setting.label.c_str(), kEditorControlWidth));
            if (ImGui::Combo("##Value", &selected, labels.data(),
                static_cast<int>(labels.size())))
            {
                next = setting.options[selected];
                changed = true;
            }
        }
        else
        {
            char buffer[512]{};
            strncpy_s(buffer, current.c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(beginEditorRow(setting.label.c_str(), kEditorControlWidth));
            if (ImGui::InputText("##Value", buffer, sizeof(buffer)))
            {
                next = buffer;
                changed = true;
            }
        }
        ImGui::PopID();
        if (changed) setStorage(setting.key, next);
    };

    int defaultPresetIndex = -1;
    for (size_t i = 0; i < presets.size(); ++i)
    {
        if (presets[i].isDefault || presets[i].id == "default")
        {
            defaultPresetIndex = static_cast<int>(i);
            break;
        }
    }
    auto defaultPresetValues = [&]() -> const std::unordered_map<std::string, std::string>* {
        if (defaultPresetIndex < 0) return nullptr;
        return &presets[static_cast<size_t>(defaultPresetIndex)].values;
    };
    auto applyDefaultSettingValues = [&]() {
        const auto* values = defaultPresetValues();
        for (const auto& setting : settings)
        {
            if (setting.key.empty() || IsHostStructureSettingKey(setting.key) ||
                IsHostAppearanceSettingKey(setting.key))
                continue;

            std::string value = setting.defaultValue;
            if (value.empty() && values)
            {
                auto it = values->find(setting.key);
                if (it != values->end())
                    value = it->second;
            }
            setStorage(setting.key, value);
        }
    };

    if (widget.customStyle)
    {
        ImGui::SeparatorText(_L("app.settings.appearance"));

        bool followPersonalization =
            getStorage("followPersonalization", "0") == "1" ||
            getStorage("followPersonalization", "0") == "true";
        if (editorCheckbox(_L("engine.editor.follow_global"), "##FollowPersonalization", &followPersonalization))
            setStorage("followPersonalization", followPersonalization ? "1" : "0");

        constexpr const char* builtInThemeIds[] = {
            "__global_dark", "__global_light",
            "__global_glass_dark", "__global_glass_light",
            "__global_acrylic_dark", "__global_acrylic_light"
        };
        constexpr int builtInPresetIds[] = {
            kAppearancePresetDark, kAppearancePresetLight,
            kAppearancePresetGlassDark, kAppearancePresetGlassLight,
            kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight
        };
        constexpr int builtInThemeCount = IM_ARRAYSIZE(builtInThemeIds);
        constexpr int customThemeIndex = builtInThemeCount;
        constexpr const char* customThemeId = "__custom";
        constexpr const char* legacyMainSnapshotId = "__main_personalization";
        constexpr const char* builtInThemeNameKeys[] = {
            L10N_KEY("app.settings.dark"), L10N_KEY("app.settings.light"),
            L10N_KEY("app.settings.dark_glass"), L10N_KEY("app.settings.light_glass"),
            L10N_KEY("app.settings.dark_acrylic"), L10N_KEY("app.settings.light_acrylic")
        };
        static_assert(IM_ARRAYSIZE(builtInThemeNameKeys) == builtInThemeCount);
        std::vector<std::string> builtInThemeLabels;
        builtInThemeLabels.reserve(builtInThemeCount);
        for (const char* key : builtInThemeNameKeys)
            builtInThemeLabels.push_back(
                _LF("engine.editor.global_theme_format", _L(key)));

        std::vector<std::string> themeItemLabels;
        themeItemLabels.reserve(builtInThemeCount + 1 + presets.size());
        for (int i = 0; i < builtInThemeCount; ++i)
            themeItemLabels.push_back(builtInThemeLabels[i] +
                "###BuiltInTheme" + std::to_string(i));
        themeItemLabels.push_back(std::string(_L("app.settings.custom")) +
            "###CustomTheme");
        for (size_t i = 0; i < presets.size(); ++i)
            themeItemLabels.push_back(presets[i].label +
                "###ComponentTheme" + std::to_string(i));
        std::vector<const char*> themeLabels;
        themeLabels.reserve(themeItemLabels.size());
        for (const auto& label : themeItemLabels)
            themeLabels.push_back(label.c_str());

        std::string currentPreset = getStorage("__preset",
            defaultPresetIndex >= 0
                ? presets[static_cast<size_t>(defaultPresetIndex)].id
                : customThemeId);
        int selectedTheme = customThemeIndex;
        for (int i = 0; i < IM_ARRAYSIZE(builtInThemeIds); ++i)
        {
            if (currentPreset == builtInThemeIds[i])
            {
                selectedTheme = i;
                break;
            }
        }
        if (currentPreset != customThemeId && currentPreset != legacyMainSnapshotId &&
            selectedTheme == customThemeIndex)
        {
            for (size_t i = 0; i < presets.size(); ++i)
            {
                if (presets[i].id == currentPreset)
                {
                    selectedTheme = customThemeIndex + 1 + static_cast<int>(i);
                    break;
                }
            }
        }

        ImGui::BeginDisabled(followPersonalization);
        ImGui::SetNextItemWidth(beginEditorRow(
            _L("app.settings.theme"), kEditorControlWidth));
        if (ImGui::Combo("##Theme", &selectedTheme, themeLabels.data(),
            static_cast<int>(themeLabels.size())))
        {
            if (selectedTheme >= 0 && selectedTheme < builtInThemeCount)
            {
                auto preset = MakeAppearancePreset(
                    builtInPresetIds[selectedTheme]);
                applyAppearance(preset);
                setStorage("__preset", builtInThemeIds[selectedTheme]);
                setStorage("__contentTheme", std::to_string(preset.contentTheme));
            }
            else if (selectedTheme == customThemeIndex)
            {
                setStorage("__preset", customThemeId);
            }
            else
            {
                const size_t presetIndex = static_cast<size_t>(
                    selectedTheme - customThemeIndex - 1);
                if (presetIndex < presets.size())
                {
                    if (presets[presetIndex].values.find("glassEnabled") ==
                        presets[presetIndex].values.end())
                        setStorage("glassEnabled", "0");
                    if (presets[presetIndex].values.find("acrylicEnabled") ==
                        presets[presetIndex].values.end())
                        setStorage("acrylicEnabled", "0");
                    for (const auto& kv : presets[presetIndex].values)
                    {
                        if (kv.first != "followPersonalization")
                            setStorage(kv.first, kv.second);
                    }
                    setStorage("__preset", presets[presetIndex].id);
                    setStorage("__contentTheme", "");
                }
            }
        }
        ImGui::EndDisabled();

        const bool customThemeSelected = selectedTheme == customThemeIndex;
        if (!followPersonalization && customThemeSelected)
        {
            ImGui::Spacing();
            ImGui::Indent();
            float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f, alpha = 0.36f;
            float borderR = 1.0f, borderG = 1.0f, borderB = 1.0f;
            float borderAlpha = 0.40f;
            float gradientEndA = 0.0f;
            bool glassEnabled = false;
            bool acrylicEnabled = false;
            ReadCustomColors(widgetId, bgR, bgG, bgB, alpha,
                borderR, borderG, borderB, borderAlpha,
                gradientEndA, glassEnabled, acrylicEnabled);

            float bgColor[3] = { bgR, bgG, bgB };
            ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.bg_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x));
            if (ImGui::ColorEdit3("##BackgroundColor", bgColor,
                ImGuiColorEditFlags_NoInputs))
                setStorage("bg", std::to_string(colorToInt(
                    bgColor[0], bgColor[1], bgColor[2])));
            ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.bg_opacity"), kEditorSliderWidth));
            if (ImGui::SliderFloat("##BackgroundOpacity", &alpha, 0.0f, 1.0f))
                setStorage("alpha", std::to_string(alpha));
            float borderColor[3] = { borderR, borderG, borderB };
            ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.border_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x));
            if (ImGui::ColorEdit3("##BorderColor", borderColor,
                ImGuiColorEditFlags_NoInputs))
                setStorage("border", std::to_string(colorToInt(
                    borderColor[0], borderColor[1], borderColor[2])));
            ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.border_opacity"), kEditorSliderWidth));
            if (ImGui::SliderFloat("##BorderOpacity", &borderAlpha, 0.0f, 1.0f))
                setStorage("borderAlpha", std::to_string(borderAlpha));
            ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.gradient_end_alpha"), kEditorSliderWidth));
            if (ImGui::SliderFloat("##GradientEndOpacity", &gradientEndA, 0.0f, 1.0f))
                setStorage("gradientEndA", std::to_string(gradientEndA));
            if (editorCheckbox(_L("app.settings.glass_bg"), "##GlassBackground", &glassEnabled))
            {
                setStorage("glassEnabled", glassEnabled ? "1" : "0");
                if (!glassEnabled && acrylicEnabled)
                {
                    acrylicEnabled = false;
                    setStorage("acrylicEnabled", "0");
                }
            }
            if (editorCheckbox(_L("app.settings.acrylic_enable"), "##AcrylicBackground", &acrylicEnabled))
            {
                setStorage("acrylicEnabled", acrylicEnabled ? "1" : "0");
                if (acrylicEnabled && !glassEnabled)
                {
                    glassEnabled = true;
                    setStorage("glassEnabled", "1");
                }
            }
            {
                const char* contentThemeNames[] = { _L("app.settings.light"), _L("app.settings.dark") };
                std::string stored = getStorage("__contentTheme", "");
                int ctValue = std::clamp(stored.empty() ? mainPersonalization.contentTheme
                    : std::stoi(stored), 0, 1);
                ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.widget_content_theme"), kEditorControlWidth));
                if (ImGui::Combo("##ContentTheme", &ctValue,
                    contentThemeNames, IM_ARRAYSIZE(contentThemeNames)))
                    setStorage("__contentTheme", std::to_string(ctValue));
            }
            ImGui::Unindent();
        }
        ImGui::Spacing();
    }

    if (!widget.customStyle && !presets.empty())
    {
        ImGui::SeparatorText(_L("app.settings.style_preview"));
        std::string currentPreset = getStorage("__preset", defaultPresetIndex >= 0
            ? presets[static_cast<size_t>(defaultPresetIndex)].id : presets[0].id);
        int selectedPreset = 0;
        for (size_t i = 0; i < presets.size(); ++i)
            if (presets[i].id == currentPreset) selectedPreset = static_cast<int>(i);
        std::vector<std::string> presetItemLabels;
        presetItemLabels.reserve(presets.size());
        for (size_t i = 0; i < presets.size(); ++i)
            presetItemLabels.push_back(presets[i].label +
                "###ComponentPreset" + std::to_string(i));
        std::vector<const char*> presetLabels;
        presetLabels.reserve(presetItemLabels.size());
        for (const auto& label : presetItemLabels)
            presetLabels.push_back(label.c_str());
        ImGui::SetNextItemWidth(beginEditorRow(_L("app.settings.current_preview"), kEditorControlWidth));
        if (ImGui::Combo("##WidgetPreset", &selectedPreset, presetLabels.data(),
            static_cast<int>(presetLabels.size())))
        {
            applyValues(presets[static_cast<size_t>(selectedPreset)].values);
            setStorage("__preset", presets[static_cast<size_t>(selectedPreset)].id);
        }
        const char* resetPresetLabel = _L("app.settings.restore_script_default");
        if (defaultPresetIndex >= 0)
        {
            beginEditorRow(_L("app.settings.preview_default"), editorButtonWidth(resetPresetLabel));
            if (whiteTextButton(resetPresetLabel))
            {
                applyValues(presets[static_cast<size_t>(defaultPresetIndex)].values);
                setStorage("__preset", presets[static_cast<size_t>(defaultPresetIndex)].id);
            }
        }
        ImGui::Spacing();
    }

    if (!settings.empty())
    {
        ImGui::SeparatorText(_L("app.settings.script_settings"));
        for (size_t settingIndex = 0;
            settingIndex < settings.size(); ++settingIndex)
        {
            ImGui::PushID(static_cast<int>(settingIndex));
            renderSetting(settings[settingIndex]);
            ImGui::PopID();
        }
        const char* resetSettingsLabel = _L("app.settings.restore_default_settings");
        beginEditorRow(_L("app.settings.set_as_default"), editorButtonWidth(resetSettingsLabel));
        if (whiteTextButton(resetSettingsLabel))
            applyDefaultSettingValues();
        ImGui::Spacing();
    }

    flushStorageChanges();

    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    if (lua_istable(state, -1))
    {
        // Inject widgetId global
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(state, widUtf8.c_str());
        lua_setfield(state, -2, "widgetId");

        lua_getfield(state, -1, "imguiRender");
        if (lua_isfunction(state, -1))
        {
            ImGui::Spacing();
            ImGui::PushID("ScriptImguiRender");

            if (snowdesktop::lua_runtime::ProtectedCall(state, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(state, -1);
                RuntimeRecordError(widgetId, err ? err : "(imguiRender error)");
                lua_pop(state, 1);
            }
            ImGui::PopID();
        }
        else
            lua_pop(state, 1);
    }
    lua_pop(state, 1);
    ImGui::PopID();
    return true;
}

void WidgetEngine::RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath,
    ID2D1DeviceContext* context, RECT bounds, int columns, int rows)
{
    (void)scriptPath;

    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    LuaWidget* found = &widgets_[idx];
    PreviewExecutionScope previewScope(
        found && found->preview ? &found->previewStorage : nullptr);

    // Hot-reload: check if file changed or deleted
    if (!found->preview)
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        bool exists = GetFileAttributesExW(found->filePath.c_str(), GetFileExInfoStandard, &attr) != 0;
        if (!exists) { found->valid = false; return; }
        if (CompareFileTime(&attr.ftLastWriteTime, &found->lastModified) != 0)
        {
            if (!ReloadWidget(widgetId)) return;
            idx = FindWidget(widgetId);
            if (idx < 0) return;
            found = &widgets_[idx];
        }
    }
    if (!found->valid) return;

    lua_State* state = found->state;
    if (!state) return;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    d2dState_->ctx = context;
    DrainShellIconResults(d2dState_);
    found->lastBounds = bounds;
    d2dState_->gridColumns = std::max(1, columns);
    d2dState_->gridRows = std::max(1, rows);
    SetWidgetRectContext(d2dState_, bounds);

    if (!found->hostVisible)
    {
        found->hostVisible = true;
        InvokeSimpleCallback(*found, "onVisible");
    }
    if (found->lastColumns != columns || found->lastRows != rows)
    {
        found->lastColumns = std::max(1, columns);
        found->lastRows = std::max(1, rows);
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->ref);
        if (lua_istable(state, -1))
        {
            lua_getfield(state, -1, "onSizeChanged");
            if (lua_isfunction(state, -1))
            {
                lua_pushinteger(state, found->lastColumns);
                lua_pushinteger(state, found->lastRows);
                if (snowdesktop::lua_runtime::ProtectedCall(state, 2, 0) != LUA_OK)
                {
                    const char* error = lua_tostring(state, -1);
                    RuntimeRecordError(widgetId, error ? error : "(onSizeChanged error)");
                    lua_pop(state, 1);
                }
            }
            else
                lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    found->lastRenderTime = std::chrono::steady_clock::now();
    found->hostControls.clear();
    d2dState_->widgetClipDepth = 0;

    lua_rawgeti(state, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return; }

    // Inject widgetId global
    {
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(state, widUtf8.c_str());
        lua_setfield(state, -2, "widgetId");
    }

    lua_getfield(state, -1, "render");
    if (lua_isfunction(state, -1))
    {
        if (snowdesktop::lua_runtime::ProtectedCall(state, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(render error)");
            lua_pop(state, 1);
            while (d2dState_->widgetClipDepth > 0)
            {
                d2dState_->ctx->PopAxisAlignedClip();
                --d2dState_->widgetClipDepth;
            }

            // draw conspicuous placeholder
            if (d2dState_ && d2dState_->ctx)
            {
                ComPtr<ID2D1SolidColorBrush> brush;
                d2dState_->ctx->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.15f, 0.15f, 1.0f), &brush);
                d2dState_->ctx->FillRectangle(d2dState_->widgetRect, brush.Get());

                // draw white text
                if (d2dState_->dwrite)
                {
                    ComPtr<IDWriteTextFormat> format;
                    const float errFontSize = std::max(9.0f, 15.0f * CalculateWidgetCellScale(
                        d2dState_->gridCellW, d2dState_->gridCellH));
                    d2dState_->dwrite->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        errFontSize, L"", &format);
                    if (format)
                    {
                        std::wstring wmsg = L"WIDGET ERROR: ";
                        std::string serr = err ? err : "(unknown)";
                        // append part of error
                        size_t maxlen = 128;
                        if (serr.size() > maxlen) serr = serr.substr(0, maxlen) + "...";
                        int wlen = MultiByteToWideChar(CP_UTF8, 0, serr.c_str(), -1, nullptr, 0);
                        std::wstring werr(wlen, L'\0');
                        MultiByteToWideChar(CP_UTF8, 0, serr.c_str(), -1, &werr[0], wlen);
                        wmsg += werr;

                        D2D1_RECT_F r = d2dState_->widgetRect;
                        r.left += 8; r.top += 8; r.right -= 8; r.bottom -= 8;
                        ComPtr<ID2D1SolidColorBrush> textBrush;
                        d2dState_->ctx->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &textBrush);
                        d2dState_->ctx->DrawTextW(wmsg.c_str(), (UINT32)wmsg.size() - 1, format.Get(), &r, textBrush.Get());
                    }
                }
            }
            // mark invalid to avoid repeated attempts
            if (const int currentIndex = FindWidget(widgetId);
                currentIndex >= 0)
            {
                widgets_[currentIndex].valid = false;
            }
            lua_pop(state, 1);
            return;
        }
    }
    else
    {
        lua_pop(state, 1);
    }
    while (d2dState_->widgetClipDepth > 0)
    {
        d2dState_->ctx->PopAxisAlignedClip();
        --d2dState_->widgetClipDepth;
    }
    lua_pop(state, 1);
}

bool WidgetEngine::RenderWidgetPanel(
    const std::wstring& widgetId,
    ID2D1DeviceContext* context, RECT bounds)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !context)
        return false;
    auto& widget = widgets_[index];
    if (!widget.valid)
        return false;
    lua_State* state = widget.state;
    if (!state)
        return false;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    d2dState_->ctx = context;
    DrainShellIconResults(d2dState_);
    widget.lastBounds = bounds;
    SetWidgetRectContext(d2dState_, bounds);
    widget.lastRenderTime =
        std::chrono::steady_clock::now();
    widget.hostControls.clear();
    d2dState_->widgetClipDepth = 0;

    lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return false;
    }
    lua_getfield(state, -1, "renderPanel");
    if (!lua_isfunction(state, -1))
    {
        lua_pop(state, 2);
        return false;
    }
    const bool succeeded =
        snowdesktop::lua_runtime::ProtectedCall(state, 0, 0) == LUA_OK;
    if (!succeeded)
    {
        const char* error = lua_tostring(state, -1);
        RuntimeRecordError(
            widgetId,
            error ? error : "(renderPanel error)");
        lua_pop(state, 1);
    }
    while (d2dState_->widgetClipDepth > 0)
    {
        d2dState_->ctx->PopAxisAlignedClip();
        --d2dState_->widgetClipDepth;
    }
    lua_pop(state, 1);
    return succeeded;
}

void WidgetEngine::TickRuntime()
{
    if (calendarService_)
        calendarService_->Tick();
    const bool eventsChanged =
        std::exchange(
            pendingCalendarEventsChange_, false);
    const bool selectionChanged =
        std::exchange(
            pendingCalendarSelectionChange_, false);
    if (eventsChanged)
        NotifyCalendarChanged("events");
    if (selectionChanged)
    {
        NotifyCalendarChanged("selection");
    }
    const bool systemChanged = systemSnapshotChanged_.exchange(false);
    const bool mediaChanged = mediaSnapshotChanged_.exchange(false);
    if (systemChanged || mediaChanged)
    {
        std::unordered_set<std::wstring> dirtyWidgets;
        for (const auto& widget : widgets_)
        {
            if (!widget.valid || widget.preview) continue;
            if ((systemChanged && widget.usesSystemSnapshot) ||
                (mediaChanged && widget.usesMediaSnapshot))
            {
                dirtyWidgets.insert(widget.widgetId);
            }
        }
        for (const auto& widgetId : dirtyWidgets)
            RuntimeInvalidateHost(widgetId);
    }

    if (httpService_)
    {
        for (auto& response : httpService_->Drain())
        {
            int index = FindWidget(response.widgetId);
            if (index < 0) continue;
            auto& widget = widgets_[index];
            lua_State* state = widget.state;
            if (!state) continue;
            const std::wstring widgetId = widget.widgetId;
            WidgetExecutionContextGuard contextGuard(
                d2dState_, widgetId);
            snowdesktop::lua_runtime::StackGuard stackGuard(state);
            SetWidgetRectContext(d2dState_, widget.lastBounds);
            lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
            if (lua_istable(state, -1))
            {
                lua_getfield(state, -1, "onHttpResponse");
                if (lua_isfunction(state, -1))
                {
                    lua_pushinteger(state, response.id);
                    lua_createtable(state, 0, 5);
                    lua_pushinteger(state, response.status); lua_setfield(state, -2, "status");
                    lua_pushlstring(state, response.body.data(), response.body.size()); lua_setfield(state, -2, "body");
                    lua_pushstring(state, response.error.c_str()); lua_setfield(state, -2, "error");
                    lua_pushboolean(state, response.fromCache); lua_setfield(state, -2, "fromCache");
                    lua_pushboolean(state, response.error.empty() &&
                        response.status >= 200 && response.status < 300);
                    lua_setfield(state, -2, "ok");
                    if (snowdesktop::lua_runtime::ProtectedCall(state, 2, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(state, -1);
                        RuntimeRecordError(widgetId, error ? error : "(onHttpResponse error)");
                        lua_pop(state, 1);
                    }
                    RuntimeInvalidateHost(widgetId);
                }
                else
                    lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview) continue;
        if (widget.hostVisible && widget.lastRenderTime.time_since_epoch().count() > 0 &&
            now - widget.lastRenderTime > std::chrono::milliseconds(2500))
        {
            widget.hostVisible = false;
            InvokeSimpleCallback(widget, "onHidden");
        }

    }
}

void WidgetEngine::OnWidgetTimer(const std::wstring& widgetId, UINT_PTR timerId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& widget = widgets_[idx];
    lua_State* state = widget.state;
    if (!state) return;
    const std::wstring activeWidgetId = widget.widgetId;
    WidgetExecutionContextGuard contextGuard(
        d2dState_, activeWidgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, widget.lastBounds);

    if (timerId == widget.refreshTimerId)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
        if (lua_istable(state, -1))
        {
            lua_getfield(state, -1, "onTimer");
            if (lua_isfunction(state, -1))
            {
                lua_pushstring(state, "refresh");
                if (snowdesktop::lua_runtime::ProtectedCall(state, 1, 0) != LUA_OK)
                {
                    const char* error = lua_tostring(state, -1);
                    RuntimeRecordError(activeWidgetId, error ? error : "(onTimer refresh error)");
                    lua_pop(state, 1);
                }
            }
            else
                lua_pop(state, 1);
        }
        lua_pop(state, 1);
        RuntimeInvalidateHost(activeWidgetId);
        return;
    }

    if (timerId != widget.namedTimerId)
        return;

    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    const auto now = std::chrono::steady_clock::now();
    const std::vector<std::string> dueNames =
        widget.namedTimers.DueNames(now);

    bool invoked = false;
    for (const auto& name : dueNames)
    {
        if (!widget.namedTimers.ConsumeDue(name, now))
            continue;

        lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
        if (lua_istable(state, -1))
        {
            lua_getfield(state, -1, "onTimer");
            if (lua_isfunction(state, -1))
            {
                lua_pushstring(state, name.c_str());
                if (snowdesktop::lua_runtime::ProtectedCall(state, 1, 0) != LUA_OK)
                {
                    const char* error = lua_tostring(state, -1);
                    RuntimeRecordError(activeWidgetId, error ? error : "(onTimer error)");
                    lua_pop(state, 1);
                }
                invoked = true;
            }
            else
                lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }

    RescheduleNamedTimer(widget);
    if (invoked)
        RuntimeInvalidateHost(activeWidgetId);
}

// ── Check if widget uses custom style ────────────────────────────
bool WidgetEngine::HasCustomStyle(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 && widgets_[idx].customStyle;
}

void WidgetEngine::InvokeOpen(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    InvokeSimpleCallback(widgets_[idx], "onOpen");
}

void WidgetEngine::InvokeSelected(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    InvokeSimpleCallback(widgets_[idx], "onSelected");
}

void WidgetEngine::InvokeClick(const std::wstring& widgetId, int x, int y)
{
    InvokeMouseEvent(widgetId, "onClick", x, y, 1, 0);
}

void WidgetEngine::InvokeMouseEvent(const std::wstring& widgetId, const char* callbackName, int x, int y,
    int button, int delta)
{
    if (!callbackName || !*callbackName) return;
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return;
    const int widgetRef = w.ref;
    const RECT bounds = w.lastBounds;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, bounds);
    lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return; }
    lua_getfield(state, -1, callbackName);
    if (lua_isfunction(state, -1))
    {
        lua_pushinteger(state, x);
        lua_pushinteger(state, y);
        lua_pushinteger(state, button);
        lua_pushinteger(state, delta);
        if (snowdesktop::lua_runtime::ProtectedCall(state, 4, 0) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(mouse callback error)");
            lua_pop(state, 1);
        }
    }
    else
    {
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

std::vector<LuaWidgetMenuItem> WidgetEngine::GetContextMenu(const std::wstring& widgetId)
{
    std::vector<LuaWidgetMenuItem> result;
    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return result;
    int idx = FindWidget(widgetId);
    if (idx < 0) return result;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return result;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return result; }
    lua_getfield(state, -1, "getContextMenu");
    if (lua_isfunction(state, -1))
    {
        if (snowdesktop::lua_runtime::ProtectedCall(state, 0, 1) == LUA_OK && lua_istable(state, -1))
        {
            int count = static_cast<int>(lua_rawlen(state, -1));
            for (int i = 1; i <= count; ++i)
            {
                lua_rawgeti(state, -1, i);
                if (lua_istable(state, -1))
                {
                    LuaWidgetMenuItem item;
                    lua_getfield(state, -1, "id");
                    item.id = lua_isinteger(state, -1) ? static_cast<int>(lua_tointeger(state, -1)) : i;
                    lua_pop(state, 1);
                    lua_getfield(state, -1, "label");
                    item.label = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
                    lua_pop(state, 1);
                    lua_getfield(state, -1, "icon");
                    item.icon = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
                    lua_pop(state, 1);
                    lua_getfield(state, -1, "iconFont");
                    item.iconFont = lua_isstring(state, -1)
                        ? lua_tostring(state, -1) : "fa";
                    lua_pop(state, 1);
                    lua_getfield(state, -1, "enabled");
                    item.enabled = lua_isnil(state, -1) ? true : (lua_toboolean(state, -1) != 0);
                    lua_pop(state, 1);
                    lua_getfield(state, -1, "separator");
                    item.separator = lua_toboolean(state, -1) != 0;
                    lua_pop(state, 1);
                    if (item.separator || !item.label.empty())
                        result.push_back(std::move(item));
                }
                lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
        else
        {
            const char* err = lua_tostring(state, -1);
            if (err)
                RuntimeRecordError(widgetId, err);
            lua_pop(state, 1);
        }
    }
    else
    {
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return result;
}

void WidgetEngine::InvokeMenu(const std::wstring& widgetId, int menuId)
{
    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return;
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return;
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return; }
    lua_getfield(state, -1, "onMenu");
    if (lua_isfunction(state, -1))
    {
        lua_pushinteger(state, menuId);
        if (snowdesktop::lua_runtime::ProtectedCall(state, 1, 0) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(onMenu error)");
            lua_pop(state, 1);
        }
    }
    else
    {
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

void WidgetEngine::NotifyDesktopChanged(const std::string& reason)
{
    if (d2dState_)
    {
        d2dState_->shellIconCache.clear();
        d2dState_->shellIconFailures.clear();
    }
    std::vector<std::wstring> targets;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || !RuntimeHasPermission(widget.widgetId, "desktop.read"))
            continue;
        targets.push_back(widget.widgetId);
    }
    for (const auto& widgetId : targets)
    {
        const int index = FindWidget(widgetId);
        if (index < 0) continue;
        lua_State* state = widgets_[index].state;
        if (!state) continue;
        const int widgetRef = widgets_[index].ref;
        const RECT bounds = widgets_[index].lastBounds;
        WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
        snowdesktop::lua_runtime::StackGuard stackGuard(state);
        SetWidgetRectContext(d2dState_, bounds);
        lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
        if (!lua_istable(state, -1)) { lua_pop(state, 1); continue; }
        lua_getfield(state, -1, "onDesktopChanged");
        if (lua_isfunction(state, -1))
        {
            lua_pushstring(state, reason.c_str());
            if (snowdesktop::lua_runtime::ProtectedCall(state, 1, 0) != LUA_OK)
            {
                const char* err = lua_tostring(state, -1);
                RuntimeRecordError(widgetId, err ? err : "(onDesktopChanged error)");
                lua_pop(state, 1);
            }
        }
        else
        {
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
}

bool WidgetEngine::ReadBoolFlag(const std::wstring& packageId, const char* flag, bool defaultVal) const
{
    for (const auto& w : widgets_)
    {
        if (w.valid && Utf8ToWideLocal(w.packageId) == packageId)
        {
            lua_State* state = w.state;
            if (!state) return defaultVal;
            snowdesktop::lua_runtime::StackGuard stackGuard(state);
            lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
            if (!lua_istable(state, -1)) { lua_pop(state, 1); return defaultVal; }
            lua_getfield(state, -1, flag);
            bool result = lua_isnil(state, -1) ? defaultVal : (lua_toboolean(state, -1) != 0);
            lua_pop(state, 2);
            return result;
        }
    }
    return defaultVal;
}

bool WidgetEngine::ReadCustomColors(const std::wstring& widgetId,
    float& bgR, float& bgG, float& bgB, float& alpha,
    float& borderR, float& borderG, float& borderB, float& borderAlpha,
    float& gradientEndA, bool& glassEnabled, bool& acrylicEnabled) const
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return false;
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return false; }

            auto readHex = [&](const char* key, float& r, float& g, float& b, int def) {
                lua_getfield(state, -1, key);
                int val = lua_isinteger(state, -1) ? (int)lua_tointeger(state, -1) : def;
                lua_pop(state, 1);
                r = ((val >> 16) & 0xFF) / 255.0f;
                g = ((val >> 8) & 0xFF) / 255.0f;
                b = (val & 0xFF) / 255.0f;
            };

            auto readFloat = [&](const char* key, float& out, float def) {
                lua_getfield(state, -1, key);
                out = lua_isnumber(state, -1) ? (float)lua_tonumber(state, -1) : def;
                lua_pop(state, 1);
            };

            auto readBool = [&](const char* key, bool& out, bool def) {
                lua_getfield(state, -1, key);
                out = lua_isnil(state, -1) ? def : (lua_toboolean(state, -1) != 0);
                lua_pop(state, 1);
            };

            readHex("bg", bgR, bgG, bgB, 0x151A21);
            readHex("border", borderR, borderG, borderB, 0xFFFFFF);
            readFloat("alpha", alpha, 0.36f);
            readFloat("borderAlpha", borderAlpha, alpha);
            readFloat("gradientEndA", gradientEndA, 0.0f);
            readBool("glassEnabled", glassEnabled, false);
            readBool("acrylicEnabled", acrylicEnabled, false);

            const std::string prefix = WidgetWideToUtf8(widgetId) + ".";
            auto readStoredColor = [&](const char* key, float& r, float& g, float& b) {
                auto it = g_storage.find(prefix + key);
                if (it == g_storage.end()) return;
                int val = std::atoi(it->second.c_str());
                r = ((val >> 16) & 0xFF) / 255.0f;
                g = ((val >> 8) & 0xFF) / 255.0f;
                b = (val & 0xFF) / 255.0f;
            };
            auto readStoredFloat = [&](const char* key, float& out) {
                auto it = g_storage.find(prefix + key);
                if (it != g_storage.end()) out = static_cast<float>(std::atof(it->second.c_str()));
            };
            auto readStoredBool = [&](const char* key, bool& out) {
                auto it = g_storage.find(prefix + key);
                if (it != g_storage.end())
                    out = it->second == "1" || it->second == "true";
            };
            readStoredColor("bg", bgR, bgG, bgB);
            readStoredColor("border", borderR, borderG, borderB);
            readStoredFloat("alpha", alpha);
            readStoredFloat("borderAlpha", borderAlpha);
            readStoredFloat("gradientEndA", gradientEndA);
            readStoredBool("glassEnabled", glassEnabled);
            readStoredBool("acrylicEnabled", acrylicEnabled);

            lua_pop(state, 1);
            return true;
}

std::vector<WidgetErrorEntry> WidgetEngine::GetWidgetErrors() const
{
    std::vector<WidgetErrorEntry> result;
    for (const auto& kv : g_storage)
    {
        if (!EndsWithLastError(kv.first))
            continue;
        result.push_back({ kv.first, kv.second });
    }
    std::sort(result.begin(), result.end(), [](const WidgetErrorEntry& a, const WidgetErrorEntry& b) {
        return a.key < b.key;
    });
    return result;
}

std::string WidgetEngine::GetSystemSnapshotError() const
{
    return systemSnapshotService_ ? systemSnapshotService_->GetLastError() : std::string{};
}

void WidgetEngine::ClearWidgetErrors()
{
    for (auto it = g_storage.begin(); it != g_storage.end(); )
    {
        if (EndsWithLastError(it->first))
            it = g_storage.erase(it);
        else
            ++it;
    }
    SaveStorageFile();
}

bool WidgetEngine::ReloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    WidgetExecutionContextGuard reloadContext(d2dState_, widgetId);
    const auto layoutMetrics = widgets_[idx].layoutMetrics;
    std::wstring path = ResolveWidgetPath(
        Utf8ToWideLocal(widgets_[idx].packageId));
    if (path.empty())
        path = widgets_[idx].filePath;
    const size_t oldIndex = static_cast<size_t>(idx);
    if (!LoadWidget(path, widgetId))
    {
        RecoverWidgetPackage(widgets_[idx].packageId,
            widgets_[idx].manifest.version);
        return false;
    }

    // Loading appends the replacement VM. Preserve the host metrics that were
    // already calculated for this instance before retiring the old VM.
    widgets_.back().layoutMetrics = layoutMetrics;

    // The new VM is now fully loaded. Only then retire the last-known-good VM.
    LuaWidget& old = widgets_[oldIndex];
    if (old.hostVisible)
        InvokeSimpleCallback(old, "onHidden");
    if (old.refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(old.refreshTimerId);
    if (old.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(old.namedTimerId);
    if (httpService_) httpService_->CancelWidget(widgetId);
    if (old.state)
    {
        luaL_unref(old.state, LUA_REGISTRYINDEX, old.ref);
        lua_close(old.state);
        old.state = nullptr;
    }
    widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(oldIndex));
    return true;
}

void WidgetEngine::NotifyLanguageChanged(const std::wstring& widgetId)
{
    int index = FindWidget(widgetId);
    if (index < 0)
        return;
    InvokeSimpleCallback(widgets_[index], "onLanguageChanged");
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::RuntimeHasPermission(const std::wstring& widgetId, const char* permission) const
{
    if (!permission || !*permission) return true;
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& perms = widgets_[idx].permissions;
    return perms.contains(permission);
}

void WidgetEngine::RuntimeRecordError(const std::wstring& widgetId, const std::string& message)
{
    std::string idUtf8 = WidgetWideToUtf8(widgetId);
    const int index = FindWidget(widgetId);
    if (!snowdesktop::widget_runtime::IsDryLoad() &&
        (index < 0 || !widgets_[index].preview))
    {
        g_storage[idUtf8 + ".lastError"] = message;
        SaveStorageFile();
    }
    if (index >= 0)
    {
        auto& widget = widgets_[index];
        if (widget.health.RecordError())
            widget.valid = false;
    }
    RuntimeAddLog(widgetId, "error", message);
}

void WidgetEngine::RuntimeAddLog(const std::wstring& widgetId, const std::string& level, const std::string& message)
{
    g_widgetDiagnostics.Add(
        WidgetWideToUtf8(widgetId), level, message);
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopItems() const
{
    if (snowdesktop::widget_runtime::IsDryLoad() || previewOnly_)
    {
        const std::string document =
            _L("app.widget_preview.api.desktop_document");
        const std::string folder =
            _L("app.widget_preview.api.desktop_folder");
        const std::string shortcut =
            _L("app.widget_preview.api.desktop_shortcut");
        return {
            { "preview-document", document,
                "C:\\Users\\Maya\\Desktop\\" + document,
                "desktop", "file", false },
            { "preview-folder", folder,
                "C:\\Users\\Maya\\Desktop\\" + folder,
                "desktop", "folder", false },
            { "preview-shortcut", shortcut,
                "C:\\Users\\Maya\\Desktop\\" + shortcut + ".lnk",
                "desktop", "shortcut", false },
        };
    }
    return desktopSnapshotProvider_ ? desktopSnapshotProvider_() : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopSelection() const
{
    if (snowdesktop::widget_runtime::IsDryLoad() || previewOnly_)
    {
        const std::string document =
            _L("app.widget_preview.api.desktop_document");
        return { { "preview-document", document,
            "C:\\Users\\Maya\\Desktop\\" + document,
            "desktop", "file", true } };
    }
    return selectionProvider_ ? selectionProvider_() : std::vector<LuaDesktopItemInfo>{};
}

void WidgetEngine::NotifyCalendarChanged(
    const std::string& reason)
{
    std::vector<std::wstring> targets;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview ||
            !RuntimeHasPermission(
                widget.widgetId, "calendar.read"))
            continue;
        targets.push_back(widget.widgetId);
    }
    for (const auto& widgetId : targets)
    {
        const int index = FindWidget(widgetId);
        if (index < 0) continue;
        lua_State* state = widgets_[index].state;
        if (!state) continue;
        const int widgetRef = widgets_[index].ref;
        const RECT bounds = widgets_[index].lastBounds;
        WidgetExecutionContextGuard contextGuard(
            d2dState_, widgetId);
        snowdesktop::lua_runtime::StackGuard stackGuard(state);
        SetWidgetRectContext(d2dState_, bounds);
        lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            continue;
        }
        lua_getfield(state, -1, "onCalendarChanged");
        if (lua_isfunction(state, -1))
        {
            lua_pushlstring(
                state, reason.data(), reason.size());
            if (snowdesktop::lua_runtime::ProtectedCall(state, 1, 0) != LUA_OK)
            {
                const char* error =
                    lua_tostring(state, -1);
                RuntimeRecordError(
                    widgetId,
                    error
                        ? error
                        : "(onCalendarChanged error)");
                lua_pop(state, 1);
            }
        }
        else
        {
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
        RuntimeInvalidateHost(widgetId);
    }
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeApplicationSearch(
    const std::string& query, int maxResults) const
{
    if ((snowdesktop::widget_runtime::IsDryLoad() || previewOnly_) &&
        !query.empty() && maxResults > 0)
        return { { "preview-app",
            _L("app.widget_preview.api.application"),
            "C:\\Program Files\\Travel Journal\\TravelJournal.exe",
            "startmenu", "application", false } };
    return applicationSearchProvider_
        ? applicationSearchProvider_(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeEverythingSearch(const std::string& query, int maxResults) const
{
    if ((snowdesktop::widget_runtime::IsDryLoad() || previewOnly_) &&
        !query.empty() && maxResults > 0)
    {
        const std::string title =
            _L("app.widget_preview.api.search_result");
        return { { "preview-search", title,
            "C:\\Users\\Maya\\Documents\\" + title,
            "everything", "file", false } };
    }
    return everythingSearchProvider_
        ? everythingSearchProvider_(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
}

bool WidgetEngine::RuntimeOpenDesktopPath(const std::wstring& path)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || path.empty()) return false;
    return desktopOpenCallback_ ? desktopOpenCallback_(path) : false;
}

bool WidgetEngine::RuntimeRevealDesktopPath(const std::wstring& path)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || path.empty()) return false;
    return desktopRevealCallback_ ? desktopRevealCallback_(path) : false;
}

void WidgetEngine::RuntimeRefreshDesktop()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (desktopRefreshCallback_)
        desktopRefreshCallback_();
}

void WidgetEngine::RuntimeSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (setWidgetTitleCallback_)
        setWidgetTitleCallback_(widgetId, title);
}

void WidgetEngine::RuntimeInvalidateHost(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (invalidateCallback_)
        invalidateCallback_(widgetId);
}

std::string WidgetEngine::RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const
{
    std::string fullKey = WidgetWideToUtf8(widgetId) + "." + key;
    int idx = FindWidget(widgetId);
    auto& storage = idx >= 0 && widgets_[idx].preview
        ? widgets_[idx].previewStorage : ActiveStorage();
    auto it = storage.find(fullKey);
    if (it != storage.end())
        return it->second;

    if (idx < 0) return {};
    const LuaWidget& widget = widgets_[idx];
    if (key == "followPersonalization" && widget.customStyle &&
        widget.followPersonalizationDefault)
        return "1";
    return FindDeclaredDefaultValue(widget, key);
}

void WidgetEngine::RuntimeSetStorageValue(const std::wstring& widgetId, const std::string& key, const std::string& value)
{
    if (key.empty() || IsRemovedPanelEffectSettingKey(key)) return;
    const std::string prefix = WidgetWideToUtf8(widgetId);
    if (!StorageWriteWithinQuota(prefix, key, value))
    {
        RuntimeRecordError(widgetId, "Widget storage quota exceeded");
        return;
    }
    const int index = FindWidget(widgetId);
    if (index >= 0 && widgets_[index].preview)
    {
        widgets_[index].previewStorage[prefix + "." + key] = value;
        return;
    }
    ActiveStorage()[prefix + "." + key] = value;
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
}

void WidgetEngine::ReloadStorage()
{
    LoadStorageFile();
}

void WidgetEngine::RuntimeBeginInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (inlineTextEditCallback_)
        inlineTextEditCallback_(request);
}

void WidgetEngine::RuntimeNotify(const std::wstring& widgetId,
    const std::wstring& title, const std::wstring& message)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    const auto now = std::chrono::steady_clock::now();
    if (widget.notificationWindow.time_since_epoch().count() == 0 ||
        now - widget.notificationWindow >= std::chrono::minutes(1))
    {
        widget.notificationWindow = now;
        widget.notificationsInWindow = 0;
    }
    if (widget.notificationsInWindow >= 5)
    {
        RuntimeRecordError(widgetId, "Widget notification quota exceeded");
        return;
    }
    ++widget.notificationsInWindow;
    if (notifyCallback_)
        notifyCallback_(title, message);
}

void WidgetEngine::EnsureSystemSnapshotServiceStarted()
{
    if (!systemSnapshotService_ || systemSnapshotServiceStarted_) return;
    systemSnapshotChanged_.store(false);
    mediaSnapshotChanged_.store(false);
    systemSnapshotService_->Start([this](bool systemChanged, bool mediaChanged) {
        if (systemChanged) systemSnapshotChanged_.store(true);
        if (mediaChanged) mediaSnapshotChanged_.store(true);
    });
    systemSnapshotServiceStarted_ = true;
}

CpuSnapshot WidgetEngine::RuntimeGetCpuSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, 42.0, 12,
            _L("app.widget_preview.api.cpu") };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetCpu() : CpuSnapshot{};
}

MemorySnapshot WidgetEngine::RuntimeGetMemorySnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, 16ull * 1024 * 1024 * 1024,
            9ull * 1024 * 1024 * 1024,
            7ull * 1024 * 1024 * 1024, 56.25 };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetMemory() : MemorySnapshot{};
}

BatterySnapshot WidgetEngine::RuntimeGetBatterySnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, 78.0, true, true, false };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetBattery() : BatterySnapshot{};
}

NetworkSnapshot WidgetEngine::RuntimeGetNetworkSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, true, 3ull * 1024 * 1024,
            640ull * 1024, 8ull * 1024 * 1024 * 1024,
            2ull * 1024 * 1024 * 1024 };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetNetwork() : NetworkSnapshot{};
}

GpuSnapshot WidgetEngine::RuntimeGetGpuSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true, _L("app.widget_preview.api.gpu"), 36.0,
            8ull * 1024 * 1024 * 1024,
            3ull * 1024 * 1024 * 1024 };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetGpu() : GpuSnapshot{};
}

MediaSnapshot WidgetEngine::RuntimeGetMediaSnapshot(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || IsPreviewWidget(widgetId))
        return { true,
            _LW("app.widget_preview.api.media_title"),
            _LW("app.widget_preview.api.media_artist"),
            _LW("app.widget_preview.api.media_album"),
            _LW("app.widget_preview.api.media_genre"),
            "playing", true, true, true };
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesMediaSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetMedia() : MediaSnapshot{};
}

bool WidgetEngine::RuntimeMediaPlayPause()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaPlayPause();
}

bool WidgetEngine::RuntimeMediaNext()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaNext();
}

bool WidgetEngine::RuntimeMediaPrevious()
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaPrevious();
}

bool WidgetEngine::RuntimeSetTimer(const std::wstring& widgetId, const std::string& name,
    int intervalMs, bool repeat)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    int index = FindWidget(widgetId);
    if (index < 0 || name.empty()) return false;
    if (!widgets_[index].namedTimers.Set(
            name, intervalMs, repeat, std::chrono::steady_clock::now()))
    {
        return false;
    }
    RescheduleNamedTimer(widgets_[index]);
    return true;
}

std::string WidgetEngine::RuntimeCalendarSelectedDate() const
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return "2026-08-02";
    return calendarService_
        ? calendarService_->SelectedDate()
        : std::string();
}

bool WidgetEngine::RuntimeCalendarSetSelectedDate(
    const std::string& date)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return snowdesktop::calendar::CalendarService::GetDateInfo(date).has_value();
    return calendarService_ &&
        calendarService_->SetSelectedDate(date);
}

std::optional<snowdesktop::calendar::DateInfo>
WidgetEngine::RuntimeCalendarDateInfo(
    const std::string& date) const
{
    return snowdesktop::calendar::CalendarService::
        GetDateInfo(date);
}

std::optional<std::string>
WidgetEngine::RuntimeCalendarAddDays(
    const std::string& date, int offset) const
{
    return snowdesktop::calendar::CalendarService::
        AddDays(date, offset);
}

std::vector<snowdesktop::calendar::CalendarEvent>
WidgetEngine::RuntimeCalendarEvents(
    const std::string& fromDate,
    const std::string& toDate) const
{
    if (snowdesktop::widget_runtime::IsDryLoad())
    {
        const std::vector<snowdesktop::calendar::CalendarEvent> samples = {
            { "preview-1", 1,
                _L("app.widget_preview.api.calendar_review"), "2026-08-02",
                false, 600, 660,
                _L("app.widget_preview.api.calendar_review_notes"), 15, {} },
            { "preview-2", 1,
                _L("app.widget_preview.api.calendar_publish"), "2026-08-03",
                true, 0, 0, {}, -1, {} },
        };
        std::vector<snowdesktop::calendar::CalendarEvent> result;
        for (const auto& event : samples)
            if (event.date >= fromDate && event.date <= toDate)
                result.push_back(event);
        return result;
    }
    return calendarService_
        ? calendarService_->Events(fromDate, toDate)
        : std::vector<
            snowdesktop::calendar::CalendarEvent>{};
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarCreate(
    snowdesktop::calendar::CalendarEvent event)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, {}, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_
        ? calendarService_->Create(std::move(event))
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarUpdate(
    const std::string& id,
    int expectedRevision,
    snowdesktop::calendar::CalendarEvent event)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_
        ? calendarService_->Update(
            id, expectedRevision, std::move(event))
        : snowdesktop::calendar::MutationResult{
            false, id, 0, "unavailable"
        };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarRemove(
    const std::string& id)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_
        ? calendarService_->Remove(id)
        : snowdesktop::calendar::MutationResult{
            false, id, 0, "unavailable"
        };
}

bool WidgetEngine::RuntimeCancelTimer(const std::wstring& widgetId, const std::string& name)
{
    int index = FindWidget(widgetId);
    if (index < 0) return false;
    const bool removed = widgets_[index].namedTimers.Cancel(name);
    if (removed)
        RescheduleNamedTimer(widgets_[index]);
    return removed;
}

void WidgetEngine::RebindHostTimers()
{
    for (auto& widget : widgets_)
    {
        // The host retires all old deadline tokens before rebinding. Do not
        // route stale IDs through the kill callback because another host
        // implementation may already have reused them for the new surface.
        widget.refreshTimerId = 0;
        widget.namedTimerId = 0;

        if (widget.preview)
            continue;

        if (widget.manifest.refreshIntervalMs > 0 &&
            widgetTimerRequestCallback_)
        {
            widget.refreshTimerId = widgetTimerRequestCallback_(
                widget.widgetId,
                static_cast<UINT>(widget.manifest.refreshIntervalMs));
        }
        RescheduleNamedTimer(widget);
    }
}

void WidgetEngine::RescheduleNamedTimer(LuaWidget& widget)
{
    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    if (!widgetTimerRequestCallback_)
        return;

    const auto delay = widget.namedTimers.NextDelay(
        std::chrono::steady_clock::now());
    if (!delay)
        return;
    widget.namedTimerId = widgetTimerRequestCallback_(
        widget.widgetId, static_cast<UINT>(delay->count()));
}

int WidgetEngine::RuntimeHttpRequest(const std::wstring& widgetId, HttpRequestOptions options)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return 0;
    int index = FindWidget(widgetId);
    if (index < 0 || !httpService_) return 0;
    options.widgetId = widgetId;
    options.allowAnyHttpOrHttpsUrl = true;
    options.timeoutMs = std::clamp(options.timeoutMs, 1000, 30000);
    options.cacheSeconds = std::clamp(options.cacheSeconds, 0, 86400);
    if (options.body.size() > 64 * 1024) return 0;
    return httpService_->Submit(std::move(options));
}

bool WidgetEngine::RuntimeHttpCancel(const std::wstring& widgetId, int requestId)
{
    return httpService_ && httpService_->Cancel(widgetId, requestId);
}

void WidgetEngine::RuntimeRegisterHostControl(const std::wstring& widgetId,
    LuaWidget::HostControl control)
{
    int index = FindWidget(widgetId);
    if (index < 0) return;
    if (widgets_[index].hostControls.size() >= 128) return;
    if (control.type == LuaWidget::HostControl::Type::Scroll ||
        (control.type == LuaWidget::HostControl::Type::Input &&
            control.multiline))
    {
        const int maximum = std::max(0, control.contentHeight - control.viewportHeight);
        int& offset = widgets_[index].scrollOffsets[control.id];
        offset = std::clamp(offset, 0, maximum);
    }
    widgets_[index].hostControls.push_back(std::move(control));
}

bool WidgetEngine::RuntimeFocusHostInput(const std::wstring& widgetId, const std::string& id)
{
    int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return false;
    auto& controls = widgets_[index].hostControls;
    auto found = std::find_if(controls.rbegin(), controls.rend(), [&](const auto& control) {
        return control.type == LuaWidget::HostControl::Type::Input && control.id == id;
    });
    if (found == controls.rend() || found->storageKey.empty()) return false;

    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == id)
    {
        focusedHostInput_.multiline = found->multiline;
        if (hostInputFocusCallback_)
            hostInputFocusCallback_();
        return true;
    }

    BlurHostInput(false);
    focusedHostInput_.active = true;
    focusedHostInput_.widgetId = widgetId;
    focusedHostInput_.id = id;
    focusedHostInput_.storageKey = found->storageKey;
    focusedHostInput_.text = Utf8ToWideLocal(
        RuntimeGetStorageValue(widgetId, found->storageKey));
    focusedHostInput_.originalText = focusedHostInput_.text;
    focusedHostInput_.cursor = focusedHostInput_.text.size();
    focusedHostInput_.selectionAnchor = found->selectAll
        ? 0 : focusedHostInput_.cursor;
    focusedHostInput_.liveUpdate = found->liveUpdate;
    focusedHostInput_.multiline = found->multiline;
    if (hostInputFocusCallback_)
        hostInputFocusCallback_();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeGetFocusedHostInput(const std::wstring& widgetId,
    const std::string& id, std::wstring& text, size_t& cursor,
    size_t& selectionAnchor, std::wstring& compositionText,
    size_t& compositionCursor) const
{
    if (!focusedHostInput_.active || focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id)
        return false;
    text = focusedHostInput_.text;
    cursor = focusedHostInput_.cursor;
    selectionAnchor = focusedHostInput_.selectionAnchor;
    compositionText = focusedHostInput_.compositionText;
    compositionCursor = focusedHostInput_.compositionCursor;
    return true;
}

size_t WidgetEngine::HitTestHostInputPosition(
    const LuaWidget::HostControl& control,
    const std::wstring& widgetId, int x, int y) const
{
    if (!d2dState_ || control.type !=
        LuaWidget::HostControl::Type::Input)
        return 0;
    const std::wstring& text = focusedHostInput_.text;
    const float width = static_cast<float>(
        control.rect.right - control.rect.left);
    const float height = static_cast<float>(
        control.rect.bottom - control.rect.top);
    ComPtr<IDWriteTextLayout> layout;
    float localX = static_cast<float>(
        x - control.rect.left) - control.padding;
    float localY = static_cast<float>(
        y - control.rect.top);
    if (control.multiline)
    {
        const float scrollbarReserve = 8.0f;
        const float innerWidth = std::max(1.0f,
            width - control.padding * 2.0f -
                scrollbarReserve);
        layout = CreateHostMultilineTextLayout(d2dState_,
            text, control.fontSize, innerWidth);
        localY = localY - control.padding +
            RuntimeGetScrollOffset(widgetId, control.id);
    }
    else
    {
        const float innerWidth = std::max(1.0f,
            width - control.padding * 2.0f);
        layout = CreateHostSingleLineTextLayout(d2dState_,
            text, control.fontSize, innerWidth, height);
    }
    if (!layout)
        return std::min(focusedHostInput_.cursor,
            focusedHostInput_.text.size());

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(localX, localY,
        &trailing, &inside, &metrics)))
        return std::min(focusedHostInput_.cursor,
            focusedHostInput_.text.size());
    const size_t position =
        static_cast<size_t>(metrics.textPosition) +
        (trailing
            ? static_cast<size_t>(metrics.length) : 0);
    return std::min(position, text.size());
}

bool WidgetEngine::IsFocusedHostInputAt(
    const std::wstring& widgetId, int x, int y) const
{
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId)
        return false;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const POINT point{ x, y };
    for (auto it = widgets_[index].hostControls.rbegin();
        it != widgets_[index].hostControls.rend(); ++it)
    {
        if (it->type == LuaWidget::HostControl::Type::Input &&
            it->id == focusedHostInput_.id)
            return PtInRect(&it->rect, point) != FALSE;
    }
    return false;
}

bool WidgetEngine::HandleHostInputPointerMove(
    const std::wstring& widgetId, int x, int y)
{
    if (!focusedHostInput_.active ||
        !focusedHostInput_.pointerSelecting ||
        focusedHostInput_.widgetId != widgetId)
        return false;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    auto& widget = widgets_[index];
    auto control = std::find_if(
        widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& item) {
            return item.type ==
                LuaWidget::HostControl::Type::Input &&
                item.id == focusedHostInput_.id;
        });
    if (control == widget.hostControls.rend())
        return false;
    if (control->multiline)
    {
        int offset = RuntimeGetScrollOffset(
            widgetId, control->id);
        if (y < control->rect.top +
            static_cast<int>(std::lround(control->padding)))
            offset -= 24;
        else if (y > control->rect.bottom -
            static_cast<int>(std::lround(control->padding)))
            offset += 24;
        RuntimeSetScrollOffset(widgetId,
            control->id, offset);
    }
    focusedHostInput_.cursor = HitTestHostInputPosition(
        *control, widgetId, x, y);
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::HandleHostInputPointerUp(
    const std::wstring& widgetId, int x, int y)
{
    if (!focusedHostInput_.active ||
        !focusedHostInput_.pointerSelecting ||
        focusedHostInput_.widgetId != widgetId)
        return false;
    const int index = FindWidget(widgetId);
    if (index >= 0)
    {
        auto& controls = widgets_[index].hostControls;
        auto control = std::find_if(controls.rbegin(),
            controls.rend(), [&](const auto& item) {
                return item.type ==
                    LuaWidget::HostControl::Type::Input &&
                    item.id == focusedHostInput_.id;
            });
        if (control != controls.rend())
            focusedHostInput_.cursor =
                HitTestHostInputPosition(
                    *control, widgetId, x, y);
    }
    focusedHostInput_.pointerSelecting = false;
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeIsWidgetSelected(
    const std::wstring& widgetId) const
{
    return widgetSelectedProvider_ &&
        widgetSelectedProvider_(widgetId);
}

std::wstring WidgetEngine::RuntimeSelectedWidgetPackageId() const
{
    return selectedWidgetPackageProvider_
        ? selectedWidgetPackageProvider_()
        : std::wstring{};
}

bool WidgetEngine::HasFocusedHostInput() const
{
    return focusedHostInput_.active;
}

bool WidgetEngine::IsHostInputAt(
    const std::wstring& widgetId, int x, int y) const
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const POINT point{ x, y };
    for (auto it = widgets_[index].hostControls.rbegin();
        it != widgets_[index].hostControls.rend(); ++it)
    {
        if (it->type == LuaWidget::HostControl::Type::Input &&
            PtInRect(&it->rect, point))
            return true;
    }
    return false;
}

bool WidgetEngine::GetFocusedHostInputCaretRect(RECT& rect) const
{
    rect = {};
    if (!focusedHostInput_.active || !d2dState_)
        return false;

    const int index = FindWidget(focusedHostInput_.widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const auto control = std::find_if(
        widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& item) {
            return item.type == LuaWidget::HostControl::Type::Input &&
                item.id == focusedHostInput_.id;
        });
    if (control == widget.hostControls.rend())
        return false;
    const HostInputDisplayText display =
        BuildHostInputDisplayText(focusedHostInput_.text,
            focusedHostInput_.cursor,
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.compositionText,
            focusedHostInput_.compositionCursor);

    const float width = static_cast<float>(
        control->rect.right - control->rect.left);
    const float height = static_cast<float>(
        control->rect.bottom - control->rect.top);
    ComPtr<IDWriteTextLayout> layout;
    int scrollOffset = 0;
    if (control->multiline)
    {
        constexpr float scrollbarReserve = 8.0f;
        const float innerWidth = std::max(1.0f,
            width - control->padding * 2.0f -
                scrollbarReserve);
        layout = CreateHostMultilineTextLayout(d2dState_,
            display.text, control->fontSize,
            innerWidth);
        scrollOffset = RuntimeGetScrollOffset(
            focusedHostInput_.widgetId, control->id);
    }
    else
    {
        const float innerWidth = std::max(1.0f,
            width - control->padding * 2.0f);
        layout = CreateHostSingleLineTextLayout(d2dState_,
            display.text, control->fontSize,
            innerWidth, height);
    }
    if (!layout)
        return false;

    const size_t safeCursor = std::min(
        display.cursor, display.text.size());
    UINT32 hitPosition = 0;
    BOOL trailing = FALSE;
    if (!display.text.empty())
    {
        if (control->multiline &&
            safeCursor >= display.text.size() &&
            (display.text.back() == L'\n' ||
                display.text.back() == L'\r'))
        {
            hitPosition = static_cast<UINT32>(
                display.text.size());
        }
        else if (safeCursor >= display.text.size())
        {
            hitPosition = static_cast<UINT32>(
                display.text.size() - 1);
            trailing = TRUE;
        }
        else
        {
            hitPosition = static_cast<UINT32>(safeCursor);
        }
    }

    float caretX = 0.0f;
    float caretY = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(
            hitPosition, trailing, &caretX, &caretY,
            &metrics)))
        return false;

    const float localX = static_cast<float>(control->rect.left) +
        control->padding + caretX;
    const float localY = static_cast<float>(control->rect.top) +
        (control->multiline ? control->padding : 0.0f) +
        caretY - static_cast<float>(scrollOffset);
    const float caretHeight = std::max(
        metrics.height, control->fontSize);
    rect.left = widget.lastBounds.left +
        static_cast<LONG>(std::lround(localX));
    rect.top = widget.lastBounds.top +
        static_cast<LONG>(std::lround(localY));
    rect.right = rect.left + 1;
    rect.bottom = rect.top +
        std::max<LONG>(1,
            static_cast<LONG>(std::lround(caretHeight)));
    return true;
}

void WidgetEngine::BlurHostInput(bool cancel)
{
    if (!focusedHostInput_.active) return;

    const std::wstring widgetId = focusedHostInput_.widgetId;
    if (cancel)
    {
        if (focusedHostInput_.liveUpdate)
            RuntimeSetStorageValue(widgetId, focusedHostInput_.storageKey,
                WidgetWideToUtf8(focusedHostInput_.originalText));
    }
    else
    {
        RuntimeSetStorageValue(widgetId, focusedHostInput_.storageKey,
            WidgetWideToUtf8(focusedHostInput_.text));
    }
    focusedHostInput_ = {};
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::SetHostInputComposition(
    const std::wstring& text, size_t cursor)
{
    if (!focusedHostInput_.active)
        return false;
    focusedHostInput_.compositionText = text;
    focusedHostInput_.compositionCursor =
        std::min(cursor, text.size());
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

bool WidgetEngine::CommitHostInputComposition(
    const std::wstring& text)
{
    if (!focusedHostInput_.active)
        return false;

    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    const size_t selectionStart = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor);
    const size_t selectionEnd = std::max(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor);
    focusedHostInput_.text.erase(
        selectionStart, selectionEnd - selectionStart);
    focusedHostInput_.text.insert(selectionStart, text);
    focusedHostInput_.cursor = selectionStart + text.size();
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    if (focusedHostInput_.liveUpdate)
    {
        RuntimeSetStorageValue(focusedHostInput_.widgetId,
            focusedHostInput_.storageKey,
            WidgetWideToUtf8(focusedHostInput_.text));
    }
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

void WidgetEngine::ClearHostInputComposition()
{
    if (!focusedHostInput_.active ||
        focusedHostInput_.compositionText.empty())
        return;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
}

bool WidgetEngine::HandleHostInputChar(wchar_t ch)
{
    if (!focusedHostInput_.active || ch < 0x20 || ch == 0x7F)
        return false;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;

    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    if (focusedHostInput_.selectionAnchor !=
        focusedHostInput_.cursor)
    {
        const size_t selectionStart = std::min(
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
        const size_t selectionEnd = std::max(
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
        focusedHostInput_.text.erase(selectionStart,
            selectionEnd - selectionStart);
        focusedHostInput_.cursor = selectionStart;
    }
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    focusedHostInput_.text.insert(focusedHostInput_.cursor, 1, ch);
    ++focusedHostInput_.cursor;
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    if (focusedHostInput_.liveUpdate)
        RuntimeSetStorageValue(focusedHostInput_.widgetId, focusedHostInput_.storageKey,
            WidgetWideToUtf8(focusedHostInput_.text));
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

bool WidgetEngine::HandleHostInputKey(WPARAM key)
{
    if (!focusedHostInput_.active) return false;

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    focusedHostInput_.pointerSelecting = false;
    auto hasSelection = [&]() {
        return focusedHostInput_.selectionAnchor !=
            focusedHostInput_.cursor;
    };
    auto selectionStart = [&]() {
        return std::min(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    auto selectionEnd = [&]() {
        return std::max(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    auto eraseSelection = [&]() {
        if (!hasSelection())
            return false;
        const size_t start = selectionStart();
        const size_t end = selectionEnd();
        focusedHostInput_.text.erase(start, end - start);
        focusedHostInput_.cursor = start;
        focusedHostInput_.selectionAnchor = start;
        return true;
    };
    auto finishMovement = [&](size_t nextCursor) {
        focusedHostInput_.cursor = std::min(
            nextCursor, focusedHostInput_.text.size());
        if (!shift)
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    };

    bool changed = false;
    if (key == VK_ESCAPE)
    {
        BlurHostInput(true);
        return true;
    }
    if (key == VK_RETURN)
    {
        if (!focusedHostInput_.multiline || ctrl)
        {
            BlurHostInput(false);
            return true;
        }
        eraseSelection();
        focusedHostInput_.text.insert(
            focusedHostInput_.cursor, 1, L'\n');
        ++focusedHostInput_.cursor;
        focusedHostInput_.selectionAnchor =
            focusedHostInput_.cursor;
        changed = true;
    }
    else if (ctrl && key == 'A')
    {
        focusedHostInput_.selectionAnchor = 0;
        focusedHostInput_.cursor = focusedHostInput_.text.size();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    else if (ctrl && (key == 'C' || key == 'X'))
    {
        bool copied = false;
        if (hasSelection() && OpenClipboard(nullptr))
        {
            EmptyClipboard();
            const std::wstring selectedText =
                focusedHostInput_.text.substr(
                    selectionStart(),
                    selectionEnd() - selectionStart());
            const SIZE_T bytes =
                (selectedText.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory)
            {
                if (void* target = GlobalLock(memory))
                {
                    memcpy(target, selectedText.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (SetClipboardData(CF_UNICODETEXT, memory))
                        copied = true;
                    else
                        GlobalFree(memory);
                }
                else
                    GlobalFree(memory);
            }
            CloseClipboard();
        }
        if (key == 'X' && copied)
            changed = eraseSelection();
    }
    else if (ctrl && key == 'V')
    {
        std::wstring pasted;
        if (OpenClipboard(nullptr))
        {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
            {
                if (const wchar_t* source = static_cast<const wchar_t*>(GlobalLock(data)))
                {
                    for (; *source; ++source)
                    {
                        if (*source == L'\r') continue;
                        if (*source == L'\n')
                            pasted.push_back(
                                focusedHostInput_.multiline
                                    ? L'\n' : L' ');
                        else if (*source == L'\t')
                            pasted.append(
                                focusedHostInput_.multiline
                                    ? L"    " : L" ");
                        else
                            pasted.push_back(*source);
                    }
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!pasted.empty())
        {
            eraseSelection();
            focusedHostInput_.text.insert(focusedHostInput_.cursor, pasted);
            focusedHostInput_.cursor += pasted.size();
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (key == VK_BACK)
    {
        changed = eraseSelection();
        if (!changed && focusedHostInput_.cursor > 0)
        {
            focusedHostInput_.text.erase(--focusedHostInput_.cursor, 1);
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (key == VK_DELETE)
    {
        changed = eraseSelection();
        if (!changed &&
            focusedHostInput_.cursor < focusedHostInput_.text.size())
        {
            focusedHostInput_.text.erase(focusedHostInput_.cursor, 1);
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (focusedHostInput_.multiline &&
        (key == VK_UP || key == VK_DOWN))
    {
        const size_t textSize = focusedHostInput_.text.size();
        const size_t cursor = std::min(
            focusedHostInput_.cursor, textSize);
        const size_t currentLineStart = cursor == 0
            ? 0
            : [&]() {
                const size_t newline =
                    focusedHostInput_.text.rfind(
                        L'\n', cursor - 1);
                return newline == std::wstring::npos
                    ? size_t{0} : newline + 1;
            }();
        const size_t column = cursor - currentLineStart;
        if (key == VK_UP && currentLineStart > 0)
        {
            const size_t previousLineEnd =
                currentLineStart - 1;
            const size_t previousLineStart =
                previousLineEnd == 0
                ? 0
                : [&]() {
                    const size_t newline =
                        focusedHostInput_.text.rfind(
                            L'\n', previousLineEnd - 1);
                    return newline == std::wstring::npos
                        ? size_t{0} : newline + 1;
                }();
            focusedHostInput_.cursor = std::min(
                previousLineStart + column,
                previousLineEnd);
        }
        else if (key == VK_DOWN)
        {
            const size_t currentLineEnd =
                focusedHostInput_.text.find(
                    L'\n', cursor);
            if (currentLineEnd != std::wstring::npos)
            {
                const size_t nextLineStart =
                    currentLineEnd + 1;
                const size_t nextLineEnd =
                    focusedHostInput_.text.find(
                        L'\n', nextLineStart);
                focusedHostInput_.cursor = std::min(
                    nextLineStart + column,
                    nextLineEnd == std::wstring::npos
                        ? textSize : nextLineEnd);
            }
        }
        return finishMovement(focusedHostInput_.cursor);
    }
    else if (key == VK_LEFT || key == VK_HOME)
    {
        if (!shift && hasSelection() && key == VK_LEFT)
            return finishMovement(selectionStart());
        else if (key == VK_HOME &&
            focusedHostInput_.multiline && !ctrl)
        {
            if (focusedHostInput_.cursor == 0)
                focusedHostInput_.cursor = 0;
            else
            {
                const size_t newline =
                    focusedHostInput_.text.rfind(
                        L'\n',
                        focusedHostInput_.cursor - 1);
                focusedHostInput_.cursor =
                    newline == std::wstring::npos
                    ? 0 : newline + 1;
            }
        }
        else
            focusedHostInput_.cursor = key == VK_HOME
                ? 0 : (focusedHostInput_.cursor > 0
                    ? focusedHostInput_.cursor - 1 : 0);
        return finishMovement(focusedHostInput_.cursor);
    }
    else if (key == VK_RIGHT || key == VK_END)
    {
        if (!shift && hasSelection() && key == VK_RIGHT)
            return finishMovement(selectionEnd());
        else if (key == VK_END &&
            focusedHostInput_.multiline && !ctrl)
        {
            const size_t newline =
                focusedHostInput_.text.find(
                    L'\n', focusedHostInput_.cursor);
            focusedHostInput_.cursor =
                newline == std::wstring::npos
                ? focusedHostInput_.text.size() : newline;
        }
        else
            focusedHostInput_.cursor = key == VK_END
                ? focusedHostInput_.text.size()
                : std::min(focusedHostInput_.cursor + 1,
                    focusedHostInput_.text.size());
        return finishMovement(focusedHostInput_.cursor);
    }
    else if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU ||
        key == VK_CAPITAL || key == VK_TAB)
    {
        return true;
    }
    else
    {
        // Printable keys arrive through WM_CHAR; consume the keydown so desktop
        // shortcuts do not run while the host-rendered field owns focus.
        return true;
    }

    if (changed)
    {
        if (focusedHostInput_.liveUpdate)
            RuntimeSetStorageValue(focusedHostInput_.widgetId, focusedHostInput_.storageKey,
                WidgetWideToUtf8(focusedHostInput_.text));
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
    }
    return true;
}

int WidgetEngine::RuntimeGetScrollOffset(const std::wstring& widgetId, const std::string& id) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return 0;
    auto it = widgets_[index].scrollOffsets.find(id);
    return it == widgets_[index].scrollOffsets.end() ? 0 : it->second;
}

void WidgetEngine::RuntimeSetScrollOffset(
    const std::wstring& widgetId, const std::string& id,
    int offset)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return;
    auto& widget = widgets_[index];
    int maximum = 0;
    for (auto it = widget.hostControls.rbegin();
        it != widget.hostControls.rend(); ++it)
    {
        if (it->id != id) continue;
        if (it->type != LuaWidget::HostControl::Type::Scroll &&
            !(it->type == LuaWidget::HostControl::Type::Input &&
                it->multiline))
            continue;
        maximum = std::max(
            0, it->contentHeight - it->viewportHeight);
        break;
    }
    widget.scrollOffsets[id] =
        std::clamp(offset, 0, maximum);
}

std::vector<LuaWidget::HostControl> WidgetEngine::GetScrollControls(const std::wstring& widgetId) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return {};
    std::vector<LuaWidget::HostControl> results;
    for (const auto& ctrl : widgets_[index].hostControls)
    {
        if (ctrl.type == LuaWidget::HostControl::Type::Scroll ||
            (ctrl.type == LuaWidget::HostControl::Type::Input &&
                ctrl.multiline))
            results.push_back(ctrl);
    }
    return results;
}

bool WidgetEngine::HandleHostUiPointer(const std::wstring& widgetId, int x, int y,
    int delta, bool wheel)
{
    int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    POINT point{ x, y };
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!PtInRect(&it->rect, point)) continue;
        if (wheel &&
            (it->type == LuaWidget::HostControl::Type::Scroll ||
                (it->type ==
                    LuaWidget::HostControl::Type::Input &&
                    it->multiline)))
        {
            int maximum = std::max(0, it->contentHeight - it->viewportHeight);
            int& offset = widget.scrollOffsets[it->id];
            const auto result =
                snowdesktop::widget_scroll_rules::ApplyWheelDelta(
                    offset, maximum, delta);
            if (!result.moved)
                continue;
            offset = result.offset;
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (wheel) continue;
        if (it->type == LuaWidget::HostControl::Type::Input)
        {
            if (RuntimeFocusHostInput(widgetId, it->id) &&
                focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == it->id)
            {
                focusedHostInput_.cursor =
                    HitTestHostInputPosition(
                        *it, widgetId, x, y);
                focusedHostInput_.selectionAnchor =
                    focusedHostInput_.cursor;
                focusedHostInput_.pointerSelecting = true;
                RuntimeInvalidateHost(widgetId);
            }
            return true;
        }
        if (it->type == LuaWidget::HostControl::Type::Button ||
            it->type == LuaWidget::HostControl::Type::Toggle)
        {
            lua_State* state = widget.state;
            if (!state) return false;
            const int widgetRef = widget.ref;
            const RECT bounds = widget.lastBounds;
            const std::string controlId = it->id;
            const bool controlValue =
                it->type == LuaWidget::HostControl::Type::Toggle
                    ? !it->value : true;
            WidgetExecutionContextGuard contextGuard(
                d2dState_, widgetId);
            snowdesktop::lua_runtime::StackGuard stackGuard(state);
            SetWidgetRectContext(d2dState_, bounds);
            lua_rawgeti(state, LUA_REGISTRYINDEX, widgetRef);
            if (lua_istable(state, -1))
            {
                lua_getfield(state, -1, "onUiAction");
                if (lua_isfunction(state, -1))
                {
                    lua_pushstring(state, controlId.c_str());
                    lua_pushboolean(state, controlValue);
                    if (snowdesktop::lua_runtime::ProtectedCall(state, 2, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(state, -1);
                        RuntimeRecordError(widgetId, error ? error : "(onUiAction error)");
                        lua_pop(state, 1);
                    }
                }
                else
                    lua_pop(state, 1);
            }
            lua_pop(state, 1);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
    }
    return false;
}

LuaWidgetTheme WidgetEngine::RuntimeGetWidgetTheme(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 ? widgets_[idx].theme : LuaWidgetTheme{};
}

void WidgetEngine::SetWidgetTheme(const std::wstring& widgetId, const LuaWidgetTheme& theme)
{
    int idx = FindWidget(widgetId);
    if (idx >= 0)
        widgets_[idx].theme = theme;
}

void WidgetEngine::SetWidgetLayoutMetrics(
    const std::wstring& widgetId,
    int cellWidth, int cellHeight, int gapY, int barHeight,
    DWRITE_FONT_WEIGHT fontWeight)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return;
    widgets_[index].layoutMetrics =
        snowdesktop::widget_runtime::NormalizeLayoutMetrics(
            cellWidth, cellHeight, gapY, barHeight,
            fontWeight);
}

void WidgetEngine::RuntimeOpenWidgetSettings(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (openWidgetSettingsCallback_)
        openWidgetSettingsCallback_(widgetId, L"");
}

std::vector<WidgetDiagnosticEntry> WidgetEngine::GetWidgetDiagnostics() const
{
    std::vector<WidgetDiagnosticEntry> result;
    result.reserve(widgets_.size());
    for (const auto& widget : widgets_)
    {
        WidgetDiagnosticEntry entry;
        entry.widgetId = widget.widgetId;
        entry.name = widget.name;
        entry.scriptPath = widget.filePath;
        entry.packageId = widget.packageId;
        entry.packageVersion = widget.manifest.version;
        entry.valid = widget.valid;
        entry.hasManifest = widget.manifest.hasManifest;
        entry.permissions = widget.manifest.permissions;
        if (widget.quota)
        {
            entry.memoryBytes = widget.quota->memoryBytes;
            entry.memoryLimit = widget.quota->memoryLimit;
            entry.lastCallbackMs = widget.quota->lastExecutionMs;
            entry.executionQuotaExceeded = widget.quota->executionExceeded;
            entry.memoryQuotaExceeded = widget.quota->memoryExceeded;
        }
        entry.circuitOpen = widget.health.CircuitOpen();
        std::string errorKey = WidgetWideToUtf8(widget.widgetId) + ".lastError";
        auto errIt = g_storage.find(errorKey);
        if (errIt != g_storage.end())
            entry.lastError = errIt->second;
        std::string logKey = WidgetWideToUtf8(widget.widgetId);
        entry.logs = g_widgetDiagnostics.EntriesFor(logKey);
        result.push_back(std::move(entry));
    }
    return result;
}

// ── List available widget scripts ────────────────────────────────
std::vector<std::wstring> WidgetEngine::ListAvailable()
{
    std::vector<std::wstring> result;
    for (const auto& package : GetWidgetPackageManager().ListPackages())
    {
        if (!package.active || !package.enabled) continue;
        LuaWidgetManifest manifest =
            GetWidgetManifest((package.root /
                Utf8ToWideLocal(package.manifest.entry)).wstring());
        if (!manifest.minHostVersion.empty() &&
            CompareVersions(SNOWDESKTOP_VERSION, manifest.minHostVersion) < 0)
            continue;
        result.push_back(Utf8ToWideLocal(package.manifest.id));
    }
    std::sort(result.begin(), result.end());
    return result;
}

void WidgetEngine::RuntimeOpenWidgetPanel(
    const std::wstring& widgetId, std::wstring title,
    int width, int height)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || !openWidgetPanelCallback_)
        return;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.title = std::move(title);
    request.width = std::clamp(width, 320, 900);
    request.height = std::clamp(height, 280, 900);
    openWidgetPanelCallback_(request);
}

void WidgetEngine::RuntimeCloseWidgetPanel(
    const std::wstring& widgetId)
{
    if (!snowdesktop::widget_runtime::IsDryLoad() && closeWidgetPanelCallback_)
        closeWidgetPanelCallback_(widgetId);
}

std::optional<std::wstring> WidgetEngine::RuntimeResolvePackageAsset(
    const std::wstring& widgetId, const std::wstring& relativePath) const
{
    if (!snowdesktop::widget::WidgetPackageValidator::IsSafeRelativePath(
        std::filesystem::path(relativePath)))
        return std::nullopt;
    const int index = FindWidget(widgetId);
    if (index < 0 || widgets_[index].packageRoot.empty())
        return std::nullopt;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        widgets_[index].packageRoot, error);
    const auto target = std::filesystem::weakly_canonical(
        root / relativePath, error);
    if (error) return std::nullopt;
    auto rootIt = root.begin();
    auto targetIt = target.begin();
    for (; rootIt != root.end(); ++rootIt, ++targetIt)
    {
        if (targetIt == target.end() ||
            _wcsicmp(rootIt->c_str(), targetIt->c_str()) != 0)
            return std::nullopt;
    }
    for (auto current = root; current != target; )
    {
        // Check each path component that already exists. Missing assets simply
        // fail at the image decoder without escaping the package.
        const auto relative = std::filesystem::relative(target, current, error);
        if (error || relative.empty()) break;
        current /= *relative.begin();
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return std::nullopt;
    }
    return target.wstring();
}

LuaWidgetManifest WidgetEngine::GetWidgetManifest(const std::wstring& filename)
{
    LuaWidgetManifest manifest;
    std::wstring fullPath = filename;
    if (PathIsRelativeW(fullPath.c_str()))
        fullPath = ResolveWidgetPath(filename);
    if (fullPath.empty())
        return manifest;

    std::wstring manifestPath = ManifestPathForScriptFile(fullPath);
    std::string text = ReadTextFile(manifestPath);
    if (text.empty())
        return manifest;

    JsonValue root;
    if (!ParseJson(text, root) || !root.IsObject())
        return manifest;
    manifest.hasManifest = true;

    auto readString = [](const JsonValue& object, const char* field,
        std::string& output) {
        const JsonValue* value = object.Find(field);
        if (!value || !value->IsString())
            return false;
        output = value->string;
        return true;
    };
    auto valueString = [](const JsonValue& value) -> std::string {
        if (value.IsString()) return value.string;
        if (value.IsBoolean()) return value.boolean ? "1" : "0";
        if (value.IsNumber())
        {
            std::ostringstream stream;
            stream << value.number;
            return stream.str();
        }
        return {};
    };
    auto readStringArray = [](const JsonValue& object, const char* field) {
        std::vector<std::string> result;
        const JsonValue* values = object.Find(field);
        if (!values || !values->IsArray())
            return result;
        for (const JsonValue& value : values->array)
            if (value.IsString())
                result.push_back(value.string);
        return result;
    };
    auto readNumber = [](const JsonValue& object, const char* field,
        double& output) {
        const JsonValue* value = object.Find(field);
        if (!value || !value->IsNumber())
            return false;
        output = value->number;
        return true;
    };
    auto readBool = [](const JsonValue& object, const char* field,
        bool& output) {
        const JsonValue* value = object.Find(field);
        if (!value || !value->IsBoolean())
            return false;
        output = value->boolean;
        return true;
    };

    readString(root, "id", manifest.packageId);
    readString(root, "slug", manifest.slug);
    double packageNumber = 0;
    if (readNumber(root, "schemaVersion", packageNumber))
        manifest.schemaVersion = static_cast<int>(packageNumber);
    if (readNumber(root, "apiVersion", packageNumber))
        manifest.apiVersion = static_cast<int>(packageNumber);
    if (readNumber(root, "dataVersion", packageNumber))
        manifest.dataVersion = static_cast<int>(packageNumber);
    readString(root, "name", manifest.name);
    readString(root, "nameKey", manifest.nameKey);
    readString(root, "version", manifest.version);
    readString(root, "description", manifest.description);
    readString(root, "descriptionKey", manifest.descriptionKey);
    manifest.titleKeys = readStringArray(root, "titleKeys");
    if (const JsonValue* locales = root.Find("locales");
        locales && locales->IsObject())
    {
        for (const auto& [language, values] : locales->object)
        {
            if (!values.IsObject())
                continue;
            std::unordered_map<std::string, std::string> catalog;
            bool valid = true;
            for (const auto& [key, value] : values.object)
            {
                if (!value.IsString())
                {
                    valid = false;
                    break;
                }
                catalog.emplace(key, value.string);
            }
            if (valid && !catalog.empty())
                manifest.locales.emplace(language, std::move(catalog));
        }
    }
    if (!manifest.nameKey.empty())
        manifest.name = TranslateManifest(manifest, manifest.nameKey, manifest.name);
    if (!manifest.descriptionKey.empty())
        manifest.description =
            TranslateManifest(manifest, manifest.descriptionKey, manifest.description);
    readString(root, "publisher", manifest.publisher);
    readString(root, "minHostVersion", manifest.minHostVersion);
    readString(root, "preview", manifest.preview);
    if (const JsonValue* previewData = root.Find("previewData");
        previewData && previewData->IsObject())
    {
        readString(*previewData, "introduction",
            manifest.previewIntroduction);
        readString(*previewData, "introductionKey",
            manifest.previewIntroductionKey);
        if (const JsonValue* storage = previewData->Find("storage");
            storage && storage->IsObject())
        {
            for (const auto& [key, value] : storage->object)
            {
                if (!key.empty() &&
                    (value.IsString() || value.IsNumber() || value.IsBoolean()))
                    manifest.previewStorage[key] = valueString(value);
            }
        }
        if (const JsonValue* storageKeys = previewData->Find("storageKeys");
            storageKeys && storageKeys->IsObject())
        {
            for (const auto& [storageKey, localizationKey] :
                storageKeys->object)
            {
                if (!storageKey.empty() && localizationKey.IsString() &&
                    !localizationKey.string.empty() &&
                    manifest.previewStorage.contains(storageKey))
                    manifest.previewStorageKeys[storageKey] =
                        localizationKey.string;
            }
        }
        if (const JsonValue* variants = previewData->Find("variants");
            variants && variants->IsArray())
        {
            for (const JsonValue& value : variants->array)
            {
                if (!value.IsObject() || manifest.previewVariants.size() >= 4)
                    continue;
                snowdesktop::widget::PreviewVariant variant;
                readString(value, "id", variant.id);
                readString(value, "title", variant.title);
                readString(value, "titleKey", variant.titleKey);
                readString(value, "description", variant.description);
                readString(value, "descriptionKey", variant.descriptionKey);
                if (const JsonValue* size = value.Find("size");
                    size && size->IsObject())
                {
                    double number = 0;
                    if (readNumber(*size, "columns", number) &&
                        std::isfinite(number))
                        variant.columns = std::clamp(
                            static_cast<int>(number), 1, 8);
                    if (readNumber(*size, "rows", number) &&
                        std::isfinite(number))
                        variant.rows = std::clamp(
                            static_cast<int>(number), 1, 8);
                }
                if (const JsonValue* variantStorage = value.Find("storage");
                    variantStorage && variantStorage->IsObject())
                {
                    for (const auto& [key, stored] : variantStorage->object)
                    {
                        if (!key.empty() && (stored.IsString() ||
                            stored.IsNumber() || stored.IsBoolean()))
                            variant.storage[key] = valueString(stored);
                    }
                }
                if (const JsonValue* variantStorageKeys =
                        value.Find("storageKeys");
                    variantStorageKeys && variantStorageKeys->IsObject())
                {
                    for (const auto& [storageKey, localizationKey] :
                        variantStorageKeys->object)
                    {
                        if (!storageKey.empty() &&
                            localizationKey.IsString() &&
                            !localizationKey.string.empty() &&
                            variant.storage.contains(storageKey))
                            variant.storageKeys[storageKey] =
                                localizationKey.string;
                    }
                }
                manifest.previewVariants.push_back(std::move(variant));
            }
        }
    }
    readString(root, "entry", manifest.entry);
    readString(root, "signature", manifest.signature);
    manifest.permissions = readStringArray(root, "permissions");
    manifest.networkDomains = readStringArray(root, "networkDomains");
    double refreshMs = 0;
    if (readNumber(root, "refreshIntervalMs", refreshMs) &&
        std::isfinite(refreshMs) && refreshMs > 0)
    {
        const double clamped = std::clamp(refreshMs,
            static_cast<double>(kWidgetRefreshMinIntervalMs),
            static_cast<double>(kWidgetRefreshMaxIntervalMs));
        manifest.refreshIntervalMs = static_cast<int>(clamped);
    }
    if (const JsonValue* settings = root.Find("settings");
        settings && settings->IsArray())
    {
        for (const JsonValue& object : settings->array)
        {
            if (!object.IsObject())
                continue;
            LuaWidgetManifest::Setting setting;
            readString(object, "key", setting.key);
            readString(object, "label", setting.label);
            readString(object, "type", setting.type);
            if (const JsonValue* defaultValue = object.Find("default"))
                setting.defaultValue = valueString(*defaultValue);
            readNumber(object, "min", setting.minValue);
            readNumber(object, "max", setting.maxValue);
            setting.options = readStringArray(object, "options");
            if (!setting.key.empty() && !setting.label.empty())
            {
                if (setting.type.empty()) setting.type = "text";
                if (!IsHostStructureSettingKey(setting.key) &&
                    !IsHostAppearanceSettingKey(setting.key))
                    manifest.settings.push_back(std::move(setting));
            }
        }
    }
    if (const JsonValue* presets = root.Find("presets");
        presets && presets->IsArray())
    {
        for (const JsonValue& object : presets->array)
        {
            if (!object.IsObject())
                continue;
            LuaWidgetManifest::SettingPreset preset;
            readString(object, "id", preset.id);
            readString(object, "label", preset.label);
            if (preset.label.empty())
                readString(object, "name", preset.label);
            readBool(object, "default", preset.isDefault);
            if (const JsonValue* values = object.Find("values");
                values && values->IsObject())
            {
                for (const auto& [key, value] : values->object)
                {
                    if (!IsHostStructureSettingKey(key))
                        preset.values[key] = valueString(value);
                }
            }
            if (!preset.id.empty() && !preset.label.empty() &&
                !preset.values.empty())
                manifest.presets.push_back(std::move(preset));
        }
    }
    auto readSize = [&](const char* field, int& columns, int& rows,
        int minimum, int maximum) {
        const JsonValue* size = root.Find(field);
        if (!size || !size->IsObject())
            return;
        auto normalize = [&](double value) {
            if (!std::isfinite(value))
                return minimum;
            const double upper = maximum > minimum
                ? static_cast<double>(maximum)
                : static_cast<double>((std::numeric_limits<int>::max)());
            return static_cast<int>(std::clamp(
                value, static_cast<double>(minimum), upper));
        };
        double value = 0;
        if (readNumber(*size, "columns", value))
            columns = normalize(value);
        if (readNumber(*size, "rows", value))
            rows = normalize(value);
    };
    readSize("defaultSize", manifest.defaultColumns, manifest.defaultRows, 1, 8);
    readSize("minSize", manifest.minColumns, manifest.minRows, 1, 1);
    readSize("maxSize", manifest.maxColumns, manifest.maxRows, 0, 0);

    if (manifest.maxColumns > 0)
        manifest.maxColumns = std::max(manifest.maxColumns, manifest.minColumns);
    if (manifest.maxRows > 0)
        manifest.maxRows = std::max(manifest.maxRows, manifest.minRows);
    manifest.defaultColumns = std::max(manifest.defaultColumns, manifest.minColumns);
    manifest.defaultRows = std::max(manifest.defaultRows, manifest.minRows);
    if (manifest.maxColumns > 0)
        manifest.defaultColumns = std::min(manifest.defaultColumns, manifest.maxColumns);
    if (manifest.maxRows > 0)
        manifest.defaultRows = std::min(manifest.defaultRows, manifest.maxRows);
    if (!manifest.previewIntroductionKey.empty())
    {
        manifest.previewIntroduction = TranslateManifest(manifest,
            manifest.previewIntroductionKey,
            manifest.previewIntroduction);
    }
    for (const auto& [storageKey, localizationKey] :
        manifest.previewStorageKeys)
    {
        if (auto value = manifest.previewStorage.find(storageKey);
            value != manifest.previewStorage.end())
            value->second = TranslateManifest(
                manifest, localizationKey, value->second);
    }
    for (auto& variant : manifest.previewVariants)
    {
        if (variant.columns <= 0) variant.columns = manifest.defaultColumns;
        if (variant.rows <= 0) variant.rows = manifest.defaultRows;
        if (!variant.titleKey.empty())
            variant.title = TranslateManifest(
                manifest, variant.titleKey, variant.title);
        if (!variant.descriptionKey.empty())
            variant.description = TranslateManifest(
                manifest, variant.descriptionKey, variant.description);
        for (const auto& [storageKey, localizationKey] : variant.storageKeys)
        {
            if (auto value = variant.storage.find(storageKey);
                value != variant.storage.end())
                value->second = TranslateManifest(
                    manifest, localizationKey, value->second);
        }
    }

    if (manifest.permissions.empty())
        manifest.permissions = {};
    if (!manifest.signature.empty())
    {
        std::string expected = manifest.signature;
        if (expected.starts_with("sha256:")) expected.erase(0, 7);
        std::transform(expected.begin(), expected.end(), expected.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        manifest.signatureValid = expected == Sha256File(fullPath);
    }
    return manifest;
}

bool WidgetEngine::InstallWidgetPackage(const std::wstring& manifestPath,
    std::wstring& error, bool allowSourceChange,
    bool allowPermissionExpansion,
    snowdesktop::widget::InstalledPackage* installedResult)
{
    const std::filesystem::path input(manifestPath);
    auto& manager = GetWidgetPackageManager();
    snowdesktop::widget::InstalledPackage installed;
    snowdesktop::widget::ValidationReport report;
    snowdesktop::widget::PackageManifest manifest;
    std::string installError;
    bool ok = false;
    if (_wcsicmp(input.extension().c_str(), L".snowwidget") == 0)
    {
        report = manager.ValidateArchive(input, &manifest);
        if (report.Ok())
            ok = manager.InstallArchive(input,
                { "local-import", manifest.id },
                allowSourceChange, installed, report, installError,
                allowPermissionExpansion);
    }
    else
    {
        const std::filesystem::path root =
            _wcsicmp(input.filename().c_str(), L"widget.json") == 0
            ? input.parent_path() : input;
        report = manager.ValidateDirectory(root, &manifest);
        if (report.Ok())
            ok = manager.InstallDirectory(root,
                { "local-import", manifest.id },
                allowSourceChange, installed, report, installError,
                allowPermissionExpansion);
    }
    if (!ok)
    {
        if (installError.empty()) installError = "package validation failed";
        error = Utf8ToWideLocal(installError);
        if (!report.Ok())
            error += L"\n" + Utf8ToWideLocal(report.ToJson());
        return false;
    }
    if (installedResult) *installedResult = installed;
    error.clear();
    return true;
}

bool WidgetEngine::VerifyInstalledWidgetPackage(const std::string& packageId,
    const std::optional<std::string>& previousVersion, std::wstring& error)
{
    if (snowdesktop::widget_runtime::HasStorageOverlay())
    {
        error = L"Another component storage migration is already running.";
        return false;
    }

    std::vector<std::wstring> liveInstances;
    for (const auto& widget : widgets_)
        if (widget.packageId == packageId)
            liveInstances.push_back(widget.widgetId);

    std::unordered_map<std::string, std::string> migratedStorage = g_storage;
    std::vector<std::wstring> updatedInstances;
    bool ok = true;
    std::string failure;
    {
        StorageOverlayScope migrationScope(&migratedStorage);
        if (liveInstances.empty())
        {
            const std::wstring dryId = Utf8ToWideLocal(
                "__package_dry_load_" +
                snowdesktop::widget::WidgetPackageManager::GenerateUuid());
            const std::wstring path =
                ResolveWidgetPath(Utf8ToWideLocal(packageId));
            {
                DryLoadScope dryLoadScope;
                ok = !path.empty() && LoadWidget(path, dryId);
            }
            if (!ok)
            {
                const auto stored = g_storage.find(
                    WidgetWideToUtf8(dryId) + ".lastError");
                if (stored != g_storage.end()) failure = stored->second;
            }
            DeleteWidgetInstance(dryId);
        }
        else
        {
            for (const auto& widgetId : liveInstances)
            {
                if (!ReloadWidget(widgetId))
                {
                    ok = false;
                    const auto stored = g_storage.find(
                        WidgetWideToUtf8(widgetId) + ".lastError");
                    if (stored != g_storage.end()) failure = stored->second;
                    break;
                }
                updatedInstances.push_back(widgetId);
            }
        }
    }
    if (ok)
    {
        g_storage = std::move(migratedStorage);
        SaveStorageFile();
        error.clear();
        return true;
    }

    RecoverWidgetPackage(packageId, previousVersion);
    for (const auto& widgetId : updatedInstances)
        (void)ReloadWidget(widgetId);
    error = Utf8ToWideLocal(failure.empty()
        ? "component dry-load or storage migration failed; restored last-known-good"
        : failure + "\nRestored last-known-good component version.");
    return false;
}

bool WidgetEngine::InstallAndVerifyWidgetPackage(const std::wstring& path,
    std::wstring& error, bool allowSourceChange,
    bool allowPermissionExpansion)
{
    snowdesktop::widget::PackageManifest incoming;
    snowdesktop::widget::ValidationReport report;
    const std::filesystem::path input(path);
    auto& manager = GetWidgetPackageManager();
    if (_wcsicmp(input.extension().c_str(), L".snowwidget") == 0)
        report = manager.ValidateArchive(input, &incoming);
    else
        report = manager.ValidateDirectory(
            _wcsicmp(input.filename().c_str(), L"widget.json") == 0
                ? input.parent_path() : input, &incoming);
    if (!report.Ok())
    {
        error = Utf8ToWideLocal(report.ToJson());
        return false;
    }
    std::optional<std::string> previousVersion;
    if (const auto previous = manager.Resolve(incoming.id))
        if (!previous->builtin && !previous->development)
            previousVersion = previous->manifest.version;
    snowdesktop::widget::InstalledPackage installed;
    if (!InstallWidgetPackage(path, error, allowSourceChange,
        allowPermissionExpansion, &installed))
        return false;
    return VerifyInstalledWidgetPackage(
        installed.manifest.id, previousVersion, error);
}

snowdesktop::widget::ProviderStatus
WidgetEngine::GetStaticWidgetCatalogStatus(
    const std::filesystem::path& catalogPath)
{
    snowdesktop::widget::StaticCatalogSource source(catalogPath);
    return source.Status();
}

void WidgetEngine::RegisterWidgetPackageSource(
    std::shared_ptr<snowdesktop::widget::IWidgetPackageSource> source)
{
    if (!source || source->ProviderId().empty()) return;
    GetWidgetPackageSources()[source->ProviderId()] = std::move(source);
}

bool WidgetEngine::ConfigureStaticWidgetCatalog(
    const std::filesystem::path& catalogPath, std::string& error)
{
    auto source =
        std::make_shared<snowdesktop::widget::StaticCatalogSource>(
            catalogPath);
    const auto status = source->Status();
    if (!status.available)
    {
        error = status.message;
        return false;
    }
    RegisterWidgetPackageSource(source);
    error.clear();
    return true;
}

std::vector<snowdesktop::widget::PackageSourceInfo>
WidgetEngine::ListWidgetPackageSources()
{
    std::vector<snowdesktop::widget::PackageSourceInfo> result;
    for (const auto& [providerId, source] : GetWidgetPackageSources())
        result.push_back({
            providerId, source->Capabilities(), source->Status() });
    std::sort(result.begin(), result.end(),
        [](const auto& left, const auto& right)
        {
            return left.providerId < right.providerId;
        });
    return result;
}

std::vector<snowdesktop::widget::PackageDetails>
WidgetEngine::QueryWidgetPackageSource(const std::string& providerId,
    const snowdesktop::widget::PackageQuery& query, std::string& error)
{
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end())
    {
        error = "component source is not registered: " + providerId;
        return {};
    }
    return source->second->Query(query, error);
}

bool WidgetEngine::IsSteamWorkshopBridgeAvailable()
{
    const std::filesystem::path bridge =
        std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopSteamBridge.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(bridge, error) || error)
        return false;
    const DWORD attributes = GetFileAttributesW(bridge.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool WidgetEngine::UnsubscribeSteamWorkshopItem(
    const std::string& externalItemId, std::string& error)
{
    const auto source = GetWidgetPackageSources().find("steam-workshop");
    if (source == GetWidgetPackageSources().end())
    {
        error = "Steam Workshop source is unavailable";
        return false;
    }
    const auto workshop = std::dynamic_pointer_cast<
        snowdesktop::widget::SteamWorkshopSource>(source->second);
    if (!workshop)
    {
        error = "Steam Workshop source is incompatible";
        return false;
    }
    return workshop->Unsubscribe(externalItemId, error);
}

snowdesktop::widget::SteamWorkshopSubscriptionSnapshot
WidgetEngine::QuerySteamWorkshopSubscriptions(
    const std::string& locale)
{
    snowdesktop::widget::SteamWorkshopSubscriptionSnapshot snapshot;
    try
    {
        snowdesktop::widget::SteamWorkshopSource source;
        snowdesktop::widget::PackageQuery query;
        query.locale = locale;
        query.limit = std::numeric_limits<std::size_t>::max();
        std::string error;
        // Runtime subscription reconciliation is deliberately local-only.
        // Steam's ACF is the subscription authority and content/<appid> is the
        // download authority; SteamAPI is reserved for explicit user actions.
        snapshot = source.QuerySubscriptions(query, error);
        if (!error.empty()) snapshot.error = std::move(error);
        if (snapshot.authoritative)
        {
            auto associations = snowdesktop::widget::
                BuildSteamWorkshopPackageAssociations(snapshot);
            auto& cache = GetSteamWorkshopPackageAssociationCache();
            std::lock_guard lock(cache.mutex);
            cache.associations = std::move(associations);
        }
    }
    catch (const std::exception& exception)
    {
        snapshot = {};
        snapshot.error = "Steam Workshop subscription query failed: ";
        snapshot.error += exception.what();
    }
    catch (...)
    {
        snapshot = {};
        snapshot.error = "Steam Workshop subscription query failed";
    }
    return snapshot;
}

snowdesktop::widget::SteamWorkshopSubscriptionHistory
WidgetEngine::GetSteamWorkshopSubscriptionHistory()
{
    return GetWidgetPackageManager().SteamSubscriptionHistory();
}

void WidgetEngine::PrepareSteamWorkshopSubscriptionArtifacts(
    snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const std::vector<snowdesktop::widget::InstalledPackage>& installed,
    const std::filesystem::path& stagingRoot)
{
    if (!snapshot.authoritative) return;
    const auto plan = snowdesktop::widget::BuildSteamWorkshopSyncPlan(
        installed, snapshot);
    if (plan.actions.empty()) return;

    std::error_code filesystemError;
    std::filesystem::create_directories(stagingRoot, filesystemError);
    if (filesystemError)
    {
        snapshot.preparationErrors.push_back(
            "cannot create local Workshop staging directory: " +
            filesystemError.message());
        return;
    }

    for (const auto& action : plan.actions)
    {
        if (action.kind == snowdesktop::widget::
                SteamWorkshopSyncActionKind::Uninstall)
            continue;
        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(action.externalItemId);
        const auto artifact =
            snapshot.localArtifacts.find(publishedFileId);
        if (artifact == snapshot.localArtifacts.end())
        {
            snapshot.preparationErrors.push_back(action.packageId +
                ": detected Workshop package has no local artifact");
            continue;
        }
        const auto destination = stagingRoot /
            Utf8ToWideLocal("workshop-" + action.packageId + "-" +
                snowdesktop::widget::WidgetPackageManager::GenerateUuid() +
                ".snowwidget");
        filesystemError.clear();
        std::filesystem::copy_file(artifact->second, destination,
            std::filesystem::copy_options::overwrite_existing,
            filesystemError);
        if (filesystemError)
        {
            snapshot.preparationErrors.push_back(action.packageId +
                ": cannot stage detected Workshop package: " +
                filesystemError.message());
            continue;
        }
        snapshot.preparedArtifacts[publishedFileId] = destination;
    }
}

std::unordered_map<std::string, std::string>
WidgetEngine::CachedSteamWorkshopPackageAssociations()
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    return cache.associations;
}

snowdesktop::widget::SteamWorkshopSyncResult
WidgetEngine::ApplySteamWorkshopSubscriptions(
    const snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot)
{
    auto& manager = GetWidgetPackageManager();
    const auto plan = snowdesktop::widget::BuildSteamWorkshopSyncPlan(
        manager.ListPackages(), snapshot);
    snowdesktop::widget::SteamWorkshopSyncResult result;
    result.errors = plan.conflicts;
    result.errors.insert(result.errors.end(),
        snapshot.preparationErrors.begin(), snapshot.preparationErrors.end());
    for (const auto& action : plan.actions)
    {
        if (action.kind ==
            snowdesktop::widget::SteamWorkshopSyncActionKind::Uninstall)
        {
            std::vector<std::wstring> instances;
            for (const auto& widget : widgets_)
                if (widget.packageId == action.packageId)
                    instances.push_back(widget.widgetId);
            for (const auto& instance : instances) UnloadWidget(instance);
            std::string error;
            if (manager.Uninstall(action.packageId, error))
            {
                ++result.uninstalled;
            }
            else
            {
                for (const auto& instance : instances)
                    (void)ReloadWidget(instance);
                result.errors.push_back(action.packageId + ": " + error);
            }
            continue;
        }

        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(action.externalItemId);
        const auto prepared =
            snapshot.preparedArtifacts.find(publishedFileId);
        if (prepared == snapshot.preparedArtifacts.end())
        {
            result.errors.push_back(action.packageId +
                ": detected Workshop package was not prepared locally");
            continue;
        }

        std::optional<std::string> previousVersion;
        if (const auto previous = manager.Resolve(action.packageId))
            if (!previous->builtin && !previous->development)
                previousVersion = previous->manifest.version;
        snowdesktop::widget::InstalledPackage installed;
        snowdesktop::widget::ValidationReport report;
        std::string installError;
        const snowdesktop::widget::PackageSourceRef sourceRef{
            "steam-workshop", action.externalItemId };
        const bool installedOk = manager.InstallArchive(prepared->second,
            sourceRef, false, installed, report, installError, false,
            &action.expectedManifest);
        std::error_code cleanupError;
        std::filesystem::remove(prepared->second, cleanupError);
        std::wstring error;
        const bool verified = installedOk && VerifyInstalledWidgetPackage(
            installed.manifest.id, previousVersion, error);
        if (installedOk && verified)
        {
            if (action.kind ==
                snowdesktop::widget::SteamWorkshopSyncActionKind::Install)
                ++result.installed;
            else
                ++result.updated;
        }
        else
        {
            if (!installedOk)
            {
                error = Utf8ToWideLocal(installError);
                if (!report.Ok())
                    error += L"\n" + Utf8ToWideLocal(report.ToJson());
            }
            result.errors.push_back(action.packageId + ": " +
                WidgetWideToUtf8(error));
        }
    }
    for (const auto& [publishedFileId, artifact] :
        snapshot.preparedArtifacts)
    {
        (void)publishedFileId;
        std::error_code cleanupError;
        std::filesystem::remove(artifact, cleanupError);
    }
    if (snapshot.authoritative && result.errors.empty() &&
        !snapshot.activeSteamAccountId.empty())
    {
        std::string historyError;
        if (!manager.UpdateSteamSubscriptionHistory(
                snapshot.activeSteamAccountId,
                snapshot.subscribedPublishedFileIds, historyError))
        {
            result.errors.push_back(
                "cannot save Steam subscription history: " + historyError);
        }
    }
    return result;
}

int WidgetEngine::ApplySafeWidgetPackageUpdates(
    const std::string& providerId, std::string& report)
{
    report.clear();
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end()) return 0;
    std::vector<snowdesktop::widget::PackageVersionRef> current;
    for (const auto& package : GetWidgetPackageManager().ListPackages())
    {
        if (!package.active || package.builtin || package.development ||
            package.source.providerId != providerId)
            continue;
        current.push_back({
            package.manifest.id, package.manifest.version });
    }
    if (current.empty()) return 0;
    std::string sourceError;
    const auto updates =
        source->second->CheckUpdates(current, sourceError);
    if (!sourceError.empty())
    {
        report = sourceError;
        return 0;
    }
    int applied = 0;
    for (const auto& update : updates)
    {
        std::wstring installError;
        if (InstallAndVerifyWidgetPackageFromSource(providerId,
            update.available.source.externalItemId,
            update.available.manifest.version, installError, false, false))
        {
            ++applied;
        }
        else
        {
            if (!report.empty()) report += '\n';
            report += update.current.packageId + ": " +
                WidgetWideToUtf8(installError);
        }
    }
    return applied;
}

std::vector<snowdesktop::widget::PackageDetails>
WidgetEngine::QueryStaticWidgetCatalog(
    const std::filesystem::path& catalogPath,
    const snowdesktop::widget::PackageQuery& query, std::string& error)
{
    snowdesktop::widget::StaticCatalogSource source(catalogPath);
    return source.Query(query, error);
}

bool WidgetEngine::InstallAndVerifyWidgetPackageFromSource(
    const std::string& providerId, const std::string& externalItemId,
    const std::string& version, std::wstring& error,
    bool allowSourceChange, bool allowPermissionExpansion)
{
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end())
    {
        error = Utf8ToWideLocal(
            "component source is not registered: " + providerId);
        return false;
    }
    std::string sourceError;
    const auto details = source->second->GetVersionDetails(
        externalItemId, version, sourceError);
    if (!details)
    {
        error = Utf8ToWideLocal(sourceError);
        return false;
    }
    std::optional<std::string> previousVersion;
    auto& manager = GetWidgetPackageManager();
    if (const auto previous = manager.Resolve(details->manifest.id))
        if (!previous->builtin && !previous->development)
            previousVersion = previous->manifest.version;
    snowdesktop::widget::InstalledPackage installed;
    snowdesktop::widget::ValidationReport report;
    if (!manager.InstallFromSource(*source->second, externalItemId, version,
        allowSourceChange, installed, report, sourceError,
        allowPermissionExpansion))
    {
        error = Utf8ToWideLocal(sourceError);
        if (!report.Ok()) error += L"\n" + Utf8ToWideLocal(report.ToJson());
        return false;
    }
    return VerifyInstalledWidgetPackage(
        installed.manifest.id, previousVersion, error);
}

bool WidgetEngine::InstallAndVerifyStaticWidgetPackage(
    const std::filesystem::path& catalogPath,
    const std::string& externalItemId, const std::string& version,
    std::wstring& error, bool allowSourceChange,
    bool allowPermissionExpansion)
{
    std::string sourceError;
    if (!ConfigureStaticWidgetCatalog(catalogPath, sourceError))
    {
        error = Utf8ToWideLocal(sourceError);
        return false;
    }
    return InstallAndVerifyWidgetPackageFromSource("static-catalog",
        externalItemId, version, error, allowSourceChange,
        allowPermissionExpansion);
}

snowdesktop::widget::PackagePaths WidgetEngine::GetWidgetPackagePaths()
{
    return GetWidgetPackageManager().Paths();
}

std::vector<snowdesktop::widget::InstalledPackage>
WidgetEngine::ListWidgetPackages()
{
    return GetWidgetPackageManager().ListPackages();
}

std::optional<snowdesktop::widget::InstalledPackage>
WidgetEngine::GetWidgetPackage(const std::wstring& packageId)
{
    const std::string requestedId = WidgetWideToUtf8(packageId);
    if (requestedId.empty()) return std::nullopt;
    return GetWidgetPackageManager().Resolve(requestedId);
}

bool WidgetEngine::SetWidgetPermissionDecision(
    const std::wstring& packageId,
    snowdesktop::widget::PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error)
{
    const std::string requestedId = WidgetWideToUtf8(packageId);
    if (requestedId.empty())
    {
        error = "package id is empty";
        return false;
    }
    return GetWidgetPackageManager().SetPermissionDecision(
        requestedId, state, grantedPermissions,
        grantedNetworkDomains, error);
}

std::optional<snowdesktop::widget::PackageSourceRef>
WidgetEngine::GetWidgetPackageSource(const std::wstring& packageId)
{
    const std::string requestedId = WidgetWideToUtf8(packageId);
    if (requestedId.empty()) return std::nullopt;

    std::optional<snowdesktop::widget::PackageSourceRef> fallback;
    for (const auto& package : GetWidgetPackageManager().ListPackages())
    {
        if (package.manifest.id != requestedId ||
            package.source.providerId.empty() ||
            package.source.externalItemId.empty())
            continue;
        if (package.source.providerId == "steam-workshop" &&
            !snowdesktop::widget::SteamPublishedFileId(
                package.source.externalItemId).empty())
            return package.source;
        if (!fallback || (package.active && package.enabled))
            fallback = package.source;
    }
    return fallback;
}

bool WidgetEngine::IsWidgetPackageAvailable(const std::wstring& packageId)
{
    return !packageId.empty() &&
        GetWidgetPackageManager().ResolveEntry(
            WidgetWideToUtf8(packageId)).has_value();
}

bool WidgetEngine::IsWidgetPackageInstalled(const std::wstring& packageId)
{
    const std::string requestedId = WidgetWideToUtf8(packageId);
    return !requestedId.empty() &&
        GetWidgetPackageManager().ContainsPackage(requestedId);
}

std::vector<snowdesktop::widget::LegacyPackage>
WidgetEngine::ListLegacyWidgetPackages()
{
    return GetWidgetPackageManager().FindLegacyPackages();
}

std::optional<std::wstring> WidgetEngine::ResolveLegacyWidgetPackage(
    const std::wstring& legacyName)
{
    const auto packageId =
        GetWidgetPackageManager().ResolveLegacyPackageId(legacyName);
    return packageId
        ? std::optional<std::wstring>(Utf8ToWideLocal(*packageId))
        : std::nullopt;
}

snowdesktop::widget::LegacyMigrationResult
WidgetEngine::MigrateLegacyWidgetPackage(
    const snowdesktop::widget::LegacyPackage& legacy)
{
    return GetWidgetPackageManager().MigrateLegacy(legacy);
}

bool WidgetEngine::SetWidgetPackageEnabled(const std::string& packageId,
    bool enabled, std::string& error)
{
    return GetWidgetPackageManager().SetEnabled(packageId, enabled, error);
}

bool WidgetEngine::CreateWidgetDevelopmentProject(
    const std::string& packageId, std::filesystem::path& projectRoot,
    std::string& error)
{
    return GetWidgetPackageManager().CreateDevelopmentProject(
        packageId, projectRoot, error);
}

bool WidgetEngine::SetWidgetDevelopmentOverride(
    const std::string& packageId, bool active, std::string& error)
{
    return GetWidgetPackageManager().SetDevelopmentOverride(
        packageId, active, error);
}

bool WidgetEngine::RollbackWidgetPackage(const std::string& packageId,
    const std::string& version, std::string& error)
{
    return GetWidgetPackageManager().Rollback(packageId, version, error);
}

bool WidgetEngine::UninstallWidgetPackage(const std::string& packageId,
    std::string& error)
{
    return GetWidgetPackageManager().Uninstall(packageId, error);
}

bool WidgetEngine::GetWidgetDefaultSpan(const std::wstring& filename, int& columns, int& rows)
{
    LuaWidgetManifest manifest = GetWidgetManifest(filename);
    columns = std::max(1, manifest.defaultColumns);
    rows = std::max(1, manifest.defaultRows);
    return manifest.hasManifest;
}

std::wstring WidgetEngine::GetWidgetDisplayName(const std::wstring& filename)
{
    LuaWidgetManifest manifest = GetWidgetManifest(filename);
    if (!manifest.name.empty())
        return Utf8ToWideLocal(manifest.name);

    std::wstring fallback = filename;
    if (fallback.size() > 4 && fallback.substr(fallback.size() - 4) == L".lua")
        fallback = fallback.substr(0, fallback.size() - 4);
    return fallback;
}

bool WidgetEngine::IsWidgetDefaultName(const std::wstring& filename,
    const std::wstring& title)
{
    if (title.empty())
        return false;
    const LuaWidgetManifest manifest = GetWidgetManifest(filename);
    std::vector<std::string> managedKeys = manifest.titleKeys;
    if (!manifest.nameKey.empty())
        managedKeys.push_back(manifest.nameKey);
    for (const auto& [language, catalog] : manifest.locales)
    {
        (void)language;
        for (const std::string& key : managedKeys)
        {
            auto value = catalog.find(key);
            if (value != catalog.end() &&
                Utf8ToWideLocal(value->second) == title)
                return true;
        }
    }
    return !manifest.name.empty() && Utf8ToWideLocal(manifest.name) == title;
}

static int lua_DrawLine(lua_State* L)
{
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float x2 = (float)luaL_checknumber(L, 3);
    float y2 = (float)luaL_checknumber(L, 4);
    float thick = (float)luaL_optnumber(L, 5, 1);
    int color = (int)luaL_optinteger(L, 6, 0xFFFFFF);
    float alpha = (float)luaL_optnumber(L, 7, 1.0);

    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    s->ctx->DrawLine(
        D2D1::Point2F(x1 + s->widgetRect.left, y1 + s->widgetRect.top),
        D2D1::Point2F(x2 + s->widgetRect.left, y2 + s->widgetRect.top),
        brush, thick);
    return 0;
}

static int lua_DrawCircle(lua_State* L)
{
    float cx = (float)luaL_checknumber(L, 1);
    float cy = (float)luaL_checknumber(L, 2);
    float r = (float)luaL_checknumber(L, 3);
    int color = (int)luaL_optinteger(L, 4, 0xFFFFFF);
    float alpha = (float)luaL_optnumber(L, 5, 1.0);

    auto* s = GetD2D(L);
    if (!s || !s->ctx) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color, alpha);
    if (!brush) return 0;

    s->ctx->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(cx + s->widgetRect.left, cy + s->widgetRect.top), r, r),
        brush);
    return 0;
}

static int LuaDrawFontGlyph(lua_State* L, bool fluent)
{
    const char* glyph = luaL_checkstring(L, 1);
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float size = static_cast<float>(luaL_optnumber(L, 4, 20));
    int color = static_cast<int>(luaL_optinteger(L, 5, 0xFFFFFF));

    auto* s = GetD2D(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, glyph, -1, nullptr, 0);
    if (wlen <= 1) return 0;
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, glyph, -1, wtext.data(), wlen);

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        DWRITE_FONT_WEIGHT_NORMAL, true, DWRITE_WORD_WRAPPING_NO_WRAP,
        !fluent, false, fluent);
    if (!format) return 0;

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color);
    if (!brush) return 0;

    float bx = x + s->widgetRect.left;
    float by = y + s->widgetRect.top;

    // Use a layout box much larger than size so DirectWrite centering has room
    // to work even when the glyph's advance width equals the font size (1em).
    // The box is centered on the target rect's center.
    const float box = size * 4.0f;
    const float boxX = bx + size * 0.5f - box * 0.5f;
    const float boxY = by + size * 0.5f - box * 0.5f;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s->dwrite->CreateTextLayout(wtext.c_str(),
            static_cast<UINT32>(wtext.size() - 1), format, box, box, &layout)) || !layout)
    {
        D2D1_RECT_F rect = { bx, by, bx + size, by + size };
        s->ctx->DrawTextW(wtext.c_str(), static_cast<UINT32>(wtext.size() - 1),
            format, &rect, brush);
        return 0;
    }

    // GetOverhangMetrics returns how far the drawn pixels extend beyond each
    // edge of the layout box. Positive = overhang, negative = gap (inside).
    // Visual center offset from box center = (overhang.right - overhang.left) / 2.
    // Shift the drawing origin to align the visual center with the target center.
    DWRITE_OVERHANG_METRICS overhang{};
    float drawX = boxX;
    float drawY = boxY;
    if (SUCCEEDED(layout->GetOverhangMetrics(&overhang)))
    {
        drawX -= (overhang.right - overhang.left) * 0.5f;
        drawY -= (overhang.bottom - overhang.top) * 0.5f;
    }

    s->ctx->DrawTextLayout(D2D1::Point2F(drawX, drawY), layout.Get(), brush);
    return 0;
}

static int lua_DrawFa(lua_State* L)
{
    return LuaDrawFontGlyph(L, false);
}

static int lua_DrawFluent(lua_State* L)
{
    return LuaDrawFontGlyph(L, true);
}

static int lua_StorageGet(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) { lua_pushnil(L); return 1; }
    std::string fullKey = BoundStoragePrefix(L) + "." + key;
    auto it = g_storage.find(fullKey);
    if (it != g_storage.end())
        lua_pushstring(L, it->second.c_str());
    else if (s->engine)
    {
        std::string defaultValue =
            s->engine->RuntimeGetStorageValue(BoundWidgetId(L), key);
        if (!defaultValue.empty())
            lua_pushstring(L, defaultValue.c_str());
        else
            lua_pushnil(L);
    }
    else
        lua_pushnil(L);
    return 1;
}

static bool StorageWriteWithinQuota(const std::string& prefix,
    const std::string& key, const std::string& value)
{
    constexpr std::size_t kMaxStorageKeys = 256;
    constexpr std::size_t kMaxStorageKeyBytes = 128;
    constexpr std::size_t kMaxStorageValueBytes = 64 * 1024;
    constexpr std::size_t kMaxStorageBytes = 1024 * 1024;
    if (key.empty() || key.size() > kMaxStorageKeyBytes ||
        value.size() > kMaxStorageValueBytes)
        return false;
    const std::string fullKey = prefix + "." + key;
    std::size_t count = 0;
    std::size_t bytes = 0;
    for (const auto& [storedKey, storedValue] : ActiveStorage())
    {
        if (!storedKey.starts_with(prefix + ".")) continue;
        if (storedKey == fullKey) continue;
        ++count;
        bytes += storedKey.size() + storedValue.size();
    }
    return count < kMaxStorageKeys &&
        bytes + fullKey.size() + value.size() <= kMaxStorageBytes;
}

static int lua_StorageSet(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    auto* s = GetD2D(L);
    if (!s || IsRemovedPanelEffectSettingKey(key)) return 0;
    const std::string prefix = BoundStoragePrefix(L);
    if (!StorageWriteWithinQuota(prefix, key, value))
        return luaL_error(L, "widget storage quota exceeded");
    ActiveStorage()[prefix + "." + key] = value;
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
    return 0;
}

// ── ImGui Lua API ──────────────────────────────────────────────────
static int lua_ImGuiText(lua_State* L) { ImGui::Text("%s", luaL_checkstring(L, 1)); return 0; }
static int lua_ImGuiTextWrapped(lua_State* L) { ImGui::TextWrapped("%s", luaL_checkstring(L, 1)); return 0; }

static int lua_ImGuiSeparator(lua_State* L)
{
    (void)L;
    ImGui::Separator();
    return 0;
}

static int lua_ImGuiSameLine(lua_State* L)
{
    float offset = (float)luaL_optnumber(L, 1, 0.0);
    float spacing = (float)luaL_optnumber(L, 2, -1.0);
    ImGui::SameLine(offset, spacing);
    return 0;
}

static int lua_ImGuiSettingRow(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    const float requestedWidth = static_cast<float>(
        luaL_optnumber(L, 2, 300.0));
    const float rowStart = ImGui::GetCursorPosX();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float rowRight = rowStart + availableWidth;
    const float maximumWidth = std::max(
        ImGui::GetFrameHeight(), availableWidth * 0.58f);
    const float controlWidth = std::min(
        std::max(ImGui::GetFrameHeight(), requestedWidth), maximumWidth);
    const float controlX = std::max(rowStart, rowRight - controlWidth);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(controlX);
    ImGui::SetNextItemWidth(controlWidth);
    return 0;
}

static int lua_ImGuiSpacing(lua_State* L)
{
    (void)L;
    ImGui::Spacing();
    return 0;
}

static int lua_ImGuiCollapsingHeader(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool open = ImGui::CollapsingHeader(label);
    lua_pushboolean(L, open);
    return 1;
}

static int lua_ImGuiTreeNode(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool open = ImGui::TreeNode(label);
    lua_pushboolean(L, open);
    return 1;
}

static int lua_ImGuiTreePop(lua_State* L)
{
    (void)L;
    ImGui::TreePop();
    return 0;
}

static int lua_ImGuiButton(lua_State* L)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    bool clicked = ImGui::Button(luaL_checkstring(L, 1));
    ImGui::PopStyleColor();
    lua_pushboolean(L, clicked);
    return 1;
}

static int lua_ImGuiColorEdit3(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int hex = (int)luaL_checkinteger(L, 2);
    float col[3] = {
        ((hex >> 16) & 0xFF) / 255.0f,
        ((hex >> 8) & 0xFF) / 255.0f,
        (hex & 0xFF) / 255.0f
    };
    if (ImGui::ColorEdit3(label, col, ImGuiColorEditFlags_NoInputs))
    {
        int r = (int)(col[0] * 255);
        int g = (int)(col[1] * 255);
        int b = (int)(col[2] * 255);
        lua_pushinteger(L, (r << 16) | (g << 8) | b);
    }
    else
        lua_pushinteger(L, hex);
    return 1;
}

static int lua_ImGuiSliderFloat(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    float min = (float)luaL_checknumber(L, 3);
    float max = (float)luaL_checknumber(L, 4);
    if (ImGui::SliderFloat(label, &v, min, max))
        lua_pushnumber(L, v);
    else
        lua_pushnumber(L, (double)luaL_checknumber(L, 2));
    return 1;
}

static int lua_ImGuiInputText(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    const char* text = luaL_optstring(L, 2, "");
    char buf[4096]{};
    strncpy_s(buf, sizeof(buf), text, _TRUNCATE);
    if (ImGui::InputTextMultiline(label, buf, sizeof(buf), ImVec2(-1, 120)))
        lua_pushstring(L, buf);
    else
        lua_pushstring(L, text);
    return 1;
}

static int lua_ImGuiInputTextSingle(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    const char* text = luaL_optstring(L, 2, "");
    char buf[4096]{};
    strncpy_s(buf, sizeof(buf), text, _TRUNCATE);
    if (ImGui::InputText(label, buf, sizeof(buf)))
        lua_pushstring(L, buf);
    else
        lua_pushstring(L, text);
    return 1;
}

static int lua_ImGuiCheckbox(lua_State* L)
{
    bool v = lua_toboolean(L, 2) != 0;
    if (ImGui::Checkbox(luaL_checkstring(L, 1), &v))
        lua_pushboolean(L, v);
    else
        lua_pushboolean(L, lua_toboolean(L, 2) != 0);
    return 1;
}

static int lua_ImGuiSliderInt(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int v = (int)luaL_checkinteger(L, 2);
    int min = (int)luaL_checkinteger(L, 3);
    int max = (int)luaL_checkinteger(L, 4);
    if (ImGui::SliderInt(label, &v, min, max))
        lua_pushinteger(L, v);
    else
        lua_pushinteger(L, (int)luaL_checkinteger(L, 2));
    return 1;
}

static int lua_ImGuiCombo(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    int current = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    std::vector<std::string> items;
    const int count = (int)lua_rawlen(L, 3);
    items.reserve(count);
    for (int i = 1; i <= count; ++i)
    {
        lua_rawgeti(L, 3, i);
        items.emplace_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    std::string preview = (current >= 1 && current <= count) ? items[(size_t)current - 1] : "";
    if (ImGui::BeginCombo(label, preview.c_str()))
    {
        for (int i = 0; i < count; ++i)
        {
            ImGui::PushID(i);
            bool selected = (current == i + 1);
            if (ImGui::Selectable(items[(size_t)i].c_str(), selected))
                current = i + 1;
            if (selected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    lua_pushinteger(L, current);
    return 1;
}

static int lua_ImGuiSelectable(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool selected = lua_toboolean(L, 2) != 0;
    bool clicked = ImGui::Selectable(label, selected);
    lua_pushboolean(L, clicked);
    return 1;
}

static int lua_ImGuiRadio(lua_State* L)
{
    const char* label = luaL_checkstring(L, 1);
    bool active = lua_toboolean(L, 2) != 0;
    bool clicked = ImGui::RadioButton(label, active);
    lua_pushboolean(L, clicked);
    return 1;
}

static int lua_ImGuiBeginDisabled(lua_State* L)
{
    bool disabled = lua_toboolean(L, 1) != 0;
    ImGui::BeginDisabled(disabled);
    return 0;
}

static int lua_ImGuiEndDisabled(lua_State* L)
{
    (void)L;
    ImGui::EndDisabled();
    return 0;
}

static int lua_StorageRemove(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) return 0;
    ActiveStorage().erase(BoundStoragePrefix(L) + "." + key);
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
    return 0;
}

static int lua_StorageKeys(lua_State* L)
{
    auto* s = GetD2D(L);
    std::string prefix = s ? BoundStoragePrefix(L) + "." : "";
    lua_newtable(L);
    int idx = 1;
    for (const auto& kv : ActiveStorage())
    {
        if (!prefix.empty() && kv.first.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string key = prefix.empty() ? kv.first : kv.first.substr(prefix.size());
        lua_pushstring(L, key.c_str());
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

static int lua_LayoutWidth(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushnumber(L, s ? s->widgetRect.right - s->widgetRect.left : 400);
    return 1;
}

static int lua_LayoutHeight(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushnumber(L, s ? s->widgetRect.bottom - s->widgetRect.top : 300);
    return 1;
}

static int lua_LayoutColumns(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushinteger(L, s ? s->gridColumns : 1);
    return 1;
}

static int lua_LayoutRows(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushinteger(L, s ? s->gridRows : 1);
    return 1;
}

static int lua_LayoutSizeClass(lua_State* L)
{
    auto* s = GetD2D(L);
    int area = s ? s->gridColumns * s->gridRows : 1;
    lua_pushstring(L, area <= 2 ? "small" : (area <= 6 ? "medium" : "large"));
    return 1;
}

static int lua_LayoutCellWidth(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushinteger(L, s ? std::max(4, s->gridCellW) : 92);
    return 1;
}

static int lua_LayoutCellHeight(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushinteger(L, s ? std::max(4, s->gridCellH) : 116);
    return 1;
}

static int lua_LayoutCellScale(lua_State* L)
{
    auto* s = GetD2D(L);
    const int cellWidth = s ? std::max(4, s->gridCellW) : kCellWidth;
    const int cellHeight = s ? std::max(4, s->gridCellH) : kMinCellHeight;
    lua_pushnumber(L, CalculateWidgetCellScale(cellWidth, cellHeight));
    return 1;
}

static int lua_LayoutCu(lua_State* L)
{
    const float value = static_cast<float>(luaL_checknumber(L, 1));
    auto* s = GetD2D(L);
    const int cellWidth = s ? std::max(4, s->gridCellW) : kCellWidth;
    const int cellHeight = s ? std::max(4, s->gridCellH) : kMinCellHeight;
    lua_pushinteger(L, static_cast<lua_Integer>(std::round(value * CalculateWidgetCellScale(cellWidth, cellHeight))));
    return 1;
}

static int lua_LayoutFontCu(lua_State* L)
{
    const float value = static_cast<float>(luaL_checknumber(L, 1));
    auto* s = GetD2D(L);
    const int cellWidth = s ? std::max(4, s->gridCellW) : kCellWidth;
    const int cellHeight = s ? std::max(4, s->gridCellH) : kMinCellHeight;
    lua_pushnumber(L, snowdesktop::font_cu_rules::Scale(
        value, CalculateWidgetCellScale(cellWidth, cellHeight)));
    return 1;
}

static int lua_LayoutCellGap(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushinteger(L, s ? std::max(0, s->gridGapY) : 8);
    return 1;
}

static int lua_LayoutBarHeight(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_pushinteger(L, s ? std::max(16, s->barHeight) : 24);
    return 1;
}

static int lua_L10nTr(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    lua_getfield(L, lua_upvalueindex(1), key);
    std::string result = lua_isstring(L, -1) ? lua_tostring(L, -1) : key;
    lua_pop(L, 1);
    const int argumentCount = lua_gettop(L);
    for (int index = 2; index <= argumentCount; ++index)
    {
        size_t length = 0;
        const char* value = luaL_tolstring(L, index, &length);
        const std::string placeholder =
            "{" + std::to_string(index - 2) + "}";
        size_t position = 0;
        while ((position = result.find(placeholder, position)) != std::string::npos)
        {
            result.replace(position, placeholder.size(), value, length);
            position += length;
        }
        lua_pop(L, 1);
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nLanguage(lua_State* L)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    lua_pushlstring(L, language.data(), language.size());
    return 1;
}

static void PushWidgetL10nAPI(lua_State* L, const LuaWidgetManifest& manifest)
{
    lua_newtable(L);

    lua_newtable(L);
    auto pushCatalog = [L](
        const std::unordered_map<std::string, std::string>& catalog) {
        for (const auto& [key, value] : catalog)
        {
            lua_pushlstring(L, value.data(), value.size());
            lua_setfield(L, -2, key.c_str());
        }
    };
    auto english = manifest.locales.find("en-US");
    if (english != manifest.locales.end())
        pushCatalog(english->second);

    const auto* selected = SelectManifestLocale(manifest);
    if (selected && (english == manifest.locales.end() ||
        selected != &english->second))
    {
        pushCatalog(*selected);
    }
    lua_pushcclosure(L, lua_L10nTr, 1);
    lua_setfield(L, -2, "tr");

    lua_pushcfunction(L, lua_L10nLanguage);
    lua_setfield(L, -2, "language");
}

void WidgetEngine::RegisterDrawAPI(lua_State* L)
{
    using snowdesktop::widget_api::DescribeLibrary;
    using snowdesktop::widget_api::FunctionDescriptor;
    using snowdesktop::widget_api::LibraryDescriptor;
    using snowdesktop::widget_api::RegisterLibraries;

    static constexpr FunctionDescriptor draw[] = {
        { "text", lua_DrawText },
        { "measureText", lua_MeasureText },
        { "rect", lua_DrawRect },
        { "pushClip", lua_DrawPushClip },
        { "popClip", lua_DrawPopClip },
        { "strokeRect", lua_DrawStrokeRect },
        { "line", lua_DrawLine },
        { "circle", lua_DrawCircle },
        { "fa", lua_DrawFa },
        { "fluent", lua_DrawFluent },
        { "image", lua_DrawImage },
        { "icon", lua_DrawIcon, 1, "desktop.read" },
    };
    static constexpr FunctionDescriptor widget[] = {
        { "info", lua_WidgetInfo },
        { "setTitle", lua_WidgetSetTitle },
        { "openSettings", lua_WidgetOpenSettings },
        { "openPanel", lua_WidgetOpenPanel },
        { "closePanel", lua_WidgetClosePanel },
        { "invalidate", lua_WidgetInvalidate },
        { "log", lua_WidgetLog },
        { "theme", lua_WidgetTheme },
        { "editText", lua_WidgetEditText },
        { "setTimer", lua_WidgetSetTimer },
        { "cancelTimer", lua_WidgetCancelTimer },
    };
    static constexpr FunctionDescriptor system[] = {
        { "getTime", lua_GetTime },
        { "notify", lua_Notify, 1, "ui.notify" },
        { "cpu", lua_SystemCpu, 1, "system.read" },
        { "memory", lua_SystemMemory, 1, "system.read" },
        { "battery", lua_SystemBattery, 1, "system.read" },
        { "network", lua_SystemNetwork, 1, "system.read" },
        { "gpu", lua_SystemGpu, 1, "system.read" },
    };
    static constexpr FunctionDescriptor media[] = {
        { "current", lua_MediaCurrent, 1, "media.read" },
        { "playPause", lua_MediaPlayPause, 1, "media.action" },
        { "next", lua_MediaNext, 1, "media.action" },
        { "previous", lua_MediaPrevious, 1, "media.action" },
    };
    static constexpr FunctionDescriptor http[] = {
        { "request", lua_HttpRequest, 1, "network.http" },
        { "cancel", lua_HttpCancel, 1, "network.http" },
    };
    static constexpr FunctionDescriptor ui[] = {
        { "textInput", lua_UiTextInput },
        { "textArea", lua_UiTextArea },
        { "focusInput", lua_UiFocusInput },
        { "button", lua_UiButton },
        { "toggle", lua_UiToggle },
        { "progress", lua_UiProgress },
        { "scrollArea", lua_UiScrollArea },
        { "virtualList", lua_UiVirtualList },
        { "setScrollOffset", lua_UiSetScrollOffset },
    };
    static constexpr FunctionDescriptor desktop[] = {
        { "items", lua_DesktopItems, 1, "desktop.read" },
        { "selection", lua_DesktopSelection, 1, "desktop.read" },
        { "find", lua_DesktopFind, 1, "desktop.read" },
        { "findApplications", lua_DesktopFindApplications, 1,
            "desktop.read" },
        { "open", lua_DesktopOpen, 1, "desktop.action" },
        { "reveal", lua_DesktopReveal, 1, "desktop.action" },
        { "refresh", lua_DesktopRefresh, 1, "desktop.action" },
    };
    static constexpr FunctionDescriptor everything[] = {
        { "search", lua_EverythingSearch, 1, "everything.search" },
    };
    static constexpr FunctionDescriptor calendar[] = {
        { "selectedDate", lua_CalendarSelectedDate, 1,
            "calendar.read" },
        { "setSelectedDate", lua_CalendarSetSelectedDate, 1,
            "calendar.write" },
        { "dateInfo", lua_CalendarDateInfo, 1, "calendar.read" },
        { "addDays", lua_CalendarAddDays, 1, "calendar.read" },
        { "events", lua_CalendarEvents, 1, "calendar.read" },
        { "create", lua_CalendarCreate, 1, "calendar.write" },
        { "update", lua_CalendarUpdate, 1, "calendar.write" },
        { "remove", lua_CalendarRemove, 1, "calendar.write" },
    };
    static constexpr FunctionDescriptor layout[] = {
        { "width", lua_LayoutWidth },
        { "height", lua_LayoutHeight },
        { "columns", lua_LayoutColumns },
        { "rows", lua_LayoutRows },
        { "sizeClass", lua_LayoutSizeClass },
        { "cellWidth", lua_LayoutCellWidth },
        { "cellHeight", lua_LayoutCellHeight },
        { "cellScale", lua_LayoutCellScale },
        { "cu", lua_LayoutCu },
        { "fontCu", lua_LayoutFontCu },
        { "cellGap", lua_LayoutCellGap },
        { "barHeight", lua_LayoutBarHeight },
    };
    static constexpr FunctionDescriptor storage[] = {
        { "get", lua_StorageGet },
        { "set", lua_StorageSet },
        { "remove", lua_StorageRemove },
        { "keys", lua_StorageKeys },
    };
    static constexpr FunctionDescriptor imgui[] = {
        { "text", lua_ImGuiText },
        { "textWrapped", lua_ImGuiTextWrapped },
        { "separator", lua_ImGuiSeparator },
        { "sameLine", lua_ImGuiSameLine },
        { "settingRow", lua_ImGuiSettingRow },
        { "spacing", lua_ImGuiSpacing },
        { "collapsingHeader", lua_ImGuiCollapsingHeader },
        { "treeNode", lua_ImGuiTreeNode },
        { "treePop", lua_ImGuiTreePop },
        { "button", lua_ImGuiButton },
        { "input", lua_ImGuiInputText },
        { "inputText", lua_ImGuiInputTextSingle },
        { "checkbox", lua_ImGuiCheckbox },
        { "colorEdit3", lua_ImGuiColorEdit3 },
        { "sliderFloat", lua_ImGuiSliderFloat },
        { "sliderInt", lua_ImGuiSliderInt },
        { "combo", lua_ImGuiCombo },
        { "selectable", lua_ImGuiSelectable },
        { "radio", lua_ImGuiRadio },
        { "beginDisabled", lua_ImGuiBeginDisabled },
        { "endDisabled", lua_ImGuiEndDisabled },
    };
    static constexpr LibraryDescriptor libraries[] = {
        DescribeLibrary("draw", draw),
        DescribeLibrary("widget", widget),
        DescribeLibrary("sys", system),
        DescribeLibrary("media", media),
        DescribeLibrary("http", http),
        DescribeLibrary("ui", ui),
        DescribeLibrary("desktop", desktop),
        DescribeLibrary("everything", everything),
        DescribeLibrary("calendar", calendar),
        DescribeLibrary("layout", layout),
        DescribeLibrary("storage", storage),
        DescribeLibrary("imgui", imgui),
    };

    RegisterLibraries(L, libraries);
}
