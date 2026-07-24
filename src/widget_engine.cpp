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
#include "data_paths.h"
#include "deployment_context.h"
#include "json_value.h"
#include "l10n.h"
#include "system_snapshot.h"
#include "constants.h"
#include "utils.h"
#include "search_match.h"
#include "personalization.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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

static std::filesystem::path GetBundledWidgetsDir()
{
    return std::filesystem::path(GetExecutableDirectoryPath()) / L"widgets";
}

static bool CopyWidgetDirectoryContents(
    const std::filesystem::path& sourceDir,
    const std::filesystem::path& targetDir,
    std::filesystem::copy_options copyOptions)
{
    std::error_code error;
    std::filesystem::create_directories(targetDir, error);
    if (error)
        return false;

    error.clear();
    if (!std::filesystem::is_directory(sourceDir, error))
        return false;

    std::filesystem::recursive_directory_iterator entry(
        sourceDir,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    if (error)
        return false;

    while (entry != end)
    {
        const std::filesystem::file_status status =
            entry->symlink_status(error);
        if (error || std::filesystem::is_symlink(status))
            return false;

        const std::filesystem::path relative =
            entry->path().lexically_relative(sourceDir);
        if (relative.empty() || relative.native().starts_with(L".."))
            return false;
        const std::filesystem::path target = targetDir / relative;

        if (std::filesystem::is_directory(status))
        {
            error.clear();
            std::filesystem::create_directories(target, error);
            if (error)
                return false;
        }
        else if (std::filesystem::is_regular_file(status))
        {
            error.clear();
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
                return false;
            error.clear();
            std::filesystem::copy_file(
                entry->path(), target, copyOptions, error);
            if (error)
                return false;
        }
        else
        {
            return false;
        }

        error.clear();
        entry.increment(error);
        if (error)
            return false;
    }

    return true;
}

static void SeedPackagedWidgets(const std::filesystem::path& targetDir)
{
    CopyWidgetDirectoryContents(
        GetBundledWidgetsDir(),
        targetDir,
        std::filesystem::copy_options::skip_existing);
}

static std::filesystem::path GetPortableWidgetsDir()
{
    const std::filesystem::path widgetsDir = GetBundledWidgetsDir();
    const std::filesystem::path legacyDataWidgets =
        std::filesystem::path(GetDataDirectoryPath()) / L"widgets";

    std::error_code error;
    if (!std::filesystem::is_directory(legacyDataWidgets, error))
    {
        error.clear();
        std::filesystem::create_directories(widgetsDir, error);
        return widgetsDir;
    }

    // Earlier unified-data builds temporarily stored portable widgets under
    // data\widgets. Merge user edits back into the original portable widgets
    // directory, then remove the duplicate only after a complete copy.
    if (!CopyWidgetDirectoryContents(
            legacyDataWidgets,
            widgetsDir,
            std::filesystem::copy_options::overwrite_existing))
    {
        return legacyDataWidgets;
    }

    error.clear();
    std::filesystem::remove_all(legacyDataWidgets, error);
    return widgetsDir;
}

static const std::wstring& GetWidgetsDir()
{
    static const std::wstring path = [] {
        if (!snowdesktop::deployment::IsPackaged())
            return GetPortableWidgetsDir().wstring();

        const std::filesystem::path dataDir(
            GetDataSubdirectoryPath(L"widgets"));
        SeedPackagedWidgets(dataDir);
        return dataDir.wstring();
    }();
    return path;
}

static std::wstring ResolveWidgetPath(const std::wstring& scriptPath)
{
    std::wstring fullPath = GetWidgetsDir();
    fullPath += L"\\";
    fullPath += scriptPath;
    return fullPath;
}

static std::wstring ManifestPathForScriptFile(const std::wstring& fullScriptPath)
{
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
static std::deque<WidgetLogEntry> g_widgetLogs;

static bool IsRemovedPanelEffectSettingKey(const std::string& key)
{
    const size_t separator = key.rfind('.');
    const std::string setting = separator == std::string::npos
        ? key : key.substr(separator + 1);
    return setting == "shadowAlpha" || setting == "shadowBlur" ||
        setting == "shadowOffsetY" || setting == "highlightAlpha" ||
        setting == "noiseAlpha";
}

static void LoadStorageFile()
{
    g_storage.clear();
    if (g_storagePath.empty()) return;
    std::ifstream file(g_storagePath, std::ios::binary);
    if (!file) return;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    size_t pos = text.find('{');
    if (pos == std::string::npos) return;
    while (true)
    {
        pos = text.find('"', pos);
        if (pos == std::string::npos) break;
        size_t keyEnd = text.find('"', pos + 1);
        if (keyEnd == std::string::npos) break;
        std::string key = text.substr(pos + 1, keyEnd - pos - 1);
        pos = text.find(':', keyEnd + 1);
        if (pos == std::string::npos) break;
        pos = text.find('"', pos + 1);
        if (pos == std::string::npos) break;
        size_t valEnd = text.find('"', pos + 1);
        if (valEnd == std::string::npos) break;
        if (!IsRemovedPanelEffectSettingKey(key))
            g_storage[key] = text.substr(pos + 1, valEnd - pos - 1);
        pos = valEnd + 1;
    }
}

static void SaveStorageFile()
{
    if (g_storagePath.empty()) return;
    std::ofstream file(g_storagePath, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file << "{";
    bool first = true;
    for (const auto& kv : g_storage)
    {
        if (!first) file << ",";
        file << "\n  \"" << kv.first << "\": \"" << kv.second << "\"";
        first = false;
    }
    if (!g_storage.empty()) file << "\n";
    file << "}\n";
}

static bool EndsWithLastError(const std::string& key)
{
    constexpr const char* suffix = ".lastError";
    constexpr size_t suffixLen = 10;
    return key.size() >= suffixLen && key.compare(key.size() - suffixLen, suffixLen, suffix) == 0;
}

// ── Drawing API ──────────────────────────────────────────────────
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
    float itemFontSizeScale = 1.0f;
    int widgetClipDepth = 0;
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap1>> imageCache;
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
    bool verticallyCentered = false)
{
    if (!state || !state->dwrite) return nullptr;
    const auto sizeKey = static_cast<std::uint64_t>(std::clamp(
        static_cast<int>(std::lround(size * 100.0f)), 1, 0xFFFFFF));
    const std::uint64_t key = sizeKey |
        (static_cast<std::uint64_t>(weight) << 24) |
        (static_cast<std::uint64_t>(centered) << 36) |
        (static_cast<std::uint64_t>(wrapping) << 37) |
        (static_cast<std::uint64_t>(fontAwesome) << 40) |
        (static_cast<std::uint64_t>(verticallyCentered) << 41);
    if (auto found = state->textFormatCache.find(key); found != state->textFormatCache.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    if (fontAwesome)
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

static void SetWidgetExecutionContext(D2DState* state, const std::wstring& widgetId)
{
    if (!state) return;
    state->currentWidgetId = widgetId;
    state->storagePrefix = WidgetWideToUtf8(widgetId);
}

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

    ID2D1SolidColorBrush* brush = GetCachedBrush(s, color);
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
        s->engine->RuntimeNotify(Utf8ToWideLocal(title), Utf8ToWideLocal(message));
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
    auto catalog = manifest.locales.find(language);
    if (catalog != manifest.locales.end())
        return &catalog->second;
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
        ? s->engine->RuntimeGetCpuSnapshot(s->currentWidgetId) : CpuSnapshot{};
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
        ? s->engine->RuntimeGetMemorySnapshot(s->currentWidgetId) : MemorySnapshot{};
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
        ? s->engine->RuntimeGetBatterySnapshot(s->currentWidgetId) : BatterySnapshot{};
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
        ? s->engine->RuntimeGetNetworkSnapshot(s->currentWidgetId) : NetworkSnapshot{};
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
        ? s->engine->RuntimeGetGpuSnapshot(s->currentWidgetId) : GpuSnapshot{};
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
        ? s->engine->RuntimeGetMediaSnapshot(s->currentWidgetId) : MediaSnapshot{};
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
        s->engine->RuntimeSetTimer(s->currentWidgetId, name ? name : "", intervalMs, repeat));
    return 1;
}

static int lua_WidgetCancelTimer(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine &&
        s->engine->RuntimeCancelTimer(s->currentWidgetId, name ? name : ""));
    return 1;
}

static int lua_HttpRequest(lua_State* L)
{
    if (!RequirePermission(L, "network.http")) return 0;
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* s = GetD2D(L);
    if (!s || !s->engine) { lua_pushnil(L); return 1; }

    HttpRequestOptions options;
    options.widgetId = s->currentWidgetId;
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

    int id = s->engine->RuntimeHttpRequest(s->currentWidgetId, std::move(options));
    if (id > 0) lua_pushinteger(L, id); else lua_pushnil(L);
    return 1;
}

static int lua_HttpCancel(lua_State* L)
{
    if (!RequirePermission(L, "network.http")) return 0;
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    auto* s = GetD2D(L);
    lua_pushboolean(L, s && s->engine && s->engine->RuntimeHttpCancel(s->currentWidgetId, id));
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

static void DrawHostTextPixels(D2DState* state, const std::wstring& text,
    float x, float y, float width, float height, float size, int color)
{
    if (!state || !state->ctx || !state->dwrite || text.empty()) return;
    IDWriteTextFormat* format = GetCachedTextFormat(state, std::max(9.0f, size),
        DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
    ID2D1SolidColorBrush* brush = GetCachedBrush(state, color);
    if (!format || !brush) return;
    state->ctx->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
        D2D1::RectF(state->widgetRect.left + x, state->widgetRect.top + y,
            state->widgetRect.left + x + width, state->widgetRect.top + y + height),
        brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static float MeasureHostTextWidthPixels(D2DState* state, const std::wstring& text,
    float size, float height)
{
    if (!state || !state->dwrite || text.empty()) return 0.0f;
    IDWriteTextFormat* format = GetCachedTextFormat(state, std::max(9.0f, size),
        DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_WORD_WRAPPING_NO_WRAP, false, true);
    if (!format) return 0.0f;

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(state->dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
        format, 100000.0f, std::max(1.0f, height), &layout)) || !layout)
        return 0.0f;

    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics))
        ? metrics.widthIncludingTrailingWhitespace : 0.0f;
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
    const float fontSize = std::clamp(static_cast<float>(numberOption("fontSize", 14.0)), 9.0f, 96.0f);
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
        value = s->engine->RuntimeGetStorageValue(s->currentWidgetId, storageKey);

    bool focused = false;
    bool focusedSelectAll = false;
    size_t cursor = 0;
    std::wstring focusedText;
    if (s && s->engine && id && *id)
    {
        focused = s->engine->RuntimeGetFocusedHostInput(
            s->currentWidgetId, id, focusedText, cursor, focusedSelectAll);
        if (focused)
            value = WidgetWideToUtf8(focusedText);
    }

    DrawHostRect(s, x, y, width, height, backgroundColor, radius,
        focused ? focusedBackgroundAlpha : backgroundAlpha);
    DrawHostStrokeRect(s, x, y, width, height,
        focused ? focusedBorderColor : borderColor, radius, borderThickness,
        focused ? focusedBorderAlpha : borderAlpha);
    const bool showingPlaceholder = value.empty() && !focused;
    const std::wstring displayText = Utf8ToWideLocal(showingPlaceholder ? placeholder : value);
    if (focused && focusedSelectAll && !focusedText.empty())
    {
        const float selectionWidth = std::min(std::max(1.0f, width - padding * 2.0f),
            MeasureHostTextWidthPixels(s, focusedText, fontSize, height));
        const float selectionHeight = std::min(height, std::max(fontSize * 1.35f, 12.0f));
        DrawHostRect(s, x + padding, y + (height - selectionHeight) * 0.5f,
            selectionWidth, selectionHeight, focusedBorderColor, 2.0f, 0.35f);
    }
    DrawHostTextPixels(s, displayText,
        x + padding, y, std::max(1.0f, width - padding * 2.0f), height,
        fontSize, showingPlaceholder ? placeholderColor : textColor);

    if (focused)
    {
        const size_t safeCursor = std::min(cursor, focusedText.size());
        const std::wstring beforeCursor = focusedText.substr(0, safeCursor);
        const float cursorOffset = MeasureHostTextWidthPixels(s, beforeCursor, fontSize, height);
        const float cursorX = std::clamp(x + padding + cursorOffset + 1.0f,
            x + padding, x + std::max(padding, width - padding - 1.5f));
        const float cursorHeight = std::min(height, std::max(fontSize, 12.0f));
        DrawHostRect(s, cursorX, y + (height - cursorHeight) * 0.5f,
            1.5f, cursorHeight, textColor, 0.0f, 0.98f);
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
        s->engine->RuntimeRegisterHostControl(s->currentWidgetId, std::move(control));
    }

    lua_pushlstring(L, value.data(), value.size());
    return 1;
}

static int lua_UiFocusInput(lua_State* L)
{
    const char* id = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    bool focused = s && s->engine && id && *id &&
        s->engine->RuntimeFocusHostInput(s->currentWidgetId, id);
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
        s->engine->RuntimeRegisterHostControl(s->currentWidgetId, std::move(control));
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
        s->engine->RuntimeRegisterHostControl(s->currentWidgetId, std::move(control));
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
        s->engine->RuntimeRegisterHostControl(s->currentWidgetId, std::move(control));
    }
    int offset = s && s->engine ? s->engine->RuntimeGetScrollOffset(s->currentWidgetId, id) : 0;
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
        s->engine->RuntimeRegisterHostControl(s->currentWidgetId, std::move(control));
    }
    int offset = s && s->engine ? s->engine->RuntimeGetScrollOffset(s->currentWidgetId, id) : 0;
    int first = count == 0 ? 0 : offset / itemHeight + 1;
    int last = count == 0 ? 0 : std::min(count,
        (offset + viewportHeight + itemHeight - 1) / itemHeight);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, first); lua_setfield(L, -2, "first");
    lua_pushinteger(L, last); lua_setfield(L, -2, "last");
    lua_pushinteger(L, offset); lua_setfield(L, -2, "offset");
    return 1;
}

static bool RequirePermission(lua_State* L, const char* permission)
{
    auto* s = GetD2D(L);
    if (!s || !s->engine) return false;
    if (s->engine->RuntimeHasPermission(s->currentWidgetId, permission))
        return true;
    std::string msg = std::string("Permission denied: ") + permission;
    s->engine->RuntimeRecordError(s->currentWidgetId, msg);
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
    lua_pushstring(L, WidgetWideToUtf8(s->currentWidgetId).c_str()); lua_setfield(L, -2, "id");
    lua_pushnumber(L, s->widgetRect.right - s->widgetRect.left); lua_setfield(L, -2, "width");
    lua_pushnumber(L, s->widgetRect.bottom - s->widgetRect.top); lua_setfield(L, -2, "height");
    return 1;
}

static int lua_WidgetSetTitle(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeSetWidgetTitle(s->currentWidgetId, Utf8ToWideLocal(title ? title : ""));
    return 0;
}

static int lua_WidgetOpenSettings(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetSettings(s->currentWidgetId);
    return 0;
}

static int lua_WidgetInvalidate(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeInvalidateHost(s->currentWidgetId);
    return 0;
}

static int lua_WidgetLog(lua_State* L)
{
    const char* level = luaL_optstring(L, 1, "info");
    const char* message = luaL_optstring(L, 2, "");
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeAddLog(s->currentWidgetId, level ? level : "info", message ? message : "");
    return 0;
}

static int lua_WidgetTheme(lua_State* L)
{
    auto* s = GetD2D(L);
    LuaWidgetTheme theme;
    if (s && s->engine)
        theme = s->engine->RuntimeGetWidgetTheme(s->currentWidgetId);
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
        initial = s->engine->RuntimeGetStorageValue(s->currentWidgetId, key);

    LuaInlineTextEditRequest request;
    request.widgetId = s->currentWidgetId;
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

    auto* s = GetD2D(L);
    std::vector<LuaDesktopItemInfo> items = s->engine->RuntimeDesktopItems();
    auto rankItem = [&](const LuaDesktopItemInfo& item) {
        return NameSearchMatchRank(Utf8ToWideLocal(item.title), query);
    };
    if (!query.empty())
    {
        std::stable_sort(items.begin(), items.end(),
            [&](const LuaDesktopItemInfo& a, const LuaDesktopItemInfo& b) {
                return rankItem(a) < rankItem(b);
            });
    }
    lua_newtable(L);
    int i = 1;
    for (const auto& item : items)
    {
        if (query.empty() || NameMatchesQuery(Utf8ToWideLocal(item.title), query))
        {
            PushDesktopItem(L, item);
            lua_rawseti(L, -2, i++);
        }
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

static ID2D1Bitmap1* LoadImageBitmap(D2DState* s, const std::wstring& path)
{
    if (!s || !s->ctx || path.empty()) return nullptr;
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
    if (path.empty() || !PathIsRelativeW(path.c_str()))
        return 0;
    std::wstring fullPath = GetWidgetsDir();
    fullPath += L"\\";
    fullPath += path;
    ID2D1Bitmap1* bmp = LoadImageBitmap(s, fullPath);
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

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return 0;
    SIZE bitmapSize{};
    HBITMAP hbm = GetHighResolutionShellIconBitmap(pidl, 0, bitmapSize);
    CoTaskMemFree(pidl);
    if (!hbm) return 0;
    ComPtr<ID2D1Bitmap1> bmp = BitmapFromHBitmap(s->ctx, hbm);
    DeleteObject(hbm);
    if (!bmp) return 0;
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + size, y + s->widgetRect.top + size);
    s->ctx->DrawBitmap(bmp.Get(), dst, alpha, D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

// ── WidgetEngine ──────────────────────────────────────────────────
WidgetEngine::~WidgetEngine()
{
    Shutdown();
}

bool WidgetEngine::Init(ID2D1DeviceContext* d2dContext, IDWriteFactory* dwriteFactory)
{
    d2dContext_ = d2dContext;
    dwriteFactory_ = dwriteFactory;

    L_ = luaL_newstate();
    if (!L_) return false;

    luaL_requiref(L_, "_G", luaopen_base, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L_, 1);
    luaL_requiref(L_, LUA_UTF8LIBNAME, luaopen_utf8, 1); lua_pop(L_, 1);
    RegisterDrawAPI(L_);

    // Allocate D2D state
    d2dState_ = new D2DState{};
    d2dState_->dwrite = dwriteFactory_.Get();
    d2dState_->engine = this;
    lua_pushlightuserdata(L_, d2dState_);
    lua_setfield(L_, LUA_REGISTRYINDEX, "__d2d_ptr");

    // Initialize the writable widget library before loading layouts or menus.
    (void)GetWidgetsDir();

    // Init storage path
    g_storagePath = GetDataFilePath(L"SnowDesktop.storage.json");
    LoadStorageFile();
    systemSnapshotService_ = std::make_unique<SystemSnapshotService>();
    httpService_ = std::make_unique<AsyncHttpService>();
    return true;
}

void WidgetEngine::Shutdown()
{
    focusedHostInput_ = {};
    for (auto& widget : widgets_)
    {
        if (widget.valid && widget.hostVisible)
            InvokeSimpleCallback(widget, "onHidden");
        if (widget.refreshTimerId && widgetTimerKillCallback_)
            widgetTimerKillCallback_(widget.refreshTimerId);
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
    widgets_.clear();
    if (L_) { lua_close(L_); L_ = nullptr; }
    delete d2dState_; d2dState_ = nullptr;
}

void WidgetEngine::UnloadWidget(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    if (focusedHostInput_.active && focusedHostInput_.widgetId == widgetId)
        focusedHostInput_ = {};
    if (widgets_[idx].hostVisible)
        InvokeSimpleCallback(widgets_[idx], "onHidden");
    if (widgets_[idx].refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].refreshTimerId);
    if (httpService_) httpService_->CancelWidget(widgetId);
    luaL_unref(L_, LUA_REGISTRYINDEX, widgets_[idx].ref);
    widgets_.erase(widgets_.begin() + idx);
    std::erase_if(widgets_, [&widgetId](const LuaWidget& widget) {
        return widget.widgetId == widgetId;
    });

    // Remove storage data for this widget
    std::string prefix = WidgetWideToUtf8(widgetId) + ".";
    auto it = g_storage.begin();
    while (it != g_storage.end())
    {
        if (it->first.compare(0, prefix.size(), prefix) == 0)
            it = g_storage.erase(it);
        else
            ++it;
    }
    SaveStorageFile();
}

int WidgetEngine::FindWidget(const std::wstring& widgetId) const
{
    for (int i = 0; i < (int)widgets_.size(); ++i)
    {
        if (widgets_[i].valid && widgets_[i].widgetId == widgetId)
            return i;
    }
    return -1;
}

void WidgetEngine::InvokeSimpleCallback(LuaWidget& widget, const char* callbackName)
{
    SetWidgetExecutionContext(d2dState_, widget.widgetId);
    SetWidgetRectContext(d2dState_, widget.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, callbackName);
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* error = lua_tostring(L_, -1);
            RuntimeRecordError(widget.widgetId, error ? error : "(callback error)");
            lua_pop(L_, 1);
        }
    }
    else
        lua_pop(L_, 1);
    lua_pop(L_, 1);
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
    lua_getglobal(L, "string"); lua_setfield(L, -2, "string");
    lua_getglobal(L, "table");  lua_setfield(L, -2, "table");
    lua_getglobal(L, "math");   lua_setfield(L, -2, "math");
    lua_getglobal(L, "utf8");   lua_setfield(L, -2, "utf8");

    lua_getglobal(L, "draw");    lua_setfield(L, -2, "draw");
    lua_getglobal(L, "sys");     lua_setfield(L, -2, "sys");
    lua_getglobal(L, "layout");  lua_setfield(L, -2, "layout");
    lua_getglobal(L, "storage"); lua_setfield(L, -2, "storage");
    lua_getglobal(L, "widget");  lua_setfield(L, -2, "widget");
    lua_getglobal(L, "desktop"); lua_setfield(L, -2, "desktop");
    lua_getglobal(L, "media");   lua_setfield(L, -2, "media");
    lua_getglobal(L, "http");    lua_setfield(L, -2, "http");
    lua_getglobal(L, "ui");      lua_setfield(L, -2, "ui");
    PushWidgetL10nAPI(L, widget.manifest);
    lua_setfield(L, -2, "l10n");

    if (widget.permissions.contains("ui.input"))
    {
        lua_getglobal(L, "imgui");
        lua_setfield(L, -2, "imgui");
    }

    lua_pushstring(L, WidgetWideToUtf8(widget.widgetId).c_str());
    lua_setfield(L, -2, "widgetId");
}

bool WidgetEngine::EnsureWidgetLoaded(const std::wstring& widgetId, const std::wstring& scriptPath)
{
    if (FindWidget(widgetId) >= 0) return true;
    return LoadWidget(ResolveWidgetPath(scriptPath), widgetId);
}

bool WidgetEngine::LoadWidget(const std::wstring& path, const std::wstring& widgetId)
{
    std::string source = ReadTextFile(path);
    if (source.empty()) return false;

    LuaWidget pending;
    pending.widgetId = widgetId;
    pending.filePath = path;
    pending.manifest = GetWidgetManifest(PathFindFileNameW(path.c_str()));
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
    for (const auto& permission : pending.manifest.permissions)
        pending.permissions.insert(permission);
    SetWidgetExecutionContext(d2dState_, widgetId);

    // Create a sandbox table with only the registered safe API surface.
    PushSafeEnvironment(L_, pending);

    // Load the chunk
    if (luaL_loadstring(L_, source.c_str()) != LUA_OK)
    {
        const char* err = lua_tostring(L_, -1);
        RuntimeRecordError(widgetId, err ? err : "(load error)");
        lua_pop(L_, 2);
        return false;
    }

    // Try to set the chunk's first upvalue (_ENV) to sandbox.
    lua_pushvalue(L_, -2);
    const char* envName = lua_setupvalue(L_, -2, 1);
    if (envName == nullptr)
    {
        // Chunk has no _ENV upvalue - run directly in sandbox via alternative method
        // Pop the chunk, reload with explicit environment
        lua_pop(L_, 2);  // pop sandbox copy and chunk, keep sandbox
        // Wrap the source to use the sandbox explicitly
        std::string wrapped = "local _ENV = ...;\n" + source;
        if (luaL_loadstring(L_, wrapped.c_str()) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(load error)");
            lua_pop(L_, 2);
            return false;
        }
        lua_pushvalue(L_, -2);  // push sandbox as argument
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            lua_pop(L_, 2);
            return false;
        }
    }
    else
    {
        // Execute the chunk
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(pcall error)");
            lua_pop(L_, 2);
            return false;
        }
    }

    // sandbox now contains the script's globals (name, render, etc.)
    int ref = luaL_ref(L_, LUA_REGISTRYINDEX);

    std::string name = "Unnamed";
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    lua_getfield(L_, -1, "name");
    if (lua_isstring(L_, -1))
        name = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    // Read customStyle flag
    bool customStyle = false;
    lua_getfield(L_, -1, "useCustomStyle");
    if (!lua_isnil(L_, -1))
        customStyle = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);

    bool followPersonalizationDefault = false;
    lua_getfield(L_, -1, "followPersonalizationDefault");
    if (!lua_isnil(L_, -1))
        followPersonalizationDefault = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);

    std::vector<LuaWidgetManifest::Setting> scriptSettings;
    std::vector<LuaWidgetManifest::SettingPreset> scriptPresets;
    ReadLuaDeclaredSettings(L_, -1, scriptSettings, scriptPresets);

    lua_pop(L_, 1);  // pop table

    LuaWidget w;
    w.widgetId = widgetId;
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
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        w.lastModified = attr.ftLastWriteTime;
    widgets_.push_back(w);

    if (w.manifest.refreshIntervalMs > 0 && widgetTimerRequestCallback_)
    {
        UINT_PTR tid = widgetTimerRequestCallback_(widgetId,
            static_cast<UINT>(w.manifest.refreshIntervalMs));
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

    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    int ref = (idx >= 0) ? widgets_[idx].ref : LUA_NOREF;
    if (ref == LUA_NOREF) return true;

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
            std::vector<const char*> labels;
            for (const auto& option : setting.options) labels.push_back(option.c_str());
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

        std::vector<const char*> themeLabels;
        themeLabels.reserve(builtInThemeCount + 1 + presets.size());
        for (const std::string& label : builtInThemeLabels)
            themeLabels.push_back(label.c_str());
        themeLabels.push_back(_L("app.settings.custom"));
        for (const auto& preset : presets)
            themeLabels.push_back(preset.label.c_str());

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
        std::vector<const char*> presetLabels;
        for (const auto& preset : presets) presetLabels.push_back(preset.label.c_str());
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
        for (const auto& setting : settings)
            renderSetting(setting);
        const char* resetSettingsLabel = _L("app.settings.restore_default_settings");
        beginEditorRow(_L("app.settings.set_as_default"), editorButtonWidth(resetSettingsLabel));
        if (whiteTextButton(resetSettingsLabel))
            applyDefaultSettingValues();
        ImGui::Spacing();
    }

    flushStorageChanges();

    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    if (lua_istable(L_, -1))
    {
        // Inject widgetId global
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(L_, widUtf8.c_str());
        lua_setfield(L_, -2, "widgetId");

        lua_getfield(L_, -1, "imguiRender");
        if (lua_isfunction(L_, -1))
        {
            ImGui::Spacing();

            if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(L_, -1);
                RuntimeRecordError(widgetId, err ? err : "(imguiRender error)");
                lua_pop(L_, 1);
            }
        }
        else
            lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return true;
}

void WidgetEngine::RenderWidget(const std::wstring& widgetId, const std::wstring& scriptPath,
    ID2D1DeviceContext* context, RECT bounds, int columns, int rows)
{
    (void)scriptPath;

    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    LuaWidget* found = &widgets_[idx];

    // Hot-reload: check if file changed or deleted
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

    d2dState_->ctx = context;
    SetWidgetExecutionContext(d2dState_, widgetId);
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
        lua_rawgeti(L_, LUA_REGISTRYINDEX, found->ref);
        if (lua_istable(L_, -1))
        {
            lua_getfield(L_, -1, "onSizeChanged");
            if (lua_isfunction(L_, -1))
            {
                lua_pushinteger(L_, found->lastColumns);
                lua_pushinteger(L_, found->lastRows);
                if (lua_pcall(L_, 2, 0, 0) != LUA_OK)
                {
                    const char* error = lua_tostring(L_, -1);
                    RuntimeRecordError(widgetId, error ? error : "(onSizeChanged error)");
                    lua_pop(L_, 1);
                }
            }
            else
                lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
    }
    found->lastRenderTime = std::chrono::steady_clock::now();
    found->hostControls.clear();
    d2dState_->widgetClipDepth = 0;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, found->ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }

    // Inject widgetId global
    {
        int wlen = WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), nullptr, 0, nullptr, nullptr);
        std::string widUtf8(wlen, '\0');
        WideCharToMultiByte(CP_UTF8, 0, widgetId.c_str(), (int)widgetId.size(), &widUtf8[0], wlen, nullptr, nullptr);
        lua_pushstring(L_, widUtf8.c_str());
        lua_setfield(L_, -2, "widgetId");
    }

    lua_getfield(L_, -1, "render");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(render error)");
            lua_pop(L_, 1);
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
                    const float errFontSize = std::max(9.0f, 14.0f * CalculateWidgetCellScale(
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
            found->valid = false;
            lua_pop(L_, 1);
            return;
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    while (d2dState_->widgetClipDepth > 0)
    {
        d2dState_->ctx->PopAxisAlignedClip();
        --d2dState_->widgetClipDepth;
    }
    lua_pop(L_, 1);
}

void WidgetEngine::TickRuntime()
{
    const bool systemChanged = systemSnapshotChanged_.exchange(false);
    const bool mediaChanged = mediaSnapshotChanged_.exchange(false);
    if (systemChanged || mediaChanged)
    {
        std::unordered_set<std::wstring> dirtyWidgets;
        for (const auto& widget : widgets_)
        {
            if (!widget.valid) continue;
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
            SetWidgetExecutionContext(d2dState_, widget.widgetId);
            lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
            if (lua_istable(L_, -1))
            {
                lua_getfield(L_, -1, "onHttpResponse");
                if (lua_isfunction(L_, -1))
                {
                    lua_pushinteger(L_, response.id);
                    lua_createtable(L_, 0, 5);
                    lua_pushinteger(L_, response.status); lua_setfield(L_, -2, "status");
                    lua_pushlstring(L_, response.body.data(), response.body.size()); lua_setfield(L_, -2, "body");
                    lua_pushstring(L_, response.error.c_str()); lua_setfield(L_, -2, "error");
                    lua_pushboolean(L_, response.fromCache); lua_setfield(L_, -2, "fromCache");
                    lua_pushboolean(L_, response.error.empty() &&
                        response.status >= 200 && response.status < 300);
                    lua_setfield(L_, -2, "ok");
                    if (lua_pcall(L_, 2, 0, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(L_, -1);
                        RuntimeRecordError(widget.widgetId, error ? error : "(onHttpResponse error)");
                        lua_pop(L_, 1);
                    }
                    RuntimeInvalidateHost(widget.widgetId);
                }
                else
                    lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto& widget : widgets_)
    {
        if (!widget.valid) continue;
        if (widget.hostVisible && widget.lastRenderTime.time_since_epoch().count() > 0 &&
            now - widget.lastRenderTime > std::chrono::milliseconds(2500))
        {
            widget.hostVisible = false;
            InvokeSimpleCallback(widget, "onHidden");
        }

        std::vector<std::string> dueNames;
        for (const auto& [name, timer] : widget.timers)
            if (now >= timer.due) dueNames.push_back(name);
        for (const auto& name : dueNames)
        {
            auto it = widget.timers.find(name);
            if (it == widget.timers.end()) continue;
            const auto timer = it->second;
            if (timer.repeat)
            {
                auto nextDue = timer.due + std::chrono::milliseconds(timer.intervalMs);
                while (nextDue <= now)
                    nextDue += std::chrono::milliseconds(timer.intervalMs);
                it->second.due = nextDue;
            }
            else
                widget.timers.erase(it);

            SetWidgetExecutionContext(d2dState_, widget.widgetId);
            lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
            if (lua_istable(L_, -1))
            {
                lua_getfield(L_, -1, "onTimer");
                if (lua_isfunction(L_, -1))
                {
                    lua_pushstring(L_, name.c_str());
                    if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(L_, -1);
                        RuntimeRecordError(widget.widgetId, error ? error : "(onTimer error)");
                        lua_pop(L_, 1);
                    }
                    RuntimeInvalidateHost(widget.widgetId);
                }
                else
                    lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
        }
    }
}

void WidgetEngine::OnWidgetTimer(const std::wstring& widgetId)
{
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& widget = widgets_[idx];
    SetWidgetExecutionContext(d2dState_, widget.widgetId);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
    if (lua_istable(L_, -1))
    {
        lua_getfield(L_, -1, "onTimer");
        if (lua_isfunction(L_, -1))
        {
            lua_pushstring(L_, "refresh");
            if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
            {
                const char* error = lua_tostring(L_, -1);
                RuntimeRecordError(widget.widgetId, error ? error : "(onTimer refresh error)");
                lua_pop(L_, 1);
            }
        }
        else
            lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    RuntimeInvalidateHost(widget.widgetId);
}

// ── Check if widget uses custom style ────────────────────────────
bool WidgetEngine::HasCustomStyle(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    return idx >= 0 && widgets_[idx].customStyle;
}

void WidgetEngine::InvokeOpen(const std::wstring& widgetId)
{
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, "onOpen");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(onOpen error)");
            lua_pop(L_, 1);
        }
    }
    else
        lua_pop(L_, 1);
    lua_pop(L_, 1);
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
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, callbackName);
    if (lua_isfunction(L_, -1))
    {
        lua_pushinteger(L_, x);
        lua_pushinteger(L_, y);
        lua_pushinteger(L_, button);
        lua_pushinteger(L_, delta);
        if (lua_pcall(L_, 4, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(mouse callback error)");
            lua_pop(L_, 1);
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

std::vector<LuaWidgetMenuItem> WidgetEngine::GetContextMenu(const std::wstring& widgetId)
{
    std::vector<LuaWidgetMenuItem> result;
    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return result;
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return result;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return result; }
    lua_getfield(L_, -1, "getContextMenu");
    if (lua_isfunction(L_, -1))
    {
        if (lua_pcall(L_, 0, 1, 0) == LUA_OK && lua_istable(L_, -1))
        {
            int count = static_cast<int>(lua_rawlen(L_, -1));
            for (int i = 1; i <= count; ++i)
            {
                lua_rawgeti(L_, -1, i);
                if (lua_istable(L_, -1))
                {
                    LuaWidgetMenuItem item;
                    lua_getfield(L_, -1, "id");
                    item.id = lua_isinteger(L_, -1) ? static_cast<int>(lua_tointeger(L_, -1)) : i;
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "label");
                    item.label = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "icon");
                    item.icon = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "enabled");
                    item.enabled = lua_isnil(L_, -1) ? true : (lua_toboolean(L_, -1) != 0);
                    lua_pop(L_, 1);
                    lua_getfield(L_, -1, "separator");
                    item.separator = lua_toboolean(L_, -1) != 0;
                    lua_pop(L_, 1);
                    if (item.separator || !item.label.empty())
                        result.push_back(std::move(item));
                }
                lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
        }
        else
        {
            const char* err = lua_tostring(L_, -1);
            if (err)
                RuntimeRecordError(widgetId, err);
            lua_pop(L_, 1);
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return result;
}

void WidgetEngine::InvokeMenu(const std::wstring& widgetId, int menuId)
{
    if (!RuntimeHasPermission(widgetId, "ui.contextMenu"))
        return;
    SetWidgetExecutionContext(d2dState_, widgetId);
    int idx = FindWidget(widgetId);
    if (idx < 0) return;
    auto& w = widgets_[idx];
    SetWidgetRectContext(d2dState_, w.lastBounds);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return; }
    lua_getfield(L_, -1, "onMenu");
    if (lua_isfunction(L_, -1))
    {
        lua_pushinteger(L_, menuId);
        if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
        {
            const char* err = lua_tostring(L_, -1);
            RuntimeRecordError(widgetId, err ? err : "(onMenu error)");
            lua_pop(L_, 1);
        }
    }
    else
    {
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

void WidgetEngine::NotifyDesktopChanged(const std::string& reason)
{
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || !RuntimeHasPermission(widget.widgetId, "desktop.read"))
            continue;
        SetWidgetExecutionContext(d2dState_, widget.widgetId);
        SetWidgetRectContext(d2dState_, widget.lastBounds);
        lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
        if (!lua_istable(L_, -1)) { lua_pop(L_, 1); continue; }
        lua_getfield(L_, -1, "onDesktopChanged");
        if (lua_isfunction(L_, -1))
        {
            lua_pushstring(L_, reason.c_str());
            if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
            {
                const char* err = lua_tostring(L_, -1);
                RuntimeRecordError(widget.widgetId, err ? err : "(onDesktopChanged error)");
                lua_pop(L_, 1);
            }
        }
        else
        {
            lua_pop(L_, 1);
        }
        lua_pop(L_, 1);
    }
}

bool WidgetEngine::ReadBoolFlag(const std::wstring& scriptPath, const char* flag, bool defaultVal) const
{
    for (const auto& w : widgets_)
    {
        if (w.valid && w.filePath.size() >= scriptPath.size() &&
            w.filePath.compare(w.filePath.size() - scriptPath.size(), scriptPath.size(), scriptPath) == 0)
        {
            lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
            if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return defaultVal; }
            lua_getfield(L_, -1, flag);
            bool result = lua_isnil(L_, -1) ? defaultVal : (lua_toboolean(L_, -1) != 0);
            lua_pop(L_, 2);
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

    lua_rawgeti(L_, LUA_REGISTRYINDEX, w.ref);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }

            auto readHex = [&](const char* key, float& r, float& g, float& b, int def) {
                lua_getfield(L_, -1, key);
                int val = lua_isinteger(L_, -1) ? (int)lua_tointeger(L_, -1) : def;
                lua_pop(L_, 1);
                r = ((val >> 16) & 0xFF) / 255.0f;
                g = ((val >> 8) & 0xFF) / 255.0f;
                b = (val & 0xFF) / 255.0f;
            };

            auto readFloat = [&](const char* key, float& out, float def) {
                lua_getfield(L_, -1, key);
                out = lua_isnumber(L_, -1) ? (float)lua_tonumber(L_, -1) : def;
                lua_pop(L_, 1);
            };

            auto readBool = [&](const char* key, bool& out, bool def) {
                lua_getfield(L_, -1, key);
                out = lua_isnil(L_, -1) ? def : (lua_toboolean(L_, -1) != 0);
                lua_pop(L_, 1);
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

            lua_pop(L_, 1);
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
    std::wstring path = widgets_[idx].filePath;
    if (widgets_[idx].hostVisible)
        InvokeSimpleCallback(widgets_[idx], "onHidden");
    if (widgets_[idx].refreshTimerId && widgetTimerKillCallback_)
        widgetTimerKillCallback_(widgets_[idx].refreshTimerId);
    if (httpService_) httpService_->CancelWidget(widgetId);
    luaL_unref(L_, LUA_REGISTRYINDEX, widgets_[idx].ref);
    widgets_.erase(widgets_.begin() + idx);
    std::erase_if(widgets_, [&widgetId](const LuaWidget& widget) {
        return widget.widgetId == widgetId;
    });
    return LoadWidget(path, widgetId);
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
    g_storage[idUtf8 + ".lastError"] = message;
    SaveStorageFile();
    RuntimeAddLog(widgetId, "error", message);
}

void WidgetEngine::RuntimeAddLog(const std::wstring& widgetId, const std::string& level, const std::string& message)
{
    WidgetLogEntry entry;
    entry.key = WidgetWideToUtf8(widgetId);
    entry.level = level.empty() ? "info" : level;
    entry.message = message;
    g_widgetLogs.push_back(std::move(entry));
    while (g_widgetLogs.size() > 200)
        g_widgetLogs.pop_front();
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopItems() const
{
    return desktopSnapshotProvider_ ? desktopSnapshotProvider_() : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeDesktopSelection() const
{
    return selectionProvider_ ? selectionProvider_() : std::vector<LuaDesktopItemInfo>{};
}

std::vector<LuaDesktopItemInfo> WidgetEngine::RuntimeEverythingSearch(const std::string& query, int maxResults) const
{
    return everythingSearchProvider_
        ? everythingSearchProvider_(query, maxResults)
        : std::vector<LuaDesktopItemInfo>{};
}

bool WidgetEngine::RuntimeOpenDesktopPath(const std::wstring& path)
{
    if (path.empty()) return false;
    return desktopOpenCallback_ ? desktopOpenCallback_(path) : false;
}

bool WidgetEngine::RuntimeRevealDesktopPath(const std::wstring& path)
{
    if (path.empty()) return false;
    return desktopRevealCallback_ ? desktopRevealCallback_(path) : false;
}

void WidgetEngine::RuntimeRefreshDesktop()
{
    if (desktopRefreshCallback_)
        desktopRefreshCallback_();
}

void WidgetEngine::RuntimeSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (setWidgetTitleCallback_)
        setWidgetTitleCallback_(widgetId, title);
}

void WidgetEngine::RuntimeInvalidateHost(const std::wstring& widgetId)
{
    if (invalidateCallback_)
        invalidateCallback_(widgetId);
}

std::string WidgetEngine::RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const
{
    std::string fullKey = WidgetWideToUtf8(widgetId) + "." + key;
    auto it = g_storage.find(fullKey);
    if (it != g_storage.end())
        return it->second;

    int idx = FindWidget(widgetId);
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
    g_storage[WidgetWideToUtf8(widgetId) + "." + key] = value;
    SaveStorageFile();
}

void WidgetEngine::ReloadStorage()
{
    LoadStorageFile();
}

void WidgetEngine::RuntimeBeginInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (inlineTextEditCallback_)
        inlineTextEditCallback_(request);
}

void WidgetEngine::RuntimeNotify(const std::wstring& title, const std::wstring& message)
{
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
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetCpu() : CpuSnapshot{};
}

MemorySnapshot WidgetEngine::RuntimeGetMemorySnapshot(const std::wstring& widgetId)
{
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetMemory() : MemorySnapshot{};
}

BatterySnapshot WidgetEngine::RuntimeGetBatterySnapshot(const std::wstring& widgetId)
{
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetBattery() : BatterySnapshot{};
}

NetworkSnapshot WidgetEngine::RuntimeGetNetworkSnapshot(const std::wstring& widgetId)
{
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetNetwork() : NetworkSnapshot{};
}

GpuSnapshot WidgetEngine::RuntimeGetGpuSnapshot(const std::wstring& widgetId)
{
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesSystemSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetGpu() : GpuSnapshot{};
}

MediaSnapshot WidgetEngine::RuntimeGetMediaSnapshot(const std::wstring& widgetId)
{
    EnsureSystemSnapshotServiceStarted();
    if (int index = FindWidget(widgetId); index >= 0)
        widgets_[index].usesMediaSnapshot = true;
    return systemSnapshotService_ ? systemSnapshotService_->GetMedia() : MediaSnapshot{};
}

bool WidgetEngine::RuntimeMediaPlayPause()
{
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaPlayPause();
}

bool WidgetEngine::RuntimeMediaNext()
{
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaNext();
}

bool WidgetEngine::RuntimeMediaPrevious()
{
    EnsureSystemSnapshotServiceStarted();
    return systemSnapshotService_ && systemSnapshotService_->RequestMediaPrevious();
}

bool WidgetEngine::RuntimeSetTimer(const std::wstring& widgetId, const std::string& name,
    int intervalMs, bool repeat)
{
    int index = FindWidget(widgetId);
    if (index < 0 || name.empty()) return false;
    intervalMs = std::clamp(intervalMs, 100, 86400000);
    LuaWidget::Timer timer;
    timer.name = name;
    timer.intervalMs = intervalMs;
    timer.repeat = repeat;
    timer.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs);
    widgets_[index].timers[name] = std::move(timer);
    return true;
}

bool WidgetEngine::RuntimeCancelTimer(const std::wstring& widgetId, const std::string& name)
{
    int index = FindWidget(widgetId);
    return index >= 0 && widgets_[index].timers.erase(name) > 0;
}

int WidgetEngine::RuntimeHttpRequest(const std::wstring& widgetId, HttpRequestOptions options)
{
    int index = FindWidget(widgetId);
    if (index < 0 || !httpService_) return 0;
    options.widgetId = widgetId;
    options.allowedDomains = widgets_[index].manifest.networkDomains;
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
    if (control.type == LuaWidget::HostControl::Type::Scroll)
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
    focusedHostInput_.selectAll = found->selectAll;
    focusedHostInput_.liveUpdate = found->liveUpdate;
    if (hostInputFocusCallback_)
        hostInputFocusCallback_();
    RuntimeInvalidateHost(widgetId);
    return true;
}

bool WidgetEngine::RuntimeGetFocusedHostInput(const std::wstring& widgetId,
    const std::string& id, std::wstring& text, size_t& cursor, bool& selectAll) const
{
    if (!focusedHostInput_.active || focusedHostInput_.widgetId != widgetId ||
        focusedHostInput_.id != id)
        return false;
    text = focusedHostInput_.text;
    cursor = focusedHostInput_.cursor;
    selectAll = focusedHostInput_.selectAll;
    return true;
}

bool WidgetEngine::HasFocusedHostInput() const
{
    return focusedHostInput_.active;
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

bool WidgetEngine::HandleHostInputChar(wchar_t ch)
{
    if (!focusedHostInput_.active || ch < 0x20 || ch == 0x7F)
        return false;

    if (focusedHostInput_.selectAll)
    {
        focusedHostInput_.text.clear();
        focusedHostInput_.cursor = 0;
        focusedHostInput_.selectAll = false;
    }
    focusedHostInput_.cursor = std::min(
        focusedHostInput_.cursor, focusedHostInput_.text.size());
    focusedHostInput_.text.insert(focusedHostInput_.cursor, 1, ch);
    ++focusedHostInput_.cursor;
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
    bool changed = false;
    if (key == VK_ESCAPE)
    {
        BlurHostInput(true);
        return true;
    }
    if (key == VK_RETURN)
    {
        BlurHostInput(false);
        return true;
    }
    if (ctrl && key == 'A')
    {
        focusedHostInput_.selectAll = !focusedHostInput_.text.empty();
        focusedHostInput_.cursor = focusedHostInput_.text.size();
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    if (ctrl && (key == 'C' || key == 'X'))
    {
        if (focusedHostInput_.selectAll && !focusedHostInput_.text.empty() && OpenClipboard(nullptr))
        {
            EmptyClipboard();
            const SIZE_T bytes = (focusedHostInput_.text.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory)
            {
                if (void* target = GlobalLock(memory))
                {
                    memcpy(target, focusedHostInput_.text.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (!SetClipboardData(CF_UNICODETEXT, memory))
                        GlobalFree(memory);
                }
                else
                    GlobalFree(memory);
            }
            CloseClipboard();
            if (key == 'X')
            {
                focusedHostInput_.text.clear();
                focusedHostInput_.cursor = 0;
                focusedHostInput_.selectAll = false;
                changed = true;
            }
        }
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
                        pasted.push_back((*source == L'\n' || *source == L'\t') ? L' ' : *source);
                    }
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!pasted.empty())
        {
            if (focusedHostInput_.selectAll)
            {
                focusedHostInput_.text.clear();
                focusedHostInput_.cursor = 0;
                focusedHostInput_.selectAll = false;
            }
            focusedHostInput_.cursor = std::min(
                focusedHostInput_.cursor, focusedHostInput_.text.size());
            focusedHostInput_.text.insert(focusedHostInput_.cursor, pasted);
            focusedHostInput_.cursor += pasted.size();
            changed = true;
        }
    }
    else if (key == VK_BACK)
    {
        if (focusedHostInput_.selectAll)
        {
            focusedHostInput_.text.clear();
            focusedHostInput_.cursor = 0;
            focusedHostInput_.selectAll = false;
            changed = true;
        }
        else if (focusedHostInput_.cursor > 0)
        {
            focusedHostInput_.text.erase(--focusedHostInput_.cursor, 1);
            changed = true;
        }
    }
    else if (key == VK_DELETE)
    {
        if (focusedHostInput_.selectAll)
        {
            focusedHostInput_.text.clear();
            focusedHostInput_.cursor = 0;
            focusedHostInput_.selectAll = false;
            changed = true;
        }
        else if (focusedHostInput_.cursor < focusedHostInput_.text.size())
        {
            focusedHostInput_.text.erase(focusedHostInput_.cursor, 1);
            changed = true;
        }
    }
    else if (key == VK_LEFT || key == VK_HOME)
    {
        focusedHostInput_.cursor = focusedHostInput_.selectAll || key == VK_HOME
            ? 0 : (focusedHostInput_.cursor > 0 ? focusedHostInput_.cursor - 1 : 0);
        focusedHostInput_.selectAll = false;
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
    }
    else if (key == VK_RIGHT || key == VK_END)
    {
        focusedHostInput_.cursor = focusedHostInput_.selectAll || key == VK_END
            ? focusedHostInput_.text.size()
            : std::min(focusedHostInput_.cursor + 1, focusedHostInput_.text.size());
        focusedHostInput_.selectAll = false;
        RuntimeInvalidateHost(focusedHostInput_.widgetId);
        return true;
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

std::vector<LuaWidget::HostControl> WidgetEngine::GetScrollControls(const std::wstring& widgetId) const
{
    int index = FindWidget(widgetId);
    if (index < 0) return {};
    std::vector<LuaWidget::HostControl> results;
    for (const auto& ctrl : widgets_[index].hostControls)
    {
        if (ctrl.type == LuaWidget::HostControl::Type::Scroll)
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
        if (wheel && it->type == LuaWidget::HostControl::Type::Scroll)
        {
            int maximum = std::max(0, it->contentHeight - it->viewportHeight);
            int& offset = widget.scrollOffsets[it->id];
            offset = std::clamp(offset - delta / WHEEL_DELTA * 48, 0, maximum);
            RuntimeInvalidateHost(widgetId);
            return true;
        }
        if (wheel) continue;
        if (it->type == LuaWidget::HostControl::Type::Input)
        {
            RuntimeFocusHostInput(widgetId, it->id);
            return true;
        }
        if (it->type == LuaWidget::HostControl::Type::Button ||
            it->type == LuaWidget::HostControl::Type::Toggle)
        {
            SetWidgetExecutionContext(d2dState_, widgetId);
            lua_rawgeti(L_, LUA_REGISTRYINDEX, widget.ref);
            if (lua_istable(L_, -1))
            {
                lua_getfield(L_, -1, "onUiAction");
                if (lua_isfunction(L_, -1))
                {
                    lua_pushstring(L_, it->id.c_str());
                    lua_pushboolean(L_, it->type == LuaWidget::HostControl::Type::Toggle ? !it->value : true);
                    if (lua_pcall(L_, 2, 0, 0) != LUA_OK)
                    {
                        const char* error = lua_tostring(L_, -1);
                        RuntimeRecordError(widgetId, error ? error : "(onUiAction error)");
                        lua_pop(L_, 1);
                    }
                }
                else
                    lua_pop(L_, 1);
            }
            lua_pop(L_, 1);
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

void WidgetEngine::SetGridCellSize(int cellWidth, int cellHeight)
{
    if (d2dState_)
    {
        d2dState_->gridCellW = std::max(4, cellWidth);
        d2dState_->gridCellH = std::max(4, cellHeight);
    }
}

void WidgetEngine::SetGridCellGap(int gapY)
{
    if (d2dState_)
        d2dState_->gridGapY = std::max(0, gapY);
}

void WidgetEngine::SetBarHeight(int barHeight)
{
    if (d2dState_)
        d2dState_->barHeight = barHeight;
}

void WidgetEngine::SetItemFontWeight(DWRITE_FONT_WEIGHT weight)
{
    if (!d2dState_) return;
    if (d2dState_->itemFontWeight == weight) return;
    d2dState_->itemFontWeight = weight;
    d2dState_->textFormatCache.clear();
}

void WidgetEngine::SetItemFontSizeScale(float scale)
{
    if (!d2dState_) return;
    d2dState_->itemFontSizeScale = std::max(0.5f, scale);
}

void WidgetEngine::RuntimeOpenWidgetSettings(const std::wstring& widgetId)
{
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
        entry.valid = widget.valid;
        entry.hasManifest = widget.manifest.hasManifest;
        entry.permissions = widget.manifest.permissions;
        std::string errorKey = WidgetWideToUtf8(widget.widgetId) + ".lastError";
        auto errIt = g_storage.find(errorKey);
        if (errIt != g_storage.end())
            entry.lastError = errIt->second;
        std::string logKey = WidgetWideToUtf8(widget.widgetId);
        for (const auto& log : g_widgetLogs)
        {
            if (log.key == logKey)
                entry.logs.push_back(log);
        }
        result.push_back(std::move(entry));
    }
    return result;
}

// ── List available widget scripts ────────────────────────────────
std::vector<std::wstring> WidgetEngine::ListAvailable()
{
    std::vector<std::wstring> result;
    std::wstring search = GetWidgetsDir();
    search += L"\\*.lua";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LuaWidgetManifest manifest = GetWidgetManifest(fd.cFileName);
        if (!manifest.signatureValid) continue;
        if (!manifest.minHostVersion.empty() &&
            CompareVersions(SNOWDESKTOP_VERSION, manifest.minHostVersion) < 0)
            continue;
        result.push_back(fd.cFileName);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return result;
}

LuaWidgetManifest WidgetEngine::GetWidgetManifest(const std::wstring& filename)
{
    LuaWidgetManifest manifest;
    std::wstring fullPath = filename;
    if (PathIsRelativeW(fullPath.c_str()))
        fullPath = ResolveWidgetPath(filename);

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

bool WidgetEngine::InstallWidgetPackage(const std::wstring& manifestPath, std::wstring& error)
{
    std::string text = ReadTextFile(manifestPath);
    if (text.empty()) { error = _LW("engine.error.read_manifest"); return false; }
    JsonValue packageRoot;
    std::string parseError;
    if (!ParseJson(text, packageRoot, &parseError) || !packageRoot.IsObject())
    {
        error = _LW("engine.error.invalid_manifest");
        return false;
    }
    std::string entry;
    if (const JsonValue* entryValue = packageRoot.Find("entry"); entryValue && entryValue->IsString())
        entry = entryValue->string;
    wchar_t sourceDirBuffer[MAX_PATH]{};
    wcsncpy_s(sourceDirBuffer, manifestPath.c_str(), _TRUNCATE);
    PathRemoveFileSpecW(sourceDirBuffer);
    std::wstring sourceDir = sourceDirBuffer;
    std::wstring sourceScript;
    if (!entry.empty())
        sourceScript = sourceDir + L"\\" + Utf8ToWideLocal(entry);
    else
    {
        sourceScript = manifestPath;
        if (sourceScript.size() > 12 &&
            sourceScript.substr(sourceScript.size() - 12) == L".widget.json")
            sourceScript.replace(sourceScript.size() - 12, 12, L".lua");
    }
    if (GetFileAttributesW(sourceScript.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = _LW("engine.error.missing_entry");
        return false;
    }
    wchar_t canonicalDir[MAX_PATH]{};
    wchar_t canonicalScript[MAX_PATH]{};
    if (!GetFullPathNameW(sourceDir.c_str(), MAX_PATH, canonicalDir, nullptr) ||
        !GetFullPathNameW(sourceScript.c_str(), MAX_PATH, canonicalScript, nullptr))
    {
        error = _LW("engine.error.resolve_path");
        return false;
    }
    std::wstring allowedPrefix = canonicalDir;
    if (!allowedPrefix.empty() && allowedPrefix.back() != L'\\')
        allowedPrefix.push_back(L'\\');
    if (_wcsnicmp(canonicalScript, allowedPrefix.c_str(), allowedPrefix.size()) != 0 ||
        _wcsicmp(PathFindExtensionW(canonicalScript), L".lua") != 0)
    {
        error = _LW("engine.error.entry_local");
        return false;
    }

    std::wstring stem = PathFindFileNameW(manifestPath.c_str());
    if (stem.size() > 12 && stem.substr(stem.size() - 12) == L".widget.json")
        stem.resize(stem.size() - 12);
    std::wstring targetDir = GetWidgetsDir();
    std::wstring targetManifest = targetDir + L"\\" + stem + L".widget.json";
    std::wstring targetScript = targetDir + L"\\" + stem + L".lua";
    std::wstring tempStem = stem + L".installing";
    std::wstring tempManifest = targetDir + L"\\" + tempStem + L".widget.json";
    std::wstring tempScript = targetDir + L"\\" + tempStem + L".lua";
    DeleteFileW(tempScript.c_str());
    DeleteFileW(tempManifest.c_str());
    if (!CopyFileW(sourceScript.c_str(), tempScript.c_str(), FALSE) ||
        !CopyFileW(manifestPath.c_str(), tempManifest.c_str(), FALSE))
    {
        DeleteFileW(tempScript.c_str());
        DeleteFileW(tempManifest.c_str());
        error = _LW("engine.error.copy_failed");
        return false;
    }
    LuaWidgetManifest installed = GetWidgetManifest(tempScript);
    if (!installed.signatureValid)
    {
        DeleteFileW(tempScript.c_str());
        DeleteFileW(tempManifest.c_str());
        error = _LW("engine.error.signature_failed");
        return false;
    }
    if (!installed.minHostVersion.empty() &&
        CompareVersions(SNOWDESKTOP_VERSION, installed.minHostVersion) < 0)
    {
        DeleteFileW(tempScript.c_str());
        DeleteFileW(tempManifest.c_str());
        error = _LW("engine.error.host_version");
        return false;
    }

    const std::wstring backupScript = targetScript + L".backup";
    const std::wstring backupManifest = targetManifest + L".backup";
    DeleteFileW(backupScript.c_str());
    DeleteFileW(backupManifest.c_str());
    const bool hadScript = GetFileAttributesW(targetScript.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool hadManifest = GetFileAttributesW(targetManifest.c_str()) != INVALID_FILE_ATTRIBUTES;
    if ((hadScript && !MoveFileExW(targetScript.c_str(), backupScript.c_str(), MOVEFILE_REPLACE_EXISTING)) ||
        (hadManifest && !MoveFileExW(targetManifest.c_str(), backupManifest.c_str(), MOVEFILE_REPLACE_EXISTING)))
    {
        if (GetFileAttributesW(backupScript.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExW(backupScript.c_str(), targetScript.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(tempScript.c_str());
        DeleteFileW(tempManifest.c_str());
        error = _LW("engine.error.backup_failed");
        return false;
    }
    const bool scriptInstalled = MoveFileExW(tempScript.c_str(), targetScript.c_str(), MOVEFILE_REPLACE_EXISTING);
    const bool manifestInstalled = scriptInstalled &&
        MoveFileExW(tempManifest.c_str(), targetManifest.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!manifestInstalled)
    {
        DeleteFileW(targetScript.c_str());
        DeleteFileW(targetManifest.c_str());
        if (hadScript)
            MoveFileExW(backupScript.c_str(), targetScript.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (hadManifest)
            MoveFileExW(backupManifest.c_str(), targetManifest.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(tempScript.c_str());
        DeleteFileW(tempManifest.c_str());
        error = _LW("engine.error.replace_failed");
        return false;
    }
    DeleteFileW(backupScript.c_str());
    DeleteFileW(backupManifest.c_str());
    error.clear();
    return true;
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
            if (value != catalog.end() && Utf8ToWideLocal(value->second) == title)
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

static int lua_DrawFa(lua_State* L)
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
        DWRITE_FONT_WEIGHT_NORMAL, true, DWRITE_WORD_WRAPPING_NO_WRAP, true);
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

static int lua_StorageGet(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (!s) { lua_pushnil(L); return 1; }
    std::string fullKey = s->storagePrefix + "." + key;
    auto it = g_storage.find(fullKey);
    if (it != g_storage.end())
        lua_pushstring(L, it->second.c_str());
    else if (s->engine)
    {
        std::string defaultValue = s->engine->RuntimeGetStorageValue(s->currentWidgetId, key);
        if (!defaultValue.empty())
            lua_pushstring(L, defaultValue.c_str());
        else
            lua_pushnil(L);
    }
    else
        lua_pushnil(L);
    return 1;
}

static int lua_StorageSet(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    const char* value = luaL_checkstring(L, 2);
    auto* s = GetD2D(L);
    if (!s || IsRemovedPanelEffectSettingKey(key)) return 0;
    g_storage[s->storagePrefix + "." + key] = value;
    SaveStorageFile();
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
            bool selected = (current == i + 1);
            if (ImGui::Selectable(items[(size_t)i].c_str(), selected))
                current = i + 1;
            if (selected)
                ImGui::SetItemDefaultFocus();
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
    g_storage.erase(s->storagePrefix + "." + key);
    SaveStorageFile();
    return 0;
}

static int lua_StorageKeys(lua_State* L)
{
    auto* s = GetD2D(L);
    std::string prefix = s ? s->storagePrefix + "." : "";
    lua_newtable(L);
    int idx = 1;
    for (const auto& kv : g_storage)
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
    const float fontScale = s ? s->itemFontSizeScale : 1.0f;
    lua_pushnumber(L, std::max(9.0f, value * CalculateWidgetCellScale(cellWidth, cellHeight) * fontScale));
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
    lua_newtable(L);
    lua_pushcfunction(L, lua_DrawText);  lua_setfield(L, -2, "text");
    lua_pushcfunction(L, lua_MeasureText); lua_setfield(L, -2, "measureText");
    lua_pushcfunction(L, lua_DrawRect);  lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, lua_DrawPushClip); lua_setfield(L, -2, "pushClip");
    lua_pushcfunction(L, lua_DrawPopClip); lua_setfield(L, -2, "popClip");
    lua_pushcfunction(L, lua_DrawStrokeRect); lua_setfield(L, -2, "strokeRect");
    lua_pushcfunction(L, lua_DrawLine);  lua_setfield(L, -2, "line");
    lua_pushcfunction(L, lua_DrawCircle);lua_setfield(L, -2, "circle");
    lua_pushcfunction(L, lua_DrawFa);    lua_setfield(L, -2, "fa");
    lua_pushcfunction(L, lua_DrawImage); lua_setfield(L, -2, "image");
    lua_pushcfunction(L, lua_DrawIcon);  lua_setfield(L, -2, "icon");
    lua_setglobal(L, "draw");

    lua_newtable(L);
    lua_pushcfunction(L, lua_WidgetInfo); lua_setfield(L, -2, "info");
    lua_pushcfunction(L, lua_WidgetSetTitle); lua_setfield(L, -2, "setTitle");
    lua_pushcfunction(L, lua_WidgetOpenSettings); lua_setfield(L, -2, "openSettings");
    lua_pushcfunction(L, lua_WidgetInvalidate); lua_setfield(L, -2, "invalidate");
    lua_pushcfunction(L, lua_WidgetLog); lua_setfield(L, -2, "log");
    lua_pushcfunction(L, lua_WidgetTheme); lua_setfield(L, -2, "theme");
    lua_pushcfunction(L, lua_WidgetEditText); lua_setfield(L, -2, "editText");
    lua_pushcfunction(L, lua_WidgetSetTimer); lua_setfield(L, -2, "setTimer");
    lua_pushcfunction(L, lua_WidgetCancelTimer); lua_setfield(L, -2, "cancelTimer");
    lua_setglobal(L, "widget");

    lua_newtable(L);
    lua_pushcfunction(L, lua_GetTime);   lua_setfield(L, -2, "getTime");
    lua_pushcfunction(L, lua_Notify);    lua_setfield(L, -2, "notify");
    lua_pushcfunction(L, lua_SystemCpu); lua_setfield(L, -2, "cpu");
    lua_pushcfunction(L, lua_SystemMemory); lua_setfield(L, -2, "memory");
    lua_pushcfunction(L, lua_SystemBattery); lua_setfield(L, -2, "battery");
    lua_pushcfunction(L, lua_SystemNetwork); lua_setfield(L, -2, "network");
    lua_pushcfunction(L, lua_SystemGpu); lua_setfield(L, -2, "gpu");
    lua_setglobal(L, "sys");

    lua_newtable(L);
    lua_pushcfunction(L, lua_MediaCurrent); lua_setfield(L, -2, "current");
    lua_pushcfunction(L, lua_MediaPlayPause); lua_setfield(L, -2, "playPause");
    lua_pushcfunction(L, lua_MediaNext); lua_setfield(L, -2, "next");
    lua_pushcfunction(L, lua_MediaPrevious); lua_setfield(L, -2, "previous");
    lua_setglobal(L, "media");

    lua_newtable(L);
    lua_pushcfunction(L, lua_HttpRequest); lua_setfield(L, -2, "request");
    lua_pushcfunction(L, lua_HttpCancel); lua_setfield(L, -2, "cancel");
    lua_setglobal(L, "http");

    lua_newtable(L);
    lua_pushcfunction(L, lua_UiTextInput); lua_setfield(L, -2, "textInput");
    lua_pushcfunction(L, lua_UiFocusInput); lua_setfield(L, -2, "focusInput");
    lua_pushcfunction(L, lua_UiButton); lua_setfield(L, -2, "button");
    lua_pushcfunction(L, lua_UiToggle); lua_setfield(L, -2, "toggle");
    lua_pushcfunction(L, lua_UiProgress); lua_setfield(L, -2, "progress");
    lua_pushcfunction(L, lua_UiScrollArea); lua_setfield(L, -2, "scrollArea");
    lua_pushcfunction(L, lua_UiVirtualList); lua_setfield(L, -2, "virtualList");
    lua_setglobal(L, "ui");

    lua_newtable(L);
    lua_pushcfunction(L, lua_DesktopItems); lua_setfield(L, -2, "items");
    lua_pushcfunction(L, lua_DesktopSelection); lua_setfield(L, -2, "selection");
    lua_pushcfunction(L, lua_DesktopFind); lua_setfield(L, -2, "find");
    lua_pushcfunction(L, lua_DesktopOpen); lua_setfield(L, -2, "open");
    lua_pushcfunction(L, lua_DesktopReveal); lua_setfield(L, -2, "reveal");
    lua_pushcfunction(L, lua_DesktopRefresh); lua_setfield(L, -2, "refresh");
    lua_setglobal(L, "desktop");

    lua_newtable(L);
    lua_pushcfunction(L, lua_EverythingSearch); lua_setfield(L, -2, "search");
    lua_setglobal(L, "everything");

    lua_newtable(L);
    lua_pushcfunction(L, lua_LayoutWidth);  lua_setfield(L, -2, "width");
    lua_pushcfunction(L, lua_LayoutHeight); lua_setfield(L, -2, "height");
    lua_pushcfunction(L, lua_LayoutColumns); lua_setfield(L, -2, "columns");
    lua_pushcfunction(L, lua_LayoutRows); lua_setfield(L, -2, "rows");
    lua_pushcfunction(L, lua_LayoutSizeClass); lua_setfield(L, -2, "sizeClass");
    lua_pushcfunction(L, lua_LayoutCellWidth); lua_setfield(L, -2, "cellWidth");
    lua_pushcfunction(L, lua_LayoutCellHeight); lua_setfield(L, -2, "cellHeight");
    lua_pushcfunction(L, lua_LayoutCellScale); lua_setfield(L, -2, "cellScale");
    lua_pushcfunction(L, lua_LayoutCu); lua_setfield(L, -2, "cu");
    lua_pushcfunction(L, lua_LayoutFontCu); lua_setfield(L, -2, "fontCu");
    lua_pushcfunction(L, lua_LayoutCellGap);    lua_setfield(L, -2, "cellGap");
    lua_pushcfunction(L, lua_LayoutBarHeight);  lua_setfield(L, -2, "barHeight");
    lua_setglobal(L, "layout");

    lua_newtable(L);
    lua_pushcfunction(L, lua_StorageGet);   lua_setfield(L, -2, "get");
    lua_pushcfunction(L, lua_StorageSet);   lua_setfield(L, -2, "set");
    lua_pushcfunction(L, lua_StorageRemove);lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, lua_StorageKeys);  lua_setfield(L, -2, "keys");
    lua_setglobal(L, "storage");

    lua_newtable(L);
    lua_pushcfunction(L, lua_ImGuiText);     lua_setfield(L, -2, "text");
    lua_pushcfunction(L, lua_ImGuiTextWrapped); lua_setfield(L, -2, "textWrapped");
    lua_pushcfunction(L, lua_ImGuiSeparator); lua_setfield(L, -2, "separator");
    lua_pushcfunction(L, lua_ImGuiSameLine);  lua_setfield(L, -2, "sameLine");
    lua_pushcfunction(L, lua_ImGuiSpacing);   lua_setfield(L, -2, "spacing");
    lua_pushcfunction(L, lua_ImGuiCollapsingHeader); lua_setfield(L, -2, "collapsingHeader");
    lua_pushcfunction(L, lua_ImGuiTreeNode);  lua_setfield(L, -2, "treeNode");
    lua_pushcfunction(L, lua_ImGuiTreePop);   lua_setfield(L, -2, "treePop");
    lua_pushcfunction(L, lua_ImGuiButton);   lua_setfield(L, -2, "button");
    lua_pushcfunction(L, lua_ImGuiInputText);lua_setfield(L, -2, "input");
    lua_pushcfunction(L, lua_ImGuiInputTextSingle); lua_setfield(L, -2, "inputText");
    lua_pushcfunction(L, lua_ImGuiCheckbox); lua_setfield(L, -2, "checkbox");
    lua_pushcfunction(L, lua_ImGuiColorEdit3); lua_setfield(L, -2, "colorEdit3");
    lua_pushcfunction(L, lua_ImGuiSliderFloat); lua_setfield(L, -2, "sliderFloat");
    lua_pushcfunction(L, lua_ImGuiSliderInt); lua_setfield(L, -2, "sliderInt");
    lua_pushcfunction(L, lua_ImGuiCombo); lua_setfield(L, -2, "combo");
    lua_pushcfunction(L, lua_ImGuiSelectable); lua_setfield(L, -2, "selectable");
    lua_pushcfunction(L, lua_ImGuiRadio); lua_setfield(L, -2, "radio");
    lua_pushcfunction(L, lua_ImGuiBeginDisabled); lua_setfield(L, -2, "beginDisabled");
    lua_pushcfunction(L, lua_ImGuiEndDisabled); lua_setfield(L, -2, "endDisabled");
    lua_setglobal(L, "imgui");
}
