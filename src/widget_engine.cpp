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
#include "widget_logical_slot_manifest.h"
#include "widget_scroll_rules.h"
#include "data_paths.h"
#include "deployment_context.h"
#include "json_value.h"
#include "l10n.h"
#include "system_snapshot.h"
#include "constants.h"
#include "utils.h"
#include "font_cu_rules.h"
#include "name_pinyin.h"
#include "search_match.h"
#include "personalization.h"
#include "widget_package.h"
#include "steam_workshop_source.h"
#include "widget_api_registry.h"
#include "widget_l10n_format.h"
#include "widget_time.h"
#include "widget_permission_broker.h"
#include "widget_preview_context.h"
#include "widget_interaction_region.h"
#include "widget_draw_geometry.h"
#include "widget_view_lua.h"
#include "widget_view_tree.h"
#include "widget_resource_lua.h"
#include "widget_text_input_rules.h"
#include "widget_storage_transaction.h"
#include "widget_system_settings.h"
#include "widgets/widget_chrome_rules.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <dwrite_3.h>
#include <dwmapi.h>

#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <set>
#include <span>
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

static bool IsValidUtf8Local(const std::string& value)
{
    if (value.empty()) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
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
    std::vector<snowdesktop::widget::SteamWorkshopInstallFailure>
        installFailures;
};

static SteamWorkshopPackageAssociationCache&
GetSteamWorkshopPackageAssociationCache()
{
    static SteamWorkshopPackageAssociationCache cache;
    return cache;
}

static void CacheSteamWorkshopInstallFailure(
    snowdesktop::widget::SteamWorkshopInstallFailure failure)
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    auto existing = std::find_if(cache.installFailures.begin(),
        cache.installFailures.end(), [&](const auto& candidate)
        {
            return candidate.packageId == failure.packageId;
        });
    if (existing == cache.installFailures.end())
        cache.installFailures.push_back(std::move(failure));
    else
        *existing = std::move(failure);
}

static void ClearSteamWorkshopInstallFailure(
    const std::string& packageId)
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    std::erase_if(cache.installFailures, [&](const auto& failure)
    {
        return failure.packageId == packageId;
    });
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

struct PrivateFontResource
{
    ComPtr<IDWriteFontCollection1> collection;
    std::wstring familyName;
};

struct RuntimeImageResource
{
    std::shared_ptr<const snowdesktop::widget_runtime::
        WidgetRuntimeImagePixels> pixels;
    std::wstring ownerWidgetId;
    std::string source;
};

struct D2DState
{
    ID2D1DeviceContext* ctx = nullptr;
    IDWriteFactory* dwrite = nullptr;
    D2D1_RECT_F widgetRect{};
    WidgetEngine* engine = nullptr;
    std::string storagePrefix;
    std::wstring currentWidgetId;
    const char* surfaceKind = "desktop";
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
    std::unordered_map<std::string, RuntimeImageResource> runtimeImages;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap1>>
        runtimeImageBitmaps;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> shellIconCache;
    std::unordered_set<std::wstring> shellIconFailures;
    std::unique_ptr<AsyncShellIconLoader> shellIconLoader;
    ID2D1DeviceContext* brushContext = nullptr;
    std::unordered_map<std::uint32_t, ComPtr<ID2D1SolidColorBrush>> brushCache;
    std::unordered_map<std::uint64_t, ComPtr<IDWriteTextFormat>> textFormatCache;
    std::unordered_map<std::wstring, PrivateFontResource> privateFonts;
    std::unordered_map<std::wstring, ComPtr<IDWriteTextFormat>>
        privateTextFormatCache;
};

static std::string BindRuntimeImageToken(
    std::string_view token, std::wstring_view widgetId)
{
    if (!snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(token) ||
        widgetId.empty())
        return {};
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t character : widgetId)
    {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= prime;
    }
    std::string result(token);
    result.push_back(':');
    result.append(std::to_string(hash));
    return result.size() <= 64 ? result : std::string{};
}

static std::string RegisterRuntimeImageSource(D2DState* state,
    const std::wstring& widgetId, std::string_view token,
    std::shared_ptr<const snowdesktop::widget_runtime::
        WidgetRuntimeImagePixels> pixels,
    std::string source)
{
    const std::string boundToken = BindRuntimeImageToken(token, widgetId);
    if (!state || boundToken.empty() || !pixels ||
        !snowdesktop::widget_runtime::IsValidWidgetRuntimeImage(*pixels) ||
        source.empty())
        return {};
    const std::size_t ownerCount = static_cast<std::size_t>(std::count_if(
        state->runtimeImages.begin(), state->runtimeImages.end(),
        [&widgetId](const auto& item) {
            return item.second.ownerWidgetId == widgetId;
        }));
    if ((!state->runtimeImages.contains(boundToken) && ownerCount >= 16) ||
        state->runtimeImages.size() >= 128)
    {
        std::vector<std::string> removed;
        for (const auto& [key, resource] : state->runtimeImages)
        {
            if (resource.ownerWidgetId == widgetId) removed.push_back(key);
        }
        for (const auto& key : removed)
        {
            state->runtimeImages.erase(key);
            state->runtimeImageBitmaps.erase(key);
        }
    }
    if (!state->runtimeImages.contains(boundToken) &&
        state->runtimeImages.size() >= 128)
    {
        const std::string evicted = state->runtimeImages.begin()->first;
        state->runtimeImages.erase(evicted);
        state->runtimeImageBitmaps.erase(evicted);
    }
    state->runtimeImages.insert_or_assign(
        boundToken, RuntimeImageResource{
            std::move(pixels), widgetId, std::move(source) });
    return boundToken;
}

static void ClearRuntimeImagesForWidget(
    D2DState* state, const std::wstring& widgetId)
{
    if (!state || widgetId.empty()) return;
    std::vector<std::string> removed;
    for (const auto& [key, resource] : state->runtimeImages)
    {
        if (resource.ownerWidgetId == widgetId) removed.push_back(key);
    }
    for (const auto& key : removed)
    {
        state->runtimeImages.erase(key);
        state->runtimeImageBitmaps.erase(key);
    }
}

static void ClearRuntimeImagesForSource(
    D2DState* state, std::string_view source)
{
    if (!state || source.empty()) return;
    std::vector<std::string> removed;
    for (const auto& [key, resource] : state->runtimeImages)
    {
        if (resource.source == source) removed.push_back(key);
    }
    for (const auto& key : removed)
    {
        state->runtimeImages.erase(key);
        state->runtimeImageBitmaps.erase(key);
    }
}

static PrivateFontResource* LoadPrivateFont(
    D2DState* state, const std::wstring& path)
{
    if (!state || !state->dwrite || path.empty()) return nullptr;
    if (auto found = state->privateFonts.find(path);
        found != state->privateFonts.end())
        return &found->second;
    ComPtr<IDWriteFactory3> factory;
    if (FAILED(state->dwrite->QueryInterface(IID_PPV_ARGS(&factory))) ||
        !factory)
        return nullptr;
    ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(state->dwrite->CreateFontFileReference(
            path.c_str(), nullptr, &fontFile)) || !fontFile)
        return nullptr;
    ComPtr<IDWriteFontSetBuilder> baseBuilder;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory->CreateFontSetBuilder(&baseBuilder)) || !baseBuilder ||
        FAILED(baseBuilder.As(&builder)) || !builder ||
        FAILED(builder->AddFontFile(fontFile.Get())))
        return nullptr;
    ComPtr<IDWriteFontSet> fontSet;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(baseBuilder->CreateFontSet(&fontSet)) || !fontSet ||
        FAILED(factory->CreateFontCollectionFromFontSet(
            fontSet.Get(), &collection)) || !collection ||
        collection->GetFontFamilyCount() == 0)
        return nullptr;
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(collection->GetFontFamily(0, &family)) || !family ||
        FAILED(family->GetFamilyNames(&names)) || !names ||
        names->GetCount() == 0)
        return nullptr;
    UINT32 nameIndex = 0;
    BOOL exists = FALSE;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0)
        names->FindLocaleName(locale, &nameIndex, &exists);
    if (!exists)
        names->FindLocaleName(L"en-US", &nameIndex, &exists);
    if (!exists) nameIndex = 0;
    UINT32 nameLength = 0;
    if (FAILED(names->GetStringLength(nameIndex, &nameLength)) ||
        nameLength == 0 || nameLength > 255)
        return nullptr;
    std::wstring familyName(nameLength + 1, L'\0');
    if (FAILED(names->GetString(nameIndex, familyName.data(),
            nameLength + 1)))
        return nullptr;
    familyName.resize(nameLength);
    auto [inserted, added] = state->privateFonts.emplace(path,
        PrivateFontResource{ std::move(collection), std::move(familyName) });
    return added ? &inserted->second : nullptr;
}

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
    bool fluent = false, const std::wstring* privateFontPath = nullptr)
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
    if (privateFontPath)
    {
        const std::wstring privateKey = *privateFontPath + L"#" +
            std::to_wstring(key);
        if (auto found = state->privateTextFormatCache.find(privateKey);
            found != state->privateTextFormatCache.end())
            return found->second.Get();
        PrivateFontResource* resource = LoadPrivateFont(
            state, *privateFontPath);
        if (!resource) return nullptr;
        ComPtr<IDWriteTextFormat> format;
        state->dwrite->CreateTextFormat(resource->familyName.c_str(),
            resource->collection.Get(), weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        if (!format) return nullptr;
        format->SetTextAlignment(centered
            ? DWRITE_TEXT_ALIGNMENT_CENTER
            : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(centered || verticallyCentered
            ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
            : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping(wrapping);
        if (state->privateTextFormatCache.size() >= 128)
            state->privateTextFormatCache.clear();
        return state->privateTextFormatCache.emplace(
            privateKey, std::move(format)).first->second.Get();
    }
    if (auto found = state->textFormatCache.find(key);
        found != state->textFormatCache.end())
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
    setting.searchKey = LuaReadStorageField(L, tableIndex, "searchKey");
    setting.emptyLabel = LuaReadStorageField(L, tableIndex, "emptyLabel");
    setting.noResultsLabel =
        LuaReadStorageField(L, tableIndex, "noResultsLabel");

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

    lua_getfield(L, tableIndex, "optionLabels");
    if (lua_istable(L, -1))
    {
        int labelsIndex = lua_absindex(L, -1);
        for (int i = 1;; ++i)
        {
            lua_rawgeti(L, labelsIndex, i);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
            setting.optionLabels.push_back(
                LuaValueToStorageString(L, -1));
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

static std::uint64_t BoundWidgetRuntimeToken(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_runtime_token");
    const lua_Integer value = lua_isinteger(state, -1)
        ? lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

static int BoundWidgetApiVersion(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_api_version");
    const lua_Integer value = lua_isinteger(state, -1)
        ? lua_tointeger(state, -1) : 1;
    lua_pop(state, 1);
    return std::max(1, static_cast<int>(value));
}

static std::optional<std::wstring> ResolvePackageAssetWithinRoot(
    const std::filesystem::path& packageRoot,
    const std::wstring& relativePath);

static std::optional<std::wstring> ResolveCurrentPackageAsset(
    lua_State* state, const std::wstring& relativePath)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_package_root");
    size_t length = 0;
    const char* rootRaw = lua_tolstring(state, -1, &length);
    const std::filesystem::path root = rootRaw
        ? Utf8ToWideLocal(std::string(rootRaw, length))
        : std::wstring{};
    lua_pop(state, 1);
    if (root.empty()) return std::nullopt;
    return ResolvePackageAssetWithinRoot(root, relativePath);
}

static std::optional<snowdesktop::widget::PackageResource>
CurrentPackageResource(lua_State* state, const std::string& name,
    const char* expectedType = nullptr)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_resources");
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return std::nullopt;
    }
    lua_getfield(state, -1, name.c_str());
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 2);
        return std::nullopt;
    }

    snowdesktop::widget::PackageResource resource;
    auto readField = [&](const char* field, std::string& output) {
        lua_getfield(state, -1, field);
        size_t fieldLength = 0;
        const char* value = lua_tolstring(state, -1, &fieldLength);
        if (value) output.assign(value, fieldLength);
        lua_pop(state, 1);
    };
    readField("type", resource.type);
    readField("path", resource.path);
    readField("license", resource.license);
    lua_pop(state, 2);
    if (resource.type.empty() || resource.path.empty() ||
        (expectedType && resource.type != expectedType))
        return std::nullopt;
    return resource;
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
        surfaceKind_ = state_->surfaceKind;
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
        state_->surfaceKind = surfaceKind_;
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
    const char* surfaceKind_ = "desktop";
    int gridColumns_ = 1;
    int gridRows_ = 1;
    snowdesktop::widget_runtime::LayoutMetrics layoutMetrics_;
    int widgetClipDepth_ = 0;
};

class WidgetSurfaceScope
{
public:
    WidgetSurfaceScope(D2DState* state, const char* surfaceKind)
        : state_(state)
    {
        if (!state_) return;
        previous_ = state_->surfaceKind;
        state_->surfaceKind = surfaceKind ? surfaceKind : "desktop";
    }

    ~WidgetSurfaceScope()
    {
        if (state_) state_->surfaceKind = previous_;
    }

    WidgetSurfaceScope(const WidgetSurfaceScope&) = delete;
    WidgetSurfaceScope& operator=(const WidgetSurfaceScope&) = delete;

private:
    D2DState* state_ = nullptr;
    const char* previous_ = "desktop";
};

static void SetWidgetRectContext(D2DState* state, RECT bounds)
{
    if (!state || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    state->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
}

using snowdesktop::widget_runtime::LuaResourceType;
using snowdesktop::widget_runtime::PushResourceHandle;
using snowdesktop::widget_runtime::TestResourceHandle;

static std::optional<std::wstring> ResolveResourceHandlePath(
    lua_State* L, int index, LuaResourceType expected)
{
    const auto* handle = TestResourceHandle(L, index);
    if (!handle || handle->type != expected) return std::nullopt;
    const char* type = expected == LuaResourceType::Image ? "image" : "font";
    const auto resource = CurrentPackageResource(L, handle->name, type);
    if (!resource) return std::nullopt;
    return ResolveCurrentPackageAsset(L, Utf8ToWideLocal(resource->path));
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

    std::optional<std::wstring> privateFontPath;
    if (!lua_isnoneornil(L, 11))
    {
        privateFontPath = ResolveResourceHandlePath(
            L, 11, LuaResourceType::Font);
        if (!privateFontPath)
            return luaL_error(L, "draw.text: invalid font resource handle");
    }

    auto* s = GetD2D(L);
    if (!s || !s->ctx || !s->dwrite) return 0;

    IDWriteTextFormat* format = GetCachedTextFormat(s, size,
        bold ? DWRITE_FONT_WEIGHT_BOLD : s->itemFontWeight,
        false, maxWidth > 0 && singleLine
            ? DWRITE_WORD_WRAPPING_NO_WRAP
            : DWRITE_WORD_WRAPPING_WRAP,
        false, false, false,
        privateFontPath ? &*privateFontPath : nullptr);
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
    std::optional<std::wstring> privateFontPath;
    if (!lua_isnoneornil(L, 5))
    {
        privateFontPath = ResolveResourceHandlePath(
            L, 5, LuaResourceType::Font);
        if (!privateFontPath)
            return luaL_error(L,
                "draw.measureText: invalid font resource handle");
    }

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
        false, DWRITE_WORD_WRAPPING_WRAP, false, false, false,
        privateFontPath ? &*privateFontPath : nullptr);
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

constexpr char kSystemPerformancePermission[] =
    "system.performance.read";
constexpr char kSystemPowerPermission[] = "system.power.read";
constexpr char kSystemNetworkPermission[] = "system.network.read";
constexpr char kSystemStoragePermission[] = "system.storage.read";
constexpr char kSystemDisplayPermission[] = "system.display.read";
constexpr char kAudioOutputReadPermission[] = "audio.output.read";
constexpr char kAudioOutputAnalyzePermission[] = "audio.output.analyze";
constexpr char kAudioOutputControlPermission[] = "audio.output.control";
constexpr char kMediaReadPermission[] = "media.read";
constexpr char kMediaActionPermission[] = "media.action";
constexpr char kDesktopReadPermission[] = "desktop.read";
constexpr char kDesktopActionPermission[] = "desktop.action";
constexpr char kEverythingSearchPermission[] = "everything.search";
constexpr char kCalendarReadPermission[] = "calendar.read";
constexpr char kCalendarWritePermission[] = "calendar.write";
constexpr char kNetworkInternetPermission[] = "network.internet";
constexpr char kShellLaunchPermission[] = "shell.launch";
constexpr char kAppDiscoveryPermission[] = "app.discovery";
constexpr char kAppLaunchPermission[] = "app.launch";
constexpr char kNotificationPostPermission[] = "notification.post";
constexpr char kClipboardReadPermission[] = "clipboard.read";
constexpr char kClipboardWritePermission[] = "clipboard.write";
constexpr char kFilesystemReadPermission[] =
    "filesystem.userSelected.read";
constexpr char kFilesystemWritePermission[] =
    "filesystem.userSelected.write";
constexpr char kFilesystemWatchPermission[] =
    "filesystem.userSelected.watch";

static bool IsFilesystemPickerTask(std::string_view taskName)
{
    return taskName == "filesystem.pickOpen" ||
        taskName == "filesystem.pickSave" ||
        taskName == "filesystem.pickFolder";
}

static bool IsFilesystemHandleTask(std::string_view taskName)
{
    return snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::
            SupportsAction(taskName) ||
        taskName == "filesystem.release";
}

static bool IsPreviewFilesystemHandle(std::string_view handle)
{
    return handle == "filesystem:00000000000000000000000000000000" ||
        handle == "filesystem:11111111111111111111111111111111";
}

static bool ReadInteractionValue(lua_State* state, int index,
    snowdesktop::widget_runtime::InteractionValue& output,
    std::size_t depth, std::size_t& nodes, std::size_t& stringBytes,
    std::unordered_set<const void*>& ancestors, std::string& error)
{
    using Value = snowdesktop::widget_runtime::InteractionValue;
    index = lua_absindex(state, index);
    if (++nodes > 256 || depth > 8)
    {
        error = "interaction action value exceeds its size or depth limit";
        return false;
    }
    switch (lua_type(state, index))
    {
    case LUA_TNIL:
        output.type = Value::Type::Null;
        return true;
    case LUA_TBOOLEAN:
        output.type = Value::Type::Boolean;
        output.boolean = lua_toboolean(state, index) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index))
        {
            output.type = Value::Type::Integer;
            output.integer = static_cast<long long>(lua_tointeger(state, index));
        }
        else
        {
            output.type = Value::Type::Number;
            output.number = static_cast<double>(lua_tonumber(state, index));
            if (!std::isfinite(output.number))
            {
                error = "interaction action numbers must be finite";
                return false;
            }
        }
        return true;
    case LUA_TSTRING:
    {
        std::size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        if (length > 16 * 1024 || stringBytes > 16 * 1024 - length)
        {
            error = "interaction action strings exceed 16 KiB";
            return false;
        }
        stringBytes += length;
        output.type = Value::Type::String;
        output.string.assign(value ? value : "", length);
        return true;
    }
    case LUA_TTABLE:
        break;
    default:
        error = "interaction action values must be serializable";
        return false;
    }

    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = "interaction action tables cannot have metatables";
        return false;
    }
    const void* identity = lua_topointer(state, index);
    if (!ancestors.insert(identity).second)
    {
        error = "interaction action values cannot be cyclic";
        return false;
    }

    bool integerKeys = false;
    bool stringKeys = false;
    std::size_t count = 0;
    std::size_t maximumIndex = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        ++count;
        if (lua_isinteger(state, -2))
        {
            const lua_Integer key = lua_tointeger(state, -2);
            if (key <= 0 || key > 256)
            {
                lua_pop(state, 2);
                ancestors.erase(identity);
                error = "interaction action array keys must be contiguous";
                return false;
            }
            integerKeys = true;
            maximumIndex = std::max(maximumIndex,
                static_cast<std::size_t>(key));
        }
        else if (lua_type(state, -2) == LUA_TSTRING)
            stringKeys = true;
        else
        {
            lua_pop(state, 2);
            ancestors.erase(identity);
            error = "interaction action object keys must be strings";
            return false;
        }
        lua_pop(state, 1);
    }
    if ((integerKeys && stringKeys) ||
        (integerKeys && maximumIndex != count))
    {
        ancestors.erase(identity);
        error = "interaction action tables must be arrays or objects";
        return false;
    }

    if (integerKeys)
    {
        output.type = Value::Type::Array;
        output.array.resize(count);
        for (std::size_t item = 0; item < count; ++item)
        {
            lua_rawgeti(state, index, static_cast<lua_Integer>(item + 1));
            if (!ReadInteractionValue(state, -1, output.array[item],
                    depth + 1, nodes, stringBytes, ancestors, error))
            {
                lua_pop(state, 1);
                ancestors.erase(identity);
                return false;
            }
            lua_pop(state, 1);
        }
    }
    else
    {
        output.type = Value::Type::Object;
        lua_pushnil(state);
        while (lua_next(state, index) != 0)
        {
            std::size_t keyLength = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            if (!key || keyLength == 0 || keyLength > 128 ||
                stringBytes > 16 * 1024 - keyLength)
            {
                lua_pop(state, 2);
                ancestors.erase(identity);
                error = "interaction action object key is invalid";
                return false;
            }
            stringBytes += keyLength;
            Value child;
            if (!ReadInteractionValue(state, -1, child, depth + 1,
                    nodes, stringBytes, ancestors, error))
            {
                lua_pop(state, 2);
                ancestors.erase(identity);
                return false;
            }
            output.object.emplace(std::string(key, keyLength),
                std::move(child));
            lua_pop(state, 1);
        }
    }
    ancestors.erase(identity);
    return true;
}

static void PushInteractionValue(lua_State* state,
    const snowdesktop::widget_runtime::InteractionValue& value)
{
    using Type = snowdesktop::widget_runtime::InteractionValue::Type;
    switch (value.type)
    {
    case Type::Null:
        lua_pushnil(state);
        break;
    case Type::Boolean:
        lua_pushboolean(state, value.boolean ? 1 : 0);
        break;
    case Type::Integer:
        lua_pushinteger(state, static_cast<lua_Integer>(value.integer));
        break;
    case Type::Number:
        lua_pushnumber(state, static_cast<lua_Number>(value.number));
        break;
    case Type::String:
        lua_pushlstring(state, value.string.data(), value.string.size());
        break;
    case Type::Array:
        lua_createtable(state, static_cast<int>(value.array.size()), 0);
        for (std::size_t index = 0; index < value.array.size(); ++index)
        {
            PushInteractionValue(state, value.array[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        break;
    case Type::Object:
        lua_createtable(state, 0, static_cast<int>(value.object.size()));
        for (const auto& [key, child] : value.object)
        {
            PushInteractionValue(state, child);
            lua_setfield(state, -2, key.c_str());
        }
        break;
    }
}

static std::string ReadRequiredStringField(lua_State* state, int table,
    const char* field)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    std::size_t length = 0;
    const char* value = luaL_checklstring(state, -1, &length);
    std::string result(value ? value : "", length);
    lua_pop(state, 1);
    return result;
}

static int lua_InteractionRegion(lua_State* state)
{
    using namespace snowdesktop::widget_runtime;
    luaL_checktype(state, 1, LUA_TTABLE);
    const int descriptor = lua_absindex(state, 1);
    InteractionRegion region;
    region.key = ReadRequiredStringField(state, descriptor, "key");

    lua_getfield(state, descriptor, "shape");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int shape = lua_absindex(state, -1);
    const std::string type = ReadRequiredStringField(state, shape, "type");
    if (type == "rect")
        region.shape.type = InteractionShapeType::Rect;
    else if (type == "roundedRect")
        region.shape.type = InteractionShapeType::RoundedRect;
    else if (type == "circle")
        region.shape.type = InteractionShapeType::Circle;
    else
        return luaL_error(state, "interaction.region: unsupported shape type");
    lua_getfield(state, shape, "x");
    region.shape.x = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, shape, "y");
    region.shape.y = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    if (region.shape.type == InteractionShapeType::Circle)
    {
        lua_getfield(state, shape, "radius");
        region.shape.radius = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
    }
    else
    {
        lua_getfield(state, shape, "width");
        region.shape.width = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, shape, "height");
        region.shape.height = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, shape, "radius");
        if (!lua_isnil(state, -1))
            region.shape.radius = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "cursor");
    if (!lua_isnil(state, -1))
        region.cursor = luaL_checkstring(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "enabled");
    region.enabled = lua_isnil(state, -1) || lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "accessibility");
    if (lua_istable(state, -1))
    {
        lua_getfield(state, -1, "role");
        if (!lua_isnil(state, -1))
            region.accessibilityRole = luaL_checkstring(state, -1);
        lua_pop(state, 1);
        lua_getfield(state, -1, "label");
        if (!lua_isnil(state, -1))
            region.accessibilityLabel = luaL_checkstring(state, -1);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "events");
    if (lua_istable(state, -1))
    {
        const int events = lua_absindex(state, -1);
        for (const char* eventName : { "pointerEnter", "pointerLeave",
            "pointerDown", "pointerUp", "pointerMove", "click",
            "doubleClick", "wheel", "contextMenu" })
        {
            lua_getfield(state, events, eventName);
            if (lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                continue;
            }
            luaL_checktype(state, -1, LUA_TTABLE);
            const int actionTable = lua_absindex(state, -1);
            InteractionAction action;
            action.id = ReadRequiredStringField(state, actionTable, "id");
            lua_getfield(state, actionTable, "scope");
            if (!lua_isnil(state, -1))
            {
                const char* scope = luaL_checkstring(state, -1);
                if (std::strcmp(scope, "component") == 0)
                    action.contextMenuScope =
                        InteractionAction::ContextMenuScope::Component;
                else if (std::strcmp(scope, "element") != 0)
                    return luaL_error(state,
                        "interaction.region: action scope must be element or component");
            }
            lua_pop(state, 1);
            lua_getfield(state, actionTable, "value");
            std::size_t nodes = 0;
            std::size_t bytes = 0;
            std::unordered_set<const void*> ancestors;
            std::string error;
            if (!ReadInteractionValue(state, -1, action.value, 0,
                    nodes, bytes, ancestors, error))
                return luaL_error(state, "interaction.region: %s", error.c_str());
            lua_pop(state, 1);
            region.events.emplace(eventName, std::move(action));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    auto* d2d = GetD2D(state);
    std::string error;
    if (!d2d || !d2d->engine ||
        !d2d->engine->RuntimeSubmitInteractionRegion(
            BoundWidgetId(state), std::move(region), error))
    {
        return luaL_error(state, "interaction.region: %s",
            error.empty() ? "host context is unavailable" : error.c_str());
    }
    return 0;
}

static int lua_InteractionIsHovered(lua_State* state)
{
    const char* key = luaL_checkstring(state, 1);
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeInteractionHovered(
            BoundWidgetId(state), key ? key : ""));
    return 1;
}

static int lua_InteractionIsPressed(lua_State* state)
{
    const char* key = luaL_checkstring(state, 1);
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeInteractionPressed(
            BoundWidgetId(state), key ? key : ""));
    return 1;
}

static int lua_InteractionScroll(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    const int descriptor = lua_absindex(state, 1);
    const std::string key = ReadRequiredStringField(
        state, descriptor, "key");
    if (key.size() > 128 || key.find('\0') != std::string::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "interaction.scroll: key must contain 1 to 128 bytes of valid UTF-8");
    }

    lua_getfield(state, descriptor, "shape");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int shape = lua_absindex(state, -1);
    if (ReadRequiredStringField(state, shape, "type") != "rect")
        return luaL_error(state,
            "interaction.scroll: shape type must be 'rect'");
    const auto readNumber = [&](const char* field) {
        lua_getfield(state, shape, field);
        const double value = luaL_checknumber(state, -1);
        lua_pop(state, 1);
        return value;
    };
    const double x = readNumber("x");
    const double y = readNumber("y");
    const double width = readNumber("width");
    const double height = readNumber("height");
    lua_pop(state, 1);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        std::abs(x) > 1'000'000.0 || std::abs(y) > 1'000'000.0 ||
        width <= 0.0 || height <= 0.0 ||
        width > 1'000'000.0 || height > 1'000'000.0)
    {
        return luaL_error(state,
            "interaction.scroll: shape geometry must be finite, positive, and bounded");
    }

    lua_getfield(state, descriptor, "contentHeight");
    const lua_Integer requestedContentHeight =
        luaL_checkinteger(state, -1);
    lua_pop(state, 1);
    if (requestedContentHeight < 0 ||
        requestedContentHeight > 1'000'000)
    {
        return luaL_error(state,
            "interaction.scroll: contentHeight must be from 0 to 1000000");
    }

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "interaction.scroll: host context is unavailable");
    const int viewportHeight = std::max(
        1, static_cast<int>(std::lround(height)));
    const int contentHeight = std::max(
        viewportHeight, static_cast<int>(requestedContentHeight));
    LuaWidget::HostControl control;
    control.type = LuaWidget::HostControl::Type::Scroll;
    control.id = key;
    control.contentHeight = contentHeight;
    control.viewportHeight = viewportHeight;
    control.rect = {
        static_cast<LONG>(std::lround(x)),
        static_cast<LONG>(std::lround(y)),
        static_cast<LONG>(std::lround(x + width)),
        static_cast<LONG>(std::lround(y + height)),
    };
    d2d->engine->RuntimeRegisterHostControl(
        BoundWidgetId(state), std::move(control));
    const int offset = d2d->engine->RuntimeGetScrollOffset(
        BoundWidgetId(state), key);
    lua_createtable(state, 0, 4);
    lua_pushinteger(state, offset);
    lua_setfield(state, -2, "offset");
    lua_pushinteger(state, std::max(0, contentHeight - viewportHeight));
    lua_setfield(state, -2, "maximum");
    lua_pushinteger(state, viewportHeight);
    lua_setfield(state, -2, "viewportHeight");
    lua_pushinteger(state, contentHeight);
    lua_setfield(state, -2, "contentHeight");
    return 1;
}

static int lua_InteractionSetScrollOffset(lua_State* state)
{
    std::size_t keyLength = 0;
    const char* keyValue = luaL_checklstring(state, 1, &keyLength);
    const std::string key(keyValue ? keyValue : "", keyLength);
    if (key.size() > 128 || key.find('\0') != std::string::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "interaction.setScrollOffset: key must contain 1 to 128 bytes of valid UTF-8");
    }
    const lua_Integer requested = luaL_checkinteger(state, 2);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "interaction.setScrollOffset: host context is unavailable");
    d2d->engine->RuntimeSetScrollOffset(BoundWidgetId(state), key,
        static_cast<int>(std::clamp<lua_Integer>(
            requested, 0, 1'000'000)));
    lua_pushinteger(state, d2d->engine->RuntimeGetScrollOffset(
        BoundWidgetId(state), key));
    return 1;
}

static int lua_UiMenu(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    return 1;
}

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
    if (!RequirePermission(L, kSystemPerformancePermission)) return 0;
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
    if (!RequirePermission(L, kSystemPerformancePermission)) return 0;
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
    if (!RequirePermission(L, kSystemPowerPermission)) return 0;
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
    if (!RequirePermission(L, kSystemNetworkPermission)) return 0;
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
    if (!RequirePermission(L, kSystemPerformancePermission)) return 0;
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

static int LuaScheduleSet(lua_State* state, bool repeat,
    const char* functionName)
{
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxNameBytes)
    {
        return luaL_error(state,
            "%s: id must contain 1 to 128 bytes", functionName);
    }
    const lua_Integer delay = luaL_checkinteger(state, 2);
    if (delay <= 0 || delay >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxIntervalMs)
    {
        return luaL_error(state,
            "%s: milliseconds must be between 1 and 86400000",
            functionName);
    }
    auto hiddenPolicy =
        snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 3))
    {
        luaL_checktype(state, 3, LUA_TTABLE);
        lua_getfield(state, 3, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t valueLength = 0;
            const char* raw = luaL_checklstring(
                state, -1, &valueLength);
            const std::string_view value(raw, valueLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "%s: whenHidden must be 'pause', 'throttle', or 'continue'",
                    functionName);
        }
        lua_pop(state, 1);
    }
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeSetTimer(BoundWidgetId(state),
            std::string(name, nameLength), static_cast<int>(delay),
            repeat, hiddenPolicy));
    return 1;
}

static int lua_ScheduleAt(lua_State* state)
{
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxNameBytes)
    {
        return luaL_error(state,
            "schedule.at: id must contain 1 to 128 bytes");
    }
    const lua_Integer epochMilliseconds = luaL_checkinteger(state, 2);
    const auto wallNow = std::chrono::system_clock::now();
    const auto wallNowMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wallNow.time_since_epoch()).count();
    if (epochMilliseconds < 0 ||
        epochMilliseconds > wallNowMilliseconds +
            snowdesktop::widget_runtime::NamedTimerSchedule::
                MaxAbsoluteDelayMs)
    {
        return luaL_error(state,
            "schedule.at: epochMilliseconds must be a non-negative UTC timestamp no more than 366 days in the future");
    }
    auto hiddenPolicy =
        snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 3))
    {
        luaL_checktype(state, 3, LUA_TTABLE);
        lua_getfield(state, 3, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t valueLength = 0;
            const char* raw = luaL_checklstring(
                state, -1, &valueLength);
            const std::string_view value(raw, valueLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "schedule.at: whenHidden must be 'pause', 'throttle', or 'continue'");
        }
        lua_pop(state, 1);
    }
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeSetTimerAt(BoundWidgetId(state),
            std::string(name, nameLength),
            static_cast<std::int64_t>(epochMilliseconds), hiddenPolicy));
    return 1;
}

static int lua_ScheduleEvery(lua_State* state)
{
    return LuaScheduleSet(state, true, "schedule.every");
}

static int lua_ScheduleAfter(lua_State* state)
{
    return LuaScheduleSet(state, false, "schedule.after");
}

static int lua_ScheduleCancel(lua_State* state)
{
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxNameBytes)
    {
        return luaL_error(state,
            "schedule.cancel: id must contain 1 to 128 bytes");
    }
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeCancelTimer(BoundWidgetId(state),
            std::string(name, nameLength)));
    return 1;
}

struct LuaDataSubscriptionHandle
{
    WidgetEngine* engine = nullptr;
    std::uint64_t id = 0;
};

constexpr char kDataSubscriptionHandleMetatable[] =
    "SnowDesktop.DataSubscriptionHandle";

static LuaDataSubscriptionHandle* CheckDataSubscriptionHandle(
    lua_State* state, int index)
{
    return static_cast<LuaDataSubscriptionHandle*>(luaL_checkudata(
        state, index, kDataSubscriptionHandleMetatable));
}

static void PushDisplayDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetDisplayDataSnapshot& display)
{
    const auto pushRect = [state](
            const snowdesktop::widget_runtime::
                WidgetDisplayRectDataSnapshot& rect) {
        lua_createtable(state, 0, 4);
        lua_pushnumber(state, rect.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, rect.y);
        lua_setfield(state, -2, "y");
        lua_pushnumber(state, rect.width);
        lua_setfield(state, -2, "width");
        lua_pushnumber(state, rect.height);
        lua_setfield(state, -2, "height");
    };
    const auto pushPixelRect = [state](
            const snowdesktop::widget_runtime::
                WidgetDisplayPixelRectDataSnapshot& rect) {
        lua_createtable(state, 0, 4);
        lua_pushinteger(state, rect.x);
        lua_setfield(state, -2, "x");
        lua_pushinteger(state, rect.y);
        lua_setfield(state, -2, "y");
        lua_pushinteger(state, rect.width);
        lua_setfield(state, -2, "width");
        lua_pushinteger(state, rect.height);
        lua_setfield(state, -2, "height");
    };
    lua_createtable(state, 0, 15);
    lua_pushlstring(state, display.id.data(), display.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, display.name.data(), display.name.size());
    lua_setfield(state, -2, "name");
    lua_pushboolean(state, display.primary);
    lua_setfield(state, -2, "primary");
    pushRect(display.bounds);
    lua_setfield(state, -2, "bounds");
    pushRect(display.workArea);
    lua_setfield(state, -2, "workArea");
    pushPixelRect(display.pixelBounds);
    lua_setfield(state, -2, "pixelBounds");
    pushPixelRect(display.pixelWorkArea);
    lua_setfield(state, -2, "pixelWorkArea");
    lua_pushinteger(state, display.dpiX);
    lua_setfield(state, -2, "dpiX");
    lua_pushinteger(state, display.dpiY);
    lua_setfield(state, -2, "dpiY");
    lua_pushnumber(state, display.scale);
    lua_setfield(state, -2, "scale");
    lua_pushnumber(state, display.refreshHz);
    lua_setfield(state, -2, "refreshHz");
    lua_pushlstring(state, display.orientation.data(),
        display.orientation.size());
    lua_setfield(state, -2, "orientation");
    lua_pushboolean(state, display.hdrKnown);
    lua_setfield(state, -2, "hdrKnown");
    lua_pushboolean(state, display.hdrSupported);
    lua_setfield(state, -2, "hdrSupported");
    lua_pushboolean(state, display.hdrEnabled);
    lua_setfield(state, -2, "hdrEnabled");
}

static void PushMediaTimelineDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetMediaTimelineDataSnapshot& value)
{
    lua_createtable(state, 0, 6);
    lua_pushlstring(state, value.sessionId.data(), value.sessionId.size());
    lua_setfield(state, -2, "sessionId");
    lua_pushinteger(state, static_cast<lua_Integer>(value.positionMs));
    lua_setfield(state, -2, "positionMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.durationMs));
    lua_setfield(state, -2, "durationMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.minimumSeekMs));
    lua_setfield(state, -2, "minimumSeekMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.maximumSeekMs));
    lua_setfield(state, -2, "maximumSeekMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.updatedAtMs));
    lua_setfield(state, -2, "updatedAtMs");
}

static void PushMediaControlsDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetMediaControlsDataSnapshot& value)
{
    lua_createtable(state, 0, 10);
    lua_pushboolean(state, value.canPlay);
    lua_setfield(state, -2, "canPlay");
    lua_pushboolean(state, value.canPause);
    lua_setfield(state, -2, "canPause");
    lua_pushboolean(state, value.canPlayPause);
    lua_setfield(state, -2, "canPlayPause");
    lua_pushboolean(state, value.canStop);
    lua_setfield(state, -2, "canStop");
    lua_pushboolean(state, value.canNext);
    lua_setfield(state, -2, "canNext");
    lua_pushboolean(state, value.canPrevious);
    lua_setfield(state, -2, "canPrevious");
    lua_pushboolean(state, value.canSeek);
    lua_setfield(state, -2, "canSeek");
    lua_pushboolean(state, value.canChangePlaybackRate);
    lua_setfield(state, -2, "canChangePlaybackRate");
    lua_pushboolean(state, value.canToggleShuffle);
    lua_setfield(state, -2, "canToggleShuffle");
    lua_pushboolean(state, value.canChangeRepeatMode);
    lua_setfield(state, -2, "canChangeRepeatMode");
}

static void PushMediaSessionDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetMediaSessionDataSnapshot& value)
{
    lua_createtable(state, 0, 10);
    lua_pushlstring(state, value.id.data(), value.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, value.sourceName.data(), value.sourceName.size());
    lua_setfield(state, -2, "sourceName");
    lua_pushlstring(state, value.title.data(), value.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, value.artist.data(), value.artist.size());
    lua_setfield(state, -2, "artist");
    lua_pushlstring(state, value.album.data(), value.album.size());
    lua_setfield(state, -2, "album");
    lua_pushlstring(state, value.playbackStatus.data(),
        value.playbackStatus.size());
    lua_setfield(state, -2, "playbackStatus");
    lua_pushboolean(state, value.current);
    lua_setfield(state, -2, "current");
    PushMediaControlsDataValue(state, value.controls);
    lua_setfield(state, -2, "controls");
    PushMediaTimelineDataValue(state, value.timeline);
    lua_setfield(state, -2, "timeline");
}

static void PushDesktopDataItemValue(
    lua_State* state, const LuaDesktopItemInfo& item)
{
    lua_createtable(state, 0, 6);
    lua_pushlstring(state, item.id.data(), item.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, item.title.data(), item.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, item.source.data(), item.source.size());
    lua_setfield(state, -2, "source");
    lua_pushlstring(state, item.type.data(), item.type.size());
    lua_setfield(state, -2, "type");
    lua_pushboolean(state, item.selected);
    lua_setfield(state, -2, "selected");
}

static void PushCalendarDataEventValue(lua_State* state,
    const snowdesktop::calendar::CalendarEvent& event)
{
    lua_createtable(state, 0, 9);
    lua_pushlstring(state, event.id.data(), event.id.size());
    lua_setfield(state, -2, "id");
    lua_pushinteger(state, event.revision);
    lua_setfield(state, -2, "revision");
    lua_pushlstring(state, event.title.data(), event.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, event.date.data(), event.date.size());
    lua_setfield(state, -2, "date");
    lua_pushboolean(state, event.allDay);
    lua_setfield(state, -2, "allDay");
    lua_pushinteger(state, event.startMinutes);
    lua_setfield(state, -2, "startMinutes");
    lua_pushinteger(state, event.endMinutes);
    lua_setfield(state, -2, "endMinutes");
    lua_pushlstring(state, event.notes.data(), event.notes.size());
    lua_setfield(state, -2, "notes");
    lua_pushinteger(state, event.reminderMinutes);
    lua_setfield(state, -2, "reminderMinutes");
}

static void PushDataSnapshotEnvelope(lua_State* state,
    const std::optional<LuaWidgetDataSnapshot>& snapshot,
    const char* missingError = "unsubscribed")
{
    lua_createtable(state, 0, 7);
    const bool available = snapshot && snapshot->available;
    lua_pushboolean(state, available);
    lua_setfield(state, -2, "available");
    lua_pushboolean(state, !snapshot || snapshot->stale);
    lua_setfield(state, -2, "stale");
    lua_pushboolean(state, snapshot && snapshot->warmingUp);
    lua_setfield(state, -2, "warmingUp");
    lua_pushinteger(state, snapshot
        ? static_cast<lua_Integer>(snapshot->timestampMs) : 0);
    lua_setfield(state, -2, "timestamp");

    const std::string error = snapshot
        ? snapshot->error : std::string(missingError);
    if (!error.empty())
    {
        lua_pushlstring(state, error.data(), error.size());
        lua_setfield(state, -2, "error");
    }
    if (!available) return;

    lua_createtable(state, 0, 6);
    if (snapshot->topic == "system.cpu")
    {
        lua_pushnumber(state, snapshot->cpu.usagePercent);
        lua_setfield(state, -2, "usagePercent");
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->cpu.logicalProcessors));
        lua_setfield(state, -2, "logicalProcessors");
        lua_pushlstring(state, snapshot->cpu.name.data(),
            snapshot->cpu.name.size());
        lua_setfield(state, -2, "name");
    }
    else if (snapshot->topic == "system.memory")
    {
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->memory.totalBytes));
        lua_setfield(state, -2, "totalBytes");
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->memory.usedBytes));
        lua_setfield(state, -2, "usedBytes");
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->memory.freeBytes));
        lua_setfield(state, -2, "freeBytes");
        lua_pushnumber(state, snapshot->memory.usagePercent);
        lua_setfield(state, -2, "usagePercent");
    }
    else if (snapshot->topic == "system.power")
    {
        lua_pushboolean(state, snapshot->power.acPower);
        lua_setfield(state, -2, "acPower");
        lua_pushboolean(state, snapshot->power.charging);
        lua_setfield(state, -2, "charging");
        lua_pushboolean(state, snapshot->power.saver);
        lua_setfield(state, -2, "saver");
        lua_pushnumber(state, snapshot->power.batteryPercent);
        lua_setfield(state, -2, "batteryPercent");
        if (snapshot->power.estimatedRemainingSeconds >= 0)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(
                snapshot->power.estimatedRemainingSeconds));
            lua_setfield(state, -2, "estimatedRemainingSeconds");
        }
    }
    else if (snapshot->topic == "system.network.status")
    {
        lua_pushlstring(state, snapshot->networkStatus.connectivity.data(),
            snapshot->networkStatus.connectivity.size());
        lua_setfield(state, -2, "connectivity");
        lua_pushlstring(state, snapshot->networkStatus.transport.data(),
            snapshot->networkStatus.transport.size());
        lua_setfield(state, -2, "transport");
        lua_pushboolean(state, snapshot->networkStatus.costKnown);
        lua_setfield(state, -2, "costKnown");
        lua_pushboolean(state, snapshot->networkStatus.metered);
        lua_setfield(state, -2, "metered");
        lua_pushboolean(state, snapshot->networkStatus.roaming);
        lua_setfield(state, -2, "roaming");
        lua_pushboolean(state, snapshot->networkStatus.overLimit);
        lua_setfield(state, -2, "overLimit");
    }
    else if (snapshot->topic == "system.network.traffic")
    {
        lua_pushboolean(state, snapshot->networkTraffic.connected);
        lua_setfield(state, -2, "connected");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.receivedBytes));
        lua_setfield(state, -2, "receivedBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.sentBytes));
        lua_setfield(state, -2, "sentBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.downloadBytesPerSecond));
        lua_setfield(state, -2, "downloadBytesPerSecond");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.uploadBytesPerSecond));
        lua_setfield(state, -2, "uploadBytesPerSecond");
    }
    else if (snapshot->topic == "system.gpu")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->gpu.adapters.size()), 0);
        int adapterIndex = 1;
        for (const auto& adapter : snapshot->gpu.adapters)
        {
            lua_createtable(state, 0, 7);
            lua_pushlstring(state, adapter.id.data(), adapter.id.size());
            lua_setfield(state, -2, "id");
            lua_pushlstring(state, adapter.name.data(), adapter.name.size());
            lua_setfield(state, -2, "name");
            lua_pushnumber(state, adapter.usagePercent);
            lua_setfield(state, -2, "usagePercent");
            lua_pushinteger(state, static_cast<lua_Integer>(
                adapter.dedicatedMemoryBytes));
            lua_setfield(state, -2, "dedicatedMemoryBytes");
            lua_pushinteger(state, static_cast<lua_Integer>(
                adapter.dedicatedUsedBytes));
            lua_setfield(state, -2, "dedicatedUsedBytes");
            lua_pushinteger(state, static_cast<lua_Integer>(
                adapter.sharedMemoryBytes));
            lua_setfield(state, -2, "sharedMemoryBytes");
            lua_pushinteger(state, static_cast<lua_Integer>(
                adapter.sharedUsedBytes));
            lua_setfield(state, -2, "sharedUsedBytes");
            lua_rawseti(state, -2, adapterIndex++);
        }
        lua_setfield(state, -2, "adapters");
    }
    else if (snapshot->topic == "system.storage.volumes")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->storageVolumes.volumes.size()), 0);
        int volumeIndex = 1;
        for (const auto& volume : snapshot->storageVolumes.volumes)
        {
            lua_createtable(state, 0, 9);
            lua_pushlstring(state, volume.id.data(), volume.id.size());
            lua_setfield(state, -2, "id");
            lua_pushlstring(state, volume.displayName.data(),
                volume.displayName.size());
            lua_setfield(state, -2, "displayName");
            lua_pushlstring(state, volume.mountPoint.data(),
                volume.mountPoint.size());
            lua_setfield(state, -2, "mountPoint");
            lua_pushlstring(state, volume.kind.data(), volume.kind.size());
            lua_setfield(state, -2, "kind");
            lua_pushinteger(state, static_cast<lua_Integer>(
                volume.capacityBytes));
            lua_setfield(state, -2, "capacityBytes");
            lua_pushinteger(state, static_cast<lua_Integer>(
                volume.freeBytes));
            lua_setfield(state, -2, "freeBytes");
            lua_pushboolean(state, volume.capacityAvailable);
            lua_setfield(state, -2, "capacityAvailable");
            lua_pushboolean(state, volume.removable);
            lua_setfield(state, -2, "removable");
            lua_pushboolean(state, volume.readOnly);
            lua_setfield(state, -2, "readOnly");
            lua_rawseti(state, -2, volumeIndex++);
        }
        lua_setfield(state, -2, "volumes");
    }
    else if (snapshot->topic == "system.storage.io")
    {
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->storageIo.readBytesPerSecond));
        lua_setfield(state, -2, "readBytesPerSecond");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->storageIo.writeBytesPerSecond));
        lua_setfield(state, -2, "writeBytesPerSecond");
        lua_pushnumber(state, snapshot->storageIo.busyPercent);
        lua_setfield(state, -2, "busyPercent");
    }
    else if (snapshot->topic == "system.display.topology")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->displayTopology.displays.size()), 0);
        int displayIndex = 1;
        for (const auto& display : snapshot->displayTopology.displays)
        {
            PushDisplayDataValue(state, display);
            lua_rawseti(state, -2, displayIndex++);
        }
        lua_setfield(state, -2, "displays");
    }
    else if (snapshot->topic == "system.display.current")
    {
        PushDisplayDataValue(state, snapshot->displayCurrent);
        lua_setfield(state, -2, "display");
    }
    else if (snapshot->topic == "audio.output.default")
    {
        lua_pushlstring(state, snapshot->audioOutputDefault.id.data(),
            snapshot->audioOutputDefault.id.size());
        lua_setfield(state, -2, "id");
        lua_pushlstring(state, snapshot->audioOutputDefault.name.data(),
            snapshot->audioOutputDefault.name.size());
        lua_setfield(state, -2, "name");
        lua_pushlstring(state, snapshot->audioOutputDefault.state.data(),
            snapshot->audioOutputDefault.state.size());
        lua_setfield(state, -2, "state");
    }
    else if (snapshot->topic == "audio.output.volume")
    {
        lua_pushlstring(state, snapshot->audioOutputVolume.endpointId.data(),
            snapshot->audioOutputVolume.endpointId.size());
        lua_setfield(state, -2, "endpointId");
        lua_pushnumber(state, snapshot->audioOutputVolume.volume);
        lua_setfield(state, -2, "volume");
        lua_pushboolean(state, snapshot->audioOutputVolume.muted);
        lua_setfield(state, -2, "muted");
        lua_pushnumber(state, 0.0);
        lua_setfield(state, -2, "minimum");
        lua_pushnumber(state, 1.0);
        lua_setfield(state, -2, "maximum");
    }
    else if (snapshot->topic == "audio.output.analysis")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->audioAnalysis.waveform.size()), 0);
        int waveformIndex = 1;
        for (const double value : snapshot->audioAnalysis.waveform)
        {
            lua_pushnumber(state, value);
            lua_rawseti(state, -2, waveformIndex++);
        }
        lua_setfield(state, -2, "waveform");
        lua_createtable(state,
            static_cast<int>(snapshot->audioAnalysis.spectrum.size()), 0);
        int spectrumIndex = 1;
        for (const double value : snapshot->audioAnalysis.spectrum)
        {
            lua_pushnumber(state, value);
            lua_rawseti(state, -2, spectrumIndex++);
        }
        lua_setfield(state, -2, "spectrum");
        lua_pushnumber(state, snapshot->audioAnalysis.rms);
        lua_setfield(state, -2, "rms");
        lua_pushnumber(state, snapshot->audioAnalysis.peak);
        lua_setfield(state, -2, "peak");
        lua_pushboolean(state, snapshot->audioAnalysis.silent);
        lua_setfield(state, -2, "silent");
        lua_pushboolean(state, snapshot->audioAnalysis.deviceChanged);
        lua_setfield(state, -2, "deviceChanged");
        lua_pushlstring(state, snapshot->audioAnalysis.endpointId.data(),
            snapshot->audioAnalysis.endpointId.size());
        lua_setfield(state, -2, "endpointId");
        lua_pushinteger(state, snapshot->audioAnalysis.sampleRate);
        lua_setfield(state, -2, "sampleRate");
        lua_pushinteger(state, snapshot->audioAnalysis.channels);
        lua_setfield(state, -2, "channels");
    }
    else if (snapshot->topic == "media.sessions")
    {
        lua_pushlstring(state,
            snapshot->mediaSessions.currentSessionId.data(),
            snapshot->mediaSessions.currentSessionId.size());
        lua_setfield(state, -2, "currentSessionId");
        lua_createtable(state,
            static_cast<int>(snapshot->mediaSessions.sessions.size()), 0);
        int sessionIndex = 1;
        for (const auto& session : snapshot->mediaSessions.sessions)
        {
            PushMediaSessionDataValue(state, session);
            lua_rawseti(state, -2, sessionIndex++);
        }
        lua_setfield(state, -2, "sessions");
    }
    else if (snapshot->topic == "media.current")
    {
        PushMediaSessionDataValue(state, snapshot->mediaCurrent.session);
        lua_setfield(state, -2, "session");
    }
    else if (snapshot->topic == "media.timeline")
    {
        PushMediaTimelineDataValue(state, snapshot->mediaTimeline);
        lua_setfield(state, -2, "timeline");
    }
    else if (snapshot->topic == "media.artwork")
    {
        const std::string token = RegisterRuntimeImageSource(GetD2D(state),
            BoundWidgetId(state), snapshot->mediaArtwork.resourceToken,
            snapshot->mediaArtwork.pixels, "media");
        lua_pushlstring(state, snapshot->mediaArtwork.sessionId.data(),
            snapshot->mediaArtwork.sessionId.size());
        lua_setfield(state, -2, "sessionId");
        PushResourceHandle(state, LuaResourceType::Image,
            token);
        lua_setfield(state, -2, "image");
        if (snapshot->mediaArtwork.pixels)
        {
            lua_pushinteger(state, snapshot->mediaArtwork.pixels->width);
            lua_setfield(state, -2, "width");
            lua_pushinteger(state, snapshot->mediaArtwork.pixels->height);
            lua_setfield(state, -2, "height");
        }
    }
    else if (snapshot->topic == "filesystem.watch")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->filesystemWatchEvents.size()), 0);
        int eventIndex = 1;
        for (const auto& event : snapshot->filesystemWatchEvents)
        {
            lua_createtable(state, 0, 5);
            lua_pushlstring(state, event.kind.data(), event.kind.size());
            lua_setfield(state, -2, "kind");
            lua_pushlstring(state, event.name.data(), event.name.size());
            lua_setfield(state, -2, "name");
            if (!event.oldName.empty())
            {
                lua_pushlstring(state,
                    event.oldName.data(), event.oldName.size());
                lua_setfield(state, -2, "oldName");
            }
            if (!event.handle.empty())
            {
                lua_pushlstring(state,
                    event.handle.data(), event.handle.size());
                lua_setfield(state, -2, "handle");
            }
            if (!event.itemKind.empty())
            {
                lua_pushlstring(state,
                    event.itemKind.data(), event.itemKind.size());
                lua_setfield(state, -2, "itemKind");
            }
            lua_rawseti(state, -2, eventIndex++);
        }
        lua_setfield(state, -2, "events");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->filesystemWatchRevision));
        lua_setfield(state, -2, "revision");
        lua_pushboolean(state, snapshot->filesystemWatchOverflow);
        lua_setfield(state, -2, "overflow");
    }
    else if (snapshot->topic == "desktop.items" ||
        snapshot->topic == "desktop.selection")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->desktopItems.size()), 0);
        int itemIndex = 1;
        for (const auto& item : snapshot->desktopItems)
        {
            PushDesktopDataItemValue(state, item);
            lua_rawseti(state, -2, itemIndex++);
        }
        lua_setfield(state, -2, "items");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->desktopRevision));
        lua_setfield(state, -2, "revision");
    }
    else if (snapshot->topic == "desktop.changes")
    {
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->desktopRevision));
        lua_setfield(state, -2, "revision");
        lua_pushlstring(state, snapshot->desktopChangeReason.data(),
            snapshot->desktopChangeReason.size());
        lua_setfield(state, -2, "reason");
    }
    else if (snapshot->topic == "calendar.events")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->calendarEvents.size()), 0);
        int eventIndex = 1;
        for (const auto& event : snapshot->calendarEvents)
        {
            PushCalendarDataEventValue(state, event);
            lua_rawseti(state, -2, eventIndex++);
        }
        lua_setfield(state, -2, "events");
        lua_pushlstring(state, snapshot->calendarRangeStart.data(),
            snapshot->calendarRangeStart.size());
        lua_setfield(state, -2, "fromDate");
        lua_pushlstring(state, snapshot->calendarRangeEnd.data(),
            snapshot->calendarRangeEnd.size());
        lua_setfield(state, -2, "toDate");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->calendarRevision));
        lua_setfield(state, -2, "revision");
        lua_pushboolean(state, snapshot->calendarTruncated);
        lua_setfield(state, -2, "truncated");
    }
    else if (snapshot->topic == "calendar.selectedDate")
    {
        lua_pushlstring(state, snapshot->calendarSelectedDate.data(),
            snapshot->calendarSelectedDate.size());
        lua_setfield(state, -2, "date");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->calendarRevision));
        lua_setfield(state, -2, "revision");
    }
    else if (snapshot->topic == "app.indexStatus")
    {
        lua_pushlstring(state, snapshot->appIndexState.data(),
            snapshot->appIndexState.size());
        lua_setfield(state, -2, "state");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->appIndexRevision));
        lua_setfield(state, -2, "revision");
    }
    lua_setfield(state, -2, "value");
}

static int lua_DataSubscriptionValue(lua_State* state)
{
    auto* handle = CheckDataSubscriptionHandle(state, 1);
    if (!handle->engine || handle->id == 0)
    {
        PushDataSnapshotEnvelope(state, std::nullopt);
        return 1;
    }
    PushDataSnapshotEnvelope(
        state, handle->engine->RuntimeGetDataSnapshot(handle->id));
    return 1;
}

static int lua_DataSubscriptionUnsubscribe(lua_State* state)
{
    auto* handle = CheckDataSubscriptionHandle(state, 1);
    bool removed = false;
    if (handle->engine && handle->id != 0)
        removed = handle->engine->RuntimeUnsubscribeData(handle->id);
    handle->id = 0;
    lua_pushboolean(state, removed);
    return 1;
}

static int lua_DataSubscriptionGc(lua_State* state)
{
    auto* handle = static_cast<LuaDataSubscriptionHandle*>(
        luaL_testudata(state, 1, kDataSubscriptionHandleMetatable));
    if (handle && handle->engine && handle->id != 0)
        (void)handle->engine->RuntimeUnsubscribeData(handle->id);
    if (handle) handle->id = 0;
    return 0;
}

static void RegisterDataSubscriptionHandle(lua_State* state)
{
    if (luaL_newmetatable(state, kDataSubscriptionHandleMetatable))
    {
        lua_newtable(state);
        lua_pushcfunction(state, lua_DataSubscriptionValue);
        lua_setfield(state, -2, "value");
        lua_pushcfunction(state, lua_DataSubscriptionUnsubscribe);
        lua_setfield(state, -2, "unsubscribe");
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, lua_DataSubscriptionGc);
        lua_setfield(state, -2, "__gc");
        lua_pushliteral(state, "data subscription handle");
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);
}

static int lua_DataSubscribe(lua_State* state)
{
    size_t topicLength = 0;
    const char* topic = luaL_checklstring(state, 1, &topicLength);
    if (topicLength == 0 || topicLength > 128)
        return luaL_error(state,
            "data.subscribe: topic must contain 1 to 128 bytes");
    if (lua_gettop(state) > 2)
        return luaL_error(state,
            "data.subscribe: expected topic and optional options");

    const std::string topicValue(topic, topicLength);
    lua_Integer maxAgeMs = 1000;
    std::string rangeStart;
    std::string rangeEnd;
    std::string scopeHandle;
    auto hiddenPolicy = topicValue == "filesystem.watch"
        ? snowdesktop::widget_runtime::DataHiddenPolicy::Pause
        : snowdesktop::widget_runtime::DataHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 2))
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        lua_getfield(state, 2, "maxAgeMs");
        if (!lua_isnil(state, -1))
        {
            int isInteger = 0;
            maxAgeMs = lua_tointegerx(state, -1, &isInteger);
            if (!isInteger || maxAgeMs <= 0 || maxAgeMs > 86400000)
                return luaL_error(state,
                    "data.subscribe: maxAgeMs must be an integer from 1 to 86400000");
        }
        lua_pop(state, 1);

        lua_getfield(state, 2, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t policyLength = 0;
            const char* policy = luaL_checklstring(
                state, -1, &policyLength);
            const std::string_view value(policy, policyLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "data.subscribe: whenHidden must be 'pause', 'throttle', or 'continue'");
        }
        lua_pop(state, 1);

        const auto readDateOption = [&](const char* name,
                                        std::string& output) {
            lua_getfield(state, 2, name);
            if (!lua_isnil(state, -1))
            {
                size_t length = 0;
                const char* value = luaL_checklstring(
                    state, -1, &length);
                output.assign(value, length);
            }
            lua_pop(state, 1);
        };
        readDateOption("fromDate", rangeStart);
        readDateOption("toDate", rangeEnd);

        lua_getfield(state, 2, "handle");
        if (!lua_isnil(state, -1))
        {
            size_t handleLength = 0;
            const char* handle = luaL_checklstring(
                state, -1, &handleLength);
            if (handleLength == 0 || handleLength > 128)
                return luaL_error(state,
                    "data.subscribe: handle must contain 1 to 128 bytes");
            scopeHandle.assign(handle, handleLength);
        }
        lua_pop(state, 1);
    }

    if (topicValue == "filesystem.watch")
    {
        if (scopeHandle.empty())
            return luaL_error(state,
                "data.subscribe: filesystem.watch requires a folder handle");
    }
    else if (!scopeHandle.empty())
    {
        return luaL_error(state,
            "data.subscribe: handle is only valid for filesystem.watch");
    }

    if (!rangeStart.empty() || !rangeEnd.empty())
    {
        if (topicValue != "calendar.events" || rangeStart.empty() ||
            rangeEnd.empty() || rangeEnd < rangeStart ||
            !snowdesktop::calendar::CalendarService::
                GetDateInfo(rangeStart) ||
            !snowdesktop::calendar::CalendarService::
                GetDateInfo(rangeEnd))
        {
            return luaL_error(state,
                "data.subscribe: fromDate/toDate require a valid calendar.events range");
        }
        const auto maximumEnd = snowdesktop::calendar::CalendarService::
            AddDays(rangeStart, 366);
        if (!maximumEnd || rangeEnd > *maximumEnd)
        {
            return luaL_error(state,
                "data.subscribe: calendar.events range cannot exceed 366 days");
        }
    }

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "data.subscribe: host is unavailable");
    auto result = d2d->engine->RuntimeSubscribeData(
        BoundWidgetId(state), topicValue,
        std::chrono::milliseconds(maxAgeMs), hiddenPolicy,
        std::move(rangeStart), std::move(rangeEnd),
        std::move(scopeHandle));
    if (!result)
        return luaL_error(state, "data.subscribe: %s", result.error.c_str());

    auto* handle = static_cast<LuaDataSubscriptionHandle*>(
        lua_newuserdata(state, sizeof(LuaDataSubscriptionHandle)));
    *handle = { d2d->engine, result.id };
    luaL_getmetatable(state, kDataSubscriptionHandleMetatable);
    lua_setmetatable(state, -2);
    return 1;
}

static int lua_TaskStart(lua_State* state)
{
    if (lua_gettop(state) < 1 || lua_gettop(state) > 2)
        return luaL_error(state,
            "task.start: expected a task name and optional arguments");
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength > 128)
        return luaL_error(state,
            "task.start: task name must contain 1 to 128 bytes");
    const std::string taskName(name, nameLength);
    std::unordered_map<std::string, std::string> arguments;
    const bool hasArguments = !lua_isnoneornil(state, 2);
    if (hasArguments)
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        if (lua_getmetatable(state, 2) != 0)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: arguments must be a plain table");
        }
    }

    if (snowdesktop::widget_runtime::WidgetMediaTaskExecutor::
            SupportsAction(taskName))
    {
        const bool requiresValue = taskName == "media.seek" ||
            taskName == "media.setRate" ||
            taskName == "media.setShuffle" ||
            taskName == "media.setRepeat";
        if (requiresValue && !hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: media argument keys must be strings");
                }
                size_t keyLength = 0;
                const char* keyValue = lua_tolstring(
                    state, -2, &keyLength);
                const std::string_view key(
                    keyValue ? keyValue : "", keyLength);
                const bool allowed = key == "sessionId" ||
                    (taskName == "media.seek" && key == "positionMs") ||
                    (taskName == "media.setRate" && key == "rate") ||
                    (taskName == "media.setShuffle" && key == "shuffle") ||
                    (taskName == "media.setRepeat" && key == "mode");
                if (!allowed)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: %s received an unknown argument",
                        taskName.c_str());
                }
                lua_pop(state, 1);
            }

            lua_getfield(state, 2, "sessionId");
            if (!lua_isnil(state, -1))
            {
                if (lua_type(state, -1) != LUA_TSTRING)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: media sessionId must be a string");
                }
                size_t length = 0;
                const char* value = lua_tolstring(state, -1, &length);
                std::string sessionId(value ? value : "", length);
                lua_pop(state, 1);
                if (sessionId.empty() || sessionId.size() > 128 ||
                    sessionId.find('\0') != std::string::npos ||
                    !IsValidUtf8Local(sessionId))
                {
                    return luaL_error(state,
                        "task.start: media sessionId must contain 1 to 128 bytes of valid UTF-8");
                }
                arguments.emplace("sessionId", std::move(sessionId));
            }
            else
            {
                lua_pop(state, 1);
            }
        }

        if (taskName == "media.seek")
        {
            lua_getfield(state, 2, "positionMs");
            if (!lua_isinteger(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.seek positionMs must be an integer");
            }
            const lua_Integer position = lua_tointeger(state, -1);
            lua_pop(state, 1);
            if (position < 0 || static_cast<std::uint64_t>(position) >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max() / 10000))
            {
                return luaL_error(state,
                    "task.start: media.seek positionMs is out of range");
            }
            arguments.emplace("positionMs", std::to_string(position));
        }
        else if (taskName == "media.setRate")
        {
            lua_getfield(state, 2, "rate");
            if (lua_type(state, -1) != LUA_TNUMBER)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.setRate rate must be a number");
            }
            const double rate = lua_tonumber(state, -1);
            lua_pop(state, 1);
            if (!std::isfinite(rate) || rate <= 0.0)
                return luaL_error(state,
                    "task.start: media.setRate rate must be finite and positive");
            std::array<char, 64> buffer{};
            const auto converted = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), rate,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            if (converted.ec != std::errc{})
                return luaL_error(state,
                    "task.start: media.setRate rate cannot be represented");
            arguments.emplace("rate", std::string(
                buffer.data(), converted.ptr));
        }
        else if (taskName == "media.setShuffle")
        {
            lua_getfield(state, 2, "shuffle");
            if (!lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.setShuffle shuffle must be a boolean");
            }
            const bool shuffle = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            arguments.emplace("shuffle", shuffle ? "1" : "0");
        }
        else if (taskName == "media.setRepeat")
        {
            lua_getfield(state, 2, "mode");
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: media.setRepeat mode must be a string");
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            std::string mode(value ? value : "", length);
            lua_pop(state, 1);
            if (mode != "none" && mode != "track" && mode != "list")
                return luaL_error(state,
                    "task.start: media.setRepeat mode must be none, track, or list");
            arguments.emplace("mode", std::move(mode));
        }
    }
    else if (snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor::
            SupportsAction(taskName))
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        const std::string_view expectedKey =
            taskName == "audio.output.setVolume" ? "volume" : "muted";
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: audio output argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(
                state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            if (key != expectedKey)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s accepts only %s",
                    taskName.c_str(), expectedKey.data());
            }
            lua_pop(state, 1);
        }

        if (taskName == "audio.output.setVolume")
        {
            lua_getfield(state, 2, "volume");
            if (lua_type(state, -1) != LUA_TNUMBER)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: audio.output.setVolume volume must be a number");
            }
            const double volume = lua_tonumber(state, -1);
            lua_pop(state, 1);
            if (!std::isfinite(volume))
                return luaL_error(state,
                    "task.start: audio.output.setVolume volume must be finite");
            std::array<char, 64> buffer{};
            const auto converted = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), volume,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            if (converted.ec != std::errc{})
                return luaL_error(state,
                    "task.start: audio.output.setVolume volume cannot be represented");
            arguments.emplace("volume", std::string(
                buffer.data(), converted.ptr));
        }
        else
        {
            lua_getfield(state, 2, "muted");
            if (!lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: audio.output.setMute muted must be a boolean");
            }
            const bool muted = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            arguments.emplace("muted", muted ? "1" : "0");
        }
    }
    else if (taskName == "system.openSettings")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: system.openSettings requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING ||
                std::string_view(lua_tostring(state, -2)) != "page")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: system.openSettings accepts only page");
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "page");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: system.openSettings page must be a string");
        }
        size_t length = 0;
        const char* value = lua_tolstring(state, -1, &length);
        const std::string page(value ? value : "", length);
        lua_pop(state, 1);
        if (!snowdesktop::widget_runtime::SystemSettingsUri(page))
            return luaL_error(state,
                "task.start: system.openSettings page is not supported");
        arguments.emplace("page", page);
    }
    else if (snowdesktop::widget_runtime::WidgetClipboardTaskExecutor::
            SupportsAction(taskName))
    {
        const bool clear = taskName == "clipboard.clear";
        const bool write = taskName == "clipboard.write";
        if (!clear && !hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: clipboard argument keys must be strings");
                }
                size_t keyLength = 0;
                const char* keyValue = lua_tolstring(
                    state, -2, &keyLength);
                const std::string_view key(
                    keyValue ? keyValue : "", keyLength);
                const bool allowed = !clear &&
                    (key == "format" || (write && key == "text"));
                if (!allowed)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: %s received an unknown argument",
                        taskName.c_str());
                }
                lua_pop(state, 1);
            }
        }
        if (!clear)
        {
            lua_getfield(state, 2, "format");
            size_t formatLength = 0;
            const char* formatValue = lua_type(state, -1) == LUA_TSTRING
                ? lua_tolstring(state, -1, &formatLength) : nullptr;
            const std::string format(
                formatValue ? formatValue : "", formatLength);
            const bool supported = write
                ? format == "text"
                : (format == "text" || format == "image" ||
                    format == "file-reference");
            if (!formatValue || !supported)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    write
                        ? "task.start: clipboard.write supports only format text"
                        : "task.start: clipboard.read format must be text, image, or file-reference");
            }
            lua_pop(state, 1);
            arguments.emplace("format", format);
        }
        if (write)
        {
            lua_getfield(state, 2, "text");
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: clipboard.write text must be a string");
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            std::string text(value ? value : "", length);
            lua_pop(state, 1);
            if (text.size() > snowdesktop::widget_runtime::
                    WidgetClipboardTaskExecutor::MaximumTextBytes ||
                text.find('\0') != std::string::npos ||
                (!text.empty() && !IsValidUtf8Local(text)))
            {
                return luaL_error(state,
                    "task.start: clipboard.write text must be at most 262144 bytes of valid UTF-8 without NUL");
            }
            arguments.emplace("text", std::move(text));
        }
    }
    else if (IsFilesystemPickerTask(taskName))
    {
        if (hasArguments)
        {
            lua_pushnil(state);
            while (lua_next(state, 2) != 0)
            {
                if (lua_type(state, -2) != LUA_TSTRING)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: filesystem picker argument keys must be strings");
                }
                size_t keyLength = 0;
                const char* keyValue = lua_tolstring(
                    state, -2, &keyLength);
                const std::string_view key(
                    keyValue ? keyValue : "", keyLength);
                const bool allowed =
                    (taskName == "filesystem.pickOpen" &&
                        key == "extensions") ||
                    (taskName == "filesystem.pickSave" &&
                        (key == "extensions" || key == "suggestedName")) ||
                    (taskName == "filesystem.pickFolder" &&
                        key == "access");
                if (!allowed)
                {
                    lua_pop(state, 2);
                    return luaL_error(state,
                        "task.start: %s received an unknown argument",
                        taskName.c_str());
                }
                lua_pop(state, 1);
            }
        }

        if (taskName != "filesystem.pickFolder")
        {
            std::vector<std::string> extensions;
            if (hasArguments)
            {
                lua_getfield(state, 2, "extensions");
                if (!lua_isnil(state, -1))
                {
                    if (lua_type(state, -1) != LUA_TTABLE)
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem extensions must be an array");
                    }
                    const int extensionTable = lua_absindex(state, -1);
                    const std::size_t count = lua_rawlen(
                        state, extensionTable);
                    if (count > 16)
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem extensions accepts at most 16 entries");
                    }
                    lua_pushnil(state);
                    while (lua_next(state, extensionTable) != 0)
                    {
                        if (!lua_isinteger(state, -2))
                        {
                            lua_pop(state, 3);
                            return luaL_error(state,
                                "task.start: filesystem extensions must be an array");
                        }
                        const lua_Integer index = lua_tointeger(state, -2);
                        if (index < 1 ||
                            static_cast<std::size_t>(index) > count)
                        {
                            lua_pop(state, 3);
                            return luaL_error(state,
                                "task.start: filesystem extensions must be contiguous");
                        }
                        lua_pop(state, 1);
                    }
                    extensions.reserve(count);
                    for (std::size_t index = 1; index <= count; ++index)
                    {
                        lua_rawgeti(state, extensionTable,
                            static_cast<lua_Integer>(index));
                        size_t length = 0;
                        const char* value = lua_type(state, -1) == LUA_TSTRING
                            ? lua_tolstring(state, -1, &length) : nullptr;
                        std::string extension(
                            value ? value : "", length);
                        lua_pop(state, 1);
                        if (!extension.empty() && extension.front() == '.')
                            extension.erase(extension.begin());
                        const bool valid = !extension.empty() &&
                            extension.size() <= 32 &&
                            extension.front() != '.' &&
                            extension.back() != '.' &&
                            std::all_of(extension.begin(), extension.end(),
                                [](const unsigned char character) {
                                    return std::isalnum(character) ||
                                        character == '.';
                                }) &&
                            extension.find("..") == std::string::npos;
                        if (!valid)
                        {
                            lua_pop(state, 1);
                            return luaL_error(state,
                                "task.start: filesystem extensions must contain safe extension names");
                        }
                        std::transform(extension.begin(), extension.end(),
                            extension.begin(), [](const unsigned char character) {
                                return static_cast<char>(std::tolower(character));
                            });
                        if (std::find(extensions.begin(), extensions.end(),
                                extension) == extensions.end())
                            extensions.push_back(std::move(extension));
                    }
                }
                lua_pop(state, 1);
            }
            if (!extensions.empty())
            {
                std::string encoded;
                for (const auto& extension : extensions)
                {
                    if (!encoded.empty()) encoded.push_back(';');
                    encoded += extension;
                }
                arguments.emplace("extensions", std::move(encoded));
            }

            if (taskName == "filesystem.pickSave" && hasArguments)
            {
                lua_getfield(state, 2, "suggestedName");
                if (!lua_isnil(state, -1))
                {
                    size_t length = 0;
                    const char* value = lua_type(state, -1) == LUA_TSTRING
                        ? lua_tolstring(state, -1, &length) : nullptr;
                    std::string suggestedName(
                        value ? value : "", length);
                    const bool invalidCharacter = std::any_of(
                        suggestedName.begin(), suggestedName.end(),
                        [](const unsigned char character) {
                            return character < 0x20 ||
                                std::string_view("<>:\"/\\|?*")
                                    .find(static_cast<char>(character)) !=
                                    std::string_view::npos;
                        });
                    if (!value || suggestedName.empty() ||
                        suggestedName.size() > 255 ||
                        !IsValidUtf8Local(suggestedName) ||
                        invalidCharacter || suggestedName.back() == '.' ||
                        suggestedName.back() == ' ')
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem suggestedName is invalid");
                    }
                    arguments.emplace(
                        "suggestedName", std::move(suggestedName));
                }
                lua_pop(state, 1);
            }
        }
        else
        {
            std::string access = "read";
            if (hasArguments)
            {
                lua_getfield(state, 2, "access");
                if (!lua_isnil(state, -1))
                {
                    size_t length = 0;
                    const char* value = lua_type(state, -1) == LUA_TSTRING
                        ? lua_tolstring(state, -1, &length) : nullptr;
                    access.assign(value ? value : "", length);
                    if (!value || (access != "read" &&
                            access != "write" &&
                            access != "readWrite"))
                    {
                        lua_pop(state, 1);
                        return luaL_error(state,
                            "task.start: filesystem folder access must be read, write, or readWrite");
                    }
                }
                lua_pop(state, 1);
            }
            arguments.emplace("access", std::move(access));
        }
    }
    else if (IsFilesystemHandleTask(taskName))
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: filesystem argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(
                state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            const bool allowed = key == "handle" ||
                (taskName == "filesystem.list" &&
                    (key == "offset" || key == "limit")) ||
                (taskName == "filesystem.read" &&
                    (key == "encoding" || key == "maxBytes")) ||
                (taskName == "filesystem.write" &&
                    (key == "encoding" || key == "text" ||
                        key == "expectedRevision"));
            if (!allowed)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    taskName.c_str());
            }
            lua_pop(state, 1);
        }

        lua_getfield(state, 2, "handle");
        size_t handleLength = 0;
        const char* handleValue = lua_type(state, -1) == LUA_TSTRING
            ? lua_tolstring(state, -1, &handleLength) : nullptr;
        std::string handle(handleValue ? handleValue : "", handleLength);
        lua_pop(state, 1);
        if (!handleValue || !snowdesktop::widget_runtime::
                WidgetFilesystemHandleStore::IsOpaqueHandle(handle))
            return luaL_error(state,
                "task.start: filesystem handle is invalid");
        arguments.emplace("handle", std::move(handle));

        const auto readInteger = [&](const char* field,
            lua_Integer fallback, lua_Integer minimum,
            lua_Integer maximum, lua_Integer& output) {
            lua_getfield(state, 2, field);
            if (lua_isnil(state, -1)) output = fallback;
            else if (lua_isinteger(state, -1))
                output = lua_tointeger(state, -1);
            else
            {
                lua_pop(state, 1);
                return false;
            }
            lua_pop(state, 1);
            return output >= minimum && output <= maximum;
        };
        if (taskName == "filesystem.list")
        {
            lua_Integer offset = 0;
            lua_Integer limit = 50;
            if (!readInteger("offset", 0, 0,
                    snowdesktop::widget_runtime::
                        WidgetFilesystemTaskExecutor::MaximumListOffset,
                    offset) ||
                !readInteger("limit", 50, 1,
                    snowdesktop::widget_runtime::
                        WidgetFilesystemTaskExecutor::MaximumListLimit,
                    limit))
                return luaL_error(state,
                    "task.start: filesystem.list pagination is out of range");
            arguments.emplace("offset", std::to_string(offset));
            arguments.emplace("limit", std::to_string(limit));
        }
        if (taskName == "filesystem.read" ||
            taskName == "filesystem.write")
        {
            lua_getfield(state, 2, "encoding");
            if (!lua_isnil(state, -1))
            {
                size_t length = 0;
                const char* value = lua_type(state, -1) == LUA_TSTRING
                    ? lua_tolstring(state, -1, &length) : nullptr;
                if (!value ||
                    std::string_view(value, length) != "utf8")
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: filesystem encoding must be utf8");
                }
            }
            lua_pop(state, 1);
            arguments.emplace("encoding", "utf8");
        }
        if (taskName == "filesystem.read")
        {
            lua_Integer maxBytes = 512 * 1024;
            if (!readInteger("maxBytes", 512 * 1024, 1,
                    snowdesktop::widget_runtime::
                        WidgetFilesystemTaskExecutor::MaximumTextBytes,
                    maxBytes))
                return luaL_error(state,
                    "task.start: filesystem.read maxBytes is out of range");
            arguments.emplace("maxBytes", std::to_string(maxBytes));
        }
        else if (taskName == "filesystem.write")
        {
            lua_getfield(state, 2, "text");
            size_t length = 0;
            const char* value = lua_type(state, -1) == LUA_TSTRING
                ? lua_tolstring(state, -1, &length) : nullptr;
            std::string text(value ? value : "", length);
            lua_pop(state, 1);
            if (!value || text.size() > snowdesktop::widget_runtime::
                    WidgetFilesystemTaskExecutor::MaximumTextBytes ||
                text.find('\0') != std::string::npos ||
                !IsValidUtf8Local(text))
                return luaL_error(state,
                    "task.start: filesystem.write text must be at most 1048576 bytes of valid UTF-8 without NUL");
            arguments.emplace("text", std::move(text));

            lua_getfield(state, 2, "expectedRevision");
            if (!lua_isnil(state, -1))
            {
                size_t revisionLength = 0;
                const char* revision = lua_type(state, -1) == LUA_TSTRING
                    ? lua_tolstring(state, -1, &revisionLength) : nullptr;
                if (!revision || revisionLength == 0 ||
                    revisionLength > 64 ||
                    std::string_view(revision, revisionLength)
                        .find('\0') != std::string_view::npos)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "task.start: filesystem.write expectedRevision is invalid");
                }
                arguments.emplace("expectedRevision",
                    std::string(revision, revisionLength));
            }
            lua_pop(state, 1);
        }
    }
    else if (taskName == "app.search" || taskName == "desktop.search" ||
        taskName == "everything.search")
    {
        const std::string& searchName = taskName;
        const lua_Integer maximumOffset =
            taskName == "app.search" ? 10000 : 100;
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                searchName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s argument keys must be strings",
                    searchName.c_str());
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(keyValue ? keyValue : "", keyLength);
            if (key != "query" && key != "limit" && key != "offset")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    searchName.c_str());
            }
            lua_pop(state, 1);
        }

        lua_getfield(state, 2, "query");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: %s query must be a string",
                searchName.c_str());
        }
        size_t queryLength = 0;
        const char* queryValue = lua_tolstring(state, -1, &queryLength);
        std::string query(queryValue ? queryValue : "", queryLength);
        lua_pop(state, 1);
        if (query.empty() || query.size() > 256 ||
            !IsValidUtf8Local(query))
        {
            return luaL_error(state,
                "task.start: %s query must contain 1 to 256 bytes of valid UTF-8",
                searchName.c_str());
        }
        arguments.emplace("query", std::move(query));

        lua_Integer limit = 50;
        lua_getfield(state, 2, "limit");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isinteger(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: %s limit must be an integer",
                    searchName.c_str());
            }
            limit = lua_tointeger(state, -1);
        }
        lua_pop(state, 1);
        if (limit < 1 || limit > 100)
            return luaL_error(state,
                "task.start: %s limit must be between 1 and 100",
                searchName.c_str());
        arguments.emplace("limit", std::to_string(limit));

        lua_Integer offset = 0;
        lua_getfield(state, 2, "offset");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isinteger(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: %s offset must be an integer",
                    searchName.c_str());
            }
            offset = lua_tointeger(state, -1);
        }
        lua_pop(state, 1);
        if (offset < 0 || offset > maximumOffset)
            return luaL_error(state,
                "task.start: %s offset must be between 0 and %lld",
                searchName.c_str(),
                static_cast<long long>(maximumOffset));
        arguments.emplace("offset", std::to_string(offset));
    }
    else if (taskName == "app.launch" ||
        taskName == "shell.openItem" ||
        taskName == "shell.revealItem")
    {
        const std::string& actionName = taskName;
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                actionName.c_str());
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s argument keys must be strings",
                    actionName.c_str());
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            if (std::string_view(keyValue ? keyValue : "", keyLength) != "ref")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    actionName.c_str());
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "ref");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: %s ref must be a string",
                actionName.c_str());
        }
        size_t refLength = 0;
        const char* refValue = lua_tolstring(state, -1, &refLength);
        std::string reference(refValue ? refValue : "", refLength);
        lua_pop(state, 1);
        if (reference.empty() || reference.size() > 128)
            return luaL_error(state,
                "task.start: %s ref must contain 1 to 128 bytes",
                actionName.c_str());
        arguments.emplace("ref", std::move(reference));
    }
    else if (taskName == "notification.show")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: notification.show requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: notification.show argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(keyValue ? keyValue : "", keyLength);
            if (key != "title" && key != "message")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: notification.show received an unknown argument");
            }
            lua_pop(state, 1);
        }

        const auto readText = [&](const char* field,
            std::size_t maximum, std::string& output) -> bool {
            lua_getfield(state, 2, field);
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return false;
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            output.assign(value ? value : "", length);
            lua_pop(state, 1);
            return !output.empty() && output.size() <= maximum &&
                output.find('\0') == std::string::npos &&
                IsValidUtf8Local(output);
        };
        std::string title;
        std::string message;
        if (!readText("title", 256, title))
            return luaL_error(state,
                "task.start: notification.show title must contain 1 to 256 bytes of valid UTF-8");
        if (!readText("message", 2048, message))
            return luaL_error(state,
                "task.start: notification.show message must contain 1 to 2048 bytes of valid UTF-8");
        arguments.emplace("title", std::move(title));
        arguments.emplace("message", std::move(message));
    }
    else if (taskName == "network.request")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: network.request requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: network.request argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            if (key != "url" && key != "timeoutMs" &&
                key != "cacheSeconds" && key != "maxBytes")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: network.request received an unknown argument");
            }
            lua_pop(state, 1);
        }

        lua_getfield(state, 2, "url");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: network.request url must be a string");
        }
        size_t urlLength = 0;
        const char* urlValue = lua_tolstring(state, -1, &urlLength);
        std::string url(urlValue ? urlValue : "", urlLength);
        lua_pop(state, 1);
        if (url.empty() || url.size() > 2048 ||
            url.find('\0') != std::string::npos ||
            !IsValidUtf8Local(url))
        {
            return luaL_error(state,
                "task.start: network.request url must contain 1 to 2048 bytes of valid UTF-8");
        }
        arguments.emplace("url", std::move(url));

        const auto readBoundedInteger = [&](const char* field,
            lua_Integer fallback, lua_Integer minimum,
            lua_Integer maximum, lua_Integer& output) -> bool {
            lua_getfield(state, 2, field);
            if (lua_isnil(state, -1))
                output = fallback;
            else if (lua_isinteger(state, -1))
                output = lua_tointeger(state, -1);
            else
            {
                lua_pop(state, 1);
                return false;
            }
            lua_pop(state, 1);
            return output >= minimum && output <= maximum;
        };
        lua_Integer timeoutMs = 15000;
        lua_Integer cacheSeconds = 0;
        lua_Integer maxBytes = 512 * 1024;
        if (!readBoundedInteger(
                "timeoutMs", 15000, 1000, 30000, timeoutMs) ||
            !readBoundedInteger(
                "cacheSeconds", 0, 0, 86400, cacheSeconds) ||
            !readBoundedInteger(
                "maxBytes", 512 * 1024, 4096, 1024 * 1024,
                maxBytes))
        {
            return luaL_error(state,
                "task.start: network.request numeric options are out of range");
        }
        arguments.emplace("timeoutMs", std::to_string(timeoutMs));
        arguments.emplace("cacheSeconds", std::to_string(cacheSeconds));
        arguments.emplace("maxBytes", std::to_string(maxBytes));
    }
    else if (taskName == "shell.openUri")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: shell.openUri requires an arguments table");
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING ||
                std::string_view(lua_tostring(state, -2)) != "url")
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: shell.openUri accepts only url");
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "url");
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "task.start: shell.openUri url must be a string");
        }
        size_t urlLength = 0;
        const char* urlValue = lua_tolstring(state, -1, &urlLength);
        std::string url(urlValue ? urlValue : "", urlLength);
        lua_pop(state, 1);
        if (url.empty() || url.size() > 2048 ||
            url.find('\0') != std::string::npos ||
            !IsValidUtf8Local(url))
        {
            return luaL_error(state,
                "task.start: shell.openUri url must contain 1 to 2048 bytes of valid UTF-8");
        }
        arguments.emplace("url", std::move(url));
    }
    else if (taskName == "calendar.create" ||
        taskName == "calendar.update" ||
        taskName == "calendar.remove")
    {
        if (!hasArguments)
            return luaL_error(state,
                "task.start: %s requires an arguments table",
                taskName.c_str());
        const bool update = taskName == "calendar.update";
        const bool remove = taskName == "calendar.remove";
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: calendar argument keys must be strings");
            }
            size_t keyLength = 0;
            const char* keyValue = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(
                keyValue ? keyValue : "", keyLength);
            const bool known = key == "id" ||
                (!remove && (key == "title" || key == "date" ||
                    key == "allDay" || key == "startMinutes" ||
                    key == "endMinutes" || key == "notes" ||
                    key == "reminderMinutes" ||
                    key == "expectedRevision"));
            const bool allowed = known &&
                (update || key != "expectedRevision") &&
                ((update || remove) || key != "id");
            if (!allowed)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "task.start: %s received an unknown argument",
                    taskName.c_str());
            }
            lua_pop(state, 1);
        }

        const auto readText = [&](const char* field,
            std::size_t maximum, bool allowEmpty,
            std::string& output) -> bool {
            lua_getfield(state, 2, field);
            if (lua_type(state, -1) != LUA_TSTRING)
            {
                lua_pop(state, 1);
                return false;
            }
            size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            output.assign(value ? value : "", length);
            lua_pop(state, 1);
            return output.size() <= maximum &&
                (allowEmpty || !output.empty()) &&
                output.find('\0') == std::string::npos &&
                (output.empty() || IsValidUtf8Local(output));
        };
        const auto readInteger = [&](const char* field,
            lua_Integer& output) -> bool {
            lua_getfield(state, 2, field);
            const bool valid = lua_isinteger(state, -1);
            if (valid) output = lua_tointeger(state, -1);
            lua_pop(state, 1);
            return valid;
        };

        if (update || remove)
        {
            std::string id;
            if (!readText("id", 128, false, id))
                return luaL_error(state,
                    "task.start: %s id must contain 1 to 128 bytes of valid UTF-8",
                    taskName.c_str());
            arguments.emplace("id", std::move(id));
        }
        if (!remove)
        {
            std::string title;
            std::string date;
            std::string notes;
            if (!readText("title", 512, false, title))
                return luaL_error(state,
                    "task.start: calendar title must contain 1 to 512 bytes of valid UTF-8");
            if (!readText("date", 10, false, date) ||
                !snowdesktop::calendar::CalendarService::GetDateInfo(date))
                return luaL_error(state,
                    "task.start: calendar date must be a valid YYYY-MM-DD date");
            if (!readText("notes", 8192, true, notes))
                return luaL_error(state,
                    "task.start: calendar notes must contain at most 8192 bytes of valid UTF-8");

            lua_getfield(state, 2, "allDay");
            if (!lua_isboolean(state, -1))
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "task.start: calendar allDay must be a boolean");
            }
            const bool allDay = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);

            lua_Integer startMinutes = 0;
            lua_Integer endMinutes = 0;
            lua_Integer reminderMinutes = -1;
            if (!readInteger("startMinutes", startMinutes) ||
                startMinutes < 0 || startMinutes > 1439 ||
                !readInteger("endMinutes", endMinutes) ||
                endMinutes < 0 || endMinutes > 1439 ||
                (!allDay && endMinutes < startMinutes))
            {
                return luaL_error(state,
                    "task.start: calendar time range is invalid");
            }
            if (!readInteger("reminderMinutes", reminderMinutes) ||
                (reminderMinutes != -1 && reminderMinutes != 0 &&
                    reminderMinutes != 5 && reminderMinutes != 15 &&
                    reminderMinutes != 30 && reminderMinutes != 60 &&
                    reminderMinutes != 1440))
            {
                return luaL_error(state,
                    "task.start: calendar reminderMinutes is unsupported");
            }

            arguments.emplace("title", std::move(title));
            arguments.emplace("date", std::move(date));
            arguments.emplace("notes", std::move(notes));
            arguments.emplace("allDay", allDay ? "1" : "0");
            arguments.emplace("startMinutes", std::to_string(startMinutes));
            arguments.emplace("endMinutes", std::to_string(endMinutes));
            arguments.emplace("reminderMinutes",
                std::to_string(reminderMinutes));

            if (update)
            {
                lua_Integer expectedRevision = 0;
                if (!readInteger("expectedRevision", expectedRevision) ||
                    expectedRevision <= 0 ||
                    expectedRevision > std::numeric_limits<int>::max())
                {
                    return luaL_error(state,
                        "task.start: calendar expectedRevision must be a positive integer");
                }
                arguments.emplace("expectedRevision",
                    std::to_string(expectedRevision));
            }
        }
    }
    else if (hasArguments)
    {
        lua_pushnil(state);
        if (lua_next(state, 2) != 0)
        {
            lua_pop(state, 2);
            return luaL_error(state,
                "task.start: this task does not accept arguments");
        }
    }

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "task.start: host is unavailable");
    auto result = d2d->engine->RuntimeStartTask(
        BoundWidgetId(state), BoundWidgetRuntimeToken(state),
        taskName, std::move(arguments));
    if (!result)
    {
        lua_pushnil(state);
        lua_pushlstring(state, result.error.data(), result.error.size());
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(result.id));
    return 1;
}

static int lua_TaskCancel(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state, "task.cancel: expected one task ID");
    const lua_Integer id = luaL_checkinteger(state, 1);
    if (id <= 0)
        return luaL_error(state, "task.cancel: task ID must be positive");
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeCancelTask(
            BoundWidgetId(state), BoundWidgetRuntimeToken(state),
            static_cast<std::uint64_t>(id)));
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
    const int requestedMaxBytes = integerOption("maxBytes", 0);
    const std::size_t maximumUtf8Bytes = requestedMaxBytes > 0
        ? static_cast<std::size_t>(std::min(requestedMaxBytes, 64 * 1024))
        : 0;

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
        control.maximumUtf8Bytes = maximumUtf8Bytes;
        if (BoundWidgetApiVersion(L) >= 2)
        {
            std::string error;
            if (!s->engine->RuntimeRegisterV2HostControl(
                    BoundWidgetId(L), std::move(control), error))
            {
                return luaL_error(L, "control.textInput: %s",
                    error.c_str());
            }
        }
        else
        {
            s->engine->RuntimeRegisterHostControl(
                BoundWidgetId(L), std::move(control));
        }
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
    const int requestedMaxBytes = integerOption("maxBytes", 0);
    const std::size_t maximumUtf8Bytes = requestedMaxBytes > 0
        ? static_cast<std::size_t>(std::min(requestedMaxBytes, 64 * 1024))
        : 0;

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
        control.maximumUtf8Bytes = maximumUtf8Bytes;
        if (BoundWidgetApiVersion(L) >= 2)
        {
            std::string error;
            if (!s->engine->RuntimeRegisterV2HostControl(
                    BoundWidgetId(L), std::move(control), error))
            {
                return luaL_error(L, "control.textArea: %s",
                    error.c_str());
            }
        }
        else
        {
            s->engine->RuntimeRegisterHostControl(
                BoundWidgetId(L), std::move(control));
        }
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

static void ValidateControlTableFields(lua_State* state, int table,
    std::span<const char* const> allowed, const char* api)
{
    table = lua_absindex(state, table);
    if (lua_getmetatable(state, table) != 0)
    {
        lua_pop(state, 1);
        luaL_error(state, "%s: descriptor tables cannot have metatables", api);
        return;
    }
    lua_pushnil(state);
    while (lua_next(state, table) != 0)
    {
        std::size_t length = 0;
        const char* key = lua_type(state, -2) == LUA_TSTRING
            ? lua_tolstring(state, -2, &length) : nullptr;
        const bool known = key && std::any_of(
            allowed.begin(), allowed.end(), [&](const char* candidate) {
                return std::string_view(key, length) == candidate;
            });
        if (!known)
        {
            const std::string field = key
                ? std::string(key, length) : "<non-string>";
            lua_pop(state, 2);
            luaL_error(state, "%s: unknown field '%s'", api,
                field.c_str());
            return;
        }
        lua_pop(state, 1);
    }
}

static std::string ReadControlIdentifier(lua_State* state, int table,
    const char* field, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_type(state, -1) != LUA_TSTRING)
    {
        luaL_error(state, "%s: %s must be a string", api, field);
        return {};
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    const std::string result(value ? value : "", length);
    lua_pop(state, 1);
    if (result.empty() || result.size() > 128 ||
        result.find('\0') != std::string::npos ||
        !IsValidUtf8Local(result))
    {
        luaL_error(state,
            "%s: %s must contain 1 to 128 bytes of valid UTF-8",
            api, field);
        return {};
    }
    return result;
}

static double ReadControlNumber(lua_State* state, int table,
    const char* field, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_type(state, -1) != LUA_TNUMBER)
    {
        luaL_error(state, "%s: %s must be a number", api, field);
        return 0.0;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return value;
}

static void ValidateOptionalControlString(lua_State* state, int table,
    const char* field, std::size_t maximumBytes, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return;
    }
    if (lua_type(state, -1) != LUA_TSTRING)
    {
        luaL_error(state, "%s: %s must be a string", api, field);
        return;
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, -1, &length);
    const std::string text(value ? value : "", length);
    lua_pop(state, 1);
    if (length > maximumBytes || text.find('\0') != std::string::npos ||
        (!text.empty() && !IsValidUtf8Local(text)))
    {
        luaL_error(state, "%s: %s must be at most %d bytes of valid UTF-8",
            api, field, static_cast<int>(maximumBytes));
    }
}

static void ValidateOptionalControlBoolean(lua_State* state, int table,
    const char* field, const char* api)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TBOOLEAN)
    {
        luaL_error(state, "%s: %s must be a boolean", api, field);
        return;
    }
    lua_pop(state, 1);
}

static void ValidateOptionalControlNumber(lua_State* state, int table,
    const char* field, double minimum, double maximum, const char* api,
    bool integer = false)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return;
    }
    if (lua_type(state, -1) != LUA_TNUMBER ||
        (integer && !lua_isinteger(state, -1)))
    {
        luaL_error(state, "%s: %s must be %s", api, field,
            integer ? "an integer" : "a number");
        return;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    if (!std::isfinite(value) || value < minimum || value > maximum)
    {
        luaL_error(state, "%s: %s is outside the supported range",
            api, field);
    }
}

static int LuaControlText(lua_State* state, bool multiline)
{
    const char* api = multiline
        ? "control.textArea" : "control.textInput";
    if (lua_gettop(state) != 1)
        return luaL_error(state, "%s: expected one descriptor", api);
    luaL_checktype(state, 1, LUA_TTABLE);
    const int descriptor = lua_absindex(state, 1);
    static constexpr const char* inputFields[] = {
        "key", "storageKey", "shape", "placeholder", "fontSize",
        "textColor", "placeholderColor", "backgroundColor",
        "borderColor", "focusedBorderColor", "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha", "focusedBorderAlpha",
        "radius", "padding", "borderThickness", "selectAll",
        "liveUpdate", "maxBytes",
    };
    static constexpr const char* areaFields[] = {
        "key", "storageKey", "shape", "placeholder",
        "placeholderWhenWhitespace", "fontSize", "textColor",
        "placeholderColor", "backgroundColor", "borderColor",
        "focusedBorderColor", "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha", "focusedBorderAlpha",
        "radius", "padding", "borderThickness", "selectAll",
        "liveUpdate", "maxBytes",
    };
    ValidateControlTableFields(state, descriptor,
        multiline ? std::span<const char* const>(areaFields)
                  : std::span<const char* const>(inputFields), api);
    const std::string key = ReadControlIdentifier(
        state, descriptor, "key", api);
    const std::string storageKey = ReadControlIdentifier(
        state, descriptor, "storageKey", api);

    lua_getfield(state, descriptor, "shape");
    if (lua_type(state, -1) != LUA_TTABLE)
        return luaL_error(state, "%s: shape must be a table", api);
    const int shape = lua_absindex(state, -1);
    static constexpr const char* shapeFields[] = {
        "type", "x", "y", "width", "height",
    };
    ValidateControlTableFields(state, shape, shapeFields, api);
    lua_getfield(state, shape, "type");
    if (lua_type(state, -1) != LUA_TSTRING ||
        std::string_view(lua_tostring(state, -1)) != "rect")
    {
        return luaL_error(state, "%s: shape type must be 'rect'", api);
    }
    lua_pop(state, 1);
    const double x = ReadControlNumber(state, shape, "x", api);
    const double y = ReadControlNumber(state, shape, "y", api);
    const double width = ReadControlNumber(state, shape, "width", api);
    const double height = ReadControlNumber(state, shape, "height", api);
    lua_pop(state, 1);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        std::abs(x) > 1'000'000.0 || std::abs(y) > 1'000'000.0 ||
        width <= 0.0 || height <= 0.0 ||
        width > 1'000'000.0 || height > 1'000'000.0)
    {
        return luaL_error(state,
            "%s: shape geometry must be finite, positive, and bounded", api);
    }

    ValidateOptionalControlString(
        state, descriptor, "placeholder", 4096, api);
    ValidateOptionalControlNumber(
        state, descriptor, "fontSize", 9.0, 96.0, api);
    for (const char* field : { "textColor", "placeholderColor",
        "backgroundColor", "borderColor", "focusedBorderColor" })
    {
        ValidateOptionalControlNumber(state, descriptor, field,
            0.0, 0xFFFFFF, api, true);
    }
    for (const char* field : { "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha",
        "focusedBorderAlpha" })
    {
        ValidateOptionalControlNumber(
            state, descriptor, field, 0.0, 1.0, api);
    }
    ValidateOptionalControlNumber(
        state, descriptor, "radius", 0.0, 4096.0, api);
    ValidateOptionalControlNumber(
        state, descriptor, "padding", 0.0, 4096.0, api);
    ValidateOptionalControlNumber(
        state, descriptor, "borderThickness", 0.5, 64.0, api);
    ValidateOptionalControlNumber(
        state, descriptor, "maxBytes", 1.0, 64 * 1024.0, api, true);
    ValidateOptionalControlBoolean(
        state, descriptor, "selectAll", api);
    ValidateOptionalControlBoolean(
        state, descriptor, "liveUpdate", api);
    if (multiline)
    {
        ValidateOptionalControlBoolean(state, descriptor,
            "placeholderWhenWhitespace", api);
    }

    lua_getfield(state, descriptor, "maxBytes");
    const bool hasMaxBytes = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (!hasMaxBytes)
    {
        lua_pushinteger(state, multiline ? 64 * 1024 : 4096);
        lua_setfield(state, descriptor, "maxBytes");
    }

    lua_pushlstring(state, key.data(), key.size());
    lua_pushlstring(state, storageKey.data(), storageKey.size());
    lua_pushnumber(state, x);
    lua_pushnumber(state, y);
    lua_pushnumber(state, width);
    lua_pushnumber(state, height);
    lua_pushvalue(state, descriptor);
    lua_remove(state, descriptor);
    return multiline
        ? lua_UiTextArea(state) : lua_UiTextInput(state);
}

static int lua_ControlTextInput(lua_State* state)
{
    return LuaControlText(state, false);
}

static int lua_ControlTextArea(lua_State* state)
{
    return LuaControlText(state, true);
}

static int lua_ControlFocus(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state, "control.focus: expected one key");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "control.focus: key must be a string");
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 1, &length);
    const std::string key(raw ? raw : "", length);
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "control.focus: key must contain 1 to 128 bytes of valid UTF-8");
    }
    auto* d2d = GetD2D(state);
    std::string error = "hostUnavailable";
    const bool focused = d2d && d2d->engine &&
        d2d->engine->RuntimeFocusHostInputFromTrustedGesture(
            BoundWidgetId(state), key, error);
    lua_pushboolean(state, focused ? 1 : 0);
    if (focused)
        lua_pushnil(state);
    else
        lua_pushlstring(state, error.data(), error.size());
    return 2;
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

struct WidgetSystemEnvironment
{
    bool highContrast = false;
    bool reducedMotion = false;
    double textScale = 1.0;
    int accentColor = 0x0078D4;
    std::string locale;
    std::string region;
    std::string timeZone;
    int utcOffsetMinutes = 0;
    std::string inputLanguage;
};

static WidgetSystemEnvironment QueryWidgetSystemEnvironment()
{
    static std::mutex cacheMutex;
    static WidgetSystemEnvironment cached;
    static ULONGLONG lastRefresh = 0;
    const ULONGLONG now = GetTickCount64();
    std::scoped_lock lock(cacheMutex);
    if (lastRefresh != 0 && now - lastRefresh < 1000)
        return cached;

    WidgetSystemEnvironment result;
    HIGHCONTRASTW highContrast{ sizeof(highContrast) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST,
            sizeof(highContrast), &highContrast, 0))
        result.highContrast =
            (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    BOOL animationsEnabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0,
            &animationsEnabled, 0))
        result.reducedMotion = animationsEnabled == FALSE;

    DWORD textScalePercent = 100;
    DWORD textScaleBytes = sizeof(textScalePercent);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Accessibility", L"TextScaleFactor",
            RRF_RT_REG_DWORD, nullptr, &textScalePercent,
            &textScaleBytes) == ERROR_SUCCESS)
    {
        result.textScale = std::clamp(
            static_cast<double>(textScalePercent) / 100.0, 0.5, 5.0);
    }

    DWORD colorization = 0;
    BOOL colorizationOpaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(
            &colorization, &colorizationOpaque)))
        result.accentColor = static_cast<int>(colorization & 0x00ffffffu);
    else
    {
        const COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
        result.accentColor = (GetRValue(accent) << 16) |
            (GetGValue(accent) << 8) | GetBValue(accent);
    }

    result.locale = Locale::Instance().GetEffectiveLanguage();
    std::wstring localeName = Utf8ToWideLocal(result.locale);
    wchar_t localeBuffer[LOCALE_NAME_MAX_LENGTH]{};
    if (localeName.empty() || GetLocaleInfoEx(localeName.c_str(),
            LOCALE_SISO3166CTRYNAME, localeBuffer,
            LOCALE_NAME_MAX_LENGTH) <= 0)
    {
        if (GetUserDefaultLocaleName(localeBuffer,
                LOCALE_NAME_MAX_LENGTH) > 0)
            localeName = localeBuffer;
        localeBuffer[0] = L'\0';
        GetLocaleInfoEx(localeName.c_str(), LOCALE_SISO3166CTRYNAME,
            localeBuffer, LOCALE_NAME_MAX_LENGTH);
    }
    result.region = WidgetWideToUtf8(localeBuffer);

    DYNAMIC_TIME_ZONE_INFORMATION timeZone{};
    const DWORD timeZoneId = GetDynamicTimeZoneInformation(&timeZone);
    const wchar_t* timeZoneName = timeZone.TimeZoneKeyName[0]
        ? timeZone.TimeZoneKeyName : timeZone.StandardName;
    result.timeZone = WidgetWideToUtf8(timeZoneName);
    LONG bias = timeZone.Bias;
    if (timeZoneId == TIME_ZONE_ID_STANDARD)
        bias += timeZone.StandardBias;
    else if (timeZoneId == TIME_ZONE_ID_DAYLIGHT)
        bias += timeZone.DaylightBias;
    result.utcOffsetMinutes = -static_cast<int>(bias);

    const LANGID inputLanguage = LOWORD(
        reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)));
    wchar_t inputLocale[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(MAKELCID(inputLanguage, SORT_DEFAULT),
            inputLocale, LOCALE_NAME_MAX_LENGTH, 0) > 0)
        result.inputLanguage = WidgetWideToUtf8(inputLocale);

    cached = result;
    lastRefresh = now;
    return result;
}

static void PushContextRect(lua_State* L, const RECT& rect,
    double scaleX, double scaleY)
{
    const double divisorX = scaleX > 0.0 ? scaleX : 1.0;
    const double divisorY = scaleY > 0.0 ? scaleY : 1.0;
    lua_createtable(L, 0, 6);
    lua_pushnumber(L, rect.left / divisorX); lua_setfield(L, -2, "left");
    lua_pushnumber(L, rect.top / divisorY); lua_setfield(L, -2, "top");
    lua_pushnumber(L, rect.right / divisorX); lua_setfield(L, -2, "right");
    lua_pushnumber(L, rect.bottom / divisorY); lua_setfield(L, -2, "bottom");
    lua_pushnumber(L, (rect.right - rect.left) / divisorX);
    lua_setfield(L, -2, "width");
    lua_pushnumber(L, (rect.bottom - rect.top) / divisorY);
    lua_setfield(L, -2, "height");
}

static int lua_WidgetContext(lua_State* L)
{
    auto* d2d = GetD2D(L);
    const std::wstring widgetId = BoundWidgetId(L);
    LuaWidgetContextState state = d2d && d2d->engine
        ? d2d->engine->RuntimeGetWidgetContextState(widgetId)
        : LuaWidgetContextState{};
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_preview");
    if (lua_toboolean(L, -1) != 0) state.preview = true;
    lua_pop(L, 1);
    const WidgetSystemEnvironment system = QueryWidgetSystemEnvironment();
    const double scaleX = static_cast<double>(state.surface.dpiX) /
        USER_DEFAULT_SCREEN_DPI;
    const double scaleY = static_cast<double>(state.surface.dpiY) /
        USER_DEFAULT_SCREEN_DPI;
    const double pixelWidth = d2d
        ? d2d->widgetRect.right - d2d->widgetRect.left : 0.0;
    const double pixelHeight = d2d
        ? d2d->widgetRect.bottom - d2d->widgetRect.top : 0.0;
    const int columns = d2d ? std::max(1, d2d->gridColumns) : 1;
    const int rows = d2d ? std::max(1, d2d->gridRows) : 1;
    const int area = columns * rows;
    const LuaWidgetTheme theme = d2d && d2d->engine
        ? d2d->engine->RuntimeGetWidgetTheme(widgetId)
        : LuaWidgetTheme{};

    lua_createtable(L, 0, 18);
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, pixelWidth / scaleX); lua_setfield(L, -2, "width");
    lua_pushnumber(L, pixelHeight / scaleY); lua_setfield(L, -2, "height");
    lua_setfield(L, -2, "logicalSize");
    lua_createtable(L, 0, 2);
    lua_pushnumber(L, pixelWidth); lua_setfield(L, -2, "width");
    lua_pushnumber(L, pixelHeight); lua_setfield(L, -2, "height");
    lua_setfield(L, -2, "pixelSize");
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, state.surface.dpiX); lua_setfield(L, -2, "x");
    lua_pushinteger(L, state.surface.dpiY); lua_setfield(L, -2, "y");
    lua_pushnumber(L, scaleX); lua_setfield(L, -2, "scaleX");
    lua_pushnumber(L, scaleY); lua_setfield(L, -2, "scaleY");
    lua_setfield(L, -2, "dpi");
    lua_pushstring(L, area <= 2 ? "small" :
        (area <= 6 ? "medium" : "large"));
    lua_setfield(L, -2, "sizeClass");
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, columns); lua_setfield(L, -2, "columns");
    lua_pushinteger(L, rows); lua_setfield(L, -2, "rows");
    lua_setfield(L, -2, "grid");

    lua_createtable(L, 0, 6);
    lua_pushboolean(L, state.surface.monitorAvailable);
    lua_setfield(L, -2, "available");
    lua_pushboolean(L, state.surface.primaryMonitor);
    lua_setfield(L, -2, "primary");
    PushContextRect(L, state.surface.monitorBounds, 1.0, 1.0);
    lua_setfield(L, -2, "pixelBounds");
    PushContextRect(L, state.surface.workArea, 1.0, 1.0);
    lua_setfield(L, -2, "pixelWorkArea");
    PushContextRect(L, state.surface.monitorBounds, scaleX, scaleY);
    lua_setfield(L, -2, "logicalBounds");
    PushContextRect(L, state.surface.workArea, scaleX, scaleY);
    lua_setfield(L, -2, "logicalWorkArea");
    lua_setfield(L, -2, "monitor");

    lua_createtable(L, 0, 6);
    lua_pushstring(L, theme.contentTheme == 1 ? "light" : "dark");
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, theme.bg); lua_setfield(L, -2, "background");
    lua_pushinteger(L, theme.border); lua_setfield(L, -2, "border");
    lua_pushliteral(L, "systemAccent"); lua_setfield(L, -2, "accentToken");
    lua_pushinteger(L, system.accentColor); lua_setfield(L, -2, "accentColor");
    lua_pushboolean(L, system.highContrast); lua_setfield(L, -2, "highContrast");
    lua_setfield(L, -2, "theme");

    lua_createtable(L, 0, 3);
    lua_pushboolean(L, system.highContrast); lua_setfield(L, -2, "highContrast");
    lua_pushboolean(L, system.reducedMotion); lua_setfield(L, -2, "reducedMotion");
    lua_pushnumber(L, system.textScale); lua_setfield(L, -2, "textScale");
    lua_setfield(L, -2, "accessibility");
    lua_pushlstring(L, system.locale.data(), system.locale.size());
    lua_setfield(L, -2, "locale");
    lua_pushlstring(L, system.region.data(), system.region.size());
    lua_setfield(L, -2, "region");
    lua_pushlstring(L, system.timeZone.data(), system.timeZone.size());
    lua_setfield(L, -2, "timeZone");
    lua_pushinteger(L, system.utcOffsetMinutes);
    lua_setfield(L, -2, "utcOffsetMinutes");
    lua_pushlstring(L, system.inputLanguage.data(),
        system.inputLanguage.size());
    lua_setfield(L, -2, "inputLanguage");
    lua_pushboolean(L, state.visible); lua_setfield(L, -2, "visible");
    lua_pushboolean(L, state.preview); lua_setfield(L, -2, "preview");
    lua_pushboolean(L, state.focused); lua_setfield(L, -2, "focused");
    lua_pushboolean(L, state.selected); lua_setfield(L, -2, "selected");
    lua_pushstring(L, state.preview ? "preview" :
        (d2d && d2d->surfaceKind ? d2d->surfaceKind : "desktop"));
    lua_setfield(L, -2, "surface");
    return 1;
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

static int lua_TimeNow(lua_State* L)
{
    const auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    lua_pushinteger(L, static_cast<lua_Integer>(milliseconds));
    return 1;
}

static int lua_TimeMonotonic(lua_State* L)
{
    const auto milliseconds = std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    lua_pushinteger(L, static_cast<lua_Integer>(milliseconds));
    return 1;
}

static int LuaTimeOptions(lua_State* L, int index)
{
    if (lua_isnoneornil(L, index)) return 0;
    luaL_checktype(L, index, LUA_TTABLE);
    return lua_absindex(L, index);
}

static std::string LuaTimeStringOption(lua_State* L, int options,
    const char* name, const char* defaultValue)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    size_t length = 0;
    const char* value = luaL_checklstring(L, -1, &length);
    if (length == 0 || length > 128)
        luaL_error(L, "time: option '%s' has an invalid length", name);
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
}

static std::int64_t LuaTimeDeltaField(lua_State* L, int delta,
    const char* name)
{
    lua_getfield(L, delta, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return 0;
    }
    int isNumber = 0;
    const lua_Integer value = lua_tointegerx(L, -1, &isNumber);
    if (!isNumber || value < -1000000 || value > 1000000)
    {
        luaL_error(L,
            "time.add: '%s' must be an integer from -1000000 to 1000000",
            name);
    }
    lua_pop(L, 1);
    return static_cast<std::int64_t>(value);
}

static int lua_TimeParts(lua_State* L)
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto milliseconds = lua_isnoneornil(L, 1)
        ? now : static_cast<long long>(luaL_checkinteger(L, 1));
    const char* zone = luaL_optstring(L, 2, "local");
    snowdesktop::widget_time::DateTimeParts parts;
    const auto error = snowdesktop::widget_time::Parts(
        milliseconds, zone ? zone : "local", parts);
    if (error != snowdesktop::widget_time::TimeError::None)
    {
        return luaL_error(L, "time.parts: %s",
            snowdesktop::widget_time::DescribeError(error));
    }

    lua_createtable(L, 0, 9);
    lua_pushinteger(L, parts.year); lua_setfield(L, -2, "year");
    lua_pushinteger(L, parts.month); lua_setfield(L, -2, "month");
    lua_pushinteger(L, parts.day); lua_setfield(L, -2, "day");
    lua_pushinteger(L, parts.weekday); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, parts.hour); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, parts.minute); lua_setfield(L, -2, "min");
    lua_pushinteger(L, parts.second); lua_setfield(L, -2, "sec");
    lua_pushinteger(L, parts.millisecond);
    lua_setfield(L, -2, "millisecond");
    lua_pushstring(L, zone ? zone : "local");
    lua_setfield(L, -2, "timeZone");
    return 1;
}

static int lua_TimeFormat(lua_State* L)
{
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto milliseconds = lua_isnoneornil(L, 1)
        ? now : static_cast<std::int64_t>(luaL_checkinteger(L, 1));
    const int options = LuaTimeOptions(L, 2);
    const std::string timeZone = LuaTimeStringOption(
        L, options, "timeZone", "local");
    const std::string locale = LuaTimeStringOption(L, options, "locale",
        Locale::Instance().GetEffectiveLanguage().c_str());
    const std::string dateStyleName = LuaTimeStringOption(
        L, options, "dateStyle", "short");
    const std::string timeStyleName = LuaTimeStringOption(
        L, options, "timeStyle", "short");
    snowdesktop::widget_time::DateStyle dateStyle;
    if (dateStyleName == "none")
        dateStyle = snowdesktop::widget_time::DateStyle::None;
    else if (dateStyleName == "short")
        dateStyle = snowdesktop::widget_time::DateStyle::Short;
    else if (dateStyleName == "long")
        dateStyle = snowdesktop::widget_time::DateStyle::Long;
    else
        return luaL_error(L,
            "time.format: dateStyle must be 'none', 'short', or 'long'");
    snowdesktop::widget_time::TimeStyle timeStyle;
    if (timeStyleName == "none")
        timeStyle = snowdesktop::widget_time::TimeStyle::None;
    else if (timeStyleName == "short")
        timeStyle = snowdesktop::widget_time::TimeStyle::Short;
    else if (timeStyleName == "long")
        timeStyle = snowdesktop::widget_time::TimeStyle::Long;
    else
        return luaL_error(L,
            "time.format: timeStyle must be 'none', 'short', or 'long'");

    std::string result;
    const auto error = snowdesktop::widget_time::Format(milliseconds,
        locale, timeZone, dateStyle, timeStyle, result);
    if (error != snowdesktop::widget_time::TimeError::None)
    {
        return luaL_error(L, "time.format: %s",
            snowdesktop::widget_time::DescribeError(error));
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_TimeAdd(lua_State* L)
{
    const auto milliseconds = static_cast<std::int64_t>(
        luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    const int deltaTable = lua_absindex(L, 2);
    snowdesktop::widget_time::AddDelta delta;
    delta.years = LuaTimeDeltaField(L, deltaTable, "years");
    delta.months = LuaTimeDeltaField(L, deltaTable, "months");
    delta.days = LuaTimeDeltaField(L, deltaTable, "days");
    delta.hours = LuaTimeDeltaField(L, deltaTable, "hours");
    delta.minutes = LuaTimeDeltaField(L, deltaTable, "minutes");
    delta.seconds = LuaTimeDeltaField(L, deltaTable, "seconds");
    delta.milliseconds = LuaTimeDeltaField(
        L, deltaTable, "milliseconds");
    const int options = LuaTimeOptions(L, 3);
    const std::string timeZone = LuaTimeStringOption(
        L, options, "timeZone", "local");
    std::int64_t result = 0;
    const auto error = snowdesktop::widget_time::Add(
        milliseconds, delta, timeZone, result);
    if (error != snowdesktop::widget_time::TimeError::None)
    {
        return luaL_error(L, "time.add: %s",
            snowdesktop::widget_time::DescribeError(error));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(result));
    return 1;
}

static int lua_TimeCompare(lua_State* L)
{
    const lua_Integer left = luaL_checkinteger(L, 1);
    const lua_Integer right = luaL_checkinteger(L, 2);
    lua_pushinteger(L, left < right ? -1 : (left > right ? 1 : 0));
    return 1;
}

static const char* ProcessArchitectureName()
{
#if defined(_M_ARM64)
    return "arm64";
#elif defined(_M_X64)
    return "x64";
#elif defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

static const char* NativeArchitectureName()
{
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64:
        return "x64";
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "x86";
    default:
        return "unknown";
    }
}

static DWORD WindowsBuildNumber()
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module
        ? reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(module, "RtlGetVersion"))
        : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return function && function(&version) == 0
        ? version.dwBuildNumber : 0;
}

static int lua_SystemInfoV2(lua_State* L)
{
    const bool packaged = snowdesktop::deployment::IsPackaged();
    lua_createtable(L, 0, 9);
    lua_pushliteral(L, "windows"); lua_setfield(L, -2, "osFamily");
    const DWORD build = WindowsBuildNumber();
    if (build != 0)
    {
        lua_pushinteger(L, static_cast<lua_Integer>(build));
        lua_setfield(L, -2, "osBuild");
    }
    lua_pushstring(L, ProcessArchitectureName());
    lua_setfield(L, -2, "processArchitecture");
    lua_pushstring(L, NativeArchitectureName());
    lua_setfield(L, -2, "nativeArchitecture");
    lua_pushliteral(L, SNOWDESKTOP_VERSION);
    lua_setfield(L, -2, "hostVersion");
    lua_pushinteger(L, snowdesktop::widget::kHostApiVersion);
    lua_setfield(L, -2, "apiVersion");
    lua_pushboolean(L, packaged ? 1 : 0);
    lua_setfield(L, -2, "packaged");
    lua_pushboolean(L, packaged ? 0 : 1);
    lua_setfield(L, -2, "portable");
    lua_pushstring(L, packaged ? "packaged" : "portable");
    lua_setfield(L, -2, "deploymentMode");
    return 1;
}

static int lua_SystemUptime(lua_State* L)
{
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, static_cast<lua_Integer>(GetTickCount64()));
    lua_setfield(L, -2, "milliseconds");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "includesSleep");
    return 1;
}

static char kModuleLoadingMarker = 0;

static void ClearModuleCacheEntry(lua_State* L, int cache,
    const std::string& key)
{
    lua_pushnil(L);
    lua_setfield(L, cache, key.c_str());
}

static int lua_ModuleRequire(lua_State* L)
{
    size_t pathLength = 0;
    const char* pathRaw = luaL_checklstring(L, 1, &pathLength);
    if (pathLength == 0 || pathLength > 512)
        return luaL_error(L, "module.require: path length is invalid");

    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_loading");
    const bool loading = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (!loading)
    {
        return luaL_error(L,
            "module.require: modules can only be loaded while the entry script is loading");
    }

    const std::wstring relativePath = Utf8ToWideLocal(
        std::string(pathRaw, pathLength));
    if (relativePath.empty() ||
        _wcsicmp(std::filesystem::path(relativePath).extension().c_str(),
            L".lua") != 0)
        return luaL_error(L, "module.require: path must name a .lua file");
    const auto fullPath = ResolveCurrentPackageAsset(L, relativePath);
    if (!fullPath)
        return luaL_error(L, "module.require: path is outside the component package");

    std::error_code fileError;
    const auto fileSize = std::filesystem::file_size(*fullPath, fileError);
    if (fileError || fileSize > snowdesktop::widget::kMaxEntryLuaBytes)
    {
        return luaL_error(L,
            "module.require: module is missing or exceeds the 1 MiB limit");
    }
    const std::string cacheKey = WidgetWideToUtf8(*fullPath);
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_cache");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_cache");
    }
    const int cache = lua_absindex(L, -1);
    lua_getfield(L, cache, cacheKey.c_str());
    if (lua_touserdata(L, -1) == &kModuleLoadingMarker)
        return luaL_error(L, "module.require: circular dependency detected");
    if (!lua_isnil(L, -1))
    {
        lua_remove(L, cache);
        return 1;
    }
    lua_pop(L, 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
    const lua_Integer moduleCount = lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
    const lua_Integer moduleBytes = lua_tointeger(L, -1);
    lua_pop(L, 1);
    if (moduleCount >= 64 || moduleBytes < 0 ||
        moduleBytes > 4 * 1024 * 1024 ||
        fileSize > 4ull * 1024ull * 1024ull -
            static_cast<std::uint64_t>(moduleBytes))
    {
        return luaL_error(L,
            "module.require: per-instance module count or byte quota exceeded");
    }

    lua_pushlightuserdata(L, &kModuleLoadingMarker);
    lua_setfield(L, cache, cacheKey.c_str());
    std::ifstream file(*fullPath, std::ios::binary);
    std::string source(static_cast<std::size_t>(fileSize), '\0');
    if (!file || (fileSize > 0 &&
        !file.read(source.data(), static_cast<std::streamsize>(fileSize))))
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        return luaL_error(L, "module.require: module could not be read");
    }
    const std::string chunkName = "@module/" +
        WidgetWideToUtf8(std::filesystem::path(relativePath).
            lexically_normal().generic_wstring());
    if (luaL_loadbuffer(L, source.data(), source.size(),
            chunkName.c_str()) != LUA_OK)
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        return lua_error(L);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_environment");
    if (!lua_istable(L, -1) || lua_setupvalue(L, -2, 1) == nullptr)
    {
        if (lua_isnil(L, -1)) lua_pop(L, 1);
        ClearModuleCacheEntry(L, cache, cacheKey);
        return luaL_error(L, "module.require: sandbox environment is unavailable");
    }
    if (snowdesktop::lua_runtime::ProtectedCall(L, 0, 1) != LUA_OK)
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        return lua_error(L);
    }
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, cache, cacheKey.c_str());
    lua_pushinteger(L, moduleCount + 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
    lua_pushinteger(L, moduleBytes + static_cast<lua_Integer>(fileSize));
    lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
    lua_remove(L, cache);
    return 1;
}

static int lua_WidgetHasPermission(lua_State* L)
{
    const char* permission = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_permissions");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_pushboolean(L, false);
        return 1;
    }
    lua_getfield(L, -1, permission ? permission : "");
    const bool granted = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    lua_pushboolean(L, granted);
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

static int lua_CalendarSelectDate(lua_State* L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "calendar.selectDate: expected one date");
    size_t length = 0;
    const char* value = luaL_checklstring(L, 1, &length);
    const std::string date(value ? value : "", length);
    auto* state = GetD2D(L);
    lua_pushboolean(L, state && state->engine && length == 10 &&
        date.find('\0') == std::string::npos &&
        state->engine->RuntimeCalendarSetSelectedDate(date));
    return 1;
}

static int lua_CalendarDateInfo(lua_State* L)
{
    const int apiVersion = BoundWidgetApiVersion(L);
    if (apiVersion >= 2 && lua_gettop(L) != 1)
        return luaL_error(L, "calendar.dateInfo: expected one date");
    if (apiVersion < 2 &&
        !RequirePermission(L, "calendar.read"))
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
    const int apiVersion = BoundWidgetApiVersion(L);
    if (apiVersion >= 2 && lua_gettop(L) != 2)
        return luaL_error(L,
            "calendar.addDays: expected a date and offset");
    if (apiVersion < 2 &&
        !RequirePermission(L, "calendar.read"))
        return 0;
    const char* value = luaL_checkstring(L, 1);
    const lua_Integer requestedOffset = luaL_checkinteger(L, 2);
    if (apiVersion >= 2 &&
        (requestedOffset < -366000 || requestedOffset > 366000))
    {
        return luaL_error(L,
            "calendar.addDays: offset must be between -366000 and 366000");
    }
    const int offset = static_cast<int>(requestedOffset);
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
    state->runtimeImageBitmaps.clear();
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

static ID2D1Bitmap1* LoadRuntimeImageBitmap(
    D2DState* state, const std::wstring& widgetId,
    std::string_view token)
{
    if (!state || !state->ctx || widgetId.empty() ||
        !snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(token))
        return nullptr;
    EnsureBitmapCachesForCurrentDevice(state);
    const std::string key(token);
    if (const auto cached = state->runtimeImageBitmaps.find(key);
        cached != state->runtimeImageBitmaps.end())
        return cached->second.Get();
    const auto source = state->runtimeImages.find(key);
    if (source == state->runtimeImages.end() || !source->second.pixels ||
        source->second.ownerWidgetId != widgetId)
        return nullptr;
    const auto& pixels = *source->second.pixels;
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(state->ctx->CreateBitmap(
            D2D1::SizeU(pixels.width, pixels.height),
            pixels.bgraPremultiplied.data(), pixels.stride,
            &properties, &bitmap)) || !bitmap)
        return nullptr;
    ID2D1Bitmap1* result = bitmap.Get();
    state->runtimeImageBitmaps.emplace(key, std::move(bitmap));
    return result;
}

static bool LuaTableUsesOnlyFields(lua_State* state, int index,
    std::span<const std::string_view> allowed,
    std::string_view context, std::string& error)
{
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = std::string(context) + " cannot have a metatable";
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        if (lua_type(state, -2) != LUA_TSTRING)
        {
            lua_pop(state, 2);
            error = std::string(context) + " keys must be strings";
            return false;
        }
        std::size_t length = 0;
        const char* key = lua_tolstring(state, -2, &length);
        const std::string_view field(key ? key : "", length);
        if (std::find(allowed.begin(), allowed.end(), field) ==
            allowed.end())
        {
            lua_pop(state, 2);
            error = std::string(context) + " has unsupported field '" +
                std::string(field) + "'";
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

static bool LuaTableUsesOnlyFields(lua_State* state, int index,
    std::initializer_list<std::string_view> allowed,
    std::string_view context, std::string& error)
{
    return LuaTableUsesOnlyFields(state, index,
        std::span<const std::string_view>(allowed.begin(), allowed.size()),
        context, error);
}

static bool LuaTableIsContiguousArray(lua_State* state, int index,
    std::string_view context, std::string& error)
{
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = std::string(context) + " cannot have a metatable";
        return false;
    }
    const std::size_t length = lua_rawlen(state, index);
    std::size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        ++count;
        const bool valid = lua_isinteger(state, -2) &&
            lua_tointeger(state, -2) > 0 &&
            static_cast<std::size_t>(lua_tointeger(state, -2)) <= length;
        lua_pop(state, 1);
        if (!valid)
        {
            lua_pop(state, 1);
            error = std::string(context) +
                " must use contiguous integer keys";
            return false;
        }
    }
    if (count != length)
    {
        error = std::string(context) +
            " must use contiguous integer keys";
        return false;
    }
    return true;
}

static int lua_ViewVirtualRange(lua_State* state)
{
    using snowdesktop::widget_runtime::ComputeViewVirtualRange;
    using snowdesktop::widget_runtime::ViewTreeLimits;
    using snowdesktop::widget_runtime::ViewVirtualRange;
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    const int descriptor = lua_absindex(state, 1);
    std::string error;
    if (!LuaTableUsesOnlyFields(state, descriptor,
            { "key", "itemCount", "itemExtent", "viewportExtent",
                "columns", "rowGap", "overscan" },
            "view.virtualRange", error))
        return luaL_error(state, "view.virtualRange: %s", error.c_str());

    const std::string key = ReadRequiredStringField(
        state, descriptor, "key");
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos || !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "view.virtualRange: key must contain 1 to 128 bytes of valid UTF-8");
    }
    const auto readInteger = [&](const char* field, lua_Integer fallback) {
        lua_getfield(state, descriptor, field);
        const lua_Integer value = lua_isnil(state, -1)
            ? fallback : luaL_checkinteger(state, -1);
        lua_pop(state, 1);
        return value;
    };
    const auto readNumber = [&](const char* field, double fallback) {
        lua_getfield(state, descriptor, field);
        const double value = lua_isnil(state, -1)
            ? fallback : static_cast<double>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        return value;
    };
    const lua_Integer itemCountValue = readInteger("itemCount", -1);
    const lua_Integer columnsValue = readInteger("columns", 1);
    const lua_Integer overscanValue = readInteger("overscan", 2);
    const double itemExtentValue = readNumber(
        "itemExtent", std::numeric_limits<double>::quiet_NaN());
    const double viewportExtentValue = readNumber(
        "viewportExtent", std::numeric_limits<double>::quiet_NaN());
    const double rowGapValue = readNumber("rowGap", 0.0);
    if (itemCountValue < 0 ||
        itemCountValue > static_cast<lua_Integer>(
            ViewTreeLimits::MaximumVirtualItemCount) ||
        columnsValue <= 0 || columnsValue > 64 ||
        overscanValue < 0 ||
        overscanValue > static_cast<lua_Integer>(
            ViewTreeLimits::MaximumVirtualOverscan))
    {
        return luaL_error(state,
            "view.virtualRange: integer arguments exceed their limits");
    }

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "view.virtualRange: host context is unavailable");
    ViewVirtualRange range;
    if (!ComputeViewVirtualRange(
            static_cast<std::size_t>(itemCountValue),
            static_cast<float>(itemExtentValue),
            static_cast<std::size_t>(columnsValue),
            static_cast<float>(rowGapValue),
            static_cast<float>(viewportExtentValue),
            static_cast<float>(d2d->engine->RuntimeGetScrollOffset(
                BoundWidgetId(state), key)),
            static_cast<std::size_t>(overscanValue), range, error))
        return luaL_error(state, "view.virtualRange: %s", error.c_str());

    lua_createtable(state, 0, 6);
    lua_pushinteger(state, static_cast<lua_Integer>(range.firstIndex));
    lua_setfield(state, -2, "firstIndex");
    lua_pushinteger(state, static_cast<lua_Integer>(range.lastIndex));
    lua_setfield(state, -2, "lastIndex");
    lua_pushnumber(state, range.offset);
    lua_setfield(state, -2, "offset");
    lua_pushnumber(state, range.maximum);
    lua_setfield(state, -2, "maximum");
    lua_pushnumber(state, range.viewportExtent);
    lua_setfield(state, -2, "viewportExtent");
    lua_pushnumber(state, range.contentExtent);
    lua_setfield(state, -2, "contentExtent");
    return 1;
}

static int LuaDrawColor(lua_State* state, int index,
    int fallback, const char* api)
{
    if (lua_isnoneornil(state, index)) return fallback;
    const lua_Integer value = luaL_checkinteger(state, index);
    if (value < 0 || value > 0xFFFFFF)
        luaL_error(state, "%s: colors must be between 0 and 0xFFFFFF", api);
    return static_cast<int>(value);
}

static float LuaDrawAlpha(lua_State* state, int index, const char* api)
{
    const float value = static_cast<float>(luaL_optnumber(state, index, 1.0));
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
        luaL_error(state, "%s: alpha must be between 0 and 1", api);
    return value;
}

static ID2D1Bitmap1* ResolveDrawImageBitmap(lua_State* state,
    D2DState* d2d, int index, const char* api, bool allowLegacyPath)
{
    if (!d2d || !d2d->engine) return nullptr;
    std::optional<std::wstring> fullPath;
    ID2D1Bitmap1* bitmap = nullptr;
    if (const auto* handle = TestResourceHandle(state, index))
    {
        if (handle->type != LuaResourceType::Image)
            luaL_error(state, "%s: invalid image resource handle", api);
        if (snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(
                handle->name))
            bitmap = LoadRuntimeImageBitmap(
                d2d, BoundWidgetId(state), handle->name);
        else
            fullPath = ResolveResourceHandlePath(
                state, index, LuaResourceType::Image);
        if (!bitmap && !fullPath)
            luaL_error(state, "%s: invalid image resource handle", api);
    }
    else
    {
        if (!allowLegacyPath)
            luaL_error(state, "%s: API v2 requires an image handle", api);
        const char* pathRaw = luaL_checkstring(state, index);
        const std::wstring path = Utf8ToWideLocal(pathRaw ? pathRaw : "");
        if (path.empty()) return nullptr;
        fullPath = d2d->engine->RuntimeResolvePackageAsset(
            BoundWidgetId(state), path);
    }
    if (!bitmap && fullPath) bitmap = LoadImageBitmap(d2d, *fullPath);
    return bitmap;
}

static int lua_DrawArc(lua_State* state)
{
    const float centerX = static_cast<float>(luaL_checknumber(state, 1));
    const float centerY = static_cast<float>(luaL_checknumber(state, 2));
    const float radius = static_cast<float>(luaL_checknumber(state, 3));
    const float start = static_cast<float>(luaL_checknumber(state, 4));
    const float sweep = static_cast<float>(luaL_checknumber(state, 5));
    const float thickness = static_cast<float>(
        luaL_optnumber(state, 6, 1.0));
    const int color = LuaDrawColor(state, 7, 0xFFFFFF, "draw.arc");
    const float alpha = LuaDrawAlpha(state, 8, "draw.arc");
    if (!std::isfinite(thickness) || thickness <= 0.0f ||
        thickness > 4096.0f)
        return luaL_error(state,
            "draw.arc: thickness must be positive and bounded");
    std::vector<snowdesktop::widget_runtime::DrawArcPiece> pieces;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildDrawArc(centerX, centerY,
            radius, start, sweep, pieces, error))
        return luaL_error(state, "draw.arc: %s", error.c_str());
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->ctx) return 0;
    ComPtr<ID2D1Factory> factory;
    d2d->ctx->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) ||
        !geometry || FAILED(geometry->Open(&sink)) || !sink)
        return 0;
    const auto absolute = [d2d](const auto& point) {
        return D2D1::Point2F(point.x + d2d->widgetRect.left,
            point.y + d2d->widgetRect.top);
    };
    sink->BeginFigure(absolute(pieces.front().start),
        D2D1_FIGURE_BEGIN_HOLLOW);
    for (const auto& piece : pieces)
        sink->AddArc(D2D1::ArcSegment(absolute(piece.end),
            D2D1::SizeF(piece.radius, piece.radius), 0.0f,
            piece.clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE :
                D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
            D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return 0;
    if (ID2D1SolidColorBrush* brush = GetCachedBrush(d2d, color, alpha))
        d2d->ctx->DrawGeometry(geometry.Get(), brush, thickness);
    return 0;
}

static int lua_DrawPath(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error;
    if (!LuaTableIsContiguousArray(state, 1, "draw.path commands", error))
        return luaL_error(state, "draw.path: %s", error.c_str());
    const std::size_t count = lua_rawlen(state, 1);
    if (count == 0 || count > 256)
        return luaL_error(state,
            "draw.path: commands must contain 1 to 256 items");
    using Command = snowdesktop::widget_runtime::DrawPathCommand;
    using Type = snowdesktop::widget_runtime::DrawPathCommandType;
    std::vector<Command> commands;
    commands.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index + 1));
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "draw.path: every command must be a table");
        }
        lua_getfield(state, -1, "op");
        const char* operation = luaL_checkstring(state, -1);
        const std::string op = operation ? operation : "";
        lua_pop(state, 1);
        std::vector<std::string_view> allowed{ "op" };
        Command command;
        if (op == "move" || op == "line")
        {
            allowed = { "op", "x", "y" };
            command.type = op == "move" ? Type::Move : Type::Line;
            lua_getfield(state, -1, "x");
            command.point.x = static_cast<float>(
                luaL_checknumber(state, -1));
            lua_pop(state, 1);
            lua_getfield(state, -1, "y");
            command.point.y = static_cast<float>(
                luaL_checknumber(state, -1));
            lua_pop(state, 1);
        }
        else if (op == "cubic")
        {
            allowed = { "op", "x1", "y1", "x2", "y2", "x", "y" };
            command.type = Type::Cubic;
            const auto read = [&](const char* field) {
                lua_getfield(state, -1, field);
                const float value = static_cast<float>(
                    luaL_checknumber(state, -1));
                lua_pop(state, 1);
                return value;
            };
            command.control1 = { read("x1"), read("y1") };
            command.control2 = { read("x2"), read("y2") };
            command.point = { read("x"), read("y") };
        }
        else if (op == "quadratic")
        {
            allowed = { "op", "x1", "y1", "x", "y" };
            command.type = Type::Quadratic;
            const auto read = [&](const char* field) {
                lua_getfield(state, -1, field);
                const float value = static_cast<float>(
                    luaL_checknumber(state, -1));
                lua_pop(state, 1);
                return value;
            };
            command.control1 = { read("x1"), read("y1") };
            command.point = { read("x"), read("y") };
        }
        else if (op == "close")
            command.type = Type::Close;
        else
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "draw.path: unsupported command '%s'", op.c_str());
        }
        if (!LuaTableUsesOnlyFields(state, -1,
                std::span<const std::string_view>(allowed),
                "draw.path command", error))
        {
            lua_pop(state, 1);
            return luaL_error(state, "draw.path: %s", error.c_str());
        }
        commands.push_back(command);
        lua_pop(state, 1);
    }
    if (!snowdesktop::widget_runtime::ValidateDrawPath(commands, error))
        return luaL_error(state, "draw.path: %s", error.c_str());

    std::optional<int> fillColor;
    std::optional<int> strokeColor;
    float thickness = 1.0f;
    float alpha = 1.0f;
    D2D1_FILL_MODE fillMode = D2D1_FILL_MODE_ALTERNATE;
    if (!lua_isnoneornil(state, 2))
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        if (!LuaTableUsesOnlyFields(state, 2,
                { "fillColor", "strokeColor", "thickness", "alpha",
                    "fillRule" }, "draw.path options", error))
            return luaL_error(state, "draw.path: %s", error.c_str());
        lua_getfield(state, 2, "fillColor");
        if (!lua_isnil(state, -1))
            fillColor = LuaDrawColor(
                state, -1, 0xFFFFFF, "draw.path");
        lua_pop(state, 1);
        lua_getfield(state, 2, "strokeColor");
        if (!lua_isnil(state, -1))
            strokeColor = LuaDrawColor(
                state, -1, 0xFFFFFF, "draw.path");
        lua_pop(state, 1);
        lua_getfield(state, 2, "thickness");
        if (!lua_isnil(state, -1))
            thickness = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, 2, "alpha");
        if (!lua_isnil(state, -1))
            alpha = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, 2, "fillRule");
        if (!lua_isnil(state, -1))
        {
            const char* value = luaL_checkstring(state, -1);
            const std::string rule = value ? value : "";
            if (rule == "winding") fillMode = D2D1_FILL_MODE_WINDING;
            else if (rule != "alternate")
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "draw.path: fillRule must be alternate or winding");
            }
        }
        lua_pop(state, 1);
    }
    if (!fillColor && !strokeColor) strokeColor = 0xFFFFFF;
    if (!std::isfinite(thickness) || thickness <= 0.0f ||
        thickness > 4096.0f || !std::isfinite(alpha) || alpha < 0.0f ||
        alpha > 1.0f)
        return luaL_error(state,
            "draw.path: thickness and alpha are out of range");

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->ctx) return 0;
    ComPtr<ID2D1Factory> factory;
    d2d->ctx->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) ||
        !geometry || FAILED(geometry->Open(&sink)) || !sink)
        return 0;
    sink->SetFillMode(fillMode);
    bool open = false;
    const auto point = [d2d](const auto& value) {
        return D2D1::Point2F(value.x + d2d->widgetRect.left,
            value.y + d2d->widgetRect.top);
    };
    for (const auto& command : commands)
    {
        if (command.type == Type::Move)
        {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(point(command.point), fillColor
                ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            open = true;
        }
        else if (command.type == Type::Line)
            sink->AddLine(point(command.point));
        else if (command.type == Type::Cubic)
            sink->AddBezier(D2D1::BezierSegment(point(command.control1),
                point(command.control2), point(command.point)));
        else if (command.type == Type::Quadratic)
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                point(command.control1), point(command.point)));
        else if (open)
        {
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return 0;
    if (fillColor)
    {
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(
                d2d, *fillColor, alpha))
            d2d->ctx->FillGeometry(geometry.Get(), brush);
    }
    if (strokeColor)
    {
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(
                d2d, *strokeColor, alpha))
            d2d->ctx->DrawGeometry(geometry.Get(), brush, thickness);
    }
    return 0;
}

static int lua_DrawGradientRect(lua_State* state)
{
    const float x = static_cast<float>(luaL_checknumber(state, 1));
    const float y = static_cast<float>(luaL_checknumber(state, 2));
    const float width = static_cast<float>(luaL_checknumber(state, 3));
    const float height = static_cast<float>(luaL_checknumber(state, 4));
    const int startColor = LuaDrawColor(
        state, 5, 0xFFFFFF, "draw.gradientRect");
    const int endColor = LuaDrawColor(
        state, 6, 0x000000, "draw.gradientRect");
    const char* directionRaw = luaL_optstring(state, 7, "vertical");
    const std::string direction = directionRaw ? directionRaw : "";
    const float radius = static_cast<float>(
        luaL_optnumber(state, 8, 0.0));
    const float alpha = LuaDrawAlpha(state, 9, "draw.gradientRect");
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        width <= 0.0f || height <= 0.0f || width > 100000.0f ||
        height > 100000.0f || std::abs(x) > 1000000.0f ||
        std::abs(y) > 1000000.0f ||
        std::abs(x + width) > 1000000.0f ||
        std::abs(y + height) > 1000000.0f ||
        !std::isfinite(radius) || radius < 0.0f ||
        radius > std::min(width, height) * 0.5f)
        return luaL_error(state,
            "draw.gradientRect: geometry must be positive and bounded");
    if (direction != "horizontal" && direction != "vertical" &&
        direction != "diagonalDown" && direction != "diagonalUp")
        return luaL_error(state,
            "draw.gradientRect: unsupported direction");
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->ctx) return 0;
    const D2D1_RECT_F rect = D2D1::RectF(
        d2d->widgetRect.left + x, d2d->widgetRect.top + y,
        d2d->widgetRect.left + x + width,
        d2d->widgetRect.top + y + height);
    D2D1_POINT_2F startPoint = D2D1::Point2F(rect.left, rect.top);
    D2D1_POINT_2F endPoint = D2D1::Point2F(rect.left, rect.bottom);
    if (direction == "horizontal")
        endPoint = D2D1::Point2F(rect.right, rect.top);
    else if (direction == "diagonalDown")
        endPoint = D2D1::Point2F(rect.right, rect.bottom);
    else if (direction == "diagonalUp")
    {
        startPoint = D2D1::Point2F(rect.left, rect.bottom);
        endPoint = D2D1::Point2F(rect.right, rect.top);
    }
    const auto color = [alpha](int value) {
        return D2D1::ColorF(((value >> 16) & 0xFF) / 255.0f,
            ((value >> 8) & 0xFF) / 255.0f,
            (value & 0xFF) / 255.0f, alpha);
    };
    const D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, color(startColor) }, { 1.0f, color(endColor) }
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (FAILED(d2d->ctx->CreateGradientStopCollection(
            stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
            &collection)) || !collection ||
        FAILED(d2d->ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(startPoint, endPoint),
            collection.Get(), &brush)) || !brush)
        return 0;
    if (radius > 0.0f)
        d2d->ctx->FillRoundedRectangle(
            D2D1::RoundedRect(rect, radius, radius), brush.Get());
    else
        d2d->ctx->FillRectangle(rect, brush.Get());
    return 0;
}

static int lua_DrawShadow(lua_State* state)
{
    snowdesktop::widget_runtime::DrawRect bounds{
        static_cast<float>(luaL_checknumber(state, 1)),
        static_cast<float>(luaL_checknumber(state, 2)),
        static_cast<float>(luaL_checknumber(state, 3)),
        static_cast<float>(luaL_checknumber(state, 4)),
    };
    const int color = LuaDrawColor(
        state, 5, 0x000000, "draw.shadow");
    const float blur = static_cast<float>(luaL_optnumber(state, 6, 12.0));
    const float radius = static_cast<float>(luaL_optnumber(state, 7, 0.0));
    const float offsetX = static_cast<float>(luaL_optnumber(state, 8, 0.0));
    const float offsetY = static_cast<float>(luaL_optnumber(state, 9, 4.0));
    const float alpha = LuaDrawAlpha(state, 10, "draw.shadow");
    std::vector<snowdesktop::widget_runtime::DrawShadowLayer> layers;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildDrawShadowLayers(bounds, blur,
            radius, offsetX, offsetY, alpha, layers, error))
        return luaL_error(state, "draw.shadow: %s", error.c_str());
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->ctx) return 0;
    for (const auto& layer : layers)
    {
        ID2D1SolidColorBrush* brush = GetCachedBrush(
            d2d, color, layer.alpha);
        if (!brush) continue;
        const D2D1_RECT_F rect = D2D1::RectF(
            d2d->widgetRect.left + layer.bounds.x,
            d2d->widgetRect.top + layer.bounds.y,
            d2d->widgetRect.left + layer.bounds.x + layer.bounds.width,
            d2d->widgetRect.top + layer.bounds.y + layer.bounds.height);
        if (layer.radius > 0.0f)
            d2d->ctx->FillRoundedRectangle(D2D1::RoundedRect(
                rect, layer.radius, layer.radius), brush);
        else
            d2d->ctx->FillRectangle(rect, brush);
    }
    return 0;
}

static int lua_DrawSparkline(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    std::string error;
    if (!LuaTableIsContiguousArray(
            state, 1, "draw.sparkline values", error))
        return luaL_error(state, "draw.sparkline: %s", error.c_str());
    const std::size_t count = lua_rawlen(state, 1);
    if (count == 0 || count > 512)
        return luaL_error(state,
            "draw.sparkline: values must contain 1 to 512 items");
    std::vector<float> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index + 1));
        values.push_back(static_cast<float>(luaL_checknumber(state, -1)));
        lua_pop(state, 1);
    }
    snowdesktop::widget_runtime::DrawRect bounds{
        static_cast<float>(luaL_checknumber(state, 2)),
        static_cast<float>(luaL_checknumber(state, 3)),
        static_cast<float>(luaL_checknumber(state, 4)),
        static_cast<float>(luaL_checknumber(state, 5)),
    };
    const int color = LuaDrawColor(
        state, 6, 0xFFFFFF, "draw.sparkline");
    const float thickness = static_cast<float>(
        luaL_optnumber(state, 7, 1.0));
    std::optional<float> minimum;
    std::optional<float> maximum;
    if (!lua_isnoneornil(state, 8))
        minimum = static_cast<float>(luaL_checknumber(state, 8));
    if (!lua_isnoneornil(state, 9))
        maximum = static_cast<float>(luaL_checknumber(state, 9));
    const float alpha = LuaDrawAlpha(state, 10, "draw.sparkline");
    if (!std::isfinite(thickness) || thickness <= 0.0f ||
        thickness > 4096.0f)
        return luaL_error(state,
            "draw.sparkline: thickness must be positive and bounded");
    std::vector<snowdesktop::widget_runtime::DrawPoint> points;
    if (!snowdesktop::widget_runtime::BuildDrawSparkline(values, bounds,
            minimum, maximum, points, error))
        return luaL_error(state, "draw.sparkline: %s", error.c_str());
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->ctx) return 0;
    ID2D1SolidColorBrush* brush = GetCachedBrush(d2d, color, alpha);
    if (!brush) return 0;
    const auto point = [d2d](const auto& value) {
        return D2D1::Point2F(value.x + d2d->widgetRect.left,
            value.y + d2d->widgetRect.top);
    };
    if (points.size() == 1)
        d2d->ctx->FillEllipse(D2D1::Ellipse(
            point(points.front()), thickness, thickness), brush);
    else
    {
        for (std::size_t index = 1; index < points.size(); ++index)
            d2d->ctx->DrawLine(point(points[index - 1]),
                point(points[index]), brush, thickness);
    }
    return 0;
}

static int lua_DrawImage(lua_State* L)
{
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float w = static_cast<float>(luaL_checknumber(L, 4));
    float h = static_cast<float>(luaL_checknumber(L, 5));
    float alpha = static_cast<float>(luaL_optnumber(L, 6, 1.0));
    auto* s = GetD2D(L);
    if (!s || !s->engine) return 0;
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_api_version");
    const bool allowLegacyPath = lua_tointeger(L, -1) < 2;
    lua_pop(L, 1);
    ID2D1Bitmap1* bmp = ResolveDrawImageBitmap(
        L, s, 1, "draw.image", allowLegacyPath);
    if (!s || !s->ctx || !bmp) return 0;
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + w, y + s->widgetRect.top + h);
    s->ctx->DrawBitmap(bmp, dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

static int lua_DrawImageFit(lua_State* state)
{
    const float x = static_cast<float>(luaL_checknumber(state, 2));
    const float y = static_cast<float>(luaL_checknumber(state, 3));
    const float width = static_cast<float>(luaL_checknumber(state, 4));
    const float height = static_cast<float>(luaL_checknumber(state, 5));
    const char* fitRaw = luaL_optstring(state, 6, "contain");
    const char* alignmentRaw = luaL_optstring(state, 7, "center");
    const float alpha = LuaDrawAlpha(state, 8, "draw.imageFit");
    const char* interpolationRaw = luaL_optstring(state, 9, "linear");
    using Fit = snowdesktop::widget_runtime::DrawImageFit;
    using Alignment = snowdesktop::widget_runtime::DrawImageAlignment;
    Fit fit = Fit::Contain;
    const std::string fitName = fitRaw ? fitRaw : "";
    if (fitName == "fill") fit = Fit::Fill;
    else if (fitName == "cover") fit = Fit::Cover;
    else if (fitName == "none") fit = Fit::None;
    else if (fitName != "contain")
        return luaL_error(state, "draw.imageFit: unsupported fit");
    Alignment alignment = Alignment::Center;
    const std::string alignmentName = alignmentRaw ? alignmentRaw : "";
    if (alignmentName == "start") alignment = Alignment::Start;
    else if (alignmentName == "end") alignment = Alignment::End;
    else if (alignmentName != "center")
        return luaL_error(state, "draw.imageFit: unsupported alignment");
    const std::string interpolation = interpolationRaw
        ? interpolationRaw : "";
    if (interpolation != "linear" && interpolation != "nearest")
        return luaL_error(state,
            "draw.imageFit: interpolation must be linear or nearest");
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine) return 0;
    ID2D1Bitmap1* bitmap = ResolveDrawImageBitmap(
        state, d2d, 1, "draw.imageFit", false);
    if (!bitmap || !d2d->ctx) return 0;
    const D2D1_SIZE_F sourceSize = bitmap->GetSize();
    const auto placement = snowdesktop::widget_runtime::
        ResolveDrawImagePlacement(sourceSize.width, sourceSize.height,
            { x, y, width, height }, fit, alignment);
    if (!placement.valid)
        return luaL_error(state,
            "draw.imageFit: dimensions must be positive and bounded");
    const D2D1_RECT_F destination = D2D1::RectF(
        d2d->widgetRect.left + placement.destination.x,
        d2d->widgetRect.top + placement.destination.y,
        d2d->widgetRect.left + placement.destination.x +
            placement.destination.width,
        d2d->widgetRect.top + placement.destination.y +
            placement.destination.height);
    const D2D1_RECT_F source = D2D1::RectF(
        placement.source.x, placement.source.y,
        placement.source.x + placement.source.width,
        placement.source.y + placement.source.height);
    d2d->ctx->DrawBitmap(bitmap, destination, alpha,
        interpolation == "nearest"
            ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            : D2D1_INTERPOLATION_MODE_LINEAR, source);
    return 0;
}

static bool ResourceCreationAllowed(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_loading");
    const bool loading = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return loading;
}

static int lua_ResourceExists(lua_State* L)
{
    size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    const auto resource = length <= 64
        ? CurrentPackageResource(L, std::string(name, length))
        : std::nullopt;
    const auto path = resource ? ResolveCurrentPackageAsset(
        L, Utf8ToWideLocal(resource->path)) : std::nullopt;
    std::error_code error;
    const bool exists = path &&
        std::filesystem::is_regular_file(*path, error);
    lua_pushboolean(L, exists ? 1 : 0);
    return 1;
}

static int lua_ResourceImage(lua_State* L)
{
    if (!ResourceCreationAllowed(L))
    {
        return luaL_error(L,
            "resource.image: handles must be created while the entry script is loading");
    }
    size_t length = 0;
    const char* nameRaw = luaL_checklstring(L, 1, &length);
    if (length == 0 || length > 64)
        return luaL_error(L, "resource.image: invalid resource name");
    const std::string name(nameRaw, length);
    auto* state = GetD2D(L);
    const auto resource = CurrentPackageResource(L, name, "image");
    const auto path = resource
        ? ResolveCurrentPackageAsset(L, Utf8ToWideLocal(resource->path))
        : std::nullopt;
    if (!path || (state && state->ctx && !LoadImageBitmap(state, *path)))
        return luaL_error(L, "resource.image: resource is missing or cannot be decoded");
    PushResourceHandle(L, LuaResourceType::Image, name);
    return 1;
}

static int lua_ResourceFont(lua_State* L)
{
    if (!ResourceCreationAllowed(L))
    {
        return luaL_error(L,
            "resource.font: handles must be created while the entry script is loading");
    }
    size_t length = 0;
    const char* nameRaw = luaL_checklstring(L, 1, &length);
    if (length == 0 || length > 64)
        return luaL_error(L, "resource.font: invalid resource name");
    const std::string name(nameRaw, length);
    auto* state = GetD2D(L);
    const auto resource = CurrentPackageResource(L, name, "font");
    const auto path = resource
        ? ResolveCurrentPackageAsset(L, Utf8ToWideLocal(resource->path))
        : std::nullopt;
    if (!path || (state && state->dwrite && !LoadPrivateFont(state, *path)))
        return luaL_error(L, "resource.font: resource is missing or invalid");
    PushResourceHandle(L, LuaResourceType::Font, name);
    return 1;
}

static int lua_ResourceStatus(lua_State* L)
{
    const auto* handle = TestResourceHandle(L, 1);
    if (!handle)
        return luaL_error(L, "resource.status: invalid resource handle");
    const char* type = handle->type == LuaResourceType::Image
        ? "image" : "font";
    auto* state = GetD2D(L);
    const bool runtimeImage = handle->type == LuaResourceType::Image &&
        snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(handle->name);
    const auto path = runtimeImage
        ? std::optional<std::wstring>{}
        : ResolveResourceHandlePath(L, 1, handle->type);
    bool ready = false;
    if (runtimeImage && state)
    {
        const auto resource = state->runtimeImages.find(handle->name);
        ready = resource != state->runtimeImages.end() &&
            resource->second.ownerWidgetId == BoundWidgetId(L);
    }
    else if (path && state)
    {
        ready = handle->type == LuaResourceType::Image
            ? state->imageCache.contains(*path)
            : state->privateFonts.contains(*path);
    }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, ready ? "ready" :
        ((path || runtimeImage) ? "pending" : "error"));
    lua_setfield(L, -2, "state");
    lua_pushstring(L, type);
    lua_setfield(L, -2, "type");
    lua_pushstring(L, handle->name);
    lua_setfield(L, -2, "name");
    return 1;
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
    auto* s = GetD2D(L);
    std::wstring path;
    if (BoundWidgetApiVersion(L) >= 2)
    {
        std::size_t length = 0;
        const char* raw = luaL_checklstring(L, 1, &length);
        const std::string reference(raw ? raw : "", length);
        if (reference.empty() || reference.size() > 128)
            return luaL_error(L,
                "draw.icon: reference must contain 1 to 128 bytes");
        if (s && s->engine)
        {
            const auto resolved = s->engine->RuntimeResolveItemReference(
                BoundWidgetId(L), BoundWidgetRuntimeToken(L), reference);
            if (resolved) path = *resolved;
        }
    }
    else
    {
        path = ReadLuaPathArg(L, 1);
    }
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float size = static_cast<float>(luaL_optnumber(L, 4, 32));
    float alpha = static_cast<float>(luaL_optnumber(L, 5, 1.0));
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

void WidgetEngine::InitializeWidgetDataBroker()
{
    using namespace std::chrono_literals;
    using snowdesktop::widget_runtime::DataProviderDescriptor;

    dataBroker_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetDataBroker>();
    desktopDataRevision_ = 0;
    desktopDataChangeReason_ = "initial";
    desktopDataTimestampMs_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    calendarEventsRevision_ = 0;
    calendarSelectionRevision_ = 0;
    appIndexRevision_ = 0;
    std::string error;
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.cpu", kSystemPerformancePermission,
        500ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.memory", kSystemPerformancePermission,
        1000ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.power", kSystemPowerPermission,
        2000ms, 10000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.network.status", kSystemNetworkPermission,
        2000ms, 10000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.network.traffic", kSystemNetworkPermission,
        1000ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.gpu", kSystemPerformancePermission,
        1000ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.storage.volumes", kSystemStoragePermission,
        2000ms, 10000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.storage.io", kSystemStoragePermission,
        1000ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.display.topology", kSystemDisplayPermission,
        2000ms, 10000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "system.display.current", kSystemDisplayPermission,
        2000ms, 10000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "audio.output.default", kAudioOutputReadPermission,
        1000ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "audio.output.volume", kAudioOutputReadPermission,
        1000ms, 5000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "audio.output.analysis", kAudioOutputAnalyzePermission,
        16ms, 1000ms, 0ms, true, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "media.sessions", kMediaReadPermission,
        500ms, 2000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "media.current", kMediaReadPermission,
        500ms, 2000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "media.timeline", kMediaReadPermission,
        500ms, 2000ms, 2000ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "media.artwork", kMediaReadPermission,
        500ms, 2000ms, 0ms, false, false }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "desktop.items", kDesktopReadPermission,
        100ms, 1000ms, 0ms, false, true }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "desktop.selection", kDesktopReadPermission,
        100ms, 1000ms, 0ms, false, true }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "desktop.changes", kDesktopReadPermission,
        100ms, 1000ms, 0ms, false, true }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "calendar.events", kCalendarReadPermission,
        100ms, 1000ms, 0ms, false, true }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "calendar.selectedDate", kCalendarReadPermission,
        100ms, 1000ms, 0ms, false, true }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "app.indexStatus", kAppDiscoveryPermission,
        100ms, 1000ms, 0ms, false, true }, error);
    error.clear();
    (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
        "filesystem.watch", kFilesystemWatchPermission,
        100ms, 1000ms, 0ms, true, false }, error);

    if (previewOnly_)
    {
        widgetSystemDataProvider_.reset();
        widgetAudioAnalysisProvider_.reset();
    }
    else
    {
        widgetSystemDataProvider_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetSystemDataProvider>();
        widgetAudioAnalysisProvider_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAudioAnalysisProvider>();
    }
}

void WidgetEngine::ApplyWidgetDataBrokerActions()
{
    using snowdesktop::widget_runtime::DataBrokerActionType;
    if (!dataBroker_) return;
    for (const auto& action : dataBroker_->DrainActions())
    {
        const bool audioAnalysis =
            action.topic == "audio.output.analysis";
        const bool hostEventData =
            action.topic == "desktop.items" ||
            action.topic == "desktop.selection" ||
            action.topic == "desktop.changes" ||
            action.topic == "calendar.events" ||
            action.topic == "calendar.selectedDate" ||
            action.topic == "app.indexStatus" ||
            action.topic == "filesystem.watch";
        switch (action.type)
        {
        case DataBrokerActionType::Start:
        {
            const bool started = hostEventData
                ? true
                : audioAnalysis
                ? widgetAudioAnalysisProvider_ &&
                    widgetAudioAnalysisProvider_->Start(
                        action.effectiveInterval)
                : widgetSystemDataProvider_ &&
                    widgetSystemDataProvider_->StartTopic(
                        action.topic, action.effectiveInterval);
            (void)dataBroker_->MarkStarted(action.topic, started,
                started ? std::string{} : "data provider failed to start");
            break;
        }
        case DataBrokerActionType::Reconfigure:
            if (hostEventData)
            {
                // Event-backed desktop topics have no polling resource to
                // reconfigure. The broker still owns their lifecycle.
            }
            else if (audioAnalysis)
            {
                if (widgetAudioAnalysisProvider_)
                    (void)widgetAudioAnalysisProvider_->Start(
                        action.effectiveInterval);
            }
            else if (widgetSystemDataProvider_)
            {
                (void)widgetSystemDataProvider_->StartTopic(
                    action.topic, action.effectiveInterval);
            }
            break;
        case DataBrokerActionType::Stop:
            if (hostEventData)
            {
                // No worker or OS handle is retained for these topics.
            }
            else if (audioAnalysis)
            {
                if (widgetAudioAnalysisProvider_)
                    widgetAudioAnalysisProvider_->Stop();
            }
            else if (widgetSystemDataProvider_)
            {
                (void)widgetSystemDataProvider_->StopTopic(action.topic);
                if (action.topic == "media.artwork" && d2dState_)
                {
                    ClearRuntimeImagesForSource(d2dState_, "media");
                }
            }
            break;
        }
    }
    ReconcileFilesystemWatches();
}

void WidgetEngine::ReconcileFilesystemWatches()
{
    if (!dataBroker_) return;
    for (auto& [subscriptionId, watch] : filesystemWatchBindings_)
    {
        if (watch.preview) continue;
        const auto subscription =
            dataBroker_->SubscriptionSnapshot(subscriptionId);
        const bool shouldRun = subscription && subscription->eligible;
        if (shouldRun == watch.desiredActive) continue;

        watch.desiredActive = shouldRun;
        watch.events.clear();
        watch.overflow = false;
        watch.available = false;
        watch.warmingUp = shouldRun;
        watch.error.clear();
        if (!shouldRun)
        {
            if (filesystemWatchService_)
                (void)filesystemWatchService_->Stop(subscriptionId);
            continue;
        }
        if (!filesystemWatchService_)
        {
            watch.warmingUp = false;
            watch.error = "providerUnavailable";
            continue;
        }
        auto started = filesystemWatchService_->Start(subscriptionId,
            watch.owner.instanceId, watch.directory);
        if (!started)
        {
            watch.warmingUp = false;
            watch.error = started.error.empty()
                ? "providerUnavailable" : started.error;
        }
    }
}

void WidgetEngine::DrainFilesystemWatchCompletions()
{
    if (!filesystemWatchService_) return;
    for (auto& completion :
        filesystemWatchService_->DrainCompletions())
    {
        auto binding = filesystemWatchBindings_.find(completion.id);
        if (binding == filesystemWatchBindings_.end()) continue;
        auto& watch = binding->second;
        const auto subscription = dataBroker_
            ? dataBroker_->SubscriptionSnapshot(completion.id)
            : std::nullopt;
        const bool eligible = subscription && subscription->eligible &&
            watch.desiredActive;

        if (completion.kind == snowdesktop::widget_runtime::
                WidgetFilesystemWatchCompletionKind::Started)
        {
            if (!eligible) continue;
            watch.available = true;
            watch.warmingUp = false;
            watch.error.clear();
            watch.timestampMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            continue;
        }
        if (completion.kind == snowdesktop::widget_runtime::
                WidgetFilesystemWatchCompletionKind::Stopped)
        {
            if (!watch.desiredActive)
            {
                watch.available = false;
                watch.warmingUp = false;
                watch.events.clear();
                watch.overflow = false;
            }
            continue;
        }
        if (completion.kind == snowdesktop::widget_runtime::
                WidgetFilesystemWatchCompletionKind::Error)
        {
            if (!eligible) continue;
            watch.available = false;
            watch.warmingUp = false;
            watch.events.clear();
            watch.overflow = false;
            watch.error = completion.error.empty()
                ? "watchFailed" : completion.error;
            continue;
        }
        if (!eligible) continue;

        watch.events.clear();
        watch.overflow = completion.overflow;
        for (const auto& event : completion.events)
        {
            LuaWidgetDataSnapshot::FilesystemWatchEvent publicEvent;
            publicEvent.kind = event.kind;
            publicEvent.name = WidgetWideToUtf8(event.name);
            publicEvent.oldName = WidgetWideToUtf8(event.oldName);
            if (publicEvent.name.empty()) continue;

            if (event.kind != "removed")
            {
                const std::filesystem::path child =
                    watch.directory / event.name;
                const DWORD attributes = GetFileAttributesW(child.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                {
                    const auto kind =
                        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                        ? snowdesktop::widget_runtime::
                            WidgetFilesystemHandleKind::Folder
                        : snowdesktop::widget_runtime::
                            WidgetFilesystemHandleKind::File;
                    publicEvent.itemKind = std::string(
                        snowdesktop::widget_runtime::
                            WidgetFilesystemHandleStore::KindName(kind));
                    if (filesystemHandleStore_)
                    {
                        auto grant = filesystemHandleStore_->Grant(
                            watch.owner, child, kind, watch.access);
                        if (grant)
                            publicEvent.handle = grant.entry->handle;
                    }
                }
            }
            watch.events.push_back(std::move(publicEvent));
        }
        if (watch.events.empty() && !watch.overflow) continue;
        watch.available = true;
        watch.warmingUp = false;
        watch.error.clear();
        ++watch.revision;
        watch.timestampMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        const int widgetIndex = FindWidget(
            Utf8ToWideLocal(watch.owner.instanceId));
        if (widgetIndex < 0 || !widgets_[widgetIndex].valid) continue;
        LuaWidget& widget = widgets_[widgetIndex];
        const std::uint64_t revision = watch.revision;
        const bool overflow = watch.overflow;
        (void)InvokeLifecycleEvent(widget, "data.change",
            [subscriptionId = completion.id, revision, overflow]
            (lua_State* eventState) {
                lua_pushliteral(eventState, "filesystem.watch");
                lua_setfield(eventState, -2, "topic");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(subscriptionId));
                lua_setfield(eventState, -2, "subscriptionId");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(revision));
                lua_setfield(eventState, -2, "revision");
                lua_pushboolean(eventState, overflow);
                lua_setfield(eventState, -2, "overflow");
            });
        RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::ReleaseWidgetDataSubscriptions(LuaWidget& widget)
{
    if (!dataBroker_)
    {
        for (const auto& [subscriptionId, _] : widget.dataSubscriptions)
        {
            if (filesystemWatchService_)
                (void)filesystemWatchService_->Stop(subscriptionId);
            filesystemWatchBindings_.erase(subscriptionId);
        }
        widget.dataSubscriptions.clear();
        return;
    }
    const auto now = snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    for (const auto& [subscriptionId, _] : widget.dataSubscriptions)
    {
        if (filesystemWatchService_)
            (void)filesystemWatchService_->Stop(subscriptionId);
        filesystemWatchBindings_.erase(subscriptionId);
        (void)dataBroker_->Unsubscribe(subscriptionId, now);
    }
    widget.dataSubscriptions.clear();
    ApplyWidgetDataBrokerActions();
}

void WidgetEngine::InitializeWidgetTaskBroker()
{
    using snowdesktop::widget_runtime::TaskDescriptor;
    taskBroker_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetTaskBroker>();
    std::string error;
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.toggle", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.next", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.previous", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.play", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.pause", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.stop", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.seek", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.setRate", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.setShuffle", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "media.setRepeat", kMediaActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "audio.output.setVolume", kAudioOutputControlPermission,
        true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "audio.output.setMute", kAudioOutputControlPermission,
        true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "app.search", kAppDiscoveryPermission, false, 2 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "app.launch", kAppLaunchPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "notification.show", kNotificationPostPermission, false, 2 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "calendar.create", kCalendarWritePermission, false, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "calendar.update", kCalendarWritePermission, false, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "calendar.remove", kCalendarWritePermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "network.request", kNetworkInternetPermission, false, 2 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "shell.openUri", kShellLaunchPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "system.openSettings", kShellLaunchPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "clipboard.read", kClipboardReadPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "clipboard.write", kClipboardWritePermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "clipboard.clear", kClipboardWritePermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.pickOpen", kFilesystemReadPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.pickSave", kFilesystemWritePermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.pickFolder", "", true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.stat", kFilesystemReadPermission, false, 4 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.list", kFilesystemReadPermission, false, 2 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.read", kFilesystemReadPermission, false, 2 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.write", kFilesystemWritePermission, false, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "filesystem.release", "", false, 4 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "desktop.search", kDesktopReadPermission, false, 2 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "everything.search", kEverythingSearchPermission, false, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "shell.openItem", kDesktopActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "shell.revealItem", kDesktopActionPermission, true, 1 }, error);
    error.clear();
    (void)taskBroker_->RegisterTask(TaskDescriptor{
        "desktop.refresh", kDesktopActionPermission, true, 1 }, error);
    if (previewOnly_)
    {
        mediaTaskExecutor_.reset();
        audioOutputTaskExecutor_.reset();
        clipboardTaskExecutor_.reset();
        filesystemTaskExecutor_.reset();
        appTaskExecutor_.reset();
        desktopTaskExecutor_.reset();
        externalItemTaskExecutor_.reset();
    }
    else
    {
        mediaTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetMediaTaskExecutor>();
        audioOutputTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor>();
        clipboardTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetClipboardTaskExecutor>();
        filesystemTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor>();
        appTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAppTaskExecutor>();
        desktopTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAppTaskExecutor>();
        externalItemTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetExternalSearchTaskExecutor>();
    }
}

void WidgetEngine::ApplyWidgetTaskBrokerActions()
{
    using snowdesktop::widget_runtime::TaskBrokerActionType;
    if (!taskBroker_) return;
    if (mediaTaskExecutor_)
    {
        for (auto& completion : mediaTaskExecutor_->DrainCompletions())
        {
            (void)taskBroker_->Complete(completion.id,
                completion.accepted, std::move(completion.error));
        }
    }
    if (audioOutputTaskExecutor_)
    {
        for (auto& completion :
            audioOutputTaskExecutor_->DrainCompletions())
        {
            (void)taskBroker_->Complete(completion.id,
                completion.accepted, std::move(completion.error));
        }
    }
    if (clipboardTaskExecutor_)
    {
        for (auto& completion :
            clipboardTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok && completion.action == "clipboard.read")
                clipboardTaskCompletions_.insert_or_assign(
                    id, std::move(completion));
            else
                clipboardTaskCompletions_.erase(id);
            (void)taskBroker_->Complete(id, ok, std::move(error));
        }
    }
    if (filesystemTaskExecutor_)
    {
        for (auto& completion :
            filesystemTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            bool ok = completion.ok;
            std::string error = completion.error;
            const auto snapshot = taskBroker_->Snapshot(id);
            if (ok && (!snapshot || snapshot->cancelRequested))
            {
                ok = false;
                error = "canceled";
            }
            auto owner = snapshot
                ? std::find_if(widgets_.begin(), widgets_.end(),
                    [&snapshot](const LuaWidget& candidate) {
                        return candidate.runtimeToken ==
                            snapshot->ownerToken;
                    })
                : widgets_.end();
            if (ok && (owner == widgets_.end() ||
                    !filesystemHandleStore_))
            {
                ok = false;
                error = "instanceDisposed";
            }
            std::optional<snowdesktop::widget_runtime::
                WidgetFilesystemHandleEntry> sourceEntry;
            std::string sourceHandleValue;
            if (ok)
            {
                sourceHandleValue = completion.metadata.handle;
                sourceEntry = filesystemHandleStore_->Resolve(
                    { snapshot->instanceId, owner->packageId },
                    sourceHandleValue);
                if (!sourceEntry)
                {
                    ok = false;
                    error = "invalidReference";
                }
            }
            if (ok && completion.action == "filesystem.list")
            {
                std::vector<std::string> createdHandles;
                for (auto& item : completion.items)
                {
                    auto grant = filesystemHandleStore_->Grant(
                        { snapshot->instanceId, owner->packageId },
                        item.path, item.kind, sourceEntry->access);
                    if (!grant)
                    {
                        for (const auto& handle : createdHandles)
                        {
                            std::string revokeError;
                            (void)filesystemHandleStore_->Revoke(
                                { snapshot->instanceId, owner->packageId },
                                handle, revokeError);
                        }
                        ok = false;
                        error = grant.error.empty()
                            ? "handleGrantFailed" : grant.error;
                        break;
                    }
                    item.handle = grant.entry->handle;
                    if (grant.created)
                        createdHandles.push_back(item.handle);
                }
            }
            completion.ok = ok;
            completion.error = error;
            if (ok)
                filesystemTaskCompletions_.insert_or_assign(
                    id, std::move(completion));
            else
                filesystemTaskCompletions_.erase(id);
            filesystemTaskHandles_.erase(id);
            (void)taskBroker_->Complete(id, ok, std::move(error));
        }
    }
    if (appTaskExecutor_)
    {
        for (auto& completion : appTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok)
                appSearchCompletions_.insert_or_assign(
                    id, std::move(completion));
            else
                appSearchCompletions_.erase(id);
            (void)taskBroker_->Complete(
                id, ok, std::move(error));
        }
    }
    if (desktopTaskExecutor_)
    {
        for (auto& completion : desktopTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok)
                itemSearchCompletions_.insert_or_assign(
                    id, std::move(completion));
            else
                itemSearchCompletions_.erase(id);
            (void)taskBroker_->Complete(id, ok, std::move(error));
        }
    }
    if (externalItemTaskExecutor_)
    {
        for (auto& completion :
            externalItemTaskExecutor_->DrainCompletions())
        {
            const std::uint64_t id = completion.id;
            const bool ok = completion.ok;
            std::string error = completion.error;
            if (ok)
                itemSearchCompletions_.insert_or_assign(
                    id, std::move(completion));
            else
                itemSearchCompletions_.erase(id);
            (void)taskBroker_->Complete(id, ok, std::move(error));
        }
    }
    for (const auto& action : taskBroker_->DrainActions())
    {
        if (action.type == TaskBrokerActionType::Cancel)
        {
            if (mediaTaskExecutor_)
                (void)mediaTaskExecutor_->Cancel(action.id);
            if (audioOutputTaskExecutor_)
                (void)audioOutputTaskExecutor_->Cancel(action.id);
            if (clipboardTaskExecutor_)
                (void)clipboardTaskExecutor_->Cancel(action.id);
            if (filesystemTaskExecutor_)
                (void)filesystemTaskExecutor_->Cancel(action.id);
            if (appTaskExecutor_)
                (void)appTaskExecutor_->Cancel(action.id);
            if (desktopTaskExecutor_)
                (void)desktopTaskExecutor_->Cancel(action.id);
            if (externalItemTaskExecutor_)
                (void)externalItemTaskExecutor_->Cancel(action.id);
            appSearchCompletions_.erase(action.id);
            itemSearchCompletions_.erase(action.id);
            calendarMutationCompletions_.erase(action.id);
            clipboardTaskCompletions_.erase(action.id);
            filesystemPickerCompletions_.erase(action.id);
            filesystemTaskCompletions_.erase(action.id);
            if (const auto request = networkTaskRequests_.find(action.id);
                request != networkTaskRequests_.end())
            {
                if (httpService_)
                    (void)httpService_->Cancel(
                        action.instanceId.empty()
                            ? std::wstring{}
                            : Utf8ToWideLocal(action.instanceId),
                        request->second);
            }
            networkTaskCompletions_.erase(action.id);
            continue;
        }
        const auto snapshot = taskBroker_->Snapshot(action.id);
        if (!snapshot) continue;
        if (snapshot->cancelRequested)
        {
            (void)taskBroker_->Complete(action.id, false);
            continue;
        }

        auto owner = std::find_if(widgets_.begin(), widgets_.end(),
            [&action](const LuaWidget& candidate) {
                return candidate.runtimeToken == action.ownerToken;
            });
        if (owner == widgets_.end())
        {
            (void)taskBroker_->Complete(
                action.id, false, "instanceDisposed");
            continue;
        }

        if (action.name == "app.search")
        {
            const auto query = action.arguments.find("query");
            const auto limitValue = action.arguments.find("limit");
            const auto offsetValue = action.arguments.find("offset");
            std::size_t limit = 0;
            std::size_t offset = 0;
            const auto parseNumber = [](const std::string& text,
                std::size_t& value) {
                const char* begin = text.data();
                const char* end = begin + text.size();
                const auto parsed = std::from_chars(begin, end, value);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (query == action.arguments.end() ||
                limitValue == action.arguments.end() ||
                offsetValue == action.arguments.end() ||
                !parseNumber(limitValue->second, limit) ||
                !parseNumber(offsetValue->second, offset))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }

            if (action.preview)
            {
                snowdesktop::widget_runtime::WidgetAppSearchCompletion
                    completion;
                completion.id = action.id;
                completion.catalogRevision = 1;
                completion.ok = true;
                if (offset == 0 && limit > 0)
                {
                    completion.items.push_back({ "preview-app",
                        _L("app.widget_preview.api.application"),
                        "preview-app", "Applications", "application" });
                }
                completion.nextOffset =
                    offset + completion.items.size();
                appSearchCompletions_.insert_or_assign(
                    action.id, std::move(completion));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }

            if (!applicationCatalogProvider_ || !appTaskExecutor_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }
            LuaApplicationCatalogSnapshot catalog;
            try
            {
                catalog = applicationCatalogProvider_();
            }
            catch (...)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerFailed");
                continue;
            }
            if (catalog.state != "ready")
            {
                (void)taskBroker_->Complete(action.id, false,
                    catalog.state == "indexing"
                        ? "appIndexNotReady" : "providerUnavailable");
                continue;
            }
            const std::wstring queryWide =
                Utf8ToWideLocal(query->second);
            const std::string foldedQuery = WidgetWideToUtf8(
                ToUpperInvariant(queryWide));
            const std::string pinyinQuery =
                BuildNamePinyinFullKey(queryWide);
            if (foldedQuery.empty() ||
                !appTaskExecutor_->StartSearch(
                    action.id, foldedQuery, pinyinQuery,
                    offset, limit, appIndexRevision_,
                    std::move(catalog.entries)))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
            }
            continue;
        }

        if (action.name == "desktop.search" ||
            action.name == "everything.search")
        {
            const auto query = action.arguments.find("query");
            const auto limitValue = action.arguments.find("limit");
            const auto offsetValue = action.arguments.find("offset");
            std::size_t limit = 0;
            std::size_t offset = 0;
            const auto parseNumber = [](const std::string& text,
                std::size_t& value) {
                const char* begin = text.data();
                const char* end = begin + text.size();
                const auto parsed = std::from_chars(begin, end, value);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (query == action.arguments.end() ||
                limitValue == action.arguments.end() ||
                offsetValue == action.arguments.end() ||
                !parseNumber(limitValue->second, limit) ||
                !parseNumber(offsetValue->second, offset))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }

            if (action.preview)
            {
                snowdesktop::widget_runtime::WidgetAppSearchCompletion
                    completion;
                completion.id = action.id;
                completion.catalogRevision = 1;
                completion.ok = true;
                if (offset == 0 && limit > 0)
                {
                    const bool desktop = action.name == "desktop.search";
                    completion.items.push_back({
                        desktop ? "preview-desktop" : "preview-everything",
                        desktop
                            ? _L("app.widget_preview.api.desktop_document")
                            : _L("app.widget_preview.api.search_result"),
                        desktop
                            ? "C:\\Users\\Maya\\Desktop\\Preview.txt"
                            : "C:\\Users\\Maya\\Documents\\Preview.txt",
                        desktop ? "Desktop" : "Everything",
                        "file" });
                }
                completion.nextOffset =
                    offset + completion.items.size();
                itemSearchCompletions_.insert_or_assign(
                    action.id, std::move(completion));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }

            if (action.name == "desktop.search")
            {
                if (!desktopTaskExecutor_ || !desktopSnapshotProvider_)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerUnavailable");
                    continue;
                }
                std::vector<LuaDesktopItemInfo> snapshot;
                try
                {
                    snapshot = RuntimeDesktopItems();
                }
                catch (...)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerFailed");
                    continue;
                }
                if (snapshot.size() > 2048) snapshot.resize(2048);
                std::vector<snowdesktop::widget_runtime::
                    WidgetAppCatalogEntry> catalog;
                catalog.reserve(snapshot.size());
                for (const auto& item : snapshot)
                {
                    if (item.title.empty() || item.path.empty()) continue;
                    snowdesktop::widget_runtime::WidgetAppCatalogEntry entry;
                    entry.id = item.id.empty() ? item.path : item.id;
                    entry.title = item.title;
                    entry.launchTarget = item.path;
                    const std::wstring titleWide =
                        Utf8ToWideLocal(item.title);
                    entry.foldedTitle = WidgetWideToUtf8(
                        ToUpperInvariant(titleWide));
                    entry.pinyinFull = BuildNamePinyinFullKey(titleWide);
                    entry.pinyinInitials =
                        BuildNamePinyinInitialKey(titleWide);
                    entry.source = item.source;
                    entry.type = item.type;
                    if (!entry.id.empty() && !entry.foldedTitle.empty())
                        catalog.push_back(std::move(entry));
                }
                const std::wstring queryWide =
                    Utf8ToWideLocal(query->second);
                const std::string foldedQuery = WidgetWideToUtf8(
                    ToUpperInvariant(queryWide));
                const std::string pinyinQuery =
                    BuildNamePinyinFullKey(queryWide);
                if (foldedQuery.empty() ||
                    !desktopTaskExecutor_->StartSearch(
                        action.id, foldedQuery, pinyinQuery,
                        offset, limit, desktopDataRevision_,
                        std::move(catalog)))
                {
                    (void)taskBroker_->Complete(action.id, false,
                        "taskExecutorUnavailable");
                }
                continue;
            }

            if (!externalItemTaskExecutor_ || !everythingSearchProvider_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }
            const auto provider = [this](const std::string& searchQuery,
                std::size_t maximumResults) {
                std::vector<snowdesktop::widget_runtime::
                    WidgetAppSearchResult> result;
                const std::vector<LuaDesktopItemInfo> items =
                    RuntimeEverythingSearch(searchQuery,
                        static_cast<int>(maximumResults));
                result.reserve(items.size());
                for (const auto& item : items)
                {
                    if (item.path.empty() || item.title.empty()) continue;
                    result.push_back({
                        item.id.empty() ? item.path : item.id,
                        item.title, item.path, item.source, item.type });
                }
                return result;
            };
            if (!externalItemTaskExecutor_->StartSearch(
                    action.id, query->second, offset, limit, provider))
            {
                (void)taskBroker_->Complete(action.id, false,
                    "taskExecutorUnavailable");
            }
            continue;
        }

        if (action.name == "network.request")
        {
            if (action.preview)
            {
                HttpResponse response;
                response.status = 200;
                response.body =
                    "<?xml version=\"1.0\"?><rss><channel>"
                    "<title>Preview feed</title></channel></rss>";
                networkTaskCompletions_.insert_or_assign(
                    action.id, std::move(response));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            const auto url = action.arguments.find("url");
            const auto timeout = action.arguments.find("timeoutMs");
            const auto cache = action.arguments.find("cacheSeconds");
            const auto maximum = action.arguments.find("maxBytes");
            const auto parseInteger = [](const std::string& text,
                int& output) {
                const char* begin = text.data();
                const char* end = begin + text.size();
                const auto parsed = std::from_chars(begin, end, output);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            int timeoutMs = 0;
            int cacheSeconds = 0;
            int maximumBytes = 0;
            if (!httpService_ || url == action.arguments.end() ||
                timeout == action.arguments.end() ||
                cache == action.arguments.end() ||
                maximum == action.arguments.end() ||
                !parseInteger(timeout->second, timeoutMs) ||
                !parseInteger(cache->second, cacheSeconds) ||
                !parseInteger(maximum->second, maximumBytes))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            HttpRequestOptions options;
            options.widgetId = owner->widgetId;
            options.url = Utf8ToWideLocal(url->second);
            options.method = L"GET";
            options.timeoutMs = timeoutMs;
            options.cacheSeconds = cacheSeconds;
            options.maximumResponseBytes =
                static_cast<std::uint32_t>(maximumBytes);
            options.allowedDomains = owner->manifest.networkDomains;
            options.allowAnyHttpOrHttpsUrl = false;
            options.allowAnyPublicHttpsUrl =
                owner->manifest.networkDomains.empty();
            const int requestId = httpService_->Submit(std::move(options));
            if (requestId <= 0)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "requestRejected");
                continue;
            }
            networkTaskRequests_.insert_or_assign(action.id, requestId);
            networkRequestTasks_.insert_or_assign(requestId, action.id);
            continue;
        }

        if (action.name == "calendar.create" ||
            action.name == "calendar.update" ||
            action.name == "calendar.remove")
        {
            snowdesktop::calendar::MutationResult result;
            if (action.preview)
            {
                result.error = "previewReadOnly";
            }
            else if (action.name == "calendar.remove")
            {
                const auto id = action.arguments.find("id");
                result = id != action.arguments.end()
                    ? RuntimeCalendarRemove(id->second)
                    : snowdesktop::calendar::MutationResult{
                        false, {}, 0, "invalidArguments" };
            }
            else
            {
                const auto title = action.arguments.find("title");
                const auto date = action.arguments.find("date");
                const auto notes = action.arguments.find("notes");
                const auto allDay = action.arguments.find("allDay");
                const auto start = action.arguments.find("startMinutes");
                const auto end = action.arguments.find("endMinutes");
                const auto reminder =
                    action.arguments.find("reminderMinutes");
                const auto parseInteger = [](const std::string& text,
                    int& value) {
                    const char* begin = text.data();
                    const char* finish = begin + text.size();
                    const auto parsed = std::from_chars(
                        begin, finish, value);
                    return parsed.ec == std::errc{} &&
                        parsed.ptr == finish;
                };
                snowdesktop::calendar::CalendarEvent event;
                bool valid = title != action.arguments.end() &&
                    date != action.arguments.end() &&
                    notes != action.arguments.end() &&
                    allDay != action.arguments.end() &&
                    start != action.arguments.end() &&
                    end != action.arguments.end() &&
                    reminder != action.arguments.end();
                if (valid)
                {
                    event.title = title->second;
                    event.date = date->second;
                    event.notes = notes->second;
                    event.allDay = allDay->second == "1";
                    valid = parseInteger(
                        start->second, event.startMinutes) &&
                        parseInteger(end->second, event.endMinutes) &&
                        parseInteger(reminder->second,
                            event.reminderMinutes);
                }
                if (!valid)
                {
                    result.error = "invalidArguments";
                }
                else if (action.name == "calendar.create")
                {
                    result = RuntimeCalendarCreate(std::move(event));
                }
                else
                {
                    const auto id = action.arguments.find("id");
                    const auto revision =
                        action.arguments.find("expectedRevision");
                    int expectedRevision = 0;
                    if (id == action.arguments.end() ||
                        revision == action.arguments.end() ||
                        !parseInteger(revision->second, expectedRevision))
                    {
                        result.error = "invalidArguments";
                    }
                    else
                    {
                        result = RuntimeCalendarUpdate(id->second,
                            expectedRevision, std::move(event));
                    }
                }
            }
            calendarMutationCompletions_.insert_or_assign(
                action.id, result);
            (void)taskBroker_->Complete(
                action.id, result.ok, result.error);
            continue;
        }

        if (action.name == "notification.show")
        {
            const auto title = action.arguments.find("title");
            const auto message = action.arguments.find("message");
            if (title == action.arguments.end() ||
                message == action.arguments.end() ||
                action.arguments.size() != 2)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            const std::string notificationError = RuntimePostNotification(
                owner->widgetId, Utf8ToWideLocal(title->second),
                Utf8ToWideLocal(message->second));
            (void)taskBroker_->Complete(action.id,
                notificationError.empty(), notificationError);
            continue;
        }

        if (action.name == "app.launch")
        {
            const auto refValue = action.arguments.find("ref");
            if (refValue == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            const auto reference = owner->applicationReferences.find(
                refValue->second);
            if (reference == owner->applicationReferences.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidReference");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!reference->second.persistent &&
                reference->second.catalogRevision != appIndexRevision_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "staleReference");
                continue;
            }
            const std::wstring launchTarget = Utf8ToWideLocal(
                reference->second.launchTarget);
            const bool accepted = !launchTarget.empty() &&
                applicationLaunchCallback_ &&
                applicationLaunchCallback_(launchTarget);
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} : "launchRejected");
            continue;
        }

        if (action.name == "shell.openItem" ||
            action.name == "shell.revealItem")
        {
            const auto refValue = action.arguments.find("ref");
            if (refValue == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            const auto reference = owner->itemReferences.find(
                refValue->second);
            if (reference == owner->itemReferences.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidReference");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (reference->second.sourceTask == "desktop.search" &&
                reference->second.revision != desktopDataRevision_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "staleReference");
                continue;
            }
            const std::wstring target = Utf8ToWideLocal(
                reference->second.target);
            const bool reveal = action.name == "shell.revealItem";
            const bool accepted = !target.empty() &&
                (reveal ? RuntimeRevealDesktopPath(target)
                        : RuntimeOpenDesktopPath(target));
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} :
                    (reveal ? "revealRejected" : "openRejected"));
            continue;
        }

        if (action.name == "desktop.refresh")
        {
            if (!action.preview) RuntimeRefreshDesktop();
            (void)taskBroker_->Complete(action.id, true);
            continue;
        }

        if (action.name == "system.openSettings")
        {
            const auto page = action.arguments.find("page");
            const auto uri = page == action.arguments.end()
                ? std::nullopt
                : snowdesktop::widget_runtime::SystemSettingsUri(
                    page->second);
            if (!uri)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            const bool accepted = RuntimeOpenDesktopPath(
                std::wstring(*uri));
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} : "openRejected");
            continue;
        }

        if (action.name == "shell.openUri")
        {
            const auto url = action.arguments.find("url");
            if (url == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.preview)
            {
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            const std::wstring target = Utf8ToWideLocal(url->second);
            const bool valid = snowdesktop::http_security::
                IsAllowedPublicHttpsUrl(target);
            const bool accepted = valid && RuntimeOpenDesktopPath(target);
            (void)taskBroker_->Complete(action.id, accepted,
                accepted ? std::string{} :
                    (valid ? "openRejected" : "invalidUrl"));
            continue;
        }

        if (snowdesktop::widget_runtime::WidgetClipboardTaskExecutor::
                SupportsAction(action.name))
        {
            snowdesktop::widget_runtime::WidgetClipboardTaskRequest
                clipboardRequest;
            clipboardRequest.action = action.name;
            if (const auto format = action.arguments.find("format");
                format != action.arguments.end())
                clipboardRequest.format = format->second;
            if (const auto text = action.arguments.find("text");
                text != action.arguments.end())
                clipboardRequest.text = text->second;
            if (!snowdesktop::widget_runtime::WidgetClipboardTaskExecutor::
                    ValidateRequest(clipboardRequest))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.preview)
            {
                if (action.name == "clipboard.read")
                {
                    snowdesktop::widget_runtime::
                        WidgetClipboardTaskCompletion completion;
                    completion.id = action.id;
                    completion.action = action.name;
                    completion.ok = true;
                    completion.format = clipboardRequest.format;
                    if (completion.format == "text")
                    {
                        completion.text = "Preview clipboard";
                    }
                    else if (completion.format == "image")
                    {
                        auto pixels = std::make_shared<snowdesktop::
                            widget_runtime::WidgetRuntimeImagePixels>();
                        pixels->width = 64;
                        pixels->height = 64;
                        pixels->stride = pixels->width * 4;
                        pixels->bgraPremultiplied.resize(
                            pixels->stride * pixels->height);
                        for (std::uint32_t y = 0; y < pixels->height; ++y)
                        {
                            for (std::uint32_t x = 0; x < pixels->width; ++x)
                            {
                                const std::size_t offset =
                                    static_cast<std::size_t>(
                                        y * pixels->stride + x * 4);
                                pixels->bgraPremultiplied[offset] =
                                    static_cast<std::uint8_t>(48 + x * 3);
                                pixels->bgraPremultiplied[offset + 1] =
                                    static_cast<std::uint8_t>(80 + y * 2);
                                pixels->bgraPremultiplied[offset + 2] = 210;
                                pixels->bgraPremultiplied[offset + 3] = 255;
                            }
                        }
                        completion.resourceToken = snowdesktop::widget_runtime::
                            MakeWidgetRuntimeImageToken(
                                "clipboard", *pixels);
                        completion.image = std::move(pixels);
                    }
                    else
                    {
                        completion.files.push_back({
                            std::filesystem::path(
                                L"C:\\Users\\Maya\\Documents\\Preview.txt"),
                            false });
                        completion.files.push_back({
                            std::filesystem::path(
                                L"C:\\Users\\Maya\\Pictures"), true });
                    }
                    clipboardTaskCompletions_.insert_or_assign(
                        action.id, std::move(completion));
                }
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!clipboardTaskExecutor_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
                continue;
            }
            auto start = clipboardTaskExecutor_->Start(
                action.id, action.instanceId,
                std::move(clipboardRequest));
            if (!start)
            {
                (void)taskBroker_->Complete(action.id, false,
                    start.error.empty()
                        ? "taskExecutorUnavailable" : start.error);
            }
            continue;
        }

        if (IsFilesystemPickerTask(action.name))
        {
            LuaWidgetFilePickerRequest request;
            snowdesktop::widget_runtime::WidgetFilesystemHandleKind kind =
                snowdesktop::widget_runtime::WidgetFilesystemHandleKind::File;
            if (action.name == "filesystem.pickOpen")
            {
                request.kind = LuaWidgetFilePickerKind::OpenFile;
                request.access = snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::Read;
            }
            else if (action.name == "filesystem.pickSave")
            {
                request.kind = LuaWidgetFilePickerKind::SaveFile;
                request.access = snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::Write;
            }
            else
            {
                request.kind = LuaWidgetFilePickerKind::Folder;
                kind = snowdesktop::widget_runtime::
                    WidgetFilesystemHandleKind::Folder;
                const auto access = action.arguments.find("access");
                if (access == action.arguments.end())
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidArguments");
                    continue;
                }
                if (access->second == "read")
                    request.access = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Read;
                else if (access->second == "write")
                    request.access = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Write;
                else if (access->second == "readWrite")
                    request.access = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::ReadWrite;
                else
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidArguments");
                    continue;
                }
            }
            if (const auto extensions = action.arguments.find("extensions");
                extensions != action.arguments.end())
            {
                std::size_t begin = 0;
                while (begin <= extensions->second.size())
                {
                    const std::size_t end = extensions->second.find(
                        ';', begin);
                    const std::string value = extensions->second.substr(
                        begin, end == std::string::npos
                            ? std::string::npos : end - begin);
                    if (!value.empty())
                        request.extensions.push_back(Utf8ToWideLocal(value));
                    if (end == std::string::npos) break;
                    begin = end + 1;
                }
            }
            if (const auto suggestedName =
                    action.arguments.find("suggestedName");
                suggestedName != action.arguments.end())
                request.suggestedName =
                    Utf8ToWideLocal(suggestedName->second);

            if (action.preview)
            {
                snowdesktop::widget_runtime::WidgetFilesystemHandleEntry
                    entry;
                entry.handle = "filesystem:00000000000000000000000000000000";
                entry.owner = { action.instanceId, owner->packageId };
                entry.path = request.kind == LuaWidgetFilePickerKind::Folder
                    ? std::filesystem::path(L"Preview Folder")
                    : std::filesystem::path(L"Preview.txt");
                entry.kind = kind;
                entry.access = request.access;
                filesystemPickerCompletions_.insert_or_assign(
                    action.id, std::move(entry));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!filePickerCallback_ || !filesystemHandleStore_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }

            LuaWidgetFilePickerResult selected;
            try
            {
                selected = filePickerCallback_(request);
            }
            catch (...)
            {
                selected.error = "pickerFailed";
            }
            if (!selected)
            {
                (void)taskBroker_->Complete(action.id, false,
                    selected.canceled ? "userCanceled" :
                        (selected.error.empty()
                            ? "pickerFailed" : selected.error));
                continue;
            }
            auto grant = filesystemHandleStore_->Grant(
                { action.instanceId, owner->packageId },
                selected.path, kind, request.access);
            if (!grant)
            {
                (void)taskBroker_->Complete(action.id, false,
                    grant.error.empty()
                        ? "handleGrantFailed" : grant.error);
                continue;
            }
            filesystemPickerCompletions_.insert_or_assign(
                action.id, std::move(*grant.entry));
            (void)taskBroker_->Complete(action.id, true);
            continue;
        }

        if (IsFilesystemHandleTask(action.name))
        {
            const auto handle = action.arguments.find("handle");
            if (handle == action.arguments.end())
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            const bool previewHandle = action.preview &&
                IsPreviewFilesystemHandle(handle->second);
            std::optional<snowdesktop::widget_runtime::
                WidgetFilesystemHandleEntry> entry;
            if (!previewHandle)
            {
                if (!filesystemHandleStore_)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerUnavailable");
                    continue;
                }
                entry = filesystemHandleStore_->Resolve(
                    { action.instanceId, owner->packageId },
                    handle->second);
                if (!entry)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidReference");
                    continue;
                }
            }

            if (action.name == "filesystem.release")
            {
                if (action.preview)
                {
                    (void)taskBroker_->Complete(action.id, true);
                    continue;
                }
                const bool busy = std::any_of(
                    filesystemTaskHandles_.begin(),
                    filesystemTaskHandles_.end(),
                    [&handle](const auto& active) {
                        return active.second == handle->second;
                    }) || std::any_of(
                        filesystemWatchBindings_.begin(),
                        filesystemWatchBindings_.end(),
                        [&handle](const auto& active) {
                            return active.second.sourceHandle ==
                                handle->second;
                        });
                if (busy)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "handleBusy");
                    continue;
                }
                std::string revokeError;
                const bool revoked = filesystemHandleStore_->Revoke(
                    { action.instanceId, owner->packageId },
                    handle->second, revokeError);
                (void)taskBroker_->Complete(action.id, revoked,
                    revoked ? std::string{} :
                        (revokeError.empty()
                            ? "invalidReference" : revokeError));
                continue;
            }

            snowdesktop::widget_runtime::WidgetFilesystemTaskRequest request;
            request.action = action.name;
            request.handle = handle->second;
            if (entry) request.path = entry->path;
            const auto parseSize = [&action](const char* field,
                std::size_t fallback, std::size_t& output) {
                const auto value = action.arguments.find(field);
                if (value == action.arguments.end())
                {
                    output = fallback;
                    return true;
                }
                const char* begin = value->second.data();
                const char* end = begin + value->second.size();
                const auto parsed = std::from_chars(begin, end, output);
                return parsed.ec == std::errc{} && parsed.ptr == end;
            };
            if (action.name == "filesystem.list" &&
                (!parseSize("offset", 0, request.offset) ||
                    !parseSize("limit", 50, request.limit)))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.name == "filesystem.read" &&
                !parseSize("maxBytes", 512 * 1024, request.maxBytes))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (action.name == "filesystem.write")
            {
                const auto text = action.arguments.find("text");
                if (text == action.arguments.end())
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "invalidArguments");
                    continue;
                }
                request.text = text->second;
                if (const auto revision =
                        action.arguments.find("expectedRevision");
                    revision != action.arguments.end())
                    request.expectedRevision = revision->second;
            }

            if (action.preview)
            {
                snowdesktop::widget_runtime::WidgetFilesystemTaskCompletion
                    completion;
                completion.id = action.id;
                completion.action = action.name;
                completion.ok = true;
                completion.metadata.handle = handle->second;
                completion.metadata.name = action.name == "filesystem.list"
                    ? "Preview Folder" : "Preview.txt";
                completion.metadata.kind =
                    action.name == "filesystem.list"
                    ? snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::Folder
                    : snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::File;
                completion.metadata.size =
                    action.name == "filesystem.read" ? 20 :
                    request.text.size();
                completion.metadata.modifiedMs = 1786752000000LL;
                completion.metadata.revision =
                    "r1-0000000000000001-0000000000000001-f";
                if (action.name == "filesystem.read")
                    completion.text = "Preview file content";
                if (action.name == "filesystem.list")
                {
                    snowdesktop::widget_runtime::WidgetFilesystemMetadata item;
                    item.handle =
                        "filesystem:11111111111111111111111111111111";
                    item.name = "Preview.txt";
                    item.kind = snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::File;
                    item.size = 20;
                    item.modifiedMs = 1786752000000LL;
                    item.revision =
                        "r1-0000000000000001-0000000000000001-f";
                    completion.items.push_back(std::move(item));
                    completion.nextOffset = 1;
                }
                filesystemTaskCompletions_.insert_or_assign(
                    action.id, std::move(completion));
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!filesystemTaskExecutor_ ||
                !snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::
                    ValidateRequest(request))
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
                continue;
            }
            auto start = filesystemTaskExecutor_->Start(
                action.id, action.instanceId, std::move(request));
            if (start)
                filesystemTaskHandles_.insert_or_assign(
                    action.id, handle->second);
            else
                (void)taskBroker_->Complete(action.id, false,
                    start.error.empty()
                        ? "taskExecutorUnavailable" : start.error);
            continue;
        }

        if (action.preview)
        {
            (void)taskBroker_->Complete(action.id, true);
            continue;
        }
        if (snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor::
                SupportsAction(action.name))
        {
            snowdesktop::widget_runtime::WidgetAudioOutputTaskRequest
                audioRequest;
            audioRequest.action = action.name;
            bool validAudioRequest = true;
            if (action.name == "audio.output.setVolume")
            {
                const auto value = action.arguments.find("volume");
                double volume = 0.0;
                if (value == action.arguments.end())
                    validAudioRequest = false;
                else
                {
                    const char* begin = value->second.data();
                    const char* end = begin + value->second.size();
                    const auto parsed = std::from_chars(
                        begin, end, volume, std::chars_format::general);
                    validAudioRequest = parsed.ec == std::errc{} &&
                        parsed.ptr == end;
                    if (validAudioRequest) audioRequest.volume = volume;
                }
            }
            else
            {
                const auto value = action.arguments.find("muted");
                validAudioRequest = value != action.arguments.end() &&
                    (value->second == "0" || value->second == "1");
                if (validAudioRequest)
                    audioRequest.muted = value->second == "1";
            }
            validAudioRequest = validAudioRequest &&
                snowdesktop::widget_runtime::
                    WidgetAudioOutputTaskExecutor::ValidateRequest(
                        audioRequest);
            if (!validAudioRequest)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "invalidArguments");
                continue;
            }
            if (!audioOutputTaskExecutor_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "taskExecutorUnavailable");
                continue;
            }
            auto start = audioOutputTaskExecutor_->Start(
                action.id, action.instanceId, std::move(audioRequest));
            if (!start)
            {
                (void)taskBroker_->Complete(action.id, false,
                    start.error.empty()
                        ? "taskExecutorUnavailable" : start.error);
            }
            continue;
        }
        snowdesktop::widget_runtime::WidgetMediaTaskRequest mediaRequest;
        mediaRequest.action = action.name;
        if (const auto value = action.arguments.find("sessionId");
            value != action.arguments.end())
            mediaRequest.sessionId = value->second;
        bool validMediaRequest = true;
        if (action.name == "media.seek")
        {
            const auto value = action.arguments.find("positionMs");
            std::int64_t position = 0;
            if (value == action.arguments.end())
                validMediaRequest = false;
            else
            {
                const char* begin = value->second.data();
                const char* end = begin + value->second.size();
                const auto parsed = std::from_chars(begin, end, position);
                validMediaRequest = parsed.ec == std::errc{} &&
                    parsed.ptr == end;
                if (validMediaRequest) mediaRequest.positionMs = position;
            }
        }
        else if (action.name == "media.setRate")
        {
            const auto value = action.arguments.find("rate");
            double rate = 0.0;
            if (value == action.arguments.end())
                validMediaRequest = false;
            else
            {
                const char* begin = value->second.data();
                const char* end = begin + value->second.size();
                const auto parsed = std::from_chars(begin, end, rate,
                    std::chars_format::general);
                validMediaRequest = parsed.ec == std::errc{} &&
                    parsed.ptr == end;
                if (validMediaRequest) mediaRequest.rate = rate;
            }
        }
        else if (action.name == "media.setShuffle")
        {
            const auto value = action.arguments.find("shuffle");
            validMediaRequest = value != action.arguments.end() &&
                (value->second == "0" || value->second == "1");
            if (validMediaRequest)
                mediaRequest.shuffle = value->second == "1";
        }
        else if (action.name == "media.setRepeat")
        {
            const auto value = action.arguments.find("mode");
            validMediaRequest = value != action.arguments.end();
            if (validMediaRequest) mediaRequest.repeatMode = value->second;
        }
        validMediaRequest = validMediaRequest &&
            snowdesktop::widget_runtime::WidgetMediaTaskExecutor::
                ValidateRequest(mediaRequest);
        if (!validMediaRequest)
        {
            (void)taskBroker_->Complete(
                action.id, false, "invalidArguments");
            continue;
        }
        if (!mediaTaskExecutor_ ||
            !mediaTaskExecutor_->Start(
                action.id, std::move(mediaRequest)))
        {
            (void)taskBroker_->Complete(
                action.id, false, "taskExecutorUnavailable");
        }
    }

    for (auto& completion : taskBroker_->DrainCompletions())
    {
        auto widget = std::find_if(widgets_.begin(), widgets_.end(),
            [&completion](const LuaWidget& candidate) {
                return candidate.runtimeToken == completion.ownerToken;
            });
        const auto searchCompletion =
            appSearchCompletions_.find(completion.id);
        const auto itemSearchCompletion =
            itemSearchCompletions_.find(completion.id);
        const auto calendarCompletion =
            calendarMutationCompletions_.find(completion.id);
        const auto networkCompletion =
            networkTaskCompletions_.find(completion.id);
        const auto clipboardCompletion =
            clipboardTaskCompletions_.find(completion.id);
        const auto filesystemPickerCompletion =
            filesystemPickerCompletions_.find(completion.id);
        const auto filesystemTaskCompletion =
            filesystemTaskCompletions_.find(completion.id);
        if (widget == widgets_.end() ||
            widget->taskIds.erase(completion.id) == 0)
        {
            if (searchCompletion != appSearchCompletions_.end())
                appSearchCompletions_.erase(searchCompletion);
            if (itemSearchCompletion != itemSearchCompletions_.end())
                itemSearchCompletions_.erase(itemSearchCompletion);
            if (calendarCompletion != calendarMutationCompletions_.end())
                calendarMutationCompletions_.erase(calendarCompletion);
            if (networkCompletion != networkTaskCompletions_.end())
                networkTaskCompletions_.erase(networkCompletion);
            if (clipboardCompletion != clipboardTaskCompletions_.end())
                clipboardTaskCompletions_.erase(clipboardCompletion);
            if (filesystemPickerCompletion !=
                    filesystemPickerCompletions_.end())
                filesystemPickerCompletions_.erase(
                    filesystemPickerCompletion);
            if (filesystemTaskCompletion !=
                    filesystemTaskCompletions_.end())
                filesystemTaskCompletions_.erase(
                    filesystemTaskCompletion);
            continue;
        }
        if (completion.ok && completion.name == "app.search" &&
            searchCompletion == appSearchCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool calendarTask =
            completion.name == "calendar.create" ||
            completion.name == "calendar.update" ||
            completion.name == "calendar.remove";
        if (calendarTask &&
            calendarCompletion == calendarMutationCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool networkTask = completion.name == "network.request";
        if (completion.ok && networkTask &&
            networkCompletion == networkTaskCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool clipboardReadTask =
            completion.name == "clipboard.read";
        if (completion.ok && clipboardReadTask &&
            clipboardCompletion == clipboardTaskCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool filesystemPickerTask =
            IsFilesystemPickerTask(completion.name);
        if (completion.ok && filesystemPickerTask &&
            filesystemPickerCompletion == filesystemPickerCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool filesystemDataTask =
            snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::
                SupportsAction(completion.name);
        if (completion.ok && filesystemDataTask &&
            filesystemTaskCompletion == filesystemTaskCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool itemSearchTask =
            completion.name == "desktop.search" ||
            completion.name == "everything.search";
        if (completion.ok && itemSearchTask &&
            itemSearchCompletion == itemSearchCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }

        struct PublicAppSearchItem
        {
            std::string reference;
            std::string title;
            std::string source;
            std::string type;
        };
        std::vector<PublicAppSearchItem> publicItems;
        std::vector<PublicAppSearchItem> publicClipboardItems;
        std::size_t nextOffset = 0;
        bool hasMore = false;
        std::uint64_t catalogRevision = 0;
        if (completion.ok && completion.name == "app.search")
        {
            constexpr std::size_t MaximumReferences = 2048;
            catalogRevision = searchCompletion->second.catalogRevision;
            std::erase_if(widget->applicationReferences,
                [catalogRevision](const auto& entry) {
                    return entry.second.catalogRevision != catalogRevision;
                });
            if (widget->applicationReferences.size() +
                    searchCompletion->second.items.size() >
                MaximumReferences)
            {
                widget->applicationReferences.clear();
            }
            publicItems.reserve(searchCompletion->second.items.size());
            for (const auto& item : searchCompletion->second.items)
            {
                const std::string baseReference =
                    snowdesktop::widget_runtime::MakeWidgetAppReference(
                        item.id);
                if (baseReference.empty()) continue;
                std::string reference = baseReference;
                for (std::size_t suffix = 1;; ++suffix)
                {
                    const auto collision =
                        widget->applicationReferences.find(reference);
                    if (collision == widget->applicationReferences.end() ||
                        collision->second.catalogId == item.id)
                        break;
                    reference = baseReference + ":" +
                        std::to_string(suffix);
                }
                widget->applicationReferences.insert_or_assign(reference,
                    LuaWidget::ApplicationReference{ item.id,
                        item.launchTarget, catalogRevision, item.title,
                        item.source, item.type, false });
                publicItems.push_back({ std::move(reference), item.title,
                    item.source, item.type });
            }
            nextOffset = searchCompletion->second.nextOffset;
            hasMore = searchCompletion->second.hasMore;
        }
        else if (completion.ok && itemSearchTask)
        {
            constexpr std::size_t MaximumReferences = 4096;
            catalogRevision =
                itemSearchCompletion->second.catalogRevision;
            if (completion.name == "desktop.search")
            {
                std::erase_if(widget->itemReferences,
                    [&completion, catalogRevision](const auto& entry) {
                        return entry.second.sourceTask == completion.name &&
                            entry.second.revision != catalogRevision;
                    });
            }
            if (widget->itemReferences.size() +
                    itemSearchCompletion->second.items.size() >
                MaximumReferences)
            {
                widget->itemReferences.clear();
            }
            publicItems.reserve(
                itemSearchCompletion->second.items.size());
            for (const auto& item :
                itemSearchCompletion->second.items)
            {
                const std::string baseReference =
                    snowdesktop::widget_runtime::MakeWidgetItemReference(
                        completion.name + ":" +
                            std::to_string(widget->runtimeToken),
                        item.id);
                if (baseReference.empty()) continue;
                std::string reference = baseReference;
                for (std::size_t suffix = 1;; ++suffix)
                {
                    const auto collision =
                        widget->itemReferences.find(reference);
                    if (collision == widget->itemReferences.end() ||
                        (collision->second.sourceTask == completion.name &&
                            collision->second.target == item.launchTarget))
                        break;
                    reference = baseReference + ":" +
                        std::to_string(suffix);
                }
                widget->itemReferences.insert_or_assign(reference,
                    LuaWidget::ItemReference{ item.launchTarget,
                        completion.name, catalogRevision, item.title,
                        item.source, item.type,
                        completion.name == "desktop.search"
                            ? "desktop.item" : "filesystem.reference",
                        false });
                publicItems.push_back({ std::move(reference), item.title,
                    item.source, item.type });
            }
            nextOffset = itemSearchCompletion->second.nextOffset;
            hasMore = itemSearchCompletion->second.hasMore &&
                nextOffset <= 100;
        }
        const snowdesktop::calendar::MutationResult* calendarResult =
            calendarCompletion != calendarMutationCompletions_.end()
            ? &calendarCompletion->second : nullptr;
        const HttpResponse* networkResult =
            networkCompletion != networkTaskCompletions_.end()
            ? &networkCompletion->second : nullptr;
        const snowdesktop::widget_runtime::WidgetClipboardTaskCompletion*
            clipboardResult =
                clipboardCompletion != clipboardTaskCompletions_.end()
                ? &clipboardCompletion->second : nullptr;
        std::string clipboardImageToken;
        if (completion.ok && clipboardResult &&
            clipboardResult->format == "image")
        {
            clipboardImageToken = RegisterRuntimeImageSource(
                d2dState_, widget->widgetId,
                clipboardResult->resourceToken,
                clipboardResult->image, "clipboard");
            if (clipboardImageToken.empty())
            {
                completion.ok = false;
                completion.error = "taskResultUnavailable";
            }
        }
        if (completion.ok && clipboardResult &&
            clipboardResult->format == "file-reference")
        {
            constexpr std::size_t MaximumReferences = 4096;
            std::erase_if(widget->itemReferences, [](const auto& entry) {
                return entry.second.sourceTask == "clipboard.read";
            });
            if (widget->itemReferences.size() +
                    clipboardResult->files.size() > MaximumReferences)
            {
                widget->itemReferences.clear();
            }
            publicClipboardItems.reserve(clipboardResult->files.size());
            for (const auto& file : clipboardResult->files)
            {
                const std::string target = WidgetWideToUtf8(
                    file.path.wstring());
                std::wstring displayName = file.path.filename().wstring();
                if (displayName.empty())
                    displayName = file.path.root_name().wstring();
                const std::string name = WidgetWideToUtf8(displayName);
                const std::string reference = snowdesktop::widget_runtime::
                    MakeWidgetItemReference(
                        "clipboard.read:" +
                            std::to_string(widget->runtimeToken),
                        target);
                if (target.empty() || name.empty() || reference.empty())
                {
                    completion.ok = false;
                    completion.error = "taskResultUnavailable";
                    publicClipboardItems.clear();
                    std::erase_if(widget->itemReferences,
                        [](const auto& entry) {
                            return entry.second.sourceTask ==
                                "clipboard.read";
                        });
                    break;
                }
                widget->itemReferences.insert_or_assign(reference,
                    LuaWidget::ItemReference{ target, "clipboard.read",
                        completion.id, name, "clipboard",
                        file.folder ? "folder" : "file",
                        "filesystem.reference", false });
                publicClipboardItems.push_back({ reference, name,
                    "clipboard", file.folder ? "folder" : "file" });
            }
        }
        const snowdesktop::widget_runtime::WidgetFilesystemHandleEntry*
            filesystemPickerResult =
                filesystemPickerCompletion !=
                    filesystemPickerCompletions_.end()
                ? &filesystemPickerCompletion->second : nullptr;
        const snowdesktop::widget_runtime::WidgetFilesystemTaskCompletion*
            filesystemTaskResult =
                filesystemTaskCompletion !=
                    filesystemTaskCompletions_.end()
                ? &filesystemTaskCompletion->second : nullptr;
        snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
            trustedGestureState_, false);
        (void)InvokeLifecycleEvent(*widget, "task.complete",
            [&completion, &publicItems, &publicClipboardItems,
                clipboardImageToken,
                nextOffset, hasMore,
                catalogRevision, calendarTask, itemSearchTask,
                calendarResult, networkTask,
                networkResult, clipboardReadTask,
                clipboardResult, filesystemPickerTask,
                filesystemPickerResult, filesystemDataTask,
                filesystemTaskResult](lua_State* eventState) {
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(completion.id));
                lua_setfield(eventState, -2, "taskId");
                lua_pushlstring(eventState, completion.name.data(),
                    completion.name.size());
                lua_setfield(eventState, -2, "task");
                lua_pushboolean(eventState, completion.ok ? 1 : 0);
                lua_setfield(eventState, -2, "ok");
                if (completion.ok)
                {
                    if (completion.name == "app.search" || itemSearchTask)
                    {
                        lua_createtable(eventState, 0, 4);
                        lua_createtable(eventState,
                            static_cast<int>(publicItems.size()), 0);
                        int index = 1;
                        for (const auto& item : publicItems)
                        {
                            lua_createtable(eventState, 0, 4);
                            lua_pushlstring(eventState,
                                item.reference.data(), item.reference.size());
                            lua_setfield(eventState, -2, "ref");
                            lua_pushlstring(eventState,
                                item.title.data(), item.title.size());
                            lua_setfield(eventState, -2, "title");
                            lua_pushlstring(eventState,
                                item.source.data(), item.source.size());
                            lua_setfield(eventState, -2, "source");
                            lua_pushlstring(eventState,
                                item.type.data(), item.type.size());
                            lua_setfield(eventState, -2, "type");
                            lua_rawseti(eventState, -2, index++);
                        }
                        lua_setfield(eventState, -2, "items");
                        lua_pushinteger(eventState,
                            static_cast<lua_Integer>(nextOffset));
                        lua_setfield(eventState, -2, "nextOffset");
                        lua_pushboolean(eventState, hasMore ? 1 : 0);
                        lua_setfield(eventState, -2, "hasMore");
                        lua_pushinteger(eventState,
                            static_cast<lua_Integer>(catalogRevision));
                        lua_setfield(eventState, -2,
                            itemSearchTask ? "revision" :
                                "catalogRevision");
                    }
                    else if (calendarTask)
                    {
                        lua_createtable(eventState, 0, 2);
                        lua_pushlstring(eventState,
                            calendarResult->id.data(),
                            calendarResult->id.size());
                        lua_setfield(eventState, -2, "id");
                        lua_pushinteger(eventState,
                            calendarResult->revision);
                        lua_setfield(eventState, -2, "revision");
                    }
                    else if (networkTask)
                    {
                        lua_createtable(eventState, 0, 3);
                        lua_pushinteger(eventState, networkResult->status);
                        lua_setfield(eventState, -2, "status");
                        lua_pushlstring(eventState,
                            networkResult->body.data(),
                            networkResult->body.size());
                        lua_setfield(eventState, -2, "body");
                        lua_pushboolean(eventState,
                            networkResult->fromCache ? 1 : 0);
                        lua_setfield(eventState, -2, "fromCache");
                    }
                    else if (clipboardReadTask)
                    {
                        lua_createtable(eventState, 0, 5);
                        lua_pushlstring(eventState,
                            clipboardResult->format.data(),
                            clipboardResult->format.size());
                        lua_setfield(eventState, -2, "format");
                        if (clipboardResult->format == "text")
                        {
                            lua_pushlstring(eventState,
                                clipboardResult->text.data(),
                                clipboardResult->text.size());
                            lua_setfield(eventState, -2, "text");
                        }
                        else if (clipboardResult->format == "image")
                        {
                            PushResourceHandle(eventState,
                                LuaResourceType::Image,
                                clipboardImageToken);
                            lua_setfield(eventState, -2, "image");
                            lua_pushinteger(eventState,
                                clipboardResult->image->width);
                            lua_setfield(eventState, -2, "width");
                            lua_pushinteger(eventState,
                                clipboardResult->image->height);
                            lua_setfield(eventState, -2, "height");
                        }
                        else
                        {
                            lua_createtable(eventState,
                                static_cast<int>(
                                    publicClipboardItems.size()), 0);
                            int index = 1;
                            for (const auto& item : publicClipboardItems)
                            {
                                lua_createtable(eventState, 0, 3);
                                lua_pushlstring(eventState,
                                    item.reference.data(),
                                    item.reference.size());
                                lua_setfield(eventState, -2, "ref");
                                lua_pushlstring(eventState,
                                    item.title.data(), item.title.size());
                                lua_setfield(eventState, -2, "name");
                                lua_pushlstring(eventState,
                                    item.type.data(), item.type.size());
                                lua_setfield(eventState, -2, "type");
                                lua_rawseti(eventState, -2, index++);
                            }
                            lua_setfield(eventState, -2, "items");
                        }
                    }
                    else if (filesystemPickerTask)
                    {
                        lua_createtable(eventState, 0, 4);
                        lua_pushlstring(eventState,
                            filesystemPickerResult->handle.data(),
                            filesystemPickerResult->handle.size());
                        lua_setfield(eventState, -2, "handle");
                        const std::string_view kind =
                            snowdesktop::widget_runtime::
                                WidgetFilesystemHandleStore::KindName(
                                    filesystemPickerResult->kind);
                        lua_pushlstring(eventState,
                            kind.data(), kind.size());
                        lua_setfield(eventState, -2, "kind");
                        const std::string_view access =
                            snowdesktop::widget_runtime::
                                WidgetFilesystemHandleStore::AccessName(
                                    filesystemPickerResult->access);
                        lua_pushlstring(eventState,
                            access.data(), access.size());
                        lua_setfield(eventState, -2, "access");
                        const std::string name = WidgetWideToUtf8(
                            filesystemPickerResult->path.filename().wstring());
                        lua_pushlstring(eventState,
                            name.data(), name.size());
                        lua_setfield(eventState, -2, "name");
                    }
                    else if (filesystemDataTask)
                    {
                        const auto pushMetadata = [](lua_State* state,
                            const snowdesktop::widget_runtime::
                                WidgetFilesystemMetadata& metadata) {
                            lua_createtable(state, 0, 7);
                            lua_pushlstring(state, metadata.handle.data(),
                                metadata.handle.size());
                            lua_setfield(state, -2, "handle");
                            const std::string_view kind =
                                snowdesktop::widget_runtime::
                                    WidgetFilesystemHandleStore::KindName(
                                        metadata.kind);
                            lua_pushlstring(state, kind.data(), kind.size());
                            lua_setfield(state, -2, "kind");
                            lua_pushlstring(state, metadata.name.data(),
                                metadata.name.size());
                            lua_setfield(state, -2, "name");
                            if (metadata.kind == snowdesktop::widget_runtime::
                                    WidgetFilesystemHandleKind::File)
                            {
                                lua_pushinteger(state,
                                    static_cast<lua_Integer>(metadata.size));
                                lua_setfield(state, -2, "size");
                            }
                            lua_pushinteger(state,
                                static_cast<lua_Integer>(
                                    metadata.modifiedMs));
                            lua_setfield(state, -2, "modifiedMs");
                            lua_pushboolean(state,
                                metadata.readOnly ? 1 : 0);
                            lua_setfield(state, -2, "readOnly");
                            lua_pushlstring(state,
                                metadata.revision.data(),
                                metadata.revision.size());
                            lua_setfield(state, -2, "revision");
                        };
                        if (completion.name == "filesystem.stat")
                        {
                            pushMetadata(eventState,
                                filesystemTaskResult->metadata);
                        }
                        else if (completion.name == "filesystem.list")
                        {
                            lua_createtable(eventState, 0, 3);
                            lua_createtable(eventState,
                                static_cast<int>(
                                    filesystemTaskResult->items.size()), 0);
                            int index = 1;
                            for (const auto& item :
                                filesystemTaskResult->items)
                            {
                                pushMetadata(eventState, item);
                                lua_rawseti(eventState, -2, index++);
                            }
                            lua_setfield(eventState, -2, "items");
                            lua_pushinteger(eventState,
                                static_cast<lua_Integer>(
                                    filesystemTaskResult->nextOffset));
                            lua_setfield(eventState, -2, "nextOffset");
                            lua_pushboolean(eventState,
                                filesystemTaskResult->hasMore ? 1 : 0);
                            lua_setfield(eventState, -2, "hasMore");
                        }
                        else if (completion.name == "filesystem.read")
                        {
                            lua_createtable(eventState, 0, 4);
                            lua_pushliteral(eventState, "utf8");
                            lua_setfield(eventState, -2, "encoding");
                            lua_pushlstring(eventState,
                                filesystemTaskResult->text.data(),
                                filesystemTaskResult->text.size());
                            lua_setfield(eventState, -2, "text");
                            lua_pushinteger(eventState,
                                static_cast<lua_Integer>(
                                    filesystemTaskResult->metadata.size));
                            lua_setfield(eventState, -2, "size");
                            lua_pushlstring(eventState,
                                filesystemTaskResult->metadata.revision.data(),
                                filesystemTaskResult->metadata.revision.size());
                            lua_setfield(eventState, -2, "revision");
                        }
                        else
                        {
                            lua_createtable(eventState, 0, 4);
                            lua_pushboolean(eventState, 1);
                            lua_setfield(eventState, -2, "accepted");
                            lua_pushinteger(eventState,
                                static_cast<lua_Integer>(
                                    filesystemTaskResult->metadata.size));
                            lua_setfield(eventState, -2, "size");
                            lua_pushinteger(eventState,
                                static_cast<lua_Integer>(
                                    filesystemTaskResult->metadata.modifiedMs));
                            lua_setfield(eventState, -2, "modifiedMs");
                            lua_pushlstring(eventState,
                                filesystemTaskResult->metadata.revision.data(),
                                filesystemTaskResult->metadata.revision.size());
                            lua_setfield(eventState, -2, "revision");
                        }
                    }
                    else
                    {
                        lua_createtable(eventState, 0, 1);
                        lua_pushboolean(eventState, 1);
                        lua_setfield(eventState, -2, "accepted");
                    }
                    lua_setfield(eventState, -2, "value");
                }
                else
                {
                    lua_pushlstring(eventState, completion.error.data(),
                        completion.error.size());
                    lua_setfield(eventState, -2, "error");
                    if (calendarTask && calendarResult &&
                        calendarResult->revision > 0)
                    {
                        lua_pushinteger(eventState,
                            calendarResult->revision);
                        lua_setfield(eventState, -2, "currentRevision");
                    }
                    if (networkTask && networkResult)
                    {
                        lua_pushinteger(eventState, networkResult->status);
                        lua_setfield(eventState, -2, "status");
                    }
                }
            });
        if (searchCompletion != appSearchCompletions_.end())
            appSearchCompletions_.erase(searchCompletion);
        if (itemSearchCompletion != itemSearchCompletions_.end())
            itemSearchCompletions_.erase(itemSearchCompletion);
        if (calendarCompletion != calendarMutationCompletions_.end())
            calendarMutationCompletions_.erase(calendarCompletion);
        if (networkCompletion != networkTaskCompletions_.end())
            networkTaskCompletions_.erase(networkCompletion);
        if (clipboardCompletion != clipboardTaskCompletions_.end())
            clipboardTaskCompletions_.erase(clipboardCompletion);
        if (filesystemPickerCompletion !=
                filesystemPickerCompletions_.end())
            filesystemPickerCompletions_.erase(filesystemPickerCompletion);
        if (filesystemTaskCompletion !=
                filesystemTaskCompletions_.end())
            filesystemTaskCompletions_.erase(filesystemTaskCompletion);
    }
}

void WidgetEngine::ReleaseWidgetTasks(LuaWidget& widget,
    snowdesktop::widget_runtime::TaskBrokerCancelReason reason)
{
    if (!taskBroker_)
    {
        widget.taskIds.clear();
        return;
    }
    for (const std::uint64_t taskId : widget.taskIds)
    {
        (void)taskBroker_->Cancel(taskId, reason);
        if (mediaTaskExecutor_)
            (void)mediaTaskExecutor_->Cancel(taskId);
        if (audioOutputTaskExecutor_)
            (void)audioOutputTaskExecutor_->Cancel(taskId);
        if (clipboardTaskExecutor_)
            (void)clipboardTaskExecutor_->Cancel(taskId);
        if (filesystemTaskExecutor_)
            (void)filesystemTaskExecutor_->Cancel(taskId);
        if (appTaskExecutor_)
            (void)appTaskExecutor_->Cancel(taskId);
        if (desktopTaskExecutor_)
            (void)desktopTaskExecutor_->Cancel(taskId);
        if (externalItemTaskExecutor_)
            (void)externalItemTaskExecutor_->Cancel(taskId);
        appSearchCompletions_.erase(taskId);
        itemSearchCompletions_.erase(taskId);
        calendarMutationCompletions_.erase(taskId);
        networkTaskCompletions_.erase(taskId);
        clipboardTaskCompletions_.erase(taskId);
        filesystemPickerCompletions_.erase(taskId);
        filesystemTaskCompletions_.erase(taskId);
    }
    if (audioOutputTaskExecutor_)
        audioOutputTaskExecutor_->ForgetInstance(
            WidgetWideToUtf8(widget.widgetId));
    if (clipboardTaskExecutor_)
        clipboardTaskExecutor_->ForgetInstance(
            WidgetWideToUtf8(widget.widgetId));
    if (filesystemTaskExecutor_)
        filesystemTaskExecutor_->ForgetInstance(
            WidgetWideToUtf8(widget.widgetId));
    widget.taskIds.clear();
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
    filesystemHandleStore_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetFilesystemHandleStore>(
            GetDataFilePath(L"SnowDesktop.widget-file-handles.json"));
    std::string filesystemHandleError;
    if (!filesystemHandleStore_->Load(filesystemHandleError))
    {
        const std::string diagnostic =
            "SnowDesktop filesystem handle registry load failed: " +
            filesystemHandleError + "\n";
        OutputDebugStringA(diagnostic.c_str());
    }
    filesystemWatchService_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetFilesystemWatchService>();
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
    InitializeWidgetDataBroker();
    InitializeWidgetTaskBroker();
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
    InitializeWidgetDataBroker();
    InitializeWidgetTaskBroker();
    return d2dState_ != nullptr;
}

void WidgetEngine::Shutdown()
{
    focusedHostInput_ = {};
    if (taskBroker_)
        taskBroker_->Shutdown();
    if (d2dState_)
        d2dState_->shellIconLoader.reset();
    for (auto& widget : widgets_)
    {
        if (widget.valid && widget.hostVisible)
            InvokeSimpleCallback(widget, "onHidden");
        DisposeWidgetLifecycle(widget, "shutdown");
        ReleaseWidgetDataSubscriptions(widget);
        ReleaseWidgetTasks(widget,
            snowdesktop::widget_runtime::TaskBrokerCancelReason::Shutdown);
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
            widget.lifecycle.Release(widget.state);
            luaL_unref(widget.state, LUA_REGISTRYINDEX, widget.ref);
            lua_close(widget.state);
            widget.state = nullptr;
        }
    }
    if (dataBroker_)
    {
        dataBroker_->Shutdown(
            snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
        ApplyWidgetDataBrokerActions();
    }
    if (widgetSystemDataProvider_)
        widgetSystemDataProvider_->StopAll();
    if (widgetAudioAnalysisProvider_)
        widgetAudioAnalysisProvider_->Stop();
    widgets_.clear();
    widgetSystemDataProvider_.reset();
    widgetAudioAnalysisProvider_.reset();
    dataBroker_.reset();
    mediaTaskExecutor_.reset();
    audioOutputTaskExecutor_.reset();
    clipboardTaskExecutor_.reset();
    filesystemWatchService_.reset();
    filesystemHandleStore_.reset();
    filesystemTaskExecutor_.reset();
    appTaskExecutor_.reset();
    desktopTaskExecutor_.reset();
    externalItemTaskExecutor_.reset();
    appSearchCompletions_.clear();
    itemSearchCompletions_.clear();
    calendarMutationCompletions_.clear();
    networkTaskCompletions_.clear();
    clipboardTaskCompletions_.clear();
    filesystemPickerCompletions_.clear();
    filesystemTaskCompletions_.clear();
    filesystemTaskHandles_.clear();
    filesystemWatchBindings_.clear();
    networkTaskRequests_.clear();
    networkRequestTasks_.clear();
    taskBroker_.reset();
    widgetHostFailures_.clear();
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
    DisposeWidgetLifecycle(widgets_[idx], "unload");
    ReleaseWidgetDataSubscriptions(widgets_[idx]);
    ReleaseWidgetTasks(widgets_[idx],
        snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
    if (widgets_[idx].refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].refreshTimerId);
    if (widgets_[idx].namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].namedTimerId);
    if (httpService_) httpService_->CancelWidget(widgetId);
    lua_State* state = widgets_[idx].state;
    if (state)
    {
        widgets_[idx].lifecycle.Release(state);
        luaL_unref(state, LUA_REGISTRYINDEX, widgets_[idx].ref);
        lua_close(state);
        widgets_[idx].state = nullptr;
    }
    if (d2dState_)
    {
        ClearRuntimeImagesForWidget(d2dState_, widgetId);
        d2dState_->privateTextFormatCache.clear();
        d2dState_->privateFonts.clear();
        d2dState_->imageCache.clear();
    }
    widgets_.erase(widgets_.begin() + idx);
    std::erase_if(widgets_, [&widgetId](const LuaWidget& widget) {
        return widget.widgetId == widgetId;
    });

}

void WidgetEngine::DeleteWidgetInstance(const std::wstring& widgetId)
{
    if (filesystemHandleStore_)
    {
        std::string error;
        (void)filesystemHandleStore_->RevokeInstance(
            WidgetWideToUtf8(widgetId), error);
    }
    UnloadWidget(widgetId);
    widgetHostFailures_.erase(widgetId);
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

void WidgetEngine::RevokeFilesystemHandlesForPackage(
    const std::string& packageId)
{
    if (!filesystemHandleStore_ || packageId.empty()) return;
    std::vector<std::uint64_t> subscriptions;
    for (const auto& [subscriptionId, watch] : filesystemWatchBindings_)
    {
        if (watch.owner.packageId == packageId)
            subscriptions.push_back(subscriptionId);
    }
    for (const auto subscriptionId : subscriptions)
        (void)RuntimeUnsubscribeData(subscriptionId);
    std::string error;
    (void)filesystemHandleStore_->RevokePackage(packageId, error);
}

int WidgetEngine::FindWidget(const std::wstring& widgetId) const
{
    for (int i = static_cast<int>(widgets_.size()) - 1; i >= 0; --i)
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
    if (widget.manifest.apiVersion >= 2)
    {
        if (std::strcmp(callbackName, "onVisible") == 0 ||
            std::strcmp(callbackName, "onHidden") == 0)
        {
            const bool visible = std::strcmp(
                callbackName, "onVisible") == 0;
            (void)InvokeLifecycleEvent(widget, "visibility",
                [visible](lua_State* state) {
                    lua_pushboolean(state, visible ? 1 : 0);
                    lua_setfield(state, -2, "visible");
                });
        }
        else if (std::strcmp(callbackName, "onOpen") == 0)
        {
            (void)InvokeLifecycleEvent(widget, "action",
                [](lua_State* state) {
                    lua_pushliteral(state, "open");
                    lua_setfield(state, -2, "id");
                });
        }
        else if (std::strcmp(callbackName, "onSelected") == 0)
        {
            (void)InvokeLifecycleEvent(widget, "selection",
                [](lua_State* state) {
                    lua_pushboolean(state, 1);
                    lua_setfield(state, -2, "selected");
                });
        }
        else if (std::strcmp(callbackName, "onLanguageChanged") == 0)
        {
            (void)InvokeLifecycleEvent(widget, "environment",
                [](lua_State* state) {
                    lua_pushliteral(state, "language");
                    lua_setfield(state, -2, "reason");
                });
        }
        return;
    }
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
    bool stateChanged = false;
    if (lua_isfunction(state, -1))
    {
        if (snowdesktop::lua_runtime::ProtectedCall(state, 0, 0) != LUA_OK)
        {
            const char* error = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, error ? error : "(callback error)");
            lua_pop(state, 1);
        }
        else
        {
            stateChanged = snowdesktop::widget_api::
                ConsumeTransientStateDirty(state);
        }
    }
    else
        lua_pop(state, 1);
    lua_pop(state, 1);
    if (stateChanged)
        RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::InitializeWidgetLifecycle(LuaWidget& widget)
{
    if (widget.manifest.apiVersion < 2) return true;
    lua_State* state = widget.state;
    if (!state) return false;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, widget.lastBounds);
    std::string error;
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (widget.lifecycle.Setup(
            state, widget.ref, pushContext, error))
    {
        (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);
        return true;
    }
    RuntimeRecordError(widget.widgetId,
        error.empty() ? "Widget setup failed" : error);
    RecordWidgetHostFailure(widget.widgetId,
        error.empty() ? "Widget setup failed" : error,
        widget.quota && (widget.quota->memoryExceeded ||
            widget.quota->executionExceeded));
    return false;
}

bool WidgetEngine::InvokeLifecycleEvent(LuaWidget& widget,
    const char* kind,
    const std::function<void(lua_State*)>& pushFields)
{
    if (widget.manifest.apiVersion < 2 || !widget.state ||
        !kind || !*kind)
        return false;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    SetWidgetRectContext(d2dState_, widget.lastBounds);
    lua_createtable(widget.state, 0, 10);
    lua_pushstring(widget.state, kind);
    lua_setfield(widget.state, -2, "kind");
    if (pushFields) pushFields(widget.state);

    bool invoked = false;
    std::string error;
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    const bool succeeded = widget.lifecycle.Event(
        widget.state, widget.ref, pushContext, -1, invoked, error);
    if (!succeeded && !error.empty())
        RuntimeRecordError(widget.widgetId, error);
    const bool stateChanged = snowdesktop::widget_api::
        ConsumeTransientStateDirty(widget.state);
    if ((succeeded && invoked) || stateChanged)
        RuntimeInvalidateHost(widget.widgetId);
    return succeeded && invoked;
}

void WidgetEngine::DisposeWidgetLifecycle(
    LuaWidget& widget, const char* reason)
{
    if (widget.manifest.apiVersion < 2 || !widget.state) return;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    SetWidgetRectContext(d2dState_, widget.lastBounds);
    std::string error;
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (!widget.lifecycle.Dispose(widget.state, widget.ref,
            pushContext, reason, error) && !error.empty())
    {
        RuntimeRecordError(widget.widgetId, error);
    }
    (void)snowdesktop::widget_api::ConsumeTransientStateDirty(widget.state);
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
    for (const std::string_view name :
        snowdesktop::widget_api::SandboxLibraries(
            static_cast<std::uint32_t>(widget.manifest.apiVersion)))
    {
        if (name == "l10n") continue;
        const std::string libraryName(name);
        PushReadOnlyGlobal(L, libraryName.c_str());
        lua_setfield(L, -2, libraryName.c_str());
    }
    PushWidgetL10nAPI(L, widget.manifest);
    PushReadOnlyProxyForTopTable(L);
    lua_setfield(L, -2, "l10n");

    if (widget.manifest.apiVersion == 1 &&
        widget.permissions.contains("ui.input"))
    {
        PushReadOnlyGlobal(L, "imgui");
        lua_setfield(L, -2, "imgui");
    }

    lua_pushstring(L, WidgetWideToUtf8(widget.widgetId).c_str());
    lua_setfield(L, -2, "widgetId");
}

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& packageId)
{
    if (const int index = FindWidget(widgetId); index >= 0)
        return widgets_[index].valid;
    if (widgetHostFailures_.contains(widgetId))
        return false;
    if (const auto package = GetWidgetPackage(packageId))
    {
        const auto grant = snowdesktop::widget::WidgetPermissionBroker::
            Evaluate(package->permissionState,
                package->manifest.permissions,
                package->manifest.optionalPermissions,
                package->manifest.networkDomains,
                package->grantedPermissions,
                package->grantedNetworkDomains);
        if (grant.runtimeBlock !=
            snowdesktop::widget::PermissionRuntimeBlock::None)
        {
            return false;
        }
    }
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
    if (source.empty())
    {
        RuntimeRecordError(widgetId,
            "Widget entry script is empty or cannot be read");
        return false;
    }

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
    {
        std::string slotError;
        if (!pending.logicalSlots.Configure(
                pending.manifest.logicalSlots, slotError) ||
            !pending.logicalSlots.Restore(ActiveStorage(),
                WidgetWideToUtf8(widgetId), slotError))
        {
            RuntimeRecordError(widgetId,
                "Widget logical slot state is invalid: " + slotError);
            return false;
        }
    }
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
        const auto grant = snowdesktop::widget::WidgetPermissionBroker::
            Evaluate(package->permissionState,
                pending.manifest.permissions,
                pending.manifest.optionalPermissions,
                pending.manifest.networkDomains,
                package->grantedPermissions,
                package->grantedNetworkDomains);
        switch (grant.runtimeBlock)
        {
        case snowdesktop::widget::PermissionRuntimeBlock::PendingConsent:
            RuntimeRecordError(widgetId,
                "Widget permission consent is pending");
            return false;
        case snowdesktop::widget::PermissionRuntimeBlock::Denied:
            RuntimeRecordError(widgetId,
                "Widget permission consent was denied");
            return false;
        case snowdesktop::widget::PermissionRuntimeBlock::MissingRequired:
            RuntimeRecordError(widgetId,
                "Widget required permissions are not granted");
            return false;
        case snowdesktop::widget::PermissionRuntimeBlock::None:
            break;
        }
        pending.permissions.insert(
            grant.permissions.begin(), grant.permissions.end());
    }
    else
    {
        for (const auto& permission : pending.manifest.permissions)
            pending.permissions.insert(permission);
        for (const auto& permission : pending.manifest.optionalPermissions)
            pending.permissions.insert(permission);
    }

    WidgetExecutionContextGuard loadContext(d2dState_, widgetId);
    auto quota = std::make_unique<LuaRuntimeQuota>();
    lua_State* state = lua_newstate(LuaQuotaAllocator, quota.get());
    if (!state)
    {
        RuntimeRecordError(widgetId, "Cannot allocate isolated Lua state");
        RecordWidgetHostFailure(widgetId,
            "Cannot allocate isolated Lua state", quota->memoryExceeded);
        return false;
    }
    const bool legacyContract = pending.manifest.schemaVersion ==
            snowdesktop::widget::kLegacyPackageSchemaVersion &&
        pending.manifest.apiVersion == snowdesktop::widget::kLegacyApiVersion;
    const bool currentContract = pending.manifest.schemaVersion ==
            snowdesktop::widget::kPackageSchemaVersion &&
        pending.manifest.apiVersion == snowdesktop::widget::kHostApiVersion;
    if (!legacyContract && !currentContract)
    {
        RuntimeRecordError(widgetId,
            "Widget schema/API contract is not supported by this host");
        return false;
    }
    if (currentContract)
    {
        const auto missing = snowdesktop::widget_api::MissingFeatures(
            pending.manifest.requiredFeatures);
        if (!missing.empty())
        {
            std::string message = "Widget requires unsupported host feature";
            if (missing.size() > 1) message += "s";
            message += ": ";
            for (std::size_t index = 0; index < missing.size(); ++index)
            {
                if (index) message += ", ";
                message += missing[index];
            }
            RuntimeRecordError(widgetId, message);
            return false;
        }
    }
    std::unique_ptr<lua_State, decltype(&lua_close)> stateGuard(
        state, lua_close);
    luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
    RegisterDrawAPI(state, pending.manifest.apiVersion);
    lua_pushinteger(state, pending.manifest.apiVersion);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_api_version");
    lua_pushlightuserdata(state, d2dState_);
    lua_setfield(state, LUA_REGISTRYINDEX, "__d2d_ptr");
    lua_pushlightuserdata(state, quota.get());
    lua_setfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    lua_pushstring(state, WidgetWideToUtf8(widgetId).c_str());
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_id");
    lua_createtable(state, 0,
        static_cast<int>(pending.permissions.size()));
    for (const auto& permission : pending.permissions)
    {
        lua_pushboolean(state, true);
        lua_setfield(state, -2, permission.c_str());
    }
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_permissions");
    lua_pushstring(state,
        WidgetWideToUtf8(pending.packageRoot.wstring()).c_str());
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_package_root");
    lua_createtable(state, 0,
        static_cast<int>(pending.manifest.resources.size()));
    for (const auto& [name, resource] : pending.manifest.resources)
    {
        lua_createtable(state, 0, 3);
        lua_pushlstring(state, resource.type.data(), resource.type.size());
        lua_setfield(state, -2, "type");
        lua_pushlstring(state, resource.path.data(), resource.path.size());
        lua_setfield(state, -2, "path");
        lua_pushlstring(state, resource.license.data(),
            resource.license.size());
        lua_setfield(state, -2, "license");
        lua_setfield(state, -2, name.c_str());
    }
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_resources");
    lua_pushboolean(state, preview ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_preview");
    lua_pushboolean(state, 1);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_loading");
    if (d2dState_)
    {
        d2dState_->currentWidgetId = widgetId;
        d2dState_->storagePrefix = WidgetWideToUtf8(widgetId);
    }
    // Create a sandbox table with only the registered safe API surface.
    PushSafeEnvironment(state, pending);
    lua_pushvalue(state, -1);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_environment");

    // Load the chunk
    if (luaL_loadstring(state, source.c_str()) != LUA_OK)
    {
        const char* err = lua_tostring(state, -1);
        RuntimeRecordError(widgetId, err ? err : "(load error)");
        RecordWidgetHostFailure(widgetId, err ? err : "(load error)",
            quota->memoryExceeded || quota->executionExceeded);
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
            RecordWidgetHostFailure(widgetId,
                err ? err : "(load error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
        lua_pushvalue(state, -2);  // push sandbox as argument
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, 1, currentContract ? 1 : 0, 1000000,
                std::chrono::milliseconds(100)) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            RecordWidgetHostFailure(widgetId,
                err ? err : "(pcall error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
    }
    else
    {
        // Execute the chunk
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, 0, currentContract ? 1 : 0, 1000000,
                std::chrono::milliseconds(100)) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            RecordWidgetHostFailure(widgetId,
                err ? err : "(pcall error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
    }

    lua_pushboolean(state, 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_loading");

    int ref = LUA_NOREF;
    if (currentContract)
    {
        if (!snowdesktop::widget_api::IsDefinedWidget(state, -1))
        {
            RuntimeRecordError(widgetId,
                "API v2 entry must return widget.define({...})");
            lua_pop(state, 2);
            return false;
        }
        ref = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_pop(state, 1); // sandbox
    }
    else
    {
        // API v1 stores callbacks as globals in the sandbox. This branch is
        // retained only while bundled components are migrated to v2.
        ref = luaL_ref(state, LUA_REGISTRYINDEX);
    }
    (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);

    std::string name = pending.manifest.name.empty()
        ? "Unnamed" : pending.manifest.name;
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
            RecordWidgetHostFailure(widgetId,
                "Widget storage migration failed: " + migrationFailure,
                quota->memoryExceeded || quota->executionExceeded);
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
    w.runtimeToken = ++nextWidgetRuntimeToken_;
    if (w.runtimeToken == 0)
        w.runtimeToken = ++nextWidgetRuntimeToken_;
    w.packageId = pending.packageId;
    w.packageRoot = pending.packageRoot;
    w.state = stateGuard.release();
    lua_pushinteger(w.state,
        static_cast<lua_Integer>(w.runtimeToken));
    lua_setfield(w.state, LUA_REGISTRYINDEX,
        "__widget_runtime_token");
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
    w.logicalSlots = std::move(pending.logicalSlots);
    for (const auto& snapshot : w.logicalSlots.Snapshots())
    {
        for (const auto& item : snapshot.items)
        {
            if (item.kind == "app.reference")
            {
                w.applicationReferences.insert_or_assign(item.reference,
                    LuaWidget::ApplicationReference{ {}, item.target, 0,
                        item.title, item.source, item.type, true });
            }
            else
            {
                w.itemReferences.insert_or_assign(item.reference,
                    LuaWidget::ItemReference{ item.target, "logical.slot",
                        0, item.title, item.source, item.type,
                        item.kind, true });
            }
        }
    }
    (void)w.namedTimers.SetVisible(false,
        snowdesktop::widget_runtime::NamedTimerSchedule::Clock::now());
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
    widgets_.push_back(std::move(w));
    if (currentContract &&
        !InitializeWidgetLifecycle(widgets_.back()))
    {
        LuaWidget& failed = widgets_.back();
        ReleaseWidgetDataSubscriptions(failed);
        ReleaseWidgetTasks(failed,
            snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
        if (failed.state)
        {
            failed.lifecycle.Release(failed.state);
            luaL_unref(failed.state, LUA_REGISTRYINDEX, failed.ref);
            lua_close(failed.state);
            failed.state = nullptr;
        }
        widgets_.pop_back();
        return false;
    }
    widgetHostFailures_.erase(widgetId);

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

    for (auto& completion :
        settingsAppTaskExecutor_.DrainCompletions())
    {
        for (auto& [key, search] : settingsAppSearchStates_)
        {
            (void)key;
            if (search.taskId != completion.id)
                continue;
            search.taskId = 0;
            search.completed = completion.ok;
            search.error = completion.ok
                ? std::string{} : completion.error;
            search.results = completion.ok
                ? std::move(completion.items)
                : std::vector<snowdesktop::widget_runtime::
                    WidgetAppSearchResult>{};
            break;
        }
    }

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
    auto alignEditorControl = [](float requestedWidth) {
        const float rowStart = ImGui::GetCursorPosX();
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const float maximumWidth = std::max(
            ImGui::GetFrameHeight(), availableWidth * 0.58f);
        const float controlWidth = std::min(
            requestedWidth, maximumWidth);
        ImGui::SetCursorPosX(std::max(
            rowStart, rowStart + availableWidth - controlWidth));
        return controlWidth;
    };
    auto startSettingsAppSearch = [&](SettingsAppSearchState& search,
        const std::string& query) {
        if (search.taskId != 0)
            (void)settingsAppTaskExecutor_.Cancel(search.taskId);
        search.taskId = 0;
        search.query = query;
        search.error.clear();
        search.completed = false;
        search.results.clear();
        if (query.empty()) return;
        if (!applicationCatalogProvider_)
        {
            search.error = "providerUnavailable";
            return;
        }

        LuaApplicationCatalogSnapshot catalog;
        try
        {
            catalog = applicationCatalogProvider_();
        }
        catch (...)
        {
            search.error = "providerFailed";
            return;
        }
        if (catalog.state != "ready")
        {
            search.error = catalog.state == "indexing"
                ? "appIndexNotReady" : "providerUnavailable";
            return;
        }

        const std::wstring queryWide = Utf8ToWideLocal(query);
        const std::string foldedQuery = WidgetWideToUtf8(
            ToUpperInvariant(queryWide));
        const std::string pinyinQuery =
            BuildNamePinyinFullKey(queryWide);
        std::uint64_t taskId = ++nextSettingsAppSearchTaskId_;
        if (taskId == 0)
            taskId = ++nextSettingsAppSearchTaskId_;
        if (foldedQuery.empty() ||
            !settingsAppTaskExecutor_.StartSearch(
                taskId, foldedQuery, pinyinQuery, 0, 8,
                appIndexRevision_, std::move(catalog.entries)))
        {
            search.error = "taskExecutorUnavailable";
            return;
        }
        search.taskId = taskId;
    };
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
            {
                const std::string& label =
                    i < setting.optionLabels.size() &&
                        !setting.optionLabels[i].empty()
                    ? setting.optionLabels[i] : setting.options[i];
                optionLabels.push_back(label +
                    "###SettingOption" + std::to_string(i));
            }
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
        else if (setting.type == "appSearch" &&
            !setting.searchKey.empty())
        {
            std::string query = getStorage(setting.searchKey);
            char buffer[512]{};
            strncpy_s(buffer, query.c_str(), _TRUNCATE);
            ImGui::SetNextItemWidth(beginEditorRow(
                setting.label.c_str(), kEditorControlWidth));
            if (ImGui::InputText("##Search", buffer,
                    sizeof(buffer)))
            {
                query = buffer;
                setStorage(setting.searchKey, query);
                setStorage(setting.key, "");
            }

            const std::string stateKey = editorId + "\n" +
                setting.key;
            SettingsAppSearchState& search =
                settingsAppSearchStates_[stateKey];
            bool shouldStart = search.query != query;
            if (!shouldStart && search.taskId == 0 &&
                search.error == "appIndexNotReady" &&
                applicationIndexStatusProvider_)
            {
                try
                {
                    shouldStart =
                        applicationIndexStatusProvider_() == "ready";
                }
                catch (...)
                {
                    shouldStart = false;
                }
            }
            if (shouldStart)
                startSettingsAppSearch(search, query);

            const float listWidth = alignEditorControl(
                kEditorControlWidth);
            const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
            const int visibleRows = std::clamp(
                static_cast<int>(search.results.size()) + 1, 2, 6);
            if (ImGui::BeginListBox("##Results", ImVec2(
                    listWidth, rowHeight * visibleRows +
                        ImGui::GetStyle().FramePadding.y * 2.0f)))
            {
                const std::string emptyLabel =
                    setting.emptyLabel.empty()
                    ? std::string("-") : setting.emptyLabel;
                if (ImGui::Selectable(emptyLabel.c_str(),
                        current.empty()))
                {
                    next.clear();
                    changed = true;
                }
                for (std::size_t resultIndex = 0;
                    resultIndex < search.results.size(); ++resultIndex)
                {
                    const auto& result = search.results[resultIndex];
                    const std::string itemLabel = result.title +
                        "###AppSearchResult" +
                        std::to_string(resultIndex);
                    if (ImGui::Selectable(itemLabel.c_str(),
                            current == result.title))
                    {
                        next = result.title;
                        changed = true;
                    }
                }
                if (!query.empty() && search.results.empty())
                {
                    const std::string statusLabel =
                        search.taskId != 0 ||
                            search.error == "appIndexNotReady"
                        ? std::string("...")
                        : (setting.noResultsLabel.empty()
                            ? std::string("-")
                            : setting.noResultsLabel);
                    ImGui::BeginDisabled();
                    ImGui::Selectable(statusLabel.c_str(), false);
                    ImGui::EndDisabled();
                }
                ImGui::EndListBox();
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
            if (setting.type == "appSearch" &&
                !setting.searchKey.empty())
                setStorage(setting.searchKey, "");
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

static void OverlayViewStyle(
    snowdesktop::widget_runtime::ViewStyle& result,
    const snowdesktop::widget_runtime::ViewStyle& style)
{
    if (style.background) result.background = style.background;
    if (style.foreground) result.foreground = style.foreground;
    if (style.borderColor) result.borderColor = style.borderColor;
    if (style.borderWidth) result.borderWidth = style.borderWidth;
    if (style.cornerRadius) result.cornerRadius = style.cornerRadius;
    if (style.opacity) result.opacity = style.opacity;
}

static snowdesktop::widget_runtime::ViewStyle ResolveViewStyle(
    const snowdesktop::widget_runtime::ViewNode& node,
    bool hovered, bool pressed,
    std::optional<bool> checkedOverride = std::nullopt)
{
    using snowdesktop::widget_runtime::ViewStyle;
    ViewStyle result = node.style;
    if (checkedOverride.value_or(node.checked))
        OverlayViewStyle(result, node.checkedStyle);
    if (hovered) OverlayViewStyle(result, node.hoverStyle);
    if (pressed) OverlayViewStyle(result, node.pressedStyle);
    return result;
}

static void DrawWidgetProgressRing(D2DState* state,
    const D2D1_RECT_F& rect, float value, float thickness,
    std::uint32_t trackColor, float trackOpacity,
    std::uint32_t fillColor, float fillOpacity)
{
    if (!state || !state->ctx) return;
    const float radius = std::max(0.0f,
        std::min(rect.right - rect.left, rect.bottom - rect.top) * 0.5f -
            thickness * 0.5f);
    if (radius <= 0.0f) return;
    const D2D1_POINT_2F center = D2D1::Point2F(
        (rect.left + rect.right) * 0.5f,
        (rect.top + rect.bottom) * 0.5f);
    const D2D1_ELLIPSE ellipse = D2D1::Ellipse(center, radius, radius);
    if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
            static_cast<int>(trackColor), trackOpacity))
        state->ctx->DrawEllipse(ellipse, track, thickness);
    if (value <= 0.0f) return;
    ID2D1SolidColorBrush* fill = GetCachedBrush(state,
        static_cast<int>(fillColor), fillOpacity);
    if (!fill) return;
    if (value >= 0.9999f)
    {
        state->ctx->DrawEllipse(ellipse, fill, thickness);
        return;
    }

    ComPtr<ID2D1Factory> factory;
    state->ctx->GetFactory(&factory);
    if (!factory) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)) || !sink) return;
    constexpr float Pi = 3.14159265358979323846f;
    const float angle = value * 2.0f * Pi;
    const D2D1_POINT_2F start = D2D1::Point2F(
        center.x, center.y - radius);
    const D2D1_POINT_2F end = D2D1::Point2F(
        center.x + std::sin(angle) * radius,
        center.y - std::cos(angle) * radius);
    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(radius, radius),
        0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
        value > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (SUCCEEDED(sink->Close()))
        state->ctx->DrawGeometry(geometry.Get(), fill, thickness);
}

static bool IsWidgetDataSeriesNode(
    snowdesktop::widget_runtime::ViewNodeType type) noexcept
{
    using snowdesktop::widget_runtime::ViewNodeType;
    return type == ViewNodeType::Sparkline ||
        type == ViewNodeType::LineChart ||
        type == ViewNodeType::BarChart ||
        type == ViewNodeType::Waveform ||
        type == ViewNodeType::Spectrum;
}

static void DrawWidgetDataSeries(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style,
    const D2D1_RECT_F& bounds, float opacity)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (!state || !state->ctx || node.values.empty()) return;
    const float inset = std::min(node.padding,
        std::max(0.0f, std::min(
            bounds.right - bounds.left,
            bounds.bottom - bounds.top) * 0.5f));
    const D2D1_RECT_F rect = D2D1::RectF(
        bounds.left + inset, bounds.top + inset,
        bounds.right - inset, bounds.bottom - inset);
    const float width = std::max(0.0f, rect.right - rect.left);
    const float height = std::max(0.0f, rect.bottom - rect.top);
    if (width <= 0.0f || height <= 0.0f) return;

    float minimum = 0.0f;
    float maximum = 1.0f;
    if (node.seriesMinimum && node.seriesMaximum)
    {
        minimum = *node.seriesMinimum;
        maximum = *node.seriesMaximum;
    }
    else if (node.type == ViewNodeType::Waveform)
    {
        minimum = -1.0f;
        maximum = 1.0f;
    }
    else if (node.type == ViewNodeType::Spectrum)
    {
        minimum = 0.0f;
        maximum = 1.0f;
    }
    else
    {
        const auto range = std::minmax_element(
            node.values.begin(), node.values.end());
        minimum = *range.first;
        maximum = *range.second;
        if (node.type == ViewNodeType::BarChart)
        {
            minimum = std::min(0.0f, minimum);
            maximum = std::max(0.0f, maximum);
        }
        if (minimum == maximum)
        {
            const float padding = std::max(1.0f,
                std::abs(minimum) * 0.1f);
            minimum -= padding;
            maximum += padding;
        }
    }
    const float span = maximum - minimum;
    if (!std::isfinite(span) || span <= 0.0f) return;
    const auto valueY = [&](float value) {
        const float normalized = std::clamp(
            (value - minimum) / span, 0.0f, 1.0f);
        return rect.bottom - normalized * height;
    };

    const std::uint32_t color = style.foreground.value_or(0xFFFFFF);
    ID2D1SolidColorBrush* brush = GetCachedBrush(state,
        static_cast<int>(color), opacity * node.fillOpacity);
    ID2D1SolidColorBrush* track = GetCachedBrush(state,
        static_cast<int>(color), opacity * node.trackOpacity * 0.2f);
    if (!brush) return;

    if (track && node.type == ViewNodeType::LineChart)
    {
        for (int line = 1; line < 4; ++line)
        {
            const float y = rect.top + height * line / 4.0f;
            state->ctx->DrawLine(D2D1::Point2F(rect.left, y),
                D2D1::Point2F(rect.right, y), track, 1.0f);
        }
    }
    else if (track && (node.type == ViewNodeType::Waveform ||
            node.type == ViewNodeType::BarChart) &&
        minimum <= 0.0f && maximum >= 0.0f)
    {
        const float y = valueY(0.0f);
        state->ctx->DrawLine(D2D1::Point2F(rect.left, y),
            D2D1::Point2F(rect.right, y), track, 1.0f);
    }

    if (node.type == ViewNodeType::BarChart ||
        node.type == ViewNodeType::Spectrum)
    {
        const float slot = width /
            static_cast<float>(node.values.size());
        const float gap = std::min(2.0f, slot * 0.2f);
        const float baseline = node.type == ViewNodeType::Spectrum
            ? rect.bottom : valueY(0.0f);
        for (std::size_t index = 0; index < node.values.size(); ++index)
        {
            const float y = valueY(node.values[index]);
            const float left = rect.left + slot *
                static_cast<float>(index) + gap * 0.5f;
            const float right = std::max(left,
                rect.left + slot * static_cast<float>(index + 1) -
                    gap * 0.5f);
            state->ctx->FillRectangle(D2D1::RectF(
                left, std::min(y, baseline), right,
                std::max(y, baseline)), brush);
        }
        return;
    }

    const float stroke = std::min(node.thickness,
        std::max(0.5f, std::min(width, height)));
    if (node.values.size() == 1)
    {
        state->ctx->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(rect.left + width * 0.5f,
                valueY(node.values.front())),
            stroke, stroke), brush);
        return;
    }
    const float step = width /
        static_cast<float>(node.values.size() - 1);
    D2D1_POINT_2F previous = D2D1::Point2F(
        rect.left, valueY(node.values.front()));
    for (std::size_t index = 1; index < node.values.size(); ++index)
    {
        const D2D1_POINT_2F next = D2D1::Point2F(
            rect.left + step * static_cast<float>(index),
            valueY(node.values[index]));
        state->ctx->DrawLine(previous, next, brush, stroke);
        previous = next;
    }
}

static bool IsDeclarativeInputNode(
    snowdesktop::widget_runtime::ViewNodeType type) noexcept
{
    using snowdesktop::widget_runtime::ViewNodeType;
    return type == ViewNodeType::TextInput ||
        type == ViewNodeType::TextArea ||
        type == ViewNodeType::SearchBox ||
        type == ViewNodeType::NumberInput;
}

static std::string DeclarativeInputValue(
    const snowdesktop::widget_runtime::ViewNode& node)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (node.type != ViewNodeType::NumberInput)
        return node.inputValue;
    std::string value = std::to_string(node.value);
    while (value.size() > 1 && value.back() == '0') value.pop_back();
    if (!value.empty() && value.back() == '.') value.pop_back();
    return value;
}

static void DrawDeclarativeViewInput(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (!state || !state->ctx) return;
    const bool multiline = node.type == ViewNodeType::TextArea;
    const float x = node.frame.x;
    const float y = node.frame.y;
    const float width = node.frame.width;
    const float height = node.frame.height;
    const float padding = std::min(node.padding,
        std::max(0.0f, std::min(width, height) * 0.5f));
    bool focused = false;
    size_t cursor = 0;
    size_t selectionAnchor = 0;
    std::wstring focusedText;
    std::wstring compositionText;
    size_t compositionCursor = 0;
    if (state->engine)
        focused = state->engine->RuntimeGetFocusedHostInput(
            state->currentWidgetId, node.key, focusedText, cursor,
            selectionAnchor, compositionText, compositionCursor);
    const bool widgetSelected = state->engine &&
        state->engine->RuntimeIsWidgetSelected(state->currentWidgetId);
    const std::uint32_t background = style.background.value_or(0xFFFFFF);
    const std::uint32_t foreground = style.foreground.value_or(0xFFFFFF);
    const std::uint32_t border = focused && !widgetSelected
        ? 0x64A8FF : style.borderColor.value_or(0xFFFFFF);
    const float radius = std::max(0.0f,
        style.cornerRadius.value_or(6.0f));
    DrawHostRect(state, x, y, width, height,
        static_cast<int>(background), radius,
        opacity * (style.background ? 1.0f : (focused ? 0.10f : 0.07f)));
    DrawHostStrokeRect(state, x, y, width, height,
        static_cast<int>(border), radius,
        std::max(1.0f, style.borderWidth.value_or(1.0f)),
        opacity * (focused && !widgetSelected
            ? 0.72f : (style.borderColor ? 1.0f : 0.18f)));

    const HostInputDisplayText focusedDisplay =
        BuildHostInputDisplayText(focusedText, cursor,
            selectionAnchor, compositionText, compositionCursor);
    const std::wstring controlledText = Utf8ToWideLocal(
        DeclarativeInputValue(node));
    const bool showingPlaceholder = !focused && controlledText.empty();
    const std::wstring displayText = focused
        ? focusedDisplay.text
        : (showingPlaceholder ? Utf8ToWideLocal(node.placeholder)
            : controlledText);
    const float scrollbarReserve = multiline ? 8.0f : 0.0f;
    const float innerWidth = std::max(1.0f,
        width - padding * 2.0f - scrollbarReserve);
    ComPtr<IDWriteTextLayout> layout = multiline
        ? CreateHostMultilineTextLayout(state, displayText,
            node.fontSize, innerWidth)
        : CreateHostSingleLineTextLayout(state, displayText,
            node.fontSize, innerWidth, height);
    if (!layout) return;

    int scrollOffset = multiline && state->engine
        ? state->engine->RuntimeGetScrollOffset(
            state->currentWidgetId, node.key) : 0;
    const float originX = x + padding;
    const float originY = multiline ? y + padding - scrollOffset : y;
    const D2D1_RECT_F clip = D2D1::RectF(
        state->widgetRect.left + x + padding,
        state->widgetRect.top + y,
        state->widgetRect.left + x + width - padding,
        state->widgetRect.top + y + height);
    state->ctx->PushAxisAlignedClip(
        clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (focused && compositionText.empty() &&
        selectionAnchor != cursor)
    {
        DrawHostTextSelection(state, layout.Get(),
            std::min(selectionAnchor, cursor),
            std::max(selectionAnchor, cursor),
            originX, originY, 0x64A8FF);
    }
    ID2D1SolidColorBrush* textBrush = GetCachedBrush(state,
        static_cast<int>(showingPlaceholder ? 0x94A3B8 : foreground),
        opacity * (node.enabled ? 1.0f : 0.45f));
    if (textBrush)
        state->ctx->DrawTextLayout(D2D1::Point2F(
            state->widgetRect.left + originX,
            state->widgetRect.top + originY), layout.Get(), textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (focused && focusedDisplay.compositionLength > 0)
    {
        DrawHostCompositionUnderline(state, layout.Get(),
            focusedDisplay.compositionStart,
            focusedDisplay.compositionLength,
            originX, originY, static_cast<int>(foreground));
    }
    if (focused)
    {
        const size_t safeCursor = std::min(
            focusedDisplay.cursor, focusedDisplay.text.size());
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
                hitPosition = static_cast<UINT32>(safeCursor);
        }
        float caretX = 0.0f;
        float caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS metrics{};
        if (SUCCEEDED(layout->HitTestTextPosition(hitPosition,
                trailing, &caretX, &caretY, &metrics)))
        {
            DrawHostRect(state, originX + caretX,
                originY + caretY, 1.5f,
                std::max(metrics.height, node.fontSize),
                static_cast<int>(foreground), 0.0f, opacity);
        }
    }
    state->ctx->PopAxisAlignedClip();
}

static void DrawDeclarativeSelect(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity)
{
    if (!state || !state->ctx) return;
    const float radius = std::max(0.0f,
        style.cornerRadius.value_or(6.0f));
    DrawHostRect(state, node.frame.x, node.frame.y,
        node.frame.width, node.frame.height,
        static_cast<int>(style.background.value_or(0xFFFFFF)), radius,
        opacity * (style.background ? 1.0f : 0.07f));
    DrawHostStrokeRect(state, node.frame.x, node.frame.y,
        node.frame.width, node.frame.height,
        static_cast<int>(style.borderColor.value_or(0xFFFFFF)), radius,
        std::max(1.0f, style.borderWidth.value_or(1.0f)),
        opacity * (style.borderColor ? 1.0f : 0.18f));
    std::string label = node.placeholder;
    for (const auto& option : node.options)
        if (option.value == node.selectedValue) label = option.label;
    const float inset = std::max(8.0f, node.padding);
    if (state->dwrite && !label.empty())
    {
        IDWriteTextFormat* format = GetCachedTextFormat(state,
            node.fontSize, state->itemFontWeight, false,
            DWRITE_WORD_WRAPPING_NO_WRAP);
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(style.foreground.value_or(
                node.selectedValue.empty() ? 0x94A3B8 : 0xFFFFFF)),
            opacity * (node.enabled ? 1.0f : 0.45f));
        const std::wstring text = Utf8ToWideLocal(label);
        if (format && brush)
            state->ctx->DrawText(text.data(),
                static_cast<UINT32>(text.size()), format,
                D2D1::RectF(state->widgetRect.left + node.frame.x + inset,
                    state->widgetRect.top + node.frame.y,
                    state->widgetRect.left + node.frame.x +
                        node.frame.width - 28.0f,
                    state->widgetRect.top + node.frame.y + node.frame.height),
                brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(style.foreground.value_or(0xFFFFFF)),
            opacity * 0.8f))
    {
        const float cx = state->widgetRect.left + node.frame.x +
            node.frame.width - 14.0f;
        const float cy = state->widgetRect.top + node.frame.y +
            node.frame.height * 0.5f;
        const float direction = node.expanded ? -1.0f : 1.0f;
        state->ctx->DrawLine(D2D1::Point2F(cx - 4.0f,
                cy - 2.0f * direction),
            D2D1::Point2F(cx, cy + 2.0f * direction), brush, 1.5f);
        state->ctx->DrawLine(D2D1::Point2F(cx,
                cy + 2.0f * direction),
            D2D1::Point2F(cx + 4.0f,
                cy - 2.0f * direction), brush, 1.5f);
    }
}

static void SetViewTextLayoutAlignment(IDWriteTextLayout* layout,
    snowdesktop::widget_runtime::ViewTextAlignment alignment)
{
    using snowdesktop::widget_runtime::ViewTextAlignment;
    if (!layout) return;
    if (alignment == ViewTextAlignment::Center)
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else if (alignment == ViewTextAlignment::End)
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    else
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void DrawWidgetStyledText(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity)
{
    if (!state || !state->ctx || !state->dwrite || node.spans.empty())
        return;
    std::optional<std::wstring> privateFontPath;
    if (!node.fontResourceName.empty() && state->engine)
        privateFontPath = state->engine->RuntimeResolvePackageResource(
            state->currentWidgetId, node.fontResourceName, "font");
    IDWriteTextFormat* format = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, node.fontSize,
            node.bold ? DWRITE_FONT_WEIGHT_BOLD : state->itemFontWeight,
            false, DWRITE_WORD_WRAPPING_WRAP, false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    if (!format) return;

    std::wstring text;
    std::vector<std::pair<const snowdesktop::widget_runtime::ViewTextSpan*,
        DWRITE_TEXT_RANGE>> ranges;
    ranges.reserve(node.spans.size());
    for (const auto& span : node.spans)
    {
        const std::wstring part = Utf8ToWideLocal(span.text);
        const UINT32 start = static_cast<UINT32>(text.size());
        text += part;
        ranges.emplace_back(&span,
            DWRITE_TEXT_RANGE{ start, static_cast<UINT32>(part.size()) });
    }
    if (text.empty()) return;
    const float inset = std::min(node.padding,
        std::max(0.0f,
            std::min(node.frame.width, node.frame.height) * 0.5f));
    const float width = std::max(0.0f, node.frame.width - inset * 2.0f);
    const float height = std::max(0.0f, node.frame.height - inset * 2.0f);
    ComPtr<IDWriteTextLayout> layout;
    if (width <= 0.0f || height <= 0.0f || FAILED(
            state->dwrite->CreateTextLayout(text.data(),
                static_cast<UINT32>(text.size()), format, width, height,
                &layout)) || !layout)
        return;
    SetViewTextLayoutAlignment(layout.Get(), node.textAlign);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (const auto& [span, range] : ranges)
    {
        if (span->fontSize) layout->SetFontSize(*span->fontSize, range);
        if (span->bold)
            layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
        if (span->italic)
            layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);
        if (span->underline) layout->SetUnderline(TRUE, range);
        if (span->strikethrough) layout->SetStrikethrough(TRUE, range);
        if (span->foreground)
        {
            if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                    static_cast<int>(*span->foreground), opacity))
                layout->SetDrawingEffect(brush, range);
        }
    }
    ID2D1SolidColorBrush* brush = GetCachedBrush(state,
        static_cast<int>(style.foreground.value_or(0xFFFFFF)), opacity);
    if (brush)
        state->ctx->DrawTextLayout(D2D1::Point2F(
            state->widgetRect.left + node.frame.x + inset,
            state->widgetRect.top + node.frame.y + inset),
            layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void DrawWidgetMonthCalendar(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::ViewStyle& style, float opacity,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions)
{
    using snowdesktop::widget_runtime::ViewMonthCalendarCellFrame;
    using snowdesktop::widget_runtime::ViewMonthCalendarWeekdayFrame;
    using snowdesktop::widget_runtime::ViewStyle;
    if (!state || !state->ctx || !state->dwrite) return;
    std::array<snowdesktop::widget_runtime::ViewMonthCalendarCell, 42> cells;
    std::string error;
    if (!snowdesktop::widget_runtime::BuildViewMonthCalendarCells(
            node, cells, error))
        return;

    std::optional<std::wstring> privateFontPath;
    if (!node.fontResourceName.empty() && state->engine)
        privateFontPath = state->engine->RuntimeResolvePackageResource(
            state->currentWidgetId, node.fontResourceName, "font");
    IDWriteTextFormat* dayFormat = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, node.fontSize,
            node.bold ? DWRITE_FONT_WEIGHT_BOLD : state->itemFontWeight,
            false, DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    IDWriteTextFormat* weekdayFormat = node.fontResourceName.empty() ||
            privateFontPath
        ? GetCachedTextFormat(state, std::max(8.0f, node.fontSize * 0.78f),
            DWRITE_FONT_WEIGHT_SEMI_BOLD, false,
            DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
            privateFontPath ? &*privateFontPath : nullptr)
        : nullptr;
    if (!dayFormat || !weekdayFormat) return;

    const auto drawCentered = [&](std::wstring_view text,
            const snowdesktop::widget_runtime::ViewRect& frame,
            IDWriteTextFormat* format, std::uint32_t color, float alpha) {
        if (text.empty() || frame.width <= 0.0f || frame.height <= 0.0f)
            return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(state->dwrite->CreateTextLayout(text.data(),
                static_cast<UINT32>(text.size()), format,
                frame.width, frame.height, &layout)) || !layout)
            return;
        layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                static_cast<int>(color), alpha))
            state->ctx->DrawTextLayout(D2D1::Point2F(
                state->widgetRect.left + frame.x,
                state->widgetRect.top + frame.y), layout.Get(), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const std::uint32_t baseForeground = style.foreground.value_or(0xFFFFFF);
    for (std::size_t column = 0; column < 7; ++column)
    {
        const std::size_t labelIndex = static_cast<std::size_t>(
            (node.firstDayOfWeek - 1 + static_cast<int>(column)) % 7);
        drawCentered(Utf8ToWideLocal(node.weekdayLabels[labelIndex]),
            ViewMonthCalendarWeekdayFrame(node, column), weekdayFormat,
            baseForeground, opacity * 0.62f);
    }

    for (std::size_t index = 0; index < cells.size(); ++index)
    {
        const auto& cell = cells[index];
        if (!cell.currentMonth && !node.showAdjacentDates) continue;
        const auto frame = ViewMonthCalendarCellFrame(node, index);
        const std::string key = node.key + "/" + cell.date;
        const bool hovered = regions.IsHovered(key);
        const bool pressed = regions.IsPressed(key);
        ViewStyle cellStyle;
        cellStyle.foreground = baseForeground;
        cellStyle.opacity = style.opacity;
        if (!cell.currentMonth)
            OverlayViewStyle(cellStyle, node.adjacentStyle);
        if (cell.today) OverlayViewStyle(cellStyle, node.todayStyle);
        if (cell.selected)
            OverlayViewStyle(cellStyle, node.selectedStyle);
        if (hovered) OverlayViewStyle(cellStyle, node.hoverStyle);
        if (pressed) OverlayViewStyle(cellStyle, node.pressedStyle);
        const float cellOpacity = opacity * std::clamp(
            cellStyle.opacity.value_or(1.0f), 0.0f, 1.0f) *
            (node.enabled ? 1.0f : 0.42f) *
            (cell.currentMonth ? 1.0f :
                (node.adjacentStyle.opacity ? 1.0f : 0.42f));
        const float diameter = std::max(0.0f,
            std::min(frame.width, frame.height) * 0.76f);
        const float centerX = state->widgetRect.left + frame.x +
            frame.width * 0.5f;
        const float centerY = state->widgetRect.top + frame.y +
            frame.height * 0.46f;
        const D2D1_ELLIPSE indicator = D2D1::Ellipse(
            D2D1::Point2F(centerX, centerY), diameter * 0.5f,
            diameter * 0.5f);
        if (cell.selected || cellStyle.background || hovered || pressed)
        {
            const std::uint32_t fillColor = cellStyle.background.value_or(
                cell.selected ? 0x4C9AFF : 0xFFFFFF);
            const float fillAlpha = cellOpacity *
                (cellStyle.background || cell.selected ? 1.0f :
                    (pressed ? 0.18f : 0.11f));
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor), fillAlpha))
                state->ctx->FillEllipse(indicator, fill);
        }
        if (cell.today && !cell.selected)
        {
            const std::uint32_t borderColor =
                node.todayStyle.borderColor.value_or(baseForeground);
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    static_cast<int>(borderColor), cellOpacity * 0.78f))
                state->ctx->DrawEllipse(indicator, border,
                    std::max(1.0f,
                        node.todayStyle.borderWidth.value_or(1.25f)));
        }
        const auto textFrame = snowdesktop::widget_runtime::ViewRect{
            frame.x + (frame.width - diameter) * 0.5f,
            frame.y + frame.height * 0.46f - diameter * 0.5f,
            diameter, diameter };
        drawCentered(std::to_wstring(cell.day), textFrame, dayFormat,
            cellStyle.foreground.value_or(baseForeground), cellOpacity);
        if (cell.hasEvent)
        {
            ViewStyle eventStyle = cellStyle;
            OverlayViewStyle(eventStyle, node.eventStyle);
            const float radius = std::max(1.0f,
                std::min(frame.width, frame.height) * 0.035f);
            if (ID2D1SolidColorBrush* marker = GetCachedBrush(state,
                    static_cast<int>(eventStyle.foreground.value_or(
                        baseForeground)), cellOpacity))
                state->ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(
                    centerX, state->widgetRect.top + frame.y +
                        frame.height - radius * 2.0f), radius, radius),
                    marker);
        }
    }
}

static void DrawWidgetViewNode(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    using snowdesktop::widget_runtime::ViewShapeKind;
    using snowdesktop::widget_runtime::ViewIconFont;
    using snowdesktop::widget_runtime::ViewImageAlignment;
    using snowdesktop::widget_runtime::ViewImageFit;
    using snowdesktop::widget_runtime::ViewImageInterpolation;
    using snowdesktop::widget_runtime::ViewOrientation;
    using snowdesktop::widget_runtime::ViewTextAlignment;
    if (!state || !state->ctx || !node.visible ||
        node.frame.width <= 0.0f || node.frame.height <= 0.0f)
        return;

    const bool hovered = regions.IsHovered(node.key);
    const bool pressed = regions.IsPressed(node.key);
    auto style = ResolveViewStyle(node, hovered, pressed);
    if (node.type == ViewNodeType::Link)
    {
        if (!style.foreground) style.foreground = 0x72C7FF;
        if (pressed && !node.pressedStyle.opacity)
            style.opacity = 0.65f;
        else if (hovered && !node.hoverStyle.opacity)
            style.opacity = 0.82f;
    }
    const bool badgeNode = node.type == ViewNodeType::Badge;
    const float opacity = std::clamp(
        style.opacity.value_or(1.0f), 0.0f, 1.0f);
    const float radius = std::max(0.0f,
        style.cornerRadius.value_or(badgeNode
            ? node.frame.height * 0.5f : 0.0f));
    const D2D1_RECT_F rect = D2D1::RectF(
        state->widgetRect.left + node.frame.x,
        state->widgetRect.top + node.frame.y,
        state->widgetRect.left + node.frame.x + node.frame.width,
        state->widgetRect.top + node.frame.y + node.frame.height);

    const bool buttonNode = node.type == ViewNodeType::Button ||
        node.type == ViewNodeType::IconButton;
    const bool checkControlNode = node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox;
    const bool iconNode = node.type == ViewNodeType::Icon ||
        node.type == ViewNodeType::IconButton;
    const bool specialGeometry = node.type == ViewNodeType::Shape ||
        node.type == ViewNodeType::Divider ||
        node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::ProgressRing ||
        node.type == ViewNodeType::Meter ||
        node.type == ViewNodeType::Icon || checkControlNode ||
        node.type == ViewNodeType::RadioGroup ||
        node.type == ViewNodeType::Slider ||
        IsDeclarativeInputNode(node.type) ||
        node.type == ViewNodeType::Select;
    const bool implicitSurfaceBackground = !style.background &&
        (buttonNode || badgeNode);
    std::optional<std::uint32_t> background = style.background;
    if (!background && (buttonNode || badgeNode))
        background = 0xFFFFFF;
    if (background && !specialGeometry)
    {
        const float defaultButtonAlpha =
            implicitSurfaceBackground ? 0.12f : 1.0f;
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(*background), opacity * defaultButtonAlpha);
        if (brush)
        {
            if (radius > 0.0f)
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(rect, radius, radius), brush);
            else
                state->ctx->FillRectangle(rect, brush);
        }
    }
    const float borderWidth = std::max(0.0f,
        style.borderWidth.value_or(0.0f));
    if (borderWidth > 0.0f && style.borderColor && !specialGeometry)
    {
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(*style.borderColor), opacity);
        if (brush)
        {
            if (radius > 0.0f)
                state->ctx->DrawRoundedRectangle(
                    D2D1::RoundedRect(rect, radius, radius), brush,
                    borderWidth);
            else
                state->ctx->DrawRectangle(rect, brush, borderWidth);
        }
    }

    if (IsDeclarativeInputNode(node.type))
    {
        DrawDeclarativeViewInput(state, node, style, opacity);
        return;
    }
    if (node.type == ViewNodeType::Select)
    {
        DrawDeclarativeSelect(state, node, style, opacity);
        return;
    }
    if (node.type == ViewNodeType::StyledText)
    {
        DrawWidgetStyledText(state, node, style, opacity);
        return;
    }
    if (node.type == ViewNodeType::MonthCalendar)
    {
        DrawWidgetMonthCalendar(state, node, style, opacity, regions);
        return;
    }
    if (node.type == ViewNodeType::Toggle)
    {
        const float inset = std::min(node.padding,
            std::max(0.0f, node.frame.width * 0.5f));
        const float trackWidth = std::min(36.0f,
            std::max(0.0f, node.frame.width - inset * 2.0f));
        const float trackHeight = std::min(20.0f, node.frame.height);
        const float right = std::max(rect.left, rect.right - inset);
        const float left = std::max(rect.left, right - trackWidth);
        const float top = (rect.top + rect.bottom - trackHeight) * 0.5f;
        const D2D1_RECT_F trackRect = D2D1::RectF(
            left, top, right, top + trackHeight);
        const std::uint32_t trackColor = style.background.value_or(
            node.checked ? 0x4C9AFF : 0xFFFFFF);
        const float trackAlpha = opacity *
            (style.background || node.checked ? 1.0f : 0.18f);
        if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
                static_cast<int>(trackColor), trackAlpha))
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(trackRect,
                    trackHeight * 0.5f, trackHeight * 0.5f), track);
        if (borderWidth > 0.0f && style.borderColor)
        {
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    static_cast<int>(*style.borderColor), opacity))
                state->ctx->DrawRoundedRectangle(
                    D2D1::RoundedRect(trackRect,
                        trackHeight * 0.5f, trackHeight * 0.5f), border,
                    borderWidth);
        }
        const float knobRadius = std::max(0.0f, std::min(
            7.0f, std::min(trackHeight * 0.35f, trackWidth * 0.25f)));
        const float knobInset = std::min(trackWidth * 0.5f,
            knobRadius + std::min(3.0f, trackWidth * 0.1f));
        const float knobX = node.checked
            ? trackRect.right - knobInset : trackRect.left + knobInset;
        if (ID2D1SolidColorBrush* knob = GetCachedBrush(state,
                static_cast<int>(style.foreground.value_or(0xFFFFFF)),
                opacity))
            state->ctx->FillEllipse(D2D1::Ellipse(
                D2D1::Point2F(knobX,
                    (trackRect.top + trackRect.bottom) * 0.5f),
                knobRadius, knobRadius), knob);
    }
    else if (node.type == ViewNodeType::Checkbox)
    {
        const float inset = std::min(node.padding,
            std::max(0.0f, node.frame.width * 0.5f));
        const float boxSize = std::min(18.0f, std::max(0.0f,
            std::min(node.frame.width - inset * 2.0f,
                node.frame.height)));
        const float left = rect.left + inset;
        const float top = (rect.top + rect.bottom - boxSize) * 0.5f;
        const D2D1_RECT_F box = D2D1::RectF(
            left, top, left + boxSize, top + boxSize);
        if (node.checked || style.background)
        {
            const std::uint32_t fillColor = style.background.value_or(
                node.checked ? 0x4C9AFF : 0xFFFFFF);
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor), opacity))
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(box,
                        std::min(3.0f, boxSize * 0.2f),
                        std::min(3.0f, boxSize * 0.2f)), fill);
        }
        const std::uint32_t borderColor = style.borderColor.value_or(
            style.foreground.value_or(0xFFFFFF));
        if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                static_cast<int>(borderColor), opacity *
                    (style.borderColor ? 1.0f : 0.55f)))
            state->ctx->DrawRoundedRectangle(
                D2D1::RoundedRect(box,
                    std::min(3.0f, boxSize * 0.2f),
                    std::min(3.0f, boxSize * 0.2f)), border,
                std::max(1.0f, borderWidth));
        if (node.checked)
        {
            if (ID2D1SolidColorBrush* mark = GetCachedBrush(state,
                    static_cast<int>(style.foreground.value_or(0xFFFFFF)),
                    opacity))
            {
                state->ctx->DrawLine(D2D1::Point2F(
                        left + boxSize * 0.22f, top + boxSize * 0.50f),
                    D2D1::Point2F(
                        left + boxSize * 0.42f, top + boxSize * 0.72f),
                    mark, std::min(2.0f, boxSize * 0.12f));
                state->ctx->DrawLine(D2D1::Point2F(
                        left + boxSize * 0.42f, top + boxSize * 0.72f),
                    D2D1::Point2F(
                        left + boxSize * 0.81f, top + boxSize * 0.31f),
                    mark, std::min(2.0f, boxSize * 0.12f));
            }
        }
    }
    else if (node.type == ViewNodeType::RadioGroup)
    {
        std::optional<std::wstring> privateFontPath;
        if (!node.fontResourceName.empty() && state->engine)
            privateFontPath = state->engine->RuntimeResolvePackageResource(
                state->currentWidgetId, node.fontResourceName, "font");
        IDWriteTextFormat* format = node.fontResourceName.empty() ||
                privateFontPath
            ? GetCachedTextFormat(state, node.fontSize,
                node.bold ? DWRITE_FONT_WEIGHT_BOLD : state->itemFontWeight,
                false, DWRITE_WORD_WRAPPING_NO_WRAP, false, false, false,
                privateFontPath ? &*privateFontPath : nullptr)
            : nullptr;
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const std::string optionKey = node.key + "/" + option.key;
            const bool optionHovered = regions.IsHovered(optionKey);
            const bool optionPressed = regions.IsPressed(optionKey);
            const bool selected = option.value == node.selectedValue;
            const auto optionStyle = ResolveViewStyle(
                node, optionHovered, optionPressed, selected);
            const float optionOpacity = std::clamp(
                optionStyle.opacity.value_or(1.0f), 0.0f, 1.0f) *
                (node.enabled && option.enabled ? 1.0f : 0.42f);
            const auto frame = snowdesktop::widget_runtime::
                ViewRadioOptionFrame(node, index);
            const D2D1_RECT_F optionRect = D2D1::RectF(
                state->widgetRect.left + frame.x,
                state->widgetRect.top + frame.y,
                state->widgetRect.left + frame.x + frame.width,
                state->widgetRect.top + frame.y + frame.height);
            const float optionRadius = std::max(0.0f,
                optionStyle.cornerRadius.value_or(4.0f));
            if (optionStyle.background || optionHovered || optionPressed)
            {
                const std::uint32_t color =
                    optionStyle.background.value_or(0xFFFFFF);
                const float alpha = optionOpacity *
                    (optionStyle.background ? 1.0f :
                        (optionPressed ? 0.16f : 0.10f));
                if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                        static_cast<int>(color), alpha))
                    state->ctx->FillRoundedRectangle(
                        D2D1::RoundedRect(optionRect,
                            optionRadius, optionRadius), brush);
            }
            const float indicatorRadius = std::min(8.0f,
                std::max(0.0f, frame.height * 0.28f));
            const float indicatorX = optionRect.left +
                std::min(frame.width * 0.5f, node.padding + 10.0f);
            const float indicatorY =
                (optionRect.top + optionRect.bottom) * 0.5f;
            const std::uint32_t foreground =
                optionStyle.foreground.value_or(0xFFFFFF);
            if (ID2D1SolidColorBrush* outline = GetCachedBrush(state,
                    static_cast<int>(selected ? 0x4C9AFF : foreground),
                    optionOpacity * (selected ? 1.0f : 0.62f)))
                state->ctx->DrawEllipse(D2D1::Ellipse(
                    D2D1::Point2F(indicatorX, indicatorY),
                    indicatorRadius, indicatorRadius), outline,
                    std::max(1.0f,
                        optionStyle.borderWidth.value_or(1.5f)));
            if (selected)
            {
                if (ID2D1SolidColorBrush* mark = GetCachedBrush(state,
                        static_cast<int>(optionStyle.foreground.value_or(
                            0x4C9AFF)), optionOpacity))
                    state->ctx->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(indicatorX, indicatorY),
                        indicatorRadius * 0.5f,
                        indicatorRadius * 0.5f), mark);
            }
            if (format && state->dwrite)
            {
                const std::wstring label = Utf8ToWideLocal(option.label);
                ComPtr<IDWriteTextLayout> layout;
                const float textLeft = indicatorX + indicatorRadius + 8.0f;
                const float textWidth = std::max(0.0f,
                    optionRect.right - textLeft - node.padding);
                if (!label.empty() && SUCCEEDED(
                        state->dwrite->CreateTextLayout(label.data(),
                            static_cast<UINT32>(label.size()), format,
                            textWidth, frame.height, &layout)) && layout)
                {
                    layout->SetParagraphAlignment(
                        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DWRITE_TRIMMING trimming{};
                    trimming.granularity =
                        DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                    ComPtr<IDWriteInlineObject> ellipsis;
                    if (SUCCEEDED(state->dwrite->
                            CreateEllipsisTrimmingSign(format, &ellipsis)) &&
                        ellipsis)
                        layout->SetTrimming(&trimming, ellipsis.Get());
                    if (ID2D1SolidColorBrush* textBrush = GetCachedBrush(
                            state, static_cast<int>(foreground),
                            optionOpacity))
                        state->ctx->DrawTextLayout(D2D1::Point2F(
                            textLeft, optionRect.top), layout.Get(),
                            textBrush);
                }
            }
        }
    }
    else if (node.type == ViewNodeType::Slider)
    {
        const bool vertical = node.orientation == ViewOrientation::Vertical;
        const float span = node.maximum - node.minimum;
        const float normalized = span > 0.0f
            ? std::clamp((node.value - node.minimum) / span, 0.0f, 1.0f)
            : 0.0f;
        const float thumbRadius = std::min(8.0f, std::max(3.0f,
            std::min(node.frame.width, node.frame.height) * 0.3f));
        const float inset = std::min(node.padding + thumbRadius,
            std::max(0.0f, (vertical ? node.frame.height :
                node.frame.width) * 0.5f));
        const float trackThickness = std::min(4.0f,
            vertical ? node.frame.width : node.frame.height);
        D2D1_RECT_F trackRect{};
        if (vertical)
        {
            const float x = (rect.left + rect.right) * 0.5f;
            trackRect = D2D1::RectF(x - trackThickness * 0.5f,
                rect.top + inset, x + trackThickness * 0.5f,
                rect.bottom - inset);
        }
        else
        {
            const float y = (rect.top + rect.bottom) * 0.5f;
            trackRect = D2D1::RectF(rect.left + inset,
                y - trackThickness * 0.5f, rect.right - inset,
                y + trackThickness * 0.5f);
        }
        const float trackRadius = trackThickness * 0.5f;
        if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
                static_cast<int>(style.background.value_or(0xFFFFFF)),
                opacity * (style.background ? 1.0f : 0.22f)))
            state->ctx->FillRoundedRectangle(D2D1::RoundedRect(
                trackRect, trackRadius, trackRadius), track);
        D2D1_RECT_F fillRect = trackRect;
        D2D1_POINT_2F thumb{};
        if (vertical)
        {
            const float y = trackRect.bottom - normalized *
                (trackRect.bottom - trackRect.top);
            fillRect.top = y;
            thumb = D2D1::Point2F(
                (trackRect.left + trackRect.right) * 0.5f, y);
        }
        else
        {
            const float x = trackRect.left + normalized *
                (trackRect.right - trackRect.left);
            fillRect.right = x;
            thumb = D2D1::Point2F(x,
                (trackRect.top + trackRect.bottom) * 0.5f);
        }
        const std::uint32_t fillColor =
            style.foreground.value_or(0x4C9AFF);
        if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                static_cast<int>(fillColor), opacity))
        {
            state->ctx->FillRoundedRectangle(D2D1::RoundedRect(
                fillRect, trackRadius, trackRadius), fill);
            state->ctx->FillEllipse(D2D1::Ellipse(
                thumb, thumbRadius, thumbRadius), fill);
        }
        if (borderWidth > 0.0f && style.borderColor)
        {
            if (ID2D1SolidColorBrush* outline = GetCachedBrush(state,
                    static_cast<int>(*style.borderColor), opacity))
                state->ctx->DrawEllipse(D2D1::Ellipse(
                    thumb, thumbRadius, thumbRadius), outline, borderWidth);
        }
    }
    else if (node.type == ViewNodeType::Divider)
    {
        const std::uint32_t dividerColor = style.foreground.value_or(
            style.background.value_or(0xFFFFFF));
        if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                static_cast<int>(dividerColor), opacity))
        {
            const float stroke = std::min(node.thickness,
                std::max(0.5f, node.orientation ==
                        ViewOrientation::Horizontal
                    ? node.frame.height : node.frame.width));
            if (node.orientation == ViewOrientation::Horizontal)
            {
                const float y = (rect.top + rect.bottom) * 0.5f;
                state->ctx->DrawLine(D2D1::Point2F(rect.left, y),
                    D2D1::Point2F(rect.right, y), brush, stroke);
            }
            else
            {
                const float x = (rect.left + rect.right) * 0.5f;
                state->ctx->DrawLine(D2D1::Point2F(x, rect.top),
                    D2D1::Point2F(x, rect.bottom), brush, stroke);
            }
        }
    }
    else if (node.type == ViewNodeType::Shape)
    {
        const bool ellipse = node.shapeKind == ViewShapeKind::Circle ||
            node.shapeKind == ViewShapeKind::Ellipse;
        const float centerX = (rect.left + rect.right) * 0.5f;
        const float centerY = (rect.top + rect.bottom) * 0.5f;
        float radiusX = (rect.right - rect.left) * 0.5f;
        float radiusY = (rect.bottom - rect.top) * 0.5f;
        if (node.shapeKind == ViewShapeKind::Circle)
            radiusX = radiusY = std::min(radiusX, radiusY);
        const D2D1_ELLIPSE ellipseGeometry = D2D1::Ellipse(
            D2D1::Point2F(centerX, centerY), radiusX, radiusY);
        const float shapeRadius = node.shapeKind ==
                ViewShapeKind::RoundedRectangle
            ? std::max(0.0f, style.cornerRadius.value_or(4.0f))
            : radius;
        if (style.background)
        {
            if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                    static_cast<int>(*style.background), opacity))
            {
                if (ellipse) state->ctx->FillEllipse(ellipseGeometry, brush);
                else if (shapeRadius > 0.0f)
                    state->ctx->FillRoundedRectangle(
                        D2D1::RoundedRect(rect, shapeRadius, shapeRadius),
                        brush);
                else state->ctx->FillRectangle(rect, brush);
            }
        }
        if (borderWidth > 0.0f && style.borderColor)
        {
            if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                    static_cast<int>(*style.borderColor), opacity))
            {
                if (ellipse)
                    state->ctx->DrawEllipse(
                        ellipseGeometry, brush, borderWidth);
                else if (shapeRadius > 0.0f)
                    state->ctx->DrawRoundedRectangle(
                        D2D1::RoundedRect(rect, shapeRadius, shapeRadius),
                        brush, borderWidth);
                else state->ctx->DrawRectangle(rect, brush, borderWidth);
            }
        }
    }
    else if (node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::Meter)
    {
        const std::uint32_t trackColor =
            style.background.value_or(0xFFFFFF);
        const std::uint32_t fillColor =
            style.foreground.value_or(0xFFFFFF);
        const float barRadius = std::max(0.0f,
            style.cornerRadius.value_or(
                (rect.bottom - rect.top) * 0.5f));
        if (ID2D1SolidColorBrush* track = GetCachedBrush(state,
                static_cast<int>(trackColor),
                opacity * node.trackOpacity))
            state->ctx->FillRoundedRectangle(
                D2D1::RoundedRect(rect, barRadius, barRadius), track);
        if (node.value > 0.0f)
        {
            D2D1_RECT_F fillRect = rect;
            fillRect.right = fillRect.left +
                (fillRect.right - fillRect.left) * node.value;
            if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                    static_cast<int>(fillColor),
                    opacity * node.fillOpacity))
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(
                        fillRect, barRadius, barRadius), fill);
        }
    }
    else if (node.type == ViewNodeType::ProgressRing)
    {
        DrawWidgetProgressRing(state, rect, node.value,
            std::min(node.thickness,
                std::min(node.frame.width, node.frame.height)),
            style.background.value_or(0xFFFFFF),
            opacity * node.trackOpacity,
            style.foreground.value_or(0xFFFFFF),
            opacity * node.fillOpacity);
    }
    else if (IsWidgetDataSeriesNode(node.type))
    {
        DrawWidgetDataSeries(state, node, style, rect, opacity);
    }
    else if (node.type == ViewNodeType::Image && state->engine)
    {
        ID2D1Bitmap1* bitmap = nullptr;
        if (snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(
                node.imageResourceName))
            bitmap = LoadRuntimeImageBitmap(
                state, state->currentWidgetId, node.imageResourceName);
        else
        {
            const auto path = state->engine->RuntimeResolvePackageResource(
                state->currentWidgetId, node.imageResourceName, "image");
            if (path) bitmap = LoadImageBitmap(state, *path);
        }
        if (bitmap)
        {
            const float inset = std::min(
                node.padding + borderWidth,
                std::max(0.0f, std::min(
                    node.frame.width, node.frame.height) * 0.5f));
            D2D1_RECT_F destination = D2D1::RectF(
                rect.left + inset, rect.top + inset,
                rect.right - inset, rect.bottom - inset);
            const D2D1_SIZE_F size = bitmap->GetSize();
            D2D1_RECT_F source = D2D1::RectF(
                0.0f, 0.0f, size.width, size.height);
            const float targetWidth = std::max(
                0.0f, destination.right - destination.left);
            const float targetHeight = std::max(
                0.0f, destination.bottom - destination.top);
            const auto offset = [alignment = node.imageAlignment](
                    float available, float used) {
                if (alignment == ViewImageAlignment::Center)
                    return std::max(0.0f, (available - used) * 0.5f);
                if (alignment == ViewImageAlignment::End)
                    return std::max(0.0f, available - used);
                return 0.0f;
            };
            if (size.width > 0.0f && size.height > 0.0f &&
                targetWidth > 0.0f && targetHeight > 0.0f)
            {
                if (node.imageFit == ViewImageFit::Contain)
                {
                    const float scale = std::min(
                        targetWidth / size.width,
                        targetHeight / size.height);
                    const float width = size.width * scale;
                    const float height = size.height * scale;
                    destination.left += offset(targetWidth, width);
                    destination.top += offset(targetHeight, height);
                    destination.right = destination.left + width;
                    destination.bottom = destination.top + height;
                }
                else if (node.imageFit == ViewImageFit::Cover)
                {
                    const float targetAspect = targetWidth / targetHeight;
                    const float sourceAspect = size.width / size.height;
                    if (sourceAspect > targetAspect)
                    {
                        const float width = size.height * targetAspect;
                        source.left = offset(size.width, width);
                        source.right = source.left + width;
                    }
                    else
                    {
                        const float height = size.width / targetAspect;
                        source.top = offset(size.height, height);
                        source.bottom = source.top + height;
                    }
                }
                else if (node.imageFit == ViewImageFit::None)
                {
                    const float width = std::min(size.width, targetWidth);
                    const float height = std::min(size.height, targetHeight);
                    destination.left += offset(targetWidth, width);
                    destination.top += offset(targetHeight, height);
                    destination.right = destination.left + width;
                    destination.bottom = destination.top + height;
                    source.right = width;
                    source.bottom = height;
                }
                state->ctx->DrawBitmap(bitmap, destination, opacity,
                    node.imageInterpolation ==
                            ViewImageInterpolation::Nearest
                        ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                        : D2D1_INTERPOLATION_MODE_LINEAR,
                    source);
            }
        }
    }

    if ((node.type == ViewNodeType::Text ||
            node.type == ViewNodeType::Badge ||
            node.type == ViewNodeType::Button ||
            node.type == ViewNodeType::Link || checkControlNode ||
            iconNode) &&
        !node.text.empty() && state->dwrite)
    {
        std::optional<std::wstring> privateFontPath;
        if (!node.fontResourceName.empty() && state->engine)
        {
            privateFontPath = state->engine->RuntimeResolvePackageResource(
                state->currentWidgetId, node.fontResourceName, "font");
        }
        IDWriteTextFormat* format = node.fontResourceName.empty() ||
                privateFontPath
            ? GetCachedTextFormat(state,
            node.fontSize,
            node.bold ? DWRITE_FONT_WEIGHT_BOLD : state->itemFontWeight,
            iconNode, DWRITE_WORD_WRAPPING_NO_WRAP,
            iconNode && node.iconFont == ViewIconFont::FontAwesome,
            iconNode,
            iconNode && node.iconFont == ViewIconFont::Fluent,
            privateFontPath ? &*privateFontPath : nullptr)
            : nullptr;
        const std::uint32_t foreground =
            style.foreground.value_or(0xFFFFFF);
        ID2D1SolidColorBrush* brush = GetCachedBrush(state,
            static_cast<int>(foreground), opacity);
        const std::wstring text = Utf8ToWideLocal(node.text);
        if (format && brush && !text.empty())
        {
            const float textInset = std::min(node.padding,
                std::max(0.0f, std::min(
                    node.frame.width, node.frame.height) * 0.5f));
            float textLeft = rect.left + textInset;
            float textRight = rect.right - textInset;
            if (node.type == ViewNodeType::Checkbox)
                textLeft = std::min(textRight, textLeft + 26.0f);
            else if (node.type == ViewNodeType::Toggle)
                textRight = std::max(textLeft, textRight - 44.0f);
            const float textWidth = std::max(0.0f,
                textRight - textLeft);
            const float textHeight = std::max(0.0f,
                node.frame.height - textInset * 2.0f);
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(state->dwrite->CreateTextLayout(
                    text.data(), static_cast<UINT32>(text.size()),
                    format, textWidth, textHeight,
                    &layout)) && layout)
            {
                if (iconNode || node.textAlign == ViewTextAlignment::Center)
                    layout->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                else if (node.textAlign == ViewTextAlignment::End)
                    layout->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
                else
                    layout->SetTextAlignment(
                        DWRITE_TEXT_ALIGNMENT_LEADING);
                layout->SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (node.type == ViewNodeType::Link)
                    layout->SetUnderline(TRUE,
                        DWRITE_TEXT_RANGE{ 0,
                            static_cast<UINT32>(text.size()) });
                DWRITE_TRIMMING trimming{};
                trimming.granularity =
                    DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(state->dwrite->CreateEllipsisTrimmingSign(
                        format, &ellipsis)) && ellipsis)
                    layout->SetTrimming(&trimming, ellipsis.Get());
                D2D1_POINT_2F origin = D2D1::Point2F(
                    textLeft, rect.top + textInset);
                if (iconNode)
                {
                    DWRITE_OVERHANG_METRICS overhang{};
                    if (SUCCEEDED(layout->GetOverhangMetrics(&overhang)))
                    {
                        origin.x -= (overhang.right - overhang.left) * 0.5f;
                        origin.y -= (overhang.bottom - overhang.top) * 0.5f;
                    }
                }
                state->ctx->DrawTextLayout(origin, layout.Get(), brush);
            }
        }
    }

    if ((node.type == ViewNodeType::Scroll ||
            node.type == ViewNodeType::VirtualList ||
            node.type == ViewNodeType::VirtualGrid) && node.clipFrame)
    {
        const auto& clip = *node.clipFrame;
        state->ctx->PushAxisAlignedClip(D2D1::RectF(
            state->widgetRect.left + clip.x,
            state->widgetRect.top + clip.y,
            state->widgetRect.left + clip.x + clip.width,
            state->widgetRect.top + clip.y + clip.height),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ++state->widgetClipDepth;
        for (const auto& child : node.children)
            DrawWidgetViewNode(state, child, regions);
        state->ctx->PopAxisAlignedClip();
        --state->widgetClipDepth;

        if (node.showScrollbar && node.scrollContentExtent >
                node.scrollViewportExtent &&
            node.scrollViewportExtent > 0.0f)
        {
            const bool vertical = node.orientation ==
                ViewOrientation::Vertical;
            const float trackExtent = node.scrollViewportExtent;
            const float crossExtent = vertical ? clip.width : clip.height;
            const float edgeInset = std::min(2.0f, crossExtent * 0.25f);
            const float barThickness = std::min(3.0f,
                std::max(0.0f, crossExtent - edgeInset));
            const float thumbExtent = std::min(trackExtent,
                std::max(16.0f, trackExtent * trackExtent /
                    node.scrollContentExtent));
            const float maximum = node.scrollContentExtent -
                node.scrollViewportExtent;
            const float position = maximum > 0.0f
                ? (trackExtent - thumbExtent) * node.scrollOffset / maximum
                : 0.0f;
            const std::uint32_t scrollbarColor =
                style.foreground.value_or(0xFFFFFF);
            ID2D1SolidColorBrush* trackBrush = GetCachedBrush(state,
                static_cast<int>(scrollbarColor), opacity * 0.10f);
            ID2D1SolidColorBrush* thumbBrush = GetCachedBrush(state,
                static_cast<int>(scrollbarColor), opacity * 0.48f);
            D2D1_RECT_F trackRect{};
            D2D1_RECT_F thumbRect{};
            if (vertical)
            {
                const float right = state->widgetRect.left + clip.x +
                    clip.width - edgeInset;
                trackRect = D2D1::RectF(right - barThickness,
                    state->widgetRect.top + clip.y, right,
                    state->widgetRect.top + clip.y + clip.height);
                thumbRect = D2D1::RectF(trackRect.left,
                    trackRect.top + position, trackRect.right,
                    trackRect.top + position + thumbExtent);
            }
            else
            {
                const float bottom = state->widgetRect.top + clip.y +
                    clip.height - edgeInset;
                trackRect = D2D1::RectF(
                    state->widgetRect.left + clip.x,
                    bottom - barThickness,
                    state->widgetRect.left + clip.x + clip.width, bottom);
                thumbRect = D2D1::RectF(trackRect.left + position,
                    trackRect.top, trackRect.left + position + thumbExtent,
                    trackRect.bottom);
            }
            if (trackBrush)
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(trackRect, 1.5f, 1.5f), trackBrush);
            if (thumbBrush)
                state->ctx->FillRoundedRectangle(
                    D2D1::RoundedRect(thumbRect, 1.5f, 1.5f), thumbBrush);
        }
        return;
    }
    for (const auto& child : node.children)
        DrawWidgetViewNode(state, child, regions);
}

static std::optional<snowdesktop::widget_runtime::ViewRect>
IntersectViewClips(
    const std::optional<snowdesktop::widget_runtime::ViewRect>& first,
    const snowdesktop::widget_runtime::ViewRect& second)
{
    if (!first) return second;
    const float left = std::max(first->x, second.x);
    const float top = std::max(first->y, second.y);
    const float right = std::min(first->x + first->width,
        second.x + second.width);
    const float bottom = std::min(first->y + first->height,
        second.y + second.height);
    if (right <= left || bottom <= top) return std::nullopt;
    return snowdesktop::widget_runtime::ViewRect{
        left, top, right - left, bottom - top };
}

static void DrawWidgetSelectOverlays(D2DState* state,
    const snowdesktop::widget_runtime::ViewNode& node,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    const std::optional<snowdesktop::widget_runtime::ViewRect>& inheritedClip,
    float viewportHeight)
{
    using snowdesktop::widget_runtime::ViewNodeType;
    if (!state || !state->ctx || !node.visible) return;
    if (node.type == ViewNodeType::Select && node.expanded)
    {
        if (inheritedClip)
        {
            state->ctx->PushAxisAlignedClip(D2D1::RectF(
                state->widgetRect.left + inheritedClip->x,
                state->widgetRect.top + inheritedClip->y,
                state->widgetRect.left + inheritedClip->x +
                    inheritedClip->width,
                state->widgetRect.top + inheritedClip->y +
                    inheritedClip->height),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        IDWriteTextFormat* format = GetCachedTextFormat(state,
            node.fontSize, state->itemFontWeight, false,
            DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
        for (std::size_t index = 0; index < node.options.size(); ++index)
        {
            const auto& option = node.options[index];
            const auto frame = snowdesktop::widget_runtime::
                ViewSelectOptionFrame(node, index, viewportHeight);
            const std::string key = node.key + "/" + option.key;
            const bool hovered = regions.IsHovered(key);
            const bool selected = option.value == node.selectedValue;
            const D2D1_RECT_F rect = D2D1::RectF(
                state->widgetRect.left + frame.x,
                state->widgetRect.top + frame.y,
                state->widgetRect.left + frame.x + frame.width,
                state->widgetRect.top + frame.y + frame.height);
            if (ID2D1SolidColorBrush* background = GetCachedBrush(state,
                    selected ? 0x3478D4 : (hovered ? 0x334155 : 0x172033),
                    option.enabled && node.enabled ? 0.98f : 0.72f))
                state->ctx->FillRectangle(rect, background);
            if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                    0xFFFFFF, 0.16f))
                state->ctx->DrawRectangle(rect, border, 1.0f);
            if (format)
            {
                const std::wstring text = Utf8ToWideLocal(option.label);
                if (ID2D1SolidColorBrush* brush = GetCachedBrush(state,
                        0xFFFFFF, option.enabled && node.enabled
                            ? 1.0f : 0.42f))
                    state->ctx->DrawText(text.data(),
                        static_cast<UINT32>(text.size()), format,
                        D2D1::RectF(rect.left + 10.0f, rect.top,
                            rect.right - 10.0f, rect.bottom), brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        if (inheritedClip) state->ctx->PopAxisAlignedClip();
    }
    std::optional<snowdesktop::widget_runtime::ViewRect> childClip =
        inheritedClip;
    if ((node.type == ViewNodeType::Scroll ||
            node.type == ViewNodeType::VirtualList ||
            node.type == ViewNodeType::VirtualGrid) && node.clipFrame)
        childClip = IntersectViewClips(inheritedClip, *node.clipFrame);
    for (const auto& child : node.children)
        DrawWidgetSelectOverlays(state, child, regions,
            childClip, viewportHeight);
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
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");
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
        if (found->namedTimers.SetVisible(true,
                snowdesktop::widget_runtime::NamedTimerSchedule::Clock::now()))
        {
            RescheduleNamedTimer(*found);
        }
        if (dataBroker_)
        {
            (void)dataBroker_->SetInstanceVisible(
                WidgetWideToUtf8(found->widgetId), true,
                snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
            ApplyWidgetDataBrokerActions();
        }
        InvokeSimpleCallback(*found, "onVisible");
    }
    if (found->lastColumns != columns || found->lastRows != rows)
    {
        found->lastColumns = std::max(1, columns);
        found->lastRows = std::max(1, rows);
        if (found->manifest.apiVersion >= 2)
        {
            const int currentColumns = found->lastColumns;
            const int currentRows = found->lastRows;
            (void)InvokeLifecycleEvent(*found, "resize",
                [currentColumns, currentRows](lua_State* eventState) {
                    lua_pushinteger(eventState, currentColumns);
                    lua_setfield(eventState, -2, "columns");
                    lua_pushinteger(eventState, currentRows);
                    lua_setfield(eventState, -2, "rows");
                });
        }
        else
        {
            lua_rawgeti(state, LUA_REGISTRYINDEX, found->ref);
            if (lua_istable(state, -1))
            {
                lua_getfield(state, -1, "onSizeChanged");
                if (lua_isfunction(state, -1))
                {
                    lua_pushinteger(state, found->lastColumns);
                    lua_pushinteger(state, found->lastRows);
                    if (snowdesktop::lua_runtime::ProtectedCall(
                            state, 2, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(state, -1);
                        RuntimeRecordError(widgetId,
                            error ? error : "(onSizeChanged error)");
                        lua_pop(state, 1);
                    }
                }
                else
                    lua_pop(state, 1);
            }
            lua_pop(state, 1);
        }
    }
    found->lastRenderTime = std::chrono::steady_clock::now();
    found->hostControls.clear();
    if (found->manifest.apiVersion >= 2)
        found->interactionRegions.BeginFrame();
    d2dState_->widgetClipDepth = 0;

    lua_rawgeti(state, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(state, -1))
    {
        if (found->manifest.apiVersion >= 2)
            found->interactionRegions.AbortFrame();
        lua_pop(state, 1);
        return;
    }

    // Inject widgetId global
    {
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(state, widUtf8.c_str());
        lua_setfield(state, -2, "widgetId");
    }

    const int descriptorIndex = lua_absindex(state, -1);
    lua_getfield(state, descriptorIndex, "showTitle");
    const bool showTitle = !lua_isnil(state, -1) &&
        lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, descriptorIndex, "bottomBarHover");
    const bool bottomBarHover = lua_isnil(state, -1) ||
        lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    const int scaledBarHeight = static_cast<int>(std::round(
        static_cast<float>(d2dState_->barHeight) *
        CalculateWidgetCellScale(
            std::max(4, d2dState_->gridCellW),
            std::max(4, d2dState_->gridCellH))));
    const int reservedBarHeight =
        snowdesktop::widget_chrome_rules::ReservedBottomBarHeight(
            showTitle, bottomBarHover, scaledBarHeight);

    lua_getfield(state, descriptorIndex, "view");
    if (lua_isfunction(state, -1))
    {
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        bool accepted = false;
        std::string viewError;
        if (!found->lifecycle.PushRenderArguments(state, pushContext))
        {
            lua_pop(state, 1);
            viewError = "Widget lifecycle is not initialized";
        }
        else if (snowdesktop::lua_runtime::ProtectedCall(
                state, 2, 1) != LUA_OK)
        {
            const char* error = lua_tostring(state, -1);
            viewError = error ? error : "Widget view evaluation failed";
            lua_pop(state, 1);
        }
        else
        {
            snowdesktop::widget_runtime::ViewNode candidate;
            std::vector<snowdesktop::widget_runtime::InteractionRegion>
                regions;
            std::vector<snowdesktop::widget_runtime::ViewScrollViewport>
                scrollViewports;
            std::vector<snowdesktop::widget_runtime::ViewInputControl>
                inputControls;
            if (!snowdesktop::widget_runtime::ParseLuaViewTree(
                    state, -1, candidate, viewError))
            {
                viewError = "view(): " + viewError;
            }
            else if (!snowdesktop::widget_runtime::
                    ValidateAndLayoutViewTree(candidate,
                        d2dState_->widgetRect.right -
                            d2dState_->widgetRect.left,
                        std::max(1.0f,
                            d2dState_->widgetRect.bottom -
                                d2dState_->widgetRect.top -
                                (std::strcmp(d2dState_->surfaceKind,
                                    "panel") == 0
                                    ? 0.0f
                                    : static_cast<float>(
                                        reservedBarHeight))),
                        viewError))
            {
                viewError = "view(): " + viewError;
            }
            else if (!snowdesktop::widget_runtime::ValidateViewLogicalSlots(
                    candidate, found->logicalSlots, viewError))
            {
                viewError = "view(): " + viewError;
            }
            else if (!snowdesktop::widget_runtime::ApplyViewScrollOffsets(
                    candidate,
                    [found](std::string_view key, float) {
                        const auto position = found->scrollOffsets.find(
                            std::string(key));
                        return position == found->scrollOffsets.end()
                            ? 0.0f : static_cast<float>(position->second);
                    }, scrollViewports, viewError))
            {
                viewError = "view(): " + viewError;
            }
            else if (!snowdesktop::widget_runtime::
                    CollectViewInteractionRegions(candidate,
                        regions, viewError))
            {
                viewError = "view(): " + viewError;
            }
            else if (!snowdesktop::widget_runtime::
                    CollectViewInputControls(candidate,
                        inputControls, viewError))
            {
                viewError = "view(): " + viewError;
            }
            else
            {
                accepted = true;
                for (auto& region : regions)
                {
                    if (!found->interactionRegions.Submit(
                            std::move(region), viewError))
                    {
                        accepted = false;
                        viewError = "view(): " + viewError;
                        break;
                    }
                }
                if (accepted && found->hostControls.size() +
                        scrollViewports.size() + inputControls.size() > 128)
                {
                    accepted = false;
                    viewError = "view(): host control limit exceeded (128)";
                }
                if (accepted)
                {
                    std::unordered_set<std::string> hostKeys;
                    for (const auto& control : found->hostControls)
                        hostKeys.insert(control.id);
                    for (const auto& viewport : scrollViewports)
                    {
                        if (!hostKeys.insert(viewport.key).second)
                        {
                            accepted = false;
                            viewError = "view(): duplicate host control key: " +
                                viewport.key;
                            break;
                        }
                    }
                    if (accepted)
                    {
                        for (const auto& input : inputControls)
                        {
                            if (!hostKeys.insert(input.key).second)
                            {
                                accepted = false;
                                viewError = "view(): duplicate host control key: " +
                                    input.key;
                                break;
                            }
                        }
                    }
                }
                if (accepted)
                {
                    for (const auto& viewport : scrollViewports)
                    {
                        LuaWidget::HostControl control;
                        control.type = LuaWidget::HostControl::Type::Scroll;
                        control.id = viewport.key;
                        control.rect = {
                            static_cast<LONG>(std::lround(viewport.frame.x)),
                            static_cast<LONG>(std::lround(viewport.frame.y)),
                            static_cast<LONG>(std::lround(viewport.frame.x +
                                viewport.frame.width)),
                            static_cast<LONG>(std::lround(viewport.frame.y +
                                viewport.frame.height)),
                        };
                        control.horizontal = viewport.orientation ==
                            snowdesktop::widget_runtime::
                                ViewOrientation::Horizontal;
                        const int viewportExtent = std::max(1,
                            static_cast<int>(std::lround(
                                viewport.viewportExtent)));
                        const int contentExtent = std::max(viewportExtent,
                            static_cast<int>(std::lround(
                                viewport.contentExtent)));
                        control.viewportWidth = std::max(1,
                            static_cast<int>(std::lround(
                                viewport.frame.width)));
                        control.contentWidth = control.viewportWidth;
                        control.viewportHeight = std::max(1,
                            static_cast<int>(std::lround(
                                viewport.frame.height)));
                        control.contentHeight = control.viewportHeight;
                        if (control.horizontal)
                        {
                            control.viewportWidth = viewportExtent;
                            control.contentWidth = contentExtent;
                        }
                        else
                        {
                            control.viewportHeight = viewportExtent;
                            control.contentHeight = contentExtent;
                        }
                        RuntimeRegisterHostControl(widgetId,
                            std::move(control));
                    }
                    for (const auto& input : inputControls)
                    {
                        LuaWidget::HostControl control;
                        control.type = LuaWidget::HostControl::Type::Input;
                        control.id = input.key;
                        control.rect = {
                            static_cast<LONG>(std::lround(input.frame.x)),
                            static_cast<LONG>(std::lround(input.frame.y)),
                            static_cast<LONG>(std::lround(input.frame.x +
                                input.frame.width)),
                            static_cast<LONG>(std::lround(input.frame.y +
                                input.frame.height)),
                        };
                        if (input.clip)
                        {
                            control.clipRect = RECT{
                                static_cast<LONG>(std::lround(input.clip->x)),
                                static_cast<LONG>(std::lround(input.clip->y)),
                                static_cast<LONG>(std::lround(input.clip->x +
                                    input.clip->width)),
                                static_cast<LONG>(std::lround(input.clip->y +
                                    input.clip->height)),
                            };
                        }
                        control.enabled = input.enabled;
                        control.controlled = true;
                        control.numeric = input.type ==
                            snowdesktop::widget_runtime::
                                ViewNodeType::NumberInput;
                        control.controlledText = input.value;
                        control.placeholder = input.placeholder;
                        control.changeAction = input.changeAction;
                        control.focusAction = input.focusAction;
                        control.blurAction = input.blurAction;
                        control.submitAction = input.submitAction;
                        control.selectAll = input.selectAll;
                        control.liveUpdate = input.liveUpdate;
                        control.multiline = input.type ==
                            snowdesktop::widget_runtime::
                                ViewNodeType::TextArea;
                        control.fontSize = input.fontSize;
                        control.padding = input.padding;
                        control.maximumUtf8Bytes = input.maximumUtf8Bytes == 0
                            ? snowdesktop::widget_runtime::
                                ViewTreeLimits::MaximumTextBytes
                            : input.maximumUtf8Bytes;
                        control.minimum = input.minimum;
                        control.maximum = input.maximum;
                        control.step = input.step;
                        control.viewportHeight = std::max(1,
                            static_cast<int>(std::lround(
                                input.frame.height)));
                        control.contentHeight = control.viewportHeight;
                        if (control.multiline)
                        {
                            const float innerWidth = std::max(1.0f,
                                input.frame.width - input.padding * 2.0f -
                                    8.0f);
                            std::wstring metricText =
                                Utf8ToWideLocal(input.value);
                            size_t metricCursor = 0;
                            size_t metricAnchor = 0;
                            std::wstring metricComposition;
                            size_t metricCompositionCursor = 0;
                            (void)RuntimeGetFocusedHostInput(widgetId,
                                input.key, metricText, metricCursor,
                                metricAnchor, metricComposition,
                                metricCompositionCursor);
                            ComPtr<IDWriteTextLayout> layout =
                                CreateHostMultilineTextLayout(d2dState_,
                                    metricText,
                                    input.fontSize, innerWidth);
                            DWRITE_TEXT_METRICS metrics{};
                            if (layout) layout->GetMetrics(&metrics);
                            control.contentHeight = std::max(
                                control.viewportHeight,
                                static_cast<int>(std::ceil(metrics.height +
                                    input.padding * 2.0f)));
                        }
                        RuntimeRegisterHostControl(widgetId,
                            std::move(control));
                    }
                }
            }
            lua_pop(state, 1);
            if (accepted)
            {
                const int currentIndex = FindWidget(widgetId);
                if (currentIndex >= 0)
                {
                    found = &widgets_[currentIndex];
                    found->viewTree = std::move(candidate);
                }
                else
                    accepted = false;
            }
        }

        if (!accepted)
        {
            found->interactionRegions.AbortFrame();
            if (!viewError.empty())
                RuntimeRecordError(widgetId, viewError);
        }
        else
        {
            const auto transition =
                found->interactionRegions.CommitFrame();
            if (transition.Changed())
            {
                float pointerX = 0.0f;
                float pointerY = 0.0f;
                found->interactionRegions.LastPointer(pointerX, pointerY);
                DispatchInteractionTransition(*found, transition,
                    static_cast<int>(pointerX),
                    static_cast<int>(pointerY));
                RuntimeInvalidateHost(widgetId);
            }
            ResolveDeferredHostInputFocus(widgetId,
                d2dState_->surfaceKind ? d2dState_->surfaceKind : "desktop");
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        if (found->viewTree)
        {
            DrawWidgetViewNode(d2dState_, *found->viewTree,
                found->interactionRegions);
            DrawWidgetSelectOverlays(d2dState_, *found->viewTree,
                found->interactionRegions, std::nullopt,
                found->viewTree->frame.height);
        }
        while (d2dState_->widgetClipDepth > 0)
        {
            d2dState_->ctx->PopAxisAlignedClip();
            --d2dState_->widgetClipDepth;
        }
        lua_pop(state, 1);
        return;
    }
    lua_pop(state, 1);

    lua_getfield(state, -1, "render");
    if (lua_isfunction(state, -1))
    {
        int argumentCount = 0;
        if (found->manifest.apiVersion >= 2)
        {
            const auto pushContext = +[](lua_State* lifecycleState) {
                (void)lua_WidgetContext(lifecycleState);
            };
            if (!found->lifecycle.PushRenderArguments(
                    state, pushContext))
            {
                lua_pop(state, 2);
                RuntimeRecordError(widgetId,
                    "Widget lifecycle is not initialized");
                found->interactionRegions.AbortFrame();
                found->valid = false;
                return;
            }
            argumentCount = 2;
        }
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, argumentCount, 0) != LUA_OK)
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
                widgets_[currentIndex].interactionRegions.AbortFrame();
                widgets_[currentIndex].valid = false;
            }
            lua_pop(state, 1);
            return;
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        if (const int currentIndex = FindWidget(widgetId);
            currentIndex >= 0 &&
            widgets_[currentIndex].manifest.apiVersion >= 2)
        {
            auto& current = widgets_[currentIndex];
            const auto transition =
                current.interactionRegions.CommitFrame();
            if (transition.Changed())
            {
                float pointerX = 0.0f;
                float pointerY = 0.0f;
                current.interactionRegions.LastPointer(
                    pointerX, pointerY);
                DispatchInteractionTransition(current, transition,
                    static_cast<int>(pointerX),
                    static_cast<int>(pointerY));
                RuntimeInvalidateHost(widgetId);
            }
            ResolveDeferredHostInputFocus(widgetId,
                d2dState_->surfaceKind ? d2dState_->surfaceKind : "desktop");
        }
    }
    else
    {
        if (found->manifest.apiVersion >= 2)
            found->interactionRegions.AbortFrame();
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
    WidgetSurfaceScope surfaceScope(d2dState_, "panel");
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
    lua_getfield(state, -1,
        widget.manifest.apiVersion >= 2 ? "panel" : "renderPanel");
    if (!lua_isfunction(state, -1))
    {
        lua_pop(state, 2);
        return false;
    }
    int argumentCount = 0;
    if (widget.manifest.apiVersion >= 2)
    {
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        if (!widget.lifecycle.PushRenderArguments(state, pushContext))
        {
            lua_pop(state, 2);
            RuntimeRecordError(widgetId,
                "Widget lifecycle is not initialized");
            return false;
        }
        argumentCount = 2;
    }
    if (widget.manifest.apiVersion >= 2)
        widget.panelFrameOpen = true;
    const bool succeeded = snowdesktop::lua_runtime::ProtectedCall(
        state, argumentCount, 0) == LUA_OK;
    widget.panelFrameOpen = false;
    if (succeeded)
        ResolveDeferredHostInputFocus(widgetId, "panel");
    if (!succeeded)
    {
        const char* error = lua_tostring(state, -1);
        RuntimeRecordError(
            widgetId,
            error ? error : "(panel render error)");
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
    const auto runtimeNow =
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    if (dataBroker_)
    {
        dataBroker_->Tick(runtimeNow);
        ApplyWidgetDataBrokerActions();
    }
    DrainFilesystemWatchCompletions();
    ApplyWidgetTaskBrokerActions();
    if (widgetSystemDataProvider_)
    {
        const auto changedTopics =
            widgetSystemDataProvider_->DrainChangedTopics();
        if (!changedTopics.empty())
        {
            std::unordered_set<std::wstring> dirtyWidgets;
            for (const auto& widget : widgets_)
            {
                if (!widget.valid || widget.preview) continue;
                for (const auto& [_, topic] : widget.dataSubscriptions)
                {
                    if (std::find(changedTopics.begin(), changedTopics.end(),
                            topic) != changedTopics.end())
                    {
                        dirtyWidgets.insert(widget.widgetId);
                        break;
                    }
                }
            }
            for (const auto& widgetId : dirtyWidgets)
                RuntimeInvalidateHost(widgetId);
        }
    }
    if (widgetAudioAnalysisProvider_ &&
        widgetAudioAnalysisProvider_->DrainChanged())
    {
        for (const auto& widget : widgets_)
        {
            if (!widget.valid || widget.preview) continue;
            const bool subscribed = std::any_of(
                widget.dataSubscriptions.begin(),
                widget.dataSubscriptions.end(),
                [](const auto& entry) {
                    return entry.second == "audio.output.analysis";
                });
            if (subscribed)
                RuntimeInvalidateHost(widget.widgetId);
        }
    }
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
            if (const auto task = networkRequestTasks_.find(response.id);
                task != networkRequestTasks_.end())
            {
                const std::uint64_t taskId = task->second;
                networkRequestTasks_.erase(task);
                networkTaskRequests_.erase(taskId);
                const bool ok = response.error.empty() &&
                    response.status >= 200 && response.status < 300;
                std::string error;
                if (!ok)
                {
                    if (response.error == "Cancelled")
                        error = "canceled";
                    else if (response.error == "Response too large")
                        error = "responseTooLarge";
                    else if (!response.error.empty())
                        error = response.error.find("Redirect") !=
                            std::string::npos
                            ? "redirectRejected" : "networkError";
                    else
                        error = "httpStatus";
                }
                networkTaskCompletions_.insert_or_assign(
                    taskId, response);
                if (taskBroker_)
                    (void)taskBroker_->Complete(
                        taskId, ok, std::move(error));
                continue;
            }
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

    for (auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview) continue;
        if (widget.hostVisible && widget.lastRenderTime.time_since_epoch().count() > 0 &&
            runtimeNow - widget.lastRenderTime > std::chrono::milliseconds(2500))
        {
            widget.hostVisible = false;
            if (widget.namedTimers.SetVisible(false, runtimeNow))
                RescheduleNamedTimer(widget);
            if (dataBroker_)
            {
                (void)dataBroker_->SetInstanceVisible(
                    WidgetWideToUtf8(widget.widgetId), false, runtimeNow);
                ApplyWidgetDataBrokerActions();
            }
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
        if (widget.manifest.apiVersion >= 2)
        {
            (void)InvokeLifecycleEvent(widget, "timer",
                [](lua_State* eventState) {
                    lua_pushliteral(eventState, "refresh");
                    lua_setfield(eventState, -2, "name");
                });
            RuntimeInvalidateHost(activeWidgetId);
            return;
        }
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
        (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);
        RuntimeInvalidateHost(activeWidgetId);
        return;
    }

    if (timerId != widget.namedTimerId)
        return;

    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    const auto now = std::chrono::steady_clock::now();
    const auto wallNowMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const std::vector<std::string> dueNames =
        widget.namedTimers.DueNames(now);

    bool invoked = false;
    for (const auto& name : dueNames)
    {
        const auto fire = widget.namedTimers.ConsumeDueInfo(name, now);
        if (!fire)
            continue;

        if (widget.manifest.apiVersion >= 2)
        {
            (void)InvokeLifecycleEvent(widget, "schedule",
                [&fire, wallNowMilliseconds](lua_State* eventState) {
                    lua_pushlstring(eventState,
                        fire->name.data(), fire->name.size());
                    lua_setfield(eventState, -2, "id");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(wallNowMilliseconds));
                    lua_setfield(eventState, -2, "now");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(fire->missed));
                    lua_setfield(eventState, -2, "missed");
                    lua_pushboolean(eventState,
                        fire->coalesced ? 1 : 0);
                    lua_setfield(eventState, -2, "coalesced");
                });
            invoked = true;
            continue;
        }

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
    {
        (void)snowdesktop::widget_api::ConsumeTransientStateDirty(state);
        RuntimeInvalidateHost(activeWidgetId);
    }
}

// ── Check if widget uses custom style ────────────────────────────
bool WidgetEngine::HasCustomStyle(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 && widgets_[idx].customStyle;
}

void WidgetEngine::InvokeOpen(
    const std::wstring& widgetId, bool trustedGesture)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, trustedGesture);
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
    const bool trustedGesture = snowdesktop::widget_runtime::
        IsTrustedWidgetGestureCallback(callbackName);
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, trustedGesture);
    if (w.manifest.apiVersion >= 2)
    {
        const bool panel = std::strstr(callbackName, "Panel") != nullptr;
        WidgetSurfaceScope surfaceScope(
            d2dState_, panel ? "panel" : "desktop");
        const char* kind = "pointer";
        const char* action = callbackName;
        if (std::strcmp(callbackName, "onClick") == 0 ||
            std::strcmp(callbackName, "onPanelClick") == 0)
            action = "click";
        else if (std::strcmp(callbackName, "onDoubleClick") == 0)
            action = "doubleClick";
        else if (std::strcmp(callbackName, "onMouseDown") == 0 ||
            std::strcmp(callbackName, "onPanelMouseDown") == 0)
            action = "pointerDown";
        else if (std::strcmp(callbackName, "onMouseMove") == 0 ||
            std::strcmp(callbackName, "onPanelMouseMove") == 0)
            action = "pointerMove";
        else if (std::strcmp(callbackName, "onMouseUp") == 0 ||
            std::strcmp(callbackName, "onPanelMouseUp") == 0)
            action = "pointerUp";
        else if (std::strcmp(callbackName, "onWheel") == 0 ||
            std::strcmp(callbackName, "onPanelWheel") == 0)
            action = "wheel";
        else if (std::strcmp(callbackName, "onPanelOpened") == 0)
        {
            kind = "panel";
            action = "opened";
        }
        else if (std::strcmp(callbackName, "onPanelClosed") == 0)
        {
            kind = "panel";
            action = "closed";
        }
        std::string targetKey;
        if (!panel && std::strcmp(kind, "pointer") == 0)
        {
            const auto transition = w.interactionRegions.UpdateHover(
                static_cast<float>(x), static_cast<float>(y));
            if (transition.Changed())
            {
                DispatchInteractionTransition(w, transition, x, y);
                RuntimeInvalidateHost(widgetId);
            }
            if (std::strcmp(action, "pointerDown") == 0)
            {
                targetKey = w.interactionRegions.PointerDown(x, y, button).
                    targetKey;
                RuntimeInvalidateHost(widgetId);
            }
            else if (std::strcmp(action, "pointerUp") == 0)
            {
                targetKey = w.interactionRegions.PointerUp(x, y, button).
                    targetKey;
                RuntimeInvalidateHost(widgetId);
            }
            else if (std::strcmp(action, "click") == 0)
            {
                targetKey = w.interactionRegions.ConsumeClickTarget(x, y);
            }
            else if (std::strcmp(action, "pointerMove") == 0)
            {
                targetKey = w.interactionRegions.PointerMoveTarget(x, y);
            }
            else
            {
                targetKey = w.interactionRegions.TargetAt(x, y);
            }
            DispatchInteractionAction(w, targetKey, action, x, y,
                button, delta,
                std::strcmp(action, "doubleClick") == 0 ? 2 :
                    (std::strcmp(action, "click") == 0 ? 1 : 0));
        }
        (void)InvokeLifecycleEvent(w, kind,
            [action, panel, x, y, button, delta, trustedGesture,
                &targetKey](lua_State* eventState) {
                lua_pushstring(eventState, action);
                lua_setfield(eventState, -2, "action");
                lua_pushstring(eventState, panel ? "panel" : "desktop");
                lua_setfield(eventState, -2, "surface");
                lua_pushinteger(eventState, x);
                lua_setfield(eventState, -2, "x");
                lua_pushinteger(eventState, y);
                lua_setfield(eventState, -2, "y");
                lua_pushinteger(eventState, button);
                lua_setfield(eventState, -2, "button");
                lua_pushinteger(eventState, delta);
                lua_setfield(eventState, -2, "delta");
                if (!targetKey.empty())
                {
                    lua_pushlstring(eventState, targetKey.data(),
                        targetKey.size());
                    lua_setfield(eventState, -2, "targetKey");
                }
                lua_pushboolean(eventState, trustedGesture ? 1 : 0);
                lua_setfield(eventState, -2, "trustedGesture");
            });
        return;
    }
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

std::vector<LuaWidgetMenuItem> WidgetEngine::GetContextMenu(
    const std::wstring& widgetId, int x, int y)
{
    std::vector<LuaWidgetMenuItem> result;
    int idx = FindWidget(widgetId);
    if (idx < 0) return result;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return result;

    if (w.manifest.apiVersion >= 2)
    {
        std::string targetKey;
        const auto* requestActionPointer =
            w.interactionRegions.ActionAt(x, y, "contextMenu", &targetKey);
        if (!requestActionPointer || targetKey.empty()) return result;
        const auto requestAction = *requestActionPointer;
        const std::uint64_t generation =
            w.interactionRegions.Generation();
        WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
        snowdesktop::lua_runtime::StackGuard stackGuard(state);
        SetWidgetRectContext(d2dState_, w.lastBounds);
        lua_createtable(state, 0, 6);
        lua_pushlstring(state, requestAction.id.data(),
            requestAction.id.size());
        lua_setfield(state, -2, "id");
        PushInteractionValue(state, requestAction.value);
        lua_setfield(state, -2, "value");
        lua_pushlstring(state, targetKey.data(), targetKey.size());
        lua_setfield(state, -2, "targetKey");
        lua_pushliteral(state, "desktop");
        lua_setfield(state, -2, "surface");
        lua_pushliteral(state, "pointer");
        lua_setfield(state, -2, "source");
        lua_pushstring(state,
            requestAction.contextMenuScope ==
                    snowdesktop::widget_runtime::InteractionAction::
                        ContextMenuScope::Component
                ? "component" : "element");
        lua_setfield(state, -2, "scope");

        bool invoked = false;
        std::string error;
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        if (!w.lifecycle.Menu(state, w.ref, pushContext, -1,
                invoked, error))
        {
            if (!error.empty()) RuntimeRecordError(widgetId, error);
            return result;
        }
        if (!invoked) return result;
        if (!lua_istable(state, -1))
        {
            if (!lua_isnil(state, -1))
                RuntimeRecordError(widgetId,
                    "Widget menu callback must return ui.menu(...) or nil");
            return result;
        }

        std::unordered_set<std::string> ids;
        const int count = std::min<int>(
            static_cast<int>(lua_rawlen(state, -1)), 64);
        for (int itemIndex = 1; itemIndex <= count; ++itemIndex)
        {
            lua_rawgeti(state, -1, itemIndex);
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                continue;
            }
            LuaWidgetMenuItem item;
            lua_getfield(state, -1, "type");
            item.separator = lua_isstring(state, -1) &&
                std::strcmp(lua_tostring(state, -1), "separator") == 0;
            lua_pop(state, 1);
            if (!item.separator)
            {
                lua_getfield(state, -1, "id");
                if (lua_isstring(state, -1))
                    item.actionId = lua_tostring(state, -1);
                lua_pop(state, 1);
                lua_getfield(state, -1, "label");
                if (lua_isstring(state, -1))
                    item.label = lua_tostring(state, -1);
                lua_pop(state, 1);
                lua_getfield(state, -1, "icon");
                if (lua_isstring(state, -1))
                    item.icon = lua_tostring(state, -1);
                lua_pop(state, 1);
                lua_getfield(state, -1, "iconFont");
                if (lua_isstring(state, -1))
                    item.iconFont = lua_tostring(state, -1);
                lua_pop(state, 1);
                lua_getfield(state, -1, "enabled");
                item.enabled = lua_isnil(state, -1) ||
                    lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                lua_getfield(state, -1, "checked");
                item.checked = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (item.actionId.empty() || item.actionId.size() > 128 ||
                    item.label.empty() || item.label.size() > 512 ||
                    !ids.insert(item.actionId).second)
                {
                    lua_pop(state, 1);
                    continue;
                }
                item.v2Action = true;
                item.elementContext =
                    requestAction.contextMenuScope ==
                    snowdesktop::widget_runtime::InteractionAction::
                        ContextMenuScope::Element;
                item.targetKey = targetKey;
                item.contextValue = requestAction.value;
                item.interactionGeneration = generation;
            }
            result.push_back(std::move(item));
            lua_pop(state, 1);
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        return result;
    }

    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return result;
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
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
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

void WidgetEngine::InvokeMenu(const std::wstring& widgetId,
    const LuaWidgetMenuItem& menuItem)
{
    if (!menuItem.v2Action)
    {
        InvokeMenu(widgetId, menuItem.id);
        return;
    }
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    if (widget.manifest.apiVersion < 2 || menuItem.actionId.empty() ||
        widget.interactionRegions.Generation() !=
            menuItem.interactionGeneration ||
        !widget.interactionRegions.Find(menuItem.targetKey))
        return;
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    (void)InvokeLifecycleEvent(widget, "action",
        [&menuItem](lua_State* eventState) {
            lua_pushlstring(eventState, menuItem.actionId.data(),
                menuItem.actionId.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, menuItem.contextValue);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, menuItem.targetKey.data(),
                menuItem.targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushliteral(eventState, "contextMenu");
            lua_setfield(eventState, -2, "source");
            lua_pushliteral(eventState, "desktop");
            lua_setfield(eventState, -2, "surface");
            lua_pushboolean(eventState, 1);
            lua_setfield(eventState, -2, "trustedGesture");
        });
}

void WidgetEngine::NotifyDesktopChanged(const std::string& reason)
{
    ++desktopDataRevision_;
    desktopDataTimestampMs_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    desktopDataChangeReason_ = reason.substr(0, 64);
    if (desktopDataChangeReason_.empty())
        desktopDataChangeReason_ = "changed";
    if (reason == "applications")
    {
        ++appIndexRevision_;
        for (const auto& widget : widgets_)
        {
            if (!widget.valid || widget.preview || !dataBroker_ ||
                !RuntimeHasPermission(
                    widget.widgetId, kAppDiscoveryPermission))
                continue;
            const bool subscribed = std::any_of(
                widget.dataSubscriptions.begin(),
                widget.dataSubscriptions.end(),
                [this](const auto& entry) {
                    if (entry.second != "app.indexStatus") return false;
                    const auto binding =
                        dataBroker_->SubscriptionSnapshot(entry.first);
                    return binding && binding->eligible;
                });
            if (subscribed)
                RuntimeInvalidateHost(widget.widgetId);
        }
    }
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
        const bool subscribed = dataBroker_ && std::any_of(
            widgets_[index].dataSubscriptions.begin(),
            widgets_[index].dataSubscriptions.end(),
            [this](const auto& entry) {
                if (!entry.second.starts_with("desktop.")) return false;
                const auto binding =
                    dataBroker_->SubscriptionSnapshot(entry.first);
                return binding && binding->eligible;
            });
        if (subscribed)
            RuntimeInvalidateHost(widgetId);
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
    DisposeWidgetLifecycle(old, "hotReload");
    ReleaseWidgetDataSubscriptions(old);
    ReleaseWidgetTasks(old,
        snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
    ClearRuntimeImagesForWidget(d2dState_, widgetId);
    if (old.refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(old.refreshTimerId);
    if (old.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(old.namedTimerId);
    if (httpService_) httpService_->CancelWidget(widgetId);
    if (old.state)
    {
        old.lifecycle.Release(old.state);
        luaL_unref(old.state, LUA_REGISTRYINDEX, old.ref);
        lua_close(old.state);
        old.state = nullptr;
    }
    widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(oldIndex));
    return true;
}

bool WidgetEngine::RetryWidget(const std::wstring& widgetId,
    const std::wstring& packageId)
{
    widgetHostFailures_.erase(widgetId);
    if (FindWidget(widgetId) >= 0)
        return ReloadWidget(widgetId);
    return EnsureWidgetLoaded(widgetId, packageId);
}

snowdesktop::widget_runtime::WidgetHostState
WidgetEngine::GetWidgetHostState(const std::wstring& widgetId,
    const std::wstring& packageId) const
{
    using snowdesktop::widget_runtime::WidgetHostStateKind;

    if (!IsWidgetPackageInstalled(packageId))
        return { WidgetHostStateKind::PackageMissing, {} };
    const auto package = GetWidgetPackage(packageId);
    if (!package)
        return { WidgetHostStateKind::LoadFailed, {} };
    const auto grant = snowdesktop::widget::WidgetPermissionBroker::Evaluate(
        package->permissionState,
        package->manifest.permissions,
        package->manifest.optionalPermissions,
        package->manifest.networkDomains,
        package->grantedPermissions,
        package->grantedNetworkDomains);
    switch (grant.runtimeBlock)
    {
    case snowdesktop::widget::PermissionRuntimeBlock::PendingConsent:
        return { WidgetHostStateKind::PermissionPending, {} };
    case snowdesktop::widget::PermissionRuntimeBlock::Denied:
    case snowdesktop::widget::PermissionRuntimeBlock::MissingRequired:
        return { WidgetHostStateKind::PermissionDenied, {} };
    case snowdesktop::widget::PermissionRuntimeBlock::None:
        break;
    }

    const int index = FindWidget(widgetId);
    if (index >= 0)
    {
        const LuaWidget& widget = widgets_[index];
        if (widget.valid)
            return { WidgetHostStateKind::Ready, {} };
        const bool quotaExceeded = widget.quota &&
            (widget.quota->memoryExceeded ||
                widget.quota->executionExceeded);
        if (quotaExceeded)
            return { WidgetHostStateKind::QuotaExceeded, {} };
        if (widget.health.CircuitOpen())
            return { WidgetHostStateKind::RuntimeSuspended, {} };
    }

    if (const auto failure = widgetHostFailures_.find(widgetId);
        failure != widgetHostFailures_.end())
        return failure->second;
    return { WidgetHostStateKind::LoadFailed, {} };
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
    return snowdesktop::widget::WidgetPermissionBroker::
        AllowsPermission(perms, permission);
}

snowdesktop::widget_runtime::DataSubscriptionResult
WidgetEngine::RuntimeSubscribeData(
    const std::wstring& widgetId, std::string topic,
    std::chrono::milliseconds maxAge,
    snowdesktop::widget_runtime::DataHiddenPolicy whenHidden,
    std::string rangeStart, std::string rangeEnd,
    std::string scopeHandle)
{
    using snowdesktop::widget_runtime::DataSubscriptionOptions;
    if (!dataBroker_)
        return { 0, "data broker is not initialized" };
    const int index = FindWidget(widgetId);
    if (index < 0)
        return { 0, "widget instance is not loaded" };

    LuaWidget& widget = widgets_[index];
    std::optional<FilesystemWatchBinding> filesystemWatch;
    if (topic == "filesystem.watch")
    {
        if (scopeHandle.empty()) return { 0, "folder handle is required" };
        FilesystemWatchBinding binding;
        binding.owner = {
            WidgetWideToUtf8(widget.widgetId), widget.packageId };
        binding.sourceHandle = scopeHandle;
        binding.preview = widget.preview;
        if (widget.preview && IsPreviewFilesystemHandle(scopeHandle))
        {
            binding.directory = L"Preview Folder";
            binding.access = snowdesktop::widget_runtime::
                WidgetFilesystemHandleAccess::Read;
            binding.available = true;
            binding.warmingUp = false;
        }
        else
        {
            if (!filesystemHandleStore_)
                return { 0, "filesystem handle provider is unavailable" };
            const auto entry = filesystemHandleStore_->Resolve(
                binding.owner, scopeHandle);
            if (!entry) return { 0, "invalid filesystem handle" };
            if (entry->kind != snowdesktop::widget_runtime::
                    WidgetFilesystemHandleKind::Folder)
                return { 0, "filesystem handle is not a folder" };
            binding.directory = entry->path;
            binding.access = entry->access;
        }
        filesystemWatch = std::move(binding);
    }
    else if (!scopeHandle.empty())
    {
        return { 0, "scope handle is not supported for this topic" };
    }

    DataSubscriptionOptions options;
    options.requestedInterval = maxAge;
    options.whenHidden = whenHidden;
    options.visible = widget.hostVisible;
    const auto requiredPermission =
        dataBroker_->RequiredPermission(topic);
    options.permissionGranted = requiredPermission &&
        (requiredPermission->empty() || RuntimeHasPermission(
            widgetId, requiredPermission->c_str()));
    options.preview = widget.preview;
    options.rangeStart = std::move(rangeStart);
    options.rangeEnd = std::move(rangeEnd);
    auto result = dataBroker_->Subscribe(
        WidgetWideToUtf8(widgetId), topic, options,
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
    if (result)
    {
        widget.dataSubscriptions.emplace(result.id, std::move(topic));
        if (filesystemWatch)
            filesystemWatchBindings_.emplace(
                result.id, std::move(*filesystemWatch));
        ApplyWidgetDataBrokerActions();
    }
    return result;
}

bool WidgetEngine::RuntimeUnsubscribeData(
    std::uint64_t subscriptionId)
{
    if (subscriptionId == 0 || !dataBroker_) return false;
    bool owned = false;
    for (auto& widget : widgets_)
    {
        if (widget.dataSubscriptions.erase(subscriptionId) > 0)
        {
            owned = true;
            break;
        }
    }
    if (!owned) return false;
    if (filesystemWatchService_)
        (void)filesystemWatchService_->Stop(subscriptionId);
    filesystemWatchBindings_.erase(subscriptionId);
    const bool removed = dataBroker_->Unsubscribe(subscriptionId,
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
    ApplyWidgetDataBrokerActions();
    return removed;
}

std::optional<LuaWidgetDataSnapshot>
WidgetEngine::RuntimeGetDataSnapshot(
    std::uint64_t subscriptionId) const
{
    if (subscriptionId == 0 || !dataBroker_) return std::nullopt;
    const auto binding = dataBroker_->SubscriptionSnapshot(subscriptionId);
    if (!binding) return std::nullopt;

    LuaWidgetDataSnapshot result;
    result.topic = binding->topic;
    const auto timestampNow =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    if (binding->options.preview)
    {
        result.available = true;
        result.stale = false;
        result.timestampMs = timestampNow;
        if (result.topic == "system.cpu")
        {
            result.cpu.available = true;
            result.cpu.warmingUp = false;
            result.cpu.usagePercent = 42.0;
            result.cpu.logicalProcessors = 12;
            result.cpu.name = "Preview CPU";
            result.cpu.timestampMs = timestampNow;
        }
        else if (result.topic == "system.memory")
        {
            result.memory.available = true;
            result.memory.totalBytes = 16ull * 1024 * 1024 * 1024;
            result.memory.usedBytes = 9ull * 1024 * 1024 * 1024;
            result.memory.freeBytes = 7ull * 1024 * 1024 * 1024;
            result.memory.usagePercent = 56.25;
            result.memory.timestampMs = timestampNow;
        }
        else if (result.topic == "system.power")
        {
            result.power.available = true;
            result.power.acPower = false;
            result.power.charging = false;
            result.power.saver = false;
            result.power.batteryPercent = 73.0;
            result.power.estimatedRemainingSeconds = 10800;
            result.power.timestampMs = timestampNow;
        }
        else if (result.topic == "system.network.status")
        {
            result.networkStatus.available = true;
            result.networkStatus.connectivity = "internet";
            result.networkStatus.transport = "wifi";
            result.networkStatus.costKnown = true;
            result.networkStatus.timestampMs = timestampNow;
        }
        else if (result.topic == "system.network.traffic")
        {
            result.networkTraffic.available = true;
            result.networkTraffic.connected = true;
            result.networkTraffic.warmingUp = false;
            result.networkTraffic.receivedBytes = 987654321;
            result.networkTraffic.sentBytes = 123456789;
            result.networkTraffic.downloadBytesPerSecond = 245760;
            result.networkTraffic.uploadBytesPerSecond = 32768;
            result.networkTraffic.timestampMs = timestampNow;
        }
        else if (result.topic == "system.gpu")
        {
            result.gpu.available = true;
            result.gpu.warmingUp = false;
            result.gpu.timestampMs = timestampNow;
            result.gpu.adapters = {
                { "adapter-1", "Preview GPU", 38.0,
                    8ull * 1024 * 1024 * 1024,
                    3ull * 1024 * 1024 * 1024,
                    8ull * 1024 * 1024 * 1024,
                    1ull * 1024 * 1024 * 1024 }
            };
        }
        else if (result.topic == "system.storage.volumes")
        {
            result.storageVolumes.available = true;
            result.storageVolumes.timestampMs = timestampNow;
            result.storageVolumes.volumes = {
                { "volume-preview-system", "System", "C:\\", "fixed",
                    512ull * 1024 * 1024 * 1024,
                    214ull * 1024 * 1024 * 1024, true, false, false },
                { "volume-preview-removable", "Portable", "E:\\",
                    "removable", 64ull * 1024 * 1024 * 1024,
                    48ull * 1024 * 1024 * 1024, true, true, false }
            };
        }
        else if (result.topic == "system.storage.io")
        {
            result.storageIo.available = true;
            result.storageIo.warmingUp = false;
            result.storageIo.readBytesPerSecond = 18ull * 1024 * 1024;
            result.storageIo.writeBytesPerSecond = 4ull * 1024 * 1024;
            result.storageIo.busyPercent = 27.5;
            result.storageIo.timestampMs = timestampNow;
        }
        else if (result.topic == "system.display.topology")
        {
            result.displayTopology.available = true;
            result.displayTopology.timestampMs = timestampNow;
            result.displayTopology.displays = {
                { "display-preview-primary", "Preview Display", true,
                    { 0.0, 0.0, 1280.0, 720.0 },
                    { 0.0, 0.0, 1280.0, 680.0 },
                    { 0, 0, 1920, 1080 }, { 0, 0, 1920, 1020 },
                    144, 144, 1.5, 60.0, "landscape",
                    true, true, false }
            };
        }
        else if (result.topic == "system.display.current")
        {
            result.displayCurrent = {
                "display-preview-primary", "Preview Display", true,
                { 0.0, 0.0, 1280.0, 720.0 },
                { 0.0, 0.0, 1280.0, 680.0 },
                { 0, 0, 1920, 1080 }, { 0, 0, 1920, 1020 },
                144, 144, 1.5, 60.0, "landscape",
                true, true, false };
        }
        else if (result.topic == "audio.output.default")
        {
            result.audioOutputDefault.available = true;
            result.audioOutputDefault.id = "audio-output-preview";
            result.audioOutputDefault.name = "Preview Speakers";
            result.audioOutputDefault.state = "active";
            result.audioOutputDefault.timestampMs = timestampNow;
        }
        else if (result.topic == "audio.output.volume")
        {
            result.audioOutputVolume.available = true;
            result.audioOutputVolume.endpointId = "audio-output-preview";
            result.audioOutputVolume.volume = 0.68;
            result.audioOutputVolume.muted = false;
            result.audioOutputVolume.timestampMs = timestampNow;
        }
        else if (result.topic == "audio.output.analysis")
        {
            result.audioAnalysis.available = true;
            result.audioAnalysis.warmingUp = false;
            result.audioAnalysis.silent = false;
            result.audioAnalysis.endpointId = "audio-output-preview";
            result.audioAnalysis.sampleRate = 48000;
            result.audioAnalysis.channels = 2;
            result.audioAnalysis.rms = 0.42;
            result.audioAnalysis.peak = 0.78;
            result.audioAnalysis.timestampMs = timestampNow;
            result.audioAnalysis.waveform.resize(
                snowdesktop::widget_runtime::
                    WidgetAudioAnalysisProvider::WaveformPoints);
            for (std::size_t index = 0;
                index < result.audioAnalysis.waveform.size(); ++index)
            {
                result.audioAnalysis.waveform[index] =
                    0.62 * std::sin(
                        static_cast<double>(index) * 0.22);
            }
            result.audioAnalysis.spectrum.resize(
                snowdesktop::widget_runtime::
                    WidgetAudioAnalysisProvider::SpectrumBins);
            for (std::size_t index = 0;
                index < result.audioAnalysis.spectrum.size(); ++index)
            {
                result.audioAnalysis.spectrum[index] =
                    std::exp(-static_cast<double>(index) / 14.0);
            }
        }
        else if (result.topic == "media.sessions" ||
            result.topic == "media.current" ||
            result.topic == "media.timeline")
        {
            snowdesktop::widget_runtime::WidgetMediaSessionDataSnapshot
                session;
            session.id = "media-session-preview";
            session.sourceName = "Preview Player";
            session.title = "Preview Track";
            session.artist = "SnowDesktop";
            session.album = "Widget API v2";
            session.playbackStatus = "playing";
            session.current = true;
            session.controls = { true, true, true, true, true, true,
                true, true, true, true };
            session.timeline.available = true;
            session.timeline.sessionId = session.id;
            session.timeline.positionMs = 62000;
            session.timeline.durationMs = 215000;
            session.timeline.minimumSeekMs = 0;
            session.timeline.maximumSeekMs = 215000;
            session.timeline.updatedAtMs = timestampNow;
            session.timeline.timestampMs = timestampNow;
            if (result.topic == "media.sessions")
            {
                result.mediaSessions.available = true;
                result.mediaSessions.currentSessionId = session.id;
                result.mediaSessions.sessions = { session };
                result.mediaSessions.timestampMs = timestampNow;
            }
            else if (result.topic == "media.current")
            {
                result.mediaCurrent.available = true;
                result.mediaCurrent.session = session;
                result.mediaCurrent.timestampMs = timestampNow;
            }
            else
            {
                result.mediaTimeline = session.timeline;
            }
        }
        else if (result.topic == "media.artwork")
        {
            auto pixels = std::make_shared<snowdesktop::widget_runtime::
                WidgetRuntimeImagePixels>();
            pixels->width = 64;
            pixels->height = 64;
            pixels->stride = pixels->width * 4;
            pixels->bgraPremultiplied.resize(
                pixels->stride * pixels->height);
            for (std::uint32_t y = 0; y < pixels->height; ++y)
            {
                for (std::uint32_t x = 0; x < pixels->width; ++x)
                {
                    const std::size_t offset =
                        static_cast<std::size_t>(y * pixels->stride + x * 4);
                    pixels->bgraPremultiplied[offset] =
                        static_cast<std::uint8_t>(96 + x * 2);
                    pixels->bgraPremultiplied[offset + 1] =
                        static_cast<std::uint8_t>(64 + y * 2);
                    pixels->bgraPremultiplied[offset + 2] = 224;
                    pixels->bgraPremultiplied[offset + 3] = 255;
                }
            }
            result.mediaArtwork.available = true;
            result.mediaArtwork.sessionId = "media-session-preview";
            result.mediaArtwork.resourceToken = "@media:preview";
            result.mediaArtwork.pixels = std::move(pixels);
            result.mediaArtwork.timestampMs = timestampNow;
            result.mediaArtwork.revision = 1;
        }
        else if (result.topic == "desktop.items" ||
            result.topic == "desktop.selection")
        {
            result.desktopItems = result.topic == "desktop.items"
                ? RuntimeDesktopItems() : RuntimeDesktopSelection();
            const std::size_t maximum = result.topic == "desktop.items"
                ? 2048u : 512u;
            if (result.desktopItems.size() > maximum)
                result.desktopItems.resize(maximum);
            result.desktopRevision = 1;
        }
        else if (result.topic == "desktop.changes")
        {
            result.desktopRevision = 1;
            result.desktopChangeReason = "preview";
        }
        else if (result.topic == "calendar.events")
        {
            result.calendarRangeStart = binding->options.rangeStart.empty()
                ? "2026-06-01" : binding->options.rangeStart;
            result.calendarRangeEnd = binding->options.rangeEnd.empty()
                ? "2026-10-03" : binding->options.rangeEnd;
            const std::vector<snowdesktop::calendar::CalendarEvent> samples = {
                { "preview-1", 1, "Preview Review", "2026-08-02",
                    false, 600, 660, "Preview notes", 15, {} },
                { "preview-2", 1, "Preview Publish", "2026-08-03",
                    true, 0, 0, {}, -1, {} },
            };
            for (const auto& event : samples)
            {
                if (event.date >= result.calendarRangeStart &&
                    event.date <= result.calendarRangeEnd)
                {
                    result.calendarEvents.push_back(event);
                }
            }
            result.calendarRevision = 1;
        }
        else if (result.topic == "calendar.selectedDate")
        {
            result.calendarSelectedDate = "2026-08-02";
            result.calendarRevision = 1;
        }
        else if (result.topic == "app.indexStatus")
        {
            result.appIndexState = "ready";
            result.appIndexRevision = 1;
        }
        else if (result.topic == "filesystem.watch")
        {
            result.filesystemWatchEvents.push_back({
                "added", "Preview.txt", {},
                "filesystem:11111111111111111111111111111111",
                "file" });
            result.filesystemWatchRevision = 1;
            result.filesystemWatchOverflow = false;
        }
        return result;
    }
    if (!binding->options.permissionGranted)
    {
        result.error = "permissionDenied";
        return result;
    }
    if (result.topic == "filesystem.watch")
    {
        const auto watch = filesystemWatchBindings_.find(subscriptionId);
        if (watch == filesystemWatchBindings_.end())
        {
            result.error = "invalidReference";
            return result;
        }
        result.available = watch->second.available;
        result.warmingUp = watch->second.warmingUp;
        result.timestampMs = watch->second.timestampMs;
        result.stale = false;
        result.error = watch->second.error;
        result.filesystemWatchEvents = watch->second.events;
        result.filesystemWatchRevision = watch->second.revision;
        result.filesystemWatchOverflow = watch->second.overflow;
        return result;
    }
    if (!widgetSystemDataProvider_)
    {
        result.error = "providerUnavailable";
        return result;
    }

    const auto setFreshness = [&](std::int64_t timestamp) {
        result.timestampMs = timestamp;
        result.stale = timestamp <= 0 || timestampNow < timestamp ||
            timestampNow - timestamp >
                binding->options.requestedInterval.count();
    };
    if (result.topic == "system.cpu")
    {
        const auto snapshot = widgetSystemDataProvider_->Cpu();
        if (snapshot)
        {
            result.cpu = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.memory")
    {
        const auto snapshot = widgetSystemDataProvider_->Memory();
        if (snapshot)
        {
            result.memory = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.power")
    {
        const auto snapshot = widgetSystemDataProvider_->Power();
        if (snapshot)
        {
            result.power = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.network.status")
    {
        const auto snapshot = widgetSystemDataProvider_->NetworkStatus();
        if (snapshot)
        {
            result.networkStatus = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.network.traffic")
    {
        const auto snapshot = widgetSystemDataProvider_->NetworkTraffic();
        if (snapshot)
        {
            result.networkTraffic = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.gpu")
    {
        const auto snapshot = widgetSystemDataProvider_->Gpu();
        if (snapshot)
        {
            result.gpu = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.storage.volumes")
    {
        const auto snapshot =
            widgetSystemDataProvider_->StorageVolumes();
        if (snapshot)
        {
            result.storageVolumes = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.storage.io")
    {
        const auto snapshot = widgetSystemDataProvider_->StorageIo();
        if (snapshot)
        {
            result.storageIo = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.display.topology")
    {
        const auto snapshot =
            widgetSystemDataProvider_->DisplayTopology();
        if (snapshot)
        {
            result.displayTopology = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.display.current")
    {
        const auto snapshot = widgetSystemDataProvider_->DisplayCurrent();
        if (snapshot)
        {
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
            const int widgetIndex = FindWidget(
                Utf8ToWideLocal(binding->instanceId));
            if (snapshot->available && widgetIndex >= 0)
            {
                const RECT monitor =
                    widgets_[widgetIndex].surfaceContext.monitorBounds;
                const auto display = snowdesktop::widget_runtime::
                    MatchDisplayByPixelBounds(*snapshot,
                        { monitor.left, monitor.top,
                            monitor.right - monitor.left,
                            monitor.bottom - monitor.top });
                if (display)
                {
                    result.displayCurrent = *display;
                    result.available = true;
                }
                else
                {
                    result.error = "currentDisplayUnavailable";
                }
            }
        }
    }
    else if (result.topic == "audio.output.default")
    {
        const auto snapshot =
            widgetSystemDataProvider_->AudioOutputDefault();
        if (snapshot)
        {
            result.audioOutputDefault = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "audio.output.volume")
    {
        const auto snapshot =
            widgetSystemDataProvider_->AudioOutputVolume();
        if (snapshot)
        {
            result.audioOutputVolume = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "audio.output.analysis")
    {
        const auto snapshot = widgetAudioAnalysisProvider_
            ? widgetAudioAnalysisProvider_->Snapshot()
            : std::nullopt;
        if (snapshot)
        {
            result.audioAnalysis = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "media.sessions")
    {
        const auto snapshot = widgetSystemDataProvider_->MediaSessions();
        if (snapshot)
        {
            result.mediaSessions = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "media.current")
    {
        const auto snapshot = widgetSystemDataProvider_->MediaCurrent();
        if (snapshot)
        {
            result.mediaCurrent = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "media.timeline")
    {
        const auto snapshot = widgetSystemDataProvider_->MediaTimeline();
        if (snapshot)
        {
            result.mediaTimeline = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "media.artwork")
    {
        const auto snapshot = widgetSystemDataProvider_->MediaArtwork();
        if (snapshot)
        {
            result.mediaArtwork = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "desktop.items" ||
        result.topic == "desktop.selection")
    {
        result.desktopItems = result.topic == "desktop.items"
            ? RuntimeDesktopItems() : RuntimeDesktopSelection();
        const std::size_t maximum = result.topic == "desktop.items"
            ? 2048u : 512u;
        if (result.desktopItems.size() > maximum)
            result.desktopItems.resize(maximum);
        result.desktopRevision = desktopDataRevision_;
        result.available = true;
        setFreshness(timestampNow);
    }
    else if (result.topic == "desktop.changes")
    {
        result.desktopRevision = desktopDataRevision_;
        result.desktopChangeReason = desktopDataChangeReason_;
        result.available = true;
        result.timestampMs = desktopDataTimestampMs_;
        result.stale = false;
    }
    else if (result.topic == "calendar.events")
    {
        result.calendarRangeStart = binding->options.rangeStart;
        result.calendarRangeEnd = binding->options.rangeEnd;
        if (result.calendarRangeStart.empty())
        {
            const std::string selected = RuntimeCalendarSelectedDate();
            const auto start = snowdesktop::calendar::CalendarService::
                AddDays(selected, -62);
            const auto end = snowdesktop::calendar::CalendarService::
                AddDays(selected, 62);
            if (start && end)
            {
                result.calendarRangeStart = *start;
                result.calendarRangeEnd = *end;
            }
        }
        if (result.calendarRangeStart.empty() ||
            result.calendarRangeEnd.empty())
        {
            result.error = "calendarRangeUnavailable";
        }
        else
        {
            result.calendarEvents = RuntimeCalendarEvents(
                result.calendarRangeStart, result.calendarRangeEnd);
            constexpr std::size_t MaximumEvents = 512;
            result.calendarTruncated =
                result.calendarEvents.size() > MaximumEvents;
            if (result.calendarTruncated)
                result.calendarEvents.resize(MaximumEvents);
            result.calendarRevision = calendarEventsRevision_;
            result.available = true;
            setFreshness(timestampNow);
        }
    }
    else if (result.topic == "calendar.selectedDate")
    {
        result.calendarSelectedDate = RuntimeCalendarSelectedDate();
        result.calendarRevision = calendarSelectionRevision_;
        result.available = !result.calendarSelectedDate.empty();
        if (!result.available)
            result.error = "notPresent";
        setFreshness(timestampNow);
    }
    else if (result.topic == "app.indexStatus")
    {
        result.appIndexState = applicationIndexStatusProvider_
            ? applicationIndexStatusProvider_() : "unavailable";
        if (result.appIndexState != "ready" &&
            result.appIndexState != "indexing" &&
            result.appIndexState != "unavailable")
            result.appIndexState = "unavailable";
        result.appIndexRevision = appIndexRevision_;
        result.available = applicationIndexStatusProvider_ != nullptr &&
            result.appIndexState != "unavailable";
        if (!result.available)
            result.error = "providerUnavailable";
        setFreshness(timestampNow);
    }
    if (result.error.empty())
    {
        const auto provider = dataBroker_->Snapshot(result.topic);
        if (provider && !provider->lastError.empty())
            result.error = provider->lastError;
    }
    return result;
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
        const bool quotaExceeded = widget.quota &&
            (widget.quota->memoryExceeded ||
                widget.quota->executionExceeded);
        RecordWidgetHostFailure(widgetId, message, quotaExceeded,
            widget.health.CircuitOpen());
    }
    else
        RecordWidgetHostFailure(widgetId, message);
    RuntimeAddLog(widgetId, "error", message);
}

void WidgetEngine::RecordWidgetHostFailure(
    const std::wstring& widgetId, const std::string& message,
    bool quotaExceeded, bool circuitOpen)
{
    widgetHostFailures_[widgetId] = {
        snowdesktop::widget_runtime::ClassifyWidgetRuntimeFailure(
            quotaExceeded, circuitOpen, message),
        message
    };
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
    const bool selectionChanged = reason == "selection";
    if (selectionChanged)
        ++calendarSelectionRevision_;
    else
        ++calendarEventsRevision_;
    const std::string topic = selectionChanged
        ? "calendar.selectedDate" : "calendar.events";
    const std::uint64_t revision = selectionChanged
        ? calendarSelectionRevision_ : calendarEventsRevision_;

    std::vector<std::wstring> targets;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview) continue;
        if (widget.manifest.apiVersion >= 2)
        {
            const bool subscribed = std::any_of(
                widget.dataSubscriptions.begin(),
                widget.dataSubscriptions.end(),
                [&topic](const auto& entry) {
                    return entry.second == topic;
                });
            if (!subscribed) continue;
        }
        else if (!RuntimeHasPermission(
                widget.widgetId, "calendar.read"))
        {
            continue;
        }
        targets.push_back(widget.widgetId);
    }
    for (const auto& widgetId : targets)
    {
        const int index = FindWidget(widgetId);
        if (index < 0) continue;
        LuaWidget& widget = widgets_[index];
        if (widget.manifest.apiVersion >= 2)
        {
            (void)InvokeLifecycleEvent(widget, "data.change",
                [&topic, revision](lua_State* eventState) {
                    lua_pushlstring(eventState, topic.data(), topic.size());
                    lua_setfield(eventState, -2, "topic");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(revision));
                    lua_setfield(eventState, -2, "revision");
                });
            RuntimeInvalidateHost(widgetId);
            continue;
        }

        lua_State* state = widget.state;
        if (!state) continue;
        const int widgetRef = widget.ref;
        const RECT bounds = widget.lastBounds;
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

std::optional<std::wstring> WidgetEngine::RuntimeResolveItemReference(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    const std::string& reference) const
{
    if (reference.empty() || ownerToken == 0) return std::nullopt;
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end()) return std::nullopt;
    if (const auto item = widget->itemReferences.find(reference);
        item != widget->itemReferences.end())
    {
        if (!previewOnly_ && !item->second.persistent &&
            item->second.sourceTask == "desktop.search" &&
            item->second.revision != desktopDataRevision_)
            return std::nullopt;
        const std::wstring target = Utf8ToWideLocal(item->second.target);
        return target.empty()
            ? std::nullopt : std::optional<std::wstring>(target);
    }
    if (const auto app = widget->applicationReferences.find(reference);
        app != widget->applicationReferences.end())
    {
        if (!previewOnly_ && !app->second.persistent &&
            app->second.catalogRevision != appIndexRevision_)
            return std::nullopt;
        const std::wstring target = Utf8ToWideLocal(
            app->second.launchTarget);
        return target.empty()
            ? std::nullopt : std::optional<std::wstring>(target);
    }
    return std::nullopt;
}

namespace
{
using snowdesktop::widget_runtime::LogicalSlotKind;
using snowdesktop::widget_runtime::ViewNode;
using snowdesktop::widget_runtime::ViewNodeType;
using snowdesktop::widget_runtime::ViewRect;

std::optional<ViewRect> IntersectLogicalSlotFrames(
    const std::optional<ViewRect>& first, const ViewRect& second)
{
    if (!first) return second;
    const float left = std::max(first->x, second.x);
    const float top = std::max(first->y, second.y);
    const float right = std::min(
        first->x + first->width, second.x + second.width);
    const float bottom = std::min(
        first->y + first->height, second.y + second.height);
    if (right <= left || bottom <= top) return std::nullopt;
    return ViewRect{ left, top, right - left, bottom - top };
}

bool LogicalSlotScrollContainer(ViewNodeType type)
{
    return type == ViewNodeType::Scroll ||
        type == ViewNodeType::VirtualList ||
        type == ViewNodeType::VirtualGrid;
}

RECT LogicalSlotClientRect(const LuaWidget& widget, const ViewRect& frame)
{
    RECT result{
        widget.lastBounds.left +
            static_cast<LONG>(std::lround(frame.x)),
        widget.lastBounds.top +
            static_cast<LONG>(std::lround(frame.y)),
        widget.lastBounds.left +
            static_cast<LONG>(std::lround(frame.x + frame.width)),
        widget.lastBounds.top +
            static_cast<LONG>(std::lround(frame.y + frame.height)),
    };
    result.left = std::max(result.left, widget.lastBounds.left);
    result.top = std::max(result.top, widget.lastBounds.top);
    result.right = std::min(result.right, widget.lastBounds.right);
    result.bottom = std::min(result.bottom, widget.lastBounds.bottom);
    return result;
}

void RebuildLogicalSlotReferences(LuaWidget& widget)
{
    std::erase_if(widget.applicationReferences, [](const auto& entry) {
        return entry.second.persistent;
    });
    std::erase_if(widget.itemReferences, [](const auto& entry) {
        return entry.second.persistent;
    });
    for (const auto& snapshot : widget.logicalSlots.Snapshots())
    {
        for (const auto& item : snapshot.items)
        {
            if (item.kind == "app.reference")
            {
                widget.applicationReferences.insert_or_assign(item.reference,
                    LuaWidget::ApplicationReference{ {}, item.target, 0,
                        item.title, item.source, item.type, true });
            }
            else
            {
                widget.itemReferences.insert_or_assign(item.reference,
                    LuaWidget::ItemReference{ item.target, "logical.slot",
                        0, item.title, item.source, item.type,
                        item.kind, true });
            }
        }
    }
}

bool CommitLogicalSlotMutation(WidgetEngine& engine, LuaWidget& widget,
    snowdesktop::widget_runtime::LogicalSlotModel previousModel,
    std::unordered_map<std::string, std::string> previousStorage,
    const snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    auto& storage = ActiveStorage();
    widget.logicalSlots.Export(storage, WidgetWideToUtf8(widget.widgetId));
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        storage = std::move(previousStorage);
        widget.logicalSlots = std::move(previousModel);
        error = "logical slot persistence failed";
        return false;
    }
    widget.logicalSlotHistory.Record(std::move(previousModel), change);
    RebuildLogicalSlotReferences(widget);
    engine.RuntimeInvalidateHost(widget.widgetId);
    return true;
}
}

std::optional<LogicalSlotHostSurface>
WidgetEngine::RuntimeLogicalSlotSurface(const std::wstring& widgetId,
    std::string_view slotId) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        widget->manifest.apiVersion < 2 || !widget->viewTree)
        return std::nullopt;

    const auto declaration = widget->logicalSlots.Declarations().find(slotId);
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (declaration == widget->logicalSlots.Declarations().end() ||
        !snapshot)
        return std::nullopt;

    LogicalSlotHostSurface result;
    result.widgetId = widgetId;
    result.slotId = std::string(slotId);
    result.kind = snapshot->kind;
    result.revision = snapshot->revision;
    result.capacity = snapshot->capacity;
    result.itemCount = snapshot->items.size();
    result.allowClear = declaration->second.allowClear;
    result.accepts = declaration->second.accepts;
    result.replacePolicy = declaration->second.replacePolicy;

    const auto collect = [&](const auto& self, const ViewNode& node,
        std::optional<ViewRect> inheritedClip) -> bool {
        if (!node.visible) return false;
        if (node.type == ViewNodeType::SlotSurface &&
            node.logicalSlotId == slotId)
        {
            if (node.logicalSlotKind != snapshot->kind ||
                (node.logicalSlotRevision != 0 &&
                    node.logicalSlotRevision != snapshot->revision))
                return false;
            std::vector<const ViewNode*> sceneItems;
            for (const auto& child : node.children)
                if (child.type == ViewNodeType::SlotItem)
                    sceneItems.push_back(&child);
            if (snapshot->kind == LogicalSlotKind::Binding)
            {
                if ((!snapshot->items.empty() &&
                        (sceneItems.size() != 1 ||
                         node.children.size() != 1 ||
                         sceneItems.front()->key !=
                            snapshot->items.front().id ||
                         sceneItems.front()->logicalSlotReference !=
                            snapshot->items.front().reference)) ||
                    (snapshot->items.empty() && !sceneItems.empty()))
                    return false;
            }
            else
            {
                if (sceneItems.size() != snapshot->items.size() ||
                    node.children.size() != snapshot->items.size())
                    return false;
                for (std::size_t index = 0;
                    index < sceneItems.size(); ++index)
                {
                    if (sceneItems[index]->key != snapshot->items[index].id ||
                        sceneItems[index]->logicalSlotReference !=
                            snapshot->items[index].reference)
                        return false;
                }
            }
            const auto visibleSurface =
                IntersectLogicalSlotFrames(inheritedClip, node.frame);
            if (!visibleSurface) return false;
            result.bounds = LogicalSlotClientRect(*widget, *visibleSurface);
            if (result.bounds.right <= result.bounds.left ||
                result.bounds.bottom <= result.bounds.top)
                return false;

            for (const auto& child : node.children)
            {
                if (child.type != ViewNodeType::SlotItem || !child.visible)
                    continue;
                const auto visibleItem = IntersectLogicalSlotFrames(
                    visibleSurface, child.frame);
                if (!visibleItem) continue;
                RECT bounds = LogicalSlotClientRect(*widget, *visibleItem);
                if (bounds.right <= bounds.left ||
                    bounds.bottom <= bounds.top)
                    continue;
                result.items.push_back({ child.key, bounds });
            }
            return true;
        }

        std::optional<ViewRect> childClip = inheritedClip;
        if (LogicalSlotScrollContainer(node.type) && node.clipFrame)
            childClip = IntersectLogicalSlotFrames(
                inheritedClip, *node.clipFrame);
        if (LogicalSlotScrollContainer(node.type) && !childClip)
            return false;
        for (const auto& child : node.children)
            if (self(self, child, childClip)) return true;
        return false;
    };

    if (!collect(collect, *widget->viewTree, std::nullopt))
        return std::nullopt;
    return result;
}

bool WidgetEngine::RuntimeBindHostLogicalSlot(
    const std::wstring& widgetId, std::string_view slotId,
    snowdesktop::widget_runtime::LogicalSlotItem candidate,
    std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidateWidget) {
            return candidateWidget.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        widget->manifest.apiVersion < 2)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }

    candidate.available = true;
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Bind(slotId, std::move(candidate),
            change, error, targetIndex))
        return false;
    if (change.operation == "unchanged") return true;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;

    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

bool WidgetEngine::RuntimeOpenHostLogicalSlotPicker(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::string_view slotId, std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    const auto declaration = widget->logicalSlots.Declarations().find(slotId);
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (declaration == widget->logicalSlots.Declarations().end() ||
        !snapshot)
    {
        error = "logical slot is not declared";
        return false;
    }
    if (snapshot->kind == LogicalSlotKind::Collection &&
        snapshot->items.size() >= snapshot->capacity)
    {
        error = "logical slot is full";
        return false;
    }
    if (!logicalSlotPickerCallback_)
    {
        error = "hostPickerUnavailable";
        return false;
    }

    LogicalSlotPickerRequest request;
    request.widgetId = widgetId;
    request.slotId = std::string(slotId);
    request.kind = snapshot->kind;
    request.accepts = declaration->second.accepts;
    request.targetIndex = snapshot->kind == LogicalSlotKind::Collection
        ? snapshot->items.size() : 0;
    if (!logicalSlotPickerCallback_(request))
    {
        error = "hostPickerUnavailable";
        return false;
    }
    return true;
}

void WidgetEngine::DispatchHostLogicalSlotChange(LuaWidget& widget,
    const snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string_view source)
{
    const auto eventChange = change;
    const std::string eventSource(source);
    (void)InvokeLifecycleEvent(widget, "slot.changed",
        [eventChange, eventSource](lua_State* state) {
            lua_pushlstring(state, eventChange.slotId.data(),
                eventChange.slotId.size());
            lua_setfield(state, -2, "slotId");
            lua_pushstring(state,
                snowdesktop::widget_runtime::LogicalSlotKindName(
                    eventChange.kind));
            lua_setfield(state, -2, "slotKind");
            lua_pushinteger(state,
                static_cast<lua_Integer>(eventChange.revision));
            lua_setfield(state, -2, "revision");
            lua_pushlstring(state, eventChange.operation.data(),
                eventChange.operation.size());
            lua_setfield(state, -2, "operation");
            lua_createtable(state,
                static_cast<int>(eventChange.itemIds.size()), 0);
            for (std::size_t index = 0;
                index < eventChange.itemIds.size(); ++index)
            {
                lua_pushlstring(state, eventChange.itemIds[index].data(),
                    eventChange.itemIds[index].size());
                lua_rawseti(state, -2,
                    static_cast<lua_Integer>(index + 1));
            }
            lua_setfield(state, -2, "itemIds");
            lua_pushlstring(state, eventSource.data(), eventSource.size());
            lua_setfield(state, -2, "source");
        });
}

bool WidgetEngine::RuntimeRemoveHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view slotId,
    std::string_view itemId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        widget->manifest.apiVersion < 2)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (!snapshot)
    {
        error = "logical slot is not declared";
        return false;
    }
    const auto item = std::find_if(snapshot->items.begin(),
        snapshot->items.end(), [itemId](const auto& candidate) {
            return candidate.id == itemId;
        });
    if (item == snapshot->items.end())
    {
        error = "logical slot item does not exist";
        return false;
    }

    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    const bool changed = snapshot->kind == LogicalSlotKind::Binding
        ? widget->logicalSlots.Clear(slotId, change, error)
        : widget->logicalSlots.Remove(slotId, itemId, change, error);
    if (!changed) return false;
    if (change.operation == "unchanged") return true;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, "host.menu");
    return true;
}

bool WidgetEngine::RuntimeMoveHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view slotId,
    std::string_view itemId, std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        widget->manifest.apiVersion < 2)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Move(slotId, itemId, targetIndex,
            change, error))
        return false;
    if (change.operation == "unchanged") return true;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, "host.menu");
    return true;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotSnapshot>
WidgetEngine::RuntimeLogicalSlotSnapshot(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    snowdesktop::widget_runtime::LogicalSlotKind kind) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end()) return std::nullopt;
    const auto* snapshot = widget->logicalSlots.Find(slotId);
    if (!snapshot || snapshot->kind != kind) return std::nullopt;
    return *snapshot;
}

bool WidgetEngine::RuntimeBindLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    std::string_view reference,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview)
    {
        error = "previewReadOnly";
        return false;
    }
    if (!trustedGestureState_.Active())
    {
        error = "userGestureRequired";
        return false;
    }

    snowdesktop::widget_runtime::LogicalSlotItem candidate;
    if (const auto app = widget->applicationReferences.find(
            std::string(reference));
        app != widget->applicationReferences.end())
    {
        if (!app->second.persistent &&
            app->second.catalogRevision != appIndexRevision_)
        {
            error = "staleReference";
            return false;
        }
        candidate.kind = "app.reference";
        candidate.title = app->second.title.empty()
            ? app->second.launchTarget : app->second.title;
        candidate.source = app->second.source;
        candidate.type = app->second.type;
        candidate.target = app->second.launchTarget;
    }
    else if (const auto item = widget->itemReferences.find(
            std::string(reference));
        item != widget->itemReferences.end())
    {
        if (!item->second.persistent &&
            item->second.sourceTask == "desktop.search" &&
            item->second.revision != desktopDataRevision_)
        {
            error = "staleReference";
            return false;
        }
        candidate.kind = item->second.referenceKind;
        candidate.title = item->second.title.empty()
            ? item->second.target : item->second.title;
        candidate.source = item->second.source;
        candidate.type = item->second.type;
        candidate.target = item->second.target;
    }
    else
    {
        error = "invalidReference";
        return false;
    }
    candidate.available = true;

    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Bind(slotId, std::move(candidate),
            change, error))
        return false;
    if (change.operation == "unchanged") return true;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeClearLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Clear(slotId, change, error)) return false;
    if (change.operation == "unchanged") return true;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeRemoveLogicalSlotItem(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::string_view slotId, std::string_view itemId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Remove(slotId, itemId, change, error))
        return false;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeMoveLogicalSlotItem(const std::wstring& widgetId,
    std::uint64_t ownerToken, std::string_view slotId,
    std::string_view itemId, std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Move(slotId, itemId, targetIndex,
            change, error))
        return false;
    if (change.operation == "unchanged") return true;
    return CommitLogicalSlotMutation(*this, *widget,
        std::move(previousModel), std::move(previousStorage), change, error);
}

bool WidgetEngine::RuntimeCanUndoLogicalSlot(
    const std::wstring& widgetId, std::uint64_t ownerToken) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    return widget != widgets_.end() && !widget->preview &&
        widget->logicalSlotHistory.CanUndo();
}

bool WidgetEngine::RuntimeCanRedoLogicalSlot(
    const std::wstring& widgetId, std::uint64_t ownerToken) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    return widget != widgets_.end() && !widget->preview &&
        widget->logicalSlotHistory.CanRedo();
}

bool WidgetEngine::RuntimeCanUndoHostLogicalSlot(
    const std::wstring& widgetId) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    return widget != widgets_.end() && !widget->preview && widget->valid &&
        widget->manifest.apiVersion >= 2 &&
        widget->logicalSlotHistory.CanUndo();
}

bool WidgetEngine::RuntimeCanRedoHostLogicalSlot(
    const std::wstring& widgetId) const
{
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    return widget != widgets_.end() && !widget->preview && widget->valid &&
        widget->manifest.apiVersion >= 2 &&
        widget->logicalSlotHistory.CanRedo();
}

namespace
{
bool RestoreLogicalSlotHistory(WidgetEngine& engine, LuaWidget& widget,
    bool redo, snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    auto previousHistory = widget.logicalSlotHistory;
    auto currentModel = widget.logicalSlots;
    auto previousStorage = ActiveStorage();
    const bool restored = redo
        ? widget.logicalSlotHistory.Redo(widget.logicalSlots, change, error)
        : widget.logicalSlotHistory.Undo(widget.logicalSlots, change, error);
    if (!restored) return false;
    auto& storage = ActiveStorage();
    widget.logicalSlots.Export(storage, WidgetWideToUtf8(widget.widgetId));
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        storage = std::move(previousStorage);
        widget.logicalSlots = std::move(currentModel);
        widget.logicalSlotHistory = std::move(previousHistory);
        error = "logical slot persistence failed";
        return false;
    }
    RebuildLogicalSlotReferences(widget);
    engine.RuntimeInvalidateHost(widget.widgetId);
    return true;
}
}

bool WidgetEngine::RuntimeUndoLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    return RestoreLogicalSlotHistory(*this, *widget, false, change, error);
}

bool WidgetEngine::RuntimeRedoLogicalSlot(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (widget == widgets_.end())
    {
        error = "logical slot owner is stale";
        return false;
    }
    if (widget->preview || !trustedGestureState_.Active())
    {
        error = widget->preview ? "previewReadOnly" : "userGestureRequired";
        return false;
    }
    return RestoreLogicalSlotHistory(*this, *widget, true, change, error);
}

bool WidgetEngine::RuntimeUndoHostLogicalSlot(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        widget->manifest.apiVersion < 2)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    if (!RestoreLogicalSlotHistory(*this, *widget, false, change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, "host.keyboard");
    return true;
}

bool WidgetEngine::RuntimeRedoHostLogicalSlot(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid ||
        widget->manifest.apiVersion < 2)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    if (!RestoreLogicalSlotHistory(*this, *widget, true, change, error))
        return false;
    DispatchHostLogicalSlotChange(*widget, change, "host.keyboard");
    return true;
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

bool WidgetEngine::RuntimeSubmitInteractionRegion(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::InteractionRegion region,
    std::string& error)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || widgets_[index].manifest.apiVersion < 2)
    {
        error = "interaction regions require API v2";
        return false;
    }
    return widgets_[index].interactionRegions.Submit(
        std::move(region), error);
}

bool WidgetEngine::RuntimeInteractionHovered(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        widgets_[index].interactionRegions.IsHovered(key);
}

bool WidgetEngine::RuntimeInteractionPressed(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        widgets_[index].interactionRegions.IsPressed(key);
}

void WidgetEngine::DispatchInteractionAction(LuaWidget& widget,
    const std::string& targetKey, const char* eventName,
    int x, int y, int button, int delta, int clickCount,
    bool includeRetired)
{
    if (targetKey.empty() || !eventName || !*eventName) return;
    snowdesktop::widget_runtime::InteractionAction action;
    std::string resolvedEventName(eventName);
    std::optional<bool> previousChecked;
    std::optional<bool> checked;
    std::optional<float> previousControlValue;
    std::optional<float> controlValue;
    std::optional<std::string> previousSelection;
    std::optional<std::string> selection;
    std::optional<bool> previousExpanded;
    std::optional<bool> expanded;
    const bool trustedGesture = trustedGestureState_.Active();
    if (includeRetired)
    {
        const auto* actionPointer =
            widget.interactionRegions.FindTransitionAction(
                targetKey, eventName);
        if (!actionPointer) return;
        action = *actionPointer;
    }
    else
    {
        const auto resolved = widget.interactionRegions.ResolveAction(
            targetKey, eventName, static_cast<float>(x),
            static_cast<float>(y), button);
        if (!resolved) return;
        action = resolved->action;
        resolvedEventName = resolved->eventName;
        previousChecked = resolved->previousChecked;
        checked = resolved->checked;
        previousControlValue = resolved->previousControlValue;
        controlValue = resolved->controlValue;
        previousSelection = resolved->previousSelection;
        selection = resolved->selection;
        previousExpanded = resolved->previousExpanded;
        expanded = resolved->expanded;
    }
    (void)InvokeLifecycleEvent(widget, "action",
        [&action, &targetKey, &resolvedEventName,
            previousChecked, checked, previousControlValue, controlValue,
            previousSelection, selection, previousExpanded, expanded,
            trustedGesture, x, y, button,
            delta, clickCount](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushlstring(eventState, resolvedEventName.data(),
                resolvedEventName.size());
            lua_setfield(eventState, -2, "action");
            if (previousChecked.has_value() && checked.has_value())
            {
                lua_pushboolean(eventState, *previousChecked ? 1 : 0);
                lua_setfield(eventState, -2, "previousChecked");
                lua_pushboolean(eventState, *checked ? 1 : 0);
                lua_setfield(eventState, -2, "checked");
            }
            if (previousControlValue.has_value() &&
                controlValue.has_value())
            {
                lua_pushnumber(eventState, *previousControlValue);
                lua_setfield(eventState, -2, "previousControlValue");
                lua_pushnumber(eventState, *controlValue);
                lua_setfield(eventState, -2, "controlValue");
            }
            if (previousSelection.has_value() && selection.has_value())
            {
                lua_pushlstring(eventState, previousSelection->data(),
                    previousSelection->size());
                lua_setfield(eventState, -2, "previousSelection");
                lua_pushlstring(eventState, selection->data(),
                    selection->size());
                lua_setfield(eventState, -2, "selection");
            }
            if (previousExpanded.has_value() && expanded.has_value())
            {
                lua_pushboolean(eventState, *previousExpanded ? 1 : 0);
                lua_setfield(eventState, -2, "previousExpanded");
                lua_pushboolean(eventState, *expanded ? 1 : 0);
                lua_setfield(eventState, -2, "expanded");
            }
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushliteral(eventState, "pointer");
            lua_setfield(eventState, -2, "source");
            lua_pushliteral(eventState, "desktop");
            lua_setfield(eventState, -2, "surface");
            lua_pushinteger(eventState, x);
            lua_setfield(eventState, -2, "x");
            lua_pushinteger(eventState, y);
            lua_setfield(eventState, -2, "y");
            lua_pushinteger(eventState, button);
            lua_setfield(eventState, -2, "button");
            lua_pushinteger(eventState, delta);
            lua_setfield(eventState, -2, "delta");
            lua_pushinteger(eventState, clickCount);
            lua_setfield(eventState, -2, "clickCount");
        });
}

void WidgetEngine::DispatchHostInputChange(const std::wstring& widgetId,
    const std::string& targetKey,
    const snowdesktop::widget_runtime::InteractionAction& action,
    const std::wstring& previousText, const std::wstring& text,
    bool numeric, float minimum, float maximum,
    bool committed, bool cancelled, const char* source)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || action.id.empty()) return;
    auto parseNumber = [minimum, maximum](const std::wstring& value)
        -> std::optional<double> {
        if (value.empty()) return std::nullopt;
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(value.c_str(), &end);
        if (!end || end != value.c_str() + value.size() ||
            !std::isfinite(parsed) || parsed < minimum || parsed > maximum)
            return std::nullopt;
        return parsed;
    };
    const auto previousNumber = numeric
        ? parseNumber(previousText) : std::optional<double>{};
    const auto proposedNumber = numeric
        ? parseNumber(text) : std::optional<double>{};
    const std::string previousUtf8 = WidgetWideToUtf8(previousText);
    const std::string proposedUtf8 = WidgetWideToUtf8(text);
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "keyboard";
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, &previousUtf8, &proposedUtf8,
            numeric, previousNumber, proposedNumber, committed,
            cancelled, trustedGesture, &eventSource](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushliteral(eventState, "change");
            lua_setfield(eventState, -2, "action");
            lua_pushlstring(eventState, previousUtf8.data(),
                previousUtf8.size());
            lua_setfield(eventState, -2, "previousText");
            lua_pushlstring(eventState, proposedUtf8.data(),
                proposedUtf8.size());
            lua_setfield(eventState, -2, "text");
            if (numeric)
            {
                lua_pushboolean(eventState, proposedNumber.has_value());
                lua_setfield(eventState, -2, "numberValid");
                if (previousNumber)
                {
                    lua_pushnumber(eventState, *previousNumber);
                    lua_setfield(eventState, -2, "previousControlValue");
                }
                if (proposedNumber)
                {
                    lua_pushnumber(eventState, *proposedNumber);
                    lua_setfield(eventState, -2, "controlValue");
                }
            }
            lua_pushboolean(eventState, committed ? 1 : 0);
            lua_setfield(eventState, -2, "committed");
            lua_pushboolean(eventState, cancelled ? 1 : 0);
            lua_setfield(eventState, -2, "cancelled");
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(),
                eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushliteral(eventState, "desktop");
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchHostInputAction(const std::wstring& widgetId,
    const std::string& targetKey,
    const snowdesktop::widget_runtime::InteractionAction& action,
    const char* eventName, const std::wstring& text,
    bool cancelled, const char* source)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || action.id.empty() || !eventName || !*eventName)
        return;
    const std::string utf8 = WidgetWideToUtf8(text);
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string name(eventName);
    const std::string eventSource = source && *source ? source : "keyboard";
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, &utf8, &name, cancelled,
            trustedGesture, &eventSource](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushlstring(eventState, name.data(), name.size());
            lua_setfield(eventState, -2, "action");
            lua_pushlstring(eventState, utf8.data(), utf8.size());
            lua_setfield(eventState, -2, "text");
            lua_pushboolean(eventState, cancelled ? 1 : 0);
            lua_setfield(eventState, -2, "cancelled");
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(),
                eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushliteral(eventState, "desktop");
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchInteractionTransition(LuaWidget& widget,
    const snowdesktop::widget_runtime::InteractionHoverTransition& transition,
    int x, int y)
{
    if (!transition.leftKey.empty() &&
        transition.leftKey != transition.enteredKey)
    {
        DispatchInteractionAction(widget, transition.leftKey,
            "pointerLeave", x, y, 0, 0, 0, true);
    }
    if (!transition.enteredKey.empty() &&
        transition.enteredKey != transition.leftKey)
    {
        DispatchInteractionAction(widget, transition.enteredKey,
            "pointerEnter", x, y, 0, 0);
    }
}

void WidgetEngine::UpdateInteractionHover(
    const std::wstring& widgetId, int x, int y)
{
    for (auto& widget : widgets_)
    {
        if (widget.manifest.apiVersion < 2) continue;
        const auto transition = widget.widgetId == widgetId
            ? widget.interactionRegions.UpdateHover(
                static_cast<float>(x), static_cast<float>(y))
            : widget.interactionRegions.ClearHover();
        if (!transition.Changed()) continue;
        DispatchInteractionTransition(widget, transition, x, y);
        RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::ClearInteractionHover()
{
    for (auto& widget : widgets_)
    {
        if (widget.manifest.apiVersion < 2) continue;
        const auto transition = widget.interactionRegions.ClearHover();
        if (!transition.Changed()) continue;
        DispatchInteractionTransition(widget, transition, 0, 0);
        RuntimeInvalidateHost(widget.widgetId);
    }
}

std::string WidgetEngine::InteractionCursorAt(
    const std::wstring& widgetId, int x, int y) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return {};
    return widgets_[index].interactionRegions.CursorAt(
        static_cast<float>(x), static_cast<float>(y));
}

bool WidgetEngine::RuntimeCanWriteWidgetStorage(
    const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index < 0 || widgets_[index].manifest.apiVersion < 2 ||
        (!widgets_[index].interactionRegions.FrameOpen() &&
            !widgets_[index].panelFrameOpen);
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
    const std::string error = RuntimePostNotification(
        widgetId, title, message);
    if (!error.empty())
        RuntimeRecordError(widgetId,
            "Widget notification failed: " + error);
}

std::string WidgetEngine::RuntimePostNotification(
    const std::wstring& widgetId, const std::wstring& title,
    const std::wstring& message)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return {};
    const int index = FindWidget(widgetId);
    if (index < 0) return "instanceDisposed";
    if (title.empty() || message.empty()) return "invalidArguments";
    auto& widget = widgets_[index];
    const auto now = std::chrono::steady_clock::now();
    if (widget.notificationWindow.time_since_epoch().count() == 0 ||
        now - widget.notificationWindow >= std::chrono::minutes(1))
    {
        widget.notificationWindow = now;
        widget.notificationsInWindow = 0;
    }
    if (widget.notificationsInWindow >= 5)
        return "quotaExceeded";
    if (!notifyCallback_) return "providerUnavailable";
    ++widget.notificationsInWindow;
    try
    {
        notifyCallback_(title, message);
    }
    catch (...)
    {
        return "notificationFailed";
    }
    return {};
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

snowdesktop::widget_runtime::TaskStartResult
WidgetEngine::RuntimeStartTask(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::string name,
    std::unordered_map<std::string, std::string> arguments)
{
    using snowdesktop::widget_runtime::TaskStartOptions;
    if (!taskBroker_)
        return { 0, "task broker is not initialized" };
    const auto loaded = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (loaded == widgets_.end())
        return { 0, "widget instance is not loaded" };

    LuaWidget& widget = *loaded;
    if (name == "filesystem.pickFolder")
    {
        const auto access = arguments.find("access");
        if (access == arguments.end() ||
            (access->second != "read" && access->second != "write" &&
                access->second != "readWrite"))
            return { 0, "invalidArguments" };
        const bool needsRead = access->second != "write";
        const bool needsWrite = access->second != "read";
        if ((needsRead && !snowdesktop::widget::WidgetPermissionBroker::
                AllowsPermission(widget.permissions,
                    kFilesystemReadPermission)) ||
            (needsWrite && !snowdesktop::widget::WidgetPermissionBroker::
                AllowsPermission(widget.permissions,
                    kFilesystemWritePermission)))
            return { 0, "permissionDenied" };
    }
    else if (IsFilesystemHandleTask(name))
    {
        const auto handle = arguments.find("handle");
        if (handle == arguments.end()) return { 0, "invalidArguments" };
        const bool previewHandle = widget.preview &&
            IsPreviewFilesystemHandle(handle->second);
        std::optional<snowdesktop::widget_runtime::
            WidgetFilesystemHandleEntry> entry;
        if (!previewHandle)
        {
            if (!filesystemHandleStore_)
                return { 0, "providerUnavailable" };
            entry = filesystemHandleStore_->Resolve(
                { WidgetWideToUtf8(widget.widgetId), widget.packageId },
                handle->second);
            if (!entry) return { 0, "invalidReference" };
        }
        if (entry)
        {
            const bool readable = entry->access ==
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Read ||
                entry->access == snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::ReadWrite;
            const bool writable = entry->access ==
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleAccess::Write ||
                entry->access == snowdesktop::widget_runtime::
                    WidgetFilesystemHandleAccess::ReadWrite;
            if ((name == "filesystem.stat" ||
                    name == "filesystem.list" ||
                    name == "filesystem.read") && !readable)
                return { 0, "handleAccessDenied" };
            if (name == "filesystem.write" && !writable)
                return { 0, "handleAccessDenied" };
            if (name == "filesystem.list" && entry->kind !=
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::Folder)
                return { 0, "notFolder" };
            if ((name == "filesystem.read" ||
                    name == "filesystem.write") && entry->kind !=
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::File)
                return { 0, "notFile" };
        }
    }
    TaskStartOptions options;
    options.ownerToken = widget.runtimeToken;
    const auto requiredPermission =
        taskBroker_->RequiredPermission(name);
    options.permissionGranted = requiredPermission &&
        (requiredPermission->empty() ||
            snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
                widget.permissions, *requiredPermission));
    options.trustedGesture = trustedGestureState_.Active();
    options.preview = widget.preview;
    options.arguments = std::move(arguments);
    auto result = taskBroker_->Start(
        WidgetWideToUtf8(widgetId), name, options);
    if (result)
        widget.taskIds.insert(result.id);
    return result;
}

bool WidgetEngine::RuntimeCancelTask(
    const std::wstring& widgetId, std::uint64_t ownerToken,
    std::uint64_t taskId)
{
    if (!taskBroker_ || taskId == 0) return false;
    const auto loaded = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId, ownerToken](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId &&
                candidate.runtimeToken == ownerToken;
        });
    if (loaded == widgets_.end()) return false;
    LuaWidget& widget = *loaded;
    if (!widget.taskIds.contains(taskId)) return false;
    const auto snapshot = taskBroker_->Snapshot(taskId);
    if (!snapshot || snapshot->ownerToken != widget.runtimeToken)
        return false;
    const bool canceled = taskBroker_->Cancel(taskId);
    if (canceled && mediaTaskExecutor_)
        (void)mediaTaskExecutor_->Cancel(taskId);
    if (canceled && audioOutputTaskExecutor_)
        (void)audioOutputTaskExecutor_->Cancel(taskId);
    if (canceled && clipboardTaskExecutor_)
        (void)clipboardTaskExecutor_->Cancel(taskId);
    if (canceled && filesystemTaskExecutor_)
        (void)filesystemTaskExecutor_->Cancel(taskId);
    if (canceled && appTaskExecutor_)
        (void)appTaskExecutor_->Cancel(taskId);
    if (canceled && desktopTaskExecutor_)
        (void)desktopTaskExecutor_->Cancel(taskId);
    if (canceled && externalItemTaskExecutor_)
        (void)externalItemTaskExecutor_->Cancel(taskId);
    if (canceled)
        appSearchCompletions_.erase(taskId);
    if (canceled)
        itemSearchCompletions_.erase(taskId);
    if (canceled)
        calendarMutationCompletions_.erase(taskId);
    if (canceled)
        networkTaskCompletions_.erase(taskId);
    if (canceled)
        clipboardTaskCompletions_.erase(taskId);
    if (canceled)
        filesystemPickerCompletions_.erase(taskId);
    if (canceled)
        filesystemTaskCompletions_.erase(taskId);
    return canceled;
}

bool WidgetEngine::RuntimeSetTimer(const std::wstring& widgetId,
    const std::string& name, int intervalMs, bool repeat,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    int index = FindWidget(widgetId);
    if (index < 0 || name.empty()) return false;
    if (!widgets_[index].namedTimers.Set(
            name, intervalMs, repeat, std::chrono::steady_clock::now(),
            hiddenPolicy))
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

bool WidgetEngine::RuntimeSetTimerAt(const std::wstring& widgetId,
    const std::string& name, std::int64_t epochMilliseconds,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return false;
    int index = FindWidget(widgetId);
    if (index < 0 || name.empty()) return false;
    if (!widgets_[index].namedTimers.SetAt(name, epochMilliseconds,
            std::chrono::steady_clock::now(),
            std::chrono::system_clock::now(), hiddenPolicy))
    {
        return false;
    }
    RescheduleNamedTimer(widgets_[index]);
    return true;
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
        const int maximum = control.horizontal
            ? std::max(0, control.contentWidth - control.viewportWidth)
            : std::max(0, control.contentHeight - control.viewportHeight);
        int& offset = widgets_[index].scrollOffsets[control.id];
        offset = std::clamp(offset, 0, maximum);
    }
    if (control.type == LuaWidget::HostControl::Type::Input &&
        focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == control.id)
    {
        focusedHostInput_.controlled = control.controlled;
        focusedHostInput_.numeric = control.numeric;
        focusedHostInput_.changeAction = control.changeAction;
        focusedHostInput_.focusAction = control.focusAction;
        focusedHostInput_.blurAction = control.blurAction;
        focusedHostInput_.submitAction = control.submitAction;
        focusedHostInput_.liveUpdate = control.liveUpdate;
        focusedHostInput_.multiline = control.multiline;
        focusedHostInput_.maximumUtf8Bytes =
            control.maximumUtf8Bytes;
        focusedHostInput_.minimum = control.minimum;
        focusedHostInput_.maximum = control.maximum;
        focusedHostInput_.step = control.step;
    }
    widgets_[index].hostControls.push_back(std::move(control));
}

static bool HostControlContainsPoint(
    const LuaWidget::HostControl& control, POINT point) noexcept
{
    return PtInRect(&control.rect, point) != FALSE &&
        (!control.clipRect ||
            PtInRect(&*control.clipRect, point) != FALSE);
}

bool WidgetEngine::RuntimeRegisterV2HostControl(
    const std::wstring& widgetId, LuaWidget::HostControl control,
    std::string& error)
{
    error.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "widget instance is unavailable";
        return false;
    }
    auto& widget = widgets_[index];
    if (widget.manifest.apiVersion < 2)
    {
        error = "host text controls require API v2";
        return false;
    }
    if (!widget.interactionRegions.FrameOpen() && !widget.panelFrameOpen)
    {
        error = "host text controls may only be submitted during render or panel";
        return false;
    }
    if (widget.hostControls.size() >= 128)
    {
        error = "host control limit exceeded (128)";
        return false;
    }
    if (std::any_of(widget.hostControls.begin(),
            widget.hostControls.end(), [&](const auto& candidate) {
                return candidate.id == control.id;
            }))
    {
        error = "duplicate host control key: " + control.id;
        return false;
    }
    RuntimeRegisterHostControl(widgetId, std::move(control));
    return true;
}

bool WidgetEngine::RuntimeFocusHostInput(const std::wstring& widgetId, const std::string& id)
{
    int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return false;
    auto& controls = widgets_[index].hostControls;
    auto found = std::find_if(controls.rbegin(), controls.rend(), [&](const auto& control) {
        return control.type == LuaWidget::HostControl::Type::Input && control.id == id;
    });
    if (found == controls.rend() || !found->enabled ||
        (!found->controlled && found->storageKey.empty())) return false;

    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == id)
    {
        if (!found->controlled && found->liveUpdate)
        {
            const std::wstring storedText = Utf8ToWideLocal(
                RuntimeGetStorageValue(widgetId, found->storageKey));
            if (storedText != focusedHostInput_.text)
            {
                focusedHostInput_.text = storedText;
                focusedHostInput_.originalText = storedText;
                focusedHostInput_.cursor = storedText.size();
                focusedHostInput_.selectionAnchor =
                    found->selectAll ? 0 : storedText.size();
                focusedHostInput_.compositionText.clear();
                focusedHostInput_.compositionCursor = 0;
            }
        }
        focusedHostInput_.multiline = found->multiline;
        focusedHostInput_.controlled = found->controlled;
        focusedHostInput_.numeric = found->numeric;
        focusedHostInput_.changeAction = found->changeAction;
        focusedHostInput_.focusAction = found->focusAction;
        focusedHostInput_.blurAction = found->blurAction;
        focusedHostInput_.submitAction = found->submitAction;
        focusedHostInput_.minimum = found->minimum;
        focusedHostInput_.maximum = found->maximum;
        focusedHostInput_.step = found->step;
        focusedHostInput_.maximumUtf8Bytes =
            found->maximumUtf8Bytes;
        if (hostInputFocusCallback_)
            hostInputFocusCallback_();
        return true;
    }

    BlurHostInput(false);
    focusedHostInput_.active = true;
    focusedHostInput_.widgetId = widgetId;
    focusedHostInput_.id = id;
    focusedHostInput_.storageKey = found->storageKey;
    focusedHostInput_.text = Utf8ToWideLocal(found->controlled
        ? found->controlledText
        : RuntimeGetStorageValue(widgetId, found->storageKey));
    focusedHostInput_.originalText = focusedHostInput_.text;
    focusedHostInput_.cursor = focusedHostInput_.text.size();
    focusedHostInput_.selectionAnchor = found->selectAll
        ? 0 : focusedHostInput_.cursor;
    focusedHostInput_.liveUpdate = found->liveUpdate;
    focusedHostInput_.multiline = found->multiline;
    focusedHostInput_.controlled = found->controlled;
    focusedHostInput_.numeric = found->numeric;
    focusedHostInput_.changeAction = found->changeAction;
    focusedHostInput_.focusAction = found->focusAction;
    focusedHostInput_.blurAction = found->blurAction;
    focusedHostInput_.submitAction = found->submitAction;
    focusedHostInput_.minimum = found->minimum;
    focusedHostInput_.maximum = found->maximum;
    focusedHostInput_.step = found->step;
    focusedHostInput_.maximumUtf8Bytes =
        found->maximumUtf8Bytes;
    DispatchHostInputAction(widgetId, id,
        focusedHostInput_.focusAction, "focus",
        focusedHostInput_.text, false, "pointer");
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
            return HostControlContainsPoint(*it, point);
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
            it->enabled && HostControlContainsPoint(*it, point))
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
    if (focusedHostInput_.controlled)
    {
        if (cancel && focusedHostInput_.liveUpdate &&
            focusedHostInput_.text != focusedHostInput_.originalText)
        {
            DispatchHostInputChange(widgetId, focusedHostInput_.id,
                focusedHostInput_.changeAction, focusedHostInput_.text,
                focusedHostInput_.originalText,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, true, true, "keyboard");
        }
        else if (!cancel && !focusedHostInput_.liveUpdate &&
            focusedHostInput_.text != focusedHostInput_.originalText)
        {
            DispatchHostInputChange(widgetId, focusedHostInput_.id,
                focusedHostInput_.changeAction,
                focusedHostInput_.originalText, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, true, false, "commit");
        }
    }
    else if (cancel)
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
    DispatchHostInputAction(widgetId, focusedHostInput_.id,
        focusedHostInput_.blurAction, "blur",
        cancel ? focusedHostInput_.originalText : focusedHostInput_.text,
        cancel, "keyboard");
    focusedHostInput_ = {};
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::SetHostInputComposition(
    const std::wstring& text, size_t cursor)
{
    if (!focusedHostInput_.active)
        return false;
    focusedHostInput_.pendingHighSurrogate = 0;
    const size_t boundedCursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    const size_t boundedAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    const size_t selectionStart = std::min(
        boundedCursor, boundedAnchor);
    const size_t selectionEnd = std::max(
        boundedCursor, boundedAnchor);
    if (!snowdesktop::widget_runtime::HostTextReplacementFits(
            focusedHostInput_.text, selectionStart, selectionEnd,
            text, focusedHostInput_.maximumUtf8Bytes))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
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
    focusedHostInput_.pendingHighSurrogate = 0;

    const std::wstring previousText = focusedHostInput_.text;
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
    size_t nextCursor = focusedHostInput_.cursor;
    if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
            focusedHostInput_.text, selectionStart, selectionEnd,
            text, focusedHostInput_.maximumUtf8Bytes, nextCursor))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    focusedHostInput_.cursor = nextCursor;
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    if (focusedHostInput_.liveUpdate)
    {
        if (focusedHostInput_.controlled)
            DispatchHostInputChange(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.changeAction,
                previousText, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, false, false, "ime");
        else
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
    if (ch >= 0xD800 && ch <= 0xDBFF)
    {
        focusedHostInput_.pendingHighSurrogate = ch;
        return true;
    }
    std::wstring replacement;
    if (ch >= 0xDC00 && ch <= 0xDFFF &&
        focusedHostInput_.pendingHighSurrogate != 0)
    {
        replacement.push_back(
            focusedHostInput_.pendingHighSurrogate);
        replacement.push_back(ch);
    }
    else
    {
        replacement.push_back(ch);
    }
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;

    const std::wstring previousText = focusedHostInput_.text;
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
    size_t nextCursor = focusedHostInput_.cursor;
    if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
            focusedHostInput_.text, selectionStart, selectionEnd,
            replacement,
            focusedHostInput_.maximumUtf8Bytes, nextCursor))
    {
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    focusedHostInput_.cursor = nextCursor;
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    if (focusedHostInput_.liveUpdate)
    {
        if (focusedHostInput_.controlled)
            DispatchHostInputChange(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.changeAction,
                previousText, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, false, false, "keyboard");
        else
            RuntimeSetStorageValue(focusedHostInput_.widgetId,
                focusedHostInput_.storageKey,
                WidgetWideToUtf8(focusedHostInput_.text));
    }
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

bool WidgetEngine::HandleHostInputKey(WPARAM key)
{
    if (!focusedHostInput_.active) return false;
    focusedHostInput_.pendingHighSurrogate = 0;

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    focusedHostInput_.pointerSelecting = false;
    const std::wstring previousText = focusedHostInput_.text;
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
            DispatchHostInputAction(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.submitAction,
                "submit", focusedHostInput_.text, false, "keyboard");
            BlurHostInput(false);
            return true;
        }
        const size_t start = selectionStart();
        const size_t end = selectionEnd();
        size_t nextCursor = focusedHostInput_.cursor;
        if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
                focusedHostInput_.text, start, end, L"\n",
                focusedHostInput_.maximumUtf8Bytes, nextCursor))
        {
            RuntimeInvalidateHost(focusedHostInput_.widgetId);
            return true;
        }
        focusedHostInput_.cursor = nextCursor;
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
        bool pasteExceededLimit = false;
        const std::size_t pasteUnitLimit =
            focusedHostInput_.maximumUtf8Bytes == 0
            ? (std::numeric_limits<std::size_t>::max)()
            : focusedHostInput_.maximumUtf8Bytes + 1;
        if (OpenClipboard(nullptr))
        {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
            {
                if (const wchar_t* source = static_cast<const wchar_t*>(GlobalLock(data)))
                {
                    for (; *source; ++source)
                    {
                        if (*source == L'\r') continue;
                        const std::wstring_view normalized = *source == L'\n'
                            ? (focusedHostInput_.multiline
                                ? std::wstring_view(L"\n")
                                : std::wstring_view(L" "))
                            : (*source == L'\t'
                                ? (focusedHostInput_.multiline
                                    ? std::wstring_view(L"    ")
                                    : std::wstring_view(L" "))
                                : std::wstring_view(source, 1));
                        if (normalized.size() > pasteUnitLimit -
                                std::min(pasted.size(), pasteUnitLimit))
                        {
                            pasteExceededLimit = true;
                            break;
                        }
                        pasted.append(normalized);
                    }
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!pasted.empty() && !pasteExceededLimit)
        {
            const size_t start = selectionStart();
            const size_t end = selectionEnd();
            size_t nextCursor = focusedHostInput_.cursor;
            if (snowdesktop::widget_runtime::TryApplyHostTextReplacement(
                    focusedHostInput_.text, start, end, pasted,
                    focusedHostInput_.maximumUtf8Bytes, nextCursor))
            {
                focusedHostInput_.cursor = nextCursor;
                focusedHostInput_.selectionAnchor =
                    focusedHostInput_.cursor;
                changed = true;
            }
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
    else if (focusedHostInput_.numeric &&
        (key == VK_UP || key == VK_DOWN))
    {
        wchar_t* end = nullptr;
        double current = std::wcstod(
            focusedHostInput_.text.c_str(), &end);
        if (!end || end != focusedHostInput_.text.c_str() +
                focusedHostInput_.text.size() || !std::isfinite(current))
            current = focusedHostInput_.minimum;
        const double direction = key == VK_UP ? 1.0 : -1.0;
        const double next = std::clamp(
            current + direction * focusedHostInput_.step,
            static_cast<double>(focusedHostInput_.minimum),
            static_cast<double>(focusedHostInput_.maximum));
        focusedHostInput_.text = std::to_wstring(next);
        while (focusedHostInput_.text.size() > 1 &&
            focusedHostInput_.text.back() == L'0')
            focusedHostInput_.text.pop_back();
        if (!focusedHostInput_.text.empty() &&
            focusedHostInput_.text.back() == L'.')
            focusedHostInput_.text.pop_back();
        focusedHostInput_.cursor = focusedHostInput_.text.size();
        focusedHostInput_.selectionAnchor = focusedHostInput_.cursor;
        changed = focusedHostInput_.text != previousText;
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
        {
            if (focusedHostInput_.controlled)
                DispatchHostInputChange(focusedHostInput_.widgetId,
                    focusedHostInput_.id, focusedHostInput_.changeAction,
                    previousText, focusedHostInput_.text,
                    focusedHostInput_.numeric, focusedHostInput_.minimum,
                    focusedHostInput_.maximum, false, false, "keyboard");
            else
                RuntimeSetStorageValue(focusedHostInput_.widgetId,
                    focusedHostInput_.storageKey,
                    WidgetWideToUtf8(focusedHostInput_.text));
        }
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
        maximum = it->horizontal
            ? std::max(0, it->contentWidth - it->viewportWidth)
            : std::max(0, it->contentHeight - it->viewportHeight);
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
        if (!it->enabled || !HostControlContainsPoint(*it, point)) continue;
        if (wheel &&
            (it->type == LuaWidget::HostControl::Type::Scroll ||
                (it->type ==
                    LuaWidget::HostControl::Type::Input &&
                    it->multiline)))
        {
            const int maximum = it->horizontal
                ? std::max(0, it->contentWidth - it->viewportWidth)
                : std::max(0, it->contentHeight - it->viewportHeight);
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
            snowdesktop::widget_runtime::WidgetTrustedGestureScope
                gestureScope(trustedGestureState_, true);
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

void WidgetEngine::SetWidgetSurfaceContext(
    const std::wstring& widgetId,
    const LuaWidgetSurfaceContext& context)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    widgets_[index].surfaceContext = context;
    widgets_[index].surfaceContext.dpiX =
        std::max<UINT>(USER_DEFAULT_SCREEN_DPI, context.dpiX);
    widgets_[index].surfaceContext.dpiY =
        std::max<UINT>(USER_DEFAULT_SCREEN_DPI, context.dpiY);
}

LuaWidgetContextState WidgetEngine::RuntimeGetWidgetContextState(
    const std::wstring& widgetId) const
{
    LuaWidgetContextState result;
    const int index = FindWidget(widgetId);
    if (index < 0) return result;
    const LuaWidget& widget = widgets_[index];
    result.surface = widget.surfaceContext;
    result.visible = widget.hostVisible;
    result.preview = widget.preview;
    result.focused = focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId;
    result.selected = widgetSelectedProvider_
        ? widgetSelectedProvider_(widgetId) : false;
    return result;
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
        for (const auto& permission : widget.manifest.optionalPermissions)
        {
            if (std::find(entry.permissions.begin(),
                    entry.permissions.end(), permission) ==
                entry.permissions.end())
            {
                entry.permissions.push_back(permission);
            }
        }
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

static std::optional<std::wstring> ResolvePackageAssetWithinRoot(
    const std::filesystem::path& packageRoot,
    const std::wstring& relativePath)
{
    if (!snowdesktop::widget::WidgetPackageValidator::IsSafeRelativePath(
        std::filesystem::path(relativePath)))
        return std::nullopt;
    if (packageRoot.empty())
        return std::nullopt;
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        packageRoot, error);
    if (error) return std::nullopt;
    const DWORD rootAttributes = GetFileAttributesW(root.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
        (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return std::nullopt;
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

std::optional<std::wstring> WidgetEngine::RuntimeResolvePackageAsset(
    const std::wstring& widgetId, const std::wstring& relativePath) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return std::nullopt;
    return ResolvePackageAssetWithinRoot(
        widgets_[index].packageRoot, relativePath);
}

std::optional<std::wstring> WidgetEngine::RuntimeResolvePackageResource(
    const std::wstring& widgetId, std::string_view name,
    std::string_view expectedType) const
{
    const int index = FindWidget(widgetId);
    if (index < 0 || name.empty() || expectedType.empty())
        return std::nullopt;
    const auto resource = widgets_[index].manifest.resources.find(
        std::string(name));
    if (resource == widgets_[index].manifest.resources.end() ||
        resource->second.type != expectedType)
        return std::nullopt;
    return ResolvePackageAssetWithinRoot(widgets_[index].packageRoot,
        Utf8ToWideLocal(resource->second.path));
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
    manifest.optionalPermissions =
        readStringArray(root, "optionalPermissions");
    manifest.networkDomains = readStringArray(root, "networkDomains");
    manifest.requiredFeatures = readStringArray(root, "requiredFeatures");
    manifest.optionalFeatures = readStringArray(root, "optionalFeatures");
    {
        std::vector<snowdesktop::widget_runtime::
            LogicalSlotManifestError> slotErrors;
        (void)snowdesktop::widget_runtime::ParseLogicalSlotDeclarations(
            root.Find("slots"), manifest.logicalSlots, slotErrors);
    }
    if (const JsonValue* resources = root.Find("resources");
        resources && resources->IsObject())
    {
        for (const auto& [name, value] : resources->object)
        {
            if (!value.IsObject() || name.empty()) continue;
            snowdesktop::widget::PackageResource resource;
            readString(value, "type", resource.type);
            readString(value, "path", resource.path);
            readString(value, "license", resource.license);
            if ((resource.type == "image" || resource.type == "font") &&
                !resource.path.empty())
                manifest.resources.emplace(name, std::move(resource));
        }
    }
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
            readString(object, "searchKey", setting.searchKey);
            readString(object, "emptyLabel", setting.emptyLabel);
            readString(object, "noResultsLabel",
                setting.noResultsLabel);
            if (const JsonValue* defaultValue = object.Find("default"))
                setting.defaultValue = valueString(*defaultValue);
            readNumber(object, "min", setting.minValue);
            readNumber(object, "max", setting.maxValue);
            setting.options = readStringArray(object, "options");
            setting.optionLabels =
                readStringArray(object, "optionLabels");
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

std::vector<snowdesktop::widget::SteamWorkshopInstallFailure>
WidgetEngine::CachedSteamWorkshopInstallFailures()
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    return cache.installFailures;
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
    result.installFailures = snapshot.discoveryFailures;
    for (const auto& failure : snapshot.discoveryFailures)
    {
        result.errors.push_back(failure.packageId + ": " + failure.error);
    }
    result.errors.insert(result.errors.end(),
        snapshot.preparationErrors.begin(), snapshot.preparationErrors.end());
    for (const auto& action : plan.actions)
    {
        const auto recordInstallFailure = [&](std::string error)
        {
            snowdesktop::widget::SteamWorkshopInstallFailure failure;
            failure.packageId = action.packageId;
            failure.externalItemId = action.externalItemId;
            failure.manifest = action.expectedManifest;
            failure.error = std::move(error);
            result.installFailures.push_back(std::move(failure));
        };
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
                if (filesystemHandleStore_)
                {
                    std::string revokeError;
                    (void)filesystemHandleStore_->RevokePackage(
                        action.packageId, revokeError);
                }
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
            const std::string error =
                "detected Workshop package was not prepared locally";
            result.errors.push_back(action.packageId + ": " + error);
            recordInstallFailure(error);
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
            std::string displayError;
            if (!installedOk)
            {
                error = Utf8ToWideLocal(installError);
                if (!report.Ok())
                    error += L"\n" + Utf8ToWideLocal(report.ToJson());
                const auto issue = std::find_if(report.issues.begin(),
                    report.issues.end(), [](const auto& candidate)
                    {
                        return candidate.severity == snowdesktop::widget::
                            ValidationSeverity::Error;
                    });
                if (issue != report.issues.end())
                    displayError = "[" + issue->code + "] " +
                        issue->message;
            }
            const std::string fullError = WidgetWideToUtf8(error);
            if (displayError.empty()) displayError = fullError;
            result.errors.push_back(action.packageId + ": " + fullError);
            recordInstallFailure(std::move(displayError));
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
    if (snapshot.authoritative)
    {
        auto& cache = GetSteamWorkshopPackageAssociationCache();
        std::lock_guard lock(cache.mutex);
        cache.installFailures = result.installFailures;
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
    const auto details = version.empty()
        ? source->second->GetDetails(externalItemId, sourceError)
        : source->second->GetVersionDetails(
            externalItemId, version, sourceError);
    if (!details)
    {
        error = Utf8ToWideLocal(sourceError);
        return false;
    }
    const std::string requestedVersion = version.empty()
        ? details->manifest.version : version;
    std::optional<std::string> previousVersion;
    auto& manager = GetWidgetPackageManager();
    if (const auto previous = manager.Resolve(details->manifest.id))
        if (!previous->builtin && !previous->development)
            previousVersion = previous->manifest.version;
    snowdesktop::widget::InstalledPackage installed;
    snowdesktop::widget::ValidationReport report;
    if (!manager.InstallFromSource(*source->second, externalItemId,
        requestedVersion,
        allowSourceChange, installed, report, sourceError,
        allowPermissionExpansion))
    {
        error = Utf8ToWideLocal(sourceError);
        if (!report.Ok()) error += L"\n" + Utf8ToWideLocal(report.ToJson());
        if (providerId == "steam-workshop")
        {
            std::string displayError = sourceError;
            const auto issue = std::find_if(report.issues.begin(),
                report.issues.end(), [](const auto& candidate)
                {
                    return candidate.severity == snowdesktop::widget::
                        ValidationSeverity::Error;
                });
            if (issue != report.issues.end())
                displayError = "[" + issue->code + "] " + issue->message;
            CacheSteamWorkshopInstallFailure({ details->manifest.id,
                externalItemId, details->manifest,
                std::move(displayError) });
        }
        return false;
    }
    const bool verified = VerifyInstalledWidgetPackage(
        installed.manifest.id, previousVersion, error);
    if (providerId == "steam-workshop")
    {
        if (verified)
        {
            ClearSteamWorkshopInstallFailure(installed.manifest.id);
        }
        else
        {
            CacheSteamWorkshopInstallFailure({ details->manifest.id,
                externalItemId, details->manifest,
                WidgetWideToUtf8(error) });
        }
    }
    return verified;
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

std::vector<snowdesktop::widget::InvalidPackage>
WidgetEngine::ListInvalidWidgetPackages()
{
    return GetWidgetPackageManager().ListInvalidPackages();
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

bool WidgetEngine::ApplyWidgetPermissionDecision(
    const std::wstring& packageId,
    snowdesktop::widget::PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error)
{
    if (!SetWidgetPermissionDecision(packageId, state,
            grantedPermissions, grantedNetworkDomains, error))
        return false;
    const std::string requestedId = WidgetWideToUtf8(packageId);
    const std::unordered_set<std::string> effectivePermissions =
        state == snowdesktop::widget::PermissionDecisionState::Granted
        ? std::unordered_set<std::string>(
            grantedPermissions.begin(), grantedPermissions.end())
        : std::unordered_set<std::string>{};
    std::vector<std::wstring> instances;
    for (const auto& widget : widgets_)
    {
        if (widget.packageId == requestedId)
        {
            instances.push_back(widget.widgetId);
            if (taskBroker_)
            {
                const std::string instanceId =
                    WidgetWideToUtf8(widget.widgetId);
                for (const std::string& permission : widget.permissions)
                {
                    if (!effectivePermissions.contains(permission))
                    {
                        (void)taskBroker_->SetPermission(
                            instanceId, permission, false);
                    }
                }
            }
        }
    }
    for (const auto& instance : instances)
        UnloadWidget(instance);
    if (state == snowdesktop::widget::PermissionDecisionState::Granted)
    {
        for (const auto& instance : instances)
            (void)EnsureWidgetLoaded(instance, packageId);
    }
    return true;
}

void WidgetEngine::ResolveDeferredHostInputFocus(
    const std::wstring& widgetId, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& request = widgets_[index].deferredHostInputFocus;
    if (!request.MatchesSurface(surface)) return;
    const std::string controlId = request.ControlId();
    request.Clear();
    if (!RuntimeFocusHostInput(widgetId, controlId))
    {
        RuntimeRecordError(widgetId,
            "Deferred control.focus target was not submitted: " + controlId);
    }
}

bool WidgetEngine::RuntimeFocusHostInputFromTrustedGesture(
    const std::wstring& widgetId, const std::string& id,
    std::string& error)
{
    error.clear();
    if (!trustedGestureState_.Active())
    {
        error = "trustedGestureRequired";
        return false;
    }
    if (RuntimeFocusHostInput(widgetId, id))
        return true;

    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    const auto& controls = widgets_[index].hostControls;
    const bool submittedWithKey = std::any_of(
        controls.begin(), controls.end(), [&](const auto& control) {
            return control.id == id;
        });
    if (submittedWithKey)
    {
        error = "controlNotFound";
        return false;
    }
    const char* surface = d2dState_ && d2dState_->surfaceKind
        ? d2dState_->surfaceKind : "desktop";
    widgets_[index].deferredHostInputFocus.Request(id, surface);
    RuntimeInvalidateHost(widgetId);
    return true;
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

namespace
{
constexpr char kLogicalSlotMetatable[] = "SnowDesktop.LogicalSlot";

struct LuaLogicalSlotHandle
{
    std::uint64_t ownerToken = 0;
    snowdesktop::widget_runtime::LogicalSlotKind kind =
        snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    std::array<char, 65> id{};
};

LuaLogicalSlotHandle* CheckLogicalSlotHandle(lua_State* state,
    snowdesktop::widget_runtime::LogicalSlotKind* requiredKind = nullptr)
{
    auto* handle = static_cast<LuaLogicalSlotHandle*>(
        luaL_checkudata(state, 1, kLogicalSlotMetatable));
    if (!handle || handle->ownerToken == 0 || handle->id[0] == '\0')
    {
        luaL_error(state, "logical slot handle is closed");
        return nullptr;
    }
    if (requiredKind && handle->kind != *requiredKind)
    {
        luaL_error(state, "logical slot handle has the wrong kind");
        return nullptr;
    }
    return handle;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotSnapshot>
LogicalSlotHandleSnapshot(lua_State* state,
    LuaLogicalSlotHandle* handle)
{
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine || !handle) return std::nullopt;
    return d2d->engine->RuntimeLogicalSlotSnapshot(
        BoundWidgetId(state), handle->ownerToken, handle->id.data(),
        handle->kind);
}

void PushLogicalSlotItem(lua_State* state,
    const snowdesktop::widget_runtime::LogicalSlotItem& item)
{
    lua_createtable(state, 0, 7);
    lua_pushlstring(state, item.id.data(), item.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, item.reference.data(), item.reference.size());
    lua_setfield(state, -2, "reference");
    lua_pushlstring(state, item.kind.data(), item.kind.size());
    lua_setfield(state, -2, "kind");
    lua_pushlstring(state, item.title.data(), item.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, item.source.data(), item.source.size());
    lua_setfield(state, -2, "source");
    lua_pushlstring(state, item.type.data(), item.type.size());
    lua_setfield(state, -2, "type");
    lua_pushstring(state, item.available ? "available" : "unavailable");
    lua_setfield(state, -2, "availability");
}

void PushLogicalSlotChange(lua_State* state,
    const snowdesktop::widget_runtime::LogicalSlotChange& change)
{
    lua_createtable(state, 0, 5);
    lua_pushlstring(state, change.slotId.data(), change.slotId.size());
    lua_setfield(state, -2, "slotId");
    lua_pushstring(state,
        snowdesktop::widget_runtime::LogicalSlotKindName(change.kind));
    lua_setfield(state, -2, "kind");
    lua_pushinteger(state, static_cast<lua_Integer>(change.revision));
    lua_setfield(state, -2, "revision");
    lua_pushlstring(state, change.operation.data(), change.operation.size());
    lua_setfield(state, -2, "operation");
    lua_createtable(state, static_cast<int>(change.itemIds.size()), 0);
    for (std::size_t index = 0; index < change.itemIds.size(); ++index)
    {
        lua_pushlstring(state, change.itemIds[index].data(),
            change.itemIds[index].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    lua_setfield(state, -2, "itemIds");
}

static int lua_LogicalSlotId(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    lua_pushstring(state, handle->id.data());
    return 1;
}

static int lua_LogicalSlotRevision(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    lua_pushinteger(state, static_cast<lua_Integer>(snapshot->revision));
    return 1;
}

static int lua_LogicalSlotState(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    const bool unavailable = std::any_of(snapshot->items.begin(),
        snapshot->items.end(), [](const auto& item) {
            return !item.available;
        });
    lua_pushstring(state, unavailable ? "unavailable" :
        (snapshot->items.empty() ? "empty" : "bound"));
    return 1;
}

static int lua_LogicalSlotCapacity(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    lua_pushinteger(state, static_cast<lua_Integer>(snapshot->capacity));
    return 1;
}

static int lua_LogicalSlotItem(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    if (snapshot->items.empty()) lua_pushnil(state);
    else PushLogicalSlotItem(state, snapshot->items.front());
    return 1;
}

static int lua_LogicalSlotItems(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    const auto snapshot = LogicalSlotHandleSnapshot(state, handle);
    if (!snapshot) return luaL_error(state, "logical slot handle is stale");
    lua_createtable(state, static_cast<int>(snapshot->items.size()), 0);
    for (std::size_t index = 0; index < snapshot->items.size(); ++index)
    {
        PushLogicalSlotItem(state, snapshot->items[index]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

static int lua_LogicalSlotBind(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    std::size_t length = 0;
    const char* reference = luaL_checklstring(state, 2, &length);
    if (length == 0 || length > 128)
        return luaL_error(state, "logical slot reference is invalid");
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!d2d->engine->RuntimeBindLogicalSlot(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(),
            std::string_view(reference, length), change, error))
        return luaL_error(state, "slots.bind: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotClear(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!d2d->engine->RuntimeClearLogicalSlot(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(), change, error))
        return luaL_error(state, "slots.clear: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotRemove(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    std::size_t length = 0;
    const char* itemId = luaL_checklstring(state, 2, &length);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!d2d->engine->RuntimeRemoveLogicalSlotItem(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(),
            std::string_view(itemId, length), change, error))
        return luaL_error(state, "slots.remove: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotMove(lua_State* state)
{
    auto kind = snowdesktop::widget_runtime::LogicalSlotKind::Collection;
    auto* handle = CheckLogicalSlotHandle(state, &kind);
    std::size_t length = 0;
    const char* itemId = luaL_checklstring(state, 2, &length);
    const lua_Integer oneBasedIndex = luaL_checkinteger(state, 3);
    if (oneBasedIndex <= 0)
        return luaL_error(state, "slots.move: index must be positive");
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!d2d->engine->RuntimeMoveLogicalSlotItem(BoundWidgetId(state),
            handle->ownerToken, handle->id.data(),
            std::string_view(itemId, length),
            static_cast<std::size_t>(oneBasedIndex - 1), change, error))
        return luaL_error(state, "slots.move: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_LogicalSlotPick(lua_State* state)
{
    auto* handle = CheckLogicalSlotHandle(state);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "logical slot host is unavailable");
    std::string error;
    if (!d2d->engine->RuntimeOpenHostLogicalSlotPicker(
            BoundWidgetId(state), handle->ownerToken,
            handle->id.data(), error))
        return luaL_error(state, "slots.pick: %s", error.c_str());
    lua_pushboolean(state, 1);
    return 1;
}

void PushLogicalSlotHandle(lua_State* state,
    snowdesktop::widget_runtime::LogicalSlotKind kind)
{
    std::size_t length = 0;
    const char* id = luaL_checklstring(state, 1, &length);
    if (length == 0 || length > 64)
    {
        luaL_error(state, "logical slot id is invalid");
        return;
    }
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine || ownerToken == 0 ||
        !d2d->engine->RuntimeLogicalSlotSnapshot(BoundWidgetId(state),
            ownerToken, std::string_view(id, length), kind))
    {
        luaL_error(state, "logical slot is undeclared or has the wrong kind");
        return;
    }
    auto* handle = static_cast<LuaLogicalSlotHandle*>(
        lua_newuserdatauv(state, sizeof(LuaLogicalSlotHandle), 0));
    *handle = {};
    handle->ownerToken = ownerToken;
    handle->kind = kind;
    std::copy_n(id, length, handle->id.begin());
    handle->id[length] = '\0';
    if (luaL_newmetatable(state, kLogicalSlotMetatable) != 0)
    {
        lua_newtable(state);
        const luaL_Reg methods[] = {
            { "id", lua_LogicalSlotId },
            { "revision", lua_LogicalSlotRevision },
            { "state", lua_LogicalSlotState },
            { "capacity", lua_LogicalSlotCapacity },
            { "item", lua_LogicalSlotItem },
            { "items", lua_LogicalSlotItems },
            { "bind", lua_LogicalSlotBind },
            { "add", lua_LogicalSlotBind },
            { "clear", lua_LogicalSlotClear },
            { "remove", lua_LogicalSlotRemove },
            { "move", lua_LogicalSlotMove },
            { "pick", lua_LogicalSlotPick },
            { nullptr, nullptr },
        };
        luaL_setfuncs(state, methods, 0);
        lua_setfield(state, -2, "__index");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}

static int lua_SlotsBinding(lua_State* state)
{
    PushLogicalSlotHandle(state,
        snowdesktop::widget_runtime::LogicalSlotKind::Binding);
    return 1;
}

static int lua_SlotsCollection(lua_State* state)
{
    PushLogicalSlotHandle(state,
        snowdesktop::widget_runtime::LogicalSlotKind::Collection);
    return 1;
}

static int lua_SlotsCanUndo(lua_State* state)
{
    auto* d2d = GetD2D(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    lua_pushboolean(state, d2d && d2d->engine && ownerToken != 0 &&
        d2d->engine->RuntimeCanUndoLogicalSlot(
            BoundWidgetId(state), ownerToken));
    return 1;
}

static int lua_SlotsCanRedo(lua_State* state)
{
    auto* d2d = GetD2D(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    lua_pushboolean(state, d2d && d2d->engine && ownerToken != 0 &&
        d2d->engine->RuntimeCanRedoLogicalSlot(
            BoundWidgetId(state), ownerToken));
    return 1;
}

static int lua_SlotsUndo(lua_State* state)
{
    auto* d2d = GetD2D(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    if (!d2d || !d2d->engine || ownerToken == 0)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!d2d->engine->RuntimeUndoLogicalSlot(BoundWidgetId(state),
            ownerToken, change, error))
        return luaL_error(state, "slots.undo: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

static int lua_SlotsRedo(lua_State* state)
{
    auto* d2d = GetD2D(state);
    const std::uint64_t ownerToken = BoundWidgetRuntimeToken(state);
    if (!d2d || !d2d->engine || ownerToken == 0)
        return luaL_error(state, "logical slot host is unavailable");
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!d2d->engine->RuntimeRedoLogicalSlot(BoundWidgetId(state),
            ownerToken, change, error))
        return luaL_error(state, "slots.redo: %s", error.c_str());
    PushLogicalSlotChange(state, change);
    return 1;
}

constexpr char kStorageTransactionMetatable[] =
    "SnowDesktop.StorageTransaction";
char kActiveStorageTransactionRegistryKey = 0;

struct LuaStorageTransactionHandle
{
    snowdesktop::widget_runtime::WidgetStorageTransaction* transaction =
        nullptr;
};

static snowdesktop::widget_runtime::WidgetStorageTransaction*
ActiveLuaStorageTransaction(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX,
        &kActiveStorageTransactionRegistryKey);
    auto* transaction = static_cast<
        snowdesktop::widget_runtime::WidgetStorageTransaction*>(
            lua_touserdata(state, -1));
    lua_pop(state, 1);
    return transaction;
}

static void SetActiveLuaStorageTransaction(lua_State* state,
    snowdesktop::widget_runtime::WidgetStorageTransaction* transaction)
{
    if (transaction)
        lua_pushlightuserdata(state, transaction);
    else
        lua_pushnil(state);
    lua_rawsetp(state, LUA_REGISTRYINDEX,
        &kActiveStorageTransactionRegistryKey);
}

static std::string ReadExactStorageString(
    lua_State* state, int index, const char* field)
{
    if (lua_type(state, index) != LUA_TSTRING)
    {
        luaL_error(state, "storage.transaction: %s must be a string",
            field);
        return {};
    }
    std::size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    return std::string(value ? value : "", length);
}

static LuaStorageTransactionHandle* CheckStorageTransactionHandle(
    lua_State* state)
{
    auto* handle = static_cast<LuaStorageTransactionHandle*>(
        luaL_checkudata(state, 1, kStorageTransactionMetatable));
    if (!handle || !handle->transaction ||
        ActiveLuaStorageTransaction(state) != handle->transaction)
    {
        luaL_error(state, "storage transaction is closed");
        return nullptr;
    }
    return handle;
}

static bool IsReservedStorageTransactionKey(const std::string& key)
{
    return IsRemovedPanelEffectSettingKey(key) ||
        key == "__host" || key.starts_with("__host.");
}

static int lua_StorageTransactionGet(lua_State* state)
{
    auto* handle = CheckStorageTransactionHandle(state);
    const std::string key = ReadExactStorageString(state, 2, "key");
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(state, "storage.transaction: key is reserved");
    std::string error;
    const auto value = handle->transaction->Get(key, error);
    if (!error.empty())
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    if (value)
        lua_pushlstring(state, value->data(), value->size());
    else
        lua_pushnil(state);
    return 1;
}

static int lua_StorageTransactionSet(lua_State* state)
{
    auto* handle = CheckStorageTransactionHandle(state);
    std::string key = ReadExactStorageString(state, 2, "key");
    std::string value = ReadExactStorageString(state, 3, "value");
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(state, "storage.transaction: key is reserved");
    bool changed = false;
    std::string error;
    if (!handle->transaction->Set(
            std::move(key), std::move(value), changed, error))
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    lua_pushboolean(state, changed ? 1 : 0);
    return 1;
}

static int lua_StorageTransactionRemove(lua_State* state)
{
    auto* handle = CheckStorageTransactionHandle(state);
    const std::string key = ReadExactStorageString(state, 2, "key");
    if (IsReservedStorageTransactionKey(key))
        return luaL_error(state, "storage.transaction: key is reserved");
    bool changed = false;
    std::string error;
    if (!handle->transaction->Remove(key, changed, error))
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    lua_pushboolean(state, changed ? 1 : 0);
    return 1;
}

static void PushStorageTransactionHandle(lua_State* state,
    snowdesktop::widget_runtime::WidgetStorageTransaction* transaction)
{
    auto* handle = static_cast<LuaStorageTransactionHandle*>(
        lua_newuserdatauv(state, sizeof(LuaStorageTransactionHandle), 0));
    handle->transaction = transaction;
    if (luaL_newmetatable(state, kStorageTransactionMetatable) != 0)
    {
        lua_newtable(state);
        lua_pushcfunction(state, lua_StorageTransactionGet);
        lua_setfield(state, -2, "get");
        lua_pushcfunction(state, lua_StorageTransactionSet);
        lua_setfield(state, -2, "set");
        lua_pushcfunction(state, lua_StorageTransactionRemove);
        lua_setfield(state, -2, "remove");
        lua_setfield(state, -2, "__index");
        lua_pushboolean(state, 0);
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}

static int RejectStorageAccessInsideTransaction(lua_State* state,
    const char* api)
{
    if (BoundWidgetApiVersion(state) >= 2 &&
        ActiveLuaStorageTransaction(state))
    {
        return luaL_error(state,
            "%s cannot be used inside storage.transaction; use tx instead",
            api);
    }
    return 0;
}

static int RejectStorageWriteDuringRender(lua_State* state,
    const char* api)
{
    if (BoundWidgetApiVersion(state) < 2) return 0;
    auto* d2d = GetD2D(state);
    if (d2d && d2d->engine &&
        !d2d->engine->RuntimeCanWriteWidgetStorage(
            BoundWidgetId(state)))
    {
        return luaL_error(state,
            "%s cannot write persistent storage during render", api);
    }
    return 0;
}
}

static int lua_StorageGet(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.get"))
        return rejected;
    const char* key = luaL_checkstring(L, 1);
    if (IsReservedStorageTransactionKey(key ? key : ""))
        return luaL_error(L, "storage.get: key is reserved");
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
        if (storedKey.starts_with(prefix + ".__host.")) continue;
        if (storedKey == fullKey) continue;
        ++count;
        bytes += storedKey.size() + storedValue.size();
    }
    return count < kMaxStorageKeys &&
        bytes + fullKey.size() + value.size() <= kMaxStorageBytes;
}

static int lua_StorageSet(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.set"))
        return rejected;
    if (const int rejected = RejectStorageWriteDuringRender(
            L, "storage.set"))
        return rejected;
    const char* key = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    auto* s = GetD2D(L);
    if (!s) return 0;
    if (IsReservedStorageTransactionKey(key ? key : ""))
        return luaL_error(L, "storage.set: key is reserved");
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
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.remove"))
        return rejected;
    if (const int rejected = RejectStorageWriteDuringRender(
            L, "storage.remove"))
        return rejected;
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) return 0;
    if (IsReservedStorageTransactionKey(key ? key : ""))
        return luaL_error(L, "storage.remove: key is reserved");
    ActiveStorage().erase(BoundStoragePrefix(L) + "." + key);
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
    return 0;
}

static int lua_StorageKeys(lua_State* L)
{
    if (const int rejected = RejectStorageAccessInsideTransaction(
            L, "storage.keys"))
        return rejected;
    auto* s = GetD2D(L);
    std::string prefix = s ? BoundStoragePrefix(L) + "." : "";
    lua_newtable(L);
    int idx = 1;
    for (const auto& kv : ActiveStorage())
    {
        if (!prefix.empty() && kv.first.compare(0, prefix.size(), prefix) != 0)
            continue;
        std::string key = prefix.empty() ? kv.first : kv.first.substr(prefix.size());
        if (IsReservedStorageTransactionKey(key)) continue;
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

static int lua_StorageTransaction(lua_State* state)
{
    if (lua_gettop(state) != 1 || !lua_isfunction(state, 1))
        return luaL_error(state,
            "storage.transaction: expected one callback function");
    if (ActiveLuaStorageTransaction(state))
        return luaL_error(state,
            "storage.transaction: nested transactions are not supported");
    if (const int rejected = RejectStorageWriteDuringRender(
            state, "storage.transaction"))
        return rejected;

    auto& activeStorage = ActiveStorage();
    snowdesktop::widget_runtime::WidgetStorageTransaction transaction(
        activeStorage, BoundStoragePrefix(state));
    SetActiveLuaStorageTransaction(state, &transaction);
    lua_pushvalue(state, 1);
    PushStorageTransactionHandle(state, &transaction);
    const int callbackResult = lua_pcall(state, 1, 0, 0);
    SetActiveLuaStorageTransaction(state, nullptr);
    if (callbackResult != LUA_OK)
        return lua_error(state);

    std::string error;
    if (!transaction.ValidateCommit(error))
        return luaL_error(state, "storage.transaction: %s", error.c_str());
    const bool changed = transaction.Changed();
    if (!changed)
    {
        lua_pushboolean(state, 0);
        return 1;
    }

    auto candidate = transaction.TakeCandidate();
    activeStorage.swap(candidate);
    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        activeStorage.swap(candidate);
        return luaL_error(state,
            "storage.transaction: failed to persist storage");
    }
    lua_pushboolean(state, 1);
    return 1;
}

static int LuaL10nOptions(lua_State* L, int index)
{
    if (lua_isnoneornil(L, index)) return 0;
    luaL_checktype(L, index, LUA_TTABLE);
    return lua_absindex(L, index);
}

static std::string LuaL10nLocale(lua_State* L, int options)
{
    std::string locale = Locale::Instance().GetEffectiveLanguage();
    if (!options) return locale;
    lua_getfield(L, options, "locale");
    if (!lua_isnil(L, -1))
    {
        size_t length = 0;
        const char* value = luaL_checklstring(L, -1, &length);
        if (length == 0 || length >= LOCALE_NAME_MAX_LENGTH)
            luaL_error(L, "l10n: 'locale' must be a valid locale name");
        locale.assign(value, length);
    }
    lua_pop(L, 1);
    return locale;
}

static int LuaL10nIntegerOption(lua_State* L, int options,
    const char* name, int defaultValue, int minimum, int maximum)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    int isNumber = 0;
    const lua_Integer raw = lua_tointegerx(L, -1, &isNumber);
    if (!isNumber || raw < minimum || raw > maximum)
    {
        return luaL_error(L,
            "l10n: option '%s' must be an integer from %d to %d",
            name, minimum, maximum);
    }
    lua_pop(L, 1);
    return static_cast<int>(raw);
}

static bool LuaL10nBooleanOption(lua_State* L, int options,
    const char* name, bool defaultValue)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    if (!lua_isboolean(L, -1))
        luaL_error(L, "l10n: option '%s' must be a boolean", name);
    const bool result = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return result;
}

static std::string LuaL10nStringOption(lua_State* L, int options,
    const char* name, const char* defaultValue)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    size_t length = 0;
    const char* value = luaL_checklstring(L, -1, &length);
    if (length == 0 || length > 32)
        luaL_error(L, "l10n: option '%s' has an invalid length", name);
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
}

static int lua_L10nFormatNumber(lua_State* L)
{
    const double value = luaL_checknumber(L, 1);
    const int options = LuaL10nOptions(L, 2);
    snowdesktop::widget_l10n::NumberOptions format;
    format.minimumFractionDigits = LuaL10nIntegerOption(L, options,
        "minimumFractionDigits", 0, 0, 6);
    format.maximumFractionDigits = LuaL10nIntegerOption(L, options,
        "maximumFractionDigits", 2, 0, 6);
    if (format.minimumFractionDigits > format.maximumFractionDigits)
    {
        return luaL_error(L,
            "l10n.formatNumber: minimumFractionDigits exceeds maximumFractionDigits");
    }
    format.grouping = LuaL10nBooleanOption(
        L, options, "grouping", true);
    const std::string result = snowdesktop::widget_l10n::FormatNumber(
        value, LuaL10nLocale(L, options), format);
    if (result.empty())
        return luaL_error(L, "l10n.formatNumber: value must be finite");
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatBytes(lua_State* L)
{
    const double value = luaL_checknumber(L, 1);
    const int options = LuaL10nOptions(L, 2);
    const int base = LuaL10nIntegerOption(L, options,
        "base", 1024, 1000, 1024);
    if (base != 1000 && base != 1024)
        return luaL_error(L, "l10n.formatBytes: base must be 1000 or 1024");
    const int digits = LuaL10nIntegerOption(L, options,
        "maximumFractionDigits", 1, 0, 3);
    const std::string result = snowdesktop::widget_l10n::FormatBytes(
        value, LuaL10nLocale(L, options), base, digits);
    if (result.empty())
    {
        return luaL_error(L,
            "l10n.formatBytes: value must be finite and non-negative");
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatDuration(lua_State* L)
{
    const lua_Integer value = luaL_checkinteger(L, 1);
    if (value < 0)
    {
        return luaL_error(L,
            "l10n.formatDuration: milliseconds must be non-negative");
    }
    const int options = LuaL10nOptions(L, 2);
    std::string style = "short";
    if (options)
    {
        lua_getfield(L, options, "style");
        if (!lua_isnil(L, -1))
        {
            size_t length = 0;
            const char* styleValue = luaL_checklstring(L, -1, &length);
            style.assign(styleValue, length);
        }
        lua_pop(L, 1);
    }
    if (style != "short" && style != "clock")
    {
        return luaL_error(L,
            "l10n.formatDuration: style must be 'short' or 'clock'");
    }
    const std::string result = snowdesktop::widget_l10n::FormatDuration(
        static_cast<std::int64_t>(value), LuaL10nLocale(L, options), style);
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatRelativeTime(lua_State* L)
{
    const auto delta = static_cast<std::int64_t>(luaL_checkinteger(L, 1));
    const int options = LuaL10nOptions(L, 2);
    const std::string unit = LuaL10nStringOption(
        L, options, "unit", "auto");
    const std::string numeric = LuaL10nStringOption(
        L, options, "numeric", "auto");
    const std::string result = snowdesktop::widget_l10n::FormatRelativeTime(
        delta, LuaL10nLocale(L, options), unit, numeric);
    if (result.empty())
    {
        return luaL_error(L,
            "l10n.formatRelativeTime: unit or numeric option is invalid");
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

static int lua_L10nFormatList(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const int valuesTable = lua_absindex(L, 1);
    const size_t count = lua_rawlen(L, valuesTable);
    if (count > 64)
        return luaL_error(L, "l10n.formatList: at most 64 values are allowed");
    size_t totalBytes = 0;
    for (size_t index = 1; index <= count; ++index)
    {
        lua_rawgeti(L, valuesTable, static_cast<lua_Integer>(index));
        if (lua_type(L, -1) != LUA_TSTRING)
        {
            return luaL_error(L,
                "l10n.formatList: value %llu must be a string",
                static_cast<unsigned long long>(index));
        }
        size_t length = 0;
        lua_tolstring(L, -1, &length);
        lua_pop(L, 1);
        if (length > 65536 - totalBytes)
        {
            return luaL_error(L,
                "l10n.formatList: values exceed the 64 KiB limit");
        }
        totalBytes += length;
    }

    const int options = LuaL10nOptions(L, 2);
    const std::string locale = LuaL10nLocale(L, options);
    std::vector<std::string> values;
    values.reserve(count);
    for (size_t index = 1; index <= count; ++index)
    {
        lua_rawgeti(L, valuesTable, static_cast<lua_Integer>(index));
        size_t length = 0;
        const char* value = lua_tolstring(L, -1, &length);
        values.emplace_back(value, length);
        lua_pop(L, 1);
    }
    const std::string result = snowdesktop::widget_l10n::FormatList(
        values, locale);
    lua_pushlstring(L, result.data(), result.size());
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

    if (manifest.apiVersion >= 2)
    {
        lua_pushcfunction(L, lua_L10nFormatNumber);
        lua_setfield(L, -2, "formatNumber");
        lua_pushcfunction(L, lua_L10nFormatBytes);
        lua_setfield(L, -2, "formatBytes");
        lua_pushcfunction(L, lua_L10nFormatDuration);
        lua_setfield(L, -2, "formatDuration");
        lua_pushcfunction(L, lua_L10nFormatRelativeTime);
        lua_setfield(L, -2, "formatRelativeTime");
        lua_pushcfunction(L, lua_L10nFormatList);
        lua_setfield(L, -2, "formatList");
    }
}

void WidgetEngine::RegisterDrawAPI(lua_State* L, int apiVersion)
{
    using snowdesktop::widget_api::DescribeLibrary;
    using snowdesktop::widget_api::FunctionDescriptor;
    using snowdesktop::widget_api::LibraryDescriptor;
    using snowdesktop::widget_api::RegisterLibraries;

    static constexpr FunctionDescriptor draw[] = {
        { "text", lua_DrawText },
        { "measureText", lua_MeasureText },
        { "rect", lua_DrawRect },
        { "arc", lua_DrawArc, 2 },
        { "path", lua_DrawPath, 2 },
        { "gradientRect", lua_DrawGradientRect, 2 },
        { "shadow", lua_DrawShadow, 2 },
        { "sparkline", lua_DrawSparkline, 2 },
        { "pushClip", lua_DrawPushClip },
        { "popClip", lua_DrawPopClip },
        { "strokeRect", lua_DrawStrokeRect },
        { "line", lua_DrawLine },
        { "circle", lua_DrawCircle },
        { "fa", lua_DrawFa },
        { "fluent", lua_DrawFluent },
        { "image", lua_DrawImage },
        { "imageFit", lua_DrawImageFit, 2 },
        { "icon", lua_DrawIcon, 1, "desktop.read" },
    };
    static constexpr FunctionDescriptor interaction[] = {
        { "region", lua_InteractionRegion, 2 },
        { "isHovered", lua_InteractionIsHovered, 2 },
        { "isPressed", lua_InteractionIsPressed, 2 },
        { "scroll", lua_InteractionScroll, 2 },
        { "setScrollOffset", lua_InteractionSetScrollOffset, 2 },
    };
    static constexpr FunctionDescriptor view[] = {
        { "box", snowdesktop::widget_runtime::LuaViewBox, 2 },
        { "row", snowdesktop::widget_runtime::LuaViewRow, 2 },
        { "column", snowdesktop::widget_runtime::LuaViewColumn, 2 },
        { "grid", snowdesktop::widget_runtime::LuaViewGrid, 2 },
        { "flow", snowdesktop::widget_runtime::LuaViewFlow, 2 },
        { "stack", snowdesktop::widget_runtime::LuaViewStack, 2 },
        { "scroll", snowdesktop::widget_runtime::LuaViewScroll, 2 },
        { "list", snowdesktop::widget_runtime::LuaViewList, 2 },
        { "gridList", snowdesktop::widget_runtime::LuaViewGridList, 2 },
        { "virtualList", snowdesktop::widget_runtime::LuaViewVirtualList, 2 },
        { "virtualGrid", snowdesktop::widget_runtime::LuaViewVirtualGrid, 2 },
        { "virtualRange", lua_ViewVirtualRange, 2 },
        { "listItem", snowdesktop::widget_runtime::LuaViewListItem, 2 },
        { "text", snowdesktop::widget_runtime::LuaViewText, 2 },
        { "styledText", snowdesktop::widget_runtime::LuaViewStyledText, 2 },
        { "textInput", snowdesktop::widget_runtime::LuaViewTextInput, 2 },
        { "textArea", snowdesktop::widget_runtime::LuaViewTextArea, 2 },
        { "searchBox", snowdesktop::widget_runtime::LuaViewSearchBox, 2 },
        { "numberInput", snowdesktop::widget_runtime::LuaViewNumberInput, 2 },
        { "select", snowdesktop::widget_runtime::LuaViewSelect, 2 },
        { "image", snowdesktop::widget_runtime::LuaViewImage, 2 },
        { "button", snowdesktop::widget_runtime::LuaViewButton, 2 },
        { "link", snowdesktop::widget_runtime::LuaViewLink, 2 },
        { "toggle", snowdesktop::widget_runtime::LuaViewToggle, 2 },
        { "checkbox", snowdesktop::widget_runtime::LuaViewCheckbox, 2 },
        { "radioGroup", snowdesktop::widget_runtime::LuaViewRadioGroup, 2 },
        { "slider", snowdesktop::widget_runtime::LuaViewSlider, 2 },
        { "icon", snowdesktop::widget_runtime::LuaViewIcon, 2 },
        { "iconButton", snowdesktop::widget_runtime::LuaViewIconButton, 2 },
        { "shape", snowdesktop::widget_runtime::LuaViewShape, 2 },
        { "badge", snowdesktop::widget_runtime::LuaViewBadge, 2 },
        { "divider", snowdesktop::widget_runtime::LuaViewDivider, 2 },
        { "progressBar", snowdesktop::widget_runtime::LuaViewProgressBar, 2 },
        { "progressRing", snowdesktop::widget_runtime::LuaViewProgressRing, 2 },
        { "meter", snowdesktop::widget_runtime::LuaViewMeter, 2 },
        { "sparkline", snowdesktop::widget_runtime::LuaViewSparkline, 2 },
        { "lineChart", snowdesktop::widget_runtime::LuaViewLineChart, 2 },
        { "barChart", snowdesktop::widget_runtime::LuaViewBarChart, 2 },
        { "waveform", snowdesktop::widget_runtime::LuaViewWaveform, 2 },
        { "spectrum", snowdesktop::widget_runtime::LuaViewSpectrum, 2 },
        { "monthCalendar",
            snowdesktop::widget_runtime::LuaViewMonthCalendar, 2 },
        { "slotSurface",
            snowdesktop::widget_runtime::LuaViewSlotSurface, 2 },
        { "slotItem", snowdesktop::widget_runtime::LuaViewSlotItem, 2 },
        { "spacer", snowdesktop::widget_runtime::LuaViewSpacer, 2 },
    };
    static constexpr FunctionDescriptor widget[] = {
        { "info", lua_WidgetInfo },
        { "context", lua_WidgetContext, 2 },
        { "hasPermission", lua_WidgetHasPermission },
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
        { "define", snowdesktop::widget_api::LuaDefineWidget, 2 },
        { "apiInfo", snowdesktop::widget_api::LuaApiInfo, 2 },
        { "hasFeature", snowdesktop::widget_api::LuaHasFeature, 2 },
    };
    static constexpr FunctionDescriptor system[] = {
        { "getTime", lua_GetTime },
        { "notify", lua_Notify, 1, "ui.notify" },
        { "cpu", lua_SystemCpu, 1, kSystemPerformancePermission },
        { "memory", lua_SystemMemory, 1,
            kSystemPerformancePermission },
        { "battery", lua_SystemBattery, 1, kSystemPowerPermission },
        { "network", lua_SystemNetwork, 1,
            kSystemNetworkPermission },
        { "gpu", lua_SystemGpu, 1, kSystemPerformancePermission },
    };
    static constexpr FunctionDescriptor time[] = {
        { "now", lua_TimeNow, 2 },
        { "monotonic", lua_TimeMonotonic, 2 },
        { "parts", lua_TimeParts, 2 },
        { "format", lua_TimeFormat, 2 },
        { "add", lua_TimeAdd, 2 },
        { "compare", lua_TimeCompare, 2 },
    };
    static constexpr FunctionDescriptor systemV2[] = {
        { "info", lua_SystemInfoV2, 2 },
        { "capabilities",
            snowdesktop::widget_api::LuaSystemCapabilities, 2 },
        { "uptime", lua_SystemUptime, 2 },
    };
    static constexpr FunctionDescriptor module[] = {
        { "require", lua_ModuleRequire, 2 },
    };
    static constexpr FunctionDescriptor resource[] = {
        { "exists", lua_ResourceExists, 2 },
        { "image", lua_ResourceImage, 2 },
        { "font", lua_ResourceFont, 2 },
        { "status", lua_ResourceStatus, 2 },
    };
    static constexpr FunctionDescriptor media[] = {
        { "current", lua_MediaCurrent, 1, kMediaReadPermission, 1 },
        { "playPause", lua_MediaPlayPause, 1,
            kMediaActionPermission, 1 },
        { "next", lua_MediaNext, 1, kMediaActionPermission, 1 },
        { "previous", lua_MediaPrevious, 1,
            kMediaActionPermission, 1 },
    };
    static constexpr FunctionDescriptor http[] = {
        { "request", lua_HttpRequest, 1, "network.http" },
        { "cancel", lua_HttpCancel, 1, "network.http" },
    };
    static constexpr FunctionDescriptor ui[] = {
        { "menu", lua_UiMenu, 2 },
        { "textInput", lua_UiTextInput, 1, nullptr, 1 },
        { "textArea", lua_UiTextArea, 1, nullptr, 1 },
        { "focusInput", lua_UiFocusInput, 1, nullptr, 1 },
        { "button", lua_UiButton, 1, nullptr, 1 },
        { "toggle", lua_UiToggle, 1, nullptr, 1 },
        { "progress", lua_UiProgress, 1, nullptr, 1 },
        { "scrollArea", lua_UiScrollArea, 1, nullptr, 1 },
        { "virtualList", lua_UiVirtualList, 1, nullptr, 1 },
        { "setScrollOffset", lua_UiSetScrollOffset, 1, nullptr, 1 },
    };
    static constexpr FunctionDescriptor control[] = {
        { "textInput", lua_ControlTextInput, 2 },
        { "textArea", lua_ControlTextArea, 2 },
        { "focus", lua_ControlFocus, 2 },
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
            "calendar.read", 1 },
        { "setSelectedDate", lua_CalendarSetSelectedDate, 1,
            "calendar.write", 1 },
        { "selectDate", lua_CalendarSelectDate, 2 },
        { "dateInfo", lua_CalendarDateInfo, 1 },
        { "addDays", lua_CalendarAddDays, 1 },
        { "events", lua_CalendarEvents, 1, "calendar.read", 1 },
        { "create", lua_CalendarCreate, 1, "calendar.write", 1 },
        { "update", lua_CalendarUpdate, 1, "calendar.write", 1 },
        { "remove", lua_CalendarRemove, 1, "calendar.write", 1 },
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
        { "transaction", lua_StorageTransaction, 2 },
    };
    static constexpr FunctionDescriptor slots[] = {
        { "binding", lua_SlotsBinding, 2 },
        { "collection", lua_SlotsCollection, 2 },
        { "canUndo", lua_SlotsCanUndo, 2 },
        { "canRedo", lua_SlotsCanRedo, 2 },
        { "undo", lua_SlotsUndo, 2 },
        { "redo", lua_SlotsRedo, 2 },
    };
    static constexpr FunctionDescriptor state[] = {
        { "get", snowdesktop::widget_api::LuaTransientStateGet, 2 },
        { "set", snowdesktop::widget_api::LuaTransientStateSet, 2 },
        { "remove", snowdesktop::widget_api::LuaTransientStateRemove, 2 },
        { "has", snowdesktop::widget_api::LuaTransientStateHas, 2 },
        { "keys", snowdesktop::widget_api::LuaTransientStateKeys, 2 },
        { "clear", snowdesktop::widget_api::LuaTransientStateClear, 2 },
    };
    static constexpr FunctionDescriptor schedule[] = {
        { "every", lua_ScheduleEvery, 2 },
        { "after", lua_ScheduleAfter, 2 },
        { "at", lua_ScheduleAt, 2 },
        { "cancel", lua_ScheduleCancel, 2 },
    };
    static constexpr FunctionDescriptor data[] = {
        { "subscribe", lua_DataSubscribe, 2 },
    };
    static constexpr FunctionDescriptor task[] = {
        { "start", lua_TaskStart, 2 },
        { "cancel", lua_TaskCancel, 2 },
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
        DescribeLibrary("interaction", interaction),
        DescribeLibrary("view", view),
        DescribeLibrary("widget", widget),
        DescribeLibrary("sys", system),
        DescribeLibrary("system", systemV2),
        DescribeLibrary("time", time),
        DescribeLibrary("module", module),
        DescribeLibrary("resource", resource),
        DescribeLibrary("media", media),
        DescribeLibrary("http", http),
        DescribeLibrary("ui", ui),
        DescribeLibrary("control", control),
        DescribeLibrary("desktop", desktop),
        DescribeLibrary("everything", everything),
        DescribeLibrary("calendar", calendar),
        DescribeLibrary("layout", layout),
        DescribeLibrary("storage", storage),
        DescribeLibrary("slots", slots),
        DescribeLibrary("state", state),
        DescribeLibrary("schedule", schedule),
        DescribeLibrary("data", data),
        DescribeLibrary("task", task),
        DescribeLibrary("imgui", imgui),
    };

    if (apiVersion >= 2)
        RegisterDataSubscriptionHandle(L);
    RegisterLibraries(L,
        std::span<const LibraryDescriptor>(libraries),
        static_cast<std::uint32_t>(std::max(1, apiVersion)));
}
