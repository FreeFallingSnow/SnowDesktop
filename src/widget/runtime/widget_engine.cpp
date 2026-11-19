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
#include "widget_module_lua.h"
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
#include "widget/packages/widget_package_host.h"
#include "widget/packages/widget_package_paths.h"
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
#include "widget/view/widget_control_lua.h"
#include "widget/view/widget_layout_lua.h"
#include "widget/view/widget_view_renderer.h"
#include "widget/view/widget_view_text.h"
#include "widget/view/widget_background_cache.h"
#include "widget/view/widget_runtime_image_cache.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/view/widget_host_input_support.h"
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
using snowdesktop::widget_runtime::TrimWidgetPackageHistory;
using snowdesktop::widget_runtime::GetWidgetPackageManager;
using snowdesktop::widget_runtime::ResolveWidgetPath;
using snowdesktop::widget_runtime::RecoverWidgetPackage;
using snowdesktop::widget_runtime::ResolveWidgetEntryPackage;
using snowdesktop::widget::ResolvePackageAssetWithinRoot;
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
static std::string RegisterRuntimeImageSource(D2DState* state,
    const std::wstring& widgetId, std::string_view token,
    std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels> pixels,
    std::string source, bool preserveExisting = false)
{
    return state ? state->runtimeImageCache.Register(widgetId, token,
        std::move(pixels), std::move(source), preserveExisting) : std::string{};
}

static void ClearRuntimeImagesForWidget(D2DState* state, const std::wstring& widgetId)
{
    if (!state || widgetId.empty()) return;
    state->backgroundCache.Erase(widgetId);
    state->textLayoutCache.Erase(widgetId);
    state->runtimeImageCache.RemoveWidget(widgetId);
}

static void ClearRuntimeImagesForSource(D2DState* state, std::string_view source)
{
    if (!state || source.empty()) return;
    state->backgroundCache.Clear();
    state->runtimeImageCache.RemoveSource(source);
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
constexpr char kClipboardReadPermission[] = "clipboard.read";
constexpr char kClipboardWritePermission[] = "clipboard.write";
constexpr char kFilesystemWatchPermission[] =
    "filesystem.userSelected.watch";

using snowdesktop::widget_runtime::IsFilesystemHandleTask;
using snowdesktop::widget_runtime::IsFilesystemPickerTask;
using snowdesktop::widget_runtime::IsPreviewFilesystemHandle;
using snowdesktop::widget_runtime::kFilesystemReadPermission;
using snowdesktop::widget_runtime::kFilesystemWritePermission;

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
        return d2d && d2d->runtimeImageCache.GetPixels(BoundWidgetId(state), token) != nullptr;
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

std::string WidgetEngine::RegisterTaskImage(const std::wstring& widgetId,
    std::string_view token,
    std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels> pixels,
    std::string source)
{
    return RegisterRuntimeImageSource(d2dState_, widgetId, token, std::move(pixels), std::move(source));
}

std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>
WidgetEngine::ResolveTaskImage(const std::wstring& widgetId, const std::string& token) const
{
    return d2dState_ ? d2dState_->runtimeImageCache.GetPixels(widgetId, token) : nullptr;
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
    d2dState_->runtimeImageCache.ClearBitmaps();
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
    ResetFocusedHostInput();
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
    taskResults_.Clear();
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
    ResetHostInputForWidget(widgetId);
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
    CancelHostInputPointerState(surface);
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
    if (binding->options.preview)
    {
        LuaWidgetPreviewDataState previewState =
            LuaWidgetPreviewDataState::Ready;
        const int previewWidgetIndex = FindWidget(
            Utf8ToWideLocal(binding->instanceId));
        if (previewWidgetIndex >= 0)
            previewState = widgets_[previewWidgetIndex].previewDataState;
        return snowdesktop::widget_runtime::BuildPreviewDataSnapshot(*binding, previewState,
            [this](bool selection) {
                return selection ? RuntimeDesktopSelection() : RuntimeDesktopItems();
            });
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

    const auto timestampNow = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!snowdesktop::widget_runtime::PopulatePlatformDataSnapshot(
            result, *binding, *widgetSystemDataProvider_, widgetAudioAnalysisProvider_.get(),
            includeGpuDetails, timestampNow, [this, &binding]()
                -> std::optional<snowdesktop::widget_runtime::WidgetDisplayPixelRectDataSnapshot> {
                const int index = FindWidget(Utf8ToWideLocal(binding->instanceId));
                if (index < 0) return std::nullopt;
                const RECT monitor = widgets_[index].surfaceContext.monitorBounds;
                return snowdesktop::widget_runtime::WidgetDisplayPixelRectDataSnapshot{
                    monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top };
            }))
    {
        const auto setFreshness = [&](std::int64_t timestamp) {
            result.timestampMs = timestamp;
            result.stale = snowdesktop::widget_runtime::IsDataSnapshotStale(
                timestampNow, timestamp, binding->options.requestedInterval);
        };
        if (result.topic == "desktop.items" ||
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

void WidgetEngine::CloseWidgetPanelSurface(
    const std::wstring& widgetId, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    const std::string closingSurface = IsPanelSurface(surface)
        ? NormalizeWidgetSurface(surface)
        : NormalizeWidgetSurface(widget.panelSurface);
    CloseHostInputPanel(widgetId);
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
        HasFocusedHostInputFor(widgetId);
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
        snowdesktop::performance::Scope scope("profiler", "sample.widget.resources");
        auto imageUsage = d2dState_->runtimeImageCache.Measure();
        for (const auto& widget : widgets_)
        {
            imageUsage.owners.try_emplace(widget.widgetId);
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
        for (const auto& [id, usage] : imageUsage.owners)
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
        shared("runtime_image_unique_bytes", imageUsage.uniquePixelBytes);
        shared("runtime_bitmap_bgra_bytes_estimate", imageUsage.bitmapBytes);
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
    using snowdesktop::widget_runtime::lua_ModuleRequire;
    using snowdesktop::widget_runtime::lua_UiTextInput;
    using snowdesktop::widget_runtime::lua_UiTextArea;
    using snowdesktop::widget_runtime::lua_UiFocusInput;
    using snowdesktop::widget_runtime::lua_ControlTextInput;
    using snowdesktop::widget_runtime::lua_ControlTextArea;
    using snowdesktop::widget_runtime::lua_ControlFocus;
    using snowdesktop::widget_runtime::lua_ControlBlur;
    using snowdesktop::widget_runtime::lua_UiButton;
    using snowdesktop::widget_runtime::lua_UiToggle;
    using snowdesktop::widget_runtime::lua_UiProgress;
    using snowdesktop::widget_runtime::lua_UiScrollArea;
    using snowdesktop::widget_runtime::lua_UiVirtualList;
    using snowdesktop::widget_runtime::lua_UiSetScrollOffset;
    using snowdesktop::widget_runtime::lua_TaskStart;
    using snowdesktop::widget_runtime::lua_TaskCancel;
    using snowdesktop::widget_runtime::lua_HttpRequest;
    using snowdesktop::widget_runtime::lua_HttpCancel;
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
            if (!d2d || owner.empty()) return;
            std::vector<std::string_view> tokens;
            tokens.reserve(screenTime.apps.size());
            for (const auto& app : screenTime.apps)
                tokens.push_back(app.iconToken);
            d2d->runtimeImageCache.Retain(owner, "screen-time", tokens);
        },
    };
    snowdesktop::widget_runtime::BindWidgetDataLuaImages(L, dataImages);
    RegisterDataSubscriptionHandle(L);
    RegisterFunctionCatalog(L, functions,
        static_cast<std::uint32_t>(
            snowdesktop::widget::kHostApiVersion));
}
