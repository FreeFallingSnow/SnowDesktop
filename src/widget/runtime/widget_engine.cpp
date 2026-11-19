#include "ui/input/private_pointer.h"
#include "theme/surface_theme.h"
#include "ui/render/app_font.h"
#include "widget/data/widget_system_control_data.h"
#include "widget/data/widget_gpu_lua.h"
#include "common/background_work.h"
/**
 * @file widget_engine.cpp
 * @brief WidgetEngine 类的实现，管理 Lua 小部件的完整生命周期
 *
 * 该文件实现了 WidgetEngine 类及其相关辅助功能，负责：
 * - 将 Lua 脚本加载到 Lua 5.x 沙盒环境中安全执行
 * - 注册绘制 API（draw.*）、系统 API（sys.*）、桌面 API（desktop.*）等 Lua 可调用的 C 函数
 * - 通过 D2D 渲染小部件内容并处理交互事件（点击、菜单、桌面变更等）
 * - 管理小部件的持久化键值存储（localStorage）、文件变更热重载与错误记录
 * - 读取并解析 .widget.json 清单文件以获取权限、尺寸等元信息
 */

#include "widget_engine.h"
#include "widget_lua_binding_context.h"
#include "widget/runtime/widget_logical_slot_lua.h"
#include "widget/runtime/widget_calendar_lua.h"
#include "widget/runtime/widget_schedule_lua.h"
#include "widget/runtime/widget_time_lua.h"
#include "widget/data/widget_system_lua.h"
#include "widget/data/widget_data_subscription_lua.h"
#include "widget_multiplayer.h"
#include "widget_script_read_ahead.h"
#include "widget/tasks/widget_clipboard_task_dispatch.h"
#include "widget/tasks/widget_filesystem_task_dispatch.h"
#include "widget/tasks/widget_filesystem_task_executor.h"
#include "widget/tasks/widget_task_start_lua.h"
#include "widget/tasks/widget_audio_task_dispatch.h"
#include "ui/render/smooth_corner_geometry.h"
#include "ui/input/text_input_accessibility.h"
namespace text_input = snowdesktop::text_input;
#include "widget/view/widget_appearance_presets.h"
#include "theme/edge_light_codec.h"
#include "ui/render/native_control_geometry.h"
#include "ui/render/native_tooltip_content.h"
#include "widget_menu_catalogue.h"
#include "widget/tasks/widget_filesystem_drop.h"
#include "widget/view/widget_button_fill.h"
#include "widget/view/widget_date_picker_lua.h"
#include "widget/view/widget_time_picker_lua.h"
#include "widget/view/widget_duration_picker_lua.h"
#include "settings/animation_settings.h"
#include "diagnostics/diagnostic_log.h"
#include "diagnostics/performance_trace.h"
#include "layout/logical_slot_keyboard_rules.h"
#include "layout/logical_slot_picker_rules.h"
#include "layout/logical_slot_pointer_rules.h"
#include "widget/view/widget_scroll_rules.h"
#include "data/data_paths.h"
#include "platform/deployment_context.h"
#include "common/json_value.h"
#include "common/l10n.h"
#include "system/system_snapshot.h"
#include "common/constants.h"
#include "common/utils.h"
#include "navigation/name_pinyin.h"
#include "navigation/search_match.h"
#include "widget/packages/widget_package.h"
#include "steam/steam_workshop_source.h"
#include "steam/steam_workshop_diagnostics.h"
#include "steam/steam_entitlement.h"
#include "widget/api/widget_api_registry.h"
#include "widget_l10n_lua.h"
#include "widget_time.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget_file_drop_delivery.h"
#include "widget/preview/widget_preview_context.h"
#include "widget/view/widget_interaction_region.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget/view/widget_draw_geometry.h"
#include "widget/view/widget_draw_context.h"
#include "widget/view/widget_draw_lua.h"
#include "widget/view/widget_layout_lua.h"
#include "widget/view/widget_view_renderer.h"
#include "widget/view/widget_view_text.h"
#include "widget/view/widget_background_cache.h"
#include "widget/view/widget_view_lua.h"
#include "widget/view/widget_view_tree.h"
#include "widget/view/widget_surface_renderer.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/packages/widget_package_image_cache.h"
#include "widget/view/widget_text_input_rules.h"
#include "widget/storage/widget_storage_transaction.h"
#include "widget/storage/widget_storage_runtime.h"
#include "widget/storage/widget_storage_lua.h"
#include "widget/tasks/widget_filesystem_watch_origin.h"
#include "widget_host_control_projection.h"
#include "widget_timer_dispatch.h"
#include "widget_calendar_provider.h"
#include "widget/api/widget_calendar_query_options.h"
#include "widget/storage/widget_storage_value.h"
#include "widget/storage/widget_storage_write_budget.h"
#include "widget/storage/widget_secret_store.h"
#include "widget/settings/widget_setting_rules.h"
#include "widget/settings/widget_settings_declarations.h"
#include "widget/settings/widget_system_settings.h"
#include "widget/view/widget_ui_metrics.h"
#include "data/atomic_file.h"
#include "widgets/widget_chrome_rules.h"

using snowdesktop::lua_runtime::DeferLuaError;
using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_runtime::BoundWidgetRuntimeToken;
using snowdesktop::widget_runtime::RequirePermission;
using snowdesktop::widget_runtime::PushSystemControlJson;
using snowdesktop::lua_runtime::LuaTableIsContiguousArray;
using snowdesktop::widget_l10n::PushWidgetL10nAPI;
using snowdesktop::widget_runtime::IsRemovedPanelEffectSettingKey;
using snowdesktop::widget_runtime::LuaReadStorageField;
using snowdesktop::widget_runtime::ReadLuaDeclaredSettings;
using snowdesktop::widget_runtime::IsEntityReferenceSettingType;
using snowdesktop::widget_runtime::IsFilesystemHandleSettingType;
using snowdesktop::widget_runtime::FilesystemSettingAccess;
using snowdesktop::widget_runtime::FilesystemSettingMetadataKey;
using snowdesktop::widget_runtime::EntityReferenceSettingTypeFilter;
using snowdesktop::widget_runtime::ValidateWidgetSettings;
using snowdesktop::widget_runtime::CurrentLayoutContentHeight;
using snowdesktop::widget_runtime::CurrentLayoutContentWidth;
using snowdesktop::widget_runtime::ActiveStorage;
using snowdesktop::widget_runtime::LoadStorageFile;
using snowdesktop::widget_runtime::MutablePersistentStorage;
using snowdesktop::widget_runtime::PersistentStorage;
using snowdesktop::widget_runtime::ReadStorage;
using snowdesktop::widget_runtime::SaveStorageFile;
using snowdesktop::widget_runtime::StorageWriteWithinQuota;
using snowdesktop::widget_runtime::PushInteractionValue;
using snowdesktop::widget_runtime::ReadInteractionValue;
using snowdesktop::lua_runtime::LuaTableUsesOnlyFields;
using snowdesktop::widget_runtime::ApplyViewTextLocaleAndDirection;
using snowdesktop::widget_runtime::BuildHostInputDisplayText;
using snowdesktop::widget_runtime::HostInputDisplayText;
using snowdesktop::widget_runtime::IsDeclarativeInputNode;
using snowdesktop::widget_runtime::SetViewTextLayoutAlignment;

#include <dwrite_3.h>
#include <d2d1effects.h>
#include <dwmapi.h>

#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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

static std::string ViewVirtualKeyName(WPARAM key)
{
    if (key >= '0' && key <= '9')
        return std::string(1, static_cast<char>(key));
    if (key >= 'A' && key <= 'Z')
        return std::string(1, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24)
        return "F" + std::to_string(key - VK_F1 + 1);
    switch (key)
    {
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Control";
    case VK_MENU: return "Alt";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "CapsLock";
    case VK_ESCAPE: return "Escape";
    case VK_SPACE: return "Space";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "ArrowLeft";
    case VK_UP: return "ArrowUp";
    case VK_RIGHT: return "ArrowRight";
    case VK_DOWN: return "ArrowDown";
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_LWIN: return "MetaLeft";
    case VK_RWIN: return "MetaRight";
    case VK_APPS: return "ContextMenu";
    case VK_NUMLOCK: return "NumLock";
    case VK_SCROLL: return "ScrollLock";
    default: return "Unidentified";
    }
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

static void TrimWidgetPackageHistory(snowdesktop::widget::WidgetPackageManager& manager,
    std::string_view packageId = {})
{
    std::string error;
    if (!manager.TrimInstalledVersions(error, packageId))
        WriteDiagnosticLogEntry(Utf8ToWideLocal(error).c_str(), DiagnosticLogLevel::Warning);
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
    struct RejectedArchive
    {
        std::string digest;
        std::string error;
    };
    std::mutex mutex;
    std::unordered_map<std::string, std::string> associations;
    std::vector<snowdesktop::widget::SteamWorkshopInstallFailure>
        installFailures;
    std::unordered_map<std::string, RejectedArchive> rejectedArchives;
};

static SteamWorkshopPackageAssociationCache&
GetSteamWorkshopPackageAssociationCache()
{
    static SteamWorkshopPackageAssociationCache cache;
    return cache;
}

static std::optional<std::string> RejectedSteamWorkshopArchive(
    const snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const std::string& item)
{
    const auto digest = snapshot.archiveDigests.find(item);
    if (digest == snapshot.archiveDigests.end() || digest->second.empty())
        return std::nullopt;
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    const auto rejected = cache.rejectedArchives.find(item);
    if (rejected == cache.rejectedArchives.end() || rejected->second.digest != digest->second)
        return std::nullopt;
    return rejected->second.error;
}

static void RememberRejectedSteamWorkshopArchive(const std::string& item,
    std::string digest, std::string error)
{
    if (digest.empty()) return;
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    if (cache.rejectedArchives.size() >= 256 && !cache.rejectedArchives.contains(item))
        cache.rejectedArchives.erase(cache.rejectedArchives.begin());
    cache.rejectedArchives[item] = {std::move(digest), std::move(error)};
}

static void ForgetRejectedSteamWorkshopArchive(const std::string& item)
{
    auto& cache = GetSteamWorkshopPackageAssociationCache();
    std::lock_guard lock(cache.mutex);
    cache.rejectedArchives.erase(item);
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
    auto& manager = GetWidgetPackageManager();
    const auto id = WidgetWideToUtf8(packageId);
    const auto entry = snowdesktop::widget_runtime::IsDryLoad()
        ? manager.ResolveSelectedEntry(id) : manager.ResolveEntry(id);
    return entry ? entry->wstring() : std::wstring{};
}

static bool RecoverWidgetPackage(const std::string& packageId,
    const std::optional<std::string>& preferredVersion = std::nullopt,
    std::string_view rejectedVersion = {})
{
    auto& manager = GetWidgetPackageManager();
    const auto packages = manager.ListPackages();
    auto tryVersion = [&](const std::string& version) {
        std::string error;
        return manager.Rollback(packageId, version, error);
    };
    if (preferredVersion && *preferredVersion != rejectedVersion && tryVersion(*preferredVersion))
        return true;
    std::vector<std::string> candidates;
    bool activeUserPackage = false;
    for (const auto& package : packages)
    {
        if (package.manifest.id != packageId ||
            package.builtin || package.development)
            continue;
        if (package.active) activeUserPackage = true;
        else if (package.manifest.version != rejectedVersion)
            candidates.push_back(package.manifest.version);
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
            return manager.ResolveSelected(packageId).has_value();
    }
    return false;
}

static std::optional<snowdesktop::widget::InstalledPackage>
ResolveWidgetEntryPackage(const std::filesystem::path& path)
{
    auto& manager = GetWidgetPackageManager();
    return snowdesktop::widget_runtime::IsDryLoad()
        ? manager.ResolveSelectedEntryPath(path) : manager.ResolveEntryPath(path);
}

// ── Storage (shared localStorage for Lua widgets) ────────────────
static snowdesktop::widget_runtime::DiagnosticsLog g_widgetDiagnostics;

static std::filesystem::path WidgetSecretStorePath()
{
    const std::filesystem::path data = GetDataDirectoryPath();
    return data.parent_path() / L"PrivateState" /
        L"SnowDesktop.widget-secrets.bin";
}

using snowdesktop::widget_runtime::DryLoadScope;
using snowdesktop::widget_runtime::PreviewExecutionScope;
using snowdesktop::widget_runtime::StorageOverlayScope;

std::unordered_map<std::string, std::string>&
WidgetEngine::WidgetSettingsPersistentStorageForBackend() noexcept
{
    return snowdesktop::widget_runtime::MutablePersistentStorage();
}

const std::unordered_map<std::string, std::string>&
WidgetEngine::WidgetSettingsPersistentStorageForBackend() const noexcept
{
    return snowdesktop::widget_runtime::PersistentStorage();
}

bool WidgetEngine::PersistWidgetSettingsStorageForBackend()
{
    return SaveStorageFile();
}

const snowdesktop::widget_runtime::StorageMap&
WidgetEngine::WidgetSettingsReadStorageForBackend() const
{
    return ReadStorage();
}

bool WidgetEngine::StageWidgetSettingsPreviewForBackend(const void* owner,
    const std::string& prefix,
    const snowdesktop::widget_runtime::StorageMap& previous,
    const snowdesktop::widget_runtime::StorageMap& candidate)
{
    return snowdesktop::widget_runtime::StageStoragePreview(owner, prefix, previous, candidate);
}

bool WidgetEngine::CommitWidgetSettingsPreviewForBackend(const void* owner)
{
    return snowdesktop::widget_runtime::CommitStoragePreview(owner);
}

void WidgetEngine::RemoveWidgetSettingsPreviewForBackend(const void* owner) noexcept
{
    snowdesktop::widget_runtime::RemoveStoragePreview(owner);
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
    struct Result { std::wstring path; HBITMAP bitmap = nullptr; };
    using ReadyCallback = std::function<void(const std::wstring&)>;
    explicit AsyncShellIconLoader(ReadyCallback readyCallback) : ready_(std::move(readyCallback)) {}
    ~AsyncShellIconLoader()
    {
        work_.Stop();
        for (auto& result : completed_) if (result.bitmap) DeleteObject(result.bitmap);
    }
    void Request(const std::wstring& path, const std::wstring& widgetId)
    {
        if (path.empty()) return;
        work_.Submit(path, [path] {
            auto result = std::make_shared<snowdesktop::BackgroundBitmap>();
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
            {
                result->bitmap = GetHighResolutionShellIconBitmap(pidl, -1, result->size);
                CoTaskMemFree(pidl);
            }
            return result;
        }, [this, path, widgetId](auto result) {
            completed_.push_back({path, result ? std::exchange(result->bitmap, nullptr) : nullptr});
            if (ready_) ready_(widgetId); // UI only; never inspect widgets on the worker.
        }, nullptr, 0);
    }
    void Pump() { work_.Drain(); }
    std::vector<Result> Drain()
    {
        Pump();
        return std::exchange(completed_, {});
    }
private:
    snowdesktop::BackgroundWork work_{2, 128};
    ReadyCallback ready_;
    std::vector<Result> completed_;
};

struct RuntimeImageResource
{
    std::shared_ptr<const snowdesktop::widget_runtime::
        WidgetRuntimeImagePixels> pixels;
    std::wstring ownerWidgetId;
    std::string source;
};

struct D2DState : snowdesktop::widget_runtime::WidgetLayoutContext
{
    WidgetEngine* engine = nullptr;
    std::string storagePrefix;
    std::string_view desktopPopupOpenerKey;
    bool openingFocusAllowed = false;
    float semanticCuScale = 1.0f;
    snowdesktop::widget_runtime::SemanticUiMetricTokens semanticUiMetrics;
    std::uint64_t nextDesktopPaintRevision = 0;
    ComPtr<ID2D1Device> bitmapDevice;
    ComPtr<ID2D1DeviceContext> immediateCommandContext;
    snowdesktop::widget_runtime::WidgetBackgroundCache backgroundCache;
    snowdesktop::widget_runtime::WidgetScriptReadAhead scriptReadAhead;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap1>> imageCache;
    std::unordered_map<std::string, std::uint64_t> imageCacheRevisions;
    snowdesktop::widget_runtime::WidgetPackageImageCache packageImageCache;
    std::unordered_map<std::string, RuntimeImageResource> runtimeImages;
    std::unordered_map<std::string, ComPtr<ID2D1Bitmap1>>
        runtimeImageBitmaps;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> shellIconCache;
    std::unordered_set<std::wstring> shellIconFailures;
    std::unique_ptr<AsyncShellIconLoader> shellIconLoader;
};

static void EraseRuntimeImageBitmap(D2DState* state, const std::string& key)
{
    if (const auto found = state->runtimeImageBitmaps.find(key);
        found != state->runtimeImageBitmaps.end())
    {
        state->imageSamplingCache.Remove(found->second.Get());
        state->runtimeImageBitmaps.erase(found);
    }
}

static void ClearShellIconBitmaps(D2DState* state)
{
    for (const auto& [path, bitmap] : state->shellIconCache)
        state->imageSamplingCache.Remove(bitmap.Get());
    state->shellIconCache.clear();
}

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
    std::string source, bool preserveExisting = false)
{
    const std::string boundToken = BindRuntimeImageToken(token, widgetId);
    if (!state || boundToken.empty() || !pixels ||
        !snowdesktop::widget_runtime::IsValidWidgetRuntimeImage(*pixels) ||
        source.empty())
        return {};
    constexpr std::size_t maximumBytes = 64 * 1024 * 1024;
    std::size_t bytes = pixels->bgraPremultiplied.size();
    for (const auto& [key, resource] : state->runtimeImages)
        if (key != boundToken && resource.pixels) bytes += resource.pixels->bgraPremultiplied.size();
    if (preserveExisting && (bytes > maximumBytes ||
        (!state->runtimeImages.contains(boundToken) &&
            (state->runtimeImages.size() >= 128 ||
             std::count_if(state->runtimeImages.begin(), state->runtimeImages.end(),
                 [&widgetId](const auto& entry) {
                     return entry.second.ownerWidgetId == widgetId;
                 }) >= 16))))
        return {};
    while (bytes > maximumBytes && !state->runtimeImages.empty())
    {
        auto victim = std::find_if(state->runtimeImages.begin(), state->runtimeImages.end(),
            [&boundToken](const auto& item) { return item.first != boundToken; });
        if (victim == state->runtimeImages.end()) break;
        if (victim->second.pixels) bytes -= victim->second.pixels->bgraPremultiplied.size();
        EraseRuntimeImageBitmap(state, victim->first);
        state->runtimeImages.erase(victim);
    }
    const std::size_t ownerCount = static_cast<std::size_t>(std::count_if(
        state->runtimeImages.begin(), state->runtimeImages.end(),
        [&widgetId](const auto& item) {
            return item.second.ownerWidgetId == widgetId;
        }));
    if (!preserveExisting &&
        ((!state->runtimeImages.contains(boundToken) && ownerCount >= 16) ||
         state->runtimeImages.size() >= 128))
    {
        std::vector<std::string> removed;
        for (const auto& [key, resource] : state->runtimeImages)
        {
            if (resource.ownerWidgetId == widgetId) removed.push_back(key);
        }
        for (const auto& key : removed)
        {
            state->runtimeImages.erase(key);
            EraseRuntimeImageBitmap(state, key);
        }
    }
    if (!state->runtimeImages.contains(boundToken) &&
        state->runtimeImages.size() >= 128)
    {
        const std::string evicted = state->runtimeImages.begin()->first;
        state->runtimeImages.erase(evicted);
        EraseRuntimeImageBitmap(state, evicted);
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
    state->backgroundCache.Erase(widgetId);
    state->textLayoutCache.Erase(widgetId);
    std::vector<std::string> removed;
    for (const auto& [key, resource] : state->runtimeImages)
    {
        if (resource.ownerWidgetId == widgetId) removed.push_back(key);
    }
    for (const auto& key : removed)
    {
        state->runtimeImages.erase(key);
        EraseRuntimeImageBitmap(state, key);
    }
}

static void ClearRuntimeImagesForSource(
    D2DState* state, std::string_view source)
{
    if (!state || source.empty()) return;
    state->backgroundCache.Clear();
    std::vector<std::string> removed;
    for (const auto& [key, resource] : state->runtimeImages)
    {
        if (resource.source == source) removed.push_back(key);
    }
    for (const auto& key : removed)
    {
        state->runtimeImages.erase(key);
        EraseRuntimeImageBitmap(state, key);
    }
}

static D2DState* GetD2D(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__d2d_ptr");
    auto* s = static_cast<D2DState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return s;
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

static std::string BoundStoragePrefix(lua_State* state)
{
    return WidgetWideToUtf8(BoundWidgetId(state));
}

static const snowdesktop::widget_runtime::WidgetStorageLuaHost kStorageLuaHostServices{
    BoundStoragePrefix,
    +[](lua_State* state) {
        auto* d2d = GetD2D(state);
        return !d2d || !d2d->engine ||
            d2d->engine->RuntimeCanWriteWidgetStorage(BoundWidgetId(state));
    },
    +[](lua_State* state, const std::string& key, std::string& value) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeGetSecretReference(BoundWidgetId(state), key, value);
    },
    +[](lua_State* state, const std::string& key, std::string& value) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeGetFilesystemSettingHandle(BoundWidgetId(state), key, value);
    },
    +[](lua_State* state, const std::string& key) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeIsEntityReferenceSetting(BoundWidgetId(state), key);
    },
    +[](lua_State* state, const std::string& key, snowdesktop::widget_runtime::InteractionValue& value) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeGetTypedSettingDefault(BoundWidgetId(state), key, value);
    },
    +[](lua_State* state, const std::string& key) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine
            ? d2d->engine->RuntimeGetStorageValue(BoundWidgetId(state), key) : std::string{};
    },
    +[](lua_State* state) {
        auto* d2d = GetD2D(state);
        if (!d2d || !d2d->engine) return std::vector<std::string>{};
        const auto widgetId = BoundWidgetId(state);
        auto keys = d2d->engine->RuntimeSecretStorageKeys(widgetId);
        const auto filesystemKeys = d2d->engine->RuntimeFilesystemSettingKeys(widgetId);
        keys.insert(keys.end(), filesystemKeys.begin(), filesystemKeys.end());
        return keys;
    },
};

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
        : performanceScope_("widget", "context", widgetId), state_(state)
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
        layoutContentWidth_ = state_->layoutContentWidth;
        layoutContentHeight_ = state_->layoutContentHeight;
        layoutMetrics_ = snowdesktop::widget_runtime::
            CaptureLayoutMetrics(*state_);
        widgetClipDepth_ = state_->widgetClipDepth;
        backgroundLayerActive_ = state_->backgroundLayerActive;
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
        state_->layoutContentWidth = layoutContentWidth_;
        state_->layoutContentHeight = layoutContentHeight_;
        state_->widgetClipDepth = widgetClipDepth_;
        state_->backgroundLayerActive = backgroundLayerActive_;
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
    snowdesktop::performance::Scope performanceScope_;
    D2DState* state_ = nullptr;
    ID2D1DeviceContext* context_ = nullptr;
    D2D1_RECT_F widgetRect_{};
    std::string storagePrefix_;
    std::wstring widgetId_;
    const char* surfaceKind_ = "desktop";
    int gridColumns_ = 1;
    int gridRows_ = 1;
    float layoutContentWidth_ = 0.0f;
    float layoutContentHeight_ = 0.0f;
    snowdesktop::widget_runtime::LayoutMetrics layoutMetrics_;
    int widgetClipDepth_ = 0;
    bool backgroundLayerActive_ = false;
};

class WidgetSurfaceScope
{
public:
    WidgetSurfaceScope(D2DState* state, const char* surfaceKind,
        bool allowOpeningFocus = false, std::string_view desktopPopupOpenerKey = {})
        : state_(state)
    {
        if (!state_) return;
        previous_ = state_->surfaceKind;
        previousPopupOpenerKey_ = state_->desktopPopupOpenerKey;
        previousOpeningFocus_ = state_->openingFocusAllowed;
        state_->surfaceKind = surfaceKind ? surfaceKind : "desktop";
        state_->desktopPopupOpenerKey = desktopPopupOpenerKey;
        state_->openingFocusAllowed = allowOpeningFocus;
    }

    ~WidgetSurfaceScope()
    {
        if (!state_) return;
        state_->surfaceKind = previous_;
        state_->desktopPopupOpenerKey = previousPopupOpenerKey_;
        state_->openingFocusAllowed = previousOpeningFocus_;
    }

    WidgetSurfaceScope(const WidgetSurfaceScope&) = delete;
    WidgetSurfaceScope& operator=(const WidgetSurfaceScope&) = delete;

private:
    D2DState* state_ = nullptr;
    const char* previous_ = "desktop";
    std::string_view previousPopupOpenerKey_;
    bool previousOpeningFocus_ = false;
};

static std::string ImageUseOwner(D2DState* state, bool background = false)
{
    return WidgetWideToUtf8(state->currentWidgetId) + "|" +
        state->surfaceKind +
        ((background || state->backgroundLayerActive) ? "#background" : "");
}

static void BeginImageFrame(D2DState* state, bool background = false)
{
    if (state) state->packageImageCache.BeginFrame(ImageUseOwner(state, background));
}

static bool IsPanelSurface(std::string_view surface) noexcept
{
    return surface == "panel" || surface == "dialog" ||
        surface == "popover";
}

static std::string NormalizeWidgetSurface(std::string_view surface)
{
    if (surface == "dialog") return "dialog";
    if (surface == "popover") return "popover";
    if (surface == "panel") return "panel";
    return "desktop";
}

static snowdesktop::widget_runtime::WidgetInteractionRegions&
InteractionRegionsForSurface(LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelInteractionRegions : widget.interactionRegions;
}

static const snowdesktop::widget_runtime::WidgetInteractionRegions&
InteractionRegionsForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelInteractionRegions : widget.interactionRegions;
}

static bool HasDeclarativeViewForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept
{
    return IsPanelSurface(surface)
        ? widget.panelViewTree.has_value() : widget.viewTree.has_value();
}

static const snowdesktop::widget_runtime::ViewNode*
DeclarativeViewForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept
{
    const auto& tree = IsPanelSurface(surface)
        ? widget.panelViewTree : widget.viewTree;
    return tree ? &*tree : nullptr;
}

static const snowdesktop::widget_runtime::ViewNode* FindViewNodeByKey(
    const snowdesktop::widget_runtime::ViewNode& node,
    std::string_view key) noexcept
{
    if (node.key == key) return &node;
    for (const auto& child : node.children)
    {
        if (const auto* found = FindViewNodeByKey(child, key))
            return found;
    }
    return nullptr;
}

static void QueueDeclarativeVisualFrame(
    LuaWidget& widget, std::string_view surface) noexcept
{
    if (IsPanelSurface(surface))
        widget.panelViewTransitionFramePending = true;
    else
        widget.viewTransitionFramePending = true;
}

static std::unordered_map<std::string, int>& ScrollOffsetsForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelScrollOffsets : widget.scrollOffsets;
}

static const std::unordered_map<std::string, int>& ScrollOffsetsForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelScrollOffsets : widget.scrollOffsets;
}

static std::unordered_map<std::string, LuaWidget::VariableVirtualState>&
VariableVirtualStatesForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelVariableVirtualStates : widget.variableVirtualStates;
}

static const std::unordered_map<std::string,
    LuaWidget::VariableVirtualState>& VariableVirtualStatesForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelVariableVirtualStates : widget.variableVirtualStates;
}

static bool VariableVirtualStateMatches(
    const LuaWidget::VariableVirtualState& state,
    std::size_t itemCount, float estimatedItemSize, float mainGap,
    bool horizontal, std::uint64_t layoutRevision) noexcept
{
    return state.itemCount == itemCount &&
        state.estimatedItemSize == estimatedItemSize &&
        state.mainGap == mainGap &&
        state.horizontal == horizontal &&
        state.layoutRevision == layoutRevision;
}

static std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
VariableVirtualMeasurements(
    const LuaWidget::VariableVirtualState& state)
{
    std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
        result;
    result.reserve(state.measurements.size());
    for (const auto& [index, measurement] : state.measurements)
        result.push_back({ index, measurement.extent });
    std::ranges::sort(result, {},
        &snowdesktop::widget_runtime::ViewVirtualItemMeasurement::index);
    return result;
}

static void AttachVariableVirtualMeasurements(
    snowdesktop::widget_runtime::ViewNode& node,
    const LuaWidget& widget, std::string_view surface)
{
    node.virtualMeasurements.clear();
    if (node.estimatedItemSize)
    {
        const auto& states = VariableVirtualStatesForSurface(widget, surface);
        const auto found = states.find(node.key);
        const bool horizontal = node.orientation ==
            snowdesktop::widget_runtime::ViewOrientation::Horizontal;
        const float mainGap = horizontal
            ? node.columnGap.value_or(node.gap)
            : node.rowGap.value_or(node.gap);
        if (found != states.end() && VariableVirtualStateMatches(
                found->second, node.itemCount, *node.estimatedItemSize,
                mainGap, horizontal,
                node.virtualLayoutRevision))
            node.virtualMeasurements =
                VariableVirtualMeasurements(found->second);
    }
    for (auto& child : node.children)
        AttachVariableVirtualMeasurements(child, widget, surface);
}

static bool ValidateAndLayoutWidgetView(
    snowdesktop::widget_runtime::ViewNode& root,
    LuaWidget& widget, std::string_view surface,
    float width, float height, std::string& error)
{
    snowdesktop::performance::Scope scope("widget.view", "layout", widget.widgetId);
    AttachVariableVirtualMeasurements(root, widget, surface);
    auto& cache = IsPanelSurface(surface)
        ? widget.panelViewLayoutCache : widget.viewLayoutCache;
    bool reused = false;
    const bool accepted = cache.ValidateAndLayout(
        root, width, height, error, &reused);
    snowdesktop::performance::Value("widget.view.layout_cache",
        reused ? "hit" : "miss", widget.widgetId, 1);
    return accepted;
}

static std::string FormatViewValidationDiagnostic(
    std::string_view code, std::string_view message)
{
    if (!snowdesktop::widget_runtime::
            IsKnownViewValidationDiagnosticCode(code))
        code = "view.layout";
    return "[" + std::string(code) + "] " + std::string(message);
}

static bool CommitVariableVirtualMeasurements(
    snowdesktop::widget_runtime::ViewNode& node,
    LuaWidget& widget, std::string_view surface)
{
    using snowdesktop::widget_runtime::ComputeViewVariableVirtualItemStart;
    using snowdesktop::widget_runtime::ComputeViewVariableVirtualRange;
    using snowdesktop::widget_runtime::ViewNodeType;
    using snowdesktop::widget_runtime::ViewVirtualRange;
    constexpr std::size_t maximumCachedMeasurements = 4096;
    bool changed = false;
    auto& states = VariableVirtualStatesForSurface(widget, surface);
    if (node.type == ViewNodeType::VirtualList && !node.estimatedItemSize)
        states.erase(node.key);
    if (node.estimatedItemSize &&
        node.collectionContent == snowdesktop::widget_runtime::
            ViewCollectionContent::Items &&
        node.virtualMeasuredExtents.size() == node.children.size() &&
        node.virtualMeasuredIndices.size() == node.children.size())
    {
        const bool horizontal = node.orientation ==
            snowdesktop::widget_runtime::ViewOrientation::Horizontal;
        const float mainGap = horizontal
            ? node.columnGap.value_or(node.gap)
            : node.rowGap.value_or(node.gap);
        const auto oldMeasurements = node.virtualMeasurements;
        ViewVirtualRange oldRange;
        std::string rangeError;
        std::size_t anchorIndex = node.firstIndex;
        float oldAnchorStart = 0.0f;
        if (node.itemCount > 0 &&
            ComputeViewVariableVirtualRange(node.itemCount,
                *node.estimatedItemSize, mainGap,
                node.scrollViewportExtent, node.scrollOffset, 0,
                oldMeasurements, oldRange, rangeError))
        {
            anchorIndex = oldRange.firstIndex;
            (void)ComputeViewVariableVirtualItemStart(node.itemCount,
                *node.estimatedItemSize, mainGap, anchorIndex,
                oldMeasurements, oldAnchorStart, rangeError);
        }

        LuaWidget::VariableVirtualState nextState;
        const auto existing = states.find(node.key);
        if (existing != states.end() && VariableVirtualStateMatches(
                existing->second, node.itemCount,
                *node.estimatedItemSize, mainGap, horizontal,
                node.virtualLayoutRevision))
            nextState = existing->second;
        else
        {
            nextState.itemCount = node.itemCount;
            nextState.estimatedItemSize = *node.estimatedItemSize;
            nextState.mainGap = mainGap;
            nextState.horizontal = horizontal;
            nextState.layoutRevision = node.virtualLayoutRevision;
            changed = true;
        }
        for (std::size_t child = 0;
            child < node.virtualMeasuredExtents.size(); ++child)
        {
            const std::size_t index = node.virtualMeasuredIndices[child];
            const float extent = node.virtualMeasuredExtents[child];
            const std::string& itemKey = node.children[child].key;
            auto& measurement = nextState.measurements[index];
            if (std::fabs(measurement.extent - extent) > 0.01f ||
                measurement.itemKey != itemKey)
                changed = true;
            measurement.extent = extent;
            measurement.itemKey = itemKey;
            measurement.lastUsed = ++nextState.sequence;
        }
        while (nextState.measurements.size() > maximumCachedMeasurements)
        {
            auto oldest = nextState.measurements.end();
            for (auto current = nextState.measurements.begin();
                current != nextState.measurements.end(); ++current)
            {
                if (std::ranges::find(node.virtualMeasuredIndices,
                        current->first) != node.virtualMeasuredIndices.end())
                    continue;
                if (oldest == nextState.measurements.end() ||
                    current->second.lastUsed < oldest->second.lastUsed)
                    oldest = current;
            }
            if (oldest == nextState.measurements.end()) break;
            nextState.measurements.erase(oldest);
            changed = true;
        }

        if (changed && node.itemCount > 0)
        {
            const auto newMeasurements =
                VariableVirtualMeasurements(nextState);
            float newAnchorStart = oldAnchorStart;
            if (ComputeViewVariableVirtualItemStart(node.itemCount,
                    *node.estimatedItemSize, mainGap, anchorIndex,
                    newMeasurements, newAnchorStart, rangeError))
            {
                ViewVirtualRange newRange;
                if (ComputeViewVariableVirtualRange(node.itemCount,
                        *node.estimatedItemSize, mainGap,
                        node.scrollViewportExtent, node.scrollOffset, 0,
                        newMeasurements, newRange, rangeError))
                {
                    auto& offsets = ScrollOffsetsForSurface(widget, surface);
                    const auto offset = offsets.find(node.key);
                    if (offset != offsets.end())
                    {
                        const std::int64_t anchored =
                            static_cast<std::int64_t>(offset->second) +
                            static_cast<std::int64_t>(std::lround(
                                newAnchorStart - oldAnchorStart));
                        offset->second = static_cast<int>(
                            std::clamp<std::int64_t>(anchored, 0,
                                static_cast<std::int64_t>(std::lround(
                                    newRange.maximum))));
                    }
                    states[node.key] = std::move(nextState);
                }
            }
        }
        else if (node.itemCount == 0 || !changed)
            states[node.key] = std::move(nextState);
    }
    for (auto& child : node.children)
        changed = CommitVariableVirtualMeasurements(
            child, widget, surface) || changed;
    return changed;
}

static std::string& ViewFocusForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewKeyboardFocusKey : widget.viewKeyboardFocusKey;
}

static const std::string& ViewFocusForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewKeyboardFocusKey : widget.viewKeyboardFocusKey;
}

static bool& ViewFocusCueForSurface(
    LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewFocusCueVisible : widget.viewFocusCueVisible;
}

static bool ViewFocusCueForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return IsPanelSurface(surface)
        ? widget.panelViewFocusCueVisible : widget.viewFocusCueVisible;
}

static std::string_view VisualViewFocusForSurface(
    const LuaWidget& widget, std::string_view surface)
{
    return ViewFocusCueForSurface(widget, surface)
        ? std::string_view(ViewFocusForSurface(widget, surface))
        : std::string_view{};
}

static RECT BoundsForSurface(
    const LuaWidget& widget, std::string_view surface) noexcept
{
    return IsPanelSurface(surface) ? widget.panelBounds : widget.lastBounds;
}

static bool HostControlBelongsToSurface(
    const LuaWidget::HostControl& control, std::string_view surface) noexcept
{
    return control.surface == surface;
}

static void SetWidgetRectContext(D2DState* state, RECT bounds)
{
    if (!state || bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
    state->widgetRect = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
    state->layoutContentWidth = static_cast<float>(bounds.right - bounds.left);
    state->layoutContentHeight = static_cast<float>(bounds.bottom - bounds.top);
}

using snowdesktop::widget_runtime::LuaResourceType;
using snowdesktop::widget_runtime::PushResourceHandle;
using snowdesktop::widget_runtime::ResolveResourceHandleImageContentKey;
using snowdesktop::widget_runtime::ResolveResourceHandlePath;
using snowdesktop::widget_runtime::TestResourceHandle;

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

static std::string WidgetPrewarmContentDigest(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > (std::numeric_limits<ULONG>::max)()) return {};
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
            digest.data(), static_cast<ULONG>(digest.size())) < 0)
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string key;
    key.reserve(64);
    for (const auto byte : digest)
    {
        key.push_back(hex[byte >> 4]);
        key.push_back(hex[byte & 0x0f]);
    }
    return key;
}

static void PrewarmWidgetPackageResources(D2DState* state)
{
    // Preparation is optional: the normal loader remains authoritative for
    // current files, permissions, errors, Lua execution and device resources.
    try
    {
        std::vector<std::wstring> files;
        std::vector<std::wstring> images;
        auto enqueue = [&](const std::filesystem::path& root,
            const std::wstring& relative, bool image = false) {
            if (files.size() + images.size() >= 128) return;
            if (const auto path = ResolvePackageAssetWithinRoot(root, relative))
                (image ? images : files).push_back(*path);
        };
        for (const auto& package : GetWidgetPackageManager().ListPackages())
        {
            if (!package.active || !package.enabled ||
                !snowdesktop::widget::IsExecutablePackageContract(package.manifest))
                continue;
            const auto grant = snowdesktop::widget::WidgetPermissionBroker::Evaluate(
                package.permissionState, package.manifest.permissions,
                package.manifest.optionalPermissions, package.manifest.networkDomains,
                package.grantedPermissions, package.grantedNetworkDomains);
            if (grant.runtimeBlock != snowdesktop::widget::PermissionRuntimeBlock::None)
                continue;
            enqueue(package.root, L"widget.json");
            enqueue(package.root, Utf8ToWideLocal(package.manifest.entry));
            const auto defersImages = [](const auto& features) {
                return std::any_of(features.begin(), features.end(), [](const auto& feature) {
                    return feature == "resource.image.lazy" ||
                        feature == "resource.image.onDemand";
                });
            };
            const bool deferredImages = defersImages(package.manifest.requiredFeatures) ||
                defersImages(package.manifest.optionalFeatures);
            for (const auto& [name, resource] : package.manifest.resources)
            {
                (void)name;
                if (resource.type == "image" && !deferredImages)
                    enqueue(package.root, Utf8ToWideLocal(resource.path), true);
            }
            if (files.size() + images.size() >= 128) break;
        }
        state->packageImageCache.Prewarm(std::move(files), std::move(images),
            WidgetPrewarmContentDigest);
    }
    catch (...)
    {
        // Speculation must never replace the synchronous load/error path.
    }
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

using snowdesktop::widget_runtime::IsFilesystemHandleTask;
using snowdesktop::widget_runtime::IsFilesystemPickerTask;

static bool IsPreviewFilesystemHandle(std::string_view handle)
{
    return handle == "filesystem:00000000000000000000000000000000" ||
        handle == "filesystem:11111111111111111111111111111111";
}

static const char* ResolveClipboardWriteFile(const LuaWidget& owner,
    const snowdesktop::widget_runtime::TaskBrokerAction& action,
    const snowdesktop::widget_runtime::WidgetFilesystemHandleStore* handleStore,
    const std::string& reference, std::filesystem::path& path)
{
    using snowdesktop::widget::WidgetPermissionBroker;
    using snowdesktop::widget_runtime::WidgetFilesystemHandleAccess;
    if (reference.starts_with("item:"))
    {
        if (!WidgetPermissionBroker::AllowsPermission(owner.permissions, kClipboardReadPermission))
            return "permissionDenied";
        const auto found = owner.itemReferences.find(reference);
        if (found == owner.itemReferences.end() || found->second.sourceTask != "clipboard.read")
            return "invalidReference";
        path = Utf8ToWideLocal(found->second.target);
        return nullptr;
    }
    if (!WidgetPermissionBroker::AllowsPermission(owner.permissions, kFilesystemReadPermission))
        return "permissionDenied";
    if (action.preview && IsPreviewFilesystemHandle(reference))
    {
        path = L"C:\\Preview\\Preview.txt";
        return nullptr;
    }
    const auto entry = handleStore ? handleStore->Resolve(
        { action.instanceId, owner.packageId }, reference) : std::nullopt;
    if (!entry) return "invalidReference";
    if (entry->access == WidgetFilesystemHandleAccess::Write)
        return "handleAccessDenied";
    path = entry->path;
    return nullptr;
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
    if (auto* d2d = GetD2D(state); d2d && d2d->backgroundLayerActive)
        return luaL_error(state,
            "interaction.region: backgroundLayer is decorative and cannot register interactions");
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
    lua_getfield(state, descriptor, "tooltip");
    if (!lua_isnil(state, -1))
    {
        if (lua_isstring(state, -1))
        {
            std::size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            region.tooltip.assign(value ? value : "", length);
        }
        else if (lua_istable(state, -1))
        {
            const int tooltip = lua_absindex(state, -1);
            lua_pushnil(state);
            while (lua_next(state, tooltip) != 0)
            {
                std::size_t keyLength = 0;
                const char* key = lua_tolstring(state, -2, &keyLength);
                const std::string_view field(key ? key : "", keyLength);
                if (lua_type(state, -2) != LUA_TSTRING ||
                    (field != "title" && field != "text"))
                {
                    lua_pop(state, 3);
                    return luaL_error(state,
                        "interaction.region: tooltip supports only title and text");
                }
                lua_pop(state, 1);
            }
            lua_getfield(state, tooltip, "title");
            if (!lua_isnil(state, -1))
            {
                std::size_t length = 0;
                const char* value = luaL_checklstring(state, -1, &length);
                region.tooltipTitle.assign(value ? value : "", length);
            }
            lua_pop(state, 1);
            lua_getfield(state, tooltip, "text");
            if (!lua_isstring(state, -1))
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "interaction.region: rich tooltip requires string text");
            }
            std::size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            region.tooltip.assign(value ? value : "", length);
            lua_pop(state, 1);
            if (region.tooltip.empty())
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "interaction.region: rich tooltip text must not be empty");
            }
        }
        else
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "interaction.region: tooltip must be a string or tooltip descriptor");
        }
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "enabled");
    region.enabled = lua_isnil(state, -1) || lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "capturePointer");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isboolean(state, -1))
            return luaL_error(state,
                "interaction.region: capturePointer must be a boolean");
        region.capturePointer = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "focusable");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isboolean(state, -1))
            return luaL_error(state,
                "interaction.region: focusable must be a boolean");
        region.focusable = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "tabIndex");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isinteger(state, -1))
            return luaL_error(state,
                "interaction.region: tabIndex must be an integer");
        const lua_Integer value = lua_tointeger(state, -1);
        if (value < -1 || value > 32767)
            return luaL_error(state,
                "interaction.region: tabIndex must be between -1 and 32767");
        region.tabIndex = static_cast<int>(value);
    }
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "accessibility");
    if (lua_istable(state, -1))
    {
        const int accessibility = lua_absindex(state, -1);
        lua_pushnil(state);
        while (lua_next(state, accessibility) != 0)
        {
            std::size_t keyLength = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            const std::string_view field(key ? key : "", keyLength);
            const bool supported = lua_type(state, -2) == LUA_TSTRING &&
                (field == "role" || field == "label" ||
                    field == "value" || field == "hint" ||
                    field == "headingLevel" || field == "live" ||
                    field == "positionInSet" || field == "setSize" ||
                    field == "hidden");
            lua_pop(state, 1);
            if (!supported)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "interaction.region: unsupported accessibility field");
            }
        }
        const auto readAccessibilityString = [&](const char* field,
            std::string& output) {
            lua_getfield(state, accessibility, field);
            if (!lua_isnil(state, -1))
            {
                std::size_t length = 0;
                const char* value = luaL_checklstring(
                    state, -1, &length);
                output.assign(value ? value : "", length);
            }
            lua_pop(state, 1);
        };
        readAccessibilityString("role", region.accessibilityRole);
        readAccessibilityString("label", region.accessibilityLabel);
        readAccessibilityString("value", region.accessibilityValue);
        readAccessibilityString("hint", region.accessibilityHint);
        const auto readAccessibilityInteger = [&](const char* field,
            int& output) {
            lua_getfield(state, accessibility, field);
            if (!lua_isnil(state, -1))
            {
                if (!lua_isinteger(state, -1))
                    return false;
                const lua_Integer value = lua_tointeger(state, -1);
                if (value < (std::numeric_limits<int>::min)() ||
                    value > (std::numeric_limits<int>::max)())
                    return false;
                output = static_cast<int>(value);
            }
            lua_pop(state, 1);
            return true;
        };
        if (!readAccessibilityInteger("headingLevel",
                region.accessibilityHeadingLevel) ||
            !readAccessibilityInteger("positionInSet",
                region.accessibilityPositionInSet) ||
            !readAccessibilityInteger("setSize",
                region.accessibilitySetSize))
            return luaL_error(state,
                "interaction.region: accessibility indices must be integers");
        std::string live;
        readAccessibilityString("live", live);
        if (live.empty() || live == "off")
            region.accessibilityLive = AccessibilityLive::Off;
        else if (live == "polite")
            region.accessibilityLive = AccessibilityLive::Polite;
        else if (live == "assertive")
            region.accessibilityLive = AccessibilityLive::Assertive;
        else
            return luaL_error(state,
                "interaction.region: accessibility live must be off, polite, or assertive");
        lua_getfield(state, accessibility, "hidden");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isboolean(state, -1))
                return luaL_error(state,
                    "interaction.region: accessibility hidden must be a boolean");
            region.accessibilityHidden =
                lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
    }
    else if (!lua_isnil(state, -1))
        return luaL_error(state,
            "interaction.region: accessibility must be a table");
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "events");
    if (lua_istable(state, -1))
    {
        const int events = lua_absindex(state, -1);
        for (const char* eventName : { "pointerEnter", "pointerLeave",
            "pointerDown", "pointerUp", "pointerMove", "click",
            "doubleClick", "fileDrop", "wheel", "contextMenu", "keyDown", "keyUp" })
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

    if (region.capturePointer &&
        !region.events.contains("pointerMove") &&
        !region.events.contains("pointerUp"))
    {
        return luaL_error(state,
            "interaction.region: capturePointer requires pointerMove or pointerUp");
    }

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

static int lua_InteractionIsFocused(lua_State* state)
{
    const char* key = luaL_checkstring(state, 1);
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeInteractionFocused(
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

    std::string orientation = "vertical";
    lua_getfield(state, descriptor, "orientation");
    if (!lua_isnil(state, -1))
    {
        std::size_t length = 0;
        const char* value = luaL_checklstring(state, -1, &length);
        orientation.assign(value ? value : "", length);
    }
    lua_pop(state, 1);
    if (orientation != "vertical" && orientation != "horizontal")
        return luaL_error(state,
            "interaction.scroll: orientation must be vertical or horizontal");
    const bool horizontal = orientation == "horizontal";
    const char* contentField = horizontal ? "contentWidth" : "contentHeight";
    const char* rejectedField = horizontal ? "contentHeight" : "contentWidth";
    lua_getfield(state, descriptor, rejectedField);
    const bool hasRejectedExtent = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (hasRejectedExtent)
        return luaL_error(state, horizontal
            ? "interaction.scroll: horizontal scrolling uses contentWidth, not contentHeight"
            : "interaction.scroll: vertical scrolling uses contentHeight, not contentWidth");
    lua_getfield(state, descriptor, contentField);
    if (!lua_isinteger(state, -1))
        return luaL_error(state, horizontal
            ? "interaction.scroll: contentWidth must be an integer"
            : "interaction.scroll: contentHeight must be an integer");
    const lua_Integer requestedContentExtent = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (requestedContentExtent < 0 ||
        requestedContentExtent > 1'000'000)
    {
        return luaL_error(state, horizontal
            ? "interaction.scroll: contentWidth must be from 0 to 1000000"
            : "interaction.scroll: contentHeight must be from 0 to 1000000");
    }

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "interaction.scroll: host context is unavailable");
    const int viewportWidth = std::max(
        1, static_cast<int>(std::lround(width)));
    const int viewportHeight = std::max(
        1, static_cast<int>(std::lround(height)));
    const int viewportExtent = horizontal ? viewportWidth : viewportHeight;
    const int contentExtent = std::max(
        viewportExtent, static_cast<int>(requestedContentExtent));
    LuaWidget::HostControl control;
    control.type = LuaWidget::HostControl::Type::Scroll;
    control.id = key;
    control.horizontal = horizontal;
    control.contentWidth = horizontal ? contentExtent : viewportWidth;
    control.viewportWidth = viewportWidth;
    control.contentHeight = horizontal ? viewportHeight : contentExtent;
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
    lua_createtable(state, 0, 7);
    lua_pushinteger(state, offset);
    lua_setfield(state, -2, "offset");
    lua_pushinteger(state, std::max(0, contentExtent - viewportExtent));
    lua_setfield(state, -2, "maximum");
    lua_pushlstring(state, orientation.data(), orientation.size());
    lua_setfield(state, -2, "orientation");
    lua_pushinteger(state, viewportExtent);
    lua_setfield(state, -2, "viewportExtent");
    lua_pushinteger(state, contentExtent);
    lua_setfield(state, -2, "contentExtent");
    lua_pushinteger(state, viewportExtent);
    lua_setfield(state, -2,
        horizontal ? "viewportWidth" : "viewportHeight");
    lua_pushinteger(state, contentExtent);
    lua_setfield(state, -2,
        horizontal ? "contentWidth" : "contentHeight");
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

static int lua_UiDurationPicker(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    if (luaL_loadbuffer(state, kWidgetDurationPickerLua,
            sizeof(kWidgetDurationPickerLua) - 1, "@host/ui.durationPicker") != LUA_OK)
        return lua_error(state);
    lua_pushvalue(state, 1);
    lua_newtable(state);
    lua_pushstring(state, _L("app.widget.time_picker.hour")); lua_setfield(state, -2, "hours");
    lua_pushstring(state, _L("app.widget.time_picker.minute")); lua_setfield(state, -2, "minutes");
    lua_pushstring(state, _L("app.widget.duration_picker.second")); lua_setfield(state, -2, "seconds");
    lua_pushstring(state, _L("app.widget.duration_picker.invalid")); lua_setfield(state, -2, "invalid");
    lua_pushstring(state, _L("app.widget.duration_picker.increase")); lua_setfield(state, -2, "increase");
    lua_pushstring(state, _L("app.widget.duration_picker.decrease")); lua_setfield(state, -2, "decrease");
    lua_pushstring(state, _L("app.widget.date_picker.confirm")); lua_setfield(state, -2, "confirm");
    lua_call(state, 2, 1);
    return 1;
}

static int lua_UiTimePicker(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    if (luaL_loadbuffer(state, kWidgetTimePickerLua, sizeof(kWidgetTimePickerLua)-1, "@host/ui.timePicker") != LUA_OK) return lua_error(state);
    lua_pushvalue(state,1);
    lua_newtable(state);
    lua_pushstring(state, _L("app.widget.time_picker.time")); lua_setfield(state,-2,"time");
    lua_pushstring(state, _L("app.widget.time_picker.startTime")); lua_setfield(state,-2,"startTime");
    lua_pushstring(state, _L("app.widget.time_picker.endTime")); lua_setfield(state,-2,"endTime");
    lua_pushstring(state, _L("app.widget.time_picker.hour")); lua_setfield(state,-2,"hour");
    lua_pushstring(state, _L("app.widget.time_picker.minute")); lua_setfield(state,-2,"minute");
    lua_pushstring(state, _L("app.widget.time_picker.invalid")); lua_setfield(state,-2,"invalid");
    lua_pushstring(state, _L("app.widget.time_picker.order")); lua_setfield(state,-2,"order");
    lua_pushstring(state, _L("app.widget.date_picker.clear")); lua_setfield(state,-2,"clear");
    lua_pushstring(state, _L("app.widget.date_picker.confirm")); lua_setfield(state,-2,"confirm");
    lua_call(state,2,1);
    return 1;
}

static int lua_UiDatePicker(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    if (luaL_loadbuffer(state, kWidgetDatePickerLua,
            sizeof(kWidgetDatePickerLua) - 1, "@host/ui.datePicker") != LUA_OK)
        return lua_error(state);
    lua_pushvalue(state, 1);
    lua_newtable(state);
    lua_pushstring(state, _L("app.widget.date_picker.date"));
    lua_setfield(state, -2, "date");
    lua_pushstring(state, _L("app.widget.date_picker.startDate"));
    lua_setfield(state, -2, "startDate");
    lua_pushstring(state, _L("app.widget.date_picker.endDate"));
    lua_setfield(state, -2, "endDate");
    lua_pushstring(state, _L("app.widget.date_picker.year"));
    lua_setfield(state, -2, "year");
    lua_pushstring(state, _L("app.widget.date_picker.month"));
    lua_setfield(state, -2, "month");
    lua_pushstring(state, _L("app.widget.date_picker.previous"));
    lua_setfield(state, -2, "previous");
    lua_pushstring(state, _L("app.widget.date_picker.next"));
    lua_setfield(state, -2, "next");
    lua_pushstring(state, _L("app.widget.date_picker.today"));
    lua_setfield(state, -2, "today");
    lua_pushstring(state, _L("app.widget.date_picker.clear"));
    lua_setfield(state, -2, "clear");
    lua_pushstring(state, _L("app.widget.date_picker.confirm"));
    lua_setfield(state, -2, "confirm");
    lua_pushstring(state, _L("app.widget.date_picker.invalid"));
    lua_setfield(state, -2, "invalid");
    lua_pushstring(state, _L("app.widget.date_picker.order"));
    lua_setfield(state, -2, "order");
    lua_pushstring(state, _L("app.widget.date_picker.weekday1"));
    lua_setfield(state, -2, "weekday1");
    lua_pushstring(state, _L("app.widget.date_picker.weekday2"));
    lua_setfield(state, -2, "weekday2");
    lua_pushstring(state, _L("app.widget.date_picker.weekday3"));
    lua_setfield(state, -2, "weekday3");
    lua_pushstring(state, _L("app.widget.date_picker.weekday4"));
    lua_setfield(state, -2, "weekday4");
    lua_pushstring(state, _L("app.widget.date_picker.weekday5"));
    lua_setfield(state, -2, "weekday5");
    lua_pushstring(state, _L("app.widget.date_picker.weekday6"));
    lua_setfield(state, -2, "weekday6");
    lua_pushstring(state, _L("app.widget.date_picker.weekday7"));
    lua_setfield(state, -2, "weekday7");
    lua_call(state, 2, 1);
    return 1;
}

static int lua_UiMenu(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    return 1;
}

static int lua_TaskStart(lua_State* state)
{
    using snowdesktop::widget_runtime::LuaTaskStartHost;
    static constexpr LuaTaskStartHost host{
        [](lua_State* taskState, const std::string& name,
            LuaTaskStartHost::Arguments&& arguments)
            -> std::optional<snowdesktop::widget_runtime::TaskStartResult> {
            auto* d2d = GetD2D(taskState);
            if (!d2d || !d2d->engine) return std::nullopt;
            return d2d->engine->RuntimeStartTask(
                BoundWidgetId(taskState), BoundWidgetRuntimeToken(taskState),
                name, std::move(arguments));
        },
        [](lua_State* taskState, int index) {
            return ResolveResourceHandlePath(
                taskState, index, LuaResourceType::Image).has_value();
        },
        IsValidUtf8Local,
    };
    return snowdesktop::widget_runtime::StartTaskFromLua(state, host);
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

static bool HostTextIsWhitespaceOnly(const std::wstring& text)
{
    return std::all_of(text.begin(), text.end(), [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' ||
            ch == L'\n' || ch == L'\v' || ch == L'\f';
    });
}

static snowdesktop::widget_runtime::InteractionAction ReadControlSubmitAction(
    lua_State* state, int descriptor, const char* api);

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
    const float fontSize = static_cast<float>(
        std::clamp(numberOption("fontSize", 15.0), 9.0, 96.0));
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
        control.placeholder = placeholder;
        control.fontSize = fontSize;
        control.padding = padding;
        control.maximumUtf8Bytes = maximumUtf8Bytes;
        control.submitAction = ReadControlSubmitAction(L, options, "control.textInput");
        std::string error;
        if (!s->engine->RuntimeRegisterV2HostControl(
                BoundWidgetId(L), std::move(control), error))
        {
            return luaL_error(L, "control.textInput: %s",
                error.c_str());
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
    const float fontSize = static_cast<float>(
        std::clamp(numberOption("fontSize", 15.0), 9.0, 96.0));
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
    const auto verticalExtents = snowdesktop::widget_runtime::
        ResolveHostInputVerticalExtents(
            height, textMetrics.height + padding * 2.0f);

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
        control.placeholder = placeholder;
        control.fontSize = fontSize;
        control.padding = padding;
        control.contentHeight = verticalExtents.content;
        control.viewportHeight = verticalExtents.viewport;
        control.maximumUtf8Bytes = maximumUtf8Bytes;
        control.submitAction = ReadControlSubmitAction(L, options, "control.textArea");
        std::string error;
        if (!s->engine->RuntimeRegisterV2HostControl(
                BoundWidgetId(L), std::move(control), error))
        {
            return luaL_error(L, "control.textArea: %s",
                error.c_str());
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
            if (hasCaret && s && s->engine &&
                s->engine->RuntimeConsumeHostInputCaretVisibilityRequest(
                    BoundWidgetId(L), id))
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

static snowdesktop::widget_runtime::InteractionAction ReadControlSubmitAction(
    lua_State* state, int descriptor, const char* api)
{
    snowdesktop::widget_runtime::InteractionAction action;
    if (!descriptor) return action;
    lua_getfield(state, lua_absindex(state, descriptor), "events");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return action;
    }
    luaL_checktype(state, -1, LUA_TTABLE);
    const int events = lua_absindex(state, -1);
    static constexpr const char* eventFields[] = { "submit" };
    ValidateControlTableFields(state, events, eventFields, api);
    lua_getfield(state, events, "submit");
    if (!lua_isnil(state, -1))
    {
        luaL_checktype(state, -1, LUA_TTABLE);
        const int binding = lua_absindex(state, -1);
        static constexpr const char* actionFields[] = { "id", "value" };
        ValidateControlTableFields(state, binding, actionFields, api);
        action.id = ReadControlIdentifier(state, binding, "id", api);
        lua_getfield(state, binding, "value");
        std::size_t nodes = 0;
        std::size_t bytes = 0;
        std::unordered_set<const void*> ancestors;
        std::string error;
        if (!ReadInteractionValue(state, -1, action.value, 0,
                nodes, bytes, ancestors, error))
            luaL_error(state, "%s: %s", api, error.c_str());
        lua_pop(state, 1);
    }
    lua_pop(state, 2);
    return action;
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

static void ValidateOptionalControlFontSize(lua_State* state, int table,
    const char* api)
{
    lua_getfield(state, lua_absindex(state, table), "fontSize");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return;
    }
    if (lua_type(state, -1) != LUA_TNUMBER)
    {
        luaL_error(state, "%s: fontSize must be a number", api);
        return;
    }
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    // Row, page, and user scaling can exceed the editor's rendering range.
    // The editor clamps valid sizes before converting them to float.
    if (!std::isfinite(value) || value <= 0.0)
        luaL_error(state, "%s: fontSize must be finite and positive", api);
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
        "liveUpdate", "maxBytes", "events",
    };
    static constexpr const char* areaFields[] = {
        "key", "storageKey", "shape", "placeholder",
        "placeholderWhenWhitespace", "fontSize", "textColor",
        "placeholderColor", "backgroundColor", "borderColor",
        "focusedBorderColor", "backgroundAlpha",
        "focusedBackgroundAlpha", "borderAlpha", "focusedBorderAlpha",
        "radius", "padding", "borderThickness", "selectAll",
        "liveUpdate", "maxBytes", "events",
    };
    ValidateControlTableFields(state, descriptor,
        multiline ? std::span<const char* const>(areaFields)
                  : std::span<const char* const>(inputFields), api);
    const std::string key = ReadControlIdentifier(
        state, descriptor, "key", api);
    const std::string storageKey = ReadControlIdentifier(
        state, descriptor, "storageKey", api);
    (void)ReadControlSubmitAction(state, descriptor, api);

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
    ValidateOptionalControlFontSize(state, descriptor, api);
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

static int lua_ControlBlur(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state, "control.blur: expected one key");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state, "control.blur: key must be a string");
    std::size_t length = 0;
    const char* raw = lua_tolstring(state, 1, &length);
    const std::string key(raw ? raw : "", length);
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "control.blur: key must contain 1 to 128 bytes of valid UTF-8");
    }
    auto* d2d = GetD2D(state);
    std::string error = "hostUnavailable";
    const bool blurred = d2d && d2d->engine &&
        d2d->engine->RuntimeBlurHostInputFromTrustedGesture(
            BoundWidgetId(state), key, error);
    lua_pushboolean(state, blurred ? 1 : 0);
    if (blurred)
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
    if (snowdesktop::widget_runtime::IsPreviewExecution())
    {
        WidgetSystemEnvironment preview;
        preview.locale = Locale::Instance().GetEffectiveLanguage();
        preview.inputLanguage = preview.locale;
        preview.timeZone = "UTC";
        preview.utcOffsetMinutes = 0;
        const std::wstring localeName =
            Utf8ToWideLocal(preview.locale);
        wchar_t region[LOCALE_NAME_MAX_LENGTH]{};
        if (!localeName.empty() && GetLocaleInfoEx(localeName.c_str(),
                LOCALE_SISO3166CTRYNAME, region,
                LOCALE_NAME_MAX_LENGTH) > 0)
            preview.region = WidgetWideToUtf8(region);
        return preview;
    }
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

static int lua_UiMetrics(lua_State* L)
{
    auto* d2d = GetD2D(L);
    const WidgetSystemEnvironment system = QueryWidgetSystemEnvironment();
    const auto metrics = snowdesktop::widget_runtime::
        ResolveSemanticUiMetrics(
            d2d ? d2d->semanticUiMetrics :
                snowdesktop::widget_runtime::SemanticUiMetricTokens{},
            d2d ? d2d->semanticCuScale : 1.0f,
            static_cast<float>(system.textScale),
            IsPanelSurface(CurrentWidgetSurface(d2d)) ? 1.0f :
                snowdesktop::widget_runtime::ResolveSemanticRowScale(
                    d2d ? d2d->gridRows : 2));

    lua_createtable(L, 0, 1);
    lua_pushnumber(L, metrics.layoutRowHeight);
    lua_setfield(L, -2, "layoutRowHeight");
    return 1;
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


static int LuaWidgetContextWithDeferredError(lua_State* caller)
{
    auto* contextD2D = GetD2D(caller);
    lua_getfield(caller, LUA_REGISTRYINDEX, "__widget_preview");
    const bool preview = lua_toboolean(caller, -1) != 0;
    lua_pop(caller, 1);
    const std::wstring widgetId = BoundWidgetId(caller);
    LuaWidgetContextState contextState = contextD2D && contextD2D->engine
        ? contextD2D->engine->RuntimeGetWidgetContextState(widgetId)
        : LuaWidgetContextState{};
    if (preview) contextState.preview = true;
    const WidgetSystemEnvironment contextSystem = QueryWidgetSystemEnvironment();
    const double contextScaleX = static_cast<double>(contextState.surface.dpiX) /
        USER_DEFAULT_SCREEN_DPI;
    const double contextScaleY = static_cast<double>(contextState.surface.dpiY) /
        USER_DEFAULT_SCREEN_DPI;
    const double contextPixelWidth = contextD2D
        ? contextD2D->widgetRect.right - contextD2D->widgetRect.left : 0.0;
    const double contextPixelHeight = contextD2D
        ? contextD2D->widgetRect.bottom - contextD2D->widgetRect.top : 0.0;
    const int contextColumns = contextD2D ? std::max(1, contextD2D->gridColumns) : 1;
    const int contextRows = contextD2D ? std::max(1, contextD2D->gridRows) : 1;
    const int contextArea = contextColumns * contextRows;
    const LuaWidgetTheme contextTheme = contextD2D && contextD2D->engine
        ? contextD2D->engine->RuntimeGetWidgetTheme(widgetId)
        : LuaWidgetTheme{};

    struct ContextArguments
    {
        D2DState* d2d;
        const LuaWidgetContextState* state;
        const WidgetSystemEnvironment* system;
        const LuaWidgetTheme* theme;
        double scaleX, scaleY, pixelWidth, pixelHeight;
        int columns, rows, area;
    } arguments{contextD2D, &contextState, &contextSystem, &contextTheme,
        contextScaleX, contextScaleY, contextPixelWidth, contextPixelHeight,
        contextColumns, contextRows, contextArea};
    std::string error;
    // Own all native strings above the protected call. Lua allocation failure
    // can jump only across this push-only callback, never their destructors.
    const bool pushed = snowdesktop::lua_runtime::ProtectedHostCall(caller,
        +[](lua_State* inner) {
            const auto& input = *static_cast<ContextArguments*>(lua_touserdata(inner, 1));
            lua_State* L = inner;
            auto* d2d = input.d2d;
            const auto& state = *input.state;
            const auto& system = *input.system;
            const auto& theme = *input.theme;
            const double scaleX = input.scaleX, scaleY = input.scaleY;
            const double pixelWidth = input.pixelWidth, pixelHeight = input.pixelHeight;
            const int columns = input.columns, rows = input.rows, area = input.area;

            lua_createtable(L, 0, 19);
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, pixelWidth / scaleX); lua_setfield(L, -2, "width");
            lua_pushnumber(L, pixelHeight / scaleY); lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "logicalSize");
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, pixelWidth); lua_setfield(L, -2, "width");
            lua_pushnumber(L, pixelHeight); lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "pixelSize");
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, CurrentLayoutContentWidth(d2d));
            lua_setfield(L, -2, "width");
            lua_pushnumber(L, CurrentLayoutContentHeight(d2d));
            lua_setfield(L, -2, "height");
            lua_setfield(L, -2, "layoutSize");
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
        }, &arguments, 1, error);
    return pushed ? 1 : DeferLuaError(caller, error);
}

static int lua_WidgetContext(lua_State* L)
{
    const int result = LuaWidgetContextWithDeferredError(L);
    return result < 0 ? lua_error(L) : result;
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

static char kModuleLoadingMarker = 0;

static void ClearModuleCacheEntry(lua_State* L, int cache,
    const std::string& key)
{
    lua_pushnil(L);
    lua_setfield(L, cache, key.c_str());
}

// Return ordinary validation/load errors before raising them in Lua. Lua uses
// longjmp on this build, so a rejection must not unwind the C++ file/path/string
// locals below. Keep the worker out of the object-free Lua callback's frame.
static __declspec(noinline) int LoadRequiredWidgetModule(lua_State* L,
    const char* pathRaw, size_t pathLength, const char*& error)
{
    const std::wstring relativePath = Utf8ToWideLocal(
        std::string(pathRaw, pathLength));
    if (relativePath.empty() ||
        _wcsicmp(std::filesystem::path(relativePath).extension().c_str(),
            L".lua") != 0)
    {
        error = "module.require: path must name a .lua file";
        return 0;
    }
    const auto fullPath = ResolveCurrentPackageAsset(L, relativePath);
    if (!fullPath)
    {
        error = "module.require: path is outside the component package";
        return 0;
    }

    std::error_code fileError;
    const auto fileSize = std::filesystem::file_size(*fullPath, fileError);
    if (fileError || fileSize > snowdesktop::widget::kMaxEntryLuaBytes)
    {
        error = "module.require: module is missing or exceeds the 1 MiB limit";
        return 0;
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
    {
        error = "module.require: circular dependency detected";
        return 0;
    }
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
        error = "module.require: per-instance module count or byte quota exceeded";
        return 0;
    }

    lua_pushlightuserdata(L, &kModuleLoadingMarker);
    lua_setfield(L, cache, cacheKey.c_str());
    std::ifstream file(*fullPath, std::ios::binary);
    std::string source(static_cast<std::size_t>(fileSize), '\0');
    if (!file || (fileSize > 0 &&
        !file.read(source.data(), static_cast<std::streamsize>(fileSize))))
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        error = "module.require: module could not be read";
        return 0;
    }
    const std::string chunkName = "@module/" +
        WidgetWideToUtf8(std::filesystem::path(relativePath).
            lexically_normal().generic_wstring());
    if (luaL_loadbufferx(L, source.data(), source.size(),
            chunkName.c_str(), "t") != LUA_OK)
    {
        ClearModuleCacheEntry(L, cache, cacheKey);
        return 0;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_environment");
    if (!lua_istable(L, -1) || lua_setupvalue(L, -2, 1) == nullptr)
    {
        if (lua_isnil(L, -1)) lua_pop(L, 1);
        ClearModuleCacheEntry(L, cache, cacheKey);
        error = "module.require: sandbox environment is unavailable";
        return 0;
    }
    // Reserve this module before its code can load children. Failed parents
    // release only their own reservation; successful cached children remain.
    lua_pushinteger(L, moduleCount + 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
    lua_pushinteger(L, moduleBytes + static_cast<lua_Integer>(fileSize));
    lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
    if (snowdesktop::lua_runtime::ProtectedCall(L, 0, 1) != LUA_OK)
    {
        lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
        const lua_Integer currentCount = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
        const lua_Integer currentBytes = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushinteger(L, currentCount - 1);
        lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_count");
        lua_pushinteger(L, currentBytes - static_cast<lua_Integer>(fileSize));
        lua_setfield(L, LUA_REGISTRYINDEX, "__widget_module_bytes");
        ClearModuleCacheEntry(L, cache, cacheKey);
        return 0;
    }
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, cache, cacheKey.c_str());
    lua_remove(L, cache);
    return 1;
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

    const char* error = nullptr;
    const int results = LoadRequiredWidgetModule(L, pathRaw, pathLength, error);
    if (results != 0) return results;
    if (error) return luaL_error(L, "%s", error);
    return lua_error(L);
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
    state->backgroundCache.Clear();
    state->textLayoutCache.Clear();
    state->textGlowCache.Clear();
    state->imageSamplingCache.Clear();
    state->immediateCommandContext.Reset();
    state->imageCache.clear();
    state->imageCacheRevisions.clear();
    state->runtimeImageBitmaps.clear();
    ClearShellIconBitmaps(state);
    state->shellIconFailures.clear();
}

static ID2D1Bitmap1* LoadImageBitmap(
    D2DState* s, const std::string& contentKey,
    float width = 0.0f, float height = 0.0f,
    snowdesktop::widget_runtime::DrawImageFit fit =
        snowdesktop::widget_runtime::DrawImageFit::Fill,
    bool originalPixels = false,
    snowdesktop::widget_runtime::PackageImageSize* originalSize = nullptr)
{
    if (!s || !s->ctx || contentKey.empty()) return nullptr;
    EnsureBitmapCachesForCurrentDevice(s);
    auto original = s->packageImageCache.OriginalSize(contentKey);
    const bool synchronous = s->engine && s->engine->IsPreviewOnly();
    if (!original.width && width > 0 && height > 0)
    {
        s->packageImageCache.RequestMetadata(contentKey, ImageUseOwner(s), synchronous);
        original = s->packageImageCache.OriginalSize(contentKey);
    }
    if (originalSize) *originalSize = original;
    if (!original.width || !original.height) return nullptr;
    const auto* source = s->packageImageCache.Find(contentKey);
    if (width > 0 && height > 0)
    {
        D2D1_MATRIX_3X2_F transform{};
        s->ctx->GetTransform(&transform);
        float dpiX = 96.0f, dpiY = 96.0f;
        s->ctx->GetDpi(&dpiX, &dpiY);
        const double scale = snowdesktop::widget_runtime::RequiredImageRasterScale(
            static_cast<float>(original.width), static_cast<float>(original.height),
            width, height, fit, {transform._11, transform._12, transform._21,
                transform._22, dpiX, dpiY,
                s->ctx->GetUnitMode() == D2D1_UNIT_MODE_PIXELS}, originalPixels);
        source = s->packageImageCache.Request(contentKey, ImageUseOwner(s),
            scale, snowdesktop::widget_runtime::WidgetPackageImageCache::Clock::now(),
            synchronous);
    }
    auto it = s->imageCache.find(contentKey);
    const auto revision = s->packageImageCache.Revision(contentKey);
    if (it != s->imageCache.end() &&
        (s->imageCacheRevisions[contentKey] == revision || !source))
        return it->second.Get();
    if ((!source || source->pixels.empty()) && width <= 0 &&
        s->packageImageCache.Ready(contentKey))
    {
        const std::string owner = "load:" + ImageUseOwner(s);
        source = s->packageImageCache.Request(contentKey, owner, 1.0,
            snowdesktop::widget_runtime::WidgetPackageImageCache::Clock::now(), true);
        s->packageImageCache.ForgetOwner(owner);
    }
    if (!source || source->pixels.empty()) return nullptr;
    ComPtr<ID2D1Bitmap1> bitmap;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
        D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(s->ctx->CreateBitmap(
            D2D1::SizeU(source->width, source->height),
            source->pixels.data(), source->stride,
            &properties, &bitmap)) || !bitmap)
        return nullptr;
    ID2D1Bitmap1* result = bitmap.Get();
    if (s->imageCache.size() >= 128)
    {
        // Retain only a bounded small BGRA8 working set past the original
        // 128-entry boundary. This estimates source pixels, not GPU residency.
        constexpr std::size_t smallImageEntryLimit = 256;
        constexpr std::uint64_t smallImagePixelByteLimit =
            16ULL * 1024ULL * 1024ULL;
        std::uint64_t estimatedPixelBytes = 0;
        const auto addBgra8PixelBytes =
            [&](ID2D1Bitmap1* cachedBitmap) -> bool {
                if (!cachedBitmap || cachedBitmap->GetPixelFormat().format !=
                        DXGI_FORMAT_B8G8R8A8_UNORM)
                    return false;
                const D2D1_SIZE_U pixels = cachedBitmap->GetPixelSize();
                const std::uint64_t width = pixels.width;
                const std::uint64_t height = pixels.height;
                if (width == 0 || height == 0)
                    return false;
                // Divide before multiplying: a successful check proves
                // width*height*4 <= 16MiB, so the uint64_t product is safe.
                if (height > (smallImagePixelByteLimit / 4ULL) / width)
                    return false;
                const std::uint64_t pixelBytes = width * height * 4ULL;
                // Running total never exceeds the limit; subtraction and
                // addition remain safe even for adversarial bitmap sizes.
                if (pixelBytes > smallImagePixelByteLimit -
                        estimatedPixelBytes)
                    return false;
                estimatedPixelBytes += pixelBytes;
                return true;
            };
        bool retainSmallWorkingSet =
            s->imageCache.size() < smallImageEntryLimit &&
            addBgra8PixelBytes(bitmap.Get());
        if (retainSmallWorkingSet)
        {
            for (const auto& cached : s->imageCache)
            {
                if (!addBgra8PixelBytes(cached.second.Get()))
                {
                    retainSmallWorkingSet = false;
                    break;
                }
            }
        }
        if (!retainSmallWorkingSet)
        {
            s->imageSamplingCache.Clear();
            s->imageCache.clear();
            s->imageCacheRevisions.clear();
        }
    }
    if (const auto old = s->imageCache.find(contentKey); old != s->imageCache.end())
        s->imageSamplingCache.Remove(old->second.Get());
    s->imageCache[contentKey] = bitmap;
    s->imageCacheRevisions[contentKey] = s->packageImageCache.Revision(contentKey);
    return result;
}

static void ReleasePackageImageResourcesUnprotected(
    D2DState* state, lua_State* luaState)
{
    if (!state || !luaState) return;
    state->packageImageCache.ForgetOwner(
        WidgetWideToUtf8(BoundWidgetId(luaState)) + "|");
    lua_getfield(luaState, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    if (lua_istable(luaState, -1))
    {
        lua_pushnil(luaState);
        while (lua_next(luaState, -2) != 0)
        {
            size_t length = 0;
            const char* value = lua_tolstring(luaState, -1, &length);
            if (value && length > 0)
            {
                const std::string contentKey(value, length);
                if (state->packageImageCache.Release(contentKey))
                {
                    if (const auto found = state->imageCache.find(contentKey);
                        found != state->imageCache.end())
                    {
                        state->imageSamplingCache.Remove(found->second.Get());
                        state->imageCache.erase(found);
                        state->imageCacheRevisions.erase(contentKey);
                    }
                }
            }
            lua_pop(luaState, 1);
        }
    }
    lua_pop(luaState, 1);
    lua_pushnil(luaState);
    lua_setfield(luaState, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
}

static void ReleasePackageImageResources(D2DState* state, lua_State* luaState)
{
    if (!state || !luaState) return;
    std::string error;
    (void)snowdesktop::lua_runtime::ProtectedHostCall(luaState,
        +[](lua_State* inner) {
            ReleasePackageImageResourcesUnprotected(
                static_cast<D2DState*>(lua_touserdata(inner, 1)), inner);
            return 0;
        }, state, 0, error);
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
    const auto source = state->runtimeImages.find(key);
    if (source == state->runtimeImages.end() || !source->second.pixels ||
        source->second.ownerWidgetId != widgetId)
        return nullptr;
    if (const auto cached = state->runtimeImageBitmaps.find(key);
        cached != state->runtimeImageBitmaps.end()) return cached->second.Get();
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

static int lua_ViewVirtualRange(lua_State* state)
{
    using snowdesktop::widget_runtime::ComputeViewVirtualRange;
    using snowdesktop::widget_runtime::ComputeViewVirtualItemScrollOffset;
    using snowdesktop::widget_runtime::ViewTreeLimits;
    using snowdesktop::widget_runtime::ViewVirtualRange;
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    const int descriptor = lua_absindex(state, 1);
    std::string error;
    if (!LuaTableUsesOnlyFields(state, descriptor,
            { "key", "itemCount", "itemExtent", "viewportExtent",
                "estimatedItemSize", "layoutRevision", "columns",
                "orientation", "columnGap", "rowGap", "overscan",
                "initialScrollIndex",
                "sectionHeaderIndices" },
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
    const auto hasField = [&](const char* field) {
        lua_getfield(state, descriptor, field);
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        return present;
    };
    const lua_Integer itemCountValue = readInteger("itemCount", -1);
    const lua_Integer columnsValue = readInteger("columns", 1);
    const lua_Integer overscanValue = readInteger("overscan", 2);
    const lua_Integer initialScrollIndexValue =
        readInteger("initialScrollIndex", 0);
    const lua_Integer layoutRevisionValue =
        readInteger("layoutRevision", 0);
    const double itemExtentValue = readNumber(
        "itemExtent", std::numeric_limits<double>::quiet_NaN());
    const double estimatedItemSizeValue = readNumber(
        "estimatedItemSize", std::numeric_limits<double>::quiet_NaN());
    const double viewportExtentValue = readNumber(
        "viewportExtent", std::numeric_limits<double>::quiet_NaN());
    const double columnGapValue = readNumber("columnGap", 0.0);
    const double rowGapValue = readNumber("rowGap", 0.0);
    std::string orientation = "vertical";
    lua_getfield(state, descriptor, "orientation");
    if (!lua_isnil(state, -1))
    {
        size_t length = 0;
        const char* value = luaL_checklstring(state, -1, &length);
        orientation.assign(value, length);
    }
    lua_pop(state, 1);
    if (orientation != "vertical" && orientation != "horizontal")
        return luaL_error(state,
            "view.virtualRange: orientation must be vertical or horizontal");
    const bool horizontal = orientation == "horizontal";
    if ((horizontal && hasField("rowGap")) ||
        (!horizontal && hasField("columnGap")))
        return luaL_error(state, horizontal
            ? "view.virtualRange: horizontal ranges use columnGap, not rowGap"
            : "view.virtualRange: vertical ranges use rowGap, not columnGap");
    const double mainGapValue = horizontal
        ? columnGapValue : rowGapValue;
    if (itemCountValue < 0 ||
        itemCountValue > static_cast<lua_Integer>(
            ViewTreeLimits::MaximumVirtualItemCount) ||
        columnsValue <= 0 || columnsValue > 64 ||
        overscanValue < 0 ||
        overscanValue > static_cast<lua_Integer>(
            ViewTreeLimits::MaximumVirtualOverscan) ||
        initialScrollIndexValue < 0 ||
        initialScrollIndexValue > itemCountValue ||
        layoutRevisionValue < 0)
    {
        return luaL_error(state,
            "view.virtualRange: integer arguments exceed their limits");
    }

    std::vector<std::size_t> sectionHeaderIndices;
    lua_getfield(state, descriptor, "sectionHeaderIndices");
    if (!lua_isnil(state, -1))
    {
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "view.virtualRange: sectionHeaderIndices must be an array");
        }
        if (!LuaTableIsContiguousArray(state, -1,
                "view.virtualRange.sectionHeaderIndices", error))
        {
            lua_pop(state, 1);
            return luaL_error(state, "view.virtualRange: %s",
                error.c_str());
        }
        const std::size_t count = lua_rawlen(state, -1);
        if (count > ViewTreeLimits::MaximumVirtualSectionHeaders)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "view.virtualRange: sectionHeaderIndices exceeds its item limit");
        }
        sectionHeaderIndices.reserve(count);
        std::size_t previous = 0;
        for (std::size_t item = 1; item <= count; ++item)
        {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(item));
            const bool valid = lua_isinteger(state, -1) &&
                lua_tointeger(state, -1) > 0 &&
                lua_tointeger(state, -1) <= itemCountValue &&
                static_cast<std::size_t>(lua_tointeger(state, -1)) >
                    previous;
            if (!valid)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "view.virtualRange: sectionHeaderIndices must be sorted unique bounded 1-based indices");
            }
            previous = static_cast<std::size_t>(
                lua_tointeger(state, -1));
            sectionHeaderIndices.push_back(previous);
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "view.virtualRange: host context is unavailable");
    const bool fixedExtent = std::isfinite(itemExtentValue);
    const bool variableExtent = std::isfinite(estimatedItemSizeValue);
    if (fixedExtent == variableExtent ||
        (variableExtent && columnsValue != 1) ||
        (!sectionHeaderIndices.empty() && columnsValue != 1) ||
        (horizontal && (columnsValue != 1 ||
            !sectionHeaderIndices.empty())))
    {
        return luaL_error(state,
            "view.virtualRange: provide exactly one extent; horizontal ranges require one list column and reject section headers");
    }
    ViewVirtualRange range;
    ViewVirtualRange visibleRange;
    if (variableExtent)
    {
        if (!d2d->engine->RuntimeComputeVariableVirtualRange(
                BoundWidgetId(state), key,
                static_cast<std::size_t>(itemCountValue),
                static_cast<float>(estimatedItemSizeValue),
                static_cast<float>(mainGapValue),
                static_cast<float>(viewportExtentValue),
                horizontal,
                static_cast<std::uint64_t>(layoutRevisionValue),
                static_cast<std::size_t>(overscanValue),
                static_cast<std::size_t>(initialScrollIndexValue),
                range, error))
            return luaL_error(state, "view.virtualRange: %s", error.c_str());
        if (!d2d->engine->RuntimeComputeVariableVirtualRange(
                BoundWidgetId(state), key,
                static_cast<std::size_t>(itemCountValue),
                static_cast<float>(estimatedItemSizeValue),
                static_cast<float>(mainGapValue),
                static_cast<float>(viewportExtentValue),
                horizontal,
                static_cast<std::uint64_t>(layoutRevisionValue), 0,
                static_cast<std::size_t>(initialScrollIndexValue),
                visibleRange, error))
            return luaL_error(state, "view.virtualRange: %s", error.c_str());
    }
    else
    {
        float requestedOffset = static_cast<float>(
            d2d->engine->RuntimeGetScrollOffset(BoundWidgetId(state), key));
        if (initialScrollIndexValue > 0 &&
            !d2d->engine->RuntimeHasScrollOffset(
                BoundWidgetId(state), key))
        {
            if (!ComputeViewVirtualItemScrollOffset(
                    static_cast<std::size_t>(itemCountValue),
                    static_cast<float>(itemExtentValue),
                    static_cast<std::size_t>(columnsValue),
                    static_cast<float>(mainGapValue),
                    static_cast<float>(viewportExtentValue), 0.0f,
                    static_cast<std::size_t>(initialScrollIndexValue),
                    "nearest", requestedOffset, error))
                return luaL_error(state,
                    "view.virtualRange: %s", error.c_str());
        }
        if (!ComputeViewVirtualRange(
            static_cast<std::size_t>(itemCountValue),
            static_cast<float>(itemExtentValue),
            static_cast<std::size_t>(columnsValue),
            static_cast<float>(mainGapValue),
            static_cast<float>(viewportExtentValue),
            requestedOffset,
            static_cast<std::size_t>(overscanValue), range, error))
            return luaL_error(state,
                "view.virtualRange: %s", error.c_str());
        if (!ComputeViewVirtualRange(
            static_cast<std::size_t>(itemCountValue),
            static_cast<float>(itemExtentValue),
            static_cast<std::size_t>(columnsValue),
            static_cast<float>(mainGapValue),
            static_cast<float>(viewportExtentValue), range.offset, 0,
            visibleRange, error))
            return luaL_error(state,
                "view.virtualRange: %s", error.c_str());
    }

    std::optional<std::size_t> stickyHeaderIndex;
    const auto nextHeader = std::upper_bound(sectionHeaderIndices.begin(),
        sectionHeaderIndices.end(), visibleRange.firstIndex);
    if (nextHeader != sectionHeaderIndices.begin())
        stickyHeaderIndex = *std::prev(nextHeader);

    lua_createtable(state, 0, 7);
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
    if (stickyHeaderIndex)
    {
        lua_pushinteger(state,
            static_cast<lua_Integer>(*stickyHeaderIndex));
        lua_setfield(state, -2, "stickyHeaderIndex");
    }
    return 1;
}

static std::string LuaViewScrollKey(lua_State* state, const char* api)
{
    std::size_t length = 0;
    const char* raw = luaL_checklstring(state, 1, &length);
    std::string key(raw ? raw : "", length);
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos || !IsValidUtf8Local(key))
    {
        luaL_error(state,
            "%s: key must contain 1 to 128 bytes of valid UTF-8", api);
    }
    return key;
}

static int PushViewScrollResult(lua_State* state, bool succeeded,
    int offset, int maximum, bool changed, const std::string& error)
{
    if (!succeeded)
    {
        lua_pushnil(state);
        lua_pushlstring(state, error.data(), error.size());
        return 2;
    }
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, offset);
    lua_setfield(state, -2, "offset");
    lua_pushinteger(state, maximum);
    lua_setfield(state, -2, "maximum");
    lua_pushboolean(state, changed ? 1 : 0);
    lua_setfield(state, -2, "changed");
    return 1;
}

static int LuaViewScroll(lua_State* state, bool relative)
{
    const char* api = relative ? "view.scrollBy" : "view.scrollTo";
    if (lua_gettop(state) != 2)
        return luaL_error(state, "%s: expected key and offset", api);
    const std::string key = LuaViewScrollKey(state, api);
    const lua_Integer requested = luaL_checkinteger(state, 2);
    if ((!relative && requested < 0) || requested < -1'000'000 ||
        requested > 1'000'000)
        return luaL_error(state, "%s: offset exceeds its limit", api);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "%s: host context is unavailable", api);
    if (!d2d->engine->RuntimeCanWriteWidgetStorage(BoundWidgetId(state)))
        return luaL_error(state, "%s cannot mutate scroll state during render", api);
    int offset = 0;
    int maximum = 0;
    bool changed = false;
    std::string error;
    const bool succeeded = d2d->engine->RuntimeScrollView(
        BoundWidgetId(state), key, static_cast<int>(requested), relative,
        offset, maximum, changed, error);
    return PushViewScrollResult(
        state, succeeded, offset, maximum, changed, error);
}

static int lua_ViewScrollTo(lua_State* state)
{
    return LuaViewScroll(state, false);
}

static int lua_ViewScrollBy(lua_State* state)
{
    return LuaViewScroll(state, true);
}

static int lua_ViewScrollToIndex(lua_State* state)
{
    if (lua_gettop(state) < 2 || lua_gettop(state) > 3)
        return luaL_error(state,
            "view.scrollToIndex: expected key, index, and optional alignment");
    const std::string key = LuaViewScrollKey(state, "view.scrollToIndex");
    const lua_Integer index = luaL_checkinteger(state, 2);
    if (index <= 0 || index > 1'000'000)
        return luaL_error(state,
            "view.scrollToIndex: index must be between 1 and 1000000");
    const std::string_view alignment = luaL_optstring(state, 3, "nearest");
    if (alignment != "nearest" && alignment != "start" &&
        alignment != "center" && alignment != "end")
        return luaL_error(state,
            "view.scrollToIndex: alignment must be nearest, start, center, or end");
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "view.scrollToIndex: host context is unavailable");
    if (!d2d->engine->RuntimeCanWriteWidgetStorage(BoundWidgetId(state)))
        return luaL_error(state,
            "view.scrollToIndex cannot mutate scroll state during render");
    int offset = 0;
    int maximum = 0;
    bool changed = false;
    std::string error;
    const bool succeeded = d2d->engine->RuntimeScrollViewToIndex(
        BoundWidgetId(state), key, static_cast<std::size_t>(index),
        alignment, offset, maximum, changed, error);
    return PushViewScrollResult(
        state, succeeded, offset, maximum, changed, error);
}

static snowdesktop::widget_runtime::LuaDrawImageResult ResolveDrawImageBitmap(
    lua_State* state, int index,
    float width, float height,
    snowdesktop::widget_runtime::DrawImageFit fit,
    D2D1_SIZE_F* originalSize, bool originalPixels)
{
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine) return {};

    const auto* handle = TestResourceHandle(state, index);
    if (!handle)
        return {nullptr, "API v2 requires an image handle"};
    if (handle->type != LuaResourceType::Image)
        return {nullptr, "invalid image resource handle"};

    ID2D1Bitmap1* bitmap = nullptr;
    if (snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(handle->name))
        bitmap = LoadRuntimeImageBitmap(
            d2d, BoundWidgetId(state), handle->name);
    else if (const auto contentKey = ResolveResourceHandleImageContentKey(state, index))
    {
        snowdesktop::widget_runtime::PackageImageSize original;
        bitmap = LoadImageBitmap(d2d, *contentKey, width, height, fit,
            originalPixels, &original);
        if (originalSize)
            *originalSize = D2D1::SizeF(static_cast<float>(original.width),
                static_cast<float>(original.height));
        if (!bitmap && d2d->packageImageCache.Find(*contentKey))
            return {};
    }
    if (!bitmap)
        return {nullptr, "invalid image resource handle"};
    if (originalSize && originalSize->width <= 0)
        *originalSize = bitmap->GetSize();
    return {bitmap, nullptr};
}

static const snowdesktop::widget_runtime::LuaDrawHostServices kLuaDrawHostServices{
    [](lua_State* state, int index) {
        return ResolveResourceHandlePath(state, index, LuaResourceType::Font);
    },
    ResolveDrawImageBitmap,
    [](lua_State* state, snowdesktop::widget_runtime::NativeMarqueeText&& marquee) {
        const auto* d2d = GetD2D(state);
        std::string error;
        return d2d && d2d->engine && d2d->engine->RuntimeSubmitNativeMarquee(
            BoundWidgetId(state), std::move(marquee), error);
    },
};

static int lua_WidgetOpenDialog(lua_State* L)
{
    std::string title;
    int width = 520;
    int height = 420;
    bool dismissOnOutside = false;
    bool dismissOnEscape = true;
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
        lua_getfield(L, 1, "dismissOnOutside");
        if (!lua_isnil(L, -1))
            dismissOnOutside = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 1, "dismissOnEscape");
        if (!lua_isnil(L, -1))
            dismissOnEscape = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetDialog(
            BoundWidgetId(L), Utf8ToWideLocal(title),
            width, height, dismissOnOutside,
            dismissOnEscape);
    return 0;
}

static int lua_WidgetOpenPopover(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const std::string title =
        LuaReadStorageField(L, 1, "title");
    const std::string anchorKey =
        LuaReadStorageField(L, 1, "anchorKey");
    std::string placement =
        LuaReadStorageField(L, 1, "placement");
    if (placement.empty()) placement = "auto";
    int width = 360;
    int height = 280;
    bool dismissOnOutside = true;
    bool dismissOnEscape = true;
    lua_getfield(L, 1, "width");
    if (lua_isnumber(L, -1))
        width = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "height");
    if (lua_isnumber(L, -1))
        height = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "dismissOnOutside");
    if (!lua_isnil(L, -1))
        dismissOnOutside = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "dismissOnEscape");
    if (!lua_isnil(L, -1))
        dismissOnEscape = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    auto* s = GetD2D(L);
    const bool opened = s && s->engine &&
        s->engine->RuntimeOpenWidgetPopover(
            BoundWidgetId(L), Utf8ToWideLocal(title),
            anchorKey, std::move(placement), width, height,
            dismissOnOutside, dismissOnEscape);
    lua_pushboolean(L, opened ? 1 : 0);
    return 1;
}

static const snowdesktop::widget_runtime::LuaResourceHost kLuaResourceHost{
    [](lua_State* state) -> snowdesktop::widget_runtime::WidgetPackageImageCache* {
        auto* d2d = GetD2D(state);
        return d2d ? &d2d->packageImageCache : nullptr;
    },
    [](lua_State* state, const std::string& key) {
        auto* d2d = GetD2D(state);
        return d2d && (!d2d->ctx || LoadImageBitmap(d2d, key));
    },
    snowdesktop::widget_runtime::HashWidgetFile,
    WidgetPrewarmContentDigest,
    [](lua_State* state) {
        const auto* d2d = GetD2D(state);
        return d2d && d2d->dwrite;
    },
    [](lua_State* state, const std::wstring& path) {
        return GetD2D(state)->PreloadPrivateFont(path);
    },
    [](lua_State* state, const std::wstring& path) {
        return GetD2D(state)->HasPrivateFont(path);
    },
    [](lua_State* state, const char* token) {
        const auto* d2d = GetD2D(state);
        const auto entry = d2d->runtimeImages.find(token);
        return entry != d2d->runtimeImages.end() &&
            entry->second.ownerWidgetId == BoundWidgetId(state);
    },
};

static int lua_ResourceImage(lua_State* state)
{
    return snowdesktop::widget_runtime::LoadLuaPackageImage(state, kLuaResourceHost);
}

static int lua_ResourceFont(lua_State* state)
{
    return snowdesktop::widget_runtime::LoadLuaPackageFont(state, kLuaResourceHost);
}

static int lua_ResourceStatus(lua_State* state)
{
    return snowdesktop::widget_runtime::PushLuaResourceStatus(state, kLuaResourceHost);
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
            ClearShellIconBitmaps(state);
        if (const auto old = state->shellIconCache.find(result.path);
            old != state->shellIconCache.end())
            state->imageSamplingCache.Remove(old->second.Get());
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

static int lua_DrawIcon(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    auto* s = GetD2D(L);
    std::wstring path;
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
WidgetEngine::WidgetEngine() = default;

WidgetEngine::~WidgetEngine()
{
    Shutdown();
}

void WidgetEngine::SetSystemDataProvider(std::shared_ptr<
    snowdesktop::widget_runtime::WidgetSystemDataProvider> provider)
{
    if (!dataBroker_ && !previewOnly_)
        widgetSystemDataProvider_ = std::move(provider);
}

void WidgetEngine::SetAudioAnalysisWakeCallback(
    AudioAnalysisWakeCallback callback)
{
    audioAnalysisWakeCallback_ = std::move(callback);
    if (widgetAudioAnalysisProvider_)
    {
        widgetAudioAnalysisProvider_->SetChangedCallback(
            audioAnalysisWakeCallback_);
    }
}

void WidgetEngine::InitializeWidgetDataBroker()
{
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
    for (const auto& contract :
        snowdesktop::widget_api::SystemDataTopicContracts())
    {
        (void)dataBroker_->RegisterProvider(DataProviderDescriptor{
            contract.name,
            contract.requiredPermission ? contract.requiredPermission : "",
            std::chrono::milliseconds(contract.minimumIntervalMs),
            std::chrono::milliseconds(contract.hiddenIntervalMs),
            std::chrono::milliseconds(contract.idleGraceMs),
            contract.highRisk,
            contract.supportsHiddenContinue }, error);
        error.clear();
    }

    if (previewOnly_)
    {
        widgetSystemDataProvider_.reset();
        widgetAudioAnalysisProvider_.reset();
    }
    else
    {
        if (!widgetSystemDataProvider_)
            widgetSystemDataProvider_ = std::make_shared<
                snowdesktop::widget_runtime::WidgetSystemDataProvider>();
        widgetAudioAnalysisProvider_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAudioAnalysisProvider>();
        widgetAudioAnalysisProvider_->SetChangedCallback(
            audioAnalysisWakeCallback_);
    }
}

void WidgetEngine::ApplyWidgetDataBrokerActions()
{
    using snowdesktop::widget_runtime::DataBrokerActionType;
    if (!dataBroker_) return;
    const auto audioConfiguration = [this] {
        snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration
            configuration{ false, false, false, false, 16, 16 };
        for (const auto& subscription :
            dataBroker_->SubscriptionSnapshots("audio.output.analysis"))
        {
            if (!subscription.eligible) continue;
            configuration.waveform = configuration.waveform ||
                subscription.options.audioWaveform;
            configuration.spectrum = configuration.spectrum ||
                subscription.options.audioSpectrum;
            configuration.rms = configuration.rms ||
                subscription.options.audioRms;
            configuration.peak = configuration.peak ||
                subscription.options.audioPeak;
            if (subscription.options.audioWaveform)
            {
                configuration.waveformPoints = std::max(
                    configuration.waveformPoints,
                    subscription.options.audioWaveformPoints);
            }
            if (subscription.options.audioSpectrum)
            {
                configuration.spectrumBins = std::max(
                    configuration.spectrumBins,
                    subscription.options.audioSpectrumBins);
            }
        }
        return configuration;
    };
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
                        action.effectiveInterval,
                        audioConfiguration())
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
                        action.effectiveInterval,
                        audioConfiguration());
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

void WidgetEngine::DrainAudioAnalysisChanges()
{
    snowdesktop::performance::Scope performanceScope("shared.audio", "deliver");
    if (!widgetAudioAnalysisProvider_ ||
        !widgetAudioAnalysisProvider_->DrainChanged())
    {
        return;
    }
    if (!dataBroker_) return;
    const auto now = snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    for (auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview) continue;
        bool due = false;
        for (const auto& [id, topic] : widget.dataSubscriptions)
        {
            if (topic == "audio.output.analysis" &&
                dataBroker_->ConsumeUpdateDue(id, now))
                due = true;
        }
        if (!due || (widget.desktopOccluded && !widget.panelActive)) continue;
        snowdesktop::performance::DrawLink("desktop", widget.widgetId, false);
        // Audio is asynchronous data, so share an existing animation deadline
        // or request one host frame. Pointer invalidation remains synchronous.
        if (widget.animationFrames.RequestDataRefresh() &&
            ScheduleAnimationFrame(widget))
        {
            snowdesktop::performance::Value("shared.audio", "refresh.queued",
                widget.widgetId, 1);
            continue;
        }
        (void)widget.animationFrames.ConsumeDataRefresh();
        RuntimeInvalidateHost(widget.widgetId);
    }
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

            if (event.kind != "removed" && completion.origin && filesystemHandleStore_)
            {
                auto grant = completion.origin->GrantChild(*filesystemHandleStore_,
                    watch.owner, event.name, watch.access);
                if (grant)
                {
                    publicEvent.itemKind = std::string(
                        snowdesktop::widget_runtime::
                            WidgetFilesystemHandleStore::KindName(grant.entry->kind));
                    publicEvent.handle = grant.entry->handle;
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

void WidgetEngine::BeginTaskShutdown()
{
    StopWidgetMultiplayer("canceled");
    audioPlayback_.reset();
    if (taskBroker_) taskBroker_->Shutdown();
    nativeTaskExecutor_.reset();
    systemControlTasks_.reset();
}

void WidgetEngine::InitializeWidgetTaskBroker()
{
    audioPlayback_.reset();
    systemControlTasks_.reset();
    using snowdesktop::widget_runtime::TaskDescriptor;
    taskBroker_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetTaskBroker>();
    notificationCenter_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetNotificationCenter>();
    std::string error;
    for (const auto& contract :
        snowdesktop::widget_api::SystemTaskContracts())
    {
        (void)taskBroker_->RegisterTask(TaskDescriptor{
            contract.name,
            contract.requiredPermission ? contract.requiredPermission : "",
            contract.requiresTrustedGesture,
            contract.maximumPerInstance }, error);
        error.clear();
    }
    if (previewOnly_)
    {
        nativeTaskExecutor_.reset();
        locationTaskExecutor_.reset();
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
        audioPlayback_ = std::make_unique<snowdesktop::widget_runtime::WidgetAudioPlayback>();
        audioPlayback_->SetWakeCallback(taskWakeCallback_);
        nativeTaskExecutor_ = std::make_unique<snowdesktop::widget_runtime::WidgetNativeTaskExecutor>();
        nativeTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
        locationTaskExecutor_ = std::make_unique<snowdesktop::widget_runtime::WidgetLocationTaskExecutor>();
        locationTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
        mediaTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetMediaTaskExecutor>();
        audioOutputTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAudioOutputTaskExecutor>();
        clipboardTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetClipboardTaskExecutor>();
        filesystemTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor>();
        filesystemTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
        appTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAppTaskExecutor>();
        desktopTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetAppTaskExecutor>();
        externalItemTaskExecutor_ = std::make_unique<
            snowdesktop::widget_runtime::WidgetExternalSearchTaskExecutor>();
    }
}

void WidgetEngine::LoadNotificationSchedules()
{
    notificationScheduleStore_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetNotificationScheduleStore>();
    notificationSchedulePath_ = GetDataFilePath(
        L"SnowDesktop.widget-notifications.json");
    const auto rejectLoad = [this](const std::string& error) {
        notificationScheduleStore_->MarkLoadFailed();
        OutputDebugStringA(("SnowDesktop: notification schedule load "
            "failed: " + error + "\n").c_str());
    };
    const DWORD attributes = GetFileAttributesW(notificationSchedulePath_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            rejectLoad("cannot inspect file: " + std::to_string(error));
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        rejectLoad("file is a reparse point");
        return;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(
            notificationSchedulePath_, fileError))
    {
        rejectLoad(fileError ? fileError.message() : "not a regular file");
        return;
    }
    const auto fileBytes = std::filesystem::file_size(
        notificationSchedulePath_, fileError);
    if (fileError || fileBytes > snowdesktop::widget_runtime::
            WidgetNotificationScheduleStore::MaximumFileBytes)
    {
        rejectLoad(fileError ? fileError.message() : "file exceeds size limit");
        return;
    }
    std::string text;
    std::string error;
    if (!snowdesktop::atomic_file::ReadAll(
            notificationSchedulePath_, text, &error) ||
        !notificationScheduleStore_->LoadText(text, error))
    {
        rejectLoad(error);
        return;
    }
    const std::int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            snowdesktop::widget_runtime::WidgetNotificationCenter::
                Clock::now().time_since_epoch()).count();
    constexpr std::int64_t catchUpWindowMs =
        24LL * 60 * 60 * 1000;
    bool pruned = false;
    const auto loadedEntries = notificationScheduleStore_->Entries();
    for (const auto& entry : loadedEntries)
    {
        if (entry.dueMs < nowMs - catchUpWindowMs)
        {
            pruned = notificationScheduleStore_->Remove(
                entry.instanceId, entry.notificationId) || pruned;
        }
    }
    if (pruned) (void)SaveNotificationSchedules();
}

bool WidgetEngine::SaveNotificationSchedules()
{
    if (!notificationScheduleStore_ ||
        !notificationScheduleStore_->IsWritable() ||
        notificationSchedulePath_.empty())
        return false;
    std::string error;
    const bool saved = snowdesktop::atomic_file::WriteAll(
        notificationSchedulePath_,
        notificationScheduleStore_->Serialize(),
        notificationSchedulePath_.wstring() + L".bak", &error);
    if (!saved)
    {
        OutputDebugStringA(("SnowDesktop: notification schedule save "
            "failed: " + error + "\n").c_str());
    }
    return saved;
}

void WidgetEngine::RestoreNotificationSchedules(LuaWidget& widget)
{
    if (!notificationCenter_ || !notificationScheduleStore_ ||
        !notificationScheduleStore_->IsWritable() || widget.preview ||
        snowdesktop::widget_runtime::IsDryLoad())
        return;
    const std::string instanceId = WidgetWideToUtf8(widget.widgetId);
    if (!snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
            widget.permissions, kNotificationPostPermission))
    {
        if (notificationScheduleStore_->RemoveInstance(instanceId) > 0)
            (void)SaveNotificationSchedules();
        return;
    }
    const bool ownerMismatch = std::any_of(
        notificationScheduleStore_->Entries().begin(),
        notificationScheduleStore_->Entries().end(),
        [&instanceId, &widget](const auto& entry) {
            return entry.instanceId == instanceId &&
                entry.packageId != widget.packageId;
        });
    if (ownerMismatch)
    {
        if (notificationScheduleStore_->RemoveInstance(instanceId) > 0)
            (void)SaveNotificationSchedules();
        return;
    }

    bool changed = false;
    const auto now = snowdesktop::widget_runtime::
        WidgetNotificationCenter::Clock::now();
    for (const auto& entry : notificationScheduleStore_->ForInstance(
            instanceId, widget.packageId))
    {
        snowdesktop::widget_runtime::WidgetNotificationContent content;
        content.title = Utf8ToWideLocal(entry.title);
        content.message = Utf8ToWideLocal(entry.message);
        if (!entry.imageResource.empty())
        {
            const auto imagePath = RuntimeResolvePackageResource(
                widget.widgetId, entry.imageResource, "image");
            if (!imagePath)
            {
                changed = notificationScheduleStore_->Remove(
                    instanceId, entry.notificationId) || changed;
                continue;
            }
            content.imagePath = *imagePath;
        }
        content.progress = entry.progress;
        for (const auto& action : entry.actions)
        {
            content.actions.push_back(
                { action.id, Utf8ToWideLocal(action.label) });
        }
        const auto due = snowdesktop::widget_runtime::
            WidgetNotificationCenter::Clock::time_point(
                std::chrono::milliseconds(entry.dueMs));
        const auto restored = notificationCenter_->RestoreScheduled(
            widget.runtimeToken, entry.notificationId,
            std::move(content), due, now);
        if (!restored.ok)
        {
            changed = notificationScheduleStore_->Remove(
                instanceId, entry.notificationId) || changed;
        }
    }
    if (changed) (void)SaveNotificationSchedules();
}

void WidgetEngine::RemoveNotificationSchedules(
    const std::wstring& widgetId)
{
    if (!notificationScheduleStore_) return;
    if (notificationScheduleStore_->RemoveInstance(
            WidgetWideToUtf8(widgetId)) > 0)
        (void)SaveNotificationSchedules();
}

void WidgetEngine::ApplyWidgetTaskBrokerActions()
{
    using snowdesktop::widget_runtime::TaskBrokerActionType;
    if (!taskBroker_) return;
    // File pickers can run a nested Windows message loop. Keep wake messages
    // from reentering the broker while its dispatch/completion data is in use.
    if (applyingTaskBrokerActions_)
    {
        taskWakePending_ = true;
        return;
    }
    applyingTaskBrokerActions_ = true;
    struct DispatchScope
    {
        bool& active;
        bool& pending;
        TaskWakeCallback& wake;
        ~DispatchScope()
        {
            active = false;
            if (std::exchange(pending, false) && wake) wake();
        }
    } dispatchScope{ applyingTaskBrokerActions_, taskWakePending_, taskWakeCallback_ };
    PumpWidgetMultiplayer();
    const auto audioEvents =
        snowdesktop::widget_runtime::ConsumeAudioPlaybackMessages(
            audioPlayback_.get(), *taskBroker_);
    if (nativeTaskExecutor_)
        nativeTaskDispatch_.Consume(nativeTaskExecutor_->DrainMessages(), *taskBroker_,
            [this](const snowdesktop::widget_runtime::NativeTaskEvent& event) {
                auto owner = std::find_if(widgets_.begin(), widgets_.end(),
                    [&](const auto& widget) { return widget.runtimeToken == event.ownerToken; });
                if (owner == widgets_.end() ||
                    !owner->permissions.contains("native.execute")) return;
                JsonValue frame;
                const auto* value = event.ReadValue(frame);
                if (!value) return;
                snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                    trustedGestureState_, false);
                (void)InvokeLifecycleEvent(*owner, "native.event", [&](lua_State* state) {
                    lua_pushinteger(state, static_cast<lua_Integer>(event.taskId));
                    lua_setfield(state, -2, "taskId");
                    PushSystemControlJson(state, *value);
                    lua_setfield(state, -2, "value");
                });
            });
    if (locationTaskExecutor_)
        for (auto& completion : locationTaskExecutor_->DrainCompletions())
        {
            const auto snapshot = taskBroker_->Snapshot(completion.id);
            if (!snapshot || snapshot->cancelRequested) continue;
            const auto id = completion.id;
            const auto ok = completion.ok;
            const auto error = completion.error;
            if (ok) locationTaskCompletions_.insert_or_assign(id, std::move(completion));
            (void)taskBroker_->Complete(id, ok, error);
        }
    if (systemControlTasks_)
        for (auto& completion : systemControlTasks_->Drain())
            (void)taskBroker_->Complete(completion.id, completion.ok, std::move(completion.error));
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
        clipboardTaskDispatch_.Consume(clipboardTaskExecutor_->DrainCompletions(), *taskBroker_);
    if (filesystemTaskExecutor_)
        filesystemTaskDispatch_.Consume(filesystemTaskExecutor_->DrainCompletions(),
            *taskBroker_, filesystemHandleStore_.get(),
            [this](std::uint64_t ownerToken)
                -> std::optional<snowdesktop::widget_runtime::FilesystemCompletionOwner> {
                const auto owner = std::find_if(widgets_.begin(), widgets_.end(),
                    [ownerToken](const LuaWidget& widget) {
                        return widget.runtimeToken == ownerToken;
                    });
                if (owner == widgets_.end()) return std::nullopt;
                return snowdesktop::widget_runtime::FilesystemCompletionOwner{
                    owner->packageId,
                    snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
                        owner->permissions, kFilesystemReadPermission)};
            });
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
        snowdesktop::performance::Scope performanceScope(
            "task.dispatch", action.name,
            snowdesktop::performance::Enabled()
                ? Utf8ToWideLocal(action.instanceId) : std::wstring{}, action.id);
        if (action.type == TaskBrokerActionType::Cancel)
        {
            if (action.name.starts_with("multiplayer."))
            {
                CancelWidgetMultiplayerSend(action.id);
                (void)taskBroker_->Complete(action.id, false);
                continue;
            }
            if (audioPlayback_) audioPlayback_->Cancel(action.id);
            CancelDispatchedWidgetTask(action.id, action.name == "location.current");
            networkTaskDispatch_.Cancel(action, httpService_.get());
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

        if (action.name.starts_with("multiplayer."))
        {
            StartWidgetMultiplayerTask(action, *owner);
            continue;
        }
        if (snowdesktop::widget_runtime::WidgetAudioPlayback::SupportsAction(action.name))
        {
            using namespace snowdesktop::widget_runtime;
            AudioPlaybackRequest request;
            request.taskId = action.id; request.owner = action.ownerToken; request.action = action.name;
            const auto get = [&](const char* key) -> std::string {
                const auto value = action.arguments.find(key); return value == action.arguments.end() ? std::string{} : value->second;
            };
            request.playbackId = get("playbackId");
            if (const auto value = get("volume"); !value.empty())
                (void)std::from_chars(value.data(), value.data() + value.size(), request.volume);
            if (const auto value = get("positionMs"); !value.empty())
                (void)std::from_chars(value.data(), value.data() + value.size(), request.positionMs);
            request.loop = get("loop") == "1";
            request.readMetadata = get("readMetadata") == "1";
            request.systemMediaControls = get("systemMediaControls") == "1";
            request.shuffle = get("shuffle") == "1";
            if (const auto value = get("repeatMode"); !value.empty()) request.repeatMode = value;
            if (const auto value = get("playbackRate"); !value.empty())
                (void)std::from_chars(value.data(), value.data() + value.size(), request.playbackRate);
            std::string error;
            if (!owner->permissions.contains("audio.playback")) error = "permissionRevoked";
            else if (action.name == "audio.play")
            {
                if (const auto resource = get("resource"); !resource.empty())
                {
                    const auto found = owner->manifest.resources.find(resource);
                    if (found == owner->manifest.resources.end() || found->second.type != "audio") error = "invalidReference";
                    else if (const auto path = ResolvePackageAssetWithinRoot(owner->packageRoot, Utf8ToWideLocal(found->second.path)))
                        request.path = *path;
                    else error = "invalidReference";
                }
                else
                {
                    request.sourceHandle = get("handle");
                    const auto entry = filesystemHandleStore_ ? filesystemHandleStore_->Resolve(
                        {WidgetWideToUtf8(owner->widgetId), owner->packageId}, request.sourceHandle) : std::nullopt;
                    const auto name = get("name");
                    if (!owner->permissions.contains("filesystem.userSelected.read")) error = "permissionRevoked";
                    else if (!entry || entry->access == WidgetFilesystemHandleAccess::Write) error = "invalidReference";
                    else if (name.empty() ? entry->kind != WidgetFilesystemHandleKind::File :
                        entry->kind != WidgetFilesystemHandleKind::Folder || !WidgetFilesystemTaskExecutor::IsDirectChildName(name)) error = "invalidArguments";
                    else request.path = name.empty() ? entry->path : entry->path / Utf8ToWideLocal(name);
                }
            }
            if (action.preview) { (void)taskBroker_->Complete(action.id, true); continue; }
            if (!error.empty()) { (void)taskBroker_->Complete(action.id, false, error); continue; }
            if (!audioPlayback_ || !audioPlayback_->Submit(std::move(request)))
                (void)taskBroker_->Complete(action.id, false, "audioPlaybackUnavailable");
            continue;
        }
        if (action.name == "native.request" || action.name == "native.watch")
        {
            snowdesktop::widget_runtime::NativeRequestOptions options;
            const auto package = GetWidgetPackage(Utf8ToWideLocal(owner->packageId));
            if (!snowdesktop::widget_runtime::ParseNativeRequestOptions(action.arguments, options))
                (void)taskBroker_->Complete(action.id, false, "invalidArguments");
            else if (action.preview)
                (void)taskBroker_->Complete(action.id, false, "previewUnavailable");
            else if (!package || package->permissionState != snowdesktop::widget::PermissionDecisionState::Granted ||
                !owner->permissions.contains("native.execute") ||
                !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(package->grantedPermissions, "native.execute"))
                (void)taskBroker_->Complete(action.id, false, "permissionRevoked");
            else
            {
                options.streaming = action.name == "native.watch";
                if (!nativeTaskExecutor_ || !nativeTaskExecutor_->Start(action.id, package->root,
                        snowdesktop::widget::PackagePermissionScopeFingerprint(package->manifest), std::move(options)))
                    (void)taskBroker_->Complete(action.id, false, "taskExecutorUnavailable");
            }
            continue;
        }
        if (action.name == "location.current")
        {
            snowdesktop::widget_runtime::LocationOptions options;
            if (!snowdesktop::widget_runtime::ParseLocationOptions(action.arguments, options))
                (void)taskBroker_->Complete(action.id, false, "invalidArguments");
            else if (action.preview)
                (void)taskBroker_->Complete(action.id, false, "previewUnavailable");
            else if (!snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(owner->permissions, "location.read"))
                (void)taskBroker_->Complete(action.id, false, "permissionRevoked");
            else if (!locationTaskExecutor_ || !locationTaskExecutor_->Start(action.id, options, false))
                (void)taskBroker_->Complete(action.id, false, "taskExecutorUnavailable");
            continue;
        }
        if (snowdesktop::widget_runtime::IsSystemControlTask(action.name))
        {
            snowdesktop::system_control::Request request;
            if (!snowdesktop::widget_runtime::MakeSystemControlRequest(action.name, action.arguments, request))
            {
                (void)taskBroker_->Complete(action.id, false, "invalidArguments"); continue;
            }
            const auto dispatched = snowdesktop::widget_runtime::DispatchSystemControlTask(request, action.preview,
                [&](snowdesktop::system_control::Request& liveRequest) -> snowdesktop::system_control::Result {
                    const auto authorized = [this, id=action.id, token=action.ownerToken, name=action.name]() {
                        if (!taskBroker_) return false;
                        const auto task = taskBroker_->Snapshot(id);
                        const auto permission = taskBroker_->RequiredPermission(name);
                        const auto live = std::find_if(widgets_.begin(), widgets_.end(), [token](const auto& candidate) { return candidate.runtimeToken == token; });
                        return task && !task->cancelRequested && task->ownerToken == token && live != widgets_.end() &&
                            permission && snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(live->permissions, *permission);
                    };
                    if (!authorized())
                        return {false, "permissionRevoked", 0};
                    if (snowdesktop::system_control::RequiresConfirmation(liveRequest.name) || snowdesktop::system_control::RequiresPasswordPrompt(liveRequest))
                    {
                        if (!systemControlPromptCallback_)
                            return {false, "confirmationUnavailable", 0};
                        // Copy identity before the modal message loop can unload/reload its owner.
                        const auto identity = Utf8ToWideLocal(owner->packageId) + L"\n" + owner->widgetId;
                        snowdesktop::system_control::Result confirmed;
                        try { confirmed = systemControlPromptCallback_(liveRequest, identity, authorized); }
                        catch (...) { confirmed.error = "confirmationUnavailable"; }
                        if (!confirmed.ok || !authorized())
                            return {false, confirmed.error.empty() ? "canceled" : confirmed.error, 0};
                    }
                    if (!widgetSystemDataProvider_)
                        return {false, "providerUnavailable", 0};
                    if (!systemControlTasks_) systemControlTasks_ = std::make_unique<snowdesktop::widget_runtime::WidgetSystemControlTasks>(widgetSystemDataProvider_->Controls());
                    if (!systemControlTasks_->Start(action.id, action.ownerToken, std::move(liveRequest)))
                        return {false, "taskExecutorUnavailable", 0};
                    return {true, {}, 0};
            });
            if (!dispatched.ok || action.preview)
                (void)taskBroker_->Complete(action.id, dispatched.ok, dispatched.error);
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
                std::vector<LuaDesktopItemInfo> desktopItems;
                try
                {
                    desktopItems = RuntimeDesktopItems();
                }
                catch (...)
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "providerFailed");
                    continue;
                }
                if (desktopItems.size() > 2048) desktopItems.resize(2048);
                std::vector<snowdesktop::widget_runtime::
                    WidgetAppCatalogEntry> catalog;
                catalog.reserve(desktopItems.size());
                for (const auto& item : desktopItems)
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
            networkTaskDispatch_.Start(action, owner->widgetId,
                owner->packageId, owner->manifest.networkDomains,
                secretStore_.get(), httpService_.get(), *taskBroker_);
            continue;
        }

        if (action.name == "calendar.create" ||
            action.name == "calendar.update" ||
            action.name == "calendar.remove" ||
            action.name == "calendar.series.create" ||
            action.name == "calendar.series.update" ||
            action.name == "calendar.series.remove")
        {
            snowdesktop::calendar::MutationResult result;
            const bool seriesTask = action.name.starts_with("calendar.series.");
            const auto parseInteger = [](const std::string& text,
                int& value) {
                const char* begin = text.data();
                const char* finish = begin + text.size();
                const auto parsed = std::from_chars(begin, finish, value);
                return parsed.ec == std::errc{} && parsed.ptr == finish;
            };
            if (action.preview)
            {
                result.error = "previewReadOnly";
            }
            else if (action.name == "calendar.remove" ||
                action.name == "calendar.series.remove")
            {
                const auto id = action.arguments.find("id");
                if (id == action.arguments.end()) result.error = "invalidArguments";
                else if (seriesTask)
                {
                    const auto revision = action.arguments.find("expectedRevision");
                    int expectedRevision = 0;
                    result = revision != action.arguments.end() &&
                        parseInteger(revision->second, expectedRevision)
                        ? RuntimeCalendarSeriesRemove(id->second, expectedRevision)
                        : snowdesktop::calendar::MutationResult{false, id->second, 0, "invalidArguments"};
                }
                else result = RuntimeCalendarRemove(id->second);
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
                else if (seriesTask)
                {
                    snowdesktop::calendar::CalendarSeries series;
                    series.event = std::move(event);
                    const auto get = [&](const char* key) -> const std::string* {
                        const auto found = action.arguments.find(key);
                        return found == action.arguments.end() ? nullptr : &found->second;
                    };
                    const auto* kind = get("kind");
                    const auto* startDate = get("startDate");
                    const auto* endDate = get("endDate");
                    const auto* interval = get("interval");
                    const auto* monthDay = get("monthDay");
                    const auto* dates = get("dates");
                    const auto* weekdays = get("weekdays");
                    valid = kind && startDate && endDate && interval &&
                        monthDay && dates && weekdays &&
                        parseInteger(*interval, series.rule.interval) &&
                        parseInteger(*monthDay, series.rule.monthDay);
                    if (valid)
                    {
                        series.rule.kind = *kind;
                        series.rule.startDate = *startDate;
                        series.rule.endDate = *endDate;
                        const auto decode = [&](const std::string& encoded,
                            bool dateValues) {
                            std::size_t offset = 0;
                            while (offset < encoded.size())
                            {
                                const auto separator = encoded.find(',', offset);
                                const auto value = encoded.substr(offset,
                                    separator == std::string::npos ? std::string::npos : separator - offset);
                                if (dateValues) series.rule.dates.push_back(value);
                                else
                                {
                                    int weekday = 0;
                                    if (!parseInteger(value, weekday)) return false;
                                    series.rule.weekdays.push_back(weekday);
                                }
                                if (separator == std::string::npos) break;
                                offset = separator + 1;
                            }
                            return true;
                        };
                        valid = decode(*dates, true) && decode(*weekdays, false);
                    }
                    if (!valid) result.error = "invalidArguments";
                    else if (action.name == "calendar.series.create")
                        result = RuntimeCalendarSeriesCreate(std::move(series));
                    else
                    {
                        const auto* id = get("id");
                        const auto* revision = get("expectedRevision");
                        int expectedRevision = 0;
                        result = id && revision &&
                            parseInteger(*revision, expectedRevision)
                            ? RuntimeCalendarSeriesUpdate(*id, expectedRevision, std::move(series))
                            : snowdesktop::calendar::MutationResult{false, {}, 0, "invalidArguments"};
                    }
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

        const bool notificationTask =
            action.name == "notification.show" ||
            action.name == "notification.update" ||
            action.name == "notification.dismiss" ||
            action.name == "notification.schedule" ||
            action.name == "notification.cancel";
        if (notificationTask)
        {
            const std::string notificationInstanceId =
                WidgetWideToUtf8(owner->widgetId);
            if (action.preview)
            {
                if (action.name == "notification.show" ||
                    action.name == "notification.schedule")
                {
                    notificationTaskCompletions_.insert_or_assign(
                        action.id, "notification:preview:" +
                            std::to_string(action.id));
                }
                (void)taskBroker_->Complete(action.id, true);
                continue;
            }
            if (!notificationCenter_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "providerUnavailable");
                continue;
            }

            using Result = snowdesktop::widget_runtime::
                WidgetNotificationOperationResult;
            Result result;
            const auto host = [this](const snowdesktop::widget_runtime::
                WidgetNotificationHostRequest& request) {
                return notifyCallback_ && notifyCallback_(request);
            };
            const auto parseProgress = [](const std::string& value,
                double& output) {
                if (value.empty()) return false;
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), output,
                    std::chars_format::general);
                return parsed.ec == std::errc{} &&
                    parsed.ptr == value.data() + value.size() &&
                    std::isfinite(output) && output >= 0.0 && output <= 1.0;
            };
            const auto readActions = [&action](
                std::vector<snowdesktop::widget_runtime::
                    WidgetNotificationAction>& actions) {
                const auto count = action.arguments.find("actionCount");
                if (count == action.arguments.end()) return true;
                int actionCount = -1;
                const auto parsed = std::from_chars(
                    count->second.data(),
                    count->second.data() + count->second.size(),
                    actionCount);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != count->second.data() +
                        count->second.size() ||
                    actionCount < 0 || actionCount > 2)
                    return false;
                actions.clear();
                actions.reserve(static_cast<std::size_t>(actionCount));
                for (int index = 0; index < actionCount; ++index)
                {
                    const auto id = action.arguments.find(
                        "action" + std::to_string(index) + "Id");
                    const auto label = action.arguments.find(
                        "action" + std::to_string(index) + "Label");
                    if (id == action.arguments.end() ||
                        label == action.arguments.end())
                        return false;
                    actions.push_back({
                        id->second, Utf8ToWideLocal(label->second) });
                }
                return true;
            };
            const auto applyStructuredContent =
                [this, owner, &action, &parseProgress, &readActions](
                    snowdesktop::widget_runtime::
                        WidgetNotificationContent& content) {
                    const auto image =
                        action.arguments.find("imageResource");
                    if (image != action.arguments.end() &&
                        !image->second.empty())
                    {
                        const auto path = RuntimeResolvePackageResource(
                            owner->widgetId, image->second, "image");
                        if (!path) return false;
                        content.imagePath = *path;
                    }
                    const auto progress =
                        action.arguments.find("progress");
                    if (progress != action.arguments.end())
                    {
                        double value = 0.0;
                        if (!parseProgress(progress->second, value))
                            return false;
                        content.progress = value;
                    }
                    return readActions(content.actions);
                };
            if (action.name == "notification.show")
            {
                const auto title = action.arguments.find("title");
                const auto message = action.arguments.find("message");
                if (title == action.arguments.end() ||
                    message == action.arguments.end())
                {
                    result.error = "invalidArguments";
                }
                else
                {
                    snowdesktop::widget_runtime::
                        WidgetNotificationContent content;
                    content.title = Utf8ToWideLocal(title->second);
                    content.message = Utf8ToWideLocal(message->second);
                    if (!applyStructuredContent(content))
                    {
                        result.error = "invalidArguments";
                    }
                    std::string admissionError;
                    if (result.error.empty())
                    {
                        result = notificationCenter_->Show(
                            owner->runtimeToken, std::move(content),
                            snowdesktop::widget_runtime::
                                WidgetNotificationCenter::Clock::now(),
                            [this, owner, &admissionError, &host](
                                const snowdesktop::widget_runtime::
                                    WidgetNotificationHostRequest& request) {
                                admissionError =
                                    RuntimeAdmitNotification(owner->widgetId);
                                return admissionError.empty() && host(request);
                            });
                    }
                    if (!admissionError.empty())
                        result.error = std::move(admissionError);
                }
            }
            else if (action.name == "notification.schedule")
            {
                const auto title = action.arguments.find("title");
                const auto message = action.arguments.find("message");
                const auto at = action.arguments.find("atMs");
                std::int64_t atMs = 0;
                const auto parsed = at == action.arguments.end()
                    ? std::from_chars_result{}
                    : std::from_chars(at->second.data(),
                        at->second.data() + at->second.size(), atMs);
                const std::int64_t nowMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        snowdesktop::widget_runtime::
                            WidgetNotificationCenter::Clock::now().
                                time_since_epoch()).count();
                constexpr std::int64_t maximumDelayMs =
                    366LL * 24 * 60 * 60 * 1000;
                if (title == action.arguments.end() ||
                    message == action.arguments.end() ||
                    at == action.arguments.end() ||
                    parsed.ec != std::errc{} ||
                    parsed.ptr != at->second.data() + at->second.size() ||
                    atMs <= nowMs || atMs > nowMs + maximumDelayMs)
                {
                    result.error = "invalidArguments";
                }
                else
                {
                    snowdesktop::widget_runtime::
                        WidgetNotificationContent content;
                    content.title = Utf8ToWideLocal(title->second);
                    content.message = Utf8ToWideLocal(message->second);
                    if (!applyStructuredContent(content))
                    {
                        result.error = "invalidArguments";
                    }
                    const auto due = snowdesktop::widget_runtime::
                        WidgetNotificationCenter::Clock::time_point(
                            std::chrono::milliseconds(atMs));
                    if (result.error.empty())
                    {
                        result = notificationCenter_->Schedule(
                            owner->runtimeToken, content, due,
                            snowdesktop::widget_runtime::
                                WidgetNotificationCenter::Clock::now());
                    }
                    if (result.ok)
                    {
                        snowdesktop::widget_runtime::
                            WidgetPersistedNotificationSchedule persisted{
                                notificationInstanceId,
                                owner->packageId, result.id,
                                title->second, message->second, atMs };
                        if (const auto image = action.arguments.find(
                                "imageResource");
                            image != action.arguments.end())
                            persisted.imageResource = image->second;
                        persisted.progress = content.progress;
                        for (const auto& notificationAction :
                            content.actions)
                        {
                            persisted.actions.push_back({
                                notificationAction.id,
                                WidgetWideToUtf8(
                                    notificationAction.label) });
                        }
                        std::string persistenceError;
                        const bool stored = notificationScheduleStore_ &&
                            notificationScheduleStore_->Upsert(
                                std::move(persisted),
                                persistenceError) &&
                            SaveNotificationSchedules();
                        if (!stored)
                        {
                            if (notificationScheduleStore_)
                            {
                                (void)notificationScheduleStore_->Remove(
                                    notificationInstanceId, result.id);
                            }
                            (void)notificationCenter_->Cancel(
                                owner->runtimeToken, result.id);
                            result = { false, {}, "persistenceFailed" };
                        }
                    }
                }
            }
            else
            {
                const auto id = action.arguments.find("notificationId");
                if (id == action.arguments.end())
                {
                    result.error = "invalidArguments";
                }
                else if (action.name == "notification.update")
                {
                    const auto title = action.arguments.find("title");
                    const auto message = action.arguments.find("message");
                    snowdesktop::widget_runtime::WidgetNotificationPatch patch;
                    if (title != action.arguments.end())
                        patch.title = Utf8ToWideLocal(title->second);
                    if (message != action.arguments.end())
                        patch.message = Utf8ToWideLocal(message->second);
                    const auto image = action.arguments.find("imageResource");
                    if (image != action.arguments.end())
                    {
                        if (image->second.empty())
                        {
                            patch.imagePath = std::wstring{};
                        }
                        else
                        {
                            const auto imagePath = RuntimeResolvePackageResource(
                                owner->widgetId, image->second, "image");
                            if (!imagePath) result.error = "invalidArguments";
                            else patch.imagePath = *imagePath;
                        }
                    }
                    const auto progress = action.arguments.find("progress");
                    if (progress != action.arguments.end())
                    {
                        if (progress->second.empty())
                        {
                            patch.progress.emplace(std::optional<double>{});
                        }
                        else
                        {
                            double progressValue = 0.0;
                            if (!parseProgress(progress->second, progressValue))
                                result.error = "invalidArguments";
                            else
                                patch.progress.emplace(progressValue);
                        }
                    }
                    const auto actionCount =
                        action.arguments.find("actionCount");
                    if (actionCount != action.arguments.end())
                    {
                        std::vector<snowdesktop::widget_runtime::
                            WidgetNotificationAction> actions;
                        if (!readActions(actions))
                            result.error = "invalidArguments";
                        else
                            patch.actions = std::move(actions);
                    }
                    const auto persisted = notificationScheduleStore_
                        ? notificationScheduleStore_->Find(
                            notificationInstanceId, id->second)
                        : std::nullopt;
                    std::string admissionError;
                    if (result.error.empty())
                    {
                        result = notificationCenter_->Update(
                            owner->runtimeToken, id->second,
                            std::move(patch),
                            [this, owner, &admissionError, &host](
                                const snowdesktop::widget_runtime::
                                    WidgetNotificationHostRequest& request) {
                                admissionError =
                                    RuntimeAdmitNotification(owner->widgetId);
                                return admissionError.empty() && host(request);
                            });
                    }
                    if (!admissionError.empty())
                        result.error = std::move(admissionError);
                    if (result.ok && persisted)
                    {
                        const std::optional<std::string> persistedTitle =
                            title != action.arguments.end()
                            ? std::optional(title->second) : std::nullopt;
                        const std::optional<std::string> persistedMessage =
                            message != action.arguments.end()
                            ? std::optional(message->second) : std::nullopt;
                        const std::optional<std::string> persistedImage =
                            image != action.arguments.end()
                            ? std::optional(image->second) : std::nullopt;
                        std::optional<std::optional<double>> persistedProgress;
                        if (progress != action.arguments.end())
                        {
                            if (progress->second.empty())
                                persistedProgress.emplace(
                                    std::optional<double>{});
                            else
                            {
                                double value = 0.0;
                                (void)parseProgress(progress->second, value);
                                persistedProgress.emplace(value);
                            }
                        }
                        std::optional<std::vector<
                            snowdesktop::widget_runtime::
                                WidgetPersistedNotificationAction>>
                            persistedActions;
                        if (actionCount != action.arguments.end())
                        {
                            persistedActions.emplace();
                            int count = 0;
                            (void)std::from_chars(
                                actionCount->second.data(),
                                actionCount->second.data() +
                                    actionCount->second.size(), count);
                            for (int index = 0; index < count; ++index)
                            {
                                persistedActions->push_back({
                                    action.arguments.at("action" +
                                        std::to_string(index) + "Id"),
                                    action.arguments.at("action" +
                                        std::to_string(index) + "Label") });
                            }
                        }
                        if (!notificationScheduleStore_->UpdateContent(
                                notificationInstanceId, id->second,
                                persistedTitle, persistedMessage,
                                persistedImage, persistedProgress,
                                persistedActions) ||
                            !SaveNotificationSchedules())
                        {
                            std::string ignored;
                            (void)notificationScheduleStore_->Upsert(
                                *persisted, ignored);
                            (void)SaveNotificationSchedules();
                            snowdesktop::widget_runtime::
                                WidgetNotificationPatch rollback;
                            rollback.title = Utf8ToWideLocal(persisted->title);
                            rollback.message = Utf8ToWideLocal(
                                persisted->message);
                            if (persisted->imageResource.empty())
                                rollback.imagePath = std::wstring{};
                            else if (const auto oldImage =
                                RuntimeResolvePackageResource(owner->widgetId,
                                    persisted->imageResource, "image"))
                                rollback.imagePath = *oldImage;
                            else
                                rollback.imagePath = std::wstring{};
                            rollback.progress.emplace(persisted->progress);
                            rollback.actions.emplace();
                            for (const auto& oldAction : persisted->actions)
                            {
                                rollback.actions->push_back({ oldAction.id,
                                    Utf8ToWideLocal(oldAction.label) });
                            }
                            (void)notificationCenter_->Update(
                                owner->runtimeToken, id->second,
                                std::move(rollback), host);
                            result = { false, {}, "persistenceFailed" };
                        }
                    }
                }
                else if (action.name == "notification.dismiss")
                {
                    result = notificationCenter_->Dismiss(
                        owner->runtimeToken, id->second, host);
                }
                else
                {
                    const auto persisted = notificationScheduleStore_
                        ? notificationScheduleStore_->Find(
                            notificationInstanceId, id->second)
                        : std::nullopt;
                    result = notificationCenter_->Cancel(
                        owner->runtimeToken, id->second);
                    if (result.ok)
                    {
                        const bool removed = persisted &&
                            notificationScheduleStore_ &&
                            notificationScheduleStore_->Remove(
                                notificationInstanceId, id->second);
                        if (!removed || !SaveNotificationSchedules())
                        {
                            if (persisted && notificationScheduleStore_)
                            {
                                std::string ignored;
                                (void)notificationScheduleStore_->Upsert(
                                    *persisted, ignored);
                                (void)SaveNotificationSchedules();
                                const auto due = snowdesktop::widget_runtime::
                                    WidgetNotificationCenter::Clock::
                                        time_point(std::chrono::milliseconds(
                                            persisted->dueMs));
                                snowdesktop::widget_runtime::
                                    WidgetNotificationContent content;
                                content.title = Utf8ToWideLocal(
                                    persisted->title);
                                content.message = Utf8ToWideLocal(
                                    persisted->message);
                                if (!persisted->imageResource.empty())
                                {
                                    if (const auto imagePath =
                                        RuntimeResolvePackageResource(
                                            owner->widgetId,
                                            persisted->imageResource,
                                            "image"))
                                        content.imagePath = *imagePath;
                                }
                                content.progress = persisted->progress;
                                for (const auto& oldAction :
                                    persisted->actions)
                                {
                                    content.actions.push_back({ oldAction.id,
                                        Utf8ToWideLocal(oldAction.label) });
                                }
                                (void)notificationCenter_->RestoreScheduled(
                                    owner->runtimeToken,
                                    persisted->notificationId,
                                    std::move(content), due,
                                    snowdesktop::widget_runtime::
                                        WidgetNotificationCenter::Clock::
                                            now());
                            }
                            result = { false, {}, "persistenceFailed" };
                        }
                    }
                }
            }
            if (result.ok && (action.name == "notification.show" ||
                    action.name == "notification.schedule"))
            {
                notificationTaskCompletions_.insert_or_assign(
                    action.id, result.id);
            }
            (void)taskBroker_->Complete(action.id,
                result.ok, std::move(result.error));
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
            if (!reference->second.available)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "unavailableReference");
                continue;
            }
            if (!reference->second.persistent &&
                reference->second.catalogRevision != appIndexRevision_)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "staleReference");
                continue;
            }
            const std::wstring launchTarget = snowdesktop::
                logical_slot_picker_rules::NormalizeApplicationLaunchTarget(
                    Utf8ToWideLocal(reference->second.launchTarget));
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
            if (!reference->second.available)
            {
                (void)taskBroker_->Complete(
                    action.id, false, "unavailableReference");
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
            const auto hostPage = page == action.arguments.end()
                ? std::nullopt
                : snowdesktop::widget_runtime::HostSettingsPage(page->second);
            const auto uri = page == action.arguments.end() || hostPage
                ? std::nullopt
                : snowdesktop::widget_runtime::SystemSettingsUri(
                    page->second);
            if (!uri && !hostPage)
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
            if (hostPage)
            {
                const bool accepted = hostSettingsPageCallback_ &&
                    hostSettingsPageCallback_(*hostPage);
                (void)taskBroker_->Complete(action.id, accepted,
                    accepted ? std::string{} : "openRejected");
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
            using namespace snowdesktop::widget_runtime;
            WidgetClipboardTaskRequest clipboardRequest;
            const ClipboardWriteSources sources{
                [this, &owner](const std::string& name, WidgetClipboardTaskRequest& request) {
                    if (IsWidgetRuntimeImageToken(name))
                    {
                        if (d2dState_)
                        {
                            const auto source = d2dState_->runtimeImages.find(name);
                            if (source != d2dState_->runtimeImages.end() &&
                                source->second.ownerWidgetId == owner->widgetId)
                                request.image = source->second.pixels;
                        }
                    }
                    else if (const auto path = RuntimeResolvePackageResource(
                            owner->widgetId, name, "image"))
                    {
                        request.imagePath = *path;
                    }
                },
                [this, &owner, &action](const std::string& reference, std::filesystem::path& path) {
                    return ResolveClipboardWriteFile(*owner, action,
                        filesystemHandleStore_.get(), reference, path);
                },
            };
            if (const char* error = PrepareClipboardTaskRequest(action, sources, clipboardRequest))
            {
                (void)taskBroker_->Complete(action.id, false, error);
                continue;
            }
            if (action.preview)
            {
                if (action.name == "clipboard.read")
                    clipboardTaskDispatch_.StorePreviewRead(action.id, clipboardRequest.format);
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
            request.multiple = action.arguments.contains("multiple") &&
                action.arguments.at("multiple") == "true";
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
                filesystemTaskDispatch_.StorePreviewSelection(action.id,
                    { action.instanceId, owner->packageId }, kind, request.access);
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
            // The modal picker pumps messages; resolve the owner again before granting.
            owner = std::find_if(widgets_.begin(), widgets_.end(), [&action](const LuaWidget& candidate) {
                return candidate.runtimeToken == action.ownerToken;
            });
            const auto currentTask = taskBroker_->Snapshot(action.id);
            if (owner == widgets_.end() || !currentTask || currentTask->cancelRequested)
            {
                (void)taskBroker_->Complete(action.id, false, "canceled");
                continue;
            }
            const bool needsRead = request.access != snowdesktop::widget_runtime::WidgetFilesystemHandleAccess::Write;
            const bool needsWrite = request.access != snowdesktop::widget_runtime::WidgetFilesystemHandleAccess::Read;
            if ((needsRead && !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(owner->permissions, kFilesystemReadPermission)) ||
                (needsWrite && !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(owner->permissions, kFilesystemWritePermission)))
            {
                (void)taskBroker_->Complete(action.id, false, "permissionRevoked");
                continue;
            }
            if (selected.paths.empty()) selected.paths.push_back(selected.path);
            if (selected.paths.size() > (request.multiple ? 128u : 1u))
            {
                (void)taskBroker_->Complete(action.id, false, "tooManySelections");
                continue;
            }
            std::string grantError = filesystemTaskDispatch_.GrantSelection(action.id,
                *filesystemHandleStore_, { action.instanceId, owner->packageId },
                selected.paths, kind, request.access);
            if (!grantError.empty())
            {
                (void)taskBroker_->Complete(action.id, false, std::move(grantError));
                continue;
            }
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
                if (RuntimeIsFilesystemSettingHandleValue(
                        Utf8ToWideLocal(action.instanceId),
                        handle->second))
                {
                    (void)taskBroker_->Complete(
                        action.id, false, "hostManagedReference");
                    continue;
                }
                const bool busy = filesystemTaskDispatch_.HasActiveHandle(handle->second) ||
                    std::any_of(
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
                if (revoked && audioPlayback_) audioPlayback_->RevokeFile(action.ownerToken, handle->second);
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
            if (const auto name = action.arguments.find("name"); name != action.arguments.end())
                request.name = name->second;
            if (const auto grant = action.arguments.find("grantHandles"); grant != action.arguments.end())
                request.grantHandles = grant->second != "false";
            if (const auto encoding = action.arguments.find("encoding");
                encoding != action.arguments.end())
                request.encoding = encoding->second;
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
            if (action.name == "filesystem.image" &&
                !parseSize("maxDimension", 2048, request.maxDimension))
            {
                (void)taskBroker_->Complete(action.id, false, "invalidArguments");
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
                const auto text = action.arguments.find(
                    request.encoding == "binary" ? "data" : "text");
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
                filesystemTaskDispatch_.StorePreviewResult(action.id, request);
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
                filesystemTaskDispatch_.TrackHandle(action.id, handle->second);
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
            audioRequest.coalesce = action.arguments.contains("coalesce") &&
                action.arguments.at("coalesce") == "1";
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
        snowdesktop::performance::Scope performanceScope(
            "task.completion", completion.name,
            snowdesktop::performance::Enabled()
                ? Utf8ToWideLocal(completion.instanceId) : std::wstring{}, completion.id);
        auto widget = std::find_if(widgets_.begin(), widgets_.end(),
            [&completion](const LuaWidget& candidate) {
                return candidate.runtimeToken == completion.ownerToken;
            });
        const auto nativePayload = nativeTaskDispatch_.TakeCompletion(completion, [&] {
            return widget != widgets_.end() && widget->permissions.contains("native.execute");
        });
        const auto searchCompletion =
            appSearchCompletions_.find(completion.id);
        const auto itemSearchCompletion =
            itemSearchCompletions_.find(completion.id);
        const auto calendarCompletion =
            calendarMutationCompletions_.find(completion.id);
        const auto notificationCompletion =
            notificationTaskCompletions_.find(completion.id);
        const auto locationCompletion = locationTaskCompletions_.find(completion.id);
        if (widget == widgets_.end() ||
            widget->taskIds.erase(completion.id) == 0)
        {
            locationTaskCompletions_.erase(completion.id);
            if (searchCompletion != appSearchCompletions_.end())
                appSearchCompletions_.erase(searchCompletion);
            if (itemSearchCompletion != itemSearchCompletions_.end())
                itemSearchCompletions_.erase(itemSearchCompletion);
            if (calendarCompletion != calendarMutationCompletions_.end())
                calendarMutationCompletions_.erase(calendarCompletion);
            if (notificationCompletion !=
                    notificationTaskCompletions_.end())
                notificationTaskCompletions_.erase(
                    notificationCompletion);
            networkTaskDispatch_.DiscardCompletion(completion.id);
            clipboardTaskDispatch_.DiscardCompletion(completion.id);
            filesystemTaskDispatch_.DiscardCompletion(completion.id);
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
            completion.name == "calendar.remove" ||
            completion.name == "calendar.series.create" ||
            completion.name == "calendar.series.update" ||
            completion.name == "calendar.series.remove";
        if (calendarTask &&
            calendarCompletion == calendarMutationCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool locationTask = completion.name == "location.current";
        const auto* locationResult = locationCompletion != locationTaskCompletions_.end() ? &locationCompletion->second : nullptr;
        if (completion.ok && locationTask && (!locationResult ||
            !snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(widget->permissions, "location.read")))
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        const bool networkTask = completion.name == "network.request";
        const bool notificationIdTask =
            completion.name == "notification.show" ||
            completion.name == "notification.schedule";
        if (completion.ok && notificationIdTask &&
            notificationCompletion == notificationTaskCompletions_.end())
        {
            completion.ok = false;
            completion.error = "taskResultUnavailable";
        }
        auto clipboardPayload = clipboardTaskDispatch_.TakeCompletion(completion);
        auto filesystemPayload = filesystemTaskDispatch_.TakeCompletion(completion);
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
        const auto networkResult = networkTaskDispatch_.TakeCompletion(completion);
        if (clipboardPayload.HasValue())
        {
            clipboardPayload.PrepareValue(completion,
                [this, &widget](const std::string& token,
                    const std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>& pixels) {
                    return RegisterRuntimeImageSource(d2dState_, widget->widgetId, token, pixels, "clipboard");
                },
                [&widget, &completion](std::size_t count,
                    std::span<const snowdesktop::widget_runtime::ClipboardFileItem> files) {
                    constexpr std::size_t MaximumReferences = 4096;
                    std::erase_if(widget->itemReferences, [](const auto& entry) {
                        return entry.second.sourceTask == "clipboard.read";
                    });
                    if (widget->itemReferences.size() + count > MaximumReferences)
                        widget->itemReferences.clear();
                    for (const auto& file : files)
                    {
                        widget->itemReferences.insert_or_assign(file.reference,
                            LuaWidget::ItemReference{file.target, "clipboard.read",
                                completion.id, file.name, "clipboard",
                                file.folder ? "folder" : "file", "filesystem.reference", false});
                    }
                });
        }
        filesystemPayload.PrepareValue(completion, [this, &widget](const std::string& token,
            const std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>& pixels) {
            return RegisterRuntimeImageSource(d2dState_, widget->widgetId, token, pixels, "filesystem");
        });
        snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
            trustedGestureState_, false);
        (void)InvokeLifecycleEvent(*widget, "task.complete",
            [&completion, &publicItems, &clipboardPayload,
                nextOffset, hasMore,
                catalogRevision, calendarTask, itemSearchTask,
                calendarResult, notificationIdTask,
                notificationCompletion, locationTask, locationResult, networkTask,
                &networkResult, &filesystemPayload, &nativePayload](lua_State* eventState) {
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(completion.id));
                lua_setfield(eventState, -2, "taskId");
                lua_pushlstring(eventState, completion.name.data(),
                    completion.name.size());
                lua_setfield(eventState, -2, "task");
                lua_pushboolean(eventState, completion.ok ? 1 : 0);
                lua_setfield(eventState, -2, "ok");
                if (const auto* diagnostic = nativePayload.Diagnostic())
                {
                    PushSystemControlJson(eventState, *diagnostic);
                    lua_setfield(eventState, -2, "diagnostic");
                }
                if (completion.ok)
                {
                    if (const auto* value = nativePayload.Value())
                        PushSystemControlJson(eventState, *value);
                    else if (completion.name == "audio.play")
                    {
                        lua_createtable(eventState, 0, 1);
                        const std::string playbackId = "audio-" + std::to_string(completion.id);
                        lua_pushstring(eventState, playbackId.c_str()); lua_setfield(eventState, -2, "playbackId");
                    }
                    else if (completion.name == "app.search" || itemSearchTask)
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
                    else if (notificationIdTask)
                    {
                        lua_createtable(eventState, 0, 1);
                        lua_pushlstring(eventState,
                            notificationCompletion->second.data(),
                            notificationCompletion->second.size());
                        lua_setfield(eventState, -2,
                            "notificationId");
                    }
                    else if (locationTask)
                    {
                        lua_createtable(eventState, 0, 6);
                        lua_pushnumber(eventState, locationResult->latitude); lua_setfield(eventState, -2, "latitude");
                        lua_pushnumber(eventState, locationResult->longitude); lua_setfield(eventState, -2, "longitude");
                        lua_pushnumber(eventState, locationResult->accuracyMeters); lua_setfield(eventState, -2, "accuracyMeters");
                        lua_pushinteger(eventState, locationResult->timestampMs); lua_setfield(eventState, -2, "timestampMs");
                        lua_pushlstring(eventState, locationResult->source.data(), locationResult->source.size());
                        lua_setfield(eventState, -2, "source");
                    }
                    else if (networkTask)
                        snowdesktop::widget_runtime::PushNetworkTaskValue(eventState, *networkResult);
                    else if (clipboardPayload.HasValue())
                        clipboardPayload.PushValue(eventState);
                    else if (filesystemPayload.HasValue())
                        filesystemPayload.PushValue(eventState, completion.name);
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
                        snowdesktop::widget_runtime::PushNetworkTaskStatus(eventState, *networkResult);
                }
            });
        if (searchCompletion != appSearchCompletions_.end())
            appSearchCompletions_.erase(searchCompletion);
        if (itemSearchCompletion != itemSearchCompletions_.end())
            itemSearchCompletions_.erase(itemSearchCompletion);
        if (calendarCompletion != calendarMutationCompletions_.end())
            calendarMutationCompletions_.erase(calendarCompletion);
        if (notificationCompletion !=
                notificationTaskCompletions_.end())
            notificationTaskCompletions_.erase(notificationCompletion);
        locationTaskCompletions_.erase(completion.id);
    }
    for (const auto& message : audioEvents)
    {
            auto owner = std::find_if(widgets_.begin(), widgets_.end(), [&](const auto& candidate) {
                return candidate.runtimeToken == message.owner;
            });
            if (owner == widgets_.end() || !owner->permissions.contains("audio.playback")) continue;
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(trustedGestureState_, false);
            (void)InvokeLifecycleEvent(*owner, message.command.command.empty() ? "audio.state" : "audio.command", [&](lua_State* state) {
                lua_pushstring(state, message.playbackId.c_str()); lua_setfield(state, -2, "playbackId");
                lua_pushstring(state, message.state.status.c_str()); lua_setfield(state, -2, "status");
                lua_pushstring(state, message.state.error.c_str()); lua_setfield(state, -2, "error");
                lua_pushinteger(state, message.state.positionMs); lua_setfield(state, -2, "positionMs");
                lua_pushinteger(state, message.state.durationMs); lua_setfield(state, -2, "durationMs");
                lua_pushnumber(state, message.state.volume); lua_setfield(state, -2, "volume");
                lua_pushboolean(state, message.state.loop); lua_setfield(state, -2, "loop");
                lua_pushboolean(state, message.state.canSeek); lua_setfield(state, -2, "canSeek");
                lua_pushnumber(state, message.state.playbackRate); lua_setfield(state, -2, "playbackRate");
                lua_pushboolean(state, message.state.shuffle); lua_setfield(state, -2, "shuffle");
                lua_pushstring(state, message.state.repeatMode.c_str()); lua_setfield(state, -2, "repeatMode");
                lua_pushboolean(state, message.state.systemMediaControls); lua_setfield(state, -2, "systemMediaControls");
                lua_pushstring(state, message.state.title.c_str()); lua_setfield(state, -2, "title");
                lua_pushstring(state, message.state.artist.c_str()); lua_setfield(state, -2, "artist");
                lua_pushstring(state, message.state.album.c_str()); lua_setfield(state, -2, "album");
                if (message.state.artwork)
                {
                    const auto token = RegisterRuntimeImageSource(d2dState_, owner->widgetId,
                        message.state.artworkToken, message.state.artwork, "audio");
                    if (!token.empty())
                    {
                        snowdesktop::widget_runtime::PushResourceHandle(state,
                            snowdesktop::widget_runtime::LuaResourceType::Image, token);
                        lua_setfield(state, -2, "artwork");
                    }
                }
                if (!message.command.command.empty())
                {
                    lua_pushstring(state, message.command.command.c_str()); lua_setfield(state, -2, "command");
                    if (message.command.command == "setPlaybackRate")
                    { lua_pushnumber(state, message.command.value); lua_setfield(state, -2, "playbackRate"); }
                    if (message.command.command == "setShuffle")
                    { lua_pushboolean(state, message.command.value != 0); lua_setfield(state, -2, "shuffle"); }
                    if (message.command.command == "setRepeatMode")
                    { lua_pushstring(state, message.command.repeatMode.c_str()); lua_setfield(state, -2, "repeatMode"); }
                }
            });
    }

}

void WidgetEngine::CancelDispatchedWidgetTask(
    std::uint64_t taskId, bool completeLocation)
{
    if (nativeTaskExecutor_) (void)nativeTaskExecutor_->Cancel(taskId);
    nativeTaskDispatch_.DiscardCompletion(taskId);
    CancelWidgetTaskWork(taskId, completeLocation);
}

void WidgetEngine::CancelWidgetTaskWork(
    std::uint64_t taskId, bool completeLocation)
{
    if (locationTaskExecutor_) (void)locationTaskExecutor_->Cancel(taskId);
    locationTaskCompletions_.erase(taskId);
    if (completeLocation) (void)taskBroker_->Complete(taskId, false);
    if (systemControlTasks_) (void)systemControlTasks_->Cancel(taskId);
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
    notificationTaskCompletions_.erase(taskId);
    clipboardTaskDispatch_.DiscardCompletion(taskId);
    filesystemTaskDispatch_.DiscardCompletion(taskId);
}

void WidgetEngine::ReleaseWidgetTasks(LuaWidget& widget,
    snowdesktop::widget_runtime::TaskBrokerCancelReason reason)
{
    ForgetWidgetMultiplayer(widget.runtimeToken);
    if (audioPlayback_) audioPlayback_->ForgetOwner(widget.runtimeToken);
    if (notificationCenter_)
        notificationCenter_->RemoveOwner(
            widget.runtimeToken, notifyCallback_);
    if (!taskBroker_)
    {
        widget.taskIds.clear();
        return;
    }
    for (const std::uint64_t taskId : widget.taskIds)
    {
        (void)taskBroker_->Cancel(taskId, reason);
        CancelDispatchedWidgetTask(taskId, false);
        networkTaskDispatch_.DiscardCompletion(taskId);
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
    d2dState_->packageImageCache.SetReadyCallback(taskWakeCallback_);
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    d2dState_->shellIconLoader =
        std::make_unique<AsyncShellIconLoader>(
            [this](const std::wstring& widgetId) {
                RuntimeInvalidateHost(widgetId);
            });

    // Warm the validated package catalogue and this UI thread's menu metadata
    // before loading layouts, so the first Add Widget popup only probes changes.
    TrimWidgetPackageHistory(GetWidgetPackageManager());
    (void)ListAvailableMenuEntries();
    PrewarmWidgetPackageResources(d2dState_);

    // Init storage path
    snowdesktop::widget_runtime::OpenStorageFile(GetDataFilePath(L"SnowDesktop.storage.json"));
    secretStore_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetSecretStore>(
            WidgetSecretStorePath());
    std::string secretStoreError;
    if (!secretStore_->Load(secretStoreError))
    {
        const std::string diagnostic =
            "SnowDesktop widget secret store load failed: " +
            secretStoreError + "\n";
        OutputDebugStringA(diagnostic.c_str());
    }
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
            (void)notifyCallback_({
                snowdesktop::widget_runtime::
                    WidgetNotificationHostOperation::Show,
                {}, _LW("calendar.notification_title"),
                Utf8ToWideLocal(event.title) });
        });
    calendarServiceLoaded_ = snowdesktop::widget_runtime::LoadWidgetCalendarProvider(
        *calendarService_, GetDataFilePath(L"SnowDesktop.calendar.json"));
    systemSnapshotService_ = std::make_unique<SystemSnapshotService>();
    httpService_ = std::make_unique<AsyncHttpService>();
    InitializeWidgetDataBroker();
    InitializeWidgetTaskBroker();
    LoadNotificationSchedules();
    (void)d2dState_->packageImageCache.PrewarmPending();
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

void WidgetEngine::ResetGraphicsResources(ID2D1DeviceContext* context)
{
    d2dContext_ = context;
    if (!d2dState_) return;
    d2dState_->ctx = nullptr;
    d2dState_->bitmapDevice.Reset();
    d2dState_->immediateCommandContext.Reset();
    d2dState_->backgroundCache.Clear();
    d2dState_->imageSamplingCache.Clear();
    d2dState_->ResetBrushes();
    d2dState_->imageCache.clear();
    d2dState_->imageCacheRevisions.clear();
    d2dState_->runtimeImageBitmaps.clear();
    ClearShellIconBitmaps(d2dState_);
    d2dState_->shellIconFailures.clear();
    for (auto& widget : widgets_)
    {
        widget.desktopMarquee = {};
        widget.panelMarquee = {};
    }
}

void WidgetEngine::Shutdown()
{
    if (d2dState_) d2dState_->scriptReadAhead.Stop();
    focusedHostInput_ = {};
    if (taskBroker_)
        taskBroker_->Shutdown();
    if (notificationCenter_)
        notificationCenter_->Clear(notifyCallback_);
    if (d2dState_)
        d2dState_->shellIconLoader.reset();
    for (auto& widget : widgets_)
    {
        ClearNativeMarqueeComposition(widget);
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
        StopAnimationFrames(widget);
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
    calendarServiceLoaded_ = false;
    pendingCalendarSelectionChange_ = false;
    pendingCalendarEventsChange_ = false;
    for (auto& widget : widgets_)
    {
        if (widget.state)
        {
            widget.lifecycle.Release(widget.state);
            luaL_unref(widget.state, LUA_REGISTRYINDEX, widget.ref);
            ReleasePackageImageResources(d2dState_, widget.state);
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
        widgetSystemDataProvider_->RemoveConsumer("widgets");
    if (widgetAudioAnalysisProvider_)
        widgetAudioAnalysisProvider_->Stop();
    widgets_.clear();
    systemControlTasks_.reset();
    widgetSystemDataProvider_.reset();
    widgetAudioAnalysisProvider_.reset();
    dataBroker_.reset();
    nativeTaskExecutor_.reset();
    nativeTaskDispatch_.Clear();
    locationTaskExecutor_.reset();
    locationTaskCompletions_.clear();
    audioPlayback_.reset();
    mediaTaskExecutor_.reset();
    audioOutputTaskExecutor_.reset();
    clipboardTaskExecutor_.reset();
    filesystemWatchService_.reset();
    filesystemHandleStore_.reset();
    secretStore_.reset();
    filesystemTaskExecutor_.reset();
    appTaskExecutor_.reset();
    desktopTaskExecutor_.reset();
    externalItemTaskExecutor_.reset();
    appSearchCompletions_.clear();
    itemSearchCompletions_.clear();
    calendarMutationCompletions_.clear();
    notificationTaskCompletions_.clear();
    networkTaskDispatch_.Clear();
    clipboardTaskDispatch_.Clear();
    filesystemTaskDispatch_.Clear();
    filesystemWatchBindings_.clear();
    taskBroker_.reset();
    notificationCenter_.reset();
    notificationScheduleStore_.reset();
    notificationSchedulePath_.clear();
    widgetHostFailures_.clear();
    delete d2dState_; d2dState_ = nullptr;
}

void WidgetEngine::UnloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    if (d2dState_) d2dState_->scriptReadAhead.Forget(widgetId);
    RemoveNotificationSchedules(widgetId);
    PreviewExecutionScope previewScope(
        widgets_[idx].preview ? &widgets_[idx].previewStorage : nullptr);
    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId)
        focusedHostInput_ = {};
    if (hostScrollbarDrag_.active &&
        hostScrollbarDrag_.widgetId == widgetId)
        hostScrollbarDrag_ = {};
    if (widgets_[idx].hostVisible)
        InvokeSimpleCallback(widgets_[idx], "onHidden");
    ClearNativeMarqueeComposition(widgets_[idx]);
    DisposeWidgetLifecycle(widgets_[idx], "unload");
    ReleaseWidgetDataSubscriptions(widgets_[idx]);
    ReleaseWidgetTasks(widgets_[idx],
        snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
    if (widgets_[idx].refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].refreshTimerId);
    if (widgets_[idx].namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].namedTimerId);
    StopAnimationFrames(widgets_[idx]);
    if (httpService_) httpService_->CancelWidget(widgetId);
    lua_State* state = widgets_[idx].state;
    if (state)
    {
        widgets_[idx].lifecycle.Release(state);
        luaL_unref(state, LUA_REGISTRYINDEX, widgets_[idx].ref);
        ReleasePackageImageResources(d2dState_, state);
        lua_close(state);
        widgets_[idx].state = nullptr;
    }
    if (d2dState_)
    {
        ClearRuntimeImagesForWidget(d2dState_, widgetId);
        d2dState_->ClearPrivateTextFormats();
    }
    widgets_.erase(widgets_.begin() + idx);
    std::erase_if(widgets_, [&widgetId](const LuaWidget& widget) {
        return widget.widgetId == widgetId;
    });

}

bool WidgetEngine::RequiresRemovalConfirmation(const std::wstring& widgetId,
    const std::wstring& packageId)
{
    const int index = FindWidget(widgetId);
    if (index >= 0 && widgets_[index].manifest.confirmRemoval)
        return true;
    // Resolve metadata even if the script failed to load or was disabled.
    if (const auto package = GetWidgetPackage(packageId))
        return package->manifest.confirmRemoval;
    // A missing package must not silently discard an orphaned instance's data.
    return index < 0;
}

void WidgetEngine::DeleteWidgetInstance(const std::wstring& widgetId)
{
    RemoveNotificationSchedules(widgetId);
    if (filesystemHandleStore_)
    {
        std::string error;
        (void)filesystemHandleStore_->RevokeInstance(
            WidgetWideToUtf8(widgetId), error);
    }
    UnloadWidget(widgetId);
    if (secretStore_)
    {
        bool removed = false;
        std::string error;
        if (!secretStore_->RemoveInstance(
                WidgetWideToUtf8(widgetId), removed, error))
        {
            OutputDebugStringA((
                "SnowDesktop widget secret cleanup failed: " + error +
                "\n").c_str());
        }
    }
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
        snowdesktop::widget_runtime::ApplyLayoutSpan(
            *d2dState_, widget.lastColumns, widget.lastRows);
        snowdesktop::widget_runtime::ApplyLayoutMetrics(
            *d2dState_, widget.layoutMetrics);
    }
}

void WidgetEngine::InvokeSimpleCallback(LuaWidget& widget, const char* callbackName)
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
}

bool WidgetEngine::InitializeWidgetLifecycle(LuaWidget& widget)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.lifecycle", "setup", widget.widgetId);
    lua_State* state = widget.state;
    if (!state) return false;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, BoundsForSurface(
        widget, CurrentWidgetSurface(d2dState_)));
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
    snowdesktop::performance::Scope performanceScope(
        "widget.event", kind ? kind : "unknown", widget.widgetId);
    if (!widget.state || !kind || !*kind)
        return false;
    snowdesktop::widget_runtime::WidgetInvalidationBatch::Scope
        invalidationScope(invalidationBatch_, invalidateCallback_);
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    SetWidgetRectContext(d2dState_, BoundsForSurface(
        widget, CurrentWidgetSurface(d2dState_)));
    bool invoked = false;
    std::string error;
    struct EventArguments
    {
        const char* kind;
        const std::function<void(lua_State*)>* fields;
    } arguments{kind, &pushFields};
    if (!snowdesktop::lua_runtime::ProtectedHostCall(widget.state,
            +[](lua_State* inner) {
                const auto* args = static_cast<EventArguments*>(lua_touserdata(inner, 1));
                lua_createtable(inner, 0, 10);
                lua_pushstring(inner, args->kind);
                lua_setfield(inner, -2, "kind");
                if (*args->fields) (*args->fields)(inner);
                return 1;
            }, &arguments, 1, error))
    {
        RuntimeRecordError(widget.widgetId, error);
        return false;
    }
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    const bool succeeded = widget.lifecycle.Event(
        widget.state, widget.ref, pushContext, -1, invoked, error);
    if (!succeeded && !error.empty())
        RuntimeRecordError(widget.widgetId, error);
    else if (succeeded && invoked)
        RuntimeRecordSuccess(widget.widgetId);
    const bool stateChanged = snowdesktop::widget_api::
        ConsumeTransientStateDirty(widget.state);
    if ((succeeded && invoked) || stateChanged)
        RuntimeInvalidateHost(widget.widgetId);
    return succeeded && invoked;
}

void WidgetEngine::DisposeWidgetLifecycle(
    LuaWidget& widget, const char* reason)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.lifecycle", "dispose", widget.widgetId);
    if (!widget.state) return;
    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widget.widgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    SetWidgetRectContext(d2dState_, BoundsForSurface(
        widget, CurrentWidgetSurface(d2dState_)));
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
        snowdesktop::widget_api::SandboxLibraries())
    {
        if (name == "l10n") continue;
        const std::string libraryName(name);
        PushReadOnlyGlobal(L, libraryName.c_str());
        lua_setfield(L, -2, libraryName.c_str());
    }
    PushWidgetL10nAPI(L, widget.manifest.locales);
    PushReadOnlyProxyForTopTable(L);
    lua_setfield(L, -2, "l10n");

    lua_pushstring(L, WidgetWideToUtf8(widget.widgetId).c_str());
    lua_setfield(L, -2, "widgetId");
}

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& packageId)
{
    if (const int index = FindWidget(widgetId); index >= 0)
        return widgets_[index].valid;
    if (const auto failure = widgetHostFailures_.find(widgetId);
        failure != widgetHostFailures_.end())
    {
        // A retained desktop instance may outlive its unsubscribed package.
        // Missing content is recoverable as soon as it is installed again;
        // actual script/quota failures still require the normal retry action.
        if (failure->second.kind !=
                snowdesktop::widget_runtime::WidgetHostStateKind::PackageMissing ||
            !IsWidgetPackageInstalled(packageId))
            return false;
        widgetHostFailures_.erase(failure);
    }
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
        if (!IsWidgetPackageInstalled(packageId))
            widgetHostFailures_[widgetId].kind =
                snowdesktop::widget_runtime::WidgetHostStateKind::PackageMissing;
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

bool WidgetEngine::EnsureWidgetDirectoryPreviewLoaded(
    const std::wstring& widgetId,
    const std::filesystem::path& packageRoot,
    const std::unordered_map<std::string, std::string>& storageOverrides,
    const LuaWidgetAuthorPreviewConfiguration* previewConfiguration)
{
    if (!previewOnly_)
    {
        RuntimeRecordError(widgetId,
            "development packages may only load in a preview engine");
        return false;
    }
    snowdesktop::widget::PackageManifest manifest;
    const auto report = GetWidgetPackageManager().ValidateDirectory(
        packageRoot, &manifest);
    if (!report.Ok())
    {
        std::string message = "component package validation failed";
        for (const auto& issue : report.issues)
        {
            if (issue.severity !=
                snowdesktop::widget::ValidationSeverity::Error)
                continue;
            message = issue.code + ": " + issue.message;
            break;
        }
        RuntimeRecordError(widgetId, message);
        return false;
    }
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(
        packageRoot, error);
    if (error)
    {
        RuntimeRecordError(widgetId,
            "component package root cannot be canonicalized");
        return false;
    }
    const auto entry = std::filesystem::weakly_canonical(
        canonicalRoot / Utf8ToWideLocal(manifest.entry), error);
    if (error)
    {
        RuntimeRecordError(widgetId,
            "component entry cannot be canonicalized");
        return false;
    }
    if (const int index = FindWidget(widgetId); index >= 0)
    {
        std::error_code equivalentError;
        return widgets_[index].preview &&
            widgets_[index].packageId == manifest.id &&
            std::filesystem::equivalent(widgets_[index].packageRoot,
                canonicalRoot, equivalentError) && !equivalentError;
    }
    return LoadWidget(entry.wstring(), widgetId, true,
        &storageOverrides, &canonicalRoot, previewConfiguration);
}

bool WidgetEngine::IsPreviewWidget(const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && widgets_[index].preview;
}

bool WidgetEngine::LoadWidget(const std::wstring& path,
    const std::wstring& widgetId, bool preview,
    const std::unordered_map<std::string, std::string>*
        previewStorageOverrides,
    const std::filesystem::path* packageRootOverride,
    const LuaWidgetAuthorPreviewConfiguration* previewConfiguration,
    bool* deterministicFailure)
{
    const auto markDeterministicFailure = [deterministicFailure] {
        if (deterministicFailure) *deterministicFailure = true;
    };
    LuaWidget pending;
    pending.widgetId = widgetId;
    pending.filePath = path;
    pending.manifest = GetWidgetManifest(path);
    pending.packageId = pending.manifest.packageId;
    pending.preview = preview;
    if (!snowdesktop::widget::IsExecutablePackageContract(
            pending.manifest.schemaVersion,
            pending.manifest.apiVersion))
    {
        RuntimeRecordError(widgetId,
            "Widget schema/API contract is not supported by this host");
        return false;
    }
    std::string source = ReadTextFile(path);
    if (source.empty())
    {
        RuntimeRecordError(widgetId,
            "Widget entry script is empty or cannot be read");
        return false;
    }
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
    if (packageRootOverride)
        pending.packageRoot = *packageRootOverride;
    else if (const auto package = ResolveWidgetEntryPackage(path))
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
    if (!snowdesktop::widget::MeetsWidgetMinimumHostVersion(
            SNOWDESKTOP_VERSION, pending.manifest.minHostVersion))
    {
        markDeterministicFailure();
        RuntimeRecordError(widgetId, "Widget requires SnowDesktop " +
            pending.manifest.minHostVersion + " or newer");
        return false;
    }
    auto grant = snowdesktop::widget::WidgetPermissionBroker::
        GrantForIsolatedExecution(
            pending.manifest.permissions,
            pending.manifest.optionalPermissions,
            pending.manifest.networkDomains);
    if (!preview && !snowdesktop::widget_runtime::IsDryLoad())
    {
        if (const auto package =
            GetWidgetPackageManager().Resolve(pending.packageId))
        {
            grant = snowdesktop::widget::WidgetPermissionBroker::Evaluate(
                package->permissionState,
                pending.manifest.permissions,
                pending.manifest.optionalPermissions,
                pending.manifest.networkDomains,
                package->grantedPermissions,
                package->grantedNetworkDomains);
        }
    }
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

    if (preview && previewConfiguration && d2dState_)
    {
        d2dState_->gridColumns = std::max(
            1, previewConfiguration->columns);
        d2dState_->gridRows = std::max(1, previewConfiguration->rows);
        SetWidgetRectContext(d2dState_, previewConfiguration->bounds);
    }
    WidgetExecutionContextGuard loadContext(d2dState_, widgetId);
    auto quota = std::make_unique<LuaRuntimeQuota>();
    auto storageWriteBudget = std::make_unique<
        snowdesktop::widget_runtime::WidgetStorageWriteBudget>();
    lua_State* state = lua_newstate(snowdesktop::lua_runtime::QuotaAllocator, quota.get());
    if (!state)
    {
        RuntimeRecordError(widgetId, "Cannot allocate isolated Lua state");
        RecordWidgetHostFailure(widgetId,
            "Cannot allocate isolated Lua state", quota->memoryExceeded);
        return false;
    }
    const auto closePendingState = [d2dState = d2dState_](lua_State* value) {
        if (!value) return;
        ReleasePackageImageResources(d2dState, value);
        lua_close(value);
    };
    std::unique_ptr<lua_State, decltype(closePendingState)> stateGuard(
        state, closePendingState);
    const auto missing = snowdesktop::widget_api::MissingFeatures(
        pending.manifest.requiredFeatures);
    if (!missing.empty())
    {
        markDeterministicFailure();
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
    luaL_requiref(state, "_G", luaopen_base, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(state, 1);
    RegisterDrawAPI(state);
    lua_pushlightuserdata(state, d2dState_);
    lua_setfield(state, LUA_REGISTRYINDEX, "__d2d_ptr");
    snowdesktop::widget_runtime::BindWidgetLuaEngine(state,
        d2dState_ ? d2dState_->engine : nullptr);
    snowdesktop::widget_runtime::BindLuaDrawContext(state, d2dState_, kLuaDrawHostServices);
    snowdesktop::widget_runtime::BindWidgetLayoutContext(state, d2dState_);
    snowdesktop::widget_runtime::BindWidgetStorageContext(state,
        d2dState_ ? &kStorageLuaHostServices : nullptr);
    lua_pushlightuserdata(state, quota.get());
    lua_setfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    lua_pushlightuserdata(state, storageWriteBudget.get());
    lua_setfield(state, LUA_REGISTRYINDEX,
        "__storage_write_budget_ptr");
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
    lua_createtable(state, 0,
        static_cast<int>(pending.manifest.resources.size()));
    for (const auto& [name, resource] : pending.manifest.resources)
    {
        const auto resolved = ResolvePackageAssetWithinRoot(
            pending.packageRoot, Utf8ToWideLocal(resource.path));
        if (!resolved) continue;
        const std::string utf8Path = WidgetWideToUtf8(*resolved);
        lua_pushlstring(state, utf8Path.data(), utf8Path.size());
        lua_setfield(state, -2, name.c_str());
    }
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_resource_paths");
    lua_createtable(state, 0,
        static_cast<int>(pending.manifest.resources.size()));
    lua_setfield(state, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    lua_pushboolean(state, preview ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_preview");
    lua_pushinteger(state, std::max(1, pending.manifest.defaultColumns));
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_default_columns");
    lua_pushinteger(state, std::max(1, pending.manifest.defaultRows));
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_default_rows");
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
    if (const int status = luaL_loadstring(state, source.c_str()); status != LUA_OK)
    {
        if (status == LUA_ERRSYNTAX) markDeterministicFailure();
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
        if (const int status = luaL_loadstring(state, wrapped.c_str()); status != LUA_OK)
        {
            if (status == LUA_ERRSYNTAX) markDeterministicFailure();
            const char* err = lua_tostring(state, -1);
            RuntimeRecordError(widgetId, err ? err : "(load error)");
            RecordWidgetHostFailure(widgetId,
                err ? err : "(load error)",
                quota->memoryExceeded || quota->executionExceeded);
            lua_pop(state, 2);
            return false;
        }
        lua_pushvalue(state, -2);  // push sandbox as argument
        if (const int status = snowdesktop::lua_runtime::ProtectedCall(
                state, 1, 1, 1000000,
                std::chrono::milliseconds(100)); status != LUA_OK)
        {
            if (status == LUA_ERRRUN && snowdesktop::widget_runtime::IsDryLoad() &&
                !quota->memoryExceeded && !quota->executionExceeded)
                markDeterministicFailure();
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
        if (const int status = snowdesktop::lua_runtime::ProtectedCall(
                state, 0, 1, 1000000,
                std::chrono::milliseconds(100)); status != LUA_OK)
        {
            if (status == LUA_ERRRUN && snowdesktop::widget_runtime::IsDryLoad() &&
                !quota->memoryExceeded && !quota->executionExceeded)
                markDeterministicFailure();
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
    if (!snowdesktop::widget_api::IsDefinedWidget(state, -1))
    {
        markDeterministicFailure();
        RuntimeRecordError(widgetId,
            "API v2 entry must return widget.define({...})");
        lua_pop(state, 2);
        return false;
    }
    ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1); // sandbox
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

    lua_getfield(state, -1, "backgroundLayer");
    const bool hasBackgroundLayer = lua_istable(state, -1);
    lua_pop(state, 1);

    lua_getfield(state, -1, "showTitle");
    const bool showTitle = !lua_isnil(state, -1) &&
        lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_pushboolean(state, showTitle ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX, "__widget_show_title");

    lua_getfield(state, -1, "bottomBarHover");
    const bool bottomBarHover = lua_isnil(state, -1) ||
        lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_pushboolean(state, bottomBarHover ? 1 : 0);
    lua_setfield(state, LUA_REGISTRYINDEX,
        "__widget_bottom_bar_hover");

    std::vector<LuaWidgetManifest::Setting> scriptSettings;
    std::vector<LuaWidgetManifest::SettingGroup> scriptSettingGroups;
    std::vector<LuaWidgetManifest::SettingPreset> scriptPresets;
    ReadLuaDeclaredSettings(state, -1, scriptSettings,
        scriptSettingGroups, scriptPresets);
    std::string settingsError;
    if (!ValidateWidgetSettings(pending.manifest, scriptSettings,
            scriptSettingGroups, scriptPresets, settingsError))
    {
        RuntimeRecordError(widgetId, settingsError);
        RecordWidgetHostFailure(widgetId, settingsError, false);
        lua_pop(state, 1);
        return false;
    }

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
            MutablePersistentStorage() = std::move(migratedStorage);
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
    w.storageWriteBudget = std::move(storageWriteBudget);
    w.name = name;
    w.filePath = path;
    w.manifest = pending.manifest;
    lua_getfield(w.state, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    if (lua_istable(w.state, -1))
    {
        lua_pushnil(w.state);
        while (lua_next(w.state, -2) != 0)
        {
            size_t nameLength = 0;
            size_t keyLength = 0;
            const char* resourceName = lua_tolstring(
                w.state, -2, &nameLength);
            const char* contentKey = lua_tolstring(
                w.state, -1, &keyLength);
            if (resourceName && contentKey && nameLength > 0 &&
                keyLength > 0)
            {
                w.packageImageContentKeys.insert_or_assign(
                    std::string(resourceName, nameLength),
                    std::string(contentKey, keyLength));
            }
            lua_pop(w.state, 1);
        }
    }
    lua_pop(w.state, 1);
    w.permissions = pending.permissions;
    w.ref = ref;
    w.valid = true;
    w.customStyle = customStyle;
    w.followPersonalizationDefault = followPersonalizationDefault;
    w.hasBackgroundLayer = hasBackgroundLayer;
    if (preview && previewConfiguration)
    {
        w.theme = previewConfiguration->theme;
        w.surfaceContext = previewConfiguration->surface;
        w.lastBounds = previewConfiguration->bounds;
        w.lastColumns = std::max(1, previewConfiguration->columns);
        w.lastRows = std::max(1, previewConfiguration->rows);
        w.layoutMetrics = snowdesktop::widget_runtime::
            NormalizeLayoutMetrics(
                previewConfiguration->cellWidth,
                previewConfiguration->cellHeight,
                previewConfiguration->gap,
                previewConfiguration->barHeight,
                previewConfiguration->fontWeight,
                CalculateWidgetCellScale(
                    previewConfiguration->cellWidth,
                    previewConfiguration->cellHeight));
        w.previewDataState = previewConfiguration->dataState;
    }
    else if (d2dState_)
    {
        w.layoutMetrics = snowdesktop::widget_runtime::
            CaptureLayoutMetrics(*d2dState_);
    }
    w.scriptSettings = std::move(scriptSettings);
    w.scriptSettingGroups = std::move(scriptSettingGroups);
    w.scriptPresets = std::move(scriptPresets);
    w.preview = preview;
    w.previewStorage = std::move(pending.previewStorage);
    if (!preview && !snowdesktop::widget_runtime::HasStorageOverlay())
    {
        bool removedPlaintextOpaqueSetting = false;
        const auto purgeOpaqueSettingValues = [&](const auto& settings) {
            for (const auto& setting : settings)
            {
                if (setting.type != "password" &&
                    !IsFilesystemHandleSettingType(setting.type) &&
                    !IsEntityReferenceSettingType(setting.type))
                    continue;
                removedPlaintextOpaqueSetting =
                    MutablePersistentStorage().erase(storagePrefix + setting.key) > 0 ||
                    removedPlaintextOpaqueSetting;
                removedPlaintextOpaqueSetting =
                    MutablePersistentStorage().erase(storagePrefix +
                        snowdesktop::widget_runtime::
                            TypedStorageMetadataKey(setting.key)) > 0 ||
                    removedPlaintextOpaqueSetting;
            }
        };
        purgeOpaqueSettingValues(w.manifest.settings);
        purgeOpaqueSettingValues(w.scriptSettings);
        if (removedPlaintextOpaqueSetting) SaveStorageFile();
    }
    w.logicalSlots = std::move(pending.logicalSlots);
    for (const auto& snapshot : w.logicalSlots.Snapshots())
    {
        for (const auto& item : snapshot.items)
        {
            if (item.kind == "app.reference")
            {
                w.applicationReferences.insert_or_assign(item.reference,
                    LuaWidget::ApplicationReference{ {}, item.target, 0,
                        item.title, item.source, item.type, true,
                        item.available });
            }
            else
            {
                w.itemReferences.insert_or_assign(item.reference,
                    LuaWidget::ItemReference{ item.target, "logical.slot",
                        0, item.title, item.source, item.type,
                        item.kind, true, item.available });
            }
        }
    }
    (void)w.namedTimers.SetVisible(false,
        snowdesktop::widget_runtime::NamedTimerSchedule::Clock::now());
    (void)w.animationFrames.SetVisible(false);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
    widgets_.push_back(std::move(w));
    if (!InitializeWidgetLifecycle(widgets_.back()))
    {
        LuaWidget& failed = widgets_.back();
        ReleaseWidgetDataSubscriptions(failed);
        ReleaseWidgetTasks(failed,
            snowdesktop::widget_runtime::TaskBrokerCancelReason::InstanceDisposed);
        if (failed.state)
        {
            failed.lifecycle.Release(failed.state);
            luaL_unref(failed.state, LUA_REGISTRYINDEX, failed.ref);
            ReleasePackageImageResources(d2dState_, failed.state);
            lua_close(failed.state);
            failed.state = nullptr;
        }
        widgets_.pop_back();
        return false;
    }
    widgetHostFailures_.erase(widgetId);

    if (!preview && !snowdesktop::widget_runtime::IsDryLoad() && d2dState_)
    {
        // Only warm the filesystem cache. RenderWidget still checks the current
        // script synchronously, including deletion and failed-reload recovery.
        try
        {
            (void)d2dState_->scriptReadAhead.Track(widgetId, path,
                (widgets_.back().packageRoot / L"widget.json").wstring(),
                GetWidgetPackageManager().Paths().installed.wstring());
        }
        catch (...) { /* Optional path preparation must not fail the VM load. */ }
    }

    auto& loadedStorage = ActiveStorage();
    const std::string staleErrorKey =
        WidgetWideToUtf8(widgetId) + ".lastError";
    if (loadedStorage.erase(staleErrorKey) > 0 &&
        !snowdesktop::widget_runtime::HasStorageOverlay())
    {
        SaveStorageFile();
    }

    if (!preview && !snowdesktop::widget_runtime::IsDryLoad() &&
        widgets_.back().manifest.refreshIntervalMs > 0 &&
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
    RestoreNotificationSchedules(widgets_.back());
    RefreshLogicalSlotAvailability();
    return true;
}

void WidgetEngine::RenderAll(ID2D1DeviceContext* context)
{
    d2dState_->ctx = context;
}

// Host-controlled visuals have their own effective preference. The public Lua
// system accessibility snapshot and animation.request semantics stay intact.
static bool HostManagedAnimationsSuppressed() noexcept
{
    return !snowdesktop::animation::RuntimeAnimationsEnabled();
}

namespace
{
using snowdesktop::widget_runtime::DrawImageFit;
using snowdesktop::widget_runtime::PackageImageSize;
using snowdesktop::widget_runtime::ViewNode;
using snowdesktop::widget_runtime::ViewThemePalette;
using snowdesktop::widget_runtime::ViewTransitionRuntime;
using snowdesktop::widget_runtime::WidgetInteractionRegions;
using snowdesktop::widget_runtime::WidgetPanelContentClip;
using snowdesktop::widget_runtime::WidgetViewHost;

ViewThemePalette BuildWidgetViewThemePalette(D2DState* state,
    const std::wstring& widgetId)
{
    const WidgetSystemEnvironment system = QueryWidgetSystemEnvironment();
    const LuaWidgetTheme theme = state && state->engine
        ? state->engine->RuntimeGetWidgetTheme(widgetId)
        : LuaWidgetTheme{};
    return snowdesktop::widget_runtime::BuildWidgetViewThemePalette(
        theme, system.highContrast, system.accentColor);
}

class EngineWidgetViewHost final : public WidgetViewHost
{
public:
    explicit EngineWidgetViewHost(D2DState* state) : state_(state) {}

    ViewThemePalette ThemePalette() const override
    {
        return BuildWidgetViewThemePalette(state_, state_->currentWidgetId);
    }

    std::optional<std::wstring> ResolveFont(std::string_view name) const override
    {
        return state_->engine ? state_->engine->RuntimeResolvePackageResource(
            state_->currentWidgetId, name, "font") : std::nullopt;
    }

    bool ReadFocusedInput(const std::string& key, std::wstring& text,
        std::size_t& cursor, std::size_t& selectionAnchor,
        std::wstring& composition, std::size_t& compositionCursor) const override
    {
        return state_->engine && state_->engine->RuntimeGetFocusedHostInput(
            state_->currentWidgetId, key, text, cursor, selectionAnchor,
            composition, compositionCursor);
    }

    bool IsSelected() const override
    {
        return state_->engine &&
            state_->engine->RuntimeIsWidgetSelected(state_->currentWidgetId);
    }

    int InputScrollOffset(const std::string& key) const override
    {
        return state_->engine
            ? state_->engine->RuntimeGetScrollOffset(state_->currentWidgetId, key)
            : 0;
    }

    ID2D1Bitmap1* LoadImage(std::string_view name, float width, float height,
        DrawImageFit fit, bool nearest, PackageImageSize& original) const override
    {
        if (!state_->engine) return nullptr;
        if (snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(name))
            return LoadRuntimeImageBitmap(state_, state_->currentWidgetId, name);
        const auto contentKey = state_->engine->RuntimeResolvePackageImageContentKey(
            state_->currentWidgetId, name);
        return contentKey ? LoadImageBitmap(state_, *contentKey,
            width, height, fit, nearest, &original) : nullptr;
    }

    std::optional<std::wstring> ResolveReference(const std::string& reference) const override
    {
        return state_->engine ? state_->engine->RuntimeResolveViewReference(
            state_->currentWidgetId, reference) : std::nullopt;
    }

    bool IsPreview() const override
    {
        return state_->engine && state_->engine->IsPreviewOnly();
    }

    ID2D1Bitmap1* LoadShellIcon(const std::wstring& path) const override
    {
        EnsureBitmapCachesForCurrentDevice(state_);
        if (state_->shellIconFailures.contains(path)) return nullptr;
        if (const auto cached = state_->shellIconCache.find(path);
            cached != state_->shellIconCache.end())
            return cached->second.Get();
        if (state_->shellIconLoader)
            state_->shellIconLoader->Request(path, state_->currentWidgetId);
        return nullptr;
    }

private:
    D2DState* state_;
};

bool DrawWidgetViewTree(D2DState* state, const ViewNode& tree,
    const WidgetInteractionRegions& regions, std::string_view focusedKey,
    ViewTransitionRuntime& transitions, bool reducedMotion,
    WidgetPanelContentClip* contentClip = nullptr)
{
    return snowdesktop::widget_runtime::DrawWidgetViewTree(state,
        EngineWidgetViewHost(state), tree, regions, focusedKey, transitions,
        reducedMotion, contentClip);
}

void DrawWidgetSelectOverlays(D2DState* state, const ViewNode& node,
    const WidgetInteractionRegions& regions, std::string_view focusedKey,
    float viewportHeight)
{
    snowdesktop::widget_runtime::DrawWidgetSelectOverlays(
        state, EngineWidgetViewHost(state), node, regions, focusedKey, viewportHeight);
}

void DrawWidgetViewTooltip(D2DState* state, const WidgetInteractionRegions& regions)
{
    if (!state || !state->ctx || !state->dwrite) return;
    const auto context = state->engine
        ? state->engine->RuntimeGetWidgetContextState(state->currentWidgetId)
        : LuaWidgetContextState{};
    if (state->engine && state->engine->UsesHostedInteractionTooltips() && !context.preview)
        return;
    const float scale = static_cast<float>(context.surface.dpiX) / 96.f;
    snowdesktop::widget_runtime::DrawWidgetViewTooltip(state, regions, scale);
}
}

bool WidgetEngine::MeasureHoveredInteractionTooltip(const std::wstring& widgetId,
    std::string_view surface, RECT frame, snowdesktop::NativeTooltipTextLayout& measured,
    D2D1_RECT_F& bounds, std::wstring& name) const
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !d2dState_ || !d2dState_->dwrite) return false;
    const auto& widget = widgets_[index];
    if (!widget.valid || widget.preview || (IsPanelSurface(surface)
        ? (!widget.panelActive || widget.panelSurface != surface)
        : !widget.desktopVisible)) return false;
    const auto& regions = InteractionRegionsForSurface(widget, surface);
    const auto context = RuntimeGetWidgetContextState(widgetId);
    const float scale = static_cast<float>(context.surface.dpiX) / 96.f;
    return snowdesktop::widget_runtime::MeasureWidgetViewTooltip(d2dState_->dwrite,
        D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
            static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
        regions, scale, measured, bounds, name);
}

static LuaWidget::HostControl BuildViewScrollHostControl(
    const snowdesktop::widget_runtime::ViewScrollViewport& viewport, bool panel)
{
    LuaWidget::HostControl control;
    control.type = LuaWidget::HostControl::Type::Scroll;
    control.id = viewport.key;
    control.rect = {
        static_cast<LONG>(std::lround(viewport.frame.x)),
        static_cast<LONG>(std::lround(viewport.frame.y)),
        static_cast<LONG>(std::lround(
            viewport.frame.x + viewport.frame.width)),
        static_cast<LONG>(std::lround(
            viewport.frame.y + viewport.frame.height)),
    };
    if (panel)
    {
        control.selectPopup = viewport.selectPopup;
        control.initialScrollOffset =
            static_cast<int>(std::lround(viewport.offset));
    }
    if (panel && viewport.scrollbarFrame)
    {
        const auto& track = *viewport.scrollbarFrame;
        control.rect.right = static_cast<LONG>(std::lround(
            track.x + track.width));
        control.rect.bottom = static_cast<LONG>(std::lround(
            track.y + track.height));
    }
    control.horizontal = viewport.orientation ==
        snowdesktop::widget_runtime::ViewOrientation::Horizontal;
    const int viewportExtent = std::max(1,
        static_cast<int>(std::lround(viewport.viewportExtent)));
    const int contentExtent = std::max(viewportExtent,
        static_cast<int>(std::lround(viewport.contentExtent)));
    control.viewportWidth = std::max(1,
        static_cast<int>(std::lround(viewport.frame.width)));
    control.contentWidth = control.viewportWidth;
    control.viewportHeight = std::max(1,
        static_cast<int>(std::lround(viewport.frame.height)));
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
    return control;
}

static LuaWidget::HostControl BuildViewInputHostControl(
    WidgetEngine& engine, D2DState* state, const std::wstring& widgetId,
    const snowdesktop::widget_runtime::ViewInputControl& input)
{
    LuaWidget::HostControl control;
    control.type = LuaWidget::HostControl::Type::Input;
    control.id = input.key;
    control.rect = {
        static_cast<LONG>(std::lround(input.frame.x)),
        static_cast<LONG>(std::lround(input.frame.y)),
        static_cast<LONG>(std::lround(
            input.frame.x + input.frame.width)),
        static_cast<LONG>(std::lround(
            input.frame.y + input.frame.height)),
    };
    if (input.clip)
    {
        control.clipRect = RECT{
            static_cast<LONG>(std::lround(input.clip->x)),
            static_cast<LONG>(std::lround(input.clip->y)),
            static_cast<LONG>(std::lround(
                input.clip->x + input.clip->width)),
            static_cast<LONG>(std::lround(
                input.clip->y + input.clip->height)),
        };
    }
    control.enabled = input.enabled;
    control.focusable = input.focusable;
    control.readOnly = input.readOnly;
    control.controlled = true;
    control.numeric = input.type ==
        snowdesktop::widget_runtime::ViewNodeType::NumberInput;
    control.controlledText = input.value;
    control.placeholder = input.placeholder;
    control.changeAction = input.changeAction;
    control.selection = input.selection;
    control.selectionChangeAction = input.selectionChangeAction;
    control.focusAction = input.focusAction;
    control.blurAction = input.blurAction;
    control.submitAction = input.submitAction;
    control.selectAll = input.selectAll;
    control.liveUpdate = input.liveUpdate;
    control.multiline = input.type ==
        snowdesktop::widget_runtime::ViewNodeType::TextArea;
    control.fontSize = input.fontSize;
    control.padding = input.padding;
    control.maximumUtf8Bytes = input.maximumUtf8Bytes == 0
        ? snowdesktop::widget_runtime::ViewTreeLimits::MaximumTextBytes
        : input.maximumUtf8Bytes;
    control.minimum = input.minimum;
    control.maximum = input.maximum;
    control.step = input.step;
    control.viewportHeight = std::max(1,
        static_cast<int>(std::lround(input.frame.height)));
    control.contentHeight = control.viewportHeight;
    if (control.multiline)
    {
        const float innerWidth = std::max(1.0f,
            input.frame.width - input.padding.left -
                input.padding.right - 8.0f);
        std::wstring metricText = Utf8ToWideLocal(input.value);
        size_t metricCursor = 0;
        size_t metricAnchor = 0;
        std::wstring metricComposition;
        size_t metricCompositionCursor = 0;
        (void)engine.RuntimeGetFocusedHostInput(widgetId,
            input.key, metricText, metricCursor, metricAnchor,
            metricComposition, metricCompositionCursor);
        ComPtr<IDWriteTextLayout> layout =
            CreateHostMultilineTextLayout(state,
                metricText, input.fontSize, innerWidth);
        DWRITE_TEXT_METRICS metrics{};
        if (layout) layout->GetMetrics(&metrics);
        control.contentHeight = std::max(control.viewportHeight,
            static_cast<int>(std::ceil(metrics.height +
                input.padding.top + input.padding.bottom)));
    }
    return control;
}

static std::vector<LuaWidget::HostControl> BuildPanelViewHostControls(
    WidgetEngine& engine, D2DState* state, const std::wstring& widgetId,
    const snowdesktop::widget_runtime::WidgetViewFrame& frame)
{
    std::vector<LuaWidget::HostControl> result;
    result.reserve(frame.scrollViewports.size() + frame.inputControls.size());
    for (const auto& viewport : frame.scrollViewports)
        result.push_back(BuildViewScrollHostControl(viewport, true));
    for (const auto& input : frame.inputControls)
        result.push_back(BuildViewInputHostControl(engine, state, widgetId, input));
    return result;
}

static bool WidgetDeclaresFeature(
    const LuaWidget& widget, std::string_view feature)
{
    const auto contains = [feature](const std::vector<std::string>& values) {
        return std::any_of(values.begin(), values.end(),
            [feature](const std::string& value) {
                return value == feature;
            });
    };
    return contains(widget.manifest.requiredFeatures) ||
        contains(widget.manifest.optionalFeatures);
}

static bool WidgetDeclaresNativeMarquee(const LuaWidget& widget)
{
    return WidgetDeclaresFeature(widget, "draw.marqueeText");
}

static bool HasActiveNativeMarquee(
    const LuaWidget::NativeMarqueeSurface& surface) noexcept
{
    return std::any_of(surface.marquees.begin(), surface.marquees.end(),
        [](const LuaWidget::NativeMarqueeText& marquee) {
            return marquee.scrolling;
        });
}

static std::optional<RECT> NativeMarqueeDirtyRect(
    const LuaWidget::NativeMarqueeSurface& surface)
{
    std::optional<RECT> result;
    for (const auto& marquee : surface.marquees)
    {
        if (!marquee.scrolling) continue;
        RECT current{
            static_cast<LONG>(std::floor(marquee.viewport.left)),
            static_cast<LONG>(std::floor(marquee.viewport.top)),
            static_cast<LONG>(std::ceil(marquee.viewport.right)),
            static_cast<LONG>(std::ceil(marquee.viewport.bottom)),
        };
        if (IsRectEmpty(&current)) continue;
        if (!result)
            result = current;
        else
        {
            RECT combined{};
            UnionRect(&combined, &*result, &current);
            result = combined;
        }
    }
    return result;
}

static bool BeginNativeMarqueeCapture(D2DState* state,
    LuaWidget::NativeMarqueeSurface& surface,
    snowdesktop::widget_runtime::WidgetCommandCapture& capture)
{
    surface.pendingMarquees.clear();
    surface.collecting = false;
    surface.requiresLuaRender = true;
    if (!state || !state->ctx) return false;
    EnsureBitmapCachesForCurrentDevice(state);
    using Result = snowdesktop::widget_runtime::WidgetCommandCapture::BeginResult;
    if (capture.Begin(*state, state->immediateClipRects,
            state->bitmapDevice.Get(), state->immediateCommandContext) != Result::Started)
        return false;
    surface.collecting = true;
    surface.requiresLuaRender = false;
    return true;
}

static void CommitNativeMarqueeCapture(D2DState* state,
    LuaWidget::NativeMarqueeSurface& surface,
    ComPtr<ID2D1CommandList> commands)
{
    for (auto& pending : surface.pendingMarquees)
    {
        const auto previous = std::find_if(surface.marquees.begin(),
            surface.marquees.end(), [&pending](const auto& candidate) {
                return candidate.key == pending.key;
            });
        const float cycle = pending.textWidth + pending.gap;
        if (pending.scrolling && previous != surface.marquees.end() &&
            std::isfinite(previous->offset) && cycle > 0.0f)
            pending.offset = std::fmod(previous->offset, cycle);
    }
    surface.marquees = std::move(surface.pendingMarquees);
    surface.pendingMarquees.clear();
    surface.staticCommands = std::move(commands);
    surface.commandDevice.Reset();
    if (state) surface.commandDevice = state->bitmapDevice;
    surface.lastAdvance = std::chrono::steady_clock::now();
    surface.framePending = false;
}

static void DrawNativeMarqueeSurface(D2DState* state,
    const LuaWidget::NativeMarqueeSurface& surface,
    bool reducedMotion, bool drawMarquees = true)
{
    if (!state || !state->ctx) return;
    if (surface.staticCommands)
        state->ctx->DrawImage(surface.staticCommands.Get());
    if (!drawMarquees) return;
    for (const auto& marquee : surface.marquees)
    {
        if (!marquee.layout ||
            marquee.viewport.right <= marquee.viewport.left ||
            marquee.viewport.bottom <= marquee.viewport.top)
            continue;
        auto* brush = GetCachedBrush(
            state, marquee.color, marquee.alpha);
        if (!brush) continue;
        state->ctx->PushAxisAlignedClip(marquee.viewport,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float offset = marquee.scrolling && !reducedMotion
            ? marquee.offset : 0.0f;
        const float firstX = marquee.originX - offset;
        state->ctx->DrawTextLayout(D2D1::Point2F(
                firstX, marquee.originY),
            marquee.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (marquee.scrolling && !reducedMotion)
        {
            state->ctx->DrawTextLayout(D2D1::Point2F(
                    firstX + marquee.textWidth + marquee.gap,
                    marquee.originY),
                marquee.layout.Get(), brush,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        state->ctx->PopAxisAlignedClip();
    }
}

static bool AdvanceNativeMarqueeSurface(
    LuaWidget::NativeMarqueeSurface& surface,
    std::chrono::steady_clock::time_point now)
{
    if (surface.compositionManaged || !HasActiveNativeMarquee(surface))
        return false;
    const float deltaMilliseconds = surface.lastAdvance.time_since_epoch().
            count() == 0
        ? 0.0f
        : static_cast<float>(std::chrono::duration_cast<
            std::chrono::microseconds>(now - surface.lastAdvance).count()) /
            1000.0f;
    surface.lastAdvance = now;
    for (auto& marquee : surface.marquees)
    {
        if (!marquee.scrolling) continue;
        marquee.offset = snowdesktop::widget_runtime::
            AdvanceDrawMarqueeOffset(marquee.offset, deltaMilliseconds,
                marquee.speed / static_cast<float>(snowdesktop::animation::RuntimeDurationScale()),
                marquee.textWidth + marquee.gap);
    }
    surface.framePending = true;
    return true;
}

static void ConfigureDesktopContentLayout(D2DState* state,
    lua_State* lua, int descriptorIndex)
{
    if (!state || !lua) return;
    lua_getfield(lua, descriptorIndex, "showTitle");
    const bool showTitle = !lua_isnil(lua, -1) &&
        lua_toboolean(lua, -1) != 0;
    lua_pop(lua, 1);
    lua_getfield(lua, descriptorIndex, "bottomBarHover");
    const bool bottomBarHover = lua_isnil(lua, -1) ||
        lua_toboolean(lua, -1) != 0;
    lua_pop(lua, 1);
    const int scaledBarHeight = static_cast<int>(std::round(
        static_cast<float>(state->barHeight) *
        CalculateWidgetCellScale(
            std::max(4, state->gridCellW),
            std::max(4, state->gridCellH))));
    const int reservedBarHeight =
        snowdesktop::widget_chrome_rules::ReservedBottomBarHeight(
            showTitle, bottomBarHover, scaledBarHeight);
    state->layoutContentWidth = std::max(1.0f,
        state->widgetRect.right - state->widgetRect.left);
    state->layoutContentHeight = std::max(1.0f,
        state->widgetRect.bottom - state->widgetRect.top -
            static_cast<float>(reservedBarHeight));
}

bool WidgetEngine::RenderWidgetBackgroundLayer(
    const std::wstring& widgetId, ID2D1DeviceContext* context,
    RECT bounds, int columns, int rows, float inheritedBlurRadius,
    float cornerRadius)
{
    snowdesktop::performance::Scope performanceScope("widget.render", "backgroundLayer", widgetId);
    const int index = FindWidget(widgetId);
    if (index < 0 || !context || IsRectEmpty(&bounds)) return false;
    LuaWidget& widget = widgets_[index];
    if (!widget.valid || !widget.state) return false;

    PreviewExecutionScope previewScope(
        widget.preview ? &widget.previewStorage : nullptr);
    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");
    BeginImageFrame(d2dState_, true);
    snowdesktop::lua_runtime::StackGuard stackGuard(widget.state);
    lua_State* state = widget.state;
    d2dState_->ctx = context;
    d2dState_->gridColumns = std::max(1, columns);
    d2dState_->gridRows = std::max(1, rows);
    SetWidgetRectContext(d2dState_, bounds);

    auto recordError = [&](std::string message) {
        d2dState_->backgroundCache.Erase(widgetId);
        if (message.size() > 4096) message.resize(4096);
        if (widget.backgroundLayerError == message) return;
        widget.backgroundLayerError = message;
        RuntimeAddLog(widgetId, "error", message);
    };

    lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
    if (!lua_istable(state, -1)) return false;
    const int descriptor = lua_absindex(state, -1);
    ConfigureDesktopContentLayout(d2dState_, state, descriptor);
    lua_getfield(state, descriptor, "backgroundLayer");
    if (lua_isnil(state, -1))
    {
        widget.backgroundLayerError.clear();
        d2dState_->backgroundCache.Erase(widgetId);
        return false;
    }
    if (!lua_istable(state, -1))
    {
        recordError("backgroundLayer must remain a table while rendering");
        return false;
    }
    const int background = lua_absindex(state, -1);

    float opacity = 1.0f;
    lua_getfield(state, background, "opacity");
    if (!lua_isnil(state, -1))
    {
        const double value = lua_tonumber(state, -1);
        if (lua_type(state, -1) != LUA_TNUMBER || !std::isfinite(value) ||
            value < 0.0 || value > 1.0)
        {
            recordError(
                "backgroundLayer.opacity must be finite and between 0 and 1");
            return false;
        }
        opacity = static_cast<float>(value);
    }
    lua_pop(state, 1);

    float blurRadius = std::clamp(
        inheritedBlurRadius, 0.0f, 48.0f);
    lua_getfield(state, background, "blurRadius");
    if (!lua_isnil(state, -1))
    {
        const double value = lua_tonumber(state, -1);
        if (lua_type(state, -1) != LUA_TNUMBER || !std::isfinite(value) ||
            value < 0.0 || value > 48.0)
        {
            recordError(
                "backgroundLayer.blurRadius must be finite and between 0 and 48");
            return false;
        }
        blurRadius = static_cast<float>(value);
    }
    lua_pop(state, 1);

    lua_getfield(state, background, "render");
    if (!lua_isfunction(state, -1))
    {
        recordError("backgroundLayer.render must remain a function");
        return false;
    }
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (!widget.lifecycle.PushRenderArguments(state, pushContext))
    {
        recordError("backgroundLayer lifecycle is not initialized");
        return false;
    }

    EnsureBitmapCachesForCurrentDevice(d2dState_);
    snowdesktop::widget_runtime::WidgetCommandCapture capture;
    using CaptureResult = snowdesktop::widget_runtime::WidgetCommandCapture::BeginResult;
    switch (capture.Begin(*d2dState_, d2dState_->immediateClipRects,
            d2dState_->bitmapDevice.Get(), d2dState_->immediateCommandContext))
    {
    case CaptureResult::NoDevice:
        recordError("backgroundLayer cannot access the Direct2D device");
        return false;
    case CaptureResult::NoRecordingContext:
        recordError("backgroundLayer cannot create a recording context");
        return false;
    case CaptureResult::NoCommandList:
        recordError("backgroundLayer cannot create a command list");
        return false;
    case CaptureResult::Started:
        break;
    }
    d2dState_->backgroundLayerActive = true;

    const int callResult = snowdesktop::lua_runtime::ProtectedCall(
        state, 2, 0);
    std::string callbackError;
    if (callResult != LUA_OK)
    {
        const char* value = lua_tostring(state, -1);
        callbackError = value ? value : "backgroundLayer render failed";
        lua_pop(state, 1);
    }
    const HRESULT drawResult = capture.FinishDraw();
    d2dState_->backgroundLayerActive = false;
    const HRESULT closeResult = capture.Close();
    if (!callbackError.empty())
    {
        recordError("backgroundLayer.render: " + callbackError);
        return false;
    }
    if (FAILED(drawResult) || FAILED(closeResult))
    {
        recordError("backgroundLayer command recording failed");
        return false;
    }

    const auto cachedBackground = d2dState_->backgroundCache.Resolve(widgetId,
        context, capture.Commands(), D2D1::RectF(static_cast<float>(bounds.left),
            static_cast<float>(bounds.top), static_cast<float>(bounds.right),
            static_cast<float>(bounds.bottom)), blurRadius, cornerRadius,
        [this](ID2D1Bitmap* bitmap) {
            // These caches create immutable decoded bitmaps. Arbitrary target
            // bitmaps or effect inputs must never enter the signature cache.
            const auto contains = [bitmap](const auto& cache) {
                return std::any_of(cache.begin(), cache.end(), [&](const auto& item) {
                    return static_cast<ID2D1Bitmap*>(item.second.Get()) == bitmap;
                });
            };
            return contains(d2dState_->imageCache) || contains(d2dState_->runtimeImageBitmaps) ||
                d2dState_->imageSamplingCache.Contains(bitmap);
        });
    using CacheOutcome = snowdesktop::widget_runtime::WidgetBackgroundCache::Outcome;
    snowdesktop::performance::Value("widget.background.cache",
        cachedBackground.outcome == CacheOutcome::Hit ? "hit" :
            cachedBackground.outcome == CacheOutcome::Miss ? "miss" : "bypass", widgetId, 1);
    snowdesktop::performance::Value("widget.background.cache", "retained_bytes_estimate", {},
        static_cast<double>(d2dState_->backgroundCache.RetainedBytes()));
    if (cachedBackground.bitmap)
    {
        if (auto* paint = d2dState_->desktopSurfacePaint)
        {
            paint->backgroundKnown = true;
            paint->background = cachedBackground.bitmap;
            paint->backgroundOpacity = opacity;
        }
        // Cached pixels already contain the rounded mask. Applying opacity to
        // the premultiplied bitmap preserves the group's opacity semantics.
        context->DrawBitmap(cachedBackground.bitmap.Get(),
            D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)),
            opacity, D2D1_INTERPOLATION_MODE_LINEAR);
        widget.backgroundLayerError.clear();
        return true;
    }
    if (auto* paint = d2dState_->desktopSurfacePaint)
    {
        D2D1_RECT_F ink{};
        paint->backgroundKnown =
            SUCCEEDED(context->GetImageLocalBounds(capture.Commands(), &ink)) &&
            (ink.left >= ink.right || ink.top >= ink.bottom);
    }
    ComPtr<ID2D1Image> output;
    if (FAILED(capture.Commands()->QueryInterface(IID_PPV_ARGS(&output))) || !output)
    {
        recordError("backgroundLayer cannot expose its command image");
        return false;
    }
    ComPtr<ID2D1Effect> blur;
    if (!cachedBackground.bitmap && blurRadius > 0.0f)
    {
        if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) ||
            !blur)
        {
            recordError("backgroundLayer cannot create the blur effect");
            return false;
        }
        blur->SetInput(0, capture.Commands());
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
            blurRadius);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
            D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
            D2D1_BORDER_MODE_HARD);
        blur->GetOutput(&output);
        if (!output)
        {
            recordError("backgroundLayer blur did not expose an image");
            return false;
        }
    }

    ComPtr<ID2D1Factory> factory;
    context->GetFactory(&factory);
    const D2D1_RECT_F frame = D2D1::RectF(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top),
        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
    const auto clip = snowdesktop::smooth_corners::Geometry(factory.Get(), frame, cornerRadius);
    if (!clip)
    {
        recordError("backgroundLayer cannot create the rounded clip");
        return false;
    }
    context->PushLayer(D2D1::LayerParameters(frame, clip.Get(),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
        D2D1::Matrix3x2F::Identity(), opacity), nullptr);
    context->DrawImage(output.Get());
    context->PopLayer();
    widget.backgroundLayerError.clear();
    return true;
}

void WidgetEngine::RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath,
    ID2D1DeviceContext* context, RECT bounds, int columns, int rows)
{
    snowdesktop::performance::Scope performanceScope("widget.render", "desktop", widgetId);
    // A stack is a surface owner; invalidation belongs to the active Lua VM.
    snowdesktop::performance::DrawLink("desktop", widgetId, true);
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
    const RECT previousBounds = found->lastBounds;
    const auto previousPaintRevision = std::exchange(found->desktopPaintRevision, 0);
    auto* surfacePaint = d2dState_->desktopSurfacePaint;
    const bool resumeAfterOcclusion = std::exchange(found->desktopOcclusionResumePending, false);
    const int previousColumns = found->lastColumns;
    const int previousRows = found->lastRows;
    found->lastBounds = bounds;
    d2dState_->gridColumns = std::max(1, columns);
    d2dState_->gridRows = std::max(1, rows);
    SetWidgetRectContext(d2dState_, bounds);

    found->desktopVisible = true;
    found->keepRuntimeActiveForHiddenPage = false;
    ApplyWidgetHostVisibility(*found, true);
    if (found->lastColumns != columns || found->lastRows != rows)
    {
        found->lastColumns = std::max(1, columns);
        found->lastRows = std::max(1, rows);
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
    const bool transitionFrameRequested =
        std::exchange(found->viewTransitionFramePending, false);
    const bool transitionGeometryUnchanged =
        EqualRect(&previousBounds, &bounds) != FALSE &&
        previousColumns == columns && previousRows == rows;
    if (!resumeAfterOcclusion && transitionFrameRequested && transitionGeometryUnchanged &&
        found->viewTree)
    {
        BeginImageFrame(d2dState_);
        d2dState_->widgetClipDepth = 0;
        const bool transitionActive = DrawWidgetViewTree(
            d2dState_, *found->viewTree, found->interactionRegions,
            VisualViewFocusForSurface(*found, "desktop"),
            found->viewTransitions,
            found->preview || !widgetTimerRequestCallback_ ||
                HostManagedAnimationsSuppressed());
        DrawWidgetSelectOverlays(d2dState_, *found->viewTree,
            found->interactionRegions,
            VisualViewFocusForSurface(*found, "desktop"),
            found->viewTree->frame.height);
        DrawHostViewInteractionOverlays(*found,
            found->interactionRegions,
            found->viewKeyboardFocusKey, true);
        DrawWidgetViewTooltip(d2dState_, found->interactionRegions);
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        if (transitionActive || found->viewIndeterminateProgressActive)
            (void)ScheduleAnimationFrame(*found);
        return;
    }
    const bool nativeMarqueeFrame =
        std::exchange(found->desktopMarquee.framePending, false);
    if (!resumeAfterOcclusion && nativeMarqueeFrame && transitionGeometryUnchanged &&
        !found->desktopMarquee.requiresLuaRender &&
        found->desktopMarquee.staticCommands)
    {
        EnsureBitmapCachesForCurrentDevice(d2dState_);
        if (found->desktopMarquee.commandDevice.Get() ==
                d2dState_->bitmapDevice.Get())
        {
            DrawNativeMarqueeSurface(d2dState_, found->desktopMarquee,
                found->preview || !widgetTimerRequestCallback_ ||
                    HostManagedAnimationsSuppressed());
            DrawHostViewInteractionOverlays(*found,
                found->interactionRegions,
                found->viewKeyboardFocusKey, true);
            DrawWidgetViewTooltip(d2dState_, found->interactionRegions);
            (void)ScheduleAnimationFrame(*found);
            return;
        }
        found->desktopMarquee.requiresLuaRender = true;
    }
    // A replayed command list still consumes its images. Replace demands only
    // when drawing the tree or recording a fresh Lua frame.
    if (resumeAfterOcclusion) (void)ScheduleAnimationFrame(*found);
    BeginImageFrame(d2dState_);
    snowdesktop::widget_runtime::HostControlFrame<LuaWidget::HostControl>
        hostControlFrame([this, ownerId = widgetId]() -> std::vector<LuaWidget::HostControl>* {
            const int currentIndex = FindWidget(ownerId);
            return currentIndex < 0 ? nullptr : &widgets_[currentIndex].hostControls;
        }, "desktop");
    found->interactionRegions.BeginFrame();
    d2dState_->widgetClipDepth = 0;

    lua_rawgeti(state, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(state, -1))
    {
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
    ConfigureDesktopContentLayout(d2dState_, state, descriptorIndex);

    lua_getfield(state, descriptorIndex, "view");
    if (lua_isfunction(state, -1))
    {
        const auto pushContext = +[](lua_State* lifecycleState) {
            (void)lua_WidgetContext(lifecycleState);
        };
        bool accepted = false;
        std::string viewError;
        std::string_view viewErrorCode;
        if (!found->lifecycle.PushRenderArguments(state, pushContext))
        {
            lua_pop(state, 1);
            viewError = "Widget lifecycle is not initialized";
            viewErrorCode = "view.lifecycle";
        }
        else if (snowdesktop::lua_runtime::ProtectedCall(
                state, 2, 1) != LUA_OK)
        {
            const char* error = lua_tostring(state, -1);
            viewError = error ? error : "Widget view evaluation failed";
            viewErrorCode = "view.evaluate";
            lua_pop(state, 1);
        }
        else
        {
            snowdesktop::widget_runtime::WidgetViewFrame frame;
            auto& candidate = frame.tree;
            const auto& scrollViewports = frame.scrollViewports;
            const auto& inputControls = frame.inputControls;
            viewErrorCode = snowdesktop::widget_runtime::PrepareWidgetViewFrame(
                state, -1, d2dState_, EngineWidgetViewHost(d2dState_),
                found->logicalSlots, found->scrollOffsets,
                [this, found](auto& tree, std::string& error) {
                    return ValidateAndLayoutWidgetView(tree, *found, "desktop",
                        CurrentLayoutContentWidth(d2dState_), CurrentLayoutContentHeight(d2dState_), error);
                },
                frame, viewError);
            if (!viewErrorCode.empty())
                viewError = "view(): " + viewError;
            else
            {
                accepted = frame.SubmitRegions(found->interactionRegions, viewError);
                if (!accepted)
                {
                    viewError = "view(): " + viewError;
                    viewErrorCode = "view.interaction";
                }
                const auto desktopControlCount = std::count_if(
                    found->hostControls.begin(), found->hostControls.end(),
                    [](const auto& control) {
                        return HostControlBelongsToSurface(
                            control, "desktop");
                    });
                if (accepted && desktopControlCount +
                        scrollViewports.size() + inputControls.size() > 128)
                {
                    accepted = false;
                    viewError = "view(): host control limit exceeded (128)";
                    viewErrorCode = "view.hostControls";
                }
                if (accepted)
                {
                    std::unordered_set<std::string> hostKeys;
                    for (const auto& control : found->hostControls)
                    {
                        if (HostControlBelongsToSurface(control, "desktop"))
                            hostKeys.insert(control.id);
                    }
                    for (const auto& viewport : scrollViewports)
                    {
                        if (!hostKeys.insert(viewport.key).second)
                        {
                            accepted = false;
                            viewError = "view(): duplicate host control key: " +
                                viewport.key;
                            viewErrorCode = "view.hostControls";
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
                                viewErrorCode = "view.hostControls";
                                break;
                            }
                        }
                    }
                }
                if (accepted)
                {
                    for (const auto& viewport : scrollViewports)
                        RuntimeRegisterHostControl(widgetId,
                            BuildViewScrollHostControl(viewport, false));
                    for (const auto& input : inputControls)
                        RuntimeRegisterHostControl(widgetId,
                            BuildViewInputHostControl(*this, d2dState_, widgetId, input));
                }
            }
            lua_pop(state, 1);
            if (accepted)
            {
                const int currentIndex = FindWidget(widgetId);
                if (currentIndex >= 0)
                {
                    found = &widgets_[currentIndex];
                    for (const auto& viewport : scrollViewports)
                    {
                        if (viewport.initialized)
                            found->scrollOffsets.emplace(viewport.key,
                                static_cast<int>(std::lround(
                                    viewport.offset)));
                    }
                    const bool variableLayoutChanged =
                        CommitVariableVirtualMeasurements(
                            candidate, *found, "desktop");
                    if (found->viewTree)
                    {
                        found->viewTransitions.SetDurationScale(
                            snowdesktop::animation::RuntimeDurationScale());
                        found->viewTransitions.CommitScene(
                            *found->viewTree, candidate,
                            snowdesktop::widget_runtime::
                                ViewTransitionRuntime::Clock::now(),
                            found->preview ||
                                !widgetTimerRequestCallback_ ||
                                HostManagedAnimationsSuppressed());
                    }
                    if (surfacePaint && HasLocalRefresh(candidate))
                    {
                        surfacePaint->revision = ++d2dState_->nextDesktopPaintRevision;
                        if (previousPaintRevision != 0 && found->viewTree &&
                            transitionGeometryUnchanged)
                        {
                            CollectLocalRefresh(*found->viewTree, candidate, bounds,
                                *surfacePaint);
                            if (candidate == *found->viewTree)
                                surfacePaint->baseRevision = previousPaintRevision;
                        }
                    }
                    found->viewTree = std::move(candidate);
                    found->viewIndeterminateProgressActive =
                        snowdesktop::widget_runtime::
                            HasVisibleIndeterminateProgress(
                                *found->viewTree);
                    if (variableLayoutChanged)
                        RuntimeInvalidateHost(widgetId);
                }
                else
                    accepted = false;
            }
        }

        if (!accepted)
        {
            hostControlFrame.Restore();
            found->interactionRegions.AbortFrame();
            if (!viewError.empty())
            {
                RuntimeRecordError(widgetId,
                    FormatViewValidationDiagnostic(
                        viewErrorCode, viewError));
                RuntimeInvalidateHost(widgetId);
            }
        }
        else
        {
            hostControlFrame.Commit();
            RuntimeRecordSuccess(widgetId);
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
            ReconcileHostViewKeyboardFocus(*found, "desktop");
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        if (found->viewTree)
        {
            const bool transitionActive = DrawWidgetViewTree(
                d2dState_, *found->viewTree,
                found->interactionRegions,
                VisualViewFocusForSurface(*found, "desktop"),
                found->viewTransitions,
                found->preview || !widgetTimerRequestCallback_ ||
                    HostManagedAnimationsSuppressed());
            DrawWidgetSelectOverlays(d2dState_, *found->viewTree,
                found->interactionRegions,
                VisualViewFocusForSurface(*found, "desktop"),
                found->viewTree->frame.height);
            if (!RuntimeIsWidgetSelected(found->widgetId) ||
                (!found->viewKeyboardFocusKey.empty() &&
                    !found->interactionRegions.IsKeyboardFocusable(
                        found->viewKeyboardFocusKey)))
            {
                found->viewKeyboardFocusKey.clear();
                found->viewFocusCueVisible = false;
                found->logicalSlotFocus.reset();
            }
            DrawHostViewInteractionOverlays(*found,
                found->interactionRegions,
                found->viewKeyboardFocusKey, true);
            DrawWidgetViewTooltip(
                d2dState_, found->interactionRegions);
            if (transitionActive ||
                found->viewIndeterminateProgressActive)
                (void)ScheduleAnimationFrame(*found);
            if (surfacePaint)
            {
                if (transitionActive || found->viewIndeterminateProgressActive ||
                    !found->interactionRegions.HoveredKey().empty() ||
                    !found->interactionRegions.PressedKey().empty() ||
                    !found->viewKeyboardFocusKey.empty())
                    surfacePaint->revision = surfacePaint->baseRevision = 0;
                found->desktopPaintRevision = surfacePaint->revision;
            }
        }
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        lua_pop(state, 1);
        return;
    }
    lua_pop(state, 1);

    lua_getfield(state, -1, "render");
    if (lua_isfunction(state, -1))
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
            RuntimeInvalidateHost(widgetId);
            found->interactionRegions.AbortFrame();
            return;
        }
        snowdesktop::widget_runtime::WidgetCommandCapture nativeMarqueeCapture;
        const bool capturingNativeMarquee =
            WidgetDeclaresNativeMarquee(*found) &&
            BeginNativeMarqueeCapture(d2dState_,
                found->desktopMarquee, nativeMarqueeCapture);
        if (snowdesktop::lua_runtime::ProtectedCall(
                state, 2, 0) != LUA_OK)
        {
            const char* err = lua_tostring(state, -1);
            if (capturingNativeMarquee)
            {
                nativeMarqueeCapture.Abort();
                found->desktopMarquee.pendingMarquees.clear();
                found->desktopMarquee.collecting = false;
                found->desktopMarquee.requiresLuaRender = true;
            }
            RuntimeRecordError(widgetId, err ? err : "(render error)");
            lua_pop(state, 1);
            snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);

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
                    snowdesktop::app_fonts::CreateTextFormat(d2dState_->dwrite, L"Segoe UI",
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
            if (const int currentIndex = FindWidget(widgetId);
                currentIndex >= 0)
            {
                widgets_[currentIndex].interactionRegions.AbortFrame();
            }
            lua_pop(state, 1);
            RuntimeInvalidateHost(widgetId);
            return;
        }
        if (capturingNativeMarquee)
        {
            const HRESULT drawResult = nativeMarqueeCapture.FinishDraw();
            found->desktopMarquee.collecting = false;
            if (FAILED(drawResult) || FAILED(nativeMarqueeCapture.Close()))
            {
                found->desktopMarquee.pendingMarquees.clear();
                found->desktopMarquee.requiresLuaRender = true;
                RuntimeRecordError(widgetId,
                    "draw.marqueeText: static draw cache could not be recorded");
                found->interactionRegions.AbortFrame();
                lua_pop(state, 1);
                RuntimeInvalidateHost(widgetId);
                return;
            }
            CommitNativeMarqueeCapture(d2dState_,
                found->desktopMarquee,
                nativeMarqueeCapture.TakeCommands());
            const bool reducedMotion = found->preview ||
                !widgetTimerRequestCallback_ ||
                HostManagedAnimationsSuppressed();
            const bool compositionManaged =
                SyncNativeMarqueeComposition(*found, reducedMotion);
            DrawNativeMarqueeSurface(d2dState_,
                found->desktopMarquee,
                reducedMotion, !compositionManaged);
            if (!compositionManaged &&
                HasActiveNativeMarquee(found->desktopMarquee))
                (void)ScheduleAnimationFrame(*found);
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
        hostControlFrame.Commit();
        RuntimeRecordSuccess(widgetId);
        if (const int currentIndex = FindWidget(widgetId);
            currentIndex >= 0)
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
            ReconcileHostViewKeyboardFocus(current, "desktop");
        }
    }
    else
    {
        hostControlFrame.Restore();
        found->interactionRegions.AbortFrame();
        lua_pop(state, 1);
    }
    snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
    if (const int currentIndex = FindWidget(widgetId); currentIndex >= 0)
    {
        auto& current = widgets_[currentIndex];
        if (!RuntimeIsWidgetSelected(current.widgetId) ||
            (!current.viewKeyboardFocusKey.empty() &&
                !current.interactionRegions.IsKeyboardFocusable(
                    current.viewKeyboardFocusKey)))
        {
            current.viewKeyboardFocusKey.clear();
            current.viewFocusCueVisible = false;
            current.logicalSlotFocus.reset();
        }
        DrawHostViewInteractionOverlays(current,
            current.interactionRegions,
            current.viewKeyboardFocusKey, true);
        // Immediate-mode regions carry the same tooltip contract as view nodes.
        // Paint after the component's clips and cached content have been restored.
        DrawWidgetViewTooltip(d2dState_, current.interactionRegions);
    }
    lua_pop(state, 1);
}

bool WidgetEngine::RenderWidgetPanel(
    const std::wstring& widgetId,
    ID2D1DeviceContext* context, RECT bounds,
    std::string_view surface, const RECT* scrollbarBounds)
{
    snowdesktop::performance::Scope performanceScope("widget.render", surface, widgetId);
    snowdesktop::performance::DrawLink(surface, widgetId, true);
    const int index = FindWidget(widgetId);
    if (index < 0 || !context)
        return false;
    auto& widget = widgets_[index];
    if (!widget.valid)
        return false;
    lua_State* state = widget.state;
    if (!state)
        return false;
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    if (!IsPanelSurface(normalizedSurface))
        return false;
    const std::string callbackName =
        normalizedSurface == "dialog" ? "dialog" :
        normalizedSurface == "popover" ? "popover" :
        "panel";

    WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
    WidgetSurfaceScope surfaceScope(
        d2dState_, normalizedSurface.c_str());
    BeginImageFrame(d2dState_);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    d2dState_->ctx = context;
    WidgetPanelContentClip contentClip(context, bounds);
    DrainShellIconResults(d2dState_);
    const RECT previousPanelBounds = widget.panelBounds;
    const std::string previousPanelSurface = widget.panelSurface;
    const bool panelWasActive = widget.panelActive;
    widget.panelBounds = bounds;
    widget.panelSurface = normalizedSurface;
    widget.panelActive = true;
    SetWidgetRectContext(d2dState_, bounds);
    ApplyWidgetHostVisibility(widget, true);
    const auto drawPanelView = [this, &normalizedSurface, &contentClip](LuaWidget& current) {
        const bool active = DrawWidgetViewTree(
            d2dState_, *current.panelViewTree, current.panelInteractionRegions,
            VisualViewFocusForSurface(current, normalizedSurface),
            current.panelViewTransitions, current.preview || !widgetTimerRequestCallback_ ||
                HostManagedAnimationsSuppressed(), &contentClip);
        DrawWidgetSelectOverlays(d2dState_, *current.panelViewTree,
            current.panelInteractionRegions,
            VisualViewFocusForSurface(current, normalizedSurface),
            current.panelViewTree->frame.height);
        DrawWidgetViewTooltip(d2dState_, current.panelInteractionRegions);
        return active;
    };
    const bool transitionFrameRequested =
        std::exchange(widget.panelViewTransitionFramePending, false);
    if (transitionFrameRequested && panelWasActive &&
        EqualRect(&previousPanelBounds, &bounds) != FALSE &&
        previousPanelSurface == normalizedSurface &&
        widget.panelViewTree)
    {
        d2dState_->widgetClipDepth = 0;
        const bool transitionActive = drawPanelView(widget);
        DrawHostViewInteractionOverlays(widget,
            widget.panelInteractionRegions,
            widget.panelViewKeyboardFocusKey, false);
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        if (transitionActive || widget.panelIndeterminateProgressActive)
            (void)ScheduleAnimationFrame(widget);
        return true;
    }
    std::vector<LuaWidget::HostControl> previousPanelControls;
    for (const auto& control : widget.hostControls)
    {
        if (HostControlBelongsToSurface(control, normalizedSurface))
            previousPanelControls.push_back(control);
    }
    std::erase_if(widget.hostControls, [&normalizedSurface](
            const auto& control) {
        return HostControlBelongsToSurface(
            control, normalizedSurface);
    });
    widget.panelInteractionRegions.BeginFrame();
    d2dState_->widgetClipDepth = 0;

    const auto restorePanelControls = [this, &widgetId,
        &previousPanelControls, &normalizedSurface]() {
        const int currentIndex = FindWidget(widgetId);
        if (currentIndex < 0) return;
        auto& controls = widgets_[currentIndex].hostControls;
        std::erase_if(controls, [&normalizedSurface](const auto& control) {
            return HostControlBelongsToSurface(
                control, normalizedSurface);
        });
        controls.insert(controls.end(), previousPanelControls.begin(),
            previousPanelControls.end());
    };

    lua_rawgeti(state, LUA_REGISTRYINDEX, widget.ref);
    if (!lua_istable(state, -1))
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        lua_pop(state, 1);
        return false;
    }
    lua_getfield(state, -1, callbackName.c_str());
    if (!lua_isfunction(state, -1))
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        lua_pop(state, 2);
        return false;
    }
    const auto pushContext = +[](lua_State* lifecycleState) {
        (void)lua_WidgetContext(lifecycleState);
    };
    if (!widget.lifecycle.PushRenderArguments(state, pushContext))
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        lua_pop(state, 2);
        RuntimeRecordError(widgetId, FormatViewValidationDiagnostic(
            "view.lifecycle",
            "Widget lifecycle is not initialized"));
        return false;
    }
    widget.panelFrameOpen = true;
    const bool succeeded = snowdesktop::lua_runtime::ProtectedCall(
        state, 2, 1) == LUA_OK;
    widget.panelFrameOpen = false;
    bool panelAccepted = succeeded;
    if (!succeeded)
    {
        widget.panelInteractionRegions.AbortFrame();
        restorePanelControls();
        const char* error = lua_tostring(state, -1);
        RuntimeRecordError(widgetId, FormatViewValidationDiagnostic(
            "view.evaluate", error ? std::string(error)
                : callbackName + " render error"));
        lua_pop(state, 1);
    }
    else
    {
        snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
        const bool hasView = !lua_isnil(state, -1);
        std::string viewError;
        std::string_view viewErrorCode;
        snowdesktop::widget_runtime::WidgetViewFrame frame;
        auto& candidate = frame.tree;
        auto& scrollViewports = frame.scrollViewports;
        if (hasView)
        {
            viewErrorCode = snowdesktop::widget_runtime::PrepareWidgetViewFrame(
                state, -1, d2dState_, EngineWidgetViewHost(d2dState_),
                widget.logicalSlots, widget.panelScrollOffsets,
                [this, &widget, &normalizedSurface](auto& tree, std::string& error) {
                    return ValidateAndLayoutWidgetView(tree, widget, normalizedSurface,
                        std::max(1.0f, d2dState_->widgetRect.right - d2dState_->widgetRect.left),
                        std::max(1.0f, d2dState_->widgetRect.bottom - d2dState_->widgetRect.top),
                        error);
                },
                frame, viewError);
            if (!viewErrorCode.empty())
            {
                panelAccepted = false;
                viewError = callbackName + "(): " + viewError;
            }
        }

        std::vector<LuaWidget::HostControl> viewControls;
        if (panelAccepted && hasView)
        {
            if (scrollbarBounds)
                snowdesktop::widget_runtime::ApplyViewSurfaceScrollbarGutters(
                    candidate, scrollViewports,
                    {0, 0, static_cast<float>(bounds.right - bounds.left),
                        static_cast<float>(bounds.bottom - bounds.top)},
                    {0, 0, static_cast<float>(scrollbarBounds->right - bounds.left),
                        static_cast<float>(scrollbarBounds->bottom - bounds.top)});
            viewControls = BuildPanelViewHostControls(*this, d2dState_, widgetId, frame);
            const std::size_t existingCount =
                static_cast<std::size_t>(std::count_if(
                    widget.hostControls.begin(), widget.hostControls.end(),
                    [&normalizedSurface](const auto& control) {
                        return HostControlBelongsToSurface(
                            control, normalizedSurface);
                    }));
            if (existingCount + viewControls.size() > 128)
            {
                panelAccepted = false;
                viewError =
                    callbackName +
                    "(): host control limit exceeded (128)";
                viewErrorCode = "view.hostControls";
            }
            std::unordered_set<std::string> keys;
            if (panelAccepted)
            {
                for (const auto& control : widget.hostControls)
                {
                    if (HostControlBelongsToSurface(
                            control, normalizedSurface))
                        keys.insert(control.id);
                }
                for (const auto& control : viewControls)
                {
                    if (!keys.insert(control.id).second)
                    {
                        panelAccepted = false;
                        viewError =
                            callbackName +
                            "(): duplicate host control key: " +
                            control.id;
                        viewErrorCode = "view.hostControls";
                        break;
                    }
                }
            }
            if (panelAccepted &&
                !frame.SubmitRegions(widget.panelInteractionRegions, viewError))
            {
                panelAccepted = false;
                viewError = callbackName + "(): " + viewError;
                viewErrorCode = "view.interaction";
            }
            if (panelAccepted)
            {
                for (auto& control : viewControls)
                    RuntimeRegisterHostControl(widgetId, std::move(control));
            }
        }
        lua_pop(state, 1);

        if (!panelAccepted)
        {
            widget.panelInteractionRegions.AbortFrame();
            restorePanelControls();
            if (!viewError.empty())
            {
                RuntimeRecordError(widgetId,
                    FormatViewValidationDiagnostic(
                        viewErrorCode, viewError));
            }
        }
        else
        {
            const int currentIndex = FindWidget(widgetId);
            if (currentIndex >= 0)
            {
                auto& current = widgets_[currentIndex];
                if (hasView)
                {
                    for (const auto& viewport : scrollViewports)
                    {
                        if (viewport.initialized)
                            current.panelScrollOffsets.emplace(viewport.key,
                                static_cast<int>(std::lround(
                                    viewport.offset)));
                    }
                    const bool variableLayoutChanged =
                        CommitVariableVirtualMeasurements(candidate,
                            current, normalizedSurface);
                    if (current.panelViewTree)
                    {
                        current.panelViewTransitions.SetDurationScale(
                            snowdesktop::animation::RuntimeDurationScale());
                        current.panelViewTransitions.CommitScene(
                            *current.panelViewTree, candidate,
                            snowdesktop::widget_runtime::
                                ViewTransitionRuntime::Clock::now(),
                            current.preview ||
                                !widgetTimerRequestCallback_ ||
                                HostManagedAnimationsSuppressed());
                    }
                    current.panelViewTree = std::move(candidate);
                    current.panelIndeterminateProgressActive =
                        snowdesktop::widget_runtime::
                            HasVisibleIndeterminateProgress(
                                *current.panelViewTree);
                    if (variableLayoutChanged)
                        RuntimeInvalidateHost(widgetId);
                }
                else
                {
                    current.panelViewTree.reset();
                    current.panelViewTransitions.Clear();
                    current.panelViewTransitionFramePending = false;
                    current.panelIndeterminateProgressActive = false;
                }
                const auto transition =
                    current.panelInteractionRegions.CommitFrame();
                if (transition.Changed())
                {
                    float pointerX = 0.0f;
                    float pointerY = 0.0f;
                    current.panelInteractionRegions.LastPointer(
                        pointerX, pointerY);
                    DispatchInteractionTransition(current, transition,
                        static_cast<int>(pointerX),
                        static_cast<int>(pointerY), normalizedSurface);
                    RuntimeInvalidateHost(widgetId);
                }
                ResolveDeferredHostInputFocus(
                    widgetId, normalizedSurface);
                ReconcileHostViewKeyboardFocus(current, normalizedSurface);
                const auto initialFocusKeys =
                    current.panelInteractionRegions.KeyboardFocusableKeys();
                const auto initialFocus = snowdesktop::widget_runtime::
                    BeginAuxiliarySurfaceKeyboardFocus(
                        initialFocusKeys.size(),
                        current.panelInitialKeyboardFocusPending,
                        !current.panelViewKeyboardFocusKey.empty());
                if (!current.panelViewKeyboardFocusKey.empty())
                {
                    current.panelInitialKeyboardFocusPending = false;
                }
                else if (initialFocus && RuntimeFocusViewTarget(
                        widgetId, initialFocusKeys[*initialFocus], "keyboard"))
                {
                    const int focusedIndex = FindWidget(widgetId);
                    if (focusedIndex >= 0)
                    {
                        widgets_[focusedIndex].
                            panelInitialKeyboardFocusPending = false;
                    }
                }
                if (current.panelViewTree)
                {
                    const bool transitionActive = drawPanelView(current);
                    if (transitionActive ||
                        current.panelIndeterminateProgressActive)
                        (void)ScheduleAnimationFrame(current);
                }
                if (!current.panelViewKeyboardFocusKey.empty() &&
                    !current.panelInteractionRegions.IsKeyboardFocusable(
                        current.panelViewKeyboardFocusKey))
                {
                    current.panelViewKeyboardFocusKey.clear();
                    current.panelViewFocusCueVisible = false;
                }
                DrawHostViewInteractionOverlays(current,
                    current.panelInteractionRegions,
                    current.panelViewKeyboardFocusKey, false);
            }
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
    }
    if (panelAccepted)
        RuntimeRecordSuccess(widgetId);
    if (!panelAccepted)
    {
        RuntimeInvalidateHost(widgetId);
        const int currentIndex = FindWidget(widgetId);
        if (currentIndex >= 0)
        {
            auto& current = widgets_[currentIndex];
            if (current.panelViewTree)
            {
                const bool transitionActive = drawPanelView(current);
                if (transitionActive ||
                    current.panelIndeterminateProgressActive)
                    (void)ScheduleAnimationFrame(current);
            }
            DrawHostViewInteractionOverlays(current,
                current.panelInteractionRegions,
                current.panelViewKeyboardFocusKey, false);
        }
    }
    snowdesktop::widget_runtime::PopWidgetClips(*d2dState_);
    lua_pop(state, 1);
    return panelAccepted;
}

void WidgetEngine::OnNotificationAction(
    std::string_view notificationId, std::string_view actionId)
{
    if (!notificationCenter_) return;
    const auto activation = notificationCenter_->Activate(
        notificationId, actionId);
    if (!activation.ok) return;
    const auto owner = std::find_if(widgets_.begin(), widgets_.end(),
        [&activation](const LuaWidget& widget) {
            return widget.runtimeToken == activation.ownerToken &&
                widget.valid && !widget.preview;
        });
    if (owner == widgets_.end()) return;

    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    (void)InvokeLifecycleEvent(*owner, "notification.action",
        [&activation](lua_State* eventState) {
            lua_pushlstring(eventState,
                activation.notificationId.data(),
                activation.notificationId.size());
            lua_setfield(eventState, -2, "notificationId");
            lua_pushlstring(eventState, activation.actionId.data(),
                activation.actionId.size());
            lua_setfield(eventState, -2, "actionId");
        });
    RuntimeInvalidateHost(owner->widgetId);
}

void WidgetEngine::OnAudioAnalysisWake()
{
    DrainAudioAnalysisChanges();
}

void WidgetEngine::SetTaskWakeCallback(TaskWakeCallback callback)
{
    taskWakeCallback_ = std::move(callback);
    if (d2dState_) d2dState_->packageImageCache.SetReadyCallback(taskWakeCallback_);
    if (audioPlayback_) audioPlayback_->SetWakeCallback(taskWakeCallback_);
    if (nativeTaskExecutor_) nativeTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
    if (locationTaskExecutor_) locationTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
    if (filesystemTaskExecutor_)
        filesystemTaskExecutor_->SetCompletionCallback(taskWakeCallback_);
}

void WidgetEngine::OnTaskWake()
{
    DrainPackageImageChanges();
    ApplyWidgetTaskBrokerActions();
}

void WidgetEngine::DrainPackageImageChanges()
{
    if (d2dState_)
    {
        for (const auto& widget : widgets_)
        {
            const auto prefix = WidgetWideToUtf8(widget.widgetId) + "|";
            if (!widget.valid)
                d2dState_->packageImageCache.ForgetOwner(prefix);
            else if (!widget.preview && !widget.desktopVisible)
                d2dState_->packageImageCache.ForgetOwner(prefix + "desktop");
        }
        d2dState_->packageImageCache.Pump();
        const auto changed = d2dState_->packageImageCache.TakeChanged();
        std::vector<std::wstring> invalidated;
        for (const auto& key : changed)
        {
            if (const auto bitmap = d2dState_->imageCache.find(key);
                bitmap != d2dState_->imageCache.end())
            {
                d2dState_->imageSamplingCache.Remove(bitmap->second.Get());
                d2dState_->imageCache.erase(bitmap);
                d2dState_->imageCacheRevisions.erase(key);
            }
            for (const auto& widget : widgets_)
            {
                if (!std::any_of(widget.packageImageContentKeys.begin(),
                        widget.packageImageContentKeys.end(),
                        [&](const auto& resource) { return resource.second == key; }))
                    continue;
                d2dState_->backgroundCache.Erase(widget.widgetId);
                if (d2dState_->packageImageCache.DecodeFailed(key))
                    RuntimeAddLog(widget.widgetId, "error", "Lazy package image decoding failed");
                invalidated.push_back(widget.widgetId);
            }
        }
        std::sort(invalidated.begin(), invalidated.end());
        invalidated.erase(std::unique(invalidated.begin(), invalidated.end()), invalidated.end());
        for (const auto& id : invalidated) RuntimeInvalidateHost(id);
    }
}

void WidgetEngine::TickRuntime()
{
    DrainPackageImageChanges();
    if (d2dState_ && d2dState_->shellIconLoader)
        d2dState_->shellIconLoader->Pump();
    const auto healthNow = snowdesktop::widget_runtime::
        RuntimeHealth::Clock::now();
    std::vector<std::wstring> recoveryTargets;
    for (auto& widget : widgets_)
    {
        const bool memoryQuotaExceeded = widget.quota &&
            widget.quota->memoryExceeded;
        if (widget.preview || widget.valid || memoryQuotaExceeded ||
            !widget.health.BeginRecovery(healthNow))
        {
            continue;
        }
        widget.valid = true;
        widgetHostFailures_.erase(widget.widgetId);
        recoveryTargets.push_back(widget.widgetId);
        RuntimeAddLog(widget.widgetId, "info",
            "Automatic runtime recovery probe started");
    }
    for (const auto& widgetId : recoveryTargets)
        RuntimeInvalidateHost(widgetId);

    const auto runtimeNow =
        snowdesktop::widget_runtime::WidgetDataBroker::Clock::now();
    if (dataBroker_)
    {
        dataBroker_->Tick(runtimeNow);
        ApplyWidgetDataBrokerActions();
    }
    DrainFilesystemWatchCompletions();
    ApplyWidgetTaskBrokerActions();
    if (notificationCenter_ && notificationScheduleStore_)
    {
        const auto notificationNow = snowdesktop::widget_runtime::
            WidgetNotificationCenter::Clock::now();
        const std::int64_t notificationNowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                notificationNow.time_since_epoch()).count();
        std::vector<snowdesktop::widget_runtime::
            WidgetPersistedNotificationSchedule> durableDue;
        for (const auto& entry :
            notificationScheduleStore_->Due(notificationNowMs))
        {
            const bool loaded = std::any_of(
                widgets_.begin(), widgets_.end(),
                [&entry](const LuaWidget& widget) {
                    return widget.valid && !widget.preview &&
                        WidgetWideToUtf8(widget.widgetId) ==
                            entry.instanceId &&
                        widget.packageId == entry.packageId;
                });
            if (loaded) durableDue.push_back(entry);
        }
        bool dispatchDue = !durableDue.empty();
        if (dispatchDue)
        {
            for (const auto& entry : durableDue)
            {
                (void)notificationScheduleStore_->Remove(
                    entry.instanceId, entry.notificationId);
            }
            if (!SaveNotificationSchedules())
            {
                std::string ignored;
                for (const auto& entry : durableDue)
                    (void)notificationScheduleStore_->Upsert(entry, ignored);
                dispatchDue = false;
            }
        }
        const auto deliveries = dispatchDue
            ? notificationCenter_->DispatchDue(notificationNow,
            [this](std::uint64_t ownerToken) {
                const auto owner = std::find_if(
                    widgets_.begin(), widgets_.end(),
                    [ownerToken](const LuaWidget& widget) {
                        return widget.runtimeToken == ownerToken &&
                            widget.valid && !widget.preview;
                    });
                return owner == widgets_.end()
                    ? std::string("instanceDisposed")
                    : RuntimeAdmitNotification(owner->widgetId);
            },
            [this](const snowdesktop::widget_runtime::
                WidgetNotificationHostRequest& request) {
                return notifyCallback_ && notifyCallback_(request);
            })
            : std::vector<snowdesktop::widget_runtime::
                WidgetNotificationDelivery>{};
        for (const auto& delivery : deliveries)
        {
            const auto owner = std::find_if(
                widgets_.begin(), widgets_.end(),
                [&delivery](const LuaWidget& widget) {
                    return widget.runtimeToken == delivery.ownerToken;
                });
            if (owner == widgets_.end() || !owner->valid) continue;
            (void)InvokeLifecycleEvent(*owner, "notification.delivered",
                [&delivery](lua_State* eventState) {
                    lua_pushlstring(eventState, delivery.id.data(),
                        delivery.id.size());
                    lua_setfield(eventState, -2, "notificationId");
                    lua_pushboolean(eventState, delivery.ok ? 1 : 0);
                    lua_setfield(eventState, -2, "ok");
                    if (!delivery.ok)
                    {
                        lua_pushlstring(eventState, delivery.error.data(),
                            delivery.error.size());
                        lua_setfield(eventState, -2, "error");
                    }
                });
            RuntimeInvalidateHost(owner->widgetId);
        }
    }
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
    DrainAudioAnalysisChanges();
    if (calendarService_ && calendarServiceLoaded_)
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
            if (networkTaskDispatch_.Consume(response, taskBroker_.get()))
                continue;
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
                    snowdesktop::widget_runtime::PushHttpResponseValue(state, response);
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

}

void WidgetEngine::ApplyWidgetHostVisibility(
    LuaWidget& widget, bool visible)
{
    if (!widget.valid || widget.hostVisible == visible)
        return;

    widget.hostVisible = visible;
    if (!visible && d2dState_)
    {
        d2dState_->backgroundCache.Erase(widget.widgetId);
        d2dState_->textLayoutCache.Erase(widget.widgetId);
    }
    const auto timerNow = widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_)
        ? snowdesktop::widget_runtime::NamedTimerSchedule::TimePoint{
            std::chrono::milliseconds(snowdesktop::widget_runtime::
                PreviewMonotonicMilliseconds) }
        : snowdesktop::widget_runtime::NamedTimerSchedule::Clock::now();
    if (widget.namedTimers.SetVisible(visible, timerNow))
        RescheduleNamedTimer(widget);

    if (visible)
        (void)widget.animationFrames.SetVisible(true);
    else
        StopAnimationFrames(widget);

    if (dataBroker_)
    {
        (void)dataBroker_->SetInstanceVisible(
            WidgetWideToUtf8(widget.widgetId), visible,
            snowdesktop::widget_runtime::WidgetDataBroker::Clock::now());
        ApplyWidgetDataBrokerActions();
    }
    InvokeSimpleCallback(widget, visible ? "onVisible" : "onHidden");
    if (visible)
        (void)ScheduleAnimationFrame(widget);
}

void WidgetEngine::SetWidgetDesktopVisible(
    const std::wstring& widgetId, bool visible,
    bool keepRuntimeActive)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    if (!widget.valid || (widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_)))
        return;
    widget.keepRuntimeActiveForHiddenPage =
        !visible && keepRuntimeActive;
    widget.desktopVisible = visible;
    if (!visible)
        widget.desktopOccluded = widget.desktopOcclusionResumePending = false;
    if (!visible && d2dState_)
        d2dState_->packageImageCache.ForgetOwner(WidgetWideToUtf8(widgetId) + "|desktop");
    if (!visible)
        ClearNativeMarqueeComposition(widget);
    ApplyWidgetHostVisibility(
        widget, widget.desktopVisible || widget.panelActive ||
            widget.keepRuntimeActiveForHiddenPage);
}

void WidgetEngine::SetAllWidgetDesktopVisible(bool visible)
{
    for (auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview)
            continue;
        widget.keepRuntimeActiveForHiddenPage = false;
        widget.desktopVisible = visible;
        if (!visible)
            widget.desktopOccluded = widget.desktopOcclusionResumePending = false;
        if (!visible && d2dState_)
            d2dState_->packageImageCache.ForgetOwner(
                WidgetWideToUtf8(widget.widgetId) + "|desktop");
        if (!visible)
            ClearNativeMarqueeComposition(widget);
        ApplyWidgetHostVisibility(
            widget, widget.desktopVisible || widget.panelActive);
    }
}

void WidgetEngine::SetWidgetDesktopOccluded(const std::wstring& widgetId, bool occluded)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    if (widget.desktopOccluded == occluded) return;
    widget.desktopOccluded = occluded;
    widget.desktopOcclusionResumePending = !occluded && widget.desktopVisible;
    if (!occluded) return;
    widget.viewTransitions.Clear();
    widget.viewTransitionFramePending = false;
    if (widget.panelActive) return;
    if (widget.animationTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.animationTimerId);
    widget.animationTimerId = 0;
    (void)widget.animationFrames.ConsumeDataRefresh();
    widget.animationFrames.ResetFrameTiming();
}

void WidgetEngine::OnWidgetTimer(const std::wstring& widgetId, UINT_PTR timerId)
{
    snowdesktop::performance::Scope performanceScope(
        "widget", "timer.dispatch", widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& widget = widgets_[idx];
    lua_State* state = widget.state;
    if (!state) return;
    const std::wstring activeWidgetId = widget.widgetId;
    snowdesktop::widget_runtime::WidgetInvalidationBatch::Scope
        invalidationScope(invalidationBatch_, invalidateCallback_);
    WidgetExecutionContextGuard contextGuard(
        d2dState_, activeWidgetId);
    snowdesktop::lua_runtime::StackGuard stackGuard(state);
    SetWidgetRectContext(d2dState_, widget.lastBounds);

    if (timerId == widget.animationTimerId)
    {
        if (widget.animationTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.animationTimerId);
        widget.animationTimerId = 0;
        if (widget.desktopOccluded && !widget.panelActive) return;
        const bool dataRefreshFrame = widget.animationFrames.ConsumeDataRefresh();
        const auto now = snowdesktop::widget_runtime::
            AnimationFrameRequests::Clock::now();
        const auto nowMilliseconds = snowdesktop::widget_runtime::
            CurrentMonotonicMilliseconds();
        const bool desktopTransitionFrame =
            widget.viewTransitions.Tick(now);
        const bool panelTransitionFrame =
            widget.panelViewTransitions.Tick(now);
        const bool reducedMotion =
            HostManagedAnimationsSuppressed();
        const bool animateIndeterminateProgress = !reducedMotion;
        const bool desktopProgressFrame = animateIndeterminateProgress &&
            widget.hostVisible && !widget.desktopOccluded &&
            widget.viewIndeterminateProgressActive;
        const bool panelProgressFrame = animateIndeterminateProgress &&
            widget.panelActive &&
            widget.panelIndeterminateProgressActive;
        if (desktopTransitionFrame || desktopProgressFrame)
            widget.viewTransitionFramePending = true;
        if ((panelTransitionFrame && widget.panelActive) ||
            panelProgressFrame)
            widget.panelViewTransitionFramePending = true;
        const bool desktopMarqueeFrame = !reducedMotion &&
            widget.desktopVisible && AdvanceNativeMarqueeSurface(
                widget.desktopMarquee, now);
        const auto frames = widget.animationFrames.Consume(now);
        for (const auto& frame : frames)
        {
            (void)InvokeLifecycleEvent(widget, "frame",
                [&frame, nowMilliseconds](lua_State* eventState) {
                    lua_pushlstring(eventState,
                        frame.name.data(), frame.name.size());
                    lua_setfield(eventState, -2, "id");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(nowMilliseconds));
                    lua_setfield(eventState, -2, "now");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(
                            frame.deltaMilliseconds));
                    lua_setfield(eventState, -2, "deltaMs");
                });
        }
        const bool generalAnimationFrame =
            desktopTransitionFrame || desktopProgressFrame ||
            (panelTransitionFrame && widget.panelActive) ||
            panelProgressFrame;
        if (generalAnimationFrame || dataRefreshFrame)
            RuntimeInvalidateHost(activeWidgetId);
        else if (desktopMarqueeFrame && invalidateCallback_)
            invalidateCallback_(activeWidgetId,
                NativeMarqueeDirtyRect(widget.desktopMarquee), "desktop");
        (void)ScheduleAnimationFrame(widget);
        return;
    }

    if (timerId == widget.refreshTimerId)
    {
        (void)InvokeLifecycleEvent(widget, "timer",
            [](lua_State* eventState) {
                lua_pushliteral(eventState, "refresh");
                lua_setfield(eventState, -2, "name");
            });
        RuntimeInvalidateHost(activeWidgetId);
        return;
    }

    if (timerId != widget.namedTimerId)
        return;

    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    const auto now = std::chrono::steady_clock::now();
    const auto wallNow = std::chrono::system_clock::now();
    const auto wallNowMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wallNow.time_since_epoch()).count();
    const std::vector<std::string> dueNames =
        widget.namedTimers.DueNames(now, wallNow);

    bool invoked = false;
    for (const auto& name : dueNames)
    {
        const auto fire = widget.namedTimers.ConsumeDueInfo(
            name, now, wallNow);
        if (!fire)
            continue;

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
                if (fire->timeline)
                {
                    PushInteractionValue(eventState, fire->value);
                    lua_setfield(eventState, -2, "value");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(
                            fire->timelineIndex));
                    lua_setfield(eventState, -2, "timelineIndex");
                    lua_pushinteger(eventState,
                        static_cast<lua_Integer>(
                            fire->timelineCount));
                    lua_setfield(eventState, -2, "timelineCount");
                    lua_pushboolean(eventState,
                        fire->timelineEnded ? 1 : 0);
                    lua_setfield(eventState, -2, "timelineEnded");
                    lua_pushboolean(eventState,
                        fire->reload ? 1 : 0);
                    lua_setfield(eventState, -2, "reload");
                }
            });
        invoked = true;
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

bool WidgetEngine::HasBackgroundLayer(const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && widgets_[index].hasBackgroundLayer;
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

bool WidgetEngine::HasFileDropTarget(const std::wstring& widgetId, int x, int y) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& widget = widgets_[index];
    return widget.state && !widget.preview && filesystemHandleStore_ &&
        snowdesktop::widget::WidgetPermissionBroker::AllowsPermission(
            widget.permissions, kFilesystemReadPermission) &&
        widget.interactionRegions.FileDropActionAt(
            static_cast<float>(x), static_cast<float>(y));
}

std::optional<WidgetEngine::FileDropTarget> WidgetEngine::CaptureFileDropTarget(
    const std::wstring& widgetId, int x, int y) const
{
    if (!HasFileDropTarget(widgetId, x, y)) return std::nullopt;
    const auto& widget = widgets_[FindWidget(widgetId)];
    FileDropTarget target;
    target.widgetId = widgetId;
    target.packageId = widget.packageId;
    target.runtimeToken = widget.runtimeToken;
    target.action = *widget.interactionRegions.FileDropActionAt(
        static_cast<float>(x), static_cast<float>(y), &target.targetKey);
    return target;
}

bool WidgetEngine::InvokeFileDrop(const std::wstring& widgetId, int x, int y,
    const std::vector<std::wstring>& paths)
{
    const auto target = CaptureFileDropTarget(widgetId, x, y);
    return target && InvokeFileDrop(*target, paths);
}

bool WidgetEngine::InvokeFileDrop(const FileDropTarget& target,
    const std::vector<std::wstring>& paths)
{
    using namespace snowdesktop::widget_runtime;
    const int index = FindWidget(target.widgetId);
    if (index < 0 || !filesystemHandleStore_) return false;
    auto& widget = widgets_[index];
    const WidgetFileDropDeliveryState current{widget.state != nullptr, widget.preview,
        widget.runtimeToken, widget.permissions, widget.interactionRegions};
    const WidgetFilesystemHandleOwner owner{WideToUtf8(target.widgetId), widget.packageId};
    return DeliverWidgetFileDrop(target, current, *filesystemHandleStore_, owner,
        trustedGestureState_, paths, [&](const auto& items) {
            return InvokeLifecycleEvent(widget, "action", [&](lua_State* state) {
                PushWidgetFileDropFields(state, target, items, PushInteractionValue, WideToUtf8);
            });
        });
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
    const std::string eventSurface =
            std::strstr(callbackName, "Dialog") != nullptr
                ? "dialog"
                : (std::strstr(callbackName, "Popover") != nullptr
                    ? "popover"
                    : (std::strstr(callbackName, "Panel") != nullptr
                        ? "panel" : "desktop"));
        WidgetSurfaceScope surfaceScope(
            d2dState_, eventSurface.c_str(),
            std::strcmp(callbackName, "onPanelOpened") == 0 ||
                std::strcmp(callbackName, "onDialogOpened") == 0 ||
                std::strcmp(callbackName, "onPopoverOpened") == 0);
        const char* kind = "pointer";
        const char* action = callbackName;
        if (std::strcmp(callbackName, "onClick") == 0 ||
            std::strcmp(callbackName, "onPanelClick") == 0 ||
            std::strcmp(callbackName, "onDialogClick") == 0 ||
            std::strcmp(callbackName, "onPopoverClick") == 0)
            action = "click";
        else if (std::strcmp(callbackName, "onDoubleClick") == 0 ||
            std::strcmp(callbackName, "onPanelDoubleClick") == 0 ||
            std::strcmp(callbackName, "onDialogDoubleClick") == 0 ||
            std::strcmp(callbackName, "onPopoverDoubleClick") == 0)
            action = "doubleClick";
        else if (std::strcmp(callbackName, "onMouseDown") == 0 ||
            std::strcmp(callbackName, "onPanelMouseDown") == 0 ||
            std::strcmp(callbackName, "onDialogMouseDown") == 0 ||
            std::strcmp(callbackName, "onPopoverMouseDown") == 0)
            action = "pointerDown";
        else if (std::strcmp(callbackName, "onMouseMove") == 0 ||
            std::strcmp(callbackName, "onPanelMouseMove") == 0 ||
            std::strcmp(callbackName, "onDialogMouseMove") == 0 ||
            std::strcmp(callbackName, "onPopoverMouseMove") == 0)
            action = "pointerMove";
        else if (std::strcmp(callbackName, "onMouseUp") == 0 ||
            std::strcmp(callbackName, "onPanelMouseUp") == 0 ||
            std::strcmp(callbackName, "onDialogMouseUp") == 0 ||
            std::strcmp(callbackName, "onPopoverMouseUp") == 0)
            action = "pointerUp";
        else if (std::strcmp(callbackName, "onWheel") == 0 ||
            std::strcmp(callbackName, "onPanelWheel") == 0 ||
            std::strcmp(callbackName, "onDialogWheel") == 0 ||
            std::strcmp(callbackName, "onPopoverWheel") == 0)
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
        else if (std::strcmp(callbackName, "onDialogOpened") == 0)
        {
            kind = "dialog";
            action = "opened";
        }
        else if (std::strcmp(callbackName, "onDialogClosed") == 0)
        {
            kind = "dialog";
            action = "closed";
        }
        else if (std::strcmp(callbackName, "onPopoverOpened") == 0)
        {
            kind = "popover";
            action = "opened";
        }
        else if (std::strcmp(callbackName, "onPopoverClosed") == 0)
        {
            kind = "popover";
            action = "closed";
        }
        std::string targetKey;
        auto& interactionRegions = InteractionRegionsForSurface(
            w, eventSurface);
        const bool declarativeSurface =
            HasDeclarativeViewForSurface(w, eventSurface);
        if (std::strcmp(kind, "pointer") == 0)
        {
            bool declarativeVisualChanged = false;
            bool visualChangeRequiresLua = false;
            const auto transition = interactionRegions.UpdateHover(
                static_cast<float>(x), static_cast<float>(y));
            if (transition.Changed())
            {
                declarativeVisualChanged = true;
                visualChangeRequiresLua =
                    interactionRegions.HasTransitionAction(transition);
                DispatchInteractionTransition(w, transition, x, y,
                    eventSurface);
            }
            if (std::strcmp(action, "pointerDown") == 0)
            {
                const std::string previousPressed =
                    interactionRegions.PressedKey();
                targetKey = interactionRegions.PointerDown(
                    static_cast<float>(x), static_cast<float>(y), button).
                    targetKey;
                if (previousPressed != targetKey)
                {
                    declarativeVisualChanged = true;
                    visualChangeRequiresLua = visualChangeRequiresLua ||
                        interactionRegions.FindAction(
                            targetKey, "pointerDown") != nullptr;
                }
            }
            else if (std::strcmp(action, "pointerUp") == 0)
            {
                const std::string previousPressed =
                    interactionRegions.PressedKey();
                targetKey = interactionRegions.PointerUp(
                    static_cast<float>(x), static_cast<float>(y), button).
                    targetKey;
                if (!previousPressed.empty())
                {
                    declarativeVisualChanged = true;
                    visualChangeRequiresLua = visualChangeRequiresLua ||
                        interactionRegions.FindAction(
                            targetKey, "pointerUp") != nullptr;
                }
            }
            else if (std::strcmp(action, "click") == 0)
            {
                targetKey = interactionRegions.ConsumeClickTarget(
                    static_cast<float>(x), static_cast<float>(y));
            }
            else if (std::strcmp(action, "pointerMove") == 0)
            {
                targetKey = interactionRegions.PointerMoveTarget(
                    static_cast<float>(x), static_cast<float>(y));
            }
            else
            {
                targetKey = interactionRegions.TargetAt(
                    static_cast<float>(x), static_cast<float>(y));
            }
            DispatchInteractionAction(w, targetKey, action, x, y,
                button, delta,
                std::strcmp(action, "doubleClick") == 0 ? 2 :
                    (std::strcmp(action, "click") == 0 ? 1 : 0),
                false, "pointer", 0, std::nullopt, std::nullopt,
                eventSurface);
            if (declarativeVisualChanged)
            {
                if (declarativeSurface && !visualChangeRequiresLua)
                    QueueDeclarativeVisualFrame(w, eventSurface);
                RuntimeInvalidateHost(widgetId);
            }
        }
        if (std::strcmp(kind, "pointer") != 0 || !declarativeSurface)
        {
            (void)InvokeLifecycleEvent(w, kind,
                [action, eventSurface, x, y, button, delta, trustedGesture,
                    &targetKey](lua_State* eventState) {
                    lua_pushstring(eventState, action);
                    lua_setfield(eventState, -2, "action");
                    lua_pushlstring(eventState, eventSurface.data(),
                        eventSurface.size());
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
        }
}

bool WidgetEngine::HasInteractionPointerCapture(
    const std::wstring& widgetId, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        InteractionRegionsForSurface(widgets_[index], surface).
            HasPointerCapture();
}

void WidgetEngine::CancelInteractionPointerPress(std::string_view surface)
{
    if (surface.empty() || focusedHostInput_.surface == NormalizeWidgetSurface(surface))
    {
        focusedHostInput_.pointerSelecting = false;
        focusedHostInput_.selectionDragArmed = false;
    }
    if (hostScrollbarDrag_.active &&
        (surface.empty() || hostScrollbarDrag_.surface ==
            NormalizeWidgetSurface(surface)))
        hostScrollbarDrag_ = {};
    for (auto& widget : widgets_)
    {
        bool changed = false;
        if (surface.empty())
        {
            changed = !widget.interactionRegions.PressedKey().empty() ||
                !widget.panelInteractionRegions.PressedKey().empty();
            widget.interactionRegions.CancelPointerPress();
            widget.panelInteractionRegions.CancelPointerPress();
        }
        else
        {
            auto& regions = InteractionRegionsForSurface(widget, surface);
            changed = !regions.PressedKey().empty();
            regions.CancelPointerPress();
        }
        if (changed) RuntimeInvalidateHost(widget.widgetId);
    }
}

static bool PushProtectedMenuField(lua_State* state, int tableIndex,
    const char* field, std::chrono::steady_clock::time_point deadline,
    std::string& error)
{
    // Menu descriptors are user-owned tables. Preserve __index/string
    // conversion semantics, but keep their errors and execution budget inside
    // Lua while native item/vector owners remain on the calling C++ stack.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
    {
        error = "Widget menu descriptor execution quota exceeded";
        return false;
    }
    struct FieldArguments
    {
        const char* name;
        std::chrono::milliseconds remaining;
    } arguments{field, remaining};
    return snowdesktop::lua_runtime::ProtectedHostCall(state,
        +[](lua_State* inner) {
            const auto* input = static_cast<FieldArguments*>(lua_touserdata(inner, 1));
            lua_pushcfunction(inner, +[](lua_State* fieldState) {
                const auto* fieldInput = static_cast<FieldArguments*>(lua_touserdata(fieldState, 2));
                lua_getfield(fieldState, 1, fieldInput->name);
                if (lua_type(fieldState, -1) == LUA_TNUMBER)
                    (void)lua_tolstring(fieldState, -1, nullptr);
                return 1;
            });
            lua_pushvalue(inner, 2);
            lua_pushvalue(inner, 1);
            if (snowdesktop::lua_runtime::ProtectedCall(inner, 2, 1,
                    snowdesktop::lua_runtime::kDefaultInstructionBudget, input->remaining) != LUA_OK)
                return lua_error(inner);
            return 1;
        }, &arguments, 1, error, tableIndex);
}

std::vector<LuaWidgetMenuItem> WidgetEngine::GetContextMenu(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface, bool componentScopeOnly,
    const LuaWidgetMenuItem* refreshTarget)
{
    std::vector<LuaWidgetMenuItem> result;
    int idx = FindWidget(widgetId);
    if (idx < 0) return result;
    auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return result;
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);

        auto& interactionRegions =
            InteractionRegionsForSurface(w, surface);
        std::string targetKey;
        const snowdesktop::widget_runtime::InteractionAction* requestActionPointer;
        if (refreshTarget)
        {
            if (w.runtimeToken != refreshTarget->runtimeToken) return result;
            targetKey = refreshTarget->targetKey;
            const auto* region = interactionRegions.Find(targetKey);
            if (!region || !region->enabled) return result;
            requestActionPointer = interactionRegions.FindAction(targetKey, "contextMenu");
            if (!requestActionPointer || requestActionPointer->id != refreshTarget->bindingId ||
                requestActionPointer->value != refreshTarget->contextValue ||
                (requestActionPointer->contextMenuScope ==
                    snowdesktop::widget_runtime::InteractionAction::ContextMenuScope::Element) !=
                    refreshTarget->elementContext)
                return result;
        }
        else
        {
            requestActionPointer = interactionRegions.ContextMenuActionAt(
                static_cast<float>(x), static_cast<float>(y),
                componentScopeOnly, &targetKey);
        }
        if (!requestActionPointer || targetKey.empty()) return result;
        const auto requestAction = *requestActionPointer;
        WidgetExecutionContextGuard contextGuard(d2dState_, widgetId);
        WidgetSurfaceScope surfaceScope(d2dState_,
            normalizedSurface.c_str());
        snowdesktop::lua_runtime::StackGuard stackGuard(state);
        SetWidgetRectContext(d2dState_, BoundsForSurface(w, surface));
        bool invoked = false;
        std::string error;
        struct MenuRequestArguments
        {
            const snowdesktop::widget_runtime::InteractionAction* action;
            const std::string* targetKey;
            std::string_view surface;
        } arguments{&requestAction, &targetKey, surface};
        if (!snowdesktop::lua_runtime::ProtectedHostCall(state,
                +[](lua_State* inner) {
                    const auto& input = *static_cast<MenuRequestArguments*>(lua_touserdata(inner, 1));
                    lua_createtable(inner, 0, 6);
                    lua_pushlstring(inner, input.action->id.data(), input.action->id.size());
                    lua_setfield(inner, -2, "id");
                    PushInteractionValue(inner, input.action->value);
                    lua_setfield(inner, -2, "value");
                    lua_pushlstring(inner, input.targetKey->data(), input.targetKey->size());
                    lua_setfield(inner, -2, "targetKey");
                    lua_pushlstring(inner, input.surface.data(), input.surface.size());
                    lua_setfield(inner, -2, "surface");
                    lua_pushliteral(inner, "pointer");
                    lua_setfield(inner, -2, "source");
                    lua_pushstring(inner, input.action->contextMenuScope ==
                        snowdesktop::widget_runtime::InteractionAction::ContextMenuScope::Component
                            ? "component" : "element");
                    lua_setfield(inner, -2, "scope");
                    return 1;
                }, &arguments, 1, error))
        {
            RuntimeRecordError(widgetId, error);
            return result;
        }
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

        constexpr std::size_t kMaxMenuDescriptors = 64;
        constexpr int kMaxSubmenuDepth = 3;
        std::unordered_set<std::string> ids;
        std::size_t descriptorCount = 0;
        std::string menuError;
        const auto descriptorDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        const auto pushField = [&](int index, const char* field) {
            return PushProtectedMenuField(state, index, field, descriptorDeadline, menuError);
        };
        std::function<bool(int, int, std::vector<LuaWidgetMenuItem>&)>
            parseMenuItems;
        parseMenuItems = [&](int tableIndex, int depth,
                             std::vector<LuaWidgetMenuItem>& output) {
            tableIndex = lua_absindex(state, tableIndex);
            const std::size_t count = lua_rawlen(state, tableIndex);
            if (count > kMaxMenuDescriptors - descriptorCount)
            {
                menuError = "Widget menu exceeds the 64 descriptor limit";
                return false;
            }
            for (std::size_t itemIndex = 1; itemIndex <= count; ++itemIndex)
            {
                lua_rawgeti(state, tableIndex,
                    static_cast<lua_Integer>(itemIndex));
                if (!lua_istable(state, -1))
                {
                    menuError = "Widget menu items must be descriptor tables";
                    return false;
                }
                ++descriptorCount;

                LuaWidgetMenuItem item;
                if (!pushField(-1, "type")) return false;
                item.separator = lua_isstring(state, -1) &&
                    std::strcmp(lua_tostring(state, -1), "separator") == 0;
                lua_pop(state, 1);
                if (item.separator)
                {
                    output.push_back(std::move(item));
                    lua_pop(state, 1);
                    continue;
                }

                if (!pushField(-1, "id")) return false;
                if (lua_isstring(state, -1))
                    item.actionId = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "label")) return false;
                if (lua_isstring(state, -1))
                    item.label = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "icon")) return false;
                if (lua_isstring(state, -1))
                    item.icon = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "iconFont")) return false;
                if (lua_isstring(state, -1))
                    item.iconFont = lua_tostring(state, -1);
                lua_pop(state, 1);
                if (!pushField(-1, "image")) return false;
                if (!lua_isnil(state, -1))
                {
                    if (!snowdesktop::lua_runtime::ProtectedHostCall(state,
                            +[](lua_State* inner) {
                                auto* resource = TestResourceHandle(inner, 2);
                                lua_pushlightuserdata(inner, resource);
                                return 1;
                            }, nullptr, 1, menuError, -1)) return false;
                    const auto* handle = static_cast<const snowdesktop::widget_runtime::LuaResourceHandle*>(
                        lua_touserdata(state, -1));
                    lua_pop(state, 1);
                    if (!handle || handle->type != LuaResourceType::Image ||
                        snowdesktop::widget_runtime::
                            IsWidgetRuntimeImageToken(handle->name))
                    {
                        menuError =
                            "Widget menu image must be a package resource.image handle";
                        return false;
                    }
                    item.imageResourceName = handle->name;
                }
                lua_pop(state, 1);
                if (!item.icon.empty() && !item.imageResourceName.empty())
                {
                    menuError =
                        "Widget menu items cannot combine icon and image";
                    return false;
                }
                if (!pushField(-1, "enabled")) return false;
                item.enabled = lua_isnil(state, -1) ||
                    lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (!pushField(-1, "checked")) return false;
                item.checked = lua_toboolean(state, -1) != 0;
                lua_pop(state, 1);
                if (!pushField(-1, "keepOpen")) return false;
                item.keepOpen = lua_isboolean(state, -1) && lua_toboolean(state, -1);
                lua_pop(state, 1);
                if (item.label.empty() || item.label.size() > 512)
                {
                    menuError =
                        "Widget menu item labels must contain 1..512 UTF-8 bytes";
                    return false;
                }

                if (!pushField(-1, "children")) return false;
                const bool hasChildren = lua_istable(state, -1);
                if (!hasChildren && !lua_isnil(state, -1))
                {
                    menuError = "Widget menu children must be an array table";
                    return false;
                }
                if (hasChildren)
                {
                    if (!item.actionId.empty() || item.keepOpen)
                    {
                        menuError =
                            "Widget submenu parents cannot declare id or keepOpen";
                        return false;
                    }
                    if (depth >= kMaxSubmenuDepth)
                    {
                        menuError = "Widget menu exceeds three submenu levels";
                        return false;
                    }
                    if (lua_rawlen(state, -1) == 0)
                    {
                        menuError = "Widget submenu children cannot be empty";
                        return false;
                    }
                    if (!parseMenuItems(-1, depth + 1, item.children))
                        return false;
                    lua_pop(state, 1);
                }
                else
                {
                    lua_pop(state, 1);
                    if (item.actionId.empty() ||
                        item.actionId.size() > 128 ||
                        !ids.insert(item.actionId).second)
                    {
                        menuError =
                            "Widget menu leaf IDs must be unique and contain 1..128 UTF-8 bytes";
                        return false;
                    }
                    item.elementContext =
                        requestAction.contextMenuScope ==
                        snowdesktop::widget_runtime::InteractionAction::
                            ContextMenuScope::Element;
                    item.targetKey = targetKey;
                    item.bindingId = requestAction.id;
                    item.surface = normalizedSurface;
                    item.contextValue = requestAction.value;
                    item.runtimeToken = w.runtimeToken;
                }
                output.push_back(std::move(item));
                lua_pop(state, 1);
            }
            return true;
        };
        if (!parseMenuItems(-1, 0, result))
        {
            result.clear();
            RuntimeRecordError(widgetId, menuError);
        }
        if (snowdesktop::widget_api::ConsumeTransientStateDirty(state))
            RuntimeInvalidateHost(widgetId);
    return result;
}

bool WidgetEngine::InvokeMenu(const std::wstring& widgetId,
    const LuaWidgetMenuItem& menuItem)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    auto& interactionRegions = InteractionRegionsForSurface(
        widget, menuItem.surface);
    if (menuItem.actionId.empty() ||
        !snowdesktop::widget_runtime::IsWidgetMenuSelectionCurrent(
            interactionRegions, menuItem.targetKey,
            menuItem.runtimeToken, widget.runtimeToken))
        return false;
    if (menuItem.keepOpen)
    {
        const auto* region = interactionRegions.Find(menuItem.targetKey);
        const auto* action = interactionRegions.FindAction(menuItem.targetKey, "contextMenu");
        if (!region || !region->enabled || !action || action->id != menuItem.bindingId ||
            action->value != menuItem.contextValue ||
            (action->contextMenuScope ==
                snowdesktop::widget_runtime::InteractionAction::ContextMenuScope::Element) !=
                menuItem.elementContext)
            return false;
    }
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    WidgetSurfaceScope surfaceScope(d2dState_, menuItem.surface.c_str());
    return InvokeLifecycleEvent(widget, "action",
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
            lua_pushlstring(eventState, menuItem.surface.data(),
                menuItem.surface.size());
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
    RefreshLogicalSlotAvailability();
    if (d2dState_)
    {
        ClearShellIconBitmaps(d2dState_);
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

bool WidgetEngine::ReadCustomAppearance(const std::wstring& widgetId,
    PersonalizationSettings& appearance) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto selection = RuntimeGetStorageValue(widgetId, "__preset");
    const auto& widget = widgets_[index];
    const auto findPreset = [&](const auto& presets) -> const LuaWidgetManifest::SettingPreset* {
        const auto found = std::find_if(presets.begin(), presets.end(),
            [&](const auto& preset) { return preset.id == selection; });
        return found == presets.end() ? nullptr : &*found;
    };
    const auto* authored = findPreset(widget.manifest.presets);
    if (!authored) authored = findPreset(widget.scriptPresets);
    const int fallbackContentTheme = appearance.contentTheme;
    if (!ReadCustomColors(widgetId,
            appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha,
            appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha,
            appearance.widgetBorderWidth, appearance.widgetEdgeHighlightEnabled,
            appearance.widgetEdgeHighlightWidth, appearance.widgetEdgeHighlightStrength,
            appearance.gradientEndA, appearance.glassEnabled, appearance.acrylicEnabled,
            &appearance.panelGradient, &appearance.edgeLight, !authored)) return false;
    const auto contentTheme = RuntimeGetStorageValue(widgetId, "__contentTheme");
    if (contentTheme == "0" || contentTheme == "1") appearance.contentTheme = contentTheme == "1" ? 1 : 0;
    if (authored && !authored->values.contains("__contentTheme"))
        appearance.contentTheme = fallbackContentTheme;
    appearance = snowdesktop::widget_runtime::ResolveWidgetHostAppearancePreset(
        std::move(appearance), selection, authored ? &authored->values : nullptr);
    return true;
}

bool WidgetEngine::ReadCustomColors(const std::wstring& widgetId,
    float& bgR, float& bgG, float& bgB, float& alpha,
    float& borderR, float& borderG, float& borderB, float& borderAlpha,
    float& borderWidth, bool& edgeHighlightEnabled,
    float& edgeHighlightWidth, float& edgeHighlightStrength,
    float& gradientEndA,
    bool& glassEnabled, bool& acrylicEnabled,
    snowdesktop::PanelGradient* panelGradient,
    snowdesktop::EdgeLightSettings* edgeLight,
    bool includeStoredValues) const
{
    if (panelGradient) *panelGradient = {};
    if (edgeLight) *edgeLight = {};
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    const auto& w = widgets_[idx];
    lua_State* state = w.state;
    if (!state) return false;
    snowdesktop::lua_runtime::StackGuard stackGuard(state);

    lua_rawgeti(state, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(state, -1)) { lua_pop(state, 1); return false; }

    auto readHex = [&](const char* key, float& r, float& g, float& b,
                       int defaultValue) {
        lua_getfield(state, -1, key);
        int value = lua_isinteger(state, -1)
            ? static_cast<int>(lua_tointeger(state, -1))
            : defaultValue;
        lua_pop(state, 1);
        r = ((value >> 16) & 0xFF) / 255.0f;
        g = ((value >> 8) & 0xFF) / 255.0f;
        b = (value & 0xFF) / 255.0f;
    };
    auto readFloat = [&](const char* key, float& out, float defaultValue) {
        lua_getfield(state, -1, key);
        out = lua_isnumber(state, -1)
            ? static_cast<float>(lua_tonumber(state, -1))
            : defaultValue;
        lua_pop(state, 1);
    };
    auto readBool = [&](const char* key, bool& out, bool defaultValue) {
        lua_getfield(state, -1, key);
        out = lua_isnil(state, -1)
            ? defaultValue : (lua_toboolean(state, -1) != 0);
        lua_pop(state, 1);
    };

    readHex("bg", bgR, bgG, bgB, 0x151A21);
    readHex("border", borderR, borderG, borderB, 0xFFFFFF);
    readFloat("alpha", alpha, 0.36f);
    readFloat("borderAlpha", borderAlpha, alpha);
    readFloat("gradientEndA", gradientEndA, 0.0f);
    readBool("glassEnabled", glassEnabled, false);
    readBool("acrylicEnabled", acrylicEnabled, false);

    lua_getfield(state, -1, "borderStyle");
    const bool legacyBorderStyleDeclared = lua_isinteger(state, -1);
    const int legacyBorderStyle = legacyBorderStyleDeclared
        ? static_cast<int>(lua_tointeger(state, -1)) : 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "borderWidth");
    const bool borderWidthDeclared = lua_isnumber(state, -1);
    if (borderWidthDeclared)
        borderWidth = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, -1, "edgeHighlightEnabled");
    const bool edgeHighlightEnabledDeclared = !lua_isnil(state, -1);
    if (edgeHighlightEnabledDeclared)
        edgeHighlightEnabled = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "edgeHighlightWidth");
    const bool edgeHighlightWidthDeclared = lua_isnumber(state, -1);
    if (edgeHighlightWidthDeclared)
        edgeHighlightWidth = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    readFloat("edgeHighlightStrength", edgeHighlightStrength,
        kDefaultEdgeHighlightStrength);

    const auto readStorage = [&](const char* key) {
        return includeStoredValues ? RuntimeGetStorageValue(widgetId, key) : std::string{};
    };
    auto readStoredColor = [&](const char* key, float& r, float& g,
                               float& b) {
        const std::string value = readStorage(key);
        if (value.empty()) return;
        const int encoded = std::atoi(value.c_str());
        r = ((encoded >> 16) & 0xFF) / 255.0f;
        g = ((encoded >> 8) & 0xFF) / 255.0f;
        b = (encoded & 0xFF) / 255.0f;
    };
    auto readStoredFloat = [&](const char* key, float& out) {
        const std::string value = readStorage(key);
        if (!value.empty())
            out = static_cast<float>(std::atof(value.c_str()));
    };
    auto readStoredBool = [&](const char* key, bool& out) {
        const std::string value = readStorage(key);
        if (!value.empty())
            out = value == "1" || value == "true";
    };
    readStoredColor("bg", bgR, bgG, bgB);
    readStoredColor("border", borderR, borderG, borderB);
    readStoredFloat("alpha", alpha);
    readStoredFloat("borderAlpha", borderAlpha);
    readStoredFloat("gradientEndA", gradientEndA);
    readStoredBool("glassEnabled", glassEnabled);
    readStoredBool("acrylicEnabled", acrylicEnabled);
    if (panelGradient)
    {
        JsonValue value;
        if (ParseJson(readStorage("__panelGradient"), value))
            (void)snowdesktop::DecodePanelGradient(value, *panelGradient);
    }

    if (edgeLight)
    {
        JsonValue value;
        if (ParseJson(readStorage("__edgeLight"), value))
            (void)snowdesktop::DecodeEdgeLight(value, *edgeLight);
    }
    const std::string storedLegacyBorderStyle =
        readStorage("borderStyle");
    const std::string storedBorderWidth =
        readStorage("borderWidth");
    if (!storedBorderWidth.empty())
    {
        borderWidth = static_cast<float>(
            std::atof(storedBorderWidth.c_str()));
    }
    else if (!borderWidthDeclared)
        borderWidth = 1.0f;
    const std::string storedEdgeHighlightEnabled =
        readStorage("edgeHighlightEnabled");
    if (!storedEdgeHighlightEnabled.empty())
        edgeHighlightEnabled = storedEdgeHighlightEnabled == "1" ||
            storedEdgeHighlightEnabled == "true";
    else if (!edgeHighlightEnabledDeclared)
        edgeHighlightEnabled = !storedLegacyBorderStyle.empty()
            ? std::atoi(storedLegacyBorderStyle.c_str()) == 1
            : legacyBorderStyleDeclared
                ? legacyBorderStyle == 1 : glassEnabled;
    const std::string storedEdgeHighlightWidth =
        readStorage("edgeHighlightWidth");
    if (!storedEdgeHighlightWidth.empty())
        edgeHighlightWidth = static_cast<float>(
            std::atof(storedEdgeHighlightWidth.c_str()));
    else if (!edgeHighlightWidthDeclared)
        edgeHighlightWidth = ((!storedLegacyBorderStyle.empty() &&
                std::atoi(storedLegacyBorderStyle.c_str()) == 1) ||
            (storedLegacyBorderStyle.empty() && legacyBorderStyle == 1)) &&
            (!storedBorderWidth.empty() || borderWidthDeclared)
            ? borderWidth : kDefaultEdgeHighlightWidth;
    const std::string storedEdgeHighlightStrength =
        readStorage("edgeHighlightStrength");
    if (!storedEdgeHighlightStrength.empty())
        edgeHighlightStrength = static_cast<float>(
            std::atof(storedEdgeHighlightStrength.c_str()));
    else
        readStoredFloat("borderEffectStrength", edgeHighlightStrength);
    if (!std::isfinite(borderWidth))
        borderWidth = 1.0f;
    if (!std::isfinite(edgeHighlightWidth))
        edgeHighlightWidth = kDefaultEdgeHighlightWidth;
    if (!std::isfinite(edgeHighlightStrength))
        edgeHighlightStrength = kDefaultEdgeHighlightStrength;
    borderWidth = std::clamp(borderWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    edgeHighlightWidth = std::clamp(edgeHighlightWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    edgeHighlightStrength = std::clamp(
        edgeHighlightStrength, 0.0f, 1.0f);
    if (storedEdgeHighlightEnabled.empty() &&
        !edgeHighlightEnabledDeclared && glassEnabled)
        borderAlpha = 0.0f;

    lua_pop(state, 1);
    return true;
}

std::vector<WidgetErrorEntry> WidgetEngine::GetWidgetErrors() const
{
    std::vector<WidgetErrorEntry> result;
    for (const auto& kv : PersistentStorage())
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
    auto& storage = MutablePersistentStorage();
    for (auto it = storage.begin(); it != storage.end(); )
    {
        if (EndsWithLastError(it->first))
            it = storage.erase(it);
        else
            ++it;
    }
    SaveStorageFile();
}

bool WidgetEngine::ReloadWidget(const std::wstring& widgetId,
    bool* deterministicFailure)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return false;
    WidgetExecutionContextGuard reloadContext(d2dState_, widgetId);
    const std::string packageId = widgets_[idx].packageId;
    if (!snowdesktop::widget_runtime::HasStorageOverlay())
    {
        auto& manager = GetWidgetPackageManager();
        bool permissionScopeChanged = false;
        std::string permissionRefreshError;
        if (!manager.RefreshChangedPermissionScope(packageId,
                permissionScopeChanged, permissionRefreshError))
        {
            RuntimeRecordError(widgetId,
                "Widget permission scope refresh failed: " +
                    permissionRefreshError);
            return false;
        }
        if (permissionScopeChanged)
        {
            const auto package = manager.Resolve(packageId);
            if (!package)
            {
                UnloadWidget(widgetId);
                return false;
            }
            const auto grant =
                snowdesktop::widget::WidgetPermissionBroker::Evaluate(
                    package->permissionState,
                    package->manifest.permissions,
                    package->manifest.optionalPermissions,
                    package->manifest.networkDomains,
                    package->grantedPermissions,
                    package->grantedNetworkDomains);
            if (grant.runtimeBlock !=
                snowdesktop::widget::PermissionRuntimeBlock::None)
            {
                // Retire the VM that still owns the previous grant. The host
                // placeholder now exposes the normal reauthorization action.
                UnloadWidget(widgetId);
                return false;
            }
        }
    }
    const auto layoutMetrics = widgets_[idx].layoutMetrics;
    std::wstring path = ResolveWidgetPath(
        Utf8ToWideLocal(packageId));
    if (path.empty())
        path = widgets_[idx].filePath;
    const size_t oldIndex = static_cast<size_t>(idx);
    if (!LoadWidget(path, widgetId, false, nullptr, nullptr, nullptr, deterministicFailure))
    {
        // The enclosing package verification owns rollback for all instances.
        if (!snowdesktop::widget_runtime::HasStorageOverlay())
            RecoverWidgetPackage(packageId, widgets_[idx].manifest.version);
        return false;
    }

    // Loading appends the replacement VM. Preserve the host metrics that were
    // already calculated for this instance before retiring the old VM.
    widgets_.back().layoutMetrics = layoutMetrics;

    // The new VM is now fully loaded. Only then retire the last-known-good VM.
    LuaWidget& old = widgets_[oldIndex];
    ClearNativeMarqueeComposition(old);
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
    StopAnimationFrames(old);
    if (httpService_) httpService_->CancelWidget(widgetId);
    if (old.state)
    {
        old.lifecycle.Release(old.state);
        luaL_unref(old.state, LUA_REGISTRYINDEX, old.ref);
        ReleasePackageImageResources(d2dState_, old.state);
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
    // This synchronous check performs no catalog mutation or callbacks. Keep
    // permission evaluation fresh without copying the manifest and locales
    // on every animated frame. The borrow ends before leaving this call.
    const auto* package = GetWidgetPackageManager().FindResolved(WidgetWideToUtf8(packageId));
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

std::optional<snowdesktop::widget_runtime::WidgetHostState>
WidgetEngine::GetWidgetRuntimeFailure(
    const std::wstring& widgetId) const
{
    if (const auto failure = widgetHostFailures_.find(widgetId);
        failure != widgetHostFailures_.end())
        return failure->second;
    return std::nullopt;
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
    std::string scopeHandle,
    snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration
        audioAnalysis, std::string eventId,
    std::optional<snowdesktop::widget_runtime::CalendarEventQueryOptions> calendarQuery)
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
    options.eventId = std::move(eventId);
    options.calendarQuery = std::move(calendarQuery);
    options.audioWaveform = audioAnalysis.waveform;
    options.audioSpectrum = audioAnalysis.spectrum;
    options.audioRms = audioAnalysis.rms;
    options.audioPeak = audioAnalysis.peak;
    options.audioWaveformPoints = audioAnalysis.waveformPoints;
    options.audioSpectrumBins = audioAnalysis.spectrumBins;
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

std::vector<snowdesktop::widget_runtime::WidgetResourcePoint>
WidgetEngine::RuntimeGetDataHistory(std::uint64_t subscriptionId, std::string& error,
    std::string_view adapterId, bool includeGpuDetails) const
{
    error.clear();
    const auto binding = dataBroker_ ? dataBroker_->SubscriptionSnapshot(subscriptionId) : std::nullopt;
    if (!binding) { error = "unsubscribed"; return {}; }
    if (previewOnly_ || binding->options.preview) return {};
    if (!binding->options.permissionGranted) { error = "permissionDenied"; return {}; }
    if (binding->topic != "system.cpu" && binding->topic != "system.memory" &&
        binding->topic != "system.gpu" && binding->topic != "system.network.traffic")
    {
        error = "historyUnavailable";
        return {};
    }
    if (!widgetSystemDataProvider_) { error = "providerUnavailable"; return {}; }
    if (binding->topic == "system.gpu")
    {
        if (!includeGpuDetails) { error = "detailsRequired"; return {}; }
        if (adapterId.empty()) { error = "adapterRequired"; return {}; }
        const auto snapshot = widgetSystemDataProvider_->Gpu(true);
        if (snapshot)
        {
            for (const auto& adapter : snapshot->adapters)
                if (adapter.id == adapterId ||
                    std::find(adapter.aliasIds.begin(), adapter.aliasIds.end(), adapterId) != adapter.aliasIds.end())
                    return widgetSystemDataProvider_->ResourceHistory(binding->topic, adapter.id);
        }
        error = "adapterNotFound";
        return {};
    }
    if (!adapterId.empty()) { error = "invalidHistoryOptions"; return {}; }
    return widgetSystemDataProvider_->ResourceHistory(binding->topic);
}

std::optional<LuaWidgetDataSnapshot>
WidgetEngine::RuntimeGetDataSnapshot(
    std::uint64_t subscriptionId, bool includeGpuDetails) const
{
    if (subscriptionId == 0 || !dataBroker_) return std::nullopt;
    const auto binding = dataBroker_->SubscriptionSnapshot(subscriptionId);
    if (!binding) return std::nullopt;

    LuaWidgetDataSnapshot result;
    result.topic = binding->topic;
    const auto timestampNow = binding->options.preview
        ? snowdesktop::widget_runtime::PreviewWallClockMilliseconds
        : std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    if (binding->options.preview)
    {
        LuaWidgetPreviewDataState previewState =
            LuaWidgetPreviewDataState::Ready;
        const int previewWidgetIndex = FindWidget(
            Utf8ToWideLocal(binding->instanceId));
        if (previewWidgetIndex >= 0)
            previewState = widgets_[previewWidgetIndex].previewDataState;
        if (previewState == LuaWidgetPreviewDataState::PermissionDenied)
        {
            result.error = "permissionDenied";
            return result;
        }
        if (previewState == LuaWidgetPreviewDataState::Loading)
        {
            result.warmingUp = true;
            return result;
        }
        if (previewState == LuaWidgetPreviewDataState::Error)
        {
            result.error = "providerUnavailable";
            return result;
        }
        result.available = true;
        result.stale = false;
        result.timestampMs = timestampNow;
        if (snowdesktop::widget_runtime::IsSystemControlDataTopic(result.topic))
            result.systemControl = snowdesktop::widget_runtime::PreviewSystemControlData(
                result.topic, previewState == LuaWidgetPreviewDataState::Empty);
        if (previewState == LuaWidgetPreviewDataState::Empty &&
            result.topic != "calendar.events")
        {
            if (result.topic == "audio.input.volume") result.available = false;
            return result;
        }
        if (result.topic == "system.cpu")
        {
            result.cpu.available = true;
            result.cpu.warmingUp = false;
            result.cpu.usagePercent = 42.0;
            result.cpu.logicalProcessors = 12;
            result.cpu.name = _L("app.widget_preview.api.cpu");
            result.cpu.timestampMs = timestampNow;
        }
        else if (result.topic == "system.memory")
        {
            result.memory.available = true;
            result.memory.totalBytes = 16ull * 1024 * 1024 * 1024;
            result.memory.usedBytes = 9ull * 1024 * 1024 * 1024;
            result.memory.freeBytes = 7ull * 1024 * 1024 * 1024;
            result.memory.commitLimitBytes = 32ull * 1024 * 1024 * 1024;
            result.memory.commitUsedBytes = 13ull * 1024 * 1024 * 1024;
            result.memory.commitAvailableBytes =
                19ull * 1024 * 1024 * 1024;
            result.memory.usagePercent = 56.25;
            result.memory.timestampMs = timestampNow;
        }
        else if (result.topic == "process.summary")
        {
            result.processSummary.available = true;
            result.processSummary.warmingUp = false;
            result.processSummary.processes = {
                { "process-preview-editor", "Editor.exe", 18.5,
                    640ull * 1024 * 1024, 512ull * 1024 * 1024 },
                { "process-preview-player", "Player.exe", 7.25,
                    384ull * 1024 * 1024, 320ull * 1024 * 1024 },
            };
            result.processSummary.observedCount = 24;
            result.processSummary.truncated = true;
            result.processSummary.timestampMs = timestampNow;
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
        else if (result.topic == "system.screenTime")
        {
            result.screenTime.available = true;
            result.screenTime.enabled = true;
            result.screenTime.totalSeconds = 5400;
            result.screenTime.hours[9] = 1800;
            result.screenTime.hours[10] = 3600;
            wchar_t windows[MAX_PATH]{};
            const UINT windowsLength = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
            const auto directory = windowsLength && windowsLength < ARRAYSIZE(windows)
                ? WidgetWideToUtf8(std::wstring(windows, windowsLength)) : std::string{};
            result.screenTime.apps.push_back(snowdesktop::widget_runtime::MakeScreenTimeAppSnapshot("File Explorer", 3600,
                directory.empty() ? std::string{} : "path:" + directory + "\\explorer.exe"));
            result.screenTime.apps.push_back(snowdesktop::widget_runtime::MakeScreenTimeAppSnapshot("Notepad", 1800,
                directory.empty() ? std::string{} : "path:" + directory + "\\System32\\notepad.exe"));
            result.screenTime.timestampMs = timestampNow;
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
                { "adapter-1", _L("app.widget_preview.api.gpu"), 38.0,
                    8ull * 1024 * 1024 * 1024,
                    3ull * 1024 * 1024 * 1024,
                    8ull * 1024 * 1024 * 1024,
                    1ull * 1024 * 1024 * 1024 }
            };
            auto& adapter = result.gpu.adapters.front();
            adapter.usageAvailable = true;
            adapter.dedicatedUsageAvailable = true;
            adapter.sharedUsageAvailable = true;
            adapter.engines = { { 0, 0, "3D", 38.0, 38.0, 2 }, { 0, 1, "Copy", 4.0, 4.0, 1 } };
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
                    WidgetAudioAnalysisProvider::MaximumWaveformPoints);
            for (std::size_t index = 0;
                index < result.audioAnalysis.waveform.size(); ++index)
            {
                result.audioAnalysis.waveform[index] =
                    0.62 * std::sin(
                        static_cast<double>(index) * 0.22);
            }
            result.audioAnalysis.spectrum.resize(
                snowdesktop::widget_runtime::
                    WidgetAudioAnalysisProvider::MaximumSpectrumBins);
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
            session.sourceName = "SnowDesktop";
            session.title = _L("app.widget_preview.api.media_title");
            session.artist = _L("app.widget_preview.api.media_artist");
            session.album = _L("app.widget_preview.api.media_album");
            session.playbackStatus = "playing";
            session.current = true;
            session.shuffle = false;
            session.repeatMode = "list";
            session.playbackRate = 1.25;
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
            session.timeline.playbackRate = session.playbackRate;
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
                { "preview-1", 1,
                    _L("app.widget_preview.api.calendar_review"),
                    "2026-08-02", false, 600, 660,
                    _L("app.widget_preview.api.calendar_review_notes"),
                    15, {} },
                { "preview-2", 1,
                    _L("app.widget_preview.api.calendar_publish"),
                    "2026-08-03",
                    true, 0, 0, {}, -1, {} },
            };
            for (const auto& event : samples)
            {
                if (previewState != LuaWidgetPreviewDataState::Empty &&
                    ((!binding->options.eventId.empty() &&
                        event.id == binding->options.eventId) ||
                    (binding->options.eventId.empty() &&
                        event.date >= result.calendarRangeStart &&
                        event.date <= result.calendarRangeEnd)))
                {
                    result.calendarEvents.push_back(event);
                }
            }
            if (!binding->options.eventId.empty())
            {
                result.calendarRangeStart.clear();
                result.calendarRangeEnd.clear();
            }
            result.calendarRevision = 1;
            if (binding->options.calendarQuery)
            {
                auto page = snowdesktop::widget_runtime::QueryCalendarEvents(
                    result.calendarEvents, *binding->options.calendarQuery);
                result.calendarEvents = std::move(page.events);
                result.calendarTotal = page.total;
                result.calendarHasMore = page.hasMore;
                result.calendarNextOffset = page.nextOffset;
                result.calendarTruncated = page.total > result.calendarEvents.size();
            }
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
        if (previewState == LuaWidgetPreviewDataState::Stale)
        {
            result.stale = true;
            result.timestampMs = timestampNow -
                binding->options.requestedInterval.count() *
                    snowdesktop::widget_runtime::
                        DataSnapshotStaleIntervalCount - 1;
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
        result.stale = snowdesktop::widget_runtime::IsDataSnapshotStale(
            timestampNow, timestamp, binding->options.requestedInterval);
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
    else if (result.topic == "process.summary")
    {
        const auto snapshot = widgetSystemDataProvider_->ProcessSummary();
        if (snapshot)
        {
            result.processSummary = *snapshot;
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
    else if (result.topic == "system.screenTime")
    {
        const auto snapshot = widgetSystemDataProvider_->ScreenTime();
        if (snapshot)
        {
            result.screenTime = *snapshot;
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
        const auto snapshot = widgetSystemDataProvider_->Gpu(includeGpuDetails);
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
    else if (snowdesktop::widget_runtime::IsSystemControlDataTopic(result.topic))
    {
        const auto snapshot = widgetSystemDataProvider_->Controls()->Current(result.topic);
        if (snapshot)
        {
            result.systemControl = snapshot->value;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
            if (result.topic == "audio.input.volume" && result.available)
            {
                result.systemControl.object["minimum"] = snowdesktop::system_control::json::Number(0);
                result.systemControl.object["maximum"] = snowdesktop::system_control::json::Number(1);
            }
        }
        else result.warmingUp = true;
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
        if (!calendarService_ || !calendarServiceLoaded_)
        {
            result.error = "providerUnavailable";
        }
        else if (!binding->options.eventId.empty())
        {
            const auto event = calendarService_->EventById(binding->options.eventId, true);
            if (event) result.calendarEvents.push_back(*event);
            result.calendarRevision = calendarEventsRevision_;
            result.available = true;
            setFreshness(timestampNow);
        }
        else
        {
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
                    result.calendarRangeStart, result.calendarRangeEnd, true);
                constexpr std::size_t MaximumEvents = 512;
                if (binding->options.calendarQuery)
                {
                    auto page = snowdesktop::widget_runtime::QueryCalendarEvents(
                        result.calendarEvents, *binding->options.calendarQuery);
                    result.calendarEvents = std::move(page.events);
                    result.calendarTotal = page.total;
                    result.calendarHasMore = page.hasMore;
                    result.calendarNextOffset = page.nextOffset;
                    result.calendarTruncated = page.total > result.calendarEvents.size();
                }
                else
                {
                    result.calendarTruncated =
                        result.calendarEvents.size() > MaximumEvents;
                    if (result.calendarTruncated)
                        result.calendarEvents.resize(MaximumEvents);
                }
                result.calendarRevision = calendarEventsRevision_;
                result.available = true;
                setFreshness(timestampNow);
            }
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
    if (result.topic == "audio.output.analysis")
    {
        result.audioAnalysis = snowdesktop::widget_runtime::
            ProjectWidgetAudioAnalysisSnapshot(result.audioAnalysis,
                { binding->options.audioWaveform,
                    binding->options.audioSpectrum,
                    binding->options.audioRms,
                    binding->options.audioPeak,
                    binding->options.audioWaveformPoints,
                    binding->options.audioSpectrumBins });
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
    bool openedCircuit = false;
    if (!previewOnly_ && !snowdesktop::widget_runtime::IsDryLoad() &&
        (index < 0 || !widgets_[index].preview))
    {
        MutablePersistentStorage()[idUtf8 + ".lastError"] = message;
        SaveStorageFile();
    }
    if (index >= 0)
    {
        auto& widget = widgets_[index];
        const bool wasOpen = widget.health.CircuitOpen();
        if (widget.health.RecordError())
            widget.valid = false;
        openedCircuit = !wasOpen && widget.health.CircuitOpen();
        const bool quotaExceeded = widget.quota &&
            (widget.quota->memoryExceeded ||
                widget.quota->executionExceeded);
        RecordWidgetHostFailure(widgetId, message, quotaExceeded,
            widget.health.CircuitOpen());
    }
    else
        RecordWidgetHostFailure(widgetId, message);
    RuntimeAddLog(widgetId, "error", message);
    if (openedCircuit)
        RuntimeInvalidateHost(widgetId);
}

void WidgetEngine::RuntimeRecordSuccess(const std::wstring& widgetId)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return;
    auto& widget = widgets_[index];
    if (!widget.valid ||
        (widget.health.ConsecutiveErrors() == 0 &&
            !widget.health.RecoveryProbe()))
    {
        return;
    }
    const bool recovered = widget.health.RecoveryProbe();
    widget.health.RecordSuccess();
    widgetHostFailures_.erase(widgetId);
    if (recovered)
    {
        RuntimeAddLog(widgetId, "info",
            "Automatic runtime recovery probe succeeded");
    }
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
        const bool subscribed = std::any_of(
            widget.dataSubscriptions.begin(),
            widget.dataSubscriptions.end(),
            [&topic](const auto& entry) {
                return entry.second == topic;
            });
        if (!subscribed) continue;
        targets.push_back(widget.widgetId);
    }
    for (const auto& widgetId : targets)
    {
        const int index = FindWidget(widgetId);
        if (index < 0) continue;
        LuaWidget& widget = widgets_[index];
        (void)InvokeLifecycleEvent(widget, "data.change",
            [&topic, revision](lua_State* eventState) {
                lua_pushlstring(eventState, topic.data(), topic.size());
                lua_setfield(eventState, -2, "topic");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(revision));
                lua_setfield(eventState, -2, "revision");
            });
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
        if (!item->second.available) return std::nullopt;
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
        if (!app->second.available) return std::nullopt;
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

std::optional<std::wstring> WidgetEngine::RuntimeResolveViewReference(
    const std::wstring& widgetId, const std::string& reference) const
{
    if (reference.empty()) return std::nullopt;
    const auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end()) return std::nullopt;
    return RuntimeResolveItemReference(
        widgetId, widget->runtimeToken, reference);
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
                        item.title, item.source, item.type, true,
                        item.available });
            }
            else
            {
                widget.itemReferences.insert_or_assign(item.reference,
                    LuaWidget::ItemReference{ item.target, "logical.slot",
                        0, item.title, item.source, item.type,
                        item.kind, true, item.available });
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
        !widget->viewTree)
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

    const auto collect = [&](const auto& self,
        const ViewNode& node) -> bool {
        if (!node.visible || node.visibility ==
                snowdesktop::widget_runtime::ViewVisibility::Hidden)
            return false;
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
                const bool declaredEmptyContent = snapshot->items.empty() &&
                    node.collectionContent ==
                        snowdesktop::widget_runtime::
                            ViewCollectionContent::Empty;
                if ((declaredEmptyContent &&
                        (!sceneItems.empty() || node.children.size() != 1)) ||
                    (!declaredEmptyContent &&
                        (sceneItems.size() != snapshot->items.size() ||
                         node.children.size() != snapshot->items.size())))
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
            const ViewRect transformedSurface =
                snowdesktop::widget_runtime::ApplyViewTransform(node.frame,
                    snowdesktop::widget_runtime::ResolveViewTransformForKey(
                        *widget->viewTree, node.key));
            const auto visibleSurface = IntersectLogicalSlotFrames(
                snowdesktop::widget_runtime::ResolveViewClipForKey(
                    *widget->viewTree, node.key, false),
                transformedSurface);
            if (!visibleSurface) return false;
            result.bounds = LogicalSlotClientRect(*widget, *visibleSurface);
            if (result.bounds.right <= result.bounds.left ||
                result.bounds.bottom <= result.bounds.top)
                return false;
            result.dropStyle =
                snowdesktop::widget_runtime::ResolveViewThemeStyle(
                    node.dropStyle,
                    BuildWidgetViewThemePalette(d2dState_, widgetId));

            for (const auto& child : node.children)
            {
                if (child.type != ViewNodeType::SlotItem || !child.visible)
                    continue;
                const ViewRect transformedItem =
                    snowdesktop::widget_runtime::ApplyViewTransform(
                        child.frame,
                        snowdesktop::widget_runtime::
                            ResolveViewTransformForKey(
                                *widget->viewTree, child.key));
                auto visibleItem = IntersectLogicalSlotFrames(
                    visibleSurface, transformedItem);
                if (visibleItem)
                    visibleItem = IntersectLogicalSlotFrames(
                        snowdesktop::widget_runtime::ResolveViewClipForKey(
                            *widget->viewTree, child.key, false),
                        *visibleItem);
                if (!visibleItem) continue;
                RECT bounds = LogicalSlotClientRect(*widget, *visibleItem);
                if (bounds.right <= bounds.left ||
                    bounds.bottom <= bounds.top)
                    continue;
                result.items.push_back({ child.key, bounds });
            }
            return true;
        }

        for (const auto& child : node.children)
            if (self(self, child)) return true;
        return false;
    };

    if (!collect(collect, *widget->viewTree))
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
    if (widget == widgets_.end() || widget->preview || !widget->valid)
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
    request.ownerToken = ownerToken;
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
            if (!eventChange.relatedSlotId.empty())
            {
                lua_pushlstring(state, eventChange.relatedSlotId.data(),
                    eventChange.relatedSlotId.size());
                lua_setfield(state, -2, "relatedSlotId");
                lua_pushinteger(state,
                    static_cast<lua_Integer>(
                        eventChange.relatedRevision));
                lua_setfield(state, -2, "relatedRevision");
            }
            lua_pushlstring(state, eventSource.data(), eventSource.size());
            lua_setfield(state, -2, "source");
        });
}

void WidgetEngine::RefreshLogicalSlotAvailability()
{
    LuaApplicationCatalogSnapshot catalog;
    bool catalogReady = false;
    if (applicationCatalogProvider_)
    {
        try
        {
            catalog = applicationCatalogProvider_();
            catalogReady = catalog.state == "ready";
        }
        catch (...)
        {
            catalogReady = false;
        }
    }

    std::unordered_set<std::wstring> availableTargets;
    if (catalogReady)
    {
        availableTargets.reserve(catalog.entries.size());
        for (const auto& entry : catalog.entries)
        {
            std::wstring target = ToUpperInvariant(
                Utf8ToWideLocal(entry.launchTarget));
            if (!target.empty())
                availableTargets.insert(std::move(target));
        }
    }

    struct AvailabilityChange
    {
        snowdesktop::widget_runtime::LogicalSlotChange change;
        std::string source;
    };
    struct ChangedWidget
    {
        std::size_t index = 0;
        snowdesktop::widget_runtime::LogicalSlotModel previous;
        std::vector<AvailabilityChange> changes;
    };
    std::vector<ChangedWidget> changedWidgets;
    const auto previousStorage = ActiveStorage();
    for (std::size_t widgetIndex = 0;
        widgetIndex < widgets_.size(); ++widgetIndex)
    {
        auto& widget = widgets_[widgetIndex];
        if (!widget.valid || widget.preview)
            continue;

        ChangedWidget changed;
        changed.index = widgetIndex;
        changed.previous = widget.logicalSlots;
        const auto expectedTypeForSlot = [&](std::string_view slotId) {
            const auto find = [&](const auto& settings) -> std::string_view {
                for (const auto& setting : settings)
                    if (setting.binding == slotId)
                        return EntityReferenceSettingTypeFilter(
                            setting.type);
                return {};
            };
            std::string_view result = find(widget.manifest.settings);
            return result.empty() ? find(widget.scriptSettings) : result;
        };
        for (const auto& snapshot : widget.logicalSlots.Snapshots())
        {
            for (const auto& item : snapshot.items)
            {
                bool available = item.available;
                std::string source;
                if (item.kind == "app.reference")
                {
                    const std::wstring target = snowdesktop::
                        logical_slot_picker_rules::
                            NormalizeApplicationLaunchTarget(
                                Utf8ToWideLocal(item.target));
                    if (snowdesktop::logical_slot_picker_rules::
                            IsFilesystemApplicationLaunchTarget(target))
                    {
                        const DWORD attributes = target.empty()
                            ? INVALID_FILE_ATTRIBUTES
                            : GetFileAttributesW(target.c_str());
                        available = attributes != INVALID_FILE_ATTRIBUTES &&
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
                        source = "host.filesystem";
                    }
                    else
                    {
                        if (!catalogReady) continue;
                        const std::wstring catalogTarget =
                            ToUpperInvariant(target);
                        available = !catalogTarget.empty() &&
                            availableTargets.contains(catalogTarget);
                        source = "host.catalog";
                    }
                }
                else if (item.kind == "filesystem.reference")
                {
                    const std::string_view expectedType =
                        expectedTypeForSlot(snapshot.id);
                    const std::wstring target =
                        Utf8ToWideLocal(item.target);
                    const DWORD attributes = target.empty()
                        ? INVALID_FILE_ATTRIBUTES
                        : GetFileAttributesW(target.c_str());
                    available = attributes != INVALID_FILE_ATTRIBUTES;
                    if (available && !expectedType.empty() &&
                        item.type != expectedType)
                        available = false;
                    const std::string_view effectiveType =
                        expectedType.empty()
                        ? std::string_view(item.type) : expectedType;
                    if (available && effectiveType == "folder")
                        available =
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    else if (available && effectiveType == "file")
                        available =
                            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
                    source = "host.filesystem";
                }
                else continue;
                snowdesktop::widget_runtime::LogicalSlotChange change;
                std::string error;
                if (!widget.logicalSlots.SetAvailability(snapshot.id,
                        item.id, available, change, error))
                {
                    OutputDebugStringA((
                        "SnowDesktop logical slot availability: " +
                        error + "\n").c_str());
                    continue;
                }
                if (change.operation != "unchanged")
                    changed.changes.push_back({
                        std::move(change), std::move(source) });
            }
        }
        if (!changed.changes.empty())
        {
            widget.logicalSlots.Export(ActiveStorage(),
                WidgetWideToUtf8(widget.widgetId));
            changedWidgets.push_back(std::move(changed));
        }
    }
    if (changedWidgets.empty()) return;

    if (!snowdesktop::widget_runtime::HasStorageOverlay() &&
        !SaveStorageFile())
    {
        ActiveStorage() = previousStorage;
        for (auto& changed : changedWidgets)
            widgets_[changed.index].logicalSlots =
                std::move(changed.previous);
        return;
    }

    for (const auto& changed : changedWidgets)
    {
        auto& widget = widgets_[changed.index];
        RebuildLogicalSlotReferences(widget);
        RuntimeInvalidateHost(widget.widgetId);
        for (const auto& change : changed.changes)
            DispatchHostLogicalSlotChange(
                widget, change.change, change.source);
    }
}

bool WidgetEngine::RuntimeRemoveHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view slotId,
    std::string_view itemId,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
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
    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

bool WidgetEngine::RuntimeMoveHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view slotId,
    std::string_view itemId, std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
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
    DispatchHostLogicalSlotChange(*widget, change, source);
    return true;
}

bool WidgetEngine::RuntimeTransferHostLogicalSlotItem(
    const std::wstring& widgetId, std::string_view sourceSlotId,
    std::string_view itemId, std::string_view targetSlotId,
    std::size_t targetIndex,
    snowdesktop::widget_runtime::LogicalSlotChange& change,
    std::string& error, std::string_view source)
{
    error.clear();
    auto widget = std::find_if(widgets_.begin(), widgets_.end(),
        [&widgetId](const LuaWidget& candidate) {
            return candidate.widgetId == widgetId;
        });
    if (widget == widgets_.end() || widget->preview || !widget->valid)
    {
        error = "logical slot host owner is unavailable";
        return false;
    }
    auto previousModel = widget->logicalSlots;
    auto previousStorage = ActiveStorage();
    if (!widget->logicalSlots.Transfer(sourceSlotId, itemId,
            targetSlotId, targetIndex, change, error))
        return false;
    if (!CommitLogicalSlotMutation(*this, *widget,
            std::move(previousModel), std::move(previousStorage),
            change, error))
        return false;
    if (widget->logicalSlotFocus &&
        widget->logicalSlotFocus->slotId == sourceSlotId &&
        widget->logicalSlotFocus->itemId == itemId)
        widget->logicalSlotFocus->slotId = std::string(targetSlotId);
    DispatchHostLogicalSlotChange(*widget, change, source);
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
        if (!app->second.available)
        {
            error = "unavailableReference";
            return false;
        }
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
        if (!item->second.available)
        {
            error = "unavailableReference";
            return false;
        }
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
    if (widget == widgets_.end() || widget->preview || !widget->valid)
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
    if (widget == widgets_.end() || widget->preview || !widget->valid)
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

void WidgetEngine::RuntimeInvalidateHost(const std::wstring& widgetId,
    std::optional<RECT> dirtyRect, std::string_view surface)
{
    snowdesktop::performance::Scope performanceScope(
        "widget.invalidate", surface.empty() ? "current" : surface, widgetId);
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    // Calls made while evaluating or interacting with an auxiliary surface
    // belong to that surface unless the caller explicitly names another one.
    // Keeping an empty surface here used to invalidate the panel and the
    // desktop copy together, which could attempt a second DComp BeginDraw
    // while the shared popup surface was still being rendered.
    if (surface.empty())
    {
        const std::string_view currentSurface =
            CurrentWidgetSurface(d2dState_);
        if (IsPanelSurface(currentSurface))
            surface = currentSurface;
    }
    snowdesktop::performance::DrawLink(
        surface.empty() ? "desktop" : surface, widgetId, false);
    const int index = widgetId.empty() ? -1 : FindWidget(widgetId);
    if (index >= 0)
    {
        auto& widget = widgets_[index];
        if (surface.empty() || surface == "desktop")
            widget.desktopMarquee.requiresLuaRender = true;
        if (surface.empty() || IsPanelSurface(surface))
            widget.panelMarquee.requiresLuaRender = true;
    }
    invalidationBatch_.Invalidate(
        invalidateCallback_, widgetId, dirtyRect, surface);
}

bool WidgetEngine::RuntimeNotifySettingsChanged(
    const std::wstring& widgetId, std::vector<std::string> keys,
    bool preview)
{
    const int index = FindWidget(widgetId);
    if (index < 0 ||
        !WidgetDeclaresFeature(widgets_[index], "settings.changeEvent"))
        return false;
    keys.erase(std::remove_if(keys.begin(), keys.end(),
        [](const std::string& key) { return key.empty(); }), keys.end());
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return InvokeLifecycleEvent(widgets_[index], "settings.changed",
        [keys = std::move(keys), preview](lua_State* eventState) {
            lua_createtable(eventState, static_cast<int>(keys.size()), 0);
            for (std::size_t index = 0; index < keys.size(); ++index)
            {
                lua_pushlstring(eventState,
                    keys[index].data(), keys[index].size());
                lua_rawseti(eventState, -2,
                    static_cast<lua_Integer>(index + 1));
            }
            lua_setfield(eventState, -2, "keys");
            lua_pushboolean(eventState, preview ? 1 : 0);
            lua_setfield(eventState, -2, "preview");
        });
}

bool WidgetEngine::RuntimeSubmitNativeMarquee(
    const std::wstring& widgetId,
    LuaWidget::NativeMarqueeText marquee, std::string& error)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "host widget instance is unavailable";
        return false;
    }
    auto& widget = widgets_[index];
    if (!WidgetDeclaresNativeMarquee(widget))
    {
        error = "the package must declare draw.marqueeText as a required or optional feature";
        return false;
    }
    if (CurrentWidgetSurface(d2dState_) != "desktop")
    {
        error = "only the desktop render surface is supported";
        return false;
    }
    auto& surface = widget.desktopMarquee;
    if (!surface.collecting)
    {
        error = "the API is only available while the host records render()";
        return false;
    }
    if (surface.pendingMarquees.size() >= 32)
    {
        error = "a render may submit at most 32 marquee texts";
        return false;
    }
    if (std::any_of(surface.pendingMarquees.begin(),
            surface.pendingMarquees.end(), [&marquee](const auto& value) {
                return value.key == marquee.key;
            }))
    {
        error = "duplicate key: " + marquee.key;
        return false;
    }
    surface.pendingMarquees.push_back(std::move(marquee));
    error.clear();
    return true;
}

bool WidgetEngine::RuntimeSubmitInteractionRegion(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::InteractionRegion region,
    std::string& error)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "interaction regions require API v2";
        return false;
    }
    auto& regions = InteractionRegionsForSurface(
        widgets_[index], CurrentWidgetSurface(d2dState_));
    return regions.Submit(
        std::move(region), error);
}

bool WidgetEngine::RuntimeInteractionHovered(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        InteractionRegionsForSurface(widgets_[index],
            CurrentWidgetSurface(d2dState_)).IsHovered(key);
}

bool WidgetEngine::RuntimeInteractionPressed(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        InteractionRegionsForSurface(widgets_[index],
            CurrentWidgetSurface(d2dState_)).IsPressed(key);
}

bool WidgetEngine::RuntimeInteractionFocused(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && !key.empty() &&
        ViewFocusForSurface(widgets_[index],
            CurrentWidgetSurface(d2dState_)) == key;
}

void WidgetEngine::DispatchInteractionAction(LuaWidget& widget,
    const std::string& targetKey, const char* eventName,
    int x, int y, int button, int delta, int clickCount,
    bool includeRetired, const char* source, int keyboardStepDirection,
    std::optional<float> requestedControlValue,
    std::optional<std::vector<std::string>> requestedSelectedKeys,
    std::string_view surface)
{
    if (targetKey.empty() || !eventName || !*eventName) return;
    snowdesktop::widget_runtime::InteractionAction action;
    std::string resolvedEventName(eventName);
    std::optional<bool> previousChecked;
    std::optional<bool> checked;
    std::optional<bool> previousIndeterminate;
    std::optional<bool> indeterminate;
    std::optional<float> previousControlValue;
    std::optional<float> controlValue;
    std::optional<std::string> previousSelection;
    std::optional<std::string> selection;
    std::optional<std::vector<std::string>> previousSelectedKeys;
    std::optional<std::vector<std::string>> selectedKeys;
    std::optional<bool> previousExpanded;
    std::optional<bool> expanded;
    bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "pointer";
    const std::string eventSurface =
        NormalizeWidgetSurface(surface);
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, eventSurface);
    if (requestedSelectedKeys)
    {
        const auto* region = interactionRegions.Find(targetKey);
        const auto* actionPointer = interactionRegions.FindAction(
            targetKey, "change");
        if (!region || !actionPointer ||
            (region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::SelectionSingle &&
                region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::SelectionMultiple))
            return;
        action = *actionPointer;
        resolvedEventName = "change";
        previousSelectedKeys = region->currentSelectedKeys;
        selectedKeys = std::move(requestedSelectedKeys);
    }
    else if (includeRetired)
    {
        const auto* actionPointer =
            interactionRegions.FindTransitionAction(
                targetKey, eventName);
        if (!actionPointer) return;
        action = *actionPointer;
    }
    else
    {
        const auto resolved = requestedControlValue
            ? interactionRegions.ResolveRangeValue(
                targetKey, *requestedControlValue)
            : keyboardStepDirection == 0
                ? interactionRegions.ResolveAction(
                    targetKey, eventName, static_cast<float>(x),
                    static_cast<float>(y), button)
                : interactionRegions.ResolveKeyboardStep(
                    targetKey, keyboardStepDirection);
        if (!resolved) return;
        action = resolved->action;
        resolvedEventName = resolved->eventName;
        previousChecked = resolved->previousChecked;
        checked = resolved->checked;
        previousIndeterminate = resolved->previousIndeterminate;
        indeterminate = resolved->indeterminate;
        previousControlValue = resolved->previousControlValue;
        controlValue = resolved->controlValue;
        previousSelection = resolved->previousSelection;
        selection = resolved->selection;
        previousSelectedKeys = resolved->previousSelectedKeys;
        selectedKeys = resolved->selectedKeys;
        previousExpanded = resolved->previousExpanded;
        expanded = resolved->expanded;
    }
    // Only a host-owned pressed slider extends the initiating pointer gesture.
    // Merely moving over an element never grants task privileges.
    if (!trustedGesture && resolvedEventName == "change" && eventSource == "pointer" &&
        interactionRegions.PressedKey() == targetKey)
    {
        const auto* range = interactionRegions.Find(targetKey);
        trustedGesture = range && range->controlKind ==
            snowdesktop::widget_runtime::InteractionControlKind::Slider;
    }
    snowdesktop::widget_runtime::WidgetTrustedGestureScope actionGesture(
        trustedGestureState_, trustedGesture);
    const std::string popupOpener = trustedGesture && eventSurface == "desktop"
        ? targetKey : std::string{};
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str(), false, popupOpener);
    (void)InvokeLifecycleEvent(widget, "action",
        [&action, &targetKey, &resolvedEventName,
            previousChecked, checked, previousIndeterminate, indeterminate,
            previousControlValue, controlValue,
            previousSelection, selection,
            previousSelectedKeys, selectedKeys,
            previousExpanded, expanded,
            trustedGesture, &eventSource, &eventSurface, x, y, button,
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
            if (previousIndeterminate.has_value() &&
                indeterminate.has_value())
            {
                lua_pushboolean(eventState,
                    *previousIndeterminate ? 1 : 0);
                lua_setfield(eventState, -2, "previousIndeterminate");
                lua_pushboolean(eventState, *indeterminate ? 1 : 0);
                lua_setfield(eventState, -2, "indeterminate");
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
            if (previousSelectedKeys.has_value() &&
                selectedKeys.has_value())
            {
                const auto pushKeys = [eventState](
                    const std::vector<std::string>& keys) {
                    lua_createtable(eventState,
                        static_cast<int>(keys.size()), 0);
                    for (std::size_t index = 0; index < keys.size(); ++index)
                    {
                        lua_pushlstring(eventState, keys[index].data(),
                            keys[index].size());
                        lua_rawseti(eventState, -2,
                            static_cast<lua_Integer>(index + 1));
                    }
                };
                pushKeys(*previousSelectedKeys);
                lua_setfield(eventState, -2, "previousSelectedKeys");
                pushKeys(*selectedKeys);
                lua_setfield(eventState, -2, "selectedKeys");
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
            lua_pushlstring(eventState, eventSource.data(), eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
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
    std::optional<snowdesktop::widget_runtime::ViewTextSelection>
        proposedSelection;
    if (focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == targetKey &&
        focusedHostInput_.controlledSelection)
    {
        proposedSelection = snowdesktop::widget_runtime::ViewTextSelection{
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                focusedHostInput_.text,
                std::min(focusedHostInput_.selectionAnchor,
                    focusedHostInput_.cursor)),
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                focusedHostInput_.text,
                std::max(focusedHostInput_.selectionAnchor,
                    focusedHostInput_.cursor)),
        };
    }
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "keyboard";
    const std::string eventSurface = focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == targetKey
        ? focusedHostInput_.surface
        : std::string(CurrentWidgetSurface(d2dState_));
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str());
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, &previousUtf8, &proposedUtf8,
            numeric, previousNumber, proposedNumber, committed,
            cancelled, proposedSelection, trustedGesture,
            &eventSource, &eventSurface](lua_State* eventState) {
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
            if (proposedSelection)
            {
                lua_createtable(eventState, 0, 2);
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(proposedSelection->start));
                lua_setfield(eventState, -2, "start");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(proposedSelection->finish));
                lua_setfield(eventState, -2, "finish");
                lua_setfield(eventState, -2, "selection");
            }
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
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchHostInputSelectionChange(
    const std::wstring& widgetId, const std::string& targetKey,
    const snowdesktop::widget_runtime::InteractionAction& action,
    const std::wstring& text, size_t previousAnchor,
    size_t previousCursor, size_t anchor, size_t cursor,
    const char* source)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || action.id.empty()) return;
    const auto makeSelection = [&text](size_t left, size_t right) {
        return snowdesktop::widget_runtime::ViewTextSelection{
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                text, std::min(left, right)),
            snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset(
                text, std::max(left, right)),
        };
    };
    const auto previous = makeSelection(previousAnchor, previousCursor);
    const auto proposed = makeSelection(anchor, cursor);
    if (previous == proposed) return;
    const bool trustedGesture = trustedGestureState_.Active();
    const std::string eventSource = source && *source ? source : "keyboard";
    const std::string eventSurface = focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == targetKey
        ? focusedHostInput_.surface
        : std::string(CurrentWidgetSurface(d2dState_));
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str());
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, previous, proposed, trustedGesture,
            &eventSource, &eventSurface](lua_State* eventState) {
            const auto pushSelection = [eventState](
                const snowdesktop::widget_runtime::ViewTextSelection& value) {
                lua_createtable(eventState, 0, 2);
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(value.start));
                lua_setfield(eventState, -2, "start");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(value.finish));
                lua_setfield(eventState, -2, "finish");
            };
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, targetKey.data(), targetKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushliteral(eventState, "selectionChange");
            lua_setfield(eventState, -2, "action");
            pushSelection(previous);
            lua_setfield(eventState, -2, "previousSelection");
            pushSelection(proposed);
            lua_setfield(eventState, -2, "selection");
            lua_pushboolean(eventState, trustedGesture ? 1 : 0);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushlstring(eventState, eventSource.data(),
                eventSource.size());
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
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
    const std::string eventSurface = focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == targetKey
        ? focusedHostInput_.surface
        : std::string(CurrentWidgetSurface(d2dState_));
    WidgetSurfaceScope surfaceScope(d2dState_, eventSurface.c_str());
    (void)InvokeLifecycleEvent(widgets_[index], "action",
        [&action, &targetKey, &utf8, &name, cancelled,
            trustedGesture, &eventSource,
            &eventSurface](lua_State* eventState) {
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
            lua_pushlstring(eventState,
                eventSurface.data(), eventSurface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::DispatchInteractionTransition(LuaWidget& widget,
    const snowdesktop::widget_runtime::InteractionHoverTransition& transition,
    int x, int y, std::string_view surface)
{
    if (!transition.leftKey.empty() &&
        transition.leftKey != transition.enteredKey)
    {
        DispatchInteractionAction(widget, transition.leftKey,
            "pointerLeave", x, y, 0, 0, 0, true, "pointer", 0,
            std::nullopt, std::nullopt, surface);
    }
    if (!transition.enteredKey.empty() &&
        transition.enteredKey != transition.leftKey)
    {
        DispatchInteractionAction(widget, transition.enteredKey,
            "pointerEnter", x, y, 0, 0, 0, false, "pointer", 0,
            std::nullopt, std::nullopt, surface);
    }
}

void WidgetEngine::UpdateInteractionHover(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface)
{
    for (auto& widget : widgets_)
    {
        auto& interactionRegions =
            InteractionRegionsForSurface(widget, surface);
        const auto transition = widget.widgetId == widgetId
            ? interactionRegions.UpdateHover(
                static_cast<float>(x), static_cast<float>(y))
            : interactionRegions.ClearHover();
        if (!transition.Changed()) continue;
        const bool transitionHasAction =
            interactionRegions.HasTransitionAction(transition);
        DispatchInteractionTransition(widget, transition, x, y, surface);
        if (HasDeclarativeViewForSurface(widget, surface) &&
            !transitionHasAction)
            QueueDeclarativeVisualFrame(widget, surface);
        RuntimeInvalidateHost(widget.widgetId);
    }
}

void WidgetEngine::ClearInteractionHover(std::string_view surface)
{
    for (auto& widget : widgets_)
    {
        const auto clearSurface = [&](std::string_view targetSurface) {
            auto& regions = InteractionRegionsForSurface(
                widget, targetSurface);
            const auto transition = regions.ClearHover();
            if (!transition.Changed()) return;
            const bool transitionHasAction =
                regions.HasTransitionAction(transition);
            DispatchInteractionTransition(
                widget, transition, 0, 0, targetSurface);
            if (HasDeclarativeViewForSurface(widget, targetSurface) &&
                !transitionHasAction)
                QueueDeclarativeVisualFrame(widget, targetSurface);
            RuntimeInvalidateHost(widget.widgetId);
        };
        if (surface.empty())
        {
            clearSurface("desktop");
            if (widget.panelActive)
                clearSurface(widget.panelSurface);
        }
        else
            clearSurface(surface);
    }
}

std::string WidgetEngine::InteractionCursorAt(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return {};
    return InteractionRegionsForSurface(widgets_[index], surface).CursorAt(
        static_cast<float>(x), static_cast<float>(y));
}

bool WidgetEngine::RuntimeCanWriteWidgetStorage(
    const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index < 0 ||
        (!widgets_[index].interactionRegions.FrameOpen() &&
            !widgets_[index].panelFrameOpen);
}

bool WidgetEngine::RuntimeGetFilesystemSettingHandle(
    const std::wstring& widgetId, const std::string& key,
    std::string& handle) const
{
    handle.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const LuaWidgetManifest::Setting* declared = nullptr;
    const auto findSetting = [&](const auto& settings) {
        return std::find_if(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key &&
                    IsFilesystemHandleSettingType(setting.type);
            });
    };
    if (const auto found = findSetting(widget.manifest.settings);
        found != widget.manifest.settings.end())
        declared = &*found;
    else if (const auto scriptFound = findSetting(widget.scriptSettings);
        scriptFound != widget.scriptSettings.end())
        declared = &*scriptFound;
    if (!declared) return false;
    if (widget.preview || !filesystemHandleStore_) return true;

    const std::string prefix = WidgetWideToUtf8(widgetId) + ".";
    const auto stored = ActiveStorage().find(
        prefix + FilesystemSettingMetadataKey(key));
    if (stored == ActiveStorage().end()) return true;
    const auto entry = filesystemHandleStore_->Resolve(
        { WidgetWideToUtf8(widgetId), widget.packageId }, stored->second);
    const auto expectedAccess = FilesystemSettingAccess(declared->access);
    const auto expectedKind = declared->type == "folderHandle"
        ? snowdesktop::widget_runtime::WidgetFilesystemHandleKind::Folder
        : snowdesktop::widget_runtime::WidgetFilesystemHandleKind::File;
    if (!entry || !expectedAccess || entry->kind != expectedKind ||
        entry->access != *expectedAccess)
        return true;
    handle = entry->handle;
    return true;
}

bool WidgetEngine::RuntimeIsFilesystemSettingHandleValue(
    const std::wstring& widgetId, std::string_view handle) const
{
    if (handle.empty()) return false;
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const LuaWidget& widget = widgets_[index];
    const auto matches = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (!IsFilesystemHandleSettingType(setting.type)) continue;
            const auto stored = ActiveStorage().find(
                WidgetWideToUtf8(widgetId) + "." +
                    FilesystemSettingMetadataKey(setting.key));
            if (stored != ActiveStorage().end() && stored->second == handle)
                return true;
        }
        return false;
    };
    return matches(widget.manifest.settings) ||
        matches(widget.scriptSettings);
}

std::vector<std::string> WidgetEngine::RuntimeFilesystemSettingKeys(
    const std::wstring& widgetId) const
{
    std::vector<std::string> result;
    const int index = FindWidget(widgetId);
    if (index < 0) return result;
    const LuaWidget& widget = widgets_[index];
    const auto append = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (!IsFilesystemHandleSettingType(setting.type)) continue;
            std::string handle;
            if (RuntimeGetFilesystemSettingHandle(
                    widgetId, setting.key, handle) && !handle.empty() &&
                std::find(result.begin(), result.end(), setting.key) ==
                    result.end())
                result.push_back(setting.key);
        }
    };
    append(widget.manifest.settings);
    append(widget.scriptSettings);
    std::sort(result.begin(), result.end());
    return result;
}

std::string WidgetEngine::RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const
{
    std::string secretReference;
    if (RuntimeGetSecretReference(widgetId, key, secretReference))
        return secretReference;
    std::string filesystemHandle;
    if (RuntimeGetFilesystemSettingHandle(
            widgetId, key, filesystemHandle))
        return filesystemHandle;
    if (RuntimeIsEntityReferenceSetting(widgetId, key)) return {};
    const std::string prefix = WidgetWideToUtf8(widgetId);
    const std::string fullKey = prefix + "." + key;
    int idx = FindWidget(widgetId);
    auto& storage = idx >= 0 && widgets_[idx].preview
        ? widgets_[idx].previewStorage : ReadStorage();
    auto it = storage.find(fullKey);
    if (it != storage.end())
    {
        const auto marker = storage.find(prefix + "." +
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
        if (marker != storage.end() &&
            marker->second == snowdesktop::widget_runtime::TypedStorageMarker)
            return {};
        return it->second;
    }

    if (idx < 0) return {};
    const LuaWidget& widget = widgets_[idx];
    if (key == "followPersonalization" && widget.customStyle &&
        widget.followPersonalizationDefault)
        return "1";
    return FindDeclaredDefaultValue(widget, key);
}

bool WidgetEngine::RuntimeGetTypedSettingDefault(
    const std::wstring& widgetId, const std::string& key,
    snowdesktop::widget_runtime::InteractionValue& value) const
{
    using Value = snowdesktop::widget_runtime::InteractionValue;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const LuaWidgetManifest::Setting* declared = nullptr;
    const auto findSetting = [&](const auto& settings) {
        return std::find_if(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key &&
                    (setting.type == "range" ||
                        setting.type == "multiSelect");
            });
    };
    if (const auto found = findSetting(widget.manifest.settings);
        found != widget.manifest.settings.end())
        declared = &*found;
    else if (const auto scriptFound = findSetting(widget.scriptSettings);
        scriptFound != widget.scriptSettings.end())
        declared = &*scriptFound;
    if (!declared) return false;

    const auto defaultPreset = [&](const auto& presets) ->
        const LuaWidgetManifest::SettingPreset* {
        const auto found = std::find_if(presets.begin(), presets.end(),
            [](const LuaWidgetManifest::SettingPreset& preset) {
                return preset.isDefault || preset.id == "default";
            });
        return found == presets.end() ? nullptr : &*found;
    };
    const LuaWidgetManifest::SettingPreset* preset =
        defaultPreset(widget.manifest.presets);
    if (!preset) preset = defaultPreset(widget.scriptPresets);

    if (declared->type == "range")
    {
        std::string encoded = declared->defaultValue;
        if (encoded.empty() && preset)
        {
            const auto found = preset->values.find(key);
            if (found != preset->values.end()) encoded = found->second;
        }
        double number = declared->minValue;
        (void)snowdesktop::widget_runtime::ParseFiniteSettingNumber(
            encoded, number);
        value = {};
        value.type = Value::Type::Number;
        value.number = snowdesktop::widget_runtime::SnapRangeSettingValue(
            number, declared->minValue, declared->maxValue,
            declared->stepValue);
        return true;
    }

    std::vector<std::string> selected = declared->defaultValues;
    if (selected.empty() && preset)
    {
        const auto found = preset->arrayValues.find(key);
        if (found != preset->arrayValues.end()) selected = found->second;
    }
    value = {};
    value.type = Value::Type::Array;
    for (const auto& item : selected)
    {
        Value entry;
        entry.type = Value::Type::String;
        entry.string = item;
        value.array.push_back(std::move(entry));
    }
    return true;
}

void WidgetEngine::RuntimeSetStorageValue(const std::wstring& widgetId, const std::string& key, const std::string& value)
{
    if (key.empty() || IsRemovedPanelEffectSettingKey(key)) return;
    std::string secretReference;
    if (RuntimeGetSecretReference(widgetId, key, secretReference)) return;
    std::string filesystemHandle;
    if (RuntimeGetFilesystemSettingHandle(
            widgetId, key, filesystemHandle)) return;
    if (RuntimeIsEntityReferenceSetting(widgetId, key)) return;
    const std::string prefix = WidgetWideToUtf8(widgetId);
    if (!StorageWriteWithinQuota(prefix, key, value))
    {
        RuntimeRecordError(widgetId, "Widget storage quota exceeded");
        return;
    }
    const int index = FindWidget(widgetId);
    if (index >= 0 && widgets_[index].preview)
    {
        auto& storage = widgets_[index].previewStorage;
        storage[prefix + "." + key] = value;
        storage.erase(prefix + "." +
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
        return;
    }
    auto& storage = ActiveStorage();
    storage[prefix + "." + key] = value;
    storage.erase(prefix + "." +
        snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
}

bool WidgetEngine::RuntimeGetSecretReference(
    const std::wstring& widgetId, const std::string& key,
    std::string& reference) const
{
    reference.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const auto declaresPassword = [&](const auto& settings) {
        return std::any_of(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key && setting.type == "password";
            });
    };
    if (!declaresPassword(widget.manifest.settings) &&
        !declaresPassword(widget.scriptSettings))
        return false;
    if (!widget.preview && secretStore_)
    {
        reference = secretStore_->Reference(widget.packageId,
            WidgetWideToUtf8(widgetId), key);
    }
    return true;
}

bool WidgetEngine::RuntimeIsEntityReferenceSetting(
    const std::wstring& widgetId, const std::string& key) const
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const auto declaresReference = [&](const auto& settings) {
        return std::any_of(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key &&
                    IsEntityReferenceSettingType(setting.type);
            });
    };
    return declaresReference(widget.manifest.settings) ||
        declaresReference(widget.scriptSettings);
}

std::vector<std::string> WidgetEngine::RuntimeSecretStorageKeys(
    const std::wstring& widgetId) const
{
    std::vector<std::string> result;
    const int index = FindWidget(widgetId);
    if (index < 0 || widgets_[index].preview || !secretStore_)
        return result;
    const LuaWidget& widget = widgets_[index];
    const std::string instanceId = WidgetWideToUtf8(widgetId);
    const auto append = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (setting.type == "password" &&
                !secretStore_->Reference(widget.packageId, instanceId,
                    setting.key).empty() &&
                std::find(result.begin(), result.end(), setting.key) ==
                    result.end())
                result.push_back(setting.key);
        }
    };
    append(widget.manifest.settings);
    append(widget.scriptSettings);
    std::sort(result.begin(), result.end());
    return result;
}

void WidgetEngine::ReloadStorage()
{
    LoadStorageFile();
}

void WidgetEngine::SetInitializationExperimentStoragePath(const std::wstring& path)
{
    snowdesktop::widget_runtime::OpenStorageFile(path);
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
    if (title.empty() || message.empty()) return "invalidArguments";
    const std::string admissionError =
        RuntimeAdmitNotification(widgetId);
    if (!admissionError.empty()) return admissionError;
    try
    {
        if (!notifyCallback_({ snowdesktop::widget_runtime::
                WidgetNotificationHostOperation::Show,
                {}, title, message }))
            return "notificationFailed";
    }
    catch (...)
    {
        return "notificationFailed";
    }
    return {};
}

std::string WidgetEngine::RuntimeAdmitNotification(
    const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return {};
    const int index = FindWidget(widgetId);
    if (index < 0) return "instanceDisposed";
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
    if (name.starts_with("multiplayer.") && snowdesktop::widget_runtime::IsDryLoad())
        return {0, "previewUnavailable"};
    if ((name == "native.request" || name == "native.watch") && snowdesktop::widget_runtime::IsDryLoad())
        return { 0, "previewUnavailable" };
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
                    name == "filesystem.read" ||
                    name == "filesystem.image") && !readable)
                return { 0, "handleAccessDenied" };
            if (name == "filesystem.write" && !writable)
                return { 0, "handleAccessDenied" };
            if (name == "filesystem.list" && entry->kind !=
                    snowdesktop::widget_runtime::
                        WidgetFilesystemHandleKind::Folder)
                return { 0, "notFolder" };
            if (name == "filesystem.image")
            {
                const auto child = arguments.find("name");
                const bool hasChild = child != arguments.end();
                if (hasChild && !snowdesktop::widget_runtime::WidgetFilesystemTaskExecutor::IsDirectChildName(child->second))
                    return { 0, "invalidArguments" };
                const auto expected = hasChild ? snowdesktop::widget_runtime::WidgetFilesystemHandleKind::Folder :
                    snowdesktop::widget_runtime::WidgetFilesystemHandleKind::File;
                if (entry->kind != expected) return { 0, hasChild ? "notFolder" : "notFile" };
            }
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
    if (name == "audio.output.setVolume" && options.arguments.contains("coalesce") &&
        options.arguments.at("coalesce") == "1" && options.permissionGranted &&
        options.trustedGesture)
    {
        if (widget.taskIds.size() >= snowdesktop::widget_runtime::WidgetTaskBroker::MaximumTasks)
            return { 0, "per-instance task limit exceeded" };
        // Retire superseded tasks before quota checks. Keep unique task IDs,
        // cancellation notifications, instance ownership and gesture checks.
        for (const auto id : widget.taskIds)
        {
            const auto previous = taskBroker_->Snapshot(id);
            if (previous && previous->ownerToken == ownerToken && previous->name == name)
            {
                (void)RuntimeCancelTask(widgetId, ownerToken, id);
                (void)taskBroker_->Complete(id, false, "canceled");
            }
        }
    }
    auto result = taskBroker_->Start(
        WidgetWideToUtf8(widgetId), name, options);
    if (result)
    {
        snowdesktop::performance::Value("task.start", name, widgetId, 1, result.id);
        widget.taskIds.insert(result.id);
        if (!widget.preview && taskWakeCallback_) taskWakeCallback_();
    }
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
    if (!taskBroker_->Cancel(taskId)) return false;
    if (audioPlayback_) audioPlayback_->Cancel(taskId);
    CancelWidgetTaskWork(taskId, false);
    networkTaskDispatch_.DiscardCompletion(taskId);
    if (!widget.preview && taskWakeCallback_) taskWakeCallback_();
    return true;
}

bool WidgetEngine::RuntimeSetTimer(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    const std::string& name, int intervalMs, bool repeat,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy)
{
    if (snowdesktop::widget_runtime::IsDryLoad() &&
        !snowdesktop::widget_runtime::IsPreviewExecution())
        return false;
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::Set;
    mutation.name = name;
    mutation.intervalMs = intervalMs;
    mutation.repeat = repeat;
    mutation.hiddenPolicy = hiddenPolicy;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}

void WidgetEngine::SetCalendarDisplayPreferences(snowdesktop::calendar::DisplayPreferences preferences)
{
    snowdesktop::calendar::Normalize(preferences);
    if (calendarDisplay_ == preferences) return;
    calendarDisplay_ = std::move(preferences);
    calendarAnnotationCacheKey_.clear();
    for (auto& widget : widgets_)
        if (widget.state && WidgetDeclaresFeature(widget, "calendar.annotations"))
            InvokeLifecycleEvent(widget, "calendar.preferences", [](lua_State*) {});
    RuntimeInvalidateHost();
}

const std::vector<snowdesktop::calendar::DayAnnotation>& WidgetEngine::RuntimeCalendarAnnotations(const std::string& from, const std::string& to)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    const std::string key = from + ":" + to + ":" + language;
    if (key != calendarAnnotationCacheKey_)
    {
        calendarAnnotationCache_ = snowdesktop::calendar::Annotate(from, to, calendarDisplay_, language);
        calendarAnnotationCacheKey_ = key;
    }
    return calendarAnnotationCache_;
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
    const std::string& toDate, bool includeSubscriptions) const
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
        ? calendarService_->Events(fromDate, toDate, includeSubscriptions)
        : std::vector<
            snowdesktop::calendar::CalendarEvent>{};
}

std::optional<std::vector<snowdesktop::calendar::SubscriptionInfo>> WidgetEngine::RuntimeCalendarSubscriptions() const
{
    return calendarService_ && calendarServiceLoaded_ ? std::optional{calendarService_->Subscriptions()} : std::nullopt;
}

std::optional<snowdesktop::calendar::SubscriptionDetails> WidgetEngine::RuntimeCalendarSubscriptionDetails(
    const std::string& id) const
{
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->SubscriptionDetailsById(id) : std::nullopt;
}

snowdesktop::calendar::SubscriptionResult WidgetEngine::RuntimeCalendarSubscription(
    snowdesktop::calendar::SubscriptionCommand command)
{
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->ApplySubscription(std::move(command))
        : snowdesktop::calendar::SubscriptionResult::Unavailable;
}

std::vector<snowdesktop::calendar::CalendarEvent>
WidgetEngine::RuntimeCalendarSingleEvents() const
{
    return calendarService_ ? calendarService_->SingleEvents() :
        std::vector<snowdesktop::calendar::CalendarEvent>{};
}

std::optional<snowdesktop::calendar::CalendarEvent>
WidgetEngine::RuntimeCalendarEventById(const std::string& id) const
{
    return calendarService_ ? calendarService_->EventById(id) : std::nullopt;
}

std::optional<snowdesktop::calendar::CalendarSeries>
WidgetEngine::RuntimeCalendarSeriesById(const std::string& id) const
{
    return calendarService_ ? calendarService_->SeriesById(id) : std::nullopt;
}

std::vector<snowdesktop::calendar::CalendarSeries>
WidgetEngine::RuntimeCalendarSeries() const
{
    return calendarService_ ? calendarService_->Series() :
        std::vector<snowdesktop::calendar::CalendarSeries>{};
}

snowdesktop::calendar::MutationResult WidgetEngine::RuntimeCalendarSeriesCreate(
    snowdesktop::calendar::CalendarSeries series)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, {}, 0, "previewReadOnly" };
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->CreateSeries(std::move(series)) :
        snowdesktop::calendar::MutationResult{ false, {}, 0, "unavailable" };
}

snowdesktop::calendar::MutationResult WidgetEngine::RuntimeCalendarSeriesUpdate(
    const std::string& id, int expectedRevision,
    snowdesktop::calendar::CalendarSeries series)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0, "previewReadOnly" };
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->UpdateSeries(id, expectedRevision, std::move(series)) :
        snowdesktop::calendar::MutationResult{ false, id, 0, "unavailable" };
}

snowdesktop::calendar::MutationResult WidgetEngine::RuntimeCalendarSeriesRemove(
    const std::string& id, int expectedRevision)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0, "previewReadOnly" };
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->RemoveSeries(id, expectedRevision) :
        snowdesktop::calendar::MutationResult{ false, id, 0, "unavailable" };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarCreate(
    snowdesktop::calendar::CalendarEvent event)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, {}, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_ && calendarServiceLoaded_
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
    return calendarService_ && calendarServiceLoaded_
        ? calendarService_->Update(
            id, expectedRevision, std::move(event))
        : snowdesktop::calendar::MutationResult{
            false, id, 0, "unavailable"
        };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarRemove(
    const std::string& id, int expectedRevision)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_ && calendarServiceLoaded_
        ? calendarService_->Remove(id, expectedRevision)
        : snowdesktop::calendar::MutationResult{
            false, id, 0, "unavailable"
        };
}

bool WidgetEngine::RuntimeCancelTimer(const std::wstring& widgetId,
    std::uint64_t ownerToken, const std::string& name)
{
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::Cancel;
    mutation.name = name;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}

std::string WidgetEngine::RuntimeRequestAnimationFrame(
    const std::wstring& widgetId, const std::string& name)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return "previewUnavailable";
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].state)
        return "hostUnavailable";
    LuaWidget& widget = widgets_[index];
    if (widget.preview)
        return "previewUnavailable";
    if (!widget.hostVisible)
        return "hidden";
    if (QueryWidgetSystemEnvironment().reducedMotion)
        return "reducedMotion";
    if (!widget.animationFrames.Request(name))
        return "quotaExceeded";
    // Retain author requests for the first live frame after uncovering.
    if ((widget.desktopOccluded || widget.desktopOcclusionResumePending) && !widget.panelActive)
        return {};
    if (ScheduleAnimationFrame(widget))
        return {};
    (void)widget.animationFrames.Cancel(name);
    return "hostUnavailable";
}

bool WidgetEngine::RuntimeCancelAnimationFrame(
    const std::wstring& widgetId, const std::string& name)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    LuaWidget& widget = widgets_[index];
    const bool removed = widget.animationFrames.Cancel(name);
    const bool indeterminateProgressActive =
        !HostManagedAnimationsSuppressed() &&
        ((widget.hostVisible && widget.viewIndeterminateProgressActive) ||
            (widget.panelActive &&
                widget.panelIndeterminateProgressActive));
    const bool nativeMarqueeActive =
        !HostManagedAnimationsSuppressed() &&
        widget.desktopVisible &&
        !widget.desktopMarquee.compositionManaged &&
        HasActiveNativeMarquee(widget.desktopMarquee);
    if (!widget.animationFrames.HasPending() &&
        !widget.viewTransitions.HasActive() &&
        !widget.panelViewTransitions.HasActive() &&
        !indeterminateProgressActive &&
        !nativeMarqueeActive &&
        widget.animationTimerId)
    {
        if (widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.animationTimerId);
        widget.animationTimerId = 0;
    }
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
        widget.animationTimerId = 0;

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
        (void)ScheduleAnimationFrame(widget);
    }
}

void WidgetEngine::RescheduleNamedTimer(LuaWidget& widget)
{
    if (widget.namedTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.namedTimerId);
    widget.namedTimerId = 0;

    if (widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_))
        return;

    if (!widgetTimerRequestCallback_)
        return;

    const auto delay = widget.namedTimers.NextDelay(
        std::chrono::steady_clock::now());
    if (!delay)
        return;
    widget.namedTimerId = widgetTimerRequestCallback_(
        widget.widgetId, static_cast<UINT>(delay->count()));
}

bool WidgetEngine::ScheduleAnimationFrame(LuaWidget& widget)
{
    if ((widget.desktopOccluded || widget.desktopOcclusionResumePending) && !widget.panelActive)
        return false;
    if (widget.animationTimerId)
        return true;
    const bool indeterminateProgressActive =
        !HostManagedAnimationsSuppressed() &&
        ((widget.hostVisible && !widget.desktopOccluded &&
            widget.viewIndeterminateProgressActive) ||
            (widget.panelActive &&
                widget.panelIndeterminateProgressActive));
    const bool nativeMarqueeActive =
        !HostManagedAnimationsSuppressed() &&
        widget.desktopVisible && !widget.desktopOccluded &&
        !widget.desktopMarquee.compositionManaged &&
        HasActiveNativeMarquee(widget.desktopMarquee);
    const bool highRateAnimationPending =
        widget.animationFrames.HasPending() ||
        widget.viewTransitions.HasActive() ||
        (widget.panelActive && widget.panelViewTransitions.HasActive()) ||
        indeterminateProgressActive;
    const bool pending = highRateAnimationPending || nativeMarqueeActive;
    if ((widget.preview && (!previewOnly_ || !widgetTimerRequestCallback_)) || (!widget.hostVisible && !widget.panelActive) ||
        !pending ||
        !widgetTimerRequestCallback_)
        return false;
    const int frameLimit = snowdesktop::animation::RuntimeFrameLimit();
    const UINT baseInterval = highRateAnimationPending ? 16 : 33;
    const UINT interval = frameLimit > 0
        ? std::max(baseInterval, static_cast<UINT>((1000 + frameLimit - 1) / frameLimit))
        : baseInterval;
    const auto& requestFrame = widgetAnimationTimerRequestCallback_
        ? widgetAnimationTimerRequestCallback_ : widgetTimerRequestCallback_;
    widget.animationTimerId = requestFrame(widget.widgetId, interval);
    return widget.animationTimerId != 0;
}

void WidgetEngine::StopAnimationFrames(LuaWidget& widget)
{
    if (widget.animationTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widget.animationTimerId);
    widget.animationTimerId = 0;
    (void)widget.animationFrames.SetVisible(false);
    widget.animationFrames.Clear();
    widget.viewTransitions.Clear();
    widget.panelViewTransitions.Clear();
    widget.viewTransitionFramePending = false;
    widget.panelViewTransitionFramePending = false;
    widget.desktopMarquee.framePending = false;
    widget.desktopMarquee.lastAdvance = {};
}

bool WidgetEngine::SyncNativeMarqueeComposition(
    LuaWidget& widget, bool reducedMotion)
{
    auto& surface = widget.desktopMarquee;
    surface.compositionManaged = false;
    if (widget.preview || !widget.desktopVisible ||
        !nativeMarqueeSyncCallback_)
        return false;
    const float durationScale = static_cast<float>(
        snowdesktop::animation::RuntimeDurationScale());
    if (durationScale == 1.0f || reducedMotion)
        surface.compositionManaged = nativeMarqueeSyncCallback_(
            widget.widgetId, surface.marquees, reducedMotion);
    else
    {
        auto presentation = surface.marquees;
        for (auto& marquee : presentation)
            marquee.speed /= durationScale;
        surface.compositionManaged = nativeMarqueeSyncCallback_(
            widget.widgetId, presentation, reducedMotion);
    }
    return surface.compositionManaged;
}

void WidgetEngine::ApplyHostAnimationPreferences()
{
    const bool enabled = !HostManagedAnimationsSuppressed();
    const double durationScale = snowdesktop::animation::RuntimeDurationScale();
    const int frameLimit = snowdesktop::animation::RuntimeFrameLimit();
    const bool motionChanged = !hostAnimationPreferencesKnown_ ||
        enabled != hostAnimationsEnabled_ || durationScale != hostAnimationDurationScale_;
    if (!motionChanged && frameLimit == hostAnimationFrameLimit_)
        return;
    hostAnimationPreferencesKnown_ = true;
    hostAnimationsEnabled_ = enabled;
    hostAnimationDurationScale_ = durationScale;
    hostAnimationFrameLimit_ = frameLimit;
    std::vector<std::wstring> ids;
    for (const auto& widget : widgets_)
        if (widget.valid && !widget.preview) ids.push_back(widget.widgetId);
    for (const auto& id : ids)
    {
        const int index = FindWidget(id);
        if (index < 0) continue;
        auto& widget = widgets_[index];
        if (motionChanged)
        {
            widget.viewTransitions.SetDurationScale(durationScale);
            widget.panelViewTransitions.SetDurationScale(durationScale);
            widget.viewTransitions.Settle();
            widget.panelViewTransitions.Settle();
            widget.viewTransitionFramePending = static_cast<bool>(widget.viewTree);
            widget.panelViewTransitionFramePending = static_cast<bool>(widget.panelViewTree);
            widget.desktopMarquee.lastAdvance = std::chrono::steady_clock::now();
            widget.desktopMarquee.framePending = true;
            if (!enabled)
                for (auto& marquee : widget.desktopMarquee.marquees) marquee.offset = 0.0f;
            (void)SyncNativeMarqueeComposition(widget, !enabled);
        }
        // Re-arm only the shared presentation wake. Named/refresh timers,
        // queued script frames and data-refresh requests are left untouched.
        if (widget.animationTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.animationTimerId);
        widget.animationTimerId = 0;
        (void)ScheduleAnimationFrame(widget);
        const bool desktopVisible = widget.desktopVisible;
        const bool panelActive = widget.panelActive;
        const auto panelSurface = widget.panelSurface;
        if (motionChanged && desktopVisible)
            invalidationBatch_.Invalidate(invalidateCallback_, id, std::nullopt, "desktop");
        if (motionChanged && panelActive)
            invalidationBatch_.Invalidate(invalidateCallback_, id, std::nullopt, panelSurface);
    }
}

void WidgetEngine::ClearNativeMarqueeComposition(LuaWidget& widget)
{
    widget.desktopMarquee.compositionManaged = false;
    if (!widget.preview && nativeMarqueeSyncCallback_)
        (void)nativeMarqueeSyncCallback_(widget.widgetId, {}, true);
}

bool WidgetEngine::RuntimeSetTimerAt(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    const std::string& name, std::int64_t epochMilliseconds,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy)
{
    if (snowdesktop::widget_runtime::IsDryLoad() &&
        !snowdesktop::widget_runtime::IsPreviewExecution())
        return false;
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::At;
    mutation.name = name;
    mutation.epochMilliseconds = epochMilliseconds;
    mutation.hiddenPolicy = hiddenPolicy;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
}

bool WidgetEngine::RuntimeSetTimeline(const std::wstring& widgetId,
    std::uint64_t ownerToken,
    const std::string& name,
    std::vector<snowdesktop::widget_runtime::
        NamedTimerSchedule::TimelineEntry> entries,
    snowdesktop::widget_runtime::ScheduleHiddenPolicy hiddenPolicy,
    bool reloadAtEnd)
{
    if (snowdesktop::widget_runtime::IsDryLoad() &&
        !snowdesktop::widget_runtime::IsPreviewExecution())
        return false;
    using namespace snowdesktop::widget_runtime;
    WidgetTimerMutation mutation;
    mutation.kind = WidgetTimerMutation::Kind::Timeline;
    mutation.name = name;
    mutation.entries = std::move(entries);
    mutation.hiddenPolicy = hiddenPolicy;
    mutation.reloadAtEnd = reloadAtEnd;
    return DispatchWidgetTimerMutation(widgets_, widgetId, ownerToken,
        std::move(mutation), NamedTimerSchedule::Clock::now(),
        NamedTimerSchedule::WallClock::now(),
        [this](auto& owner) { RescheduleNamedTimer(owner); });
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
    const std::string surface(CurrentWidgetSurface(d2dState_));
    control.surface = surface;
    const auto surfaceControlCount = std::count_if(
        widgets_[index].hostControls.begin(),
        widgets_[index].hostControls.end(), [&](const auto& candidate) {
            return HostControlBelongsToSurface(candidate, surface);
        });
    if (surfaceControlCount >= 128) return;
    if (control.type == LuaWidget::HostControl::Type::Scroll ||
        (control.type == LuaWidget::HostControl::Type::Input &&
            control.multiline))
    {
        const int maximum = control.horizontal
            ? std::max(0, control.contentWidth - control.viewportWidth)
            : std::max(0, control.contentHeight - control.viewportHeight);
        int& offset = ScrollOffsetsForSurface(
            widgets_[index], surface).try_emplace(
                control.id, control.initialScrollOffset).first->second;
        offset = std::clamp(offset, 0, maximum);
    }
    if (control.type == LuaWidget::HostControl::Type::Input &&
        focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == control.id &&
        focusedHostInput_.surface == surface)
    {
        focusedHostInput_.controlled = control.controlled;
        focusedHostInput_.readOnly = control.readOnly;
        focusedHostInput_.numeric = control.numeric;
        focusedHostInput_.changeAction = control.changeAction;
        focusedHostInput_.selectionChangeAction =
            control.selectionChangeAction;
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
        const std::wstring modelText=Utf8ToWideLocal(control.controlled?control.controlledText:
            RuntimeGetStorageValue(widgetId,control.storageKey));
        if(modelText!=focusedHostInput_.modelText&&(control.controlled||control.liveUpdate))
        {
            focusedHostInput_.modelText=modelText;
            if(modelText==focusedHostInput_.text)focusedHostInput_.deferredModelText.reset();
            else if(focusedHostInput_.composing)focusedHostInput_.deferredModelText=modelText;
            else
            {
                focusedHostInput_.text=modelText;focusedHostInput_.originalText=modelText;
                focusedHostInput_.cursor=text_input::SnapBoundary(modelText,focusedHostInput_.cursor);
                focusedHostInput_.selectionAnchor=text_input::SnapBoundary(modelText,focusedHostInput_.selectionAnchor);
                focusedHostInput_.history.Clear();focusedHostInput_.caretVisibility.Request();
            }
        }
        const bool selectionChanged =
            focusedHostInput_.controlledSelection != control.selection;
        focusedHostInput_.controlledSelection = control.selection;
        if (selectionChanged && control.selection && focusedHostInput_.composing)
            focusedHostInput_.deferredSelection=true;
        else if (selectionChanged && control.selection)
        {
            const auto anchor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, control.selection->start);
            const auto cursor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, control.selection->finish);
            if (anchor && cursor)
            {
                focusedHostInput_.selectionAnchor = *anchor;
                focusedHostInput_.cursor = *cursor;
                focusedHostInput_.compositionText.clear();
                focusedHostInput_.compositionCursor = 0;
                focusedHostInput_.caretVisibility.Request();
            }
        }
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
    const std::string surface(CurrentWidgetSurface(d2dState_));
    control.surface = surface;
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, surface);
    if (!interactionRegions.FrameOpen() && !widget.panelFrameOpen)
    {
        error = "host text controls may only be submitted during render or panel";
        return false;
    }
    if (std::count_if(widget.hostControls.begin(),
            widget.hostControls.end(), [&](const auto& candidate) {
                return HostControlBelongsToSurface(candidate, surface);
            }) >= 128)
    {
        error = "host control limit exceeded (128)";
        return false;
    }
    if (std::any_of(widget.hostControls.begin(),
            widget.hostControls.end(), [&](const auto& candidate) {
                return HostControlBelongsToSurface(candidate, surface) &&
                    candidate.id == control.id;
            }))
    {
        error = "duplicate host control key: " + control.id;
        return false;
    }
    if (control.type == LuaWidget::HostControl::Type::Input &&
        interactionRegions.FrameOpen())
    {
        snowdesktop::widget_runtime::InteractionRegion region;
        region.key = control.id;
        region.shape.type =
            snowdesktop::widget_runtime::InteractionShapeType::Rect;
        region.shape.x = static_cast<float>(control.rect.left);
        region.shape.y = static_cast<float>(control.rect.top);
        region.shape.width = static_cast<float>(
            control.rect.right - control.rect.left);
        region.shape.height = static_cast<float>(
            control.rect.bottom - control.rect.top);
        if (control.clipRect)
        {
            region.clip = snowdesktop::widget_runtime::InteractionClipRect{
                static_cast<float>(control.clipRect->left),
                static_cast<float>(control.clipRect->top),
                static_cast<float>(control.clipRect->right -
                    control.clipRect->left),
                static_cast<float>(control.clipRect->bottom -
                    control.clipRect->top) };
        }
        region.cursor = "text";
        region.accessibilityRole = "textbox";
        region.accessibilityLabel = control.placeholder.empty()
            ? control.id : control.placeholder;
        region.enabled = control.enabled;
        if (!interactionRegions.Submit(std::move(region), error))
            return false;
    }
    RuntimeRegisterHostControl(widgetId, std::move(control));
    return true;
}

bool WidgetEngine::RuntimeFocusHostInput(const std::wstring& widgetId,
    const std::string& id, const char* source)
{
    int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return false;
    const std::string surface(CurrentWidgetSurface(d2dState_));
    const auto findInput = [&](int widgetIndex) -> const LuaWidget::HostControl* {
        if (widgetIndex < 0) return nullptr;
        const auto& controls = widgets_[widgetIndex].hostControls;
        const auto found = std::find_if(controls.rbegin(), controls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.id == id && HostControlBelongsToSurface(control, surface);
        });
        if (found == controls.rend() || !found->enabled || !found->focusable ||
            (!found->controlled && found->storageKey.empty())) return nullptr;
        return &*found;
    };
    const auto* found = findInput(index);
    if (!found) return false;

    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == id &&
        focusedHostInput_.surface == surface)
    {
        ViewFocusForSurface(widgets_[index], surface) = id;
        ViewFocusCueForSurface(widgets_[index], surface) =
            snowdesktop::widget_runtime::ShouldShowInteractionFocusCue(
                source ? source : "pointer", true);
        if (!IsPanelSurface(surface))
            widgets_[index].logicalSlotFocus.reset();
        if (!found->controlled && found->liveUpdate)
        {
            const std::wstring storedText = Utf8ToWideLocal(
                RuntimeGetStorageValue(widgetId, found->storageKey));
            if (storedText != focusedHostInput_.text)
            {
                focusedHostInput_.text = storedText;
                focusedHostInput_.history.Clear();
                focusedHostInput_.originalText = storedText;
                focusedHostInput_.cursor = storedText.size();
                focusedHostInput_.selectionAnchor =
                    found->selectAll ? 0 : storedText.size();
                focusedHostInput_.compositionText.clear();
                focusedHostInput_.compositionCursor = 0;
                focusedHostInput_.caretVisibility.Request();
            }
        }
        focusedHostInput_.multiline = found->multiline;
        focusedHostInput_.controlled = found->controlled;
        focusedHostInput_.readOnly = found->readOnly;
        focusedHostInput_.numeric = found->numeric;
        focusedHostInput_.changeAction = found->changeAction;
        focusedHostInput_.selectionChangeAction =
            found->selectionChangeAction;
        focusedHostInput_.focusAction = found->focusAction;
        focusedHostInput_.blurAction = found->blurAction;
        focusedHostInput_.submitAction = found->submitAction;
        focusedHostInput_.minimum = found->minimum;
        focusedHostInput_.maximum = found->maximum;
        focusedHostInput_.step = found->step;
        focusedHostInput_.maximumUtf8Bytes =
            found->maximumUtf8Bytes;
        const bool selectionChanged =
            focusedHostInput_.controlledSelection != found->selection;
        focusedHostInput_.controlledSelection = found->selection;
        if (selectionChanged && found->selection)
        {
            const auto anchor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, found->selection->start);
            const auto cursor = snowdesktop::widget_runtime::
                HostTextOffsetFromUtf8ByteOffset(
                    focusedHostInput_.text, found->selection->finish);
            if (anchor && cursor)
            {
                focusedHostInput_.selectionAnchor = *anchor;
                focusedHostInput_.cursor = *cursor;
                focusedHostInput_.caretVisibility.Request();
            }
        }
        focusedHostInput_.caretVisibility.Request();
        if (hostInputFocusCallback_)
            hostInputFocusCallback_();
        return true;
    }

    BlurHostInput(false);
    index = FindWidget(widgetId);
    found = findInput(index);
    if (!found) return false;
    ViewFocusForSurface(widgets_[index], surface) = id;
    ViewFocusCueForSurface(widgets_[index], surface) =
        snowdesktop::widget_runtime::ShouldShowInteractionFocusCue(
            source ? source : "pointer", true);
    if (!IsPanelSurface(surface))
        widgets_[index].logicalSlotFocus.reset();
    focusedHostInput_.active = true;
    focusedHostInput_.widgetId = widgetId;
    focusedHostInput_.id = id;
    focusedHostInput_.surface = surface;
    focusedHostInput_.storageKey = found->storageKey;
    focusedHostInput_.text = Utf8ToWideLocal(found->controlled
        ? found->controlledText
        : RuntimeGetStorageValue(widgetId, found->storageKey));
    focusedHostInput_.originalText = focusedHostInput_.text;
    focusedHostInput_.modelText = focusedHostInput_.text;
    focusedHostInput_.cursor = focusedHostInput_.text.size();
    focusedHostInput_.selectionAnchor = found->selectAll
        ? 0 : focusedHostInput_.cursor;
    focusedHostInput_.controlledSelection = found->selection;
    if (found->selection)
    {
        const auto anchor = snowdesktop::widget_runtime::
            HostTextOffsetFromUtf8ByteOffset(
                focusedHostInput_.text, found->selection->start);
        const auto cursor = snowdesktop::widget_runtime::
            HostTextOffsetFromUtf8ByteOffset(
                focusedHostInput_.text, found->selection->finish);
        if (anchor && cursor)
        {
            focusedHostInput_.selectionAnchor = *anchor;
            focusedHostInput_.cursor = *cursor;
        }
    }
    focusedHostInput_.liveUpdate = found->liveUpdate;
    focusedHostInput_.multiline = found->multiline;
    focusedHostInput_.controlled = found->controlled;
    focusedHostInput_.readOnly = found->readOnly;
    focusedHostInput_.numeric = found->numeric;
    focusedHostInput_.changeAction = found->changeAction;
    focusedHostInput_.selectionChangeAction =
        found->selectionChangeAction;
    focusedHostInput_.focusAction = found->focusAction;
    focusedHostInput_.blurAction = found->blurAction;
    focusedHostInput_.submitAction = found->submitAction;
    focusedHostInput_.minimum = found->minimum;
    focusedHostInput_.maximum = found->maximum;
    focusedHostInput_.step = found->step;
    focusedHostInput_.maximumUtf8Bytes =
        found->maximumUtf8Bytes;
    focusedHostInput_.caretVisibility.Request();
    DispatchHostInputAction(widgetId, id,
        focusedHostInput_.focusAction, "focus",
        focusedHostInput_.text, false,
        source && *source ? source : "pointer");
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
        focusedHostInput_.id != id ||
        focusedHostInput_.surface != CurrentWidgetSurface(d2dState_))
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
        x - control.rect.left) - control.padding.left;
    float localY = static_cast<float>(
        y - control.rect.top) - control.padding.top;
    if (control.multiline)
    {
        const float scrollbarReserve = 8.0f;
        const float innerWidth = std::max(1.0f,
            width - control.padding.left - control.padding.right -
                scrollbarReserve);
        layout = CreateHostMultilineTextLayout(d2dState_,
            text, control.fontSize, innerWidth);
        localY += RuntimeGetScrollOffset(
            widgetId, control.id, control.surface);
    }
    else
    {
        const float innerWidth = std::max(1.0f,
            width - control.padding.left - control.padding.right);
        layout = CreateHostSingleLineTextLayout(d2dState_,
            text, control.fontSize, innerWidth,
            std::max(1.0f, height - control.padding.top -
                control.padding.bottom));
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
    const std::wstring& widgetId, int x, int y,
    std::string_view surface) const
{
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.surface != surface)
        return false;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const POINT point{ x, y };
    for (const auto& control : widgets_[index].hostControls)
        if (control.selectPopup && control.enabled &&
            HostControlBelongsToSurface(control, surface) &&
            HostControlContainsPoint(control, point))
            return false;
    for (auto it = widgets_[index].hostControls.rbegin();
        it != widgets_[index].hostControls.rend(); ++it)
    {
        if (it->type == LuaWidget::HostControl::Type::Input &&
            it->id == focusedHostInput_.id &&
            HostControlBelongsToSurface(*it, surface))
            return HostControlContainsPoint(*it, point);
    }
    return false;
}

namespace
{
struct HostLogicalSlotKeyboardItem
{
    std::string slotId;
    std::string itemId;
    snowdesktop::widget_runtime::LogicalSlotKind kind =
        snowdesktop::widget_runtime::LogicalSlotKind::Binding;
    std::size_t modelIndex = 0;
    std::size_t surfaceIndex = 0;
    std::size_t itemCount = 0;
    bool allowClear = true;
    RECT bounds{};
};

std::vector<HostLogicalSlotKeyboardItem> CollectHostLogicalSlotKeyboardItems(
    WidgetEngine& engine, const LuaWidget& widget)
{
    std::vector<HostLogicalSlotKeyboardItem> result;
    for (const auto& snapshot : widget.logicalSlots.Snapshots())
    {
        const auto surface = engine.RuntimeLogicalSlotSurface(
            widget.widgetId, snapshot.id);
        if (!surface) continue;
        for (std::size_t surfaceIndex = 0;
            surfaceIndex < surface->items.size(); ++surfaceIndex)
        {
            const auto& region = surface->items[surfaceIndex];
            const auto modelItem = std::find_if(snapshot.items.begin(),
                snapshot.items.end(), [&](const auto& candidate) {
                    return candidate.id == region.itemId;
                });
            if (modelItem == snapshot.items.end()) continue;
            result.push_back({ snapshot.id, region.itemId, snapshot.kind,
                static_cast<std::size_t>(std::distance(
                    snapshot.items.begin(), modelItem)),
                surfaceIndex, snapshot.items.size(), surface->allowClear,
                region.bounds });
        }
    }
    std::stable_sort(result.begin(), result.end(),
        [](const auto& left, const auto& right) {
            return std::tie(left.bounds.top, left.bounds.left,
                       left.bounds.bottom, left.bounds.right,
                       left.slotId, left.itemId) <
                std::tie(right.bounds.top, right.bounds.left,
                       right.bounds.bottom, right.bounds.right,
                       right.slotId, right.itemId);
        });
    return result;
}
}

void WidgetEngine::RevealHostViewKeyboardFocus(
    LuaWidget& widget, std::string_view surface)
{
    const auto& regions = InteractionRegionsForSurface(widget, surface);
    const auto* region = regions.Find(ViewFocusForSurface(widget, surface));
    const auto& tree = IsPanelSurface(surface) ? widget.panelViewTree : widget.viewTree;
    if (!region || !tree) return;
    const auto requests = snowdesktop::widget_runtime::ComputeViewKeyboardScrollOffsets(*tree, *region);
    auto& offsets = ScrollOffsetsForSurface(widget, surface);
    for (const auto& [id, value] : requests)
        offsets[id] = static_cast<int>(std::lround(value));
    if (!requests.empty()) RuntimeInvalidateHost(widget.widgetId);
}

void WidgetEngine::ReconcileHostViewKeyboardFocus(
    LuaWidget& widget, std::string_view surface)
{
    auto& regions = InteractionRegionsForSurface(widget, surface);
    auto& focus = ViewFocusForSurface(widget, surface);
    if (ViewFocusCueForSurface(widget, surface))
    {
        const auto target = regions.ReconcileKeyboardFocus(focus);
        if (target != focus)
        {
            focus.clear();
            if (!IsPanelSurface(surface)) widget.logicalSlotFocus.reset();
            if (!target.empty()) RuntimeFocusViewTarget(widget.widgetId, target, "keyboard");
        }
    }
    else if (!focus.empty() && !regions.IsKeyboardFocusable(focus))
        focus.clear();
}

bool WidgetEngine::RuntimeFocusViewTarget(const std::wstring& widgetId,
    const std::string& id, const char* source)
{
    int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return false;
    auto& widget = widgets_[index];
    const std::string surface(CurrentWidgetSurface(d2dState_));
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, surface);
    if (!widget.valid || widget.preview ||
        (IsPanelSurface(surface)
            ? !widget.panelActive
            : !RuntimeIsWidgetSelected(widgetId)))
        return false;
    if (RuntimeFocusHostInput(widgetId, id, source))
    {
        RevealHostViewKeyboardFocus(widget, surface);
        return true;
    }
    if (!interactionRegions.IsKeyboardFocusable(id)) return false;

    if (focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId)
        BlurHostInput(false);
    index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& current = widgets_[index];
    auto& currentRegions =
        InteractionRegionsForSurface(current, surface);
    if (!current.valid || current.preview ||
        (IsPanelSurface(surface)
            ? !current.panelActive
            : !RuntimeIsWidgetSelected(widgetId)) ||
        !currentRegions.IsKeyboardFocusable(id))
        return false;
    ViewFocusForSurface(current, surface) = id;
    ViewFocusCueForSurface(current, surface) =
        snowdesktop::widget_runtime::ShouldShowInteractionFocusCue(
            source ? source : "programmatic");
    if (!IsPanelSurface(surface))
    {
        current.logicalSlotFocus.reset();
        const auto slotItems =
            CollectHostLogicalSlotKeyboardItems(*this, current);
        const auto slot = std::find_if(slotItems.begin(), slotItems.end(),
            [&id](const auto& item) { return item.itemId == id; });
        if (slot != slotItems.end())
        {
            current.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
                slot->slotId, slot->itemId };
        }
    }
    RevealHostViewKeyboardFocus(current, surface);
    if (hostInputFocusCallback_) hostInputFocusCallback_();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeConsumeHostInputCaretVisibilityRequest(
    const std::wstring& widgetId, const std::string& id)
{
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id ||
        focusedHostInput_.surface != CurrentWidgetSurface(d2dState_))
        return false;
    return focusedHostInput_.caretVisibility.Consume();
}

void WidgetEngine::BeginHostLogicalSlotPointer(
    LuaWidget& widget, int x, int y)
{
    widget.logicalSlotPointerDrag.reset();
    widget.viewFocusCueVisible = false;
    const std::string previousViewFocus = widget.viewKeyboardFocusKey;
    const auto previousSlotFocus = widget.logicalSlotFocus;
    if (widget.preview || !widget.valid)
    {
        widget.viewKeyboardFocusKey.clear();
        widget.logicalSlotFocus.reset();
        return;
    }
    const std::string pointerTarget =
        widget.interactionRegions.TargetAt(
            static_cast<float>(x), static_cast<float>(y));
    widget.viewKeyboardFocusKey =
        widget.interactionRegions.IsKeyboardFocusable(pointerTarget)
        ? pointerTarget : std::string{};
    if (!widget.viewTree)
    {
        widget.logicalSlotFocus.reset();
        if (previousViewFocus != widget.viewKeyboardFocusKey ||
            previousSlotFocus.has_value())
            RuntimeInvalidateHost(widget.widgetId);
        return;
    }
    const POINT point{ widget.lastBounds.left + x,
        widget.lastBounds.top + y };
    const auto items = CollectHostLogicalSlotKeyboardItems(*this, widget);
    const auto hit = std::find_if(items.rbegin(), items.rend(),
        [&](const auto& candidate) {
            return PtInRect(&candidate.bounds, point) != FALSE;
        });
    if (hit == items.rend())
    {
        widget.logicalSlotFocus.reset();
        if (previousViewFocus != widget.viewKeyboardFocusKey ||
            previousSlotFocus.has_value())
            RuntimeInvalidateHost(widget.widgetId);
        return;
    }
    widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
        hit->slotId, hit->itemId };
    if (widget.interactionRegions.IsKeyboardFocusable(hit->itemId))
        widget.viewKeyboardFocusKey = hit->itemId;
    if (previousViewFocus != widget.viewKeyboardFocusKey ||
        !previousSlotFocus || previousSlotFocus->slotId != hit->slotId ||
        previousSlotFocus->itemId != hit->itemId)
        RuntimeInvalidateHost(widget.widgetId);
    if (hit->kind !=
            snowdesktop::widget_runtime::LogicalSlotKind::Collection)
        return;
    widget.logicalSlotPointerDrag = LuaWidget::LogicalSlotPointerDrag{
        hit->slotId, hit->slotId, hit->itemId, hit->modelIndex,
        hit->modelIndex, POINT{ x, y }, hit->bounds, RECT{}, false };
}

bool WidgetEngine::UpdateHostLogicalSlotPointer(
    LuaWidget& widget, int x, int y)
{
    if (!widget.logicalSlotPointerDrag) return false;
    auto& drag = *widget.logicalSlotPointerDrag;
    if (!drag.moved)
    {
        const int thresholdX = std::max(4, GetSystemMetrics(SM_CXDRAG));
        const int thresholdY = std::max(4, GetSystemMetrics(SM_CYDRAG));
        if (std::abs(x - drag.start.x) <= thresholdX &&
            std::abs(y - drag.start.y) <= thresholdY)
            return false;
    }
    const POINT point{ widget.lastBounds.left + x,
        widget.lastBounds.top + y };

    const auto* sourceSnapshot =
        widget.logicalSlots.Find(drag.sourceSlotId);
    if (!sourceSnapshot)
    {
        widget.logicalSlotPointerDrag.reset();
        return false;
    }
    const auto sourceItem = std::find_if(sourceSnapshot->items.begin(),
        sourceSnapshot->items.end(), [&drag](const auto& candidate) {
            return candidate.id == drag.itemId;
        });
    if (sourceItem == sourceSnapshot->items.end())
    {
        widget.logicalSlotPointerDrag.reset();
        return false;
    }

    std::string targetSlotId;
    std::uint64_t targetRevision = 0;
    std::optional<snowdesktop::widget_runtime::
        LogicalSlotPointerTarget> target;
    RECT sourceBounds = drag.sourceBounds;
    const auto sourceIndex = static_cast<std::size_t>(sourceItem - sourceSnapshot->items.begin());
    for (const auto& snapshot : widget.logicalSlots.Snapshots())
    {
        if (snapshot.kind !=
            snowdesktop::widget_runtime::LogicalSlotKind::Collection)
            continue;
        const auto surface = RuntimeLogicalSlotSurface(
            widget.widgetId, snapshot.id);
        if (!surface || !PtInRect(&surface->bounds, point)) continue;

        std::vector<RECT> itemBounds;
        std::vector<std::string_view> visibleIds;
        itemBounds.reserve(surface->items.size());
        visibleIds.reserve(surface->items.size());
        for (std::size_t index = 0; index < surface->items.size(); ++index)
        {
            itemBounds.push_back(surface->items[index].bounds);
            visibleIds.push_back(surface->items[index].itemId);
            if (snapshot.id == drag.sourceSlotId &&
                surface->items[index].itemId == drag.itemId)
            {
                sourceBounds = surface->items[index].bounds;
            }
        }

        if (snapshot.id != drag.sourceSlotId)
        {
            const auto declaration =
                widget.logicalSlots.Declarations().find(snapshot.id);
            const bool accepts = declaration !=
                    widget.logicalSlots.Declarations().end() &&
                std::find(declaration->second.accepts.begin(),
                    declaration->second.accepts.end(), sourceItem->kind) !=
                    declaration->second.accepts.end();
            const bool duplicate = std::any_of(snapshot.items.begin(),
                snapshot.items.end(), [&sourceItem](const auto& candidate) {
                    return candidate.kind == sourceItem->kind &&
                        candidate.target == sourceItem->target;
                });
            if (!accepts || duplicate ||
                snapshot.items.size() >= snapshot.capacity)
                continue;
        }
        target = snowdesktop::widget_runtime::
            ResolveLogicalSlotModelPointerTarget(itemBounds, visibleIds,
                snapshot, snapshot.id == drag.sourceSlotId
                    ? std::string_view(drag.itemId) : std::string_view{},
                point, surface->bounds);
        if (target)
        {
            targetSlotId = snapshot.id;
            targetRevision = snapshot.revision;
            break;
        }
    }

    if (!drag.moved)
        widget.interactionRegions.CancelPointerPress();
    const bool changed = !drag.moved ||
        drag.targetSlotId != targetSlotId ||
        (target && (drag.targetIndex != target->targetIndex ||
            !EqualRect(&drag.indicatorBounds, &target->indicator))) ||
        (!target && !IsRectEmpty(&drag.indicatorBounds)) ||
        !EqualRect(&drag.sourceBounds, &sourceBounds);
    drag.moved = true;
    drag.targetRevision = targetRevision;
    drag.sourceIndex = sourceIndex;
    drag.targetSlotId = std::move(targetSlotId);
    drag.targetIndex = target ? target->targetIndex : 0;
    drag.sourceBounds = sourceBounds;
    drag.indicatorBounds = target ? target->indicator : RECT{};
    if (changed) RuntimeInvalidateHost(widget.widgetId);
    return true;
}

bool WidgetEngine::EndHostLogicalSlotPointer(
    const std::wstring& widgetId)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].logicalSlotPointerDrag)
        return false;
    const LuaWidget::LogicalSlotPointerDrag drag =
        *widgets_[index].logicalSlotPointerDrag;
    widgets_[index].logicalSlotPointerDrag.reset();
    RuntimeInvalidateHost(widgetId);
    if (!drag.moved) return false;
    if (drag.targetSlotId.empty()) return true;
    const auto* targetSnapshot = widgets_[index].logicalSlots.Find(drag.targetSlotId);
    if (!targetSnapshot || targetSnapshot->revision != drag.targetRevision)
    {
        RuntimeRecordError(widgetId, "logical slot pointer target changed during drag");
        return true;
    }

    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    const bool committed = drag.targetSlotId == drag.sourceSlotId
        ? RuntimeMoveHostLogicalSlotItem(widgetId, drag.sourceSlotId,
            drag.itemId, drag.targetIndex, change, error,
            "host.pointer")
        : RuntimeTransferHostLogicalSlotItem(widgetId,
            drag.sourceSlotId, drag.itemId, drag.targetSlotId,
            drag.targetIndex, change, error, "host.pointer");
    if (!committed)
    {
        RuntimeRecordError(widgetId,
            "logical slot pointer move: " + error);
    }
    return true;
}

namespace
{
float LogicalSlotDropOpacity(
    const snowdesktop::widget_runtime::ViewStyle& style) noexcept
{
    return std::clamp(style.opacity.value_or(1.0f), 0.0f, 1.0f);
}

void DrawLogicalSlotDropSurface(D2DState* state,
    const LogicalSlotHostSurface& surface)
{
    if (!state || !state->ctx ||
        (!surface.dropStyle.background &&
         !surface.dropStyle.borderColor))
        return;
    const D2D1_RECT_F bounds = D2D1::RectF(
        static_cast<float>(surface.bounds.left),
        static_cast<float>(surface.bounds.top),
        static_cast<float>(surface.bounds.right),
        static_cast<float>(surface.bounds.bottom));
    const float radius = std::max(
        0.0f, surface.dropStyle.cornerRadius.value_or(0.0f));
    const D2D1_ROUNDED_RECT rounded =
        D2D1::RoundedRect(bounds, radius, radius);
    const float opacity = LogicalSlotDropOpacity(surface.dropStyle);
    if (surface.dropStyle.background)
        if (ID2D1SolidColorBrush* fill = GetCachedBrush(state,
                static_cast<int>(*surface.dropStyle.background), opacity))
            state->ctx->FillRoundedRectangle(rounded, fill);
    const float borderWidth = std::max(
        0.0f, surface.dropStyle.borderWidth.value_or(1.0f));
    if (surface.dropStyle.borderColor && borderWidth > 0.0f)
        if (ID2D1SolidColorBrush* border = GetCachedBrush(state,
                static_cast<int>(*surface.dropStyle.borderColor), opacity))
            state->ctx->DrawRoundedRectangle(
                rounded, border, borderWidth);
}
}

void WidgetEngine::DrawHostViewInteractionOverlays(
    const LuaWidget& widget,
    const snowdesktop::widget_runtime::WidgetInteractionRegions& regions,
    std::string_view focusedKey, bool drawLogicalSlotDrag)
{
    if (!d2dState_ || !d2dState_->ctx) return;
    const std::string_view surface = drawLogicalSlotDrag
        ? std::string_view("desktop")
        : std::string_view(widget.panelSurface);
    if (hostInputDropCaret_ && hostInputDropSurface_ == surface &&
        hostInputDropWidgetId_ == widget.widgetId)
    {
        const auto& caret = *hostInputDropCaret_;
        const auto& frame = d2dState_->widgetRect;
        const auto& clip = caret.clip;
        auto* context = d2dState_->ctx;
        context->PushAxisAlignedClip(D2D1::RectF(
            frame.left + static_cast<float>(clip.left),
            frame.top + static_cast<float>(clip.top),
            frame.left + static_cast<float>(clip.right),
            frame.top + static_cast<float>(clip.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto from = D2D1::Point2F(frame.left + caret.left, frame.top + caret.top);
        const auto to = D2D1::Point2F(frame.left + caret.right, frame.top + caret.bottom);
        if (auto* halo = GetCachedBrush(d2dState_, 0x0B1220, 0.72f))
            context->DrawLine(from, to, halo, 4.0f);
        if (auto* accent = GetCachedBrush(d2dState_, 0x72C7FF))
            context->DrawLine(from, to, accent, 2.0f);
        context->PopAxisAlignedClip();
    }
    const bool declarativeSurface = drawLogicalSlotDrag
        ? widget.viewTree.has_value() : widget.panelViewTree.has_value();
    if (!declarativeSurface &&
        ViewFocusCueForSurface(widget, surface) && !focusedKey.empty() &&
        (drawLogicalSlotDrag
            ? RuntimeIsWidgetSelected(widget.widgetId)
            : widget.panelActive))
    {
        const auto* region = regions.Find(focusedKey);
        if (region && region->enabled)
        {
            bool clipped = false;
            if (region->clip)
            {
                d2dState_->ctx->PushAxisAlignedClip(D2D1::RectF(
                    d2dState_->widgetRect.left + region->clip->x,
                    d2dState_->widgetRect.top + region->clip->y,
                    d2dState_->widgetRect.left + region->clip->x +
                        region->clip->width,
                    d2dState_->widgetRect.top + region->clip->y +
                        region->clip->height),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                clipped = true;
            }
            ID2D1SolidColorBrush* halo = GetCachedBrush(
                d2dState_, 0x0B1220, 0.72f);
            ID2D1SolidColorBrush* focus = GetCachedBrush(
                d2dState_, 0x72C7FF, 1.0f);
            if (region->shape.type == snowdesktop::widget_runtime::
                    InteractionShapeType::Circle)
            {
                const D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                    D2D1::Point2F(
                        d2dState_->widgetRect.left + region->shape.x,
                        d2dState_->widgetRect.top + region->shape.y),
                    std::max(0.0f, region->shape.radius - 1.0f),
                    std::max(0.0f, region->shape.radius - 1.0f));
                if (halo) d2dState_->ctx->DrawEllipse(ellipse, halo, 3.5f);
                if (focus) d2dState_->ctx->DrawEllipse(ellipse, focus, 2.0f);
            }
            else
            {
                const D2D1_RECT_F bounds = D2D1::RectF(
                    d2dState_->widgetRect.left + region->shape.x + 1.0f,
                    d2dState_->widgetRect.top + region->shape.y + 1.0f,
                    d2dState_->widgetRect.left + region->shape.x +
                        region->shape.width - 1.0f,
                    d2dState_->widgetRect.top + region->shape.y +
                        region->shape.height - 1.0f);
                const float radius = region->shape.type ==
                        snowdesktop::widget_runtime::
                            InteractionShapeType::RoundedRect
                    ? std::max(0.0f, region->shape.radius - 1.0f) : 3.0f;
                const D2D1_ROUNDED_RECT rounded =
                    D2D1::RoundedRect(bounds, radius, radius);
                if (halo)
                    d2dState_->ctx->DrawRoundedRectangle(
                        rounded, halo, 3.5f);
                if (focus)
                    d2dState_->ctx->DrawRoundedRectangle(
                        rounded, focus, 2.0f);
            }
            if (clipped) d2dState_->ctx->PopAxisAlignedClip();
        }
    }
    if (!drawLogicalSlotDrag || !widget.logicalSlotPointerDrag ||
        !widget.logicalSlotPointerDrag->moved)
        return;
    const auto& drag = *widget.logicalSlotPointerDrag;
    const auto dropSurface = drag.targetSlotId.empty()
        ? std::optional<LogicalSlotHostSurface>{}
        : RuntimeLogicalSlotSurface(widget.widgetId, drag.targetSlotId);
    if (dropSurface)
        DrawLogicalSlotDropSurface(d2dState_, *dropSurface);
    const D2D1_RECT_F source = D2D1::RectF(
        static_cast<float>(drag.sourceBounds.left),
        static_cast<float>(drag.sourceBounds.top),
        static_cast<float>(drag.sourceBounds.right),
        static_cast<float>(drag.sourceBounds.bottom));
    if (ID2D1SolidColorBrush* fill = GetCachedBrush(
            d2dState_, 0x4C9AFF, 0.10f))
        d2dState_->ctx->FillRoundedRectangle(
            D2D1::RoundedRect(source, 6.0f, 6.0f), fill);
    if (ID2D1SolidColorBrush* outline = GetCachedBrush(
            d2dState_, 0x72C7FF, 0.76f))
        d2dState_->ctx->DrawRoundedRectangle(
            D2D1::RoundedRect(source, 6.0f, 6.0f), outline, 1.5f);
    if (drag.targetSlotId.empty() || IsRectEmpty(&drag.indicatorBounds))
        return;
    const D2D1_RECT_F indicator = D2D1::RectF(
        static_cast<float>(drag.indicatorBounds.left),
        static_cast<float>(drag.indicatorBounds.top),
        static_cast<float>(drag.indicatorBounds.right),
        static_cast<float>(drag.indicatorBounds.bottom));
    const int indicatorColor = dropSurface &&
            dropSurface->dropStyle.foreground
        ? static_cast<int>(*dropSurface->dropStyle.foreground)
        : 0x4C9AFF;
    const float indicatorOpacity = dropSurface &&
            dropSurface->dropStyle.foreground
        ? LogicalSlotDropOpacity(dropSurface->dropStyle) : 1.0f;
    if (ID2D1SolidColorBrush* brush = GetCachedBrush(
            d2dState_, indicatorColor, indicatorOpacity))
        d2dState_->ctx->FillRoundedRectangle(
            D2D1::RoundedRect(indicator, 2.0f, 2.0f), brush);
}

bool WidgetEngine::DispatchHostViewKeyEvent(WPARAM key, bool pressed,
    bool repeated, bool ctrl, bool shift, bool alt)
{
    std::wstring widgetId;
    std::string nodeKey;
    std::string surface;
    if (!pressed)
    {
        const auto paired = pressedViewKeyTargets_.find(key);
        if (paired != pressedViewKeyTargets_.end())
        {
            widgetId = paired->second.widgetId;
            nodeKey = paired->second.nodeKey;
            surface = paired->second.surface;
            pressedViewKeyTargets_.erase(paired);
        }
    }
    if (widgetId.empty())
    {
        const auto focused = std::find_if(widgets_.begin(), widgets_.end(),
            [this](const LuaWidget& candidate) {
                return candidate.valid && !candidate.preview &&
                    ((candidate.panelActive &&
                        !candidate.panelViewKeyboardFocusKey.empty()) ||
                     (candidate.hostVisible &&
                        !candidate.viewKeyboardFocusKey.empty() &&
                        RuntimeIsWidgetSelected(candidate.widgetId)));
            });
        if (focused == widgets_.end()) return false;
        widgetId = focused->widgetId;
        surface = focused->panelActive &&
                !focused->panelViewKeyboardFocusKey.empty()
            ? NormalizeWidgetSurface(focused->panelSurface)
            : "desktop";
        nodeKey = ViewFocusForSurface(*focused, surface);
    }

    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    auto& interactionRegions =
        InteractionRegionsForSurface(widget, surface);
    if (!widget.valid || widget.preview ||
        (IsPanelSurface(surface)
            ? !widget.panelActive
            : (!widget.hostVisible || !RuntimeIsWidgetSelected(widgetId))) ||
        !interactionRegions.IsKeyboardFocusable(nodeKey))
        return false;

    const char* eventName = pressed ? "keyDown" : "keyUp";
    const auto* actionPointer = interactionRegions.FindAction(
        nodeKey, eventName);
    if (pressed &&
        (interactionRegions.FindAction(nodeKey, "keyDown") ||
            interactionRegions.FindAction(nodeKey, "keyUp")))
    {
        pressedViewKeyTargets_.insert_or_assign(
            key, PressedViewKeyTarget{ widgetId, nodeKey, surface });
    }
    if (!actionPointer) return false;

    const auto action = *actionPointer;
    const std::string keyName = ViewVirtualKeyName(key);
    snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
        trustedGestureState_, true);
    WidgetSurfaceScope surfaceScope(d2dState_, surface.c_str());
    return InvokeLifecycleEvent(widget, "action",
        [&action, &nodeKey, &keyName, key, eventName, repeated,
            ctrl, shift, alt, &surface](lua_State* eventState) {
            lua_pushlstring(eventState, action.id.data(), action.id.size());
            lua_setfield(eventState, -2, "id");
            PushInteractionValue(eventState, action.value);
            lua_setfield(eventState, -2, "value");
            lua_pushlstring(eventState, nodeKey.data(), nodeKey.size());
            lua_setfield(eventState, -2, "targetKey");
            lua_pushstring(eventState, eventName);
            lua_setfield(eventState, -2, "action");
            lua_pushlstring(eventState, keyName.data(), keyName.size());
            lua_setfield(eventState, -2, "key");
            lua_pushinteger(eventState, static_cast<lua_Integer>(key));
            lua_setfield(eventState, -2, "virtualKey");
            lua_pushboolean(eventState, repeated ? 1 : 0);
            lua_setfield(eventState, -2, "repeat");
            lua_pushboolean(eventState, ctrl ? 1 : 0);
            lua_setfield(eventState, -2, "ctrlKey");
            lua_pushboolean(eventState, shift ? 1 : 0);
            lua_setfield(eventState, -2, "shiftKey");
            lua_pushboolean(eventState, alt ? 1 : 0);
            lua_setfield(eventState, -2, "altKey");
            lua_pushboolean(eventState, 1);
            lua_setfield(eventState, -2, "trustedGesture");
            lua_pushliteral(eventState, "keyboard");
            lua_setfield(eventState, -2, "source");
            lua_pushlstring(eventState, surface.data(), surface.size());
            lua_setfield(eventState, -2, "surface");
        });
}

void WidgetEngine::ClearHostViewKeyState() noexcept
{
    pressedViewKeyTargets_.clear();
}

bool WidgetEngine::HasHostViewKeyboardFocus() const
{
    return std::any_of(widgets_.begin(), widgets_.end(), [this](const auto& widget) {
        return widget.valid && !widget.preview &&
            (widget.panelActive || (!widget.viewKeyboardFocusKey.empty() &&
                RuntimeIsWidgetSelected(widget.widgetId)));
    });
}

bool WidgetEngine::HandleHostViewKey(const std::wstring& widgetId,
    WPARAM key, bool ctrl, bool shift, bool alt,
    std::string_view surface, bool repeated)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || ctrl) return false;
    auto& widget = widgets_[index];
    if (widget.preview || !widget.valid) return false;
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, normalizedSurface.c_str());
    const bool panelSurface = IsPanelSurface(normalizedSurface);
    if ((panelSurface && !widget.panelActive) ||
        (!panelSurface && !RuntimeIsWidgetSelected(widgetId)))
        return false;
    auto& surfaceRegions = InteractionRegionsForSurface(
        widget, normalizedSurface);
    auto& surfaceFocus = ViewFocusForSurface(widget, normalizedSurface);
    if (!alt)
    {
        const auto* select = surfaceRegions.ExpandedSelectForFocus(surfaceFocus);
        if (select && key == VK_TAB)
        {
            const auto parent = select->key;
            auto keys = surfaceRegions.KeyboardFocusableKeys();
            std::erase_if(keys, [&](const auto& id) {
                const auto* region = surfaceRegions.Find(id);
                return region && !region->keyboardNavigationGroup.empty();
            });
            const auto found = std::find(keys.begin(), keys.end(), parent);
            const auto current = found == keys.end() ? std::optional<std::size_t>{} :
                std::optional<std::size_t>(static_cast<std::size_t>(found - keys.begin()));
            const auto next = snowdesktop::widget_runtime::CycleLogicalSlotFocus(keys.size(), current, shift);
            const std::string target = next ? keys[*next] : parent;
            surfaceFocus = target;
            ViewFocusCueForSurface(widget, normalizedSurface) = true;
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(trustedGestureState_, true);
            DispatchInteractionAction(widget, parent, "click", 0, 0, 1, 0, 1,
                false, "keyboard", 0, std::nullopt, std::nullopt, normalizedSurface);
            RuntimeFocusViewTarget(widgetId, target, "keyboard");
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (select && key == VK_ESCAPE)
        {
            const auto target = select->key;
            surfaceFocus = target;
            ViewFocusCueForSurface(widget, normalizedSurface) = true;
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(trustedGestureState_, true);
            DispatchInteractionAction(widget, target, "click", 0, 0, 1, 0, 1,
                false, "keyboard", 0, std::nullopt, std::nullopt, normalizedSurface);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN ||
            key == VK_HOME || key == VK_END)
        {
            const auto target = surfaceRegions.SelectKeyboardTarget(surfaceFocus,
                key == VK_LEFT || key == VK_UP || key == VK_HOME ? -1 : 1,
                key == VK_HOME || key == VK_END);
            if (target) return RuntimeFocusViewTarget(widgetId, *target, "keyboard");
        }
    }
    if (alt && !repeated && key <= 0x7f)
    {
        const auto* foundAccessTarget = surfaceRegions.FindAccessKey(
            static_cast<char>(key));
        if (foundAccessTarget)
        {
            const auto accessTarget = *foundAccessTarget;
            const std::string targetKey = accessTarget.key;
            const auto input = std::find_if(widget.hostControls.rbegin(),
                widget.hostControls.rend(), [&](const auto& control) {
                    return control.type ==
                            LuaWidget::HostControl::Type::Input &&
                        control.enabled && control.id == targetKey &&
                        HostControlBelongsToSurface(
                            control, normalizedSurface);
                });
            if (input != widget.hostControls.rend())
            {
                if (!RuntimeFocusHostInput(
                        widgetId, targetKey, "accessKey"))
                    return false;
            }
            else
            {
                if (focusedHostInput_.active &&
                    focusedHostInput_.widgetId == widgetId)
                    BlurHostInput(false);
                surfaceFocus = targetKey;
                ViewFocusCueForSurface(widget, normalizedSurface) = true;
            }
            if (!panelSurface)
            {
                widget.logicalSlotFocus.reset();
                const auto slotItems =
                    CollectHostLogicalSlotKeyboardItems(*this, widget);
                const auto slot = std::find_if(
                    slotItems.begin(), slotItems.end(),
                    [&targetKey](const auto& item) {
                        return item.itemId == targetKey;
                    });
                if (slot != slotItems.end())
                    widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
                        slot->slotId, slot->itemId };
            }
            if (accessTarget.controlKind !=
                    snowdesktop::widget_runtime::
                        InteractionControlKind::Slider &&
                surfaceRegions.ResolveAction(targetKey, "click"))
            {
                const auto& shape = accessTarget.shape;
                const int x = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.x : shape.x + shape.width * 0.5f));
                const int y = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.y : shape.y + shape.height * 0.5f));
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, targetKey, "click",
                    x, y, 0, 0, 1, false, "accessKey", 0,
                    std::nullopt, std::nullopt, normalizedSurface);
            }
            RuntimeInvalidateHost(widgetId);
            return true;
        }
    }
    if (IsPanelSurface(normalizedSurface))
    {
        auto& regions = widget.panelInteractionRegions;
        auto& focusKey = widget.panelViewKeyboardFocusKey;
        const auto focusKeys = regions.KeyboardFocusableKeys(focusKey);
        if (focusKeys.empty())
        {
            focusKey.clear();
            widget.panelViewFocusCueVisible = false;
            return false;
        }
        const auto focusedIndex = [&]() -> std::optional<std::size_t> {
            if (focusKey.empty()) return std::nullopt;
            const auto found = std::find(
                focusKeys.begin(), focusKeys.end(), focusKey);
            if (found == focusKeys.end()) return std::nullopt;
            return static_cast<std::size_t>(
                std::distance(focusKeys.begin(), found));
        }();
        const auto focusTarget = [&](const std::string& targetKey) {
            const auto input = std::find_if(widget.hostControls.rbegin(),
                widget.hostControls.rend(), [&](const auto& control) {
                    return control.type ==
                            LuaWidget::HostControl::Type::Input &&
                        control.enabled && control.id == targetKey &&
                        HostControlBelongsToSurface(
                            control, normalizedSurface);
                });
            if (input != widget.hostControls.rend())
            {
                if (!RuntimeFocusHostInput(
                        widgetId, targetKey, "keyboard"))
                    return false;
            }
            else
            {
                if (focusedHostInput_.active &&
                    focusedHostInput_.widgetId == widgetId)
                    BlurHostInput(false);
                focusKey = targetKey;
                widget.panelViewFocusCueVisible = true;
            }
            RevealHostViewKeyboardFocus(widget, normalizedSurface);
            RuntimeInvalidateHost(widgetId);
            return true;
        };
        if (key == VK_TAB && !alt)
        {
            const auto next = snowdesktop::widget_runtime::
                CycleLogicalSlotFocus(
                    focusKeys.size(), focusedIndex, shift);
            return next ? focusTarget(focusKeys[*next]) : false;
        }
        if (!focusedIndex)
        {
            if (!alt && (key == VK_RETURN || key == VK_SPACE ||
                    key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN))
                return focusTarget(focusKeys.front());
            return false;
        }
        widget.panelViewFocusCueVisible = true;
        const std::string focusedKey = focusKeys[*focusedIndex];
        const auto* focusedRegion = regions.Find(focusedKey);
        if (!focusedRegion) return false;
        if (key == VK_ESCAPE)
        {
            if (focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == focusedKey &&
                IsPanelSurface(focusedHostInput_.surface))
                BlurHostInput(true);
            // Panel Escape belongs to its dismissal policy. Retain the logical
            // keyboard session when a non-dismissible panel remains open.
            return false;
        }
        if (key == VK_LEFT || key == VK_RIGHT ||
            key == VK_UP || key == VK_DOWN)
        {
            if (alt) return false;
            if (focusedRegion->controlKind ==
                snowdesktop::widget_runtime::
                    InteractionControlKind::Slider)
            {
                const int direction =
                    key == VK_LEFT || key == VK_DOWN ? -1 : 1;
                const auto stepped = regions.ResolveKeyboardStep(
                    focusedKey, direction);
                if (stepped && stepped->controlValue &&
                    stepped->previousControlValue &&
                    std::abs(*stepped->controlValue -
                        *stepped->previousControlValue) > 0.000001f)
                {
                    const auto& shape = focusedRegion->shape;
                    const int x = static_cast<int>(std::lround(
                        shape.type == snowdesktop::widget_runtime::
                                InteractionShapeType::Circle
                            ? shape.x
                            : shape.x + shape.width * 0.5f));
                    const int y = static_cast<int>(std::lround(
                        shape.type == snowdesktop::widget_runtime::
                                InteractionShapeType::Circle
                            ? shape.y
                            : shape.y + shape.height * 0.5f));
                    snowdesktop::widget_runtime::WidgetTrustedGestureScope
                        gestureScope(trustedGestureState_, true);
                    DispatchInteractionAction(widget, focusedKey, "change",
                        x, y, 0, 0, 0, false, "keyboard", direction,
                        std::nullopt, std::nullopt,
                        normalizedSurface);
                    RuntimeInvalidateHost(widgetId);
                }
                return true;
            }
            std::vector<snowdesktop::widget_runtime::LogicalSlotFocusRect>
                bounds;
            bounds.reserve(focusKeys.size());
            for (const auto& candidateKey : focusKeys)
            {
                const auto* candidate = regions.Find(candidateKey);
                if (!candidate) return true;
                const auto& shape = candidate->shape;
                float left = shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.x - shape.radius : shape.x;
                float top = shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.y - shape.radius : shape.y;
                float right = shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.x + shape.radius : shape.x + shape.width;
                float bottom = shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.y + shape.radius : shape.y + shape.height;
                bounds.push_back({
                    static_cast<int>(std::floor(left)),
                    static_cast<int>(std::floor(top)),
                    static_cast<int>(std::ceil(right)),
                    static_cast<int>(std::ceil(bottom)) });
            }
            const auto direction = key == VK_LEFT
                ? snowdesktop::widget_runtime::
                    LogicalSlotFocusDirection::Left
                : key == VK_RIGHT
                    ? snowdesktop::widget_runtime::
                        LogicalSlotFocusDirection::Right
                    : key == VK_UP
                        ? snowdesktop::widget_runtime::
                            LogicalSlotFocusDirection::Up
                        : snowdesktop::widget_runtime::
                            LogicalSlotFocusDirection::Down;
            const auto next = snowdesktop::widget_runtime::
                FindLogicalSlotSpatialFocus(
                    bounds, *focusedIndex, direction);
            if (next) focusTarget(focusKeys[*next]);
            return true;
        }
        if ((key == VK_RETURN || key == VK_SPACE) && !alt)
        {
            if (repeated) return true;
            const auto input = std::find_if(widget.hostControls.rbegin(),
                widget.hostControls.rend(), [&](const auto& control) {
                    return control.type ==
                            LuaWidget::HostControl::Type::Input &&
                        control.enabled && control.id == focusedKey &&
                        HostControlBelongsToSurface(
                            control, normalizedSurface);
                });
            if (input != widget.hostControls.rend())
                return RuntimeFocusHostInput(
                    widgetId, focusedKey, "keyboard");
            if (focusedRegion->controlKind !=
                snowdesktop::widget_runtime::
                    InteractionControlKind::Slider)
            {
                const auto& shape = focusedRegion->shape;
                const int x = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.x
                        : shape.x + shape.width * 0.5f));
                const int y = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.y
                        : shape.y + shape.height * 0.5f));
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, focusedKey, "click",
                    x, y, 0, 0, 1, false, "keyboard", 0,
                    std::nullopt, std::nullopt,
                    normalizedSurface);
                RuntimeInvalidateHost(widgetId);
            }
            return true;
        }
        return false;
    }
    const auto focusKeys = widget.interactionRegions.KeyboardFocusableKeys(widget.viewKeyboardFocusKey);
    if (focusKeys.empty())
    {
        widget.viewKeyboardFocusKey.clear();
        widget.viewFocusCueVisible = false;
        widget.logicalSlotFocus.reset();
        return false;
    }
    const auto slotItems = CollectHostLogicalSlotKeyboardItems(*this, widget);
    const auto focusedIndex = [&]() -> std::optional<std::size_t> {
        if (widget.viewKeyboardFocusKey.empty()) return std::nullopt;
        const auto found = std::find(
            focusKeys.begin(), focusKeys.end(),
            widget.viewKeyboardFocusKey);
        if (found == focusKeys.end()) return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(focusKeys.begin(), found));
    }();

    auto synchronizeSlotFocus = [&](std::string_view targetKey) {
        const auto slot = std::find_if(slotItems.begin(), slotItems.end(),
            [targetKey](const auto& item) {
                return item.itemId == targetKey;
            });
        if (slot == slotItems.end())
            widget.logicalSlotFocus.reset();
        else
        {
            widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
                slot->slotId, slot->itemId };
        }
    };
    auto focusTarget = [&](const std::string& targetKey) {
        const auto input = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type == LuaWidget::HostControl::Type::Input &&
                    control.enabled && control.id == targetKey &&
                    HostControlBelongsToSurface(control, normalizedSurface);
            });
        if (input != widget.hostControls.rend())
        {
            if (!RuntimeFocusHostInput(widgetId, targetKey, "keyboard"))
                return false;
        }
        else
        {
            if (focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId)
                BlurHostInput(false);
            widget.viewKeyboardFocusKey = targetKey;
            widget.viewFocusCueVisible = true;
        }
        synchronizeSlotFocus(targetKey);
        RevealHostViewKeyboardFocus(widget, normalizedSurface);
        RuntimeInvalidateHost(widgetId);
        return true;
    };
    auto focusedSlot = [&]() -> const HostLogicalSlotKeyboardItem* {
        if (!widget.logicalSlotFocus) return nullptr;
        const auto found = std::find_if(slotItems.begin(), slotItems.end(),
            [&](const auto& item) {
                return item.slotId == widget.logicalSlotFocus->slotId &&
                    item.itemId == widget.logicalSlotFocus->itemId;
            });
        return found == slotItems.end() ? nullptr : &*found;
    };

    if (key == VK_RETURN)
    {
        const auto entry = snowdesktop::widget_runtime::
            EnterWidgetKeyboardFocus(
                focusKeys.size(), focusedIndex, shift, alt, repeated);
        if (entry)
            return focusTarget(focusKeys[*entry]);
    }
    if (key == VK_TAB && !alt)
    {
        const auto next = snowdesktop::widget_runtime::CycleLogicalSlotFocus(
            focusKeys.size(), focusedIndex, shift);
        if (!next) return false;
        return focusTarget(focusKeys[*next]);
    }
    if (!focusedIndex) return false;
    widget.viewFocusCueVisible = true;
    const std::string focusedKey = focusKeys[*focusedIndex];
    const auto* focusedRegion =
        widget.interactionRegions.Find(focusedKey);
    if (!focusedRegion) return false;
    if (key == VK_ESCAPE)
    {
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == focusedKey)
            BlurHostInput(true);
        widget.logicalSlotFocus.reset();
        widget.viewKeyboardFocusKey.clear();
        widget.viewFocusCueVisible = false;
        RuntimeInvalidateHost(widgetId);
        return true;
    }
    if (key == VK_LEFT || key == VK_RIGHT ||
        key == VK_UP || key == VK_DOWN)
    {
        if (!alt && focusedRegion->controlKind ==
                snowdesktop::widget_runtime::InteractionControlKind::Slider)
        {
            const int direction = key == VK_LEFT || key == VK_DOWN ? -1 : 1;
            const auto stepped = widget.interactionRegions.ResolveKeyboardStep(
                focusedKey, direction);
            if (stepped && stepped->controlValue &&
                stepped->previousControlValue &&
                std::abs(*stepped->controlValue -
                    *stepped->previousControlValue) > 0.000001f)
            {
                const auto& shape = focusedRegion->shape;
                const int x = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.x : shape.x + shape.width * 0.5f));
                const int y = static_cast<int>(std::lround(
                    shape.type == snowdesktop::widget_runtime::
                            InteractionShapeType::Circle
                        ? shape.y : shape.y + shape.height * 0.5f));
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, focusedKey, "change",
                    x, y, 0, 0, 0, false, "keyboard", direction);
                RuntimeInvalidateHost(widgetId);
            }
            return true;
        }
        if (alt)
        {
            const auto* slot = focusedSlot();
            if (!slot) return false;
            if (slot->kind != snowdesktop::widget_runtime::
                    LogicalSlotKind::Collection)
                return true;
            const int direction = key == VK_LEFT || key == VK_UP ? -1 : 1;
            const auto target = snowdesktop::widget_runtime::
                MoveLogicalSlotItemTarget(
                    slot->itemCount, slot->modelIndex, direction);
            if (!target) return true;
            snowdesktop::widget_runtime::LogicalSlotChange change;
            std::string error;
            if (!RuntimeMoveHostLogicalSlotItem(widgetId, slot->slotId,
                    slot->itemId, *target, change, error,
                    "host.keyboard"))
            {
                RuntimeRecordError(widgetId,
                    "logical slot keyboard reorder: " + error);
            }
            return true;
        }
        std::vector<snowdesktop::widget_runtime::LogicalSlotFocusRect> bounds;
        bounds.reserve(focusKeys.size());
        for (const auto& candidateKey : focusKeys)
        {
            const auto* candidate =
                widget.interactionRegions.Find(candidateKey);
            if (!candidate) return true;
            const auto& shape = candidate->shape;
            float left = shape.type == snowdesktop::widget_runtime::
                    InteractionShapeType::Circle
                ? shape.x - shape.radius : shape.x;
            float top = shape.type == snowdesktop::widget_runtime::
                    InteractionShapeType::Circle
                ? shape.y - shape.radius : shape.y;
            float right = shape.type == snowdesktop::widget_runtime::
                    InteractionShapeType::Circle
                ? shape.x + shape.radius : shape.x + shape.width;
            float bottom = shape.type == snowdesktop::widget_runtime::
                    InteractionShapeType::Circle
                ? shape.y + shape.radius : shape.y + shape.height;
            bounds.push_back({
                static_cast<int>(std::floor(left)),
                static_cast<int>(std::floor(top)),
                static_cast<int>(std::ceil(right)),
                static_cast<int>(std::ceil(bottom)) });
        }
        const auto direction = key == VK_LEFT
            ? snowdesktop::widget_runtime::LogicalSlotFocusDirection::Left
            : key == VK_RIGHT
                ? snowdesktop::widget_runtime::LogicalSlotFocusDirection::Right
                : key == VK_UP
                    ? snowdesktop::widget_runtime::LogicalSlotFocusDirection::Up
                    : snowdesktop::widget_runtime::LogicalSlotFocusDirection::Down;
        const auto next = snowdesktop::widget_runtime::
            FindLogicalSlotSpatialFocus(bounds, *focusedIndex, direction);
        if (next)
            focusTarget(focusKeys[*next]);
        return true;
    }
    if (key == VK_DELETE && !alt)
    {
        const auto* slot = focusedSlot();
        if (!slot) return false;
        const bool removable = slot->kind == snowdesktop::widget_runtime::
                LogicalSlotKind::Collection || slot->allowClear;
        if (!removable) return true;
        std::string fallback;
        if (focusKeys.size() > 1)
        {
            const std::size_t next = *focusedIndex + 1 < focusKeys.size()
                ? *focusedIndex + 1 : *focusedIndex - 1;
            fallback = focusKeys[next];
        }
        snowdesktop::widget_runtime::LogicalSlotChange change;
        std::string error;
        if (RuntimeRemoveHostLogicalSlotItem(widgetId, slot->slotId,
                slot->itemId, change, error, "host.keyboard"))
        {
            widget.viewKeyboardFocusKey = std::move(fallback);
            synchronizeSlotFocus(widget.viewKeyboardFocusKey);
        }
        else
            RuntimeRecordError(widgetId,
                "logical slot keyboard remove: " + error);
        RuntimeInvalidateHost(widgetId);
        return true;
    }
    if ((key == VK_RETURN || key == VK_SPACE) && !alt)
    {
        if (repeated) return true;
        const auto input = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type == LuaWidget::HostControl::Type::Input &&
                    control.enabled && control.id == focusedKey &&
                    HostControlBelongsToSurface(control, normalizedSurface);
            });
        if (input != widget.hostControls.rend())
            return RuntimeFocusHostInput(widgetId, focusedKey, "keyboard");
        if (focusedRegion->controlKind != snowdesktop::widget_runtime::
                InteractionControlKind::Slider)
        {
            const auto& shape = focusedRegion->shape;
            const int x = static_cast<int>(std::lround(
                shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.x : shape.x + shape.width * 0.5f));
            const int y = static_cast<int>(std::lround(
                shape.type == snowdesktop::widget_runtime::
                        InteractionShapeType::Circle
                    ? shape.y : shape.y + shape.height * 0.5f));
            snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                trustedGestureState_, true);
            DispatchInteractionAction(widget, focusedKey, "click",
                x, y, 0, 0, 1, false, "keyboard");
            RuntimeInvalidateHost(widgetId);
        }
        return true;
    }
    return false;
}

bool WidgetEngine::HandleHostInputPointerMove(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface)
{
    if (HandleHostScrollbarPointer(widgetId, x, y, surface, false))
        return true;
    const int widgetIndex = FindWidget(widgetId);
    if (!IsPanelSurface(surface) && widgetIndex >= 0 &&
        UpdateHostLogicalSlotPointer(widgets_[widgetIndex], x, y))
        return true;
    if (!focusedHostInput_.active ||
        !focusedHostInput_.pointerSelecting ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.surface != surface)
        return false;
    if (focusedHostInput_.selectionDragArmed)
        return true;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    auto& widget = widgets_[index];
    auto control = std::find_if(
        widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& item) {
            return item.type ==
                LuaWidget::HostControl::Type::Input &&
                item.id == focusedHostInput_.id &&
                HostControlBelongsToSurface(item, surface);
        });
    if (control == widget.hostControls.rend())
        return false;
    if (control->multiline)
    {
        int offset = RuntimeGetScrollOffset(
            widgetId, control->id, surface);
        if (y < control->rect.top +
            static_cast<int>(std::lround(control->padding.top)))
            offset -= 24;
        else if (y > control->rect.bottom -
            static_cast<int>(std::lround(control->padding.bottom)))
            offset += 24;
        RuntimeSetScrollOffset(widgetId,
            control->id, offset, surface);
    }
    focusedHostInput_.cursor = HitTestHostInputPosition(
        *control, widgetId, x, y);
    focusedHostInput_.caretVisibility.Request();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::HandleHostInputPointerUp(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface)
{
    if (HandleHostScrollbarPointer(widgetId, x, y, surface, true))
        return true;
    if (!IsPanelSurface(surface) && EndHostLogicalSlotPointer(widgetId))
        return true;
    if (!focusedHostInput_.active ||
        !focusedHostInput_.pointerSelecting ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.surface != surface)
        return false;
    const bool dragWasArmed = std::exchange(focusedHostInput_.selectionDragArmed, false);
    const int index = FindWidget(widgetId);
    if (index >= 0)
    {
        auto& controls = widgets_[index].hostControls;
        auto control = std::find_if(controls.rbegin(),
            controls.rend(), [&](const auto& item) {
                return item.type ==
                    LuaWidget::HostControl::Type::Input &&
                    item.id == focusedHostInput_.id &&
                    HostControlBelongsToSurface(item, surface);
            });
        if (control != controls.rend())
        {
            const auto dropPosition = dragWasArmed
                ? ResolveHostInputTextDropPosition(
                    widgets_[index], *control, {x, y}, focusedHostInput_.text, nullptr)
                : std::nullopt;
            focusedHostInput_.cursor = dropPosition ? *dropPosition :
                HitTestHostInputPosition(*control, widgetId, x, y);
            if (dragWasArmed)
                focusedHostInput_.selectionAnchor = focusedHostInput_.cursor;
            focusedHostInput_.caretVisibility.Request();
        }
    }
    DispatchHostInputSelectionChange(widgetId,
        focusedHostInput_.id,
        focusedHostInput_.selectionChangeAction,
        focusedHostInput_.text,
        focusedHostInput_.pointerSelectionStartAnchor,
        focusedHostInput_.pointerSelectionStartCursor,
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.cursor, "pointer");
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

std::vector<LuaWidgetAccessibilitySnapshot>
WidgetEngine::RuntimeAccessibilitySnapshots() const
{
    std::vector<LuaWidgetAccessibilitySnapshot> result;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview || !widget.hostVisible)
            continue;
        LuaWidgetAccessibilitySnapshot snapshot;
        snapshot.widgetId = widget.widgetId;
        snapshot.packageId = widget.packageId;
        snapshot.name = widget.name;
        snapshot.bounds = widget.lastBounds;
        snapshot.selected = RuntimeIsWidgetSelected(widget.widgetId);
        const std::string_view desktopFocus = focusedHostInput_.active &&
                focusedHostInput_.widgetId == widget.widgetId &&
                focusedHostInput_.surface != "desktop"
            ? std::string_view{} : std::string_view(widget.viewKeyboardFocusKey);
        const bool collected = widget.viewTree
            ? snowdesktop::widget_runtime::CollectViewAccessibilityNodes(
                *widget.viewTree, desktopFocus,
                snapshot.nodes, snapshot.error)
            : snowdesktop::widget_runtime::
                CollectInteractionAccessibilityNodes(
                    widget.interactionRegions.AccessibilityRegions(),
                    static_cast<float>(std::max<LONG>(0,
                        widget.lastBounds.right - widget.lastBounds.left)),
                    static_cast<float>(std::max<LONG>(0,
                        widget.lastBounds.bottom - widget.lastBounds.top)),
                    desktopFocus,
                    snapshot.nodes, snapshot.error);
        (void)collected;
        for (auto& node : snapshot.nodes)
        {
            if (node.key.empty()) continue;
            const auto projected = snowdesktop::widget_runtime::
                ProjectHostInputAccessibility(widget.hostControls,
                    focusedHostInput_, widget.widgetId, node.key, "desktop",
                    [this, &widget](const std::string& key) {
                        return RuntimeGetStorageValue(widget.widgetId, key);
                    }, [](const std::wstring& value) { return WidgetWideToUtf8(value); });
            if (!projected) continue;
            const auto* control = projected->control;
            node.valueText = projected->valueText;
            node.focused = projected->focused;
            node.valueReadOnly = control->readOnly;
            if (d2dState_)
            {
                text_input::AccessibleDocument document;
                document.text=Utf8ToWideLocal(node.valueText);
                document.cursor=document.anchor=document.text.size();
                if(projected->focused)
                {document.cursor=projected->cursor;document.anchor=projected->anchor;}
                document.enabled=node.enabled;document.readOnly=control->readOnly;
                const float innerWidth=std::max(1.f,node.bounds.width-control->padding.left-control->padding.right-(control->multiline?8.f:0.f));
                const float innerHeight=std::max(1.f,node.bounds.height-control->padding.top-control->padding.bottom);
                auto layout=control->multiline
                    ? CreateHostMultilineTextLayout(d2dState_,document.text,control->fontSize,innerWidth)
                    : CreateHostSingleLineTextLayout(d2dState_,document.text,control->fontSize,innerWidth,innerHeight);
                const float left=static_cast<float>(widget.lastBounds.left)+node.bounds.x+control->padding.left;
                const float top=static_cast<float>(widget.lastBounds.top)+node.bounds.y+control->padding.top;
                const float scroll=control->multiline?static_cast<float>(RuntimeGetScrollOffset(widget.widgetId,node.key)):0.f;
                const D2D1_RECT_F clip=D2D1::RectF(left,top,left+innerWidth,top+innerHeight);
                auto access=std::make_shared<text_input::TextAccess>();
                access->document=[document]{return std::optional<text_input::AccessibleDocument>{document};};
                access->rectangles=[layout,left,top,scroll,clip](std::size_t start,std::size_t end){
                    std::vector<text_input::Rectangle> boxesResult;if(!layout)return boxesResult;
                    UINT32 count=0;layout->HitTestTextRange(static_cast<UINT32>(start),static_cast<UINT32>(end-start),left,top-scroll,nullptr,0,&count);
                    std::vector<DWRITE_HIT_TEST_METRICS> boxes(count);
                    if(count&&SUCCEEDED(layout->HitTestTextRange(static_cast<UINT32>(start),static_cast<UINT32>(end-start),left,top-scroll,boxes.data(),count,&count)))
                        for(const auto& box:boxes){const float x=std::max(clip.left,box.left),y=std::max(clip.top,box.top);
                            const float right=std::min(clip.right,box.left+box.width),bottom=std::min(clip.bottom,box.top+box.height);
                            if(right>=x&&bottom>y)boxesResult.push_back({x,y,right-x,bottom-y});}
                    return boxesResult;
                };
                access->hit=[layout,left,top,scroll,text=document.text](text_input::Point point)->std::optional<std::size_t>{
                    if(!layout)return {};BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS hit{};
                    if(FAILED(layout->HitTestPoint(static_cast<float>(point.x)-left,static_cast<float>(point.y)-top+scroll,&trailing,&inside,&hit)))return {};
                    return text_input::SnapBoundary(text,hit.textPosition+(trailing?hit.length:0));
                };
                access->unitBoundaries=[layout,length=document.text.size()](text_input::Unit unit){
                    std::vector<std::size_t> positions;if(unit!=text_input::Unit::Line||!layout)return positions;
                    UINT32 count=0;layout->GetLineMetrics(nullptr,0,&count);std::vector<DWRITE_LINE_METRICS> lines(count);
                    if(count&&SUCCEEDED(layout->GetLineMetrics(lines.data(),count,&count))){std::size_t at=0;positions.push_back(0);
                        for(const auto& line:lines){at=std::min(length,at+line.length);positions.push_back(at);}}
                    return positions;
                };
                access->visibleRanges=[layout,scroll,innerHeight,length=document.text.size()]{
                    std::vector<std::pair<std::size_t,std::size_t>> ranges;if(!layout)return ranges;
                    UINT32 count=0;layout->GetLineMetrics(nullptr,0,&count);std::vector<DWRITE_LINE_METRICS> lines(count);
                    if(count&&SUCCEEDED(layout->GetLineMetrics(lines.data(),count,&count))){std::size_t at=0;float y=-scroll;
                        for(const auto& line:lines){const auto end=std::min(length,at+line.length);if(y+line.height>0&&y<innerHeight)ranges.emplace_back(at,end);at=end;y+=line.height;}}
                    return ranges;
                };
                node.textAccess=std::move(access);node.textAccessUsesScreenCoordinates=false;
            }
            if (control->numeric)
            {
                node.minimum = control->minimum;
                node.maximum = control->maximum;
                node.step = control->step;
                node.rangeValueReadOnly = control->readOnly;
                float parsed = 0.0f;
                const auto parsedResult = std::from_chars(
                    node.valueText.data(),
                    node.valueText.data() + node.valueText.size(), parsed);
                if (parsedResult.ec == std::errc{} &&
                    parsedResult.ptr == node.valueText.data() +
                        node.valueText.size() && std::isfinite(parsed))
                    node.value = parsed;
            }
        }
        result.push_back(std::move(snapshot));
    }
    return result;
}

bool WidgetEngine::RuntimeSetAccessibilityFocus(
    const std::wstring& widgetId, const std::string& nodeKey)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || !RuntimeIsWidgetSelected(widgetId)) return false;
    auto& widget = widgets_[index];
    if (!widget.valid || widget.preview || !widget.hostVisible ||
        !widget.interactionRegions.IsKeyboardFocusable(nodeKey))
        return false;
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");

    const auto input = std::find_if(widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.enabled && control.id == nodeKey &&
                HostControlBelongsToSurface(control, "desktop");
        });
    if (input != widget.hostControls.rend())
    {
        if (!RuntimeFocusHostInput(widgetId, nodeKey, "accessibility"))
            return false;
    }
    else
    {
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId)
            BlurHostInput(false);
        widget.viewKeyboardFocusKey = nodeKey;
        widget.viewFocusCueVisible = true;
    }

    widget.logicalSlotFocus.reset();
    const auto slotItems = CollectHostLogicalSlotKeyboardItems(*this, widget);
    const auto slot = std::find_if(slotItems.begin(), slotItems.end(),
        [&](const auto& item) { return item.itemId == nodeKey; });
    if (slot != slotItems.end())
        widget.logicalSlotFocus = LuaWidget::LogicalSlotFocus{
            slot->slotId, slot->itemId };
    RevealHostViewKeyboardFocus(widget, "desktop");
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::HandleHostScrollbarPointer(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface, bool finish)
{
    const std::string normalizedSurface = NormalizeWidgetSurface(surface);
    if (!hostScrollbarDrag_.active ||
        hostScrollbarDrag_.widgetId != widgetId ||
        hostScrollbarDrag_.surface != normalizedSurface)
        return false;

    const HostScrollbarDrag drag = hostScrollbarDrag_;
    if (finish)
        hostScrollbarDrag_ = {};
    const int index = FindWidget(widgetId);
    if (index < 0)
        return true;
    auto& widget = widgets_[index];
    const auto control = std::find_if(
        widget.hostControls.rbegin(), widget.hostControls.rend(),
        [&](const auto& item) {
            return item.enabled && item.id == drag.id &&
                HostControlBelongsToSurface(item, normalizedSurface) &&
                (item.type == LuaWidget::HostControl::Type::Scroll ||
                    (item.type == LuaWidget::HostControl::Type::Input &&
                        item.multiline));
        });
    if (control == widget.hostControls.rend())
        return true;

    const int contentExtent = drag.horizontal
        ? control->contentWidth : control->contentHeight;
    const int visibleExtent = drag.horizontal
        ? control->viewportWidth : control->viewportHeight;
    const int viewportStart = drag.horizontal
        ? control->rect.left : control->rect.top;
    const int viewportEnd = drag.horizontal
        ? control->rect.right : control->rect.bottom;
    const float scale = CalculateWidgetCellScale(
        widget.layoutMetrics.gridCellWidth,
        widget.layoutMetrics.gridCellHeight);
    const auto geometry = snowdesktop::widget_scroll_rules::
        ResolveScrollbarAxisGeometry(
            viewportStart, viewportEnd, contentExtent,
            visibleExtent, drag.offsetStart, scale);
    const int pointer = drag.horizontal ? x : y;
    const int previous = RuntimeGetScrollOffset(
        widgetId, drag.id, normalizedSurface);
    const int current = snowdesktop::widget_scroll_rules::
        ApplyScrollbarThumbDrag(
            drag.offsetStart, pointer - drag.pointerStart, geometry);
    ScrollOffsetsForSurface(widget, normalizedSurface)[drag.id] = current;
    if (current != previous)
    {
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId &&
            focusedHostInput_.id == drag.id &&
            focusedHostInput_.surface == normalizedSurface)
        {
            focusedHostInput_.caretVisibility.PreserveManualScroll();
        }
        if (snowdesktop::widget_scroll_rules::ReachedScrollEnd(
                previous, current, geometry.maximum))
        {
            snowdesktop::widget_runtime::WidgetTrustedGestureScope
                gestureScope(trustedGestureState_, true);
            DispatchInteractionAction(widget, drag.id, "scrollEnd",
                x, y, 0, 0, 0, false, "pointer", 0,
                std::nullopt, std::nullopt, normalizedSurface);
        }
        RuntimeInvalidateHost(widgetId);
    }
    return true;
}

bool WidgetEngine::RuntimePerformAccessibilityAction(
    const LuaWidgetAccessibilityActionRequest& request)
{
    const int index = FindWidget(request.widgetId);
    if (index < 0 || !RuntimeIsWidgetSelected(request.widgetId))
        return false;
    auto& widget = widgets_[index];
    if (!widget.valid || widget.preview || !widget.hostVisible ||
        request.nodeKey.empty())
        return false;
    WidgetSurfaceScope surfaceScope(d2dState_, "desktop");

    const auto setHostValue = [&](std::wstring value) {
        const auto control = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& candidate) {
                return candidate.type ==
                        LuaWidget::HostControl::Type::Input &&
                    candidate.id == request.nodeKey && HostControlBelongsToSurface(candidate, "desktop");
            });
        if (control == widget.hostControls.rend() ||
            !snowdesktop::widget_runtime::HostInputAllowsMutation(
                control->enabled, control->readOnly) ||
            (!control->controlled && control->storageKey.empty()) ||
            !snowdesktop::widget_runtime::HostTextReplacementFits(
                {}, 0, 0, value, control->maximumUtf8Bytes))
            return false;
        if (control->numeric)
        {
            if (value.empty()) return false;
            wchar_t* end = nullptr;
            const double parsed = std::wcstod(value.c_str(), &end);
            if (!end || end != value.c_str() + value.size() ||
                !std::isfinite(parsed) || parsed < control->minimum ||
                parsed > control->maximum)
                return false;
        }

        std::wstring previous = Utf8ToWideLocal(control->controlled
            ? control->controlledText
            : RuntimeGetStorageValue(
                request.widgetId, control->storageKey));
        if (focusedHostInput_.active &&
            focusedHostInput_.widgetId == request.widgetId &&
            focusedHostInput_.id == request.nodeKey && focusedHostInput_.surface == "desktop")
            previous = focusedHostInput_.text;
        if (previous == value) return true;

        const bool focused = focusedHostInput_.active &&
            focusedHostInput_.widgetId == request.widgetId &&
            focusedHostInput_.id == request.nodeKey && focusedHostInput_.surface == "desktop";
        if (focused)
        {
            focusedHostInput_.text = value;
            focusedHostInput_.history.Clear();
            focusedHostInput_.originalText = value;
            focusedHostInput_.cursor = value.size();
            focusedHostInput_.selectionAnchor = value.size();
            focusedHostInput_.compositionText.clear();
            focusedHostInput_.compositionCursor = 0;
            focusedHostInput_.caretVisibility.Request();
        }

        if (control->controlled)
        {
            DispatchHostInputChange(request.widgetId, request.nodeKey,
                control->changeAction, previous, value,
                control->numeric, control->minimum, control->maximum,
                true, false, "accessibility");
        }
        else
        {
            RuntimeSetStorageValue(request.widgetId,
                control->storageKey, WidgetWideToUtf8(value));
        }
        RuntimeInvalidateHost(request.widgetId);
        return true;
    };

    if (request.kind == LuaWidgetAccessibilityActionKind::SetValue)
        return setHostValue(request.textValue);
    if (request.kind == LuaWidgetAccessibilityActionKind::SetTextSelection ||
        request.kind == LuaWidgetAccessibilityActionKind::RevealTextPosition)
    {
        const auto control=std::find_if(widget.hostControls.rbegin(),widget.hostControls.rend(),[&](const auto& candidate){
            return candidate.type==LuaWidget::HostControl::Type::Input&&candidate.enabled&&candidate.id==request.nodeKey&&HostControlBelongsToSurface(candidate,"desktop");});
        if(control==widget.hostControls.rend())return false;
        if(request.kind==LuaWidgetAccessibilityActionKind::SetTextSelection)
        {
            if(!focusedHostInput_.active||focusedHostInput_.widgetId!=request.widgetId||focusedHostInput_.id!=request.nodeKey||focusedHostInput_.surface!="desktop")
                if(!RuntimeFocusHostInput(request.widgetId,request.nodeKey,"accessibility"))return false;
            if(focusedHostInput_.composing)return false;
            const auto previousAnchor=focusedHostInput_.selectionAnchor,previousCursor=focusedHostInput_.cursor;
            focusedHostInput_.selectionAnchor=text_input::SnapBoundary(focusedHostInput_.text,request.textAnchor);
            focusedHostInput_.cursor=text_input::SnapBoundary(focusedHostInput_.text,request.textCursor);
            focusedHostInput_.caretVisibility.Request();
            if(focusedHostInput_.controlledSelection)
                DispatchHostInputSelectionChange(request.widgetId,request.nodeKey,focusedHostInput_.selectionChangeAction,
                    focusedHostInput_.text,previousAnchor,previousCursor,focusedHostInput_.selectionAnchor,focusedHostInput_.cursor,"accessibility");
        }
        else if(control->multiline&&d2dState_)
        {
            const auto text=focusedHostInput_.active&&focusedHostInput_.widgetId==request.widgetId&&focusedHostInput_.id==request.nodeKey&&focusedHostInput_.surface=="desktop"
                ?focusedHostInput_.text:Utf8ToWideLocal(control->controlled?control->controlledText:RuntimeGetStorageValue(request.widgetId,control->storageKey));
            const auto layout=CreateHostMultilineTextLayout(d2dState_,text,control->fontSize,
                std::max(1.f,static_cast<float>(control->rect.right-control->rect.left)-control->padding.left-control->padding.right-8.f));
            if(!layout)return false;float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};
            if(FAILED(layout->HitTestTextPosition(static_cast<UINT32>(std::min(request.textCursor,text.size())),FALSE,&x,&y,&hit)))return false;
            RuntimeSetScrollOffset(request.widgetId,request.nodeKey,static_cast<int>(std::lround(y)));
        }
        RuntimeInvalidateHost(request.widgetId);return true;
    }
    if (request.kind ==
        LuaWidgetAccessibilityActionKind::SetScrollOffset)
    {
        if (!std::isfinite(request.numericValue) ||
            request.numericValue < 0.0 ||
            request.numericValue > 1'000'000.0)
            return false;
        const auto scroll = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type == LuaWidget::HostControl::Type::Scroll &&
                    control.enabled && control.id == request.nodeKey && HostControlBelongsToSurface(control, "desktop");
            });
        if (scroll == widget.hostControls.rend()) return false;
        const int maximum = scroll->horizontal
            ? std::max(0, scroll->contentWidth - scroll->viewportWidth)
            : std::max(0, scroll->contentHeight - scroll->viewportHeight);
        const int previousOffset = RuntimeGetScrollOffset(
            request.widgetId, request.nodeKey);
        RuntimeSetScrollOffset(request.widgetId, request.nodeKey,
            static_cast<int>(std::lround(request.numericValue)));
        const int currentOffset = RuntimeGetScrollOffset(
            request.widgetId, request.nodeKey);
        if (snowdesktop::widget_scroll_rules::ReachedScrollEnd(
                previousOffset, currentOffset, maximum))
            DispatchInteractionAction(widget, request.nodeKey, "scrollEnd",
                0, 0, 0, 0, 0, false, "accessibility");
        RuntimeInvalidateHost(request.widgetId);
        return true;
    }
    if (request.kind ==
        LuaWidgetAccessibilityActionKind::SetRangeValue)
    {
        if (!std::isfinite(request.numericValue)) return false;
        const auto input = std::find_if(widget.hostControls.rbegin(),
            widget.hostControls.rend(), [&](const auto& control) {
                return control.type ==
                        LuaWidget::HostControl::Type::Input &&
                    control.numeric && control.id == request.nodeKey && HostControlBelongsToSurface(control, "desktop");
            });
        if (input != widget.hostControls.rend())
        {
            char buffer[64]{};
            const auto converted = std::to_chars(buffer,
                buffer + sizeof(buffer), request.numericValue,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);
            if (converted.ec != std::errc{}) return false;
            return setHostValue(Utf8ToWideLocal(std::string(
                buffer, converted.ptr)));
        }
    }

    const auto* region =
        widget.interactionRegions.Find(request.nodeKey);
    if (!region || !region->enabled) return false;
    const auto& shape = region->shape;
    const int x = static_cast<int>(std::lround(
        shape.type == snowdesktop::widget_runtime::
                InteractionShapeType::Circle
            ? shape.x : shape.x + shape.width * 0.5f));
    const int y = static_cast<int>(std::lround(
        shape.type == snowdesktop::widget_runtime::
                InteractionShapeType::Circle
            ? shape.y : shape.y + shape.height * 0.5f));

    if (request.kind ==
        LuaWidgetAccessibilityActionKind::SetRangeValue)
    {
        const float value = static_cast<float>(request.numericValue);
        if (!widget.interactionRegions.ResolveRangeValue(
                request.nodeKey, value))
            return false;
        DispatchInteractionAction(widget, request.nodeKey, "change",
            x, y, 0, 0, 0, false, "accessibility", 0, value);
        RuntimeInvalidateHost(request.widgetId);
        return true;
    }

    const char* eventName = "click";
    if (request.kind == LuaWidgetAccessibilityActionKind::Select ||
        request.kind == LuaWidgetAccessibilityActionKind::AddToSelection ||
        request.kind ==
            LuaWidgetAccessibilityActionKind::RemoveFromSelection)
    {
        const bool single = region->controlKind ==
            snowdesktop::widget_runtime::
                InteractionControlKind::SelectionSingle;
        const bool multiple = region->controlKind ==
            snowdesktop::widget_runtime::
                InteractionControlKind::SelectionMultiple;
        if (single || multiple)
        {
            if (!widget.interactionRegions.FindAction(
                    request.nodeKey, "change"))
                return false;
            std::vector<std::string> selectedKeys =
                region->currentSelectedKeys;
            const auto selected = std::find(selectedKeys.begin(),
                selectedKeys.end(), region->proposedSelectedKey);
            if (request.kind == LuaWidgetAccessibilityActionKind::Select)
                selectedKeys = { region->proposedSelectedKey };
            else if (!multiple)
                return false;
            else if (request.kind ==
                LuaWidgetAccessibilityActionKind::AddToSelection)
            {
                if (selected == selectedKeys.end())
                    selectedKeys.push_back(region->proposedSelectedKey);
            }
            else if (selected != selectedKeys.end())
                selectedKeys.erase(selected);
            if (selectedKeys == region->currentSelectedKeys) return true;
            DispatchInteractionAction(widget, request.nodeKey, "change",
                x, y, 0, 0, 0, false, "accessibility", 0,
                std::nullopt, std::move(selectedKeys));
            RuntimeInvalidateHost(request.widgetId);
            return true;
        }
        if (request.kind != LuaWidgetAccessibilityActionKind::Select)
            return false;
    }
    if (request.kind == LuaWidgetAccessibilityActionKind::Toggle)
    {
        if ((region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::Toggle &&
                region->controlKind != snowdesktop::widget_runtime::
                    InteractionControlKind::Checkbox) ||
            !widget.interactionRegions.FindAction(
                request.nodeKey, "change"))
            return false;
    }
    else if (request.kind == LuaWidgetAccessibilityActionKind::Select)
    {
        if (region->controlKind == snowdesktop::widget_runtime::
                InteractionControlKind::Radio)
        {
            if (!widget.interactionRegions.FindAction(
                    request.nodeKey, "change"))
                return false;
        }
        else if (!widget.interactionRegions.FindAction(
                     request.nodeKey, "click"))
            return false;
    }
    else if (request.kind == LuaWidgetAccessibilityActionKind::Expand ||
        request.kind == LuaWidgetAccessibilityActionKind::Collapse)
    {
        const bool expand = request.kind ==
            LuaWidgetAccessibilityActionKind::Expand;
        if (!region->hasExpandedProposal ||
            !widget.interactionRegions.FindAction(
                request.nodeKey, "click"))
            return false;
        if (region->expanded == expand) return true;
    }
    else if (request.kind != LuaWidgetAccessibilityActionKind::Invoke ||
        !widget.interactionRegions.FindAction(
            request.nodeKey, "click"))
        return false;

    DispatchInteractionAction(widget, request.nodeKey, eventName,
        x, y, 0, 0, 1, false, "accessibility");
    RuntimeInvalidateHost(request.widgetId);
    return true;
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
    const std::wstring& widgetId, int x, int y,
    std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const POINT point{ x, y };
    for (const auto& control : widgets_[index].hostControls)
        if (control.selectPopup && control.enabled &&
            HostControlBelongsToSurface(control, surface) &&
            HostControlContainsPoint(control, point))
            return false;
    for (auto it = widgets_[index].hostControls.rbegin();
        it != widgets_[index].hostControls.rend(); ++it)
    {
        if (it->type == LuaWidget::HostControl::Type::Input &&
            it->enabled && HostControlBelongsToSurface(*it, surface) &&
            HostControlContainsPoint(*it, point))
            return true;
    }
    return false;
}

const LuaWidget::HostControl* WidgetEngine::HitTestHostInputTextDrop(
    const LuaWidget& widget, POINT point, std::string_view surface) const
{
    const bool visible = IsPanelSurface(surface)
        ? widget.panelActive && widget.panelSurface == surface : widget.desktopVisible;
    if (!widget.valid || !visible || widget.preview)
        return nullptr;
    for (const auto& control : widget.hostControls)
        if (control.selectPopup && control.enabled &&
            HostControlBelongsToSurface(control, surface) &&
            HostControlContainsPoint(control, point))
            return nullptr;
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
        if (it->type == LuaWidget::HostControl::Type::Input &&
            HostControlBelongsToSurface(*it, surface) &&
            HostControlContainsPoint(*it, point))
            return &*it;
    return nullptr;
}

bool WidgetEngine::HostInputAcceptsTextDrop(
    const LuaWidget::HostControl& control) const
{
    return d2dState_ && !IsHostInputComposing() && control.focusable &&
        snowdesktop::widget_runtime::HostInputAllowsMutation(
            control.enabled, control.readOnly) &&
        (control.controlled || !control.storageKey.empty());
}

std::wstring WidgetEngine::HostInputTextForDrop(
    const LuaWidget& widget, const LuaWidget::HostControl& control) const
{
    if (focusedHostInput_.active && focusedHostInput_.surface == control.surface &&
        focusedHostInput_.widgetId == widget.widgetId &&
        focusedHostInput_.id == control.id)
        return focusedHostInput_.text;
    return Utf8ToWideLocal(control.controlled ? control.controlledText :
        RuntimeGetStorageValue(widget.widgetId, control.storageKey));
}

std::optional<std::string> WidgetEngine::FindHostInputTextDropTarget(
    const std::wstring& widgetId, POINT point, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return std::nullopt;
    const auto* control = HitTestHostInputTextDrop(widgets_[index], point, surface);
    if (!control) return std::nullopt;
    return HostInputAcceptsTextDrop(*control) ? control->id : std::string{};
}

std::optional<size_t> WidgetEngine::ResolveHostInputTextDropPosition(
    const LuaWidget& widget, const LuaWidget::HostControl& control,
    POINT point, const std::wstring& text, HostInputDropCaret* caret,
    std::optional<size_t>* hitCharacter) const
{
    using namespace snowdesktop::widget_runtime;
    ViewRect frame{static_cast<float>(control.rect.left),
        static_cast<float>(control.rect.top),
        static_cast<float>(control.rect.right - control.rect.left),
        static_cast<float>(control.rect.bottom - control.rect.top)};
    ViewRect content{frame.x + control.padding.left,
        frame.y + (control.multiline ? control.padding.top : 0.0f),
        frame.width - control.padding.left - control.padding.right,
        frame.height - (control.multiline
            ? control.padding.top + control.padding.bottom : 0.0f)};
    D2D1::Matrix3x2F transform = D2D1::Matrix3x2F::Identity();
    const auto* tree = DeclarativeViewForSurface(widget, control.surface);
    const auto* node = tree ? FindViewNodeByKey(*tree, control.id) : nullptr;
    if (node && IsDeclarativeInputNode(node->type))
    {
        frame = node->frame;
        content = ViewNodeContentRect(*node);
        const auto t = ResolveViewTransformForKey(*tree, control.id);
        transform = D2D1::Matrix3x2F(t.m11, t.m12, t.m21, t.m22, t.dx, t.dy);
    }
    else
        node = nullptr;

    auto inverse = transform;
    if (!inverse.Invert() || content.width <= 0.0f || content.height <= 0.0f)
        return std::nullopt;
    const auto local = inverse.TransformPoint(D2D1::Point2F(
        static_cast<float>(point.x), static_cast<float>(point.y)));
    if (local.x < frame.x || local.x >= frame.x + frame.width ||
        local.y < frame.y || local.y >= frame.y + frame.height)
        return std::nullopt;

    const float fontSize = node ? node->fontSize : control.fontSize;
    const float innerWidth = std::max(1.0f,
        content.width - (control.multiline ? 8.0f : 0.0f));
    auto layout = control.multiline
        ? CreateHostMultilineTextLayout(d2dState_, text, fontSize, innerWidth)
        : CreateHostSingleLineTextLayout(d2dState_, text, fontSize, innerWidth,
            content.height);
    if (!layout) return std::nullopt;
    if (node)
    {
        ApplyViewTextLocaleAndDirection(layout.Get(), *node, text);
        SetViewTextLayoutAlignment(layout.Get(), node->textAlign);
    }
    const float scaleY = std::hypot(transform._21, transform._22);
    const float scrollOffset = control.multiline && scaleY > 0.0f
        ? static_cast<float>(RuntimeGetScrollOffset(
            widget.widgetId, control.id, control.surface)) / scaleY : 0.0f;
    const float originY = content.y - scrollOffset;
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    if (FAILED(layout->HitTestPoint(local.x - content.x, local.y - originY,
            &trailing, &inside, &hit)))
        return std::nullopt;

    const size_t rawPosition = static_cast<size_t>(hit.textPosition) +
        (trailing ? static_cast<size_t>(hit.length) : 0);
    if (hitCharacter && inside && hit.isText && hit.textPosition < text.size())
        *hitCharacter = hit.textPosition;
    const size_t position = text_input::SnapBoundary(text, rawPosition);
    if (!caret) return position;

    // Keep DirectWrite's edge at a bidi boundary. For an empty field or a
    // snapped grapheme, hit the actual insertion position instead.
    const bool useHitEdge = position == rawPosition && !text.empty();
    float x = 0.0f;
    float y = 0.0f;
    if (FAILED(layout->HitTestTextPosition(
            useHitEdge ? hit.textPosition : static_cast<UINT32>(position),
            useHitEdge && trailing, &x, &y, &hit)))
        return std::nullopt;
    x = std::clamp(content.x + x, content.x, content.x + content.width);
    const float top = std::max(content.y, originY + y);
    const float bottom = std::min(content.y + content.height,
        originY + y + std::max(hit.height, fontSize));
    const auto first = transform.TransformPoint(D2D1::Point2F(x, top));
    const auto last = transform.TransformPoint(D2D1::Point2F(x, std::max(top, bottom)));
    RECT clip = control.rect;
    if (control.clipRect)
        IntersectRect(&clip, &clip, &*control.clipRect);
    *caret = {first.x, first.y, last.x, last.y, clip};
    return position;
}

std::optional<bool> WidgetEngine::UpdateHostInputTextDropPreview(
    const std::wstring& widgetId, POINT point, std::string_view surface,
    bool copyAllowed, const text_input::DragSelection* move)
{
    const int index = FindWidget(widgetId);
    const auto* control = index >= 0
        ? HitTestHostInputTextDrop(widgets_[index], point, surface) : nullptr;
    if (!control || !copyAllowed || !HostInputAcceptsTextDrop(*control))
    {
        ClearHostInputTextDropPreview();
        return control ? std::optional<bool>(false) : std::nullopt;
    }

    const auto& widget = widgets_[index];
    HostInputDropCaret caret{};
    const auto text = HostInputTextForDrop(widget, *control);
    const auto position = ResolveHostInputTextDropPosition(
        widget, *control, point, text, &caret);
    if (!position || (move && !move->CanMove(text, *position)))
    {
        ClearHostInputTextDropPreview();
        return false;
    }
    if (hostInputDropWidgetId_ != widgetId || hostInputDropSurface_ != surface)
    {
        ClearHostInputTextDropPreview();
        hostInputDropWidgetId_ = widgetId;
        hostInputDropSurface_ = surface;
    }
    if (hostInputDropCaret_ != caret)
    {
        hostInputDropCaret_ = caret;
        RuntimeInvalidateHost(widgetId, std::nullopt, surface);
    }
    return true;
}

bool WidgetEngine::ClearHostInputTextDropPreview()
{
    if (!hostInputDropCaret_) return false;
    hostInputDropCaret_.reset();
    const auto widgetId = std::exchange(hostInputDropWidgetId_, {});
    const auto surface = std::exchange(hostInputDropSurface_, {});
    RuntimeInvalidateHost(widgetId, std::nullopt, surface);
    return true;
}

bool WidgetEngine::DropHostInputText(const std::wstring& widgetId,
    std::string_view target, POINT point, std::string_view surface,
    std::wstring_view text, const text_input::DragSelection* move)
{
    using snowdesktop::widget_runtime::TryApplyHostTextReplacement;
    int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto* control = HitTestHostInputTextDrop(widgets_[index], point, surface);
    if (!control || control->id != target || !HostInputAcceptsTextDrop(*control))
        return false;

    const std::string id = control->id;
    const auto normalized = move ? std::wstring{} : text_input::NormalizeTransfer(
        text, control->multiline, control->maximumUtf8Bytes);
    if (!move && normalized.empty()) return false;
    const auto nextText = HostInputTextForDrop(widgets_[index], *control);
    const auto position = ResolveHostInputTextDropPosition(
        widgets_[index], *control, point, nextText, nullptr);
    if (!position || (move ? !move->CanMove(nextText, *position) :
            !snowdesktop::widget_runtime::HostTextReplacementFits(
                nextText, *position, *position, normalized, control->maximumUtf8Bytes)))
        return false;

    const std::string surfaceName(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, surfaceName.c_str());
    if (!RuntimeFocusHostInput(widgetId, id, "pointer") ||
        !focusedHostInput_.active || focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id || focusedHostInput_.surface != surface)
        return false;

    // Focus/blur actions can change the control or the value. Recheck both
    // before using the insertion offset from the visible text.
    index = FindWidget(widgetId);
    if (index < 0) return false;
    control = HitTestHostInputTextDrop(widgets_[index], point, surface);
    if (!control || control->id != id || !HostInputAcceptsTextDrop(*control) ||
        focusedHostInput_.readOnly)
        return false;
    const auto updatedPosition = ResolveHostInputTextDropPosition(
        widgets_[index], *control, point, focusedHostInput_.text, nullptr);
    if (!updatedPosition) return false;
    const text_input::Snapshot before{
        focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor};
    size_t nextCursor = 0;
    if (move)
    {
        const auto moved = move->Move(focusedHostInput_.text, *updatedPosition);
        if (!moved) return false;
        nextCursor = *moved;
    }
    else if (!TryApplyHostTextReplacement(focusedHostInput_.text, *updatedPosition,
            *updatedPosition, normalized, focusedHostInput_.maximumUtf8Bytes, nextCursor))
        return false;
    focusedHostInput_.cursor = focusedHostInput_.selectionAnchor = nextCursor;
    focusedHostInput_.pointerSelecting = false;
    focusedHostInput_.selectionDragArmed = false;
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.duplicateImeResult.clear();
    FinishHostInputEdit(before, "pointer");
    return true;
}

std::optional<text_input::DragSelection> WidgetEngine::TakeHostInputTextDrag(
    std::wstring& widgetId, std::string& controlId, std::string& surface)
{
    if (!focusedHostInput_.active || !focusedHostInput_.pointerSelecting ||
        !std::exchange(focusedHostInput_.selectionDragArmed, false) ||
        focusedHostInput_.composing)
        return {};
    const auto start = std::min(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
    const auto end = std::max(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
    if (start == end || end > focusedHostInput_.text.size())
        return {};
    widgetId = focusedHostInput_.widgetId;
    controlId = focusedHostInput_.id;
    surface = focusedHostInput_.surface;
    focusedHostInput_.pointerSelecting = false;
    return text_input::DragSelection{focusedHostInput_.text, start, end};
}

bool WidgetEngine::PrepareHostInputContextMenu(
    const std::wstring& widgetId, int x, int y,
    std::string_view surface, bool clipboardHasText,
    snowdesktop::widget_runtime::HostInputContextMenuState& state)
{
    state = {};
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;

    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    auto& controls = widgets_[index].hostControls;
    const POINT point{ x, y };
    auto target = std::find_if(
        controls.rbegin(), controls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.enabled &&
                HostControlBelongsToSurface(control, normalizedSurface) &&
                HostControlContainsPoint(control, point);
        });
    if (target == controls.rend())
        return false;

    const std::string targetId = target->id;
    const bool alreadyFocused = focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        focusedHostInput_.id == targetId &&
        focusedHostInput_.surface == normalizedSurface;
    WidgetSurfaceScope surfaceScope(
        d2dState_, normalizedSurface.c_str());
    if (!RuntimeFocusHostInput(widgetId, targetId, "pointer"))
        return false;

    target = std::find_if(
        controls.rbegin(), controls.rend(), [&](const auto& control) {
            return control.type == LuaWidget::HostControl::Type::Input &&
                control.id == targetId &&
                HostControlBelongsToSurface(control, normalizedSurface);
        });
    if (target == controls.rend())
        return false;

    focusedHostInput_.pointerSelecting = false;
    if (!alreadyFocused)
    {
        const size_t previousCursor = focusedHostInput_.cursor;
        const size_t previousAnchor =
            focusedHostInput_.selectionAnchor;
        focusedHostInput_.cursor = HitTestHostInputPosition(
            *target, widgetId, x, y);
        focusedHostInput_.selectionAnchor =
            focusedHostInput_.cursor;
        focusedHostInput_.caretVisibility.Request();
        if (focusedHostInput_.controlledSelection &&
            (previousCursor != focusedHostInput_.cursor ||
                previousAnchor != focusedHostInput_.selectionAnchor))
        {
            DispatchHostInputSelectionChange(
                focusedHostInput_.widgetId,
                focusedHostInput_.id,
                focusedHostInput_.selectionChangeAction,
                focusedHostInput_.text,
                previousAnchor, previousCursor,
                focusedHostInput_.selectionAnchor,
                focusedHostInput_.cursor,
                "contextMenu");
        }
    }

    state = snowdesktop::widget_runtime::
        ResolveHostInputContextMenuState(
            focusedHostInput_.text.size(),
            focusedHostInput_.cursor,
            focusedHostInput_.selectionAnchor,
            target->enabled,
            focusedHostInput_.readOnly,
            clipboardHasText);
    RuntimeInvalidateHost(widgetId);
    return true;
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
                item.id == focusedHostInput_.id &&
                HostControlBelongsToSurface(
                    item, focusedHostInput_.surface);
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
            width - control->padding.left - control->padding.right -
                scrollbarReserve);
        layout = CreateHostMultilineTextLayout(d2dState_,
            display.text, control->fontSize,
            innerWidth);
        scrollOffset = RuntimeGetScrollOffset(
            focusedHostInput_.widgetId, control->id,
            focusedHostInput_.surface);
    }
    else
    {
        const float innerWidth = std::max(1.0f,
            width - control->padding.left - control->padding.right);
        layout = CreateHostSingleLineTextLayout(d2dState_,
            display.text, control->fontSize,
            innerWidth, std::max(1.0f,
                height - control->padding.top - control->padding.bottom));
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
        control->padding.left + caretX;
    const float localY = static_cast<float>(control->rect.top) +
        control->padding.top +
        caretY - static_cast<float>(scrollOffset);
    const float caretHeight = std::max(
        metrics.height, control->fontSize);
    const RECT surfaceBounds = BoundsForSurface(
        widget, focusedHostInput_.surface);
    rect.left = surfaceBounds.left +
        static_cast<LONG>(std::lround(localX));
    rect.top = surfaceBounds.top +
        static_cast<LONG>(std::lround(localY));
    rect.right = rect.left + 1;
    rect.bottom = rect.top +
        std::max<LONG>(1,
            static_cast<LONG>(std::lround(caretHeight)));
    return true;
}

void WidgetEngine::BlurHostInput(bool cancel, bool preserveViewFocus)
{
    if (!focusedHostInput_.active) return;

    const std::wstring widgetId = focusedHostInput_.widgetId;
    const std::string inputId = focusedHostInput_.id;
    const std::string inputSurface = focusedHostInput_.surface;
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
    if (const int index = FindWidget(widgetId); index >= 0 &&
        ViewFocusForSurface(widgets_[index], inputSurface) == inputId)
    {
        if (!preserveViewFocus) ViewFocusForSurface(widgets_[index], inputSurface).clear();
        ViewFocusCueForSurface(widgets_[index], inputSurface) = preserveViewFocus;
    }
    focusedHostInput_ = {};
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::SetHostInputComposition(
    const std::wstring& text, size_t cursor)
{
    if (!focusedHostInput_.active)
        return false;
    if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        return true;
    }
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.composing = true;
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
    focusedHostInput_.caretVisibility.Request();
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
    return true;
}

bool WidgetEngine::CommitHostInputComposition(
    const std::wstring& text)
{
    if (!focusedHostInput_.active)
        return false;
    if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly))
    {
        focusedHostInput_.compositionText.clear();
        focusedHostInput_.compositionCursor = 0;
        return true;
    }
    focusedHostInput_.pendingHighSurrogate = 0;

    const snowdesktop::text_input::Snapshot before{
        focusedHostInput_.text, focusedHostInput_.cursor,
        focusedHostInput_.selectionAnchor};

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
        focusedHostInput_.caretVisibility.Request();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    focusedHostInput_.cursor = nextCursor;
    focusedHostInput_.selectionAnchor =
        focusedHostInput_.cursor;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    focusedHostInput_.caretVisibility.Request();
    focusedHostInput_.history.Record(before, {focusedHostInput_.text,
        focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
    focusedHostInput_.duplicateImeResult = text;
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
    if (!focusedHostInput_.active) return;
    focusedHostInput_.composing = false;
    focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor = 0;
    if(focusedHostInput_.deferredModelText)
    {
        focusedHostInput_.text=std::move(*focusedHostInput_.deferredModelText);focusedHostInput_.deferredModelText.reset();
        focusedHostInput_.originalText=focusedHostInput_.text;
        focusedHostInput_.cursor=text_input::SnapBoundary(focusedHostInput_.text,focusedHostInput_.cursor);
        focusedHostInput_.selectionAnchor=text_input::SnapBoundary(focusedHostInput_.text,focusedHostInput_.selectionAnchor);
        focusedHostInput_.history.Clear();
    }
    if(focusedHostInput_.deferredSelection&&focusedHostInput_.controlledSelection)
    {
        const auto anchor=snowdesktop::widget_runtime::HostTextOffsetFromUtf8ByteOffset(focusedHostInput_.text,focusedHostInput_.controlledSelection->start);
        const auto cursor=snowdesktop::widget_runtime::HostTextOffsetFromUtf8ByteOffset(focusedHostInput_.text,focusedHostInput_.controlledSelection->finish);
        if(anchor&&cursor){focusedHostInput_.selectionAnchor=*anchor;focusedHostInput_.cursor=*cursor;}
    }
    focusedHostInput_.deferredSelection=false;
    focusedHostInput_.caretVisibility.Request();
    RuntimeInvalidateHost(focusedHostInput_.widgetId);
}
void WidgetEngine::BeginHostInputComposition()
{
    if(!focusedHostInput_.active)return;
    focusedHostInput_.composing=true;focusedHostInput_.compositionText.clear();
    focusedHostInput_.compositionCursor=0;focusedHostInput_.duplicateImeResult.clear();
}

bool WidgetEngine::HandleHostInputChar(wchar_t ch)
{
    if (!focusedHostInput_.active || ch < 0x20 || ch == 0x7F)
        return false;
    if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly)) return true;
    if(!focusedHostInput_.duplicateImeResult.empty()&&focusedHostInput_.duplicateImeResult.front()==ch)
    {focusedHostInput_.duplicateImeResult.erase(0,1);return true;}
    focusedHostInput_.duplicateImeResult.clear();
    const snowdesktop::text_input::Snapshot before{
        focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor};
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
    focusedHostInput_.caretVisibility.Request();
    focusedHostInput_.history.Record(before, {focusedHostInput_.text,
        focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
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

bool WidgetEngine::ExecuteHostInputEditCommand(
    snowdesktop::widget_runtime::HostInputEditCommand command,
    const char* source)
{
    using snowdesktop::widget_runtime::HostInputEditCommand;
    if (!focusedHostInput_.active)
        return false;

    const char* eventSource = source && *source
        ? source : "contextMenu";
    focusedHostInput_.pendingHighSurrogate = 0;
    focusedHostInput_.pointerSelecting = false;
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());

    const std::wstring previousText = focusedHostInput_.text;
    const size_t previousCursor = focusedHostInput_.cursor;
    const size_t previousSelectionAnchor =
        focusedHostInput_.selectionAnchor;
    const auto hasSelection = [&]() {
        return focusedHostInput_.selectionAnchor !=
            focusedHostInput_.cursor;
    };
    const auto selectionStart = [&]() {
        return std::min(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    const auto selectionEnd = [&]() {
        return std::max(focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor);
    };
    const auto copySelection = [&]() {
        if (!hasSelection() || !OpenClipboard(nullptr))
            return false;
        bool copied = false;
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
        return copied;
    };
    const auto dispatchSelectionChange = [&]() {
        if (!focusedHostInput_.controlledSelection)
            return;
        DispatchHostInputSelectionChange(
            focusedHostInput_.widgetId,
            focusedHostInput_.id,
            focusedHostInput_.selectionChangeAction,
            focusedHostInput_.text,
            previousSelectionAnchor, previousCursor,
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor, eventSource);
    };

    bool changed = false;
    switch (command)
    {
    case HostInputEditCommand::SelectAll:
        focusedHostInput_.selectionAnchor = 0;
        focusedHostInput_.cursor = focusedHostInput_.text.size();
        focusedHostInput_.caretVisibility.Request();
        dispatchSelectionChange();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;

    case HostInputEditCommand::Copy:
        (void)copySelection();
        return true;

    case HostInputEditCommand::Cut:
        if (snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly) &&
            copySelection())
        {
            const size_t start = selectionStart();
            const size_t end = selectionEnd();
            focusedHostInput_.text.erase(start, end - start);
            focusedHostInput_.cursor = start;
            focusedHostInput_.selectionAnchor = start;
            changed = true;
        }
        break;

    case HostInputEditCommand::Paste:
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly))
            return true;
        std::wstring pasted;
        if (OpenClipboard(nullptr))
        {
            if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
            {
                if (const wchar_t* clipboardText =
                        static_cast<const wchar_t*>(GlobalLock(data)))
                {
                    const auto* end = clipboardText + GlobalSize(data) / sizeof(wchar_t);
                    const auto* terminator = std::find(clipboardText, end, L'\0');
                    if (terminator != end)
                        pasted = text_input::NormalizeTransfer(
                            std::wstring_view(clipboardText,
                                static_cast<size_t>(terminator - clipboardText)),
                            focusedHostInput_.multiline,
                            focusedHostInput_.maximumUtf8Bytes);
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!pasted.empty())
        {
            const size_t start = selectionStart();
            const size_t end = selectionEnd();
            size_t nextCursor = focusedHostInput_.cursor;
            if (snowdesktop::widget_runtime::
                    TryApplyHostTextReplacement(
                        focusedHostInput_.text, start, end,
                        pasted,
                        focusedHostInput_.maximumUtf8Bytes,
                        nextCursor))
            {
                focusedHostInput_.cursor = nextCursor;
                focusedHostInput_.selectionAnchor = nextCursor;
                changed = true;
            }
        }
        break;
    }
    }

    if (changed)
        FinishHostInputEdit({previousText, previousCursor, previousSelectionAnchor}, eventSource);
    return true;
}

void WidgetEngine::FinishHostInputEdit(const text_input::Snapshot& before, const char* source)
{
    const auto widgetId = focusedHostInput_.widgetId;
    focusedHostInput_.history.Record(before,
        {focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
    focusedHostInput_.caretVisibility.Request();
    if (focusedHostInput_.liveUpdate)
    {
        if (focusedHostInput_.controlled)
        {
            DispatchHostInputChange(widgetId, focusedHostInput_.id,
                focusedHostInput_.changeAction, before.text, focusedHostInput_.text,
                focusedHostInput_.numeric, focusedHostInput_.minimum,
                focusedHostInput_.maximum, false, false, source);
        }
        else
        {
            RuntimeSetStorageValue(widgetId, focusedHostInput_.storageKey,
                WidgetWideToUtf8(focusedHostInput_.text));
        }
    }
    RuntimeInvalidateHost(widgetId);
}

bool WidgetEngine::HandleHostInputKey(WPARAM key)
{
    if (!focusedHostInput_.active) return false;
    focusedHostInput_.duplicateImeResult.clear();
    if (focusedHostInput_.composing &&
        (key == VK_RETURN || key == VK_ESCAPE)) return true;
    focusedHostInput_.pendingHighSurrogate = 0;

    const bool ctrl = (snowdesktop::private_pointer::KeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (snowdesktop::private_pointer::KeyState(VK_MENU) & 0x8000) != 0;
    if (key == VK_TAB && !ctrl && !alt)
    {
        const std::wstring widgetId = focusedHostInput_.widgetId;
        (void)HandleHostViewKey(widgetId, key, false, shift, false,
            focusedHostInput_.surface);
        return true;
    }
    if (ctrl && key == 'A')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::
                HostInputEditCommand::SelectAll,
            "keyboard");
    if (ctrl && key == 'X')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::HostInputEditCommand::Cut,
            "keyboard");
    if (ctrl && key == 'C')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::HostInputEditCommand::Copy,
            "keyboard");
    if (ctrl && key == 'V')
        return ExecuteHostInputEditCommand(
            snowdesktop::widget_runtime::HostInputEditCommand::Paste,
            "keyboard");
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.selectionAnchor = std::min(
        focusedHostInput_.selectionAnchor,
        focusedHostInput_.text.size());
    focusedHostInput_.pointerSelecting = false;
    const std::wstring previousText = focusedHostInput_.text;
    const size_t previousCursor = focusedHostInput_.cursor;
    const size_t previousSelectionAnchor =
        focusedHostInput_.selectionAnchor;
    const auto dispatchSelectionChange = [&]() {
        if (!focusedHostInput_.controlledSelection) return;
        DispatchHostInputSelectionChange(
            focusedHostInput_.widgetId, focusedHostInput_.id,
            focusedHostInput_.selectionChangeAction,
            focusedHostInput_.text,
            previousSelectionAnchor, previousCursor,
            focusedHostInput_.selectionAnchor,
            focusedHostInput_.cursor, "keyboard");
    };
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
        focusedHostInput_.caretVisibility.Request();
        dispatchSelectionChange();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    };

    bool changed = false;
    const bool historyNavigation = ctrl && (key == 'Z' || key == 'Y');
    if (historyNavigation)
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        changed = key == 'Y' || shift
            ? focusedHostInput_.history.Redo(focusedHostInput_.text,
                focusedHostInput_.cursor, focusedHostInput_.selectionAnchor)
            : focusedHostInput_.history.Undo(focusedHostInput_.text,
                focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
    }
    else if (key == VK_ESCAPE)
    {
        BlurHostInput(true, true);
        return true;
    }
    else if (key == VK_RETURN)
    {
        if (!focusedHostInput_.multiline || ctrl)
        {
            if (!focusedHostInput_.controlled &&
                !focusedHostInput_.submitAction.id.empty())
            {
                // Commit the storage-bound editor before the callback consumes
                // or clears its value. A later blur must not restore the draft.
                const auto widgetId = focusedHostInput_.widgetId;
                const auto inputId = focusedHostInput_.id;
                const auto inputSurface = focusedHostInput_.surface;
                const auto submitAction = focusedHostInput_.submitAction;
                const auto text = focusedHostInput_.text;
                BlurHostInput(false, true);
                WidgetSurfaceScope surfaceScope(d2dState_, inputSurface.c_str());
                snowdesktop::widget_runtime::WidgetTrustedGestureScope gestureScope(
                    trustedGestureState_, true);
                snowdesktop::widget_runtime::HostInputSubmitFocusScope focusScope(
                    hostInputSubmitFocus_, widgetId, inputId, inputSurface);
                DispatchHostInputAction(widgetId, inputId,
                    submitAction, "submit", text, false, "keyboard");
                return true;
            }
            DispatchHostInputAction(focusedHostInput_.widgetId,
                focusedHostInput_.id, focusedHostInput_.submitAction,
                "submit", focusedHostInput_.text, false, "keyboard");
            BlurHostInput(false, true);
            return true;
        }
        const size_t start = selectionStart();
        const size_t end = selectionEnd();
        size_t nextCursor = focusedHostInput_.cursor;
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        if (!snowdesktop::widget_runtime::TryApplyHostTextReplacement(
                focusedHostInput_.text, start, end, L"\n",
                focusedHostInput_.maximumUtf8Bytes, nextCursor))
        {
            RuntimeInvalidateHost(focusedHostInput_.widgetId);
            return true;
        }
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        focusedHostInput_.cursor = nextCursor;
        focusedHostInput_.selectionAnchor =
            focusedHostInput_.cursor;
        changed = true;
    }
    else if (key == VK_BACK)
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        changed = eraseSelection();
        if (!changed && focusedHostInput_.cursor > 0)
        {
            const auto start = ctrl ? snowdesktop::text_input::WordBoundary(
                focusedHostInput_.text, focusedHostInput_.cursor, false)
                : snowdesktop::text_input::PreviousBoundary(focusedHostInput_.text, focusedHostInput_.cursor);
            focusedHostInput_.text.erase(start, focusedHostInput_.cursor - start);
            focusedHostInput_.cursor = start;
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (key == VK_DELETE)
    {
        if (!snowdesktop::widget_runtime::HostInputAllowsMutation(
                true, focusedHostInput_.readOnly)) return true;
        changed = eraseSelection();
        if (!changed &&
            focusedHostInput_.cursor < focusedHostInput_.text.size())
        {
            const auto end = ctrl ? snowdesktop::text_input::WordBoundary(
                focusedHostInput_.text, focusedHostInput_.cursor, true)
                : snowdesktop::text_input::NextBoundary(focusedHostInput_.text, focusedHostInput_.cursor);
            focusedHostInput_.text.erase(focusedHostInput_.cursor, end - focusedHostInput_.cursor);
            focusedHostInput_.selectionAnchor =
                focusedHostInput_.cursor;
            changed = true;
        }
    }
    else if (snowdesktop::widget_runtime::HostInputAllowsMutation(
            true, focusedHostInput_.readOnly) && focusedHostInput_.numeric &&
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
        const int widgetIndex = FindWidget(
            focusedHostInput_.widgetId);
        if (widgetIndex >= 0 && d2dState_)
        {
            const auto& controls =
                widgets_[widgetIndex].hostControls;
            const auto control = std::find_if(
                controls.rbegin(), controls.rend(),
                [&](const auto& item) {
                    return item.type ==
                            LuaWidget::HostControl::Type::Input &&
                        item.multiline &&
                        item.id == focusedHostInput_.id &&
                        HostControlBelongsToSurface(
                            item, focusedHostInput_.surface);
                });
            if (control != controls.rend())
            {
                constexpr float scrollbarReserve = 8.0f;
                const float width = static_cast<float>(
                    control->rect.right - control->rect.left);
                const float innerWidth = std::max(1.0f,
                    width - control->padding.left -
                        control->padding.right - scrollbarReserve);
                const auto layout = CreateHostMultilineTextLayout(
                    d2dState_, focusedHostInput_.text,
                    control->fontSize, innerWidth);
                const auto visualCursor = snowdesktop::widget_runtime::
                    ResolveHostInputVerticalCaretPosition(
                        layout.Get(), focusedHostInput_.text,
                        focusedHostInput_.cursor,
                        key == VK_UP
                            ? snowdesktop::widget_runtime::
                                HostInputVerticalDirection::Up
                            : snowdesktop::widget_runtime::
                                HostInputVerticalDirection::Down);
                if (visualCursor)
                    return finishMovement(*visualCursor);
            }
        }

        // Keep explicit-newline navigation available if DirectWrite cannot
        // provide a layout, for example while the graphics device is reset.
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
                ? 0 : (ctrl ? snowdesktop::text_input::WordBoundary(focusedHostInput_.text, focusedHostInput_.cursor, false)
                    : snowdesktop::text_input::PreviousBoundary(focusedHostInput_.text, focusedHostInput_.cursor));
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
                : (ctrl ? snowdesktop::text_input::WordBoundary(focusedHostInput_.text, focusedHostInput_.cursor, true)
                    : snowdesktop::text_input::NextBoundary(focusedHostInput_.text, focusedHostInput_.cursor));
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
        if (!historyNavigation)
            focusedHostInput_.history.Record({previousText, previousCursor, previousSelectionAnchor},
                {focusedHostInput_.text, focusedHostInput_.cursor, focusedHostInput_.selectionAnchor});
        focusedHostInput_.caretVisibility.Request();
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

int WidgetEngine::RuntimeGetScrollOffset(const std::wstring& widgetId,
    const std::string& id, std::string_view surface) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return 0;
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& offsets = ScrollOffsetsForSurface(
        widgets_[index], surface);
    const auto it = offsets.find(id);
    return it == offsets.end() ? 0 : it->second;
}

bool WidgetEngine::RuntimeHasScrollOffset(const std::wstring& widgetId,
    const std::string& id, std::string_view surface) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& offsets = ScrollOffsetsForSurface(widgets_[index], surface);
    return offsets.contains(id);
}

bool WidgetEngine::RuntimeComputeVariableVirtualRange(
    const std::wstring& widgetId, const std::string& id,
    std::size_t itemCount, float estimatedItemSize, float mainGap,
    float viewportExtent, bool horizontal, std::uint64_t layoutRevision,
    std::size_t overscan, std::size_t initialScrollIndex,
    snowdesktop::widget_runtime::ViewVirtualRange& range,
    std::string& error, std::string_view surface) const
{
    using snowdesktop::widget_runtime::
        ComputeViewVariableVirtualItemScrollOffset;
    using snowdesktop::widget_runtime::ComputeViewVariableVirtualRange;
    error.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "host context is unavailable";
        return false;
    }
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& widget = widgets_[index];
    std::vector<snowdesktop::widget_runtime::ViewVirtualItemMeasurement>
        measurements;
    const auto& states = VariableVirtualStatesForSurface(widget, surface);
    const auto state = states.find(id);
    if (state != states.end() && VariableVirtualStateMatches(
            state->second, itemCount, estimatedItemSize, mainGap,
            horizontal, layoutRevision))
        measurements = VariableVirtualMeasurements(state->second);

    const auto& offsets = ScrollOffsetsForSurface(widget, surface);
    const auto stored = offsets.find(id);
    float requestedOffset = stored == offsets.end()
        ? 0.0f : static_cast<float>(stored->second);
    if (stored == offsets.end() && initialScrollIndex > 0)
    {
        if (!ComputeViewVariableVirtualItemScrollOffset(itemCount,
                estimatedItemSize, mainGap, viewportExtent, 0.0f,
                initialScrollIndex, "nearest", measurements,
                requestedOffset, error))
            return false;
    }
    return ComputeViewVariableVirtualRange(itemCount,
        estimatedItemSize, mainGap, viewportExtent, requestedOffset,
        overscan, measurements, range, error);
}

void WidgetEngine::RuntimeSetScrollOffset(
    const std::wstring& widgetId, const std::string& id,
    int offset, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0 || id.empty()) return;
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    auto& widget = widgets_[index];
    int maximum = 0;
    for (auto it = widget.hostControls.rbegin();
        it != widget.hostControls.rend(); ++it)
    {
        if (it->id != id ||
            !HostControlBelongsToSurface(*it, surface)) continue;
        if (it->type != LuaWidget::HostControl::Type::Scroll &&
            !(it->type == LuaWidget::HostControl::Type::Input &&
                it->multiline))
            continue;
        maximum = it->horizontal
            ? std::max(0, it->contentWidth - it->viewportWidth)
            : std::max(0, it->contentHeight - it->viewportHeight);
        break;
    }
    ScrollOffsetsForSurface(widget, surface)[id] =
        std::clamp(offset, 0, maximum);
}

bool WidgetEngine::RuntimeScrollView(const std::wstring& widgetId,
    const std::string& id, int value, bool relative,
    int& offset, int& maximum, bool& changed, std::string& error,
    std::string_view surface)
{
    offset = 0;
    maximum = 0;
    changed = false;
    error.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    auto& widget = widgets_[index];
    const auto control = std::find_if(widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& candidate) {
            return candidate.type == LuaWidget::HostControl::Type::Scroll &&
                candidate.id == id &&
                HostControlBelongsToSurface(candidate, surface);
        });
    if (control == widget.hostControls.rend())
    {
        error = "scrollTargetNotFound";
        return false;
    }
    maximum = control->horizontal
        ? std::max(0, control->contentWidth - control->viewportWidth)
        : std::max(0, control->contentHeight - control->viewportHeight);
    auto& offsets = ScrollOffsetsForSurface(widget, surface);
    const int previous = std::clamp(offsets[id], 0, maximum);
    const std::int64_t requested = relative
        ? static_cast<std::int64_t>(previous) + value : value;
    offset = static_cast<int>(std::clamp<std::int64_t>(
        requested, 0, maximum));
    changed = offset != previous;
    offsets[id] = offset;
    if (changed) RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeScrollViewToIndex(
    const std::wstring& widgetId, const std::string& id,
    std::size_t itemIndex, std::string_view alignment,
    int& offset, int& maximum, bool& changed, std::string& error,
    std::string_view surface)
{
    const int widgetIndex = FindWidget(widgetId);
    if (widgetIndex < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    if (surface.empty()) surface = CurrentWidgetSurface(d2dState_);
    const auto& widget = widgets_[widgetIndex];
    const auto* tree = DeclarativeViewForSurface(widget, surface);
    const auto* node = tree ? FindViewNodeByKey(*tree, id) : nullptr;
    if (!node || (node->type != snowdesktop::widget_runtime::
            ViewNodeType::VirtualList &&
            node->type != snowdesktop::widget_runtime::
                ViewNodeType::VirtualGrid))
    {
        error = "virtualScrollTargetNotFound";
        return false;
    }
    const auto control = std::find_if(widget.hostControls.rbegin(),
        widget.hostControls.rend(), [&](const auto& candidate) {
            return candidate.type == LuaWidget::HostControl::Type::Scroll &&
                candidate.id == id &&
                HostControlBelongsToSurface(candidate, surface);
        });
    if (control == widget.hostControls.rend())
    {
        error = "scrollTargetNotFound";
        return false;
    }
    const int current = RuntimeGetScrollOffset(widgetId, id, surface);
    float requested = 0.0f;
    std::string rangeError;
    const bool horizontal = node->orientation ==
        snowdesktop::widget_runtime::ViewOrientation::Horizontal;
    const float viewportExtent = static_cast<float>(horizontal
        ? control->viewportWidth : control->viewportHeight);
    const float mainGap = horizontal
        ? node->columnGap.value_or(node->gap)
        : node->rowGap.value_or(node->gap);
    const bool positioned = node->estimatedItemSize
        ? snowdesktop::widget_runtime::
            ComputeViewVariableVirtualItemScrollOffset(
                node->itemCount, *node->estimatedItemSize,
                mainGap, viewportExtent,
                static_cast<float>(current), itemIndex, alignment,
                node->virtualMeasurements, requested, rangeError)
        : snowdesktop::widget_runtime::ComputeViewVirtualItemScrollOffset(
            node->itemCount, node->itemExtent,
            node->type == snowdesktop::widget_runtime::
                    ViewNodeType::VirtualGrid ? node->columns : 1,
            mainGap, viewportExtent,
            static_cast<float>(current), itemIndex, alignment,
            requested, rangeError);
    if (!positioned)
    {
        error = itemIndex == 0 || itemIndex > node->itemCount
            ? "indexOutOfRange" : "invalidVirtualCollection";
        return false;
    }
    return RuntimeScrollView(widgetId, id,
        static_cast<int>(std::lround(requested)), false,
        offset, maximum, changed, error, surface);
}

std::vector<LuaWidget::HostControl> WidgetEngine::GetScrollControls(
    const std::wstring& widgetId, std::string_view surface) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return {};
    std::vector<LuaWidget::HostControl> results;
    for (const auto& ctrl : widgets_[index].hostControls)
    {
        if (HostControlBelongsToSurface(ctrl, surface) &&
            (ctrl.type == LuaWidget::HostControl::Type::Scroll ||
            (ctrl.type == LuaWidget::HostControl::Type::Input &&
                ctrl.multiline)))
            results.push_back(ctrl);
    }
    return results;
}

bool WidgetEngine::IsHostScrollbarDragging(
    const std::wstring& widgetId, std::string_view surface) const
{
    return hostScrollbarDrag_.active &&
        hostScrollbarDrag_.widgetId == widgetId &&
        hostScrollbarDrag_.surface == NormalizeWidgetSurface(surface);
}

bool WidgetEngine::OwnsDesktopWheel(const std::wstring& widgetId, int x, int y, int delta) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const auto& widget = widgets_[index];
    const POINT point{x, y};
    const auto canScroll = [&](const LuaWidget::HostControl& control) {
        const int maximum = control.horizontal && !control.selectPopup
            ? std::max(0, control.contentWidth - control.viewportWidth)
            : std::max(0, control.contentHeight - control.viewportHeight);
        return delta == 0 || snowdesktop::widget_scroll_rules::ApplyWheelDelta(
            RuntimeGetScrollOffset(widgetId, control.id, "desktop"), maximum, delta).moved;
    };
    // Match HandleHostUiPointer's popup-first routing, even if an enclosing
    // scroll container is still able to move behind the expanded list.
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!it->enabled || !HostControlBelongsToSurface(*it, "desktop") || !HostControlContainsPoint(*it, point)) continue;
        if (it->selectPopup) return canScroll(*it);
    }
    std::string actionKey;
    const bool customAction = widget.interactionRegions.ActionAt(
        static_cast<float>(x), static_cast<float>(y), "wheel", &actionKey) != nullptr;
    bool actionBelongsToScrollControl = false;
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!it->enabled || !HostControlBelongsToSurface(*it, "desktop") || !HostControlContainsPoint(*it, point)) continue;
        if (it->type != LuaWidget::HostControl::Type::Scroll &&
            !(it->type == LuaWidget::HostControl::Type::Input && it->multiline)) continue;
        if (canScroll(*it)) return true;
        actionBelongsToScrollControl = actionBelongsToScrollControl || actionKey == it->id;
    }
    // A custom region has no host-owned range to inspect. Keep its explicit
    // event ownership; known controls can hand off at their directional limit.
    return customAction && !actionBelongsToScrollControl;
}

bool WidgetEngine::HandleHostUiPointer(const std::wstring& widgetId, int x, int y,
    int delta, bool wheel, std::string_view surface)
{
    int index = FindWidget(widgetId);
    if (index < 0) return false;
    auto& widget = widgets_[index];
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, normalizedSurface.c_str());
    POINT point{ x, y };
    for (auto it = widget.hostControls.rbegin();
        it != widget.hostControls.rend(); ++it)
    {
        if (!it->selectPopup || !it->enabled ||
            !HostControlBelongsToSurface(*it, normalizedSurface) ||
            !HostControlContainsPoint(*it, point)) continue;
        if (!wheel)
        {
            const float scale = CalculateWidgetCellScale(
                widget.layoutMetrics.gridCellWidth,
                widget.layoutMetrics.gridCellHeight);
            const int offset = RuntimeGetScrollOffset(
                widgetId, it->id, normalizedSurface);
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(it->rect.top,
                    it->rect.bottom, it->contentHeight,
                    it->viewportHeight, offset, scale);
            if (snowdesktop::widget_scroll_rules::ScrollbarThumbHit(
                    geometry, y, x, it->rect.right, scale))
            {
                hostScrollbarDrag_ = { true, widgetId, it->id,
                    normalizedSurface, false, y, offset };
                RuntimeInvalidateHost(widgetId);
                return true;
            }
            return false;
        }
        const int maximum = std::max(0,
            it->contentHeight - it->viewportHeight);
        int& offset = ScrollOffsetsForSurface(
            widget, normalizedSurface)[it->id];
        const auto result = snowdesktop::widget_scroll_rules::
            ApplyWheelDelta(offset, maximum, delta);
        if (result.moved)
        {
            offset = result.offset;
            RuntimeInvalidateHost(widgetId);
        }
        return true;
    }
    for (auto it = widget.hostControls.rbegin(); it != widget.hostControls.rend(); ++it)
    {
        if (!HostControlBelongsToSurface(*it, normalizedSurface) ||
            !it->enabled || !HostControlContainsPoint(*it, point)) continue;
        if (wheel &&
            (it->type == LuaWidget::HostControl::Type::Scroll ||
                (it->type ==
                    LuaWidget::HostControl::Type::Input &&
                    it->multiline)))
        {
            const int maximum = it->horizontal
                ? std::max(0, it->contentWidth - it->viewportWidth)
                : std::max(0, it->contentHeight - it->viewportHeight);
            int& offset = ScrollOffsetsForSurface(
                widget, normalizedSurface)[it->id];
            const auto result =
                snowdesktop::widget_scroll_rules::ApplyWheelDelta(
                    offset, maximum, delta);
            if (!result.moved)
                continue;
            offset = result.offset;
            if (focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == it->id &&
                focusedHostInput_.surface == normalizedSurface)
            {
                focusedHostInput_.caretVisibility.PreserveManualScroll();
            }
            const std::string targetKey = it->id;
            {
                snowdesktop::widget_runtime::WidgetTrustedGestureScope
                    gestureScope(trustedGestureState_, true);
                DispatchInteractionAction(widget, targetKey, "wheel",
                    x, y, 0, delta, 0, false, "pointer", 0,
                    std::nullopt, std::nullopt, normalizedSurface);
            }
            if (result.reachedEnd)
                DispatchInteractionAction(widget, targetKey, "scrollEnd",
                    x, y, 0, delta, 0, false, "pointer", 0,
                    std::nullopt, std::nullopt, normalizedSurface);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (wheel) continue;
        if (it->type == LuaWidget::HostControl::Type::Scroll ||
            (it->type == LuaWidget::HostControl::Type::Input &&
                it->multiline))
        {
            const int contentExtent = it->horizontal
                ? it->contentWidth : it->contentHeight;
            const int visibleExtent = it->horizontal
                ? it->viewportWidth : it->viewportHeight;
            const int viewportStart = it->horizontal
                ? it->rect.left : it->rect.top;
            const int viewportEnd = it->horizontal
                ? it->rect.right : it->rect.bottom;
            const int viewportCrossEnd = it->horizontal
                ? it->rect.bottom : it->rect.right;
            const float scale = CalculateWidgetCellScale(
                widget.layoutMetrics.gridCellWidth,
                widget.layoutMetrics.gridCellHeight);
            const int offset = RuntimeGetScrollOffset(
                widgetId, it->id, normalizedSurface);
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(
                    viewportStart, viewportEnd, contentExtent,
                    visibleExtent, offset, scale);
            const int primary = it->horizontal ? x : y;
            const int cross = it->horizontal ? y : x;
            if (snowdesktop::widget_scroll_rules::ScrollbarThumbHit(
                    geometry, primary, cross, viewportCrossEnd, scale))
            {
                hostScrollbarDrag_ = {
                    true, widgetId, it->id, normalizedSurface,
                    it->horizontal, primary, offset
                };
                if (focusedHostInput_.active &&
                    focusedHostInput_.widgetId == widgetId &&
                    focusedHostInput_.id == it->id &&
                    focusedHostInput_.surface == normalizedSurface)
                {
                    focusedHostInput_.caretVisibility.PreserveManualScroll();
                }
                RuntimeInvalidateHost(widgetId);
                return true;
            }
        }
        if (it->type == LuaWidget::HostControl::Type::Input)
        {
            const std::string inputId = it->id;
            const bool alreadyFocused = focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == inputId &&
                focusedHostInput_.surface == normalizedSurface;
            if (RuntimeFocusHostInput(widgetId, inputId) &&
                focusedHostInput_.active &&
                focusedHostInput_.widgetId == widgetId &&
                focusedHostInput_.id == inputId)
            {
                // A focus callback can repaint and replace the host-control list.
                const int currentIndex = FindWidget(widgetId);
                if (currentIndex < 0) return true;
                const auto& currentWidget = widgets_[currentIndex];
                const auto control = std::find_if(currentWidget.hostControls.rbegin(),
                    currentWidget.hostControls.rend(), [&](const auto& candidate) {
                        return candidate.type == LuaWidget::HostControl::Type::Input &&
                            candidate.id == inputId &&
                            HostControlBelongsToSurface(candidate, normalizedSurface);
                    });
                if (control == currentWidget.hostControls.rend()) return true;
                focusedHostInput_.pointerSelectionStartCursor =
                    focusedHostInput_.cursor;
                focusedHostInput_.pointerSelectionStartAnchor =
                    focusedHostInput_.selectionAnchor;
                const auto start = std::min(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
                const auto end = std::max(focusedHostInput_.cursor, focusedHostInput_.selectionAnchor);
                std::optional<size_t> hitCharacter;
                if (alreadyFocused && start != end && !focusedHostInput_.composing &&
                    !snowdesktop::private_pointer::Active() &&
                    !(snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000))
                    (void)ResolveHostInputTextDropPosition(currentWidget, *control,
                        {x, y}, focusedHostInput_.text, nullptr, &hitCharacter);
                focusedHostInput_.selectionDragArmed = hitCharacter &&
                    *hitCharacter >= start && *hitCharacter < end;
                if (!focusedHostInput_.selectionDragArmed)
                {
                    focusedHostInput_.cursor = HitTestHostInputPosition(*control, widgetId, x, y);
                    focusedHostInput_.selectionAnchor = focusedHostInput_.cursor;
                }
                focusedHostInput_.pointerSelecting = true;
                focusedHostInput_.caretVisibility.Request();
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
            const RECT bounds = BoundsForSurface(
                widget, normalizedSurface);
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
    if (!wheel)
    {
        if (IsPanelSurface(normalizedSurface))
        {
            const std::string pointerTarget =
                widget.panelInteractionRegions.TargetAt(
                    static_cast<float>(x), static_cast<float>(y));
            widget.panelViewKeyboardFocusKey =
                widget.panelInteractionRegions.IsKeyboardFocusable(
                    pointerTarget)
                ? pointerTarget : std::string{};
            widget.panelViewFocusCueVisible = false;
        }
        else
            BeginHostLogicalSlotPointer(widget, x, y);
    }
    return false;
}

void WidgetEngine::CloseWidgetPanelSurface(
    const std::wstring& widgetId, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    const std::string closingSurface = IsPanelSurface(surface)
        ? NormalizeWidgetSurface(surface)
        : NormalizeWidgetSurface(widget.panelSurface);
    if (focusedHostInput_.active &&
        focusedHostInput_.widgetId == widgetId &&
        IsPanelSurface(focusedHostInput_.surface))
    {
        BlurHostInput(false);
    }
    if (hostScrollbarDrag_.active &&
        hostScrollbarDrag_.widgetId == widgetId &&
        IsPanelSurface(hostScrollbarDrag_.surface))
        hostScrollbarDrag_ = {};
    widget.panelActive = false;
    if (d2dState_)
        for (const char* closing : {"panel", "dialog", "popover"})
            d2dState_->packageImageCache.ForgetOwner(
                WidgetWideToUtf8(widgetId) + "|" + closing);
    if (d2dState_) d2dState_->textLayoutCache.Erase(widgetId, closingSurface);
    ApplyWidgetHostVisibility(
        widget, widget.desktopVisible || widget.panelActive ||
            widget.keepRuntimeActiveForHiddenPage);
    widget.panelSurface = "panel";
    widget.panelInitialKeyboardFocusPending = false;
    widget.panelViewTransitions.Clear();
    widget.panelViewTransitionFramePending = false;
    widget.panelViewKeyboardFocusKey.clear();
    widget.panelViewFocusCueVisible = false;
    widget.panelInteractionRegions.CancelPointerPress();
    const auto transition = widget.panelInteractionRegions.ClearHover();
    if (transition.Changed())
        DispatchInteractionTransition(
            widget, transition, 0, 0, closingSurface);
    std::erase_if(widget.hostControls, [](const auto& control) {
        return IsPanelSurface(control.surface);
    });
    std::erase_if(pressedViewKeyTargets_,
        [&widgetId](const auto& entry) {
            return entry.second.widgetId == widgetId &&
                IsPanelSurface(entry.second.surface);
        });
}

LuaWidgetTheme WidgetEngine::RuntimeGetWidgetTheme(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    const auto desktop = idx >= 0 ? widgets_[idx].theme : LuaWidgetTheme{};
    const auto surface = d2dState_ && d2dState_->currentWidgetId == widgetId
        ? CurrentWidgetSurface(d2dState_) : std::string_view("desktop");
    return snowdesktop::widget_runtime::ResolveSurfaceTheme(
        desktop, panelTheme_ ? &*panelTheme_ : nullptr, surface);
}

void WidgetEngine::SetPanelTheme(const PersonalizationSettings& appearance)
{
    const auto rgb = [](float r, float g, float b) {
        const auto byte = [](float v) {
            return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (byte(r) << 16) | (byte(g) << 8) | byte(b);
    };
    LuaWidgetTheme theme;
    theme.bg = rgb(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB);
    theme.border = rgb(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB);
    theme.alpha = appearance.widgetAlpha;
    theme.borderAlpha = appearance.widgetBorderAlpha;
    theme.gradientEndA = appearance.gradientEndA;
    theme.cornerRadius = 18.0f;
    theme.contentTheme = appearance.contentTheme;
    panelTheme_ = theme;
}

void WidgetEngine::SetWidgetTheme(const std::wstring& widgetId, const LuaWidgetTheme& theme)
{
    int idx = FindWidget(widgetId);
    if (idx >= 0)
        widgets_[idx].theme = theme;
}

void WidgetEngine::SetWidgetLayoutMetrics(
    const std::wstring& widgetId,
    int columns, int rows, int cellWidth, int cellHeight,
    int gapY, int barHeight,
    DWRITE_FONT_WEIGHT fontWeight, float semanticCuScale,
    const snowdesktop::widget_runtime::SemanticUiMetricTokens&
        semanticUiMetrics)
{
    const int index = FindWidget(widgetId);
    const auto metrics = snowdesktop::widget_runtime::NormalizeLayoutMetrics(
            cellWidth, cellHeight, gapY, barHeight,
            fontWeight, semanticCuScale, semanticUiMetrics);
    if (index >= 0)
        widgets_[index].layoutMetrics = metrics;
    // A package chunk and its setup callback may query ui.metrics() before the
    // LuaWidget has entered widgets_. Keep the pending execution state aligned
    // with the instance and span that the host is about to load.
    if (d2dState_)
    {
        snowdesktop::widget_runtime::ApplyLayoutSpan(
            *d2dState_, columns, rows);
        snowdesktop::widget_runtime::ApplyLayoutMetrics(*d2dState_, metrics);
    }
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

void WidgetEngine::SetDesktopImageSpinOutput(
    snowdesktop::widget_runtime::WidgetImageSpin* output)
{
    if (d2dState_) d2dState_->desktopImageSpin = output;
}

void WidgetEngine::SetDesktopSurfacePaintOutput(
    snowdesktop::widget_runtime::WidgetSurfacePaint* output)
{
    if (d2dState_) d2dState_->desktopSurfacePaint = output;
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
    result.focused = !widget.viewKeyboardFocusKey.empty() ||
        !widget.panelViewKeyboardFocusKey.empty() ||
        (focusedHostInput_.active &&
            focusedHostInput_.widgetId == widgetId);
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
        if (widget.viewTree)
            entry.desktopViewNodes =
                snowdesktop::widget_runtime::InspectViewTree(
                    *widget.viewTree);
        if (widget.panelViewTree)
        {
            entry.auxiliaryViewNodes =
                snowdesktop::widget_runtime::InspectViewTree(
                    *widget.panelViewTree);
            entry.auxiliarySurface = widget.panelSurface;
        }
        std::string errorKey = WidgetWideToUtf8(widget.widgetId) + ".lastError";
        auto errIt = PersistentStorage().find(errorKey);
        if (errIt != PersistentStorage().end())
            entry.lastError = errIt->second;
        std::string logKey = WidgetWideToUtf8(widget.widgetId);
        entry.logs = g_widgetDiagnostics.EntriesFor(logKey);
        result.push_back(std::move(entry));
    }
    return result;
}

void WidgetEngine::RecordPerformanceResources() const noexcept
{
    if (!snowdesktop::performance::Enabled() || !d2dState_) return;
    try
    {
        using snowdesktop::performance::Value;
        using Pixels = snowdesktop::widget_runtime::WidgetRuntimeImagePixels;
        snowdesktop::performance::Scope scope("profiler", "sample.widget.resources");
        struct Usage
        {
            std::size_t sources = 0, pixelBytes = 0, bitmapBytes = 0;
            std::unordered_set<const Pixels*> pixels;
        };
        std::map<std::wstring_view, Usage> owners;
        for (const auto& widget : widgets_)
        {
            owners.try_emplace(widget.widgetId);
            if (widget.quota)
            {
                Value("widget.memory", "lua_bytes", widget.widgetId, static_cast<double>(widget.quota->memoryBytes));
                snowdesktop::lua_runtime::SampleAllocationTraffic(*widget.quota, widget.widgetId);
            }
            Value("widget.state", "valid", widget.widgetId, widget.valid ? 1 : 0);
            Value("widget.state", "visible", widget.widgetId, widget.hostVisible ? 1 : 0);
            Value("widget.state", "timers", widget.widgetId, static_cast<double>(widget.namedTimers.Size()));
            Value("widget.state", "animation_requests", widget.widgetId, static_cast<double>(widget.animationFrames.Size()));
            Value("widget.package", widget.packageId, widget.widgetId, 1);
        }
        std::unordered_set<const Pixels*> uniquePixels;
        std::size_t pixelBytes = 0;
        for (const auto& [key, resource] : d2dState_->runtimeImages)
        {
            auto& owner = owners[resource.ownerWidgetId];
            ++owner.sources;
            if (!resource.pixels) continue;
            const auto* pixels = resource.pixels.get();
            const auto bytes = pixels->bgraPremultiplied.size();
            if (owner.pixels.insert(pixels).second) owner.pixelBytes += bytes;
            if (uniquePixels.insert(pixels).second) pixelBytes += bytes;
        }
        const auto bitmapBytes = [](ID2D1Bitmap* bitmap) -> std::size_t {
            if (!bitmap) return 0;
            const auto size = bitmap->GetPixelSize();
            return std::size_t(size.width) * size.height * 4;
        };
        const auto cacheBytes = [&](const auto& cache) {
            std::size_t bytes = 0;
            for (const auto& [key, bitmap] : cache) bytes += bitmapBytes(bitmap.Get());
            return bytes;
        };
        for (const auto& [key, bitmap] : d2dState_->runtimeImageBitmaps)
        {
            const auto source = d2dState_->runtimeImages.find(key);
            if (source != d2dState_->runtimeImages.end())
                owners[source->second.ownerWidgetId].bitmapBytes += bitmapBytes(bitmap.Get());
        }
        for (const auto& [id, usage] : owners)
        {
            Value("widget.memory", "runtime_image_sources", id, static_cast<double>(usage.sources));
            Value("widget.memory", "runtime_image_referenced_bytes", id, static_cast<double>(usage.pixelBytes));
            Value("widget.memory", "runtime_bitmap_bgra_bytes_estimate", id, static_cast<double>(usage.bitmapBytes));
        }
        const auto shared = [](std::string_view name, std::size_t value) {
            Value("widget.shared.memory", name, {}, static_cast<double>(value));
        };
        // Source pixels can be shared across owners. Report their deduplicated
        // total separately; owner rows are references, not exclusive ownership.
        shared("runtime_image_unique_bytes", pixelBytes);
        shared("runtime_bitmap_bgra_bytes_estimate", cacheBytes(d2dState_->runtimeImageBitmaps));
        shared("package_image_decoded_bytes", d2dState_->packageImageCache.Bytes());
        shared("package_image_encoded_bytes", d2dState_->packageImageCache.EncodedBytes());
        shared("package_image_sources", d2dState_->packageImageCache.Size());
        shared("package_bitmap_bgra_bytes_estimate", cacheBytes(d2dState_->imageCache));
        shared("image_sampling_bgra_bytes_estimate", d2dState_->imageSamplingCache.RetainedBytes());
        shared("image_sampling_entries", d2dState_->imageSamplingCache.Size());
        shared("shell_icon_bgra_bytes_estimate", cacheBytes(d2dState_->shellIconCache));
        shared("shell_icon_count", d2dState_->shellIconCache.size());
        const auto resourceCounts = d2dState_->ResourceCounts();
        shared("text_format_count", resourceCounts.textFormats);
        shared("text_layout_cache_entries", d2dState_->textLayoutCache.Size());
        shared("text_layout_cache_input_bytes", d2dState_->textLayoutCache.RetainedInputBytes());
        shared("private_text_format_count", resourceCounts.privateTextFormats);
        shared("private_font_count", resourceCounts.privateFonts);
        shared("brush_count", resourceCounts.brushes);
        shared("background_cache_retained_bytes_estimate", d2dState_->backgroundCache.RetainedBytes());
        shared("background_cache_entries", d2dState_->backgroundCache.Size());
    }
    catch (...)
    {
        // Diagnostic allocation failure must not change the running desktop.
        snowdesktop::performance::Value("profiler", "widget_resources_error", {}, 1);
    }
}

// Menu metadata cache is separate from runtime manifest/permission loading.
std::vector<snowdesktop::widget_menu::Entry> WidgetEngine::ListAvailableMenuEntries()
{
    static thread_local snowdesktop::widget_menu::Catalogue catalogue;
    std::string refreshError;
    if (!RefreshWidgetPackages(refreshError))
        OutputDebugStringW((L"Widget catalog refresh failed: " +
            Utf8ToWideLocal(refreshError) + L"\n").c_str());
    const auto packages = GetWidgetPackageManager().ListPackages();
    return catalogue.Build(packages, Locale::Instance().GetEffectiveLanguage(),
        [](const snowdesktop::widget::InstalledPackage& package) {
            const auto manifest = GetWidgetManifest((package.root /
                Utf8ToWideLocal(package.manifest.entry)).wstring());
            return snowdesktop::widget_menu::Metadata{
                Utf8ToWideLocal(package.manifest.id),
                Utf8ToWideLocal(manifest.name),
                Utf8ToWideLocal(manifest.description),
                Utf8ToWideLocal(manifest.publisher),
                snowdesktop::widget::MeetsWidgetMinimumHostVersion(
                    SNOWDESKTOP_VERSION, manifest.minHostVersion),
                manifest.hasManifest};
        });
}

void WidgetEngine::PrepareWidgetPanelFocus(const std::wstring& widgetId)
{
    if (const int index = FindWidget(widgetId); index >= 0)
    {
        widgets_[index].panelInitialKeyboardFocusPending = true;
        widgets_[index].panelViewKeyboardFocusKey.clear();
        widgets_[index].panelViewFocusCueVisible = false;
    }
}

void WidgetEngine::RuntimeOpenWidgetPanel(
    const std::wstring& widgetId, std::wstring title,
    int width, int height)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || !openWidgetPanelCallback_)
        return;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.desktopGesture = trustedGestureState_.Active() &&
        CurrentWidgetSurface(d2dState_) == "desktop";
    if (request.desktopGesture && d2dState_)
        request.desktopOpenerKey = d2dState_->desktopPopupOpenerKey;
    request.title = std::move(title);
    request.surface = "panel";
    request.width = std::clamp(width, 320, 900);
    request.height = std::clamp(height, 280, 900);
    const std::string_view sourceSurface = CurrentWidgetSurface(d2dState_);
    const int index = FindWidget(widgetId);
    if (trustedGestureState_.Active() && IsPanelSurface(sourceSurface) && index >= 0)
    {
        const auto& widget = widgets_[index];
        float x = 0.0f, y = 0.0f;
        if (InteractionRegionsForSurface(widget, sourceSurface).LastPointer(x, y))
        {
            const LONG left = widget.panelBounds.left + static_cast<LONG>(std::floor(x));
            const LONG top = widget.panelBounds.top + static_cast<LONG>(std::floor(y));
            request.hasAnchor = true;
            request.anchorRect = {left, top, left, top};
        }
    }
    openWidgetPanelCallback_(request);
}

void WidgetEngine::RuntimeOpenWidgetDialog(
    const std::wstring& widgetId, std::wstring title,
    int width, int height, bool dismissOnOutside,
    bool dismissOnEscape)
{
    if (snowdesktop::widget_runtime::IsDryLoad() ||
        !openWidgetPanelCallback_)
        return;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.desktopGesture = trustedGestureState_.Active() &&
        CurrentWidgetSurface(d2dState_) == "desktop";
    if (request.desktopGesture && d2dState_)
        request.desktopOpenerKey = d2dState_->desktopPopupOpenerKey;
    request.title = std::move(title);
    request.surface = "dialog";
    request.width = std::clamp(width, 320, 900);
    request.height = std::clamp(height, 280, 900);
    request.dismissOnOutside = dismissOnOutside;
    request.dismissOnEscape = dismissOnEscape;
    request.modal = true;
    openWidgetPanelCallback_(request);
}

bool WidgetEngine::RuntimeOpenWidgetPopover(
    const std::wstring& widgetId, std::wstring title,
    std::string anchorKey, std::string placement,
    int width, int height, bool dismissOnOutside,
    bool dismissOnEscape)
{
    if (snowdesktop::widget_runtime::IsDryLoad() ||
        !openWidgetPanelCallback_ ||
        !trustedGestureState_.Active() || anchorKey.empty() ||
        CurrentWidgetSurface(d2dState_) != "desktop")
        return false;
    static constexpr std::array<std::string_view, 9> placements = {
        "auto", "top", "bottom", "left", "right",
        "topStart", "topEnd", "bottomStart", "bottomEnd"
    };
    if (std::find(placements.begin(), placements.end(), placement) ==
        placements.end())
        return false;
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].valid ||
        widgets_[index].preview)
        return false;
    const auto* anchor =
        widgets_[index].interactionRegions.Find(anchorKey);
    if (!anchor || !anchor->enabled)
        return false;
    float left = anchor->shape.x;
    float top = anchor->shape.y;
    float right = anchor->shape.x + anchor->shape.width;
    float bottom = anchor->shape.y + anchor->shape.height;
    if (anchor->shape.type ==
        snowdesktop::widget_runtime::InteractionShapeType::Circle)
    {
        left = anchor->shape.x - anchor->shape.radius;
        top = anchor->shape.y - anchor->shape.radius;
        right = anchor->shape.x + anchor->shape.radius;
        bottom = anchor->shape.y + anchor->shape.radius;
    }
    if (anchor->clip)
    {
        left = std::max(left, anchor->clip->x);
        top = std::max(top, anchor->clip->y);
        right = std::min(right,
            anchor->clip->x + anchor->clip->width);
        bottom = std::min(bottom,
            anchor->clip->y + anchor->clip->height);
    }
    if (right <= left || bottom <= top)
        return false;
    const RECT ownerBounds = widgets_[index].lastBounds;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.desktopGesture = trustedGestureState_.Active() &&
        CurrentWidgetSurface(d2dState_) == "desktop";
    request.title = std::move(title);
    request.surface = "popover";
    request.anchorKey = anchorKey;
    request.placement = std::move(placement);
    request.anchorRect = {
        ownerBounds.left + static_cast<LONG>(std::floor(left)),
        ownerBounds.top + static_cast<LONG>(std::floor(top)),
        ownerBounds.left + static_cast<LONG>(std::ceil(right)),
        ownerBounds.top + static_cast<LONG>(std::ceil(bottom))
    };
    request.hasAnchor = true;
    request.showHeader = !request.title.empty();
    request.width = std::clamp(width, 200, 720);
    request.height = std::clamp(height, 120, 720);
    request.dismissOnOutside = dismissOnOutside;
    request.dismissOnEscape = dismissOnEscape;
    openWidgetPanelCallback_(request);
    return true;
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

std::optional<std::string>
WidgetEngine::RuntimeResolvePackageImageContentKey(
    const std::wstring& widgetId, std::string_view name) const
{
    const int index = FindWidget(widgetId);
    if (index < 0 || name.empty()) return std::nullopt;
    const auto key = widgets_[index].packageImageContentKeys.find(
        std::string(name));
    if (key == widgets_[index].packageImageContentKeys.end())
        return std::nullopt;
    return key->second;
}

const snowdesktop::widget_runtime::PackageImageSource*
WidgetEngine::RuntimeFindPackageImageSource(
    const std::wstring& widgetId, std::string_view name) const noexcept
{
    if (!d2dState_) return nullptr;
    const auto contentKey = RuntimeResolvePackageImageContentKey(
        widgetId, name);
    return contentKey
        ? d2dState_->packageImageCache.Find(*contentKey) : nullptr;
}

LuaWidgetManifest WidgetEngine::GetWidgetManifest(const std::wstring& filename)
{
    const auto path = PathIsRelativeW(filename.c_str()) ? ResolveWidgetPath(filename) : filename;
    if (path.empty()) return {};
    const auto package = ResolveWidgetEntryPackage(path);
    return snowdesktop::widget_runtime::ReadWidgetScriptManifest(path,
        package ? package->root : std::filesystem::path{});
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
    const std::optional<std::string>& previousVersion, std::wstring& error,
    std::uint64_t diagnosticQueryId,
    const snowdesktop::widget::InstalledPackage* workshopCandidate,
    bool* deterministicFailure)
{
    const auto trace = [&](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(diagnosticQueryId,
            "package=" + packageId + " " + std::string(message));
    };
    trace("phase=verify.begin previous=" + previousVersion.value_or("none"));
    if (snowdesktop::widget_runtime::HasStorageOverlay())
    {
        error = L"Another component storage migration is already running.";
        trace("phase=verify.end result=failed reason=storage_overlay_busy");
        return false;
    }

    const auto selected = GetWidgetPackageManager().ResolveSelected(packageId);
    if (!selected)
    {
        error = L"The installed component selection is unavailable.";
        trace("phase=verify.end result=failed reason=selection_unavailable");
        return false;
    }
    const bool verifyingWorkshopCandidate = workshopCandidate &&
        selected->root == workshopCandidate->root;
    if (!verifyingWorkshopCandidate) deterministicFailure = nullptr;
    std::vector<std::wstring> liveInstances;
    for (const auto& widget : widgets_)
        if (selected->enabled && widget.packageId == packageId)
            liveInstances.push_back(widget.widgetId);

    std::unordered_map<std::string, std::string> migratedStorage = PersistentStorage();
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
            DryLoadScope dryLoadScope;
            try
            {
                trace("phase=dry_load.begin version=" + selected->manifest.version);
                const std::wstring path =
                    ResolveWidgetPath(Utf8ToWideLocal(packageId));
                ok = !path.empty() &&
                    LoadWidget(path, dryId, false, nullptr, nullptr, nullptr, deterministicFailure);
                if (!ok)
                {
                    if (const auto runtimeFailure = GetWidgetRuntimeFailure(dryId))
                        failure = runtimeFailure->detail;
                }
                trace("phase=dry_load.end result=" + std::string(ok ? "ok" : "failed") +
                    " pathResolved=" + std::to_string(!path.empty()) + " error=" + failure);
            }
            catch (...)
            {
                DeleteWidgetInstance(dryId);
                throw;
            }
            DeleteWidgetInstance(dryId);
        }
        else
        {
            for (const auto& widgetId : liveInstances)
            {
                if (!ReloadWidget(widgetId, deterministicFailure))
                {
                    ok = false;
                    if (const auto runtimeFailure = GetWidgetRuntimeFailure(widgetId))
                        failure = runtimeFailure->detail;
                    break;
                }
                updatedInstances.push_back(widgetId);
            }
        }
    }
    if (ok)
    {
        MutablePersistentStorage() = std::move(migratedStorage);
        SaveStorageFile();
        TrimWidgetPackageHistory(GetWidgetPackageManager(), packageId);
        error.clear();
        trace("phase=verify.end result=ok");
        return true;
    }

    const bool rejectedManaged = !selected->builtin && !selected->development;
    const bool recovered = RecoverWidgetPackage(packageId, previousVersion,
        rejectedManaged ? std::string_view(selected->manifest.version) : std::string_view{});
    trace("phase=verify.recovery recovered=" + std::to_string(recovered) +
        " retained=" + std::to_string(GetWidgetPackageManager().ContainsPackage(packageId)));
    for (const auto& widgetId : updatedInstances)
        (void)ReloadWidget(widgetId);
    if (rejectedManaged && recovered)
    {
        // A retained failed version otherwise satisfies the next sync plan.
        std::string quarantineError;
        if (!GetWidgetPackageManager().QuarantineInactiveVersion(*selected, quarantineError))
            trace("phase=verify.quarantine result=failed error=" + quarantineError);
    }
    if (failure.empty()) failure = "Component load or storage migration failed.";
    if (recovered) failure += "\nRestored last-known-good component version.";
    error = Utf8ToWideLocal(failure);
    trace("phase=verify.end result=failed error=" + WidgetWideToUtf8(error));
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
    if (const auto previous = manager.ResolveSelected(incoming.id))
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

std::vector<std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>>
WidgetEngine::SnapshotWidgetPackageSourceProviders()
{
    std::vector<std::shared_ptr<snowdesktop::widget::IWidgetPackageSource>> result;
    for (const auto& [id, source] : GetWidgetPackageSources())
    {
        (void)id;
        result.push_back(source);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left->ProviderId() < right->ProviderId();
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
    static const bool compatible = snowdesktop::steam_entitlement::
        IsSteamBridgeExecutableCompatible(bridge, SNOWDESKTOP_VERSION);
    return compatible;
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
    const std::string& locale, std::uint64_t diagnosticQueryId)
{
    const auto trace = [diagnosticQueryId](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(diagnosticQueryId, message);
    };
    trace("phase=query.begin host=" SNOWDESKTOP_VERSION " locale=" + locale +
        " exe=" + WidgetWideToUtf8(GetExecutableDirectoryPath()) +
        " data=" + WidgetWideToUtf8(GetDataDirectoryPath()));
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
        snapshot = source.QuerySubscriptions(query, error, trace);
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
    snapshot.diagnosticQueryId = diagnosticQueryId;
    trace("phase=query.end subscribed=" + std::to_string(snapshot.subscribedPublishedFileIds.size()) +
        " installable=" + std::to_string(snapshot.installable.size()) +
        " failures=" + std::to_string(snapshot.discoveryFailures.size()) +
        " warning=" + snapshot.warning + " error=" + snapshot.error);
    return snapshot;
}

snowdesktop::widget::SteamWorkshopSubscriptionHistory
WidgetEngine::GetSteamWorkshopSubscriptionHistory()
{
    return GetWidgetPackageManager().SteamSubscriptionHistory();
}

snowdesktop::widget::SteamLibrarySubscriptionHistory
WidgetEngine::GetSteamWorkshopLibrarySubscriptionHistory()
{
    return GetWidgetPackageManager().SteamLibraryHistory();
}

snowdesktop::widget::InstalledPackageCatalog WidgetEngine::SnapshotWidgetPackageCatalog()
{
    return GetWidgetPackageManager().CatalogSnapshot();
}

snowdesktop::widget::SteamWorkshopSyncPlan WidgetEngine::PrepareSteamWorkshopSubscriptionArtifacts(
    snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const std::vector<snowdesktop::widget::InstalledPackage>& installed,
    const std::filesystem::path& stagingRoot)
{
    if (!snapshot.CanSynchronize()) return {};
    auto plan = snowdesktop::widget::BuildSteamWorkshopSyncPlan(
        installed, snapshot);
    const auto trace = [&](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(snapshot.diagnosticQueryId, message);
    };
    trace("phase=prepare.begin registered=" + std::to_string(installed.size()) +
        " actions=" + std::to_string(plan.actions.size()) +
        " conflicts=" + std::to_string(plan.conflicts.size()));
    for (const auto& package : installed)
    {
        if (std::none_of(snapshot.installable.begin(), snapshot.installable.end(),
            [&](const auto& available) { return available.manifest.id == package.manifest.id; }))
            continue;
        trace("phase=registry.candidate package=" + package.manifest.id +
            " version=" + package.manifest.version +
            " provider=" + package.source.providerId +
            " item=" + package.source.externalItemId +
            " active=" + std::to_string(package.active) +
            " builtin=" + std::to_string(package.builtin) +
            " development=" + std::to_string(package.development) +
            " unregistered=" + std::to_string(package.unregistered) +
            " path=" + WideToUtf8(package.root.wstring()));
    }
    for (const auto& conflict : plan.conflicts)
        trace("phase=plan.conflict error=" + conflict);
    if (plan.actions.empty())
    {
        trace("phase=prepare.end prepared=0 errors=0");
        return plan;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(stagingRoot, filesystemError);
    if (filesystemError)
    {
        snapshot.preparationErrors.push_back(
            "cannot create local Workshop staging directory: " +
            filesystemError.message());
        trace("phase=prepare.end result=failed error=" + filesystemError.message());
        return plan;
    }

    for (const auto& action : plan.actions)
    {
        trace("phase=plan.action item=" + action.externalItemId + " package=" + action.packageId +
            " kind=" + snowdesktop::widget::SteamWorkshopDiagnosticActionName(action.kind) +
            " version=" + action.version);
        if (action.kind == snowdesktop::widget::
                SteamWorkshopSyncActionKind::Uninstall)
            continue;
        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(action.externalItemId);
        if (RejectedSteamWorkshopArchive(snapshot, publishedFileId))
            continue;
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
        trace("phase=stage.begin item=" + action.externalItemId);
        std::filesystem::copy_file(artifact->second, destination,
            std::filesystem::copy_options::overwrite_existing,
            filesystemError);
        if (filesystemError)
        {
            snapshot.preparationErrors.push_back(action.packageId +
                ": cannot stage detected Workshop package: " +
                filesystemError.message());
            trace("phase=stage.end item=" + action.externalItemId +
                " result=failed error=" + filesystemError.message());
            continue;
        }
        const auto digest = snapshot.archiveDigests.find(publishedFileId);
        if (digest != snapshot.archiveDigests.end() && !digest->second.empty() &&
            snowdesktop::widget::WidgetPackageManager::Sha256File(destination) != digest->second)
        {
            std::filesystem::remove(destination, filesystemError);
            snapshot.preparationErrors.push_back(action.packageId +
                ": Workshop archive changed while staging");
            continue;
        }
        snapshot.preparedArtifacts[publishedFileId] = destination;
        trace("phase=stage.end item=" + action.externalItemId + " result=ok");
    }
    trace("phase=prepare.end prepared=" + std::to_string(snapshot.preparedArtifacts.size()) +
        " errors=" + std::to_string(snapshot.preparationErrors.size()));
    return plan;
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
    const snowdesktop::widget::SteamWorkshopSubscriptionSnapshot& snapshot,
    const snowdesktop::widget::InstalledPackageCatalog& preparedCatalog,
    const snowdesktop::widget::SteamWorkshopSyncPlan& preparedPlan)
{
    auto& manager = GetWidgetPackageManager();
    const auto currentCatalog = manager.CatalogSnapshot();
    std::optional<snowdesktop::widget::SteamWorkshopSyncPlan> updatedPlan;
    if (currentCatalog != preparedCatalog)
    {
        snowdesktop::performance::Scope planTrace("shared.maintenance", "steam.apply.plan");
        updatedPlan.emplace(snowdesktop::widget::BuildSteamWorkshopSyncPlan(*currentCatalog, snapshot));
    }
    const auto& plan = updatedPlan ? *updatedPlan : preparedPlan;
    const auto trace = [&](std::string_view message) {
        snowdesktop::widget::WriteSteamWorkshopDiagnostic(snapshot.diagnosticQueryId, message);
    };
    trace("phase=apply.begin actions=" + std::to_string(plan.actions.size()) +
        " conflicts=" + std::to_string(plan.conflicts.size()) +
        " prepared=" + std::to_string(snapshot.preparedArtifacts.size()));
    for (const auto& conflict : plan.conflicts)
        trace("phase=plan.conflict error=" + conflict);
    snowdesktop::widget::SteamWorkshopSyncResult result;
    result.warnings = plan.conflicts;
    result.installFailures = snapshot.discoveryFailures;
    for (const auto& failure : snapshot.discoveryFailures)
    {
        result.errors.push_back(failure.packageId + ": " + failure.error);
    }
    result.errors.insert(result.errors.end(),
        snapshot.preparationErrors.begin(), snapshot.preparationErrors.end());
    for (const auto& action : plan.actions)
    {
        snowdesktop::performance::Scope actionTrace("shared.maintenance", "steam.apply.package_action");
        trace("phase=apply.action item=" + action.externalItemId + " package=" + action.packageId +
            " kind=" + snowdesktop::widget::SteamWorkshopDiagnosticActionName(action.kind) +
            " version=" + action.version);
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
                    (void)EnsureWidgetLoaded(instance, Utf8ToWideLocal(action.packageId));
                result.errors.push_back(action.packageId + ": " + error);
            }
            continue;
        }

        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(action.externalItemId);
        if (const auto rejected = RejectedSteamWorkshopArchive(snapshot, publishedFileId))
        {
            result.errors.push_back(action.packageId + ": " + *rejected);
            recordInstallFailure(*rejected);
            continue;
        }
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
        if (const auto previous = manager.ResolveSelected(action.packageId))
            if (!previous->builtin && !previous->development)
                previousVersion = previous->manifest.version;
        snowdesktop::widget::InstalledPackage installed;
        snowdesktop::widget::ValidationReport report;
        std::string installError;
        const snowdesktop::widget::PackageSourceRef sourceRef{
            "steam-workshop", action.externalItemId };
        trace("phase=install.begin item=" + action.externalItemId + " package=" + action.packageId);
        const bool installedOk = manager.InstallArchive(prepared->second,
            sourceRef, false, installed, report, installError, false,
            &action.expectedManifest);
        trace("phase=install.end item=" + action.externalItemId + " package=" + action.packageId +
            " result=" + (installedOk ? "ok" : "failed") + " error=" + installError);
        std::error_code cleanupError;
        std::filesystem::remove(prepared->second, cleanupError);
        std::wstring error;
        bool deterministicFailure = false;
        const bool verified = installedOk && VerifyInstalledWidgetPackage(
            installed.manifest.id, previousVersion, error, snapshot.diagnosticQueryId,
            &installed, &deterministicFailure);
        if (installedOk && verified)
        {
            ForgetRejectedSteamWorkshopArchive(publishedFileId);
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
            if (deterministicFailure)
            {
                const auto digest = snapshot.archiveDigests.find(publishedFileId);
                if (digest != snapshot.archiveDigests.end())
                    RememberRejectedSteamWorkshopArchive(
                        publishedFileId, digest->second, displayError);
            }
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
    // Subscription identities describe the local manifest independently of
    // whether each package can be applied. Persist them even when one item
    // conflicts or fails; remaining managed copies are retried from their
    // installed source bindings on the next reconciliation.
    if (snapshot.CanSynchronize() &&
        !snapshot.activeSteamAccountId.empty())
    {
        std::string historyError;
        const auto [subscriptionHistory, libraryHistory] = [&] {
            snowdesktop::performance::Scope historyTrace("shared.maintenance", "steam.apply.history_build");
            return std::pair(snowdesktop::widget::BuildSteamWorkshopSubscriptionHistory(
                    snapshot, manager.SteamSubscriptionHistory()),
                snowdesktop::widget::BuildSteamWorkshopLibraryHistory(
                    snapshot, manager.SteamLibraryHistory(), *manager.CatalogSnapshot()));
        }();
        snowdesktop::performance::Scope historySaveTrace("shared.maintenance", "steam.apply.history_save");
        if (!manager.UpdateSteamSubscriptionHistory(
                snapshot.activeSteamAccountId, subscriptionHistory, historyError,
                &libraryHistory))
        {
            result.errors.push_back(
                "cannot save Steam subscription history: " + historyError);
        }
    }
    if (snapshot.authoritative || snapshot.partial)
    {
        auto& cache = GetSteamWorkshopPackageAssociationCache();
        std::lock_guard lock(cache.mutex);
        if (snapshot.authoritative)
            cache.installFailures = result.installFailures;
        else
        {
            std::erase_if(cache.installFailures, [&](const auto& failure) {
                const auto id = snowdesktop::widget::SteamPublishedFileId(failure.externalItemId);
                return std::find(snapshot.subscribedPublishedFileIds.begin(),
                    snapshot.subscribedPublishedFileIds.end(), id) !=
                    snapshot.subscribedPublishedFileIds.end();
            });
            cache.installFailures.insert(cache.installFailures.end(),
                result.installFailures.begin(), result.installFailures.end());
        }
    }
    for (const auto& error : result.errors)
        trace("phase=apply.error error=" + error);
    trace("phase=apply.end installed=" + std::to_string(result.installed) +
        " updated=" + std::to_string(result.updated) +
        " uninstalled=" + std::to_string(result.uninstalled) +
        " errors=" + std::to_string(result.errors.size()) +
        " registered=" + std::to_string(manager.CatalogSnapshot()->size()));
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
    bool allowSourceChange, bool allowPermissionExpansion, bool retryWorkshop)
{
    const auto source = GetWidgetPackageSources().find(providerId);
    if (source == GetWidgetPackageSources().end())
    {
        error = Utf8ToWideLocal(
            "component source is not registered: " + providerId);
        return false;
    }
    const bool workshop = providerId == "steam-workshop";
    const std::string workshopItem = workshop
        ? snowdesktop::widget::SteamPublishedFileId(externalItemId) : std::string{};
    std::optional<snowdesktop::widget::SteamWorkshopSource> freshWorkshopSource;
    auto* provider = source->second.get();
    if (workshop && retryWorkshop)
    {
        // Manual installation bypasses background suppression. Keep the
        // previous entry until success so a staged poll cannot lose its reason.
        provider = &freshWorkshopSource.emplace();
    }
    std::string sourceError;
    const auto details = version.empty()
        ? provider->GetDetails(externalItemId, sourceError)
        : provider->GetVersionDetails(
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
    if (const auto previous = manager.ResolveSelected(details->manifest.id))
        if (!previous->builtin && !previous->development)
            previousVersion = previous->manifest.version;
    if (workshop && retryWorkshop)
    {
        // Old hosts kept rejected versions alongside the active fallback.
        for (const auto& package : manager.ListPackages())
        {
            if (package.active || package.builtin || package.development ||
                package.manifest.id != details->manifest.id ||
                package.manifest.version != requestedVersion ||
                package.source.providerId != providerId ||
                snowdesktop::widget::SteamPublishedFileId(package.source.externalItemId) != workshopItem)
                continue;
            if (!manager.QuarantineInactiveVersion(package, sourceError))
            {
                error = Utf8ToWideLocal(sourceError);
                return false;
            }
            break;
        }
    }
    snowdesktop::widget::InstalledPackage installed;
    snowdesktop::widget::ValidationReport report;
    if (!manager.InstallFromSource(*provider, externalItemId,
        requestedVersion,
        allowSourceChange, installed, report, sourceError,
        allowPermissionExpansion))
    {
        error = Utf8ToWideLocal(sourceError);
        if (!report.Ok()) error += L"\n" + Utf8ToWideLocal(report.ToJson());
        if (workshop)
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
        installed.manifest.id, previousVersion, error, 0, workshop ? &installed : nullptr);
    if (workshop)
    {
        if (verified)
        {
            ForgetRejectedSteamWorkshopArchive(workshopItem);
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

bool WidgetEngine::RefreshWidgetPackages(std::string& error)
{
    return GetWidgetPackageManager().RefreshCatalog(error);
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
    std::string& error, bool nativeAcknowledged, std::string_view reviewedNativeIdentity)
{
    const std::string requestedId = WidgetWideToUtf8(packageId);
    if (requestedId.empty())
    {
        error = "package id is empty";
        return false;
    }
    return GetWidgetPackageManager().SetPermissionDecision(
        requestedId, state, grantedPermissions,
        grantedNetworkDomains, error, nativeAcknowledged, reviewedNativeIdentity);
}

bool WidgetEngine::ApplyWidgetPermissionDecision(
    const std::wstring& packageId,
    snowdesktop::widget::PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error, bool nativeAcknowledged, std::string_view reviewedNativeIdentity)
{
    if (!SetWidgetPermissionDecision(packageId, state,
            grantedPermissions, grantedNetworkDomains, error, nativeAcknowledged, reviewedNativeIdentity))
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
    const std::string normalizedSurface =
        NormalizeWidgetSurface(surface);
    WidgetSurfaceScope surfaceScope(d2dState_, normalizedSurface.c_str());
    if (!RuntimeFocusViewTarget(widgetId, controlId, "programmatic"))
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
    if (!trustedGestureState_.Active() &&
        !(d2dState_ && d2dState_->openingFocusAllowed))
    {
        error = "trustedGestureRequired";
        return false;
    }
    const std::string submitSurface(CurrentWidgetSurface(d2dState_));
    if (hostInputSubmitFocus_ &&
        hostInputSubmitFocus_->Matches(widgetId, id, submitSurface))
    {
        const int submittedIndex = FindWidget(widgetId);
        if (submittedIndex < 0)
        {
            error = "hostUnavailable";
            return false;
        }
        const auto& submittedWidget = widgets_[submittedIndex];
        // Input-field clicks clear the containing widget's selection. Restore
        // only this live editor, using the callback's fresh storage value.
        if (hostInputSubmitFocus_->AllowsFocus(widgetId, id, submitSurface,
                submittedWidget.valid, submittedWidget.preview,
                IsPanelSurface(submitSurface) ? submittedWidget.panelActive
                                             : submittedWidget.desktopVisible) &&
            RuntimeFocusHostInput(widgetId, id, "programmatic"))
            return true;
        error = "controlNotFound";
        return false;
    }
    if (RuntimeFocusViewTarget(widgetId, id, "programmatic"))
        return true;

    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    const auto& widget = widgets_[index];
    const auto& controls = widget.hostControls;
    const std::string surface(CurrentWidgetSurface(d2dState_));
    const bool submittedWithKey = std::any_of(
        controls.begin(), controls.end(), [&](const auto& control) {
            return HostControlBelongsToSurface(control, surface) &&
                control.id == id;
        });
    if (submittedWithKey ||
        InteractionRegionsForSurface(widget, surface).Find(id))
    {
        error = "controlNotFound";
        return false;
    }
    widgets_[index].deferredHostInputFocus.Request(id, surface);
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeBlurHostInputFromTrustedGesture(
    const std::wstring& widgetId, const std::string& id,
    std::string& error)
{
    error.clear();
    if (!trustedGestureState_.Active())
    {
        error = "trustedGestureRequired";
        return false;
    }
    if (FindWidget(widgetId) < 0)
    {
        error = "hostUnavailable";
        return false;
    }
    if (!focusedHostInput_.active ||
        focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id ||
        focusedHostInput_.surface != CurrentWidgetSurface(d2dState_))
    {
        error = "controlNotFocused";
        return false;
    }
    BlurHostInput(false);
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

void WidgetEngine::RegisterDrawAPI(lua_State* L)
{
    using snowdesktop::widget_api::CatalogFunctionDescriptor;
    using snowdesktop::widget_api::RegisterFunctionCatalog;
    using snowdesktop::widget_runtime::lua_DrawText;
    using snowdesktop::widget_runtime::lua_MeasureText;
    using snowdesktop::widget_runtime::lua_DrawRect;
    using snowdesktop::widget_runtime::lua_DrawPushClip;
    using snowdesktop::widget_runtime::lua_DrawPopClip;
    using snowdesktop::widget_runtime::lua_DrawStrokeRect;
    using snowdesktop::widget_runtime::lua_DrawMarqueeText;
    using snowdesktop::widget_runtime::lua_DrawArc;
    using snowdesktop::widget_runtime::lua_DrawPath;
    using snowdesktop::widget_runtime::lua_DrawGradientRect;
    using snowdesktop::widget_runtime::lua_DrawShadow;
    using snowdesktop::widget_runtime::lua_DrawSparkline;
    using snowdesktop::widget_runtime::lua_DrawImage;
    using snowdesktop::widget_runtime::lua_DrawImageFit;
    using snowdesktop::widget_runtime::lua_DrawLine;
    using snowdesktop::widget_runtime::lua_DrawCircle;
    using snowdesktop::widget_runtime::lua_DrawFa;
    using snowdesktop::widget_runtime::lua_DrawFluent;
    using snowdesktop::widget_runtime::lua_ResourceExists;
    using snowdesktop::widget_runtime::lua_ResourceAudio;

    using snowdesktop::widget_runtime::lua_LayoutWidth;
    using snowdesktop::widget_runtime::lua_LayoutHeight;
    using snowdesktop::widget_runtime::lua_LayoutColumns;
    using snowdesktop::widget_runtime::lua_LayoutRows;
    using snowdesktop::widget_runtime::lua_LayoutSizeClass;
    using snowdesktop::widget_runtime::lua_LayoutCellWidth;
    using snowdesktop::widget_runtime::lua_LayoutCellHeight;
    using snowdesktop::widget_runtime::lua_LayoutCellScale;
    using snowdesktop::widget_runtime::lua_LayoutCu;
    using snowdesktop::widget_runtime::lua_LayoutFontCu;
    using snowdesktop::widget_runtime::lua_LayoutCellGap;
    using snowdesktop::widget_runtime::lua_LayoutBarHeight;
    using snowdesktop::widget_runtime::lua_LayoutContentWidth;
    using snowdesktop::widget_runtime::lua_LayoutContentHeight;
    using snowdesktop::widget_runtime::lua_LayoutVw;
    using snowdesktop::widget_runtime::lua_LayoutVh;
    using snowdesktop::widget_runtime::lua_LayoutVmin;
    using snowdesktop::widget_runtime::lua_LayoutVmax;
    using snowdesktop::widget_runtime::lua_LayoutRpx;
    using snowdesktop::widget_runtime::lua_LayoutRpxX;
    using snowdesktop::widget_runtime::lua_LayoutRpxY;
    using snowdesktop::widget_runtime::lua_StorageGet;
    using snowdesktop::widget_runtime::lua_StorageSet;
    using snowdesktop::widget_runtime::lua_StorageRemove;
    using snowdesktop::widget_runtime::lua_StorageKeys;
    using snowdesktop::widget_runtime::lua_StorageTransaction;
    using snowdesktop::widget_runtime::RegisterDataSubscriptionHandle;
    using snowdesktop::widget_runtime::lua_DataSubscribe;
    using snowdesktop::widget_runtime::lua_SystemCpu;
    using snowdesktop::widget_runtime::lua_SystemMemory;
    using snowdesktop::widget_runtime::lua_SystemBattery;
    using snowdesktop::widget_runtime::lua_SystemNetwork;
    using snowdesktop::widget_runtime::lua_SystemGpu;
    using snowdesktop::widget_runtime::lua_MediaCurrent;
    using snowdesktop::widget_runtime::lua_MediaPlayPause;
    using snowdesktop::widget_runtime::lua_MediaNext;
    using snowdesktop::widget_runtime::lua_MediaPrevious;
    using snowdesktop::widget_runtime::lua_SystemInfoV2;
    using snowdesktop::widget_runtime::lua_SystemUptime;
    using snowdesktop::widget_runtime::lua_TimeNow;
    using snowdesktop::widget_runtime::lua_TimeMonotonic;
    using snowdesktop::widget_runtime::lua_TimeParts;
    using snowdesktop::widget_runtime::lua_TimeFormat;
    using snowdesktop::widget_runtime::lua_TimeAdd;
    using snowdesktop::widget_runtime::lua_TimeCompare;
    using snowdesktop::widget_runtime::lua_ScheduleAt;
    using snowdesktop::widget_runtime::lua_ScheduleTimeline;
    using snowdesktop::widget_runtime::lua_ScheduleEvery;
    using snowdesktop::widget_runtime::lua_ScheduleAfter;
    using snowdesktop::widget_runtime::lua_ScheduleCancel;
    using snowdesktop::widget_runtime::lua_AnimationRequestFrame;
    using snowdesktop::widget_runtime::lua_AnimationCancelFrame;
    using snowdesktop::widget_runtime::lua_CalendarSelectedDate;
    using snowdesktop::widget_runtime::lua_CalendarSetSelectedDate;
    using snowdesktop::widget_runtime::lua_CalendarSelectDate;
    using snowdesktop::widget_runtime::lua_CalendarPreferences;
    using snowdesktop::widget_runtime::lua_CalendarDisplayOptions;
    using snowdesktop::widget_runtime::lua_CalendarAnnotations;
    using snowdesktop::widget_runtime::lua_CalendarDateInfo;
    using snowdesktop::widget_runtime::lua_CalendarAddDays;
    using snowdesktop::widget_runtime::lua_CalendarEvents;
    using snowdesktop::widget_runtime::lua_CalendarSeriesById;
    using snowdesktop::widget_runtime::lua_CalendarCreate;
    using snowdesktop::widget_runtime::lua_CalendarUpdate;
    using snowdesktop::widget_runtime::lua_CalendarRemove;
    using snowdesktop::widget_runtime::lua_SlotsBinding;
    using snowdesktop::widget_runtime::lua_SlotsCollection;
    using snowdesktop::widget_runtime::lua_SlotsCanUndo;
    using snowdesktop::widget_runtime::lua_SlotsCanRedo;
    using snowdesktop::widget_runtime::lua_SlotsUndo;
    using snowdesktop::widget_runtime::lua_SlotsRedo;
    static constexpr CatalogFunctionDescriptor functions[] = {
#define SNOW_WIDGET_PUBLIC_FUNCTION(                                      \
    library, name, callback, sinceApi, permission, untilApi)              \
    { library, { name, callback, sinceApi, permission, untilApi } },
#define SNOW_WIDGET_MANIFEST_FUNCTION(                                    \
    library, name, callback, sinceApi, permission, untilApi)
#include "widget/api/widget_public_api.inc"
#undef SNOW_WIDGET_MANIFEST_FUNCTION
#undef SNOW_WIDGET_PUBLIC_FUNCTION
    };

    static constexpr snowdesktop::widget_runtime::WidgetDataLuaImages dataImages{
        +[](lua_State* state, std::string_view token,
            const std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>& pixels,
            const char* source, bool preserveExisting) {
            return RegisterRuntimeImageSource(GetD2D(state), BoundWidgetId(state),
                token, pixels, source, preserveExisting);
        },
        +[](lua_State* state, const snowdesktop::widget_runtime::WidgetScreenTimeDataSnapshot& screenTime) {
        auto* d2d = GetD2D(state);
        const auto owner = BoundWidgetId(state);
        if (d2d && !owner.empty())
        {
            const auto& apps = screenTime.apps;
            for (auto entry = d2d->runtimeImages.begin(); entry != d2d->runtimeImages.end();)
            {
                if (entry->second.source == "screen-time" &&
                    entry->second.ownerWidgetId == owner &&
                    std::none_of(apps.begin(), apps.end(), [&](const auto& app) {
                        return !app.iconToken.empty() &&
                            BindRuntimeImageToken(app.iconToken, owner) == entry->first;
                    }))
                {
                    EraseRuntimeImageBitmap(d2d, entry->first);
                    entry = d2d->runtimeImages.erase(entry);
                }
                else ++entry;
            }
        }
        },
    };
    snowdesktop::widget_runtime::BindWidgetDataLuaImages(L, dataImages);
    RegisterDataSubscriptionHandle(L);
    RegisterFunctionCatalog(L, functions,
        static_cast<std::uint32_t>(
            snowdesktop::widget::kHostApiVersion));
}
