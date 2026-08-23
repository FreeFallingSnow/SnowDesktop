/**
 * @file settings_window.cpp
 * @brief SnowDesktop 设置窗口实现
 *
 * 本文件实现了 SettingsWindow 类 —— 基于 ImGui 构建的设置界面。
 * 包含以下设置页面：
 *   - 通用（General）：开机自启、快捷导航快捷键配置
 *   - 个性化（Personalization）：组件颜色、透明度、渐变等外观定制
 *   - 布局备份（Backup）：布局文件的保存、恢复与删除
 *   - 调试（Debug）：组件错误日志、组件诊断与重新加载
 *   - 关于（About）：应用信息、作者链接与开发者模式解锁
 *
 * 此外还管理窗口的 DirectX 交换链、字体加载、DPI 感知和
 *  Windows 消息处理（WndProc）。
 */

#include "settings_window.h"

#include "steam_app_identity.h"
#include "steam_child_environment.h"
#include "widget_engine.h"
#include "widget_permission_broker.h"
#include "l10n.h"
#include "resource.h"
#include "crashlog.h"
#include "data_paths.h"
#include "diagnostic_log.h"
#include "deployment_context.h"
#include "full_data_backup.h"
#include "http_runtime.h"
#include "portable_data_migration.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

SettingsWindow* g_settingsWindow = nullptr;

namespace {
constexpr UINT_PTR kSettingsRefreshTimerId = 1;
constexpr UINT kSettingsRefreshIntervalMs = 500;
constexpr UINT_PTR kSettingsHotkeyCaptureTimerId = 2;
constexpr UINT kSettingsHotkeyCaptureIntervalMs = 16;
constexpr float kSettingControlWidthDip = 300.0f;
constexpr wchar_t kAutoStartRunSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoStartRunValue[] = L"SnowDesktop";
constexpr DWORD kPortableAutoStartQueryIntervalMs = 2000;

void LogSettingsWindowFailure(const wchar_t* stage, HRESULT result)
{
    wchar_t message[256]{};
    swprintf_s(message,
        L"SettingsWindow %s failed (hr=0x%08X)",
        stage ? stage : L"operation",
        static_cast<unsigned>(result));
    WriteDiagnosticLogEntry(message);
}

HRESULT LastSettingsWindowError()
{
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

std::string CodepointToUtf8(unsigned int codepoint)
{
    std::string result;
    if (codepoint <= 0x7F)
    {
        result.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        result.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
        result.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        result.push_back(static_cast<char>(
            0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0x10FFFF)
    {
        result.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        result.push_back(static_cast<char>(
            0x80 | ((codepoint >> 12) & 0x3F)));
        result.push_back(static_cast<char>(
            0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return result;
}

std::optional<std::filesystem::path> PickSettingsFile(HWND owner,
    const wchar_t* title, const COMDLG_FILTERSPEC* filters,
    UINT filterCount)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
            FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title);
    if (filters && filterCount > 0)
        dialog->SetFileTypes(filterCount, filters);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(shown))
        return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
        return std::nullopt;
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) ||
        !selected)
    {
        if (selected) CoTaskMemFree(selected);
        return std::nullopt;
    }
    std::filesystem::path result(selected);
    CoTaskMemFree(selected);
    return result;
}

std::wstring QuoteProcessArgument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool LaunchSteamWorkshopPublisher(
    const std::filesystem::path& developmentRoot,
    const std::filesystem::path& projectDirectory = {})
{
    if (!WidgetEngine::IsSteamWorkshopBridgeAvailable()) return false;
    const std::filesystem::path manager =
        std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopWorkshopManager.exe";
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(manager, filesystemError))
        return false;
    const DWORD attributes = GetFileAttributesW(manager.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;
    const std::string effectiveLanguage =
        Locale::Instance().GetEffectiveLanguage();
    const std::wstring managerLanguage(effectiveLanguage.begin(),
        effectiveLanguage.end());
    const std::wstring settingsFile = GetGeneralSettingsPath();
    std::wstring commandLine = QuoteProcessArgument(manager.wstring()) +
        L" --development-root " +
        QuoteProcessArgument(developmentRoot.wstring()) +
        L" --language " + QuoteProcessArgument(managerLanguage) +
        L" --settings-file " + QuoteProcessArgument(settingsFile);
    if (!projectDirectory.empty())
        commandLine += L" --project-directory " +
            QuoteProcessArgument(projectDirectory.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring workingDirectory = manager.parent_path().wstring();
    std::vector<wchar_t> environment =
        snowdesktop::BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty()) return false;
    if (!CreateProcessW(manager.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_UNICODE_ENVIRONMENT, environment.data(),
        workingDirectory.c_str(), &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool IsSteamWorkshopPublisherAvailable()
{
    if (!WidgetEngine::IsSteamWorkshopBridgeAvailable()) return false;
    const std::filesystem::path manager =
        std::filesystem::path(GetExecutableDirectoryPath()) /
        L"SnowDesktopWorkshopManager.exe";
    std::error_code error;
    if (!std::filesystem::is_regular_file(manager, error) || error)
        return false;
    const DWORD attributes = GetFileAttributesW(manager.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

void OpenSteamUrlWithWebFallback(
    const std::wstring& steamUrl, const std::wstring& webUrl)
{
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open",
        steamUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) > 32) return;
    ShellExecuteW(nullptr, L"open", webUrl.c_str(), nullptr, nullptr,
        SW_SHOWNORMAL);
}

std::optional<std::filesystem::path> SaveSettingsFile(HWND owner,
    const wchar_t* title, const wchar_t* defaultName,
    const wchar_t* defaultExtension,
    const COMDLG_FILTERSPEC* filters, UINT filterCount)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        return std::nullopt;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    }
    dialog->SetTitle(title);
    if (defaultName && *defaultName)
        dialog->SetFileName(defaultName);
    if (defaultExtension && *defaultExtension)
        dialog->SetDefaultExtension(defaultExtension);
    if (filters && filterCount > 0)
        dialog->SetFileTypes(filterCount, filters);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(shown))
        return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)) || !item)
        return std::nullopt;
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)) ||
        !selected)
    {
        if (selected)
            CoTaskMemFree(selected);
        return std::nullopt;
    }
    std::filesystem::path result(selected);
    CoTaskMemFree(selected);
    return result;
}

std::filesystem::path FullBackupStateRoot()
{
    std::filesystem::path stateRoot =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (stateRoot.empty())
        stateRoot = GetExecutableDirectoryPath();
    return stateRoot;
}

snowdesktop::backup::FullDataBackupManager MakeFullBackupManager()
{
    return snowdesktop::backup::FullDataBackupManager(
        FullBackupStateRoot(),
        GetDataDirectoryPath(),
        SNOWDESKTOP_VERSION,
        snowdesktop::deployment::IsPackaged()
            ? "installed" : "portable");
}

std::string FormatBackupSize(std::uint64_t bytes)
{
    static constexpr const char* units[] = {
        "B", "KiB", "MiB", "GiB"
    };
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64]{};
    if (unit == 0)
        std::snprintf(buffer, sizeof(buffer), "%llu %s",
            static_cast<unsigned long long>(bytes), units[unit]);
    else
        std::snprintf(buffer, sizeof(buffer), "%.1f %s",
            value, units[unit]);
    return buffer;
}

std::wstring QueryPortableAutoStartCommandViaWmi()
{
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))))
        return {};

    BSTR nameSpace = SysAllocString(L"ROOT\\CIMV2");
    if (!nameSpace)
        return {};
    ComPtr<IWbemServices> services;
    const HRESULT connectResult = locator->ConnectServer(
        nameSpace, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(nameSpace);
    if (FAILED(connectResult))
        return {};

    if (FAILED(CoSetProxyBlanket(services.Get(), RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE)))
        return {};

    BSTR queryLanguage = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT Command FROM Win32_StartupCommand "
        L"WHERE Name='SnowDesktop'");
    if (!queryLanguage || !query)
    {
        if (queryLanguage)
            SysFreeString(queryLanguage);
        if (query)
            SysFreeString(query);
        return {};
    }

    ComPtr<IEnumWbemClassObject> results;
    const HRESULT queryResult = services->ExecQuery(
        queryLanguage, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr, &results);
    SysFreeString(queryLanguage);
    SysFreeString(query);
    if (FAILED(queryResult))
        return {};

    ComPtr<IWbemClassObject> item;
    ULONG returned = 0;
    if (FAILED(results->Next(1000, 1, &item, &returned)) ||
        returned == 0)
    {
        return {};
    }

    VARIANT command{};
    VariantInit(&command);
    const HRESULT getResult = item->Get(
        L"Command", 0, &command, nullptr, nullptr);
    std::wstring value;
    if (SUCCEEDED(getResult) && command.vt == VT_BSTR &&
        command.bstrVal != nullptr)
    {
        value = command.bstrVal;
    }
    VariantClear(&command);
    return value;
}

std::wstring ReadPortableAutoStartCommand()
{
    if (snowdesktop::deployment::IsPackaged())
    {
        static DWORD lastQueryTick = 0;
        static std::wstring cachedCommand;
        const DWORD now = GetTickCount();
        if (lastQueryTick == 0 ||
            now - lastQueryTick >= kPortableAutoStartQueryIntervalMs)
        {
            cachedCommand = QueryPortableAutoStartCommandViaWmi();
            lastQueryTick = now;
        }
        return cachedCommand;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAutoStartRunSubKey,
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};

    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, kAutoStartRunValue,
        nullptr, &type, nullptr, &size);
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return {};
    }

    std::vector<wchar_t> buffer(
        static_cast<size_t>(size / sizeof(wchar_t)) + 1, L'\0');
    result = RegQueryValueExW(key, kAutoStartRunValue,
        nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return {};

    std::wstring command(buffer.data());
    if (type != REG_EXPAND_SZ)
        return command;

    const DWORD expandedLength =
        ExpandEnvironmentStringsW(command.c_str(), nullptr, 0);
    if (expandedLength <= 1)
        return command;
    std::wstring expanded(expandedLength, L'\0');
    if (ExpandEnvironmentStringsW(
            command.c_str(), expanded.data(), expandedLength) == 0)
    {
        return command;
    }
    expanded.resize(expandedLength - 1);
    return expanded;
}

bool LooksLikeSnowDesktopDataDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_directory(path, error))
        return false;

    return std::filesystem::is_regular_file(
               path / L"SnowDesktop.layout.json", error) ||
        std::filesystem::is_regular_file(
            path / L"SnowDesktop.general.json", error) ||
        std::filesystem::is_directory(path / L"widgets", error) ||
        std::filesystem::is_directory(path / L"backups", error);
}

std::wstring NormalizePathForComparison(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::weakly_canonical(path, error);
    if (error)
        normalized = path.lexically_normal();

    std::wstring result = normalized.wstring();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\')
        result.pop_back();
    std::transform(result.begin(), result.end(), result.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return result;
}

bool PathsOverlap(const std::filesystem::path& first,
    const std::filesystem::path& second)
{
    const std::wstring left = NormalizePathForComparison(first);
    const std::wstring right = NormalizePathForComparison(second);
    if (left == right)
        return true;
    const std::wstring leftPrefix = left + L"\\";
    const std::wstring rightPrefix = right + L"\\";
    return left.starts_with(rightPrefix) || right.starts_with(leftPrefix);
}

void DrawHelpTooltip(const char* description)
{
    if (!description || !description[0] || !ImGui::IsItemHovered())
        return;

    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(description);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void DrawHelpMarker(const char* description)
{
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("?");
    DrawHelpTooltip(description);
}

void DrawSettingSection(const char* label, const char* description = nullptr)
{
    if (!description || !description[0])
    {
        ImGui::SeparatorText(label);
        return;
    }

    const std::string displayLabel = std::string(label) + "  ?";
    ImGui::SeparatorText(displayLabel.c_str());
    DrawHelpTooltip(description);
}

bool DrawCollapsingHeaderWithHelp(const char* label, const char* description)
{
    const std::string displayLabel = std::string(label) + "  ?";
    const bool open = ImGui::CollapsingHeader(displayLabel.c_str());
    DrawHelpTooltip(description);
    return open;
}

float BeginSettingRow(const char* label, float controlWidth,
    const char* description = nullptr)
{
    const float rowStart = ImGui::GetCursorPosX();
    const float rowRight = rowStart + ImGui::GetContentRegionAvail().x;
    const float controlX = std::max(rowStart,
        rowRight - std::max(1.0f, controlWidth));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (description && description[0])
        DrawHelpMarker(description);
    ImGui::SameLine(controlX);
    return controlX;
}

bool DrawSettingCheckbox(const char* label, const char* id, bool* value,
    const char* description = nullptr)
{
    BeginSettingRow(label, ImGui::GetFrameHeight(), description);
    return ImGui::Checkbox(id, value);
}

bool DrawSettingColorEdit3(const char* label, const char* id, float color[3])
{
    const float buttonSize = ImGui::GetFrameHeight();
    BeginSettingRow(label, buttonSize);

    bool changed = false;
    ImGui::PushID(id);
    if (ImGui::ColorButton("##Button",
            ImVec4(color[0], color[1], color[2], 1.0f),
            ImGuiColorEditFlags_NoAlpha |
                ImGuiColorEditFlags_NoDragDrop,
            ImVec2(buttonSize, buttonSize)))
    {
        ImGui::OpenPopup("##PickerPopup");
    }
    if (ImGui::BeginPopup("##PickerPopup"))
    {
        ImGui::SetNextItemWidth(buttonSize * 12.0f);
        changed = ImGui::ColorPicker3("##Picker", color,
            ImGuiColorEditFlags_NoLabel |
                ImGuiColorEditFlags_NoAlpha);
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

void DrawSettingValue(const char* label, const char* value)
{
    const float valueWidth = ImGui::CalcTextSize(value).x;
    BeginSettingRow(label, valueWidth);
    ImGui::TextDisabled("%s", value);
}

float SettingButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label, nullptr, true).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
}

void CopyWideToUtf8Buffer(
    const std::wstring& text, char* buffer, size_t bufferSize);
}

/**
 * @brief 配置 ImGui 的浅色主题配色方案。
 *
 * 对 ImGui 样式表逐项设置圆角半径和颜色值，为整个设置窗口
 * 提供统一的浅色外观。颜色值覆盖窗口背景、子窗口背景、边框、
 * 按钮、标签页、滚动条、调节手柄等全部 UI 元素。
 */
static void SetupLightTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.Alpha = 1.0f;
    s.FrameRounding = 4.0f;
    s.WindowRounding = 0.0f;
    s.ChildRounding = 6.0f;
    s.ScrollbarSize = 10.0f;
    s.ScrollbarRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Border]               = ImVec4(0.78f, 0.78f, 0.82f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.88f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.82f, 0.82f, 0.87f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.10f, 0.10f, 0.14f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.25f, 0.55f, 0.90f, 0.35f);
    c[ImGuiCol_InputTextCursor]      = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.84f, 0.84f, 0.88f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.78f, 0.78f, 0.83f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.18f, 0.50f, 0.92f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.24f, 0.56f, 0.96f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.14f, 0.42f, 0.84f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.18f, 0.50f, 0.92f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.24f, 0.55f, 0.92f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.30f, 0.60f, 0.96f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.84f, 0.84f, 0.88f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.78f, 0.78f, 0.83f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.88f, 0.88f, 0.91f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.78f, 0.78f, 0.83f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.80f, 0.80f, 0.84f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.70f, 0.70f, 0.75f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.60f, 0.60f, 0.65f, 1.00f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.85f, 0.85f, 0.88f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.70f, 0.70f, 0.75f, 1.00f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
}

/**
 * @brief 析构函数，自动调用 Shutdown() 释放资源。
 */
SettingsWindow::~SettingsWindow()
{
    Shutdown();
}

/**
 * @brief 初始化设置窗口。
 *
 * 执行以下初始化序列：
 * 1. 注册窗口类并创建 Win32 窗口（DPI 感知初始尺寸）
 * 2. 创建 DirectX 交换链和渲染目标视图
 * 3. 初始化 ImGui 上下文（Win32 + DX11 后端）
 * 4. 应用浅色主题、加载字体
 * 5. 从磁盘读取个性化与导航设置
 * 6. 将窗口居中显示在屏幕上
 *
 * @param instance  应用程序实例句柄（HINSTANCE）
 * @param device    Direct3D 11 设备指针（ComPtr 的原始指针）
 * @return true  初始化成功
 * @return false 初始化失败（窗口创建或交换链创建失败）
 */
bool SettingsWindow::Init(HINSTANCE instance, ID3D11Device* device)
{
    if (!instance || !device)
    {
        LogSettingsWindowFailure(L"Init arguments", E_POINTER);
        return false;
    }
    if (hwnd_ && IsWindow(hwnd_) && swapChain_ && rtv_ &&
        imguiContext_ && imguiWin32Initialized_ &&
        imguiDx11Initialized_)
        return true;
    if (hwnd_ || swapChain_ || rtv_ || imguiContext_)
        Shutdown();

    instance_ = instance;
    device_ = device;
    context_.Reset();
    device_->GetImmediateContext(&context_);
    if (!context_)
    {
        LogSettingsWindowFailure(L"GetImmediateContext", E_FAIL);
        return false;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON_SMALL),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SnowDesktopSettingsWindow";
    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogSettingsWindowFailure(
            L"RegisterClassExW",
            LastSettingsWindowError());
        return false;
    }

    // Get DPI for initial sizing
    UINT dpi = GetDpiForSystem();
    {
        HDC screenDc = GetDC(nullptr);
        if (screenDc)
        {
            dpi = GetDeviceCaps(screenDc, LOGPIXELSX);
            ReleaseDC(nullptr, screenDc);
        }
    }
    dpiScale_ = static_cast<float>(dpi) / 96.0f;
    windowWidth_ = static_cast<int>(800.0f * dpiScale_);
    windowHeight_ = static_cast<int>(560.0f * dpiScale_);

    hwnd_ = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        _LW("app.settings.title"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth_, windowHeight_,
        nullptr, nullptr, instance, this);

    if (hwnd_ == nullptr)
    {
        LogSettingsWindowFailure(
            L"CreateWindowExW",
            LastSettingsWindowError());
        return false;
    }
    if (wc.hIcon)
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm)
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    if (!CreateSwapChain())
    {
        Shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    if (!imguiContext_)
    {
        LogSettingsWindowFailure(L"ImGui::CreateContext", E_OUTOFMEMORY);
        Shutdown();
        return false;
    }
    ImGui::SetCurrentContext(imguiContext_);

    SetupLightTheme();

    imguiWin32Initialized_ = ImGui_ImplWin32_Init(hwnd_);
    if (!imguiWin32Initialized_)
    {
        LogSettingsWindowFailure(L"ImGui Win32 backend", E_FAIL);
        Shutdown();
        return false;
    }
    imguiDx11Initialized_ =
        ImGui_ImplDX11_Init(device_.Get(), context_.Get());
    if (!imguiDx11Initialized_)
    {
        LogSettingsWindowFailure(L"ImGui DX11 backend", E_FAIL);
        Shutdown();
        return false;
    }

    SetupFonts();
    if (!ImGui_ImplDX11_CreateDeviceObjects())
    {
        LogSettingsWindowFailure(
            L"ImGui DX11 device objects", E_FAIL);
        Shutdown();
        return false;
    }

    bool categorizedTabHeightLoaded = false;
    LoadPersonalization(
        GetPersonalizationPath().c_str(),
        personalization_,
        &categorizedTabHeightLoaded);
    LoadDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
    LoadNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);
    LoadGeneralSettings(GetGeneralSettingsPath().c_str(), generalSettings_);
    if (std::strcmp(generalSettings_.language, "system") != 0 &&
        !Locale::Instance().HasLanguage(generalSettings_.language))
    {
        std::strncpy(generalSettings_.language, "system",
            sizeof(generalSettings_.language) - 1);
        generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
    }
    categorySettings_ = CategorySettings::Defaults();
    LoadCategorySettings(GetCategorySettingsPath().c_str(), categorySettings_);
    if (!categorizedTabHeightLoaded)
    {
        // 1.0.1.0 之前该值以字号形式保存在分类设置中。迁移一次后由外观设置持有，
        // 避免重置分类规则时意外重置三类组件的共同外观。
        // 默认字号 15 对应默认标签条高度 34。
        personalization_.categorizedTabHeight =
            std::clamp(
                categorySettings_.tabFontSize * 34.0f / 15.0f,
                24.0f, 48.0f);
        SavePersonalization(
            GetPersonalizationPath().c_str(),
            personalization_);
    }
    SyncCategoryRuleBuffersFromSettings();

    g_settingsWindow = this;

    RECT rc;
    GetWindowRect(hwnd_, &rc);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd_, nullptr,
        (screenW - (rc.right - rc.left)) / 2,
        (screenH - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    WriteDiagnosticLogEntry(L"SettingsWindow initialized");
    return true;
}

/**
 * @brief 关闭设置窗口并释放所有 ImGui 与 DirectX 资源。
 *
 * 清理顺序：销毁全局指针、关闭 ImGui DX11/Win32 后端、
 * 销毁 ImGui 上下文、清理交换链、销毁窗口句柄。
 */
void SettingsWindow::Shutdown()
{
    if (hwnd_ != nullptr)
    {
        KillTimer(hwnd_, kSettingsRefreshTimerId);
        KillTimer(hwnd_, kSettingsHotkeyCaptureTimerId);
    }
    hotkeyCaptureTarget_ = HotkeySettingTarget::None;

    if (personalizationDirty_)
    {
        SavePersonalization(GetPersonalizationPath().c_str(), personalization_);
        personalizationDirty_ = false;
        personalizationSaveRequested_ = false;
    }
    if (dockSettingsDirty_)
    {
        SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
        dockSettingsDirty_ = false;
        dockSettingsPreviewDirty_ = false;
        dockSettingsSaveRequested_ = false;
        if (dockSettingsChangedCallback_)
            dockSettingsChangedCallback_();
    }
    layoutSpacingPreviewPending_ = false;
    layoutSpacingCommitPending_ = false;
    itemIconSizePreviewPending_ = false;
    itemIconSizeCommitPending_ = false;
    if (categorySettingsDirty_)
    {
        NormalizeCategoryRuleBuffers();
        SaveCategorySettings(GetCategorySettingsPath().c_str(), categorySettings_);
        categorySettingsDirty_ = false;
        categorySettingsSaveRequested_ = false;
        categorySettingsSavedTick_ = GetTickCount();
        if (categorySettingsChangedCallback_)
            categorySettingsChangedCallback_();
    }
    if (g_settingsWindow == this)
        g_settingsWindow = nullptr;
    if (imguiContext_)
    {
        ImGuiContext* previousContext = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(imguiContext_);
        if (imguiDx11Initialized_)
            ImGui_ImplDX11_Shutdown();
        if (imguiWin32Initialized_)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(imguiContext_);
        if (previousContext && previousContext != imguiContext_)
            ImGui::SetCurrentContext(previousContext);
    }
    imguiContext_ = nullptr;
    imguiDx11Initialized_ = false;
    imguiWin32Initialized_ = false;
    faDebugFont_ = nullptr;
    fluentDebugFont_ = nullptr;
    faDebugCodepoints_.clear();
    fluentDebugCodepoints_.clear();
    CleanupSwapChain();
    if (hwnd_ != nullptr) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    pendingClose_ = false;
    renderRequested_ = false;
}

/**
 * @brief 显示设置窗口（若尚未初始化则先初始化再显示）。
 *
 * 将窗口置于前台并设置焦点。
 */
bool SettingsWindow::Show()
{
    if (snowdesktop::deployment::IsPackaged())
        packagedAutoStartStateKnown_ = false;

    pendingClose_ = false;
    if (!hwnd_ || !IsWindow(hwnd_) || !swapChain_ || !rtv_ ||
        !imguiContext_ || !imguiWin32Initialized_ ||
        !imguiDx11Initialized_)
    {
        Shutdown();
        if (!Init(instance_, device_.Get()))
            return false;
    }
    dockSettings_.systemTaskbarAutoHide = IsSystemTaskbarAutoHideEnabled();
    dockSettings_.systemTaskbarAlignment = IsSystemTaskbarAlignmentCentered() ? 1 : 0;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetTimer(hwnd_, kSettingsRefreshTimerId, kSettingsRefreshIntervalMs, nullptr);
    renderRequested_ = true;
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    return IsWindowVisible(hwnd_) != FALSE;
}

void SettingsWindow::ApplyLanguageChange()
{
    if (hwnd_ && IsWindow(hwnd_))
        SetWindowTextW(hwnd_, _LW("app.settings.title"));
    if (!updateCheckStatusKey_.empty())
    {
        updateCheckStatus_ = updateCheckStatusArgument_.empty()
            ? Locale::Instance().Tr(updateCheckStatusKey_.c_str())
            : Locale::Instance().TrFormat(
                updateCheckStatusKey_.c_str(), { updateCheckStatusArgument_ });
    }
    if (!categorySettingsDirty_)
    {
        SyncCategoryRuleBuffersFromSettings();
    }
    else
    {
        for (CategoryRuleEditBuffer& buffer : categoryRuleBuffers_)
        {
            if (!buffer.usesDefaultLabel)
                continue;
            CopyWideToUtf8Buffer(
                GetCategoryLabel(categorySettings_, buffer.id),
                buffer.label, sizeof(buffer.label));
        }
    }
    renderRequested_ = true;
}

bool SettingsWindow::ShowDockSettings()
{
    activePage_ = 0;
    return Show();
}

bool SettingsWindow::ShowAppearanceSettings()
{
    activePage_ = 1;
    return Show();
}

/**
 * @brief 显示退出确认对话框。
 *
 * 设置 showExitConfirm_ 标记后调用 Show()，
 * 在下一帧 Render() 中弹出模态确认框。
 */
bool SettingsWindow::ShowExitConfirm()
{
    showExitConfirm_ = true;
    return Show();
}

/**
 * @brief 请求关闭设置窗口。
 *
 * 重置组件编辑状态并设置 pendingClose_ 标记，
 * Render() 在下一帧末尾检测到该标记时执行 Shutdown()。
 */
void SettingsWindow::RequestClose()
{
    showExitConfirm_ = false;
    if (editingWidgetIndex_ != static_cast<size_t>(-1))
        editingWidgetIndex_ = static_cast<size_t>(-1);
    if (personalizationDirty_)
        personalizationSaveRequested_ = true;
    if (dockSettingsDirty_)
        dockSettingsSaveRequested_ = true;
    if (categorySettingsDirty_)
        categorySettingsSaveRequested_ = true;
    pendingClose_ = true;
    renderRequested_ = true;
}

/**
 * @brief 主渲染函数，每帧被调用以绘制设置窗口 UI。
 *
 * 执行流程：
 * 1. 检查窗口可见性，不可见或最小化时提前返回
 * 2. 启动 ImGui 帧（DX11 + Win32 后端）
 * 3. 手动修正鼠标坐标（确保首次点击有效）
 * 4. 创建全客户区主窗口，根据编辑状态选择：
 *    - 正在编辑组件时绘制组件编辑器页面
 *    - 否则绘制左侧边栏 + 右侧活动页面
 * 5. 持久化标记为脏的个人化/导航设置
 * 6. 调用失效回调以通知外部刷新
 * 7. 处理退出确认模态弹窗
 * 8. 执行 ImGui 渲染并 Present 到交换链
 * 9. 检测 pendingClose_ 标记并执行清理
 */
void SettingsWindow::Render()
{
    if (hwnd_ == nullptr || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    if (swapChain_ == nullptr) return;
    if (!imguiContext_) return;
    ImGui::SetCurrentContext(imguiContext_);
    if (renderInProgress_)
    {
        renderRequested_ = true;
        return;
    }

    renderInProgress_ = true;
    struct RenderScope final
    {
        bool& inProgress;
        ~RenderScope() { inProgress = false; }
    } renderScope{ renderInProgress_ };

    renderRequested_ = false;
    PollUpdateCheck();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    ImGui::NewFrame();

    // The Win32 backend queues input events for NewFrame to consume. Correct the
    // position afterwards so batched stale WM_MOUSEMOVE events cannot override
    // the current physical cursor position while a slider is being dragged.
    POINT mp;
    GetCursorPos(&mp);
    ScreenToClient(hwnd_, &mp);
    ImGui::GetIO().MousePos = ImVec2((float)mp.x, (float)mp.y);

    // Fill entire client area
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##MainFrame", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);

    if (editingWidgetIndex_ != static_cast<size_t>(-1))
    {
        DrawWidgetEditorPage();
    }
    else
    {
        // Sidebar + Content layout
        const float sidebarW = 160.0f;
        const float sidebarPad = 8.0f * dpiScale_;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(sidebarPad, sidebarPad));
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, 0),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        DrawSidebar();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##Content", ImVec2(0, 0), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        switch (activePage_)
        {
        case 0: DrawGeneralPage(); break;
        case 1: DrawPersonalizationPage(); break;
        case 4: DrawCategorySettingsPage(); break;
        case 5: DrawBackupPage(); break;
        case 6: DrawAboutPage(); break;
        case 8: DrawWidgetPackagesPage(); break;
        case 9:
            if (generalSettings_.widgetDeveloperToolsEnabled)
                DrawWidgetDeveloperTools();
            else
            {
                activePage_ = 8;
                DrawWidgetPackagesPage();
            }
            break;
        case 7:
            if (debugUnlocked_)
                DrawDebugPage();
            else
                DrawAboutPage();
            break;
        }
        ImGui::EndChild();
    }

    ImGui::End();

    if (widgetEditorBackPending_)
    {
        widgetEditorBackPending_ = false;
        editingWidgetIndex_ = static_cast<size_t>(-1);
    }

    if (personalizationPreviewDirty_)
    {
        personalizationPreviewDirty_ = false;
        if (invalidateCallback_)
            invalidateCallback_();
    }

    if (personalizationSaveRequested_ && personalizationDirty_)
    {
        SavePersonalization(GetPersonalizationPath().c_str(), personalization_);
        personalizationDirty_ = false;
        personalizationSaveRequested_ = false;
        if (personalizationChangedCallback_)
            personalizationChangedCallback_();
    }

    if (dockSettingsPreviewDirty_)
    {
        dockSettingsPreviewDirty_ = false;
        if (dockSettingsPreviewChangedCallback_)
            dockSettingsPreviewChangedCallback_(dockSettings_);
    }

    if (dockSettingsSaveRequested_ && dockSettingsDirty_)
    {
        SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
        dockSettingsDirty_ = false;
        dockSettingsSaveRequested_ = false;
        if (dockSettingsChangedCallback_)
            dockSettingsChangedCallback_();
    }

    if (navigationSettingsDirty_)
    {
        SaveNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);
        navigationSettingsDirty_ = false;
        if (navigationSettingsChangedCallback_)
            navigationSettingsChangedCallback_();
    }

    if (generalSettingsDirty_)
    {
        SaveGeneralSettings(GetGeneralSettingsPath().c_str(), generalSettings_);
        generalSettingsDirty_ = false;
        if (generalSettingsChangedCallback_)
            generalSettingsChangedCallback_();
    }

    if (categorySettingsSaveRequested_ && categorySettingsDirty_)
    {
        NormalizeCategoryRuleBuffers();
        SaveCategorySettings(GetCategorySettingsPath().c_str(), categorySettings_);
        categorySettingsDirty_ = false;
        categorySettingsSaveRequested_ = false;
        categorySettingsSavedTick_ = GetTickCount();
        if (categorySettingsChangedCallback_)
            categorySettingsChangedCallback_();
    }

    // Exit confirmation modal
    if (showExitConfirm_)
    {
        ImGui::OpenPopup(_L("app.settings.exit_confirm"));
        if (ImGui::BeginPopupModal(_L("app.settings.exit_confirm"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", _L("app.settings.exit_confirm_text"));
            ImGui::Text("%s", _L("app.settings.exit_restore_text"));
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool okClicked = ImGui::Button(_L("app.settings.exit_ok"), ImVec2(120, 0));
            ImGui::PopStyleColor();
            if (okClicked)
            {
                showExitConfirm_ = false;
                ImGui::CloseCurrentPopup();
                if (exitCallback_) exitCallback_();
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool cancelClicked = ImGui::Button(_L("app.settings.cancel"), ImVec2(80, 0));
            ImGui::PopStyleColor(2);
            if (cancelClicked)
            {
                showExitConfirm_ = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    ImGui::Render();

    // Dispatch only after ImGui has finalized the complete settings frame.
    // Dense mouse moves are coalesced to the newest value, and release wins
    // over a preview queued earlier in this same frame.
    DispatchPendingLayoutSpacingChange();
    DispatchPendingItemIconSizeChange();

    if (pendingClose_)
    {
        pendingClose_ = false;
        Shutdown();
        return;
    }

    const float clearColor[4] = { 0.96f, 0.96f, 0.97f, 1.0f };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(0, 0);
}

/**
 * @brief 绘制左侧导航边栏。
 *
 * 使用透明背景按钮样式，高亮当前激活页面。
 * 提供"通用"、"个性化"、"布局备份"、"关于"等入口；
 * 当 debugUnlocked_ 为 true 时额外显示"调试"入口。
 */
void SettingsWindow::DrawSidebar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.86f, 0.86f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.80f, 0.85f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.12f, 0.12f, 0.16f, 1.0f));

    ImGui::Dummy(ImVec2(0, 4));

    auto SideButton = [&](int idx, const char* label) {
        ImGui::PushID(idx);
        bool active = (activePage_ == idx);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.80f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.12f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
            activePage_ = idx;
        }
        if (active) ImGui::PopStyleColor(2);
        ImGui::PopID();
    };

    SideButton(0, _L("app.settings.general"));
    SideButton(1, _L("app.settings.appearance"));
    SideButton(4, _L("app.settings.category"));
    SideButton(8, _L("app.settings.widgets"));
    if (generalSettings_.widgetDeveloperToolsEnabled)
        SideButton(9, _L("app.settings.widgets_developer_tools"));
    SideButton(5, _L("app.settings.backup"));
    SideButton(6, _L("app.settings.about"));
    if (debugUnlocked_)
        SideButton(7, _L("app.settings.debug"));

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}

// ── UTF helpers ──────────────────────────────────────────────────
namespace {
/**
 * @brief 将 std::wstring 转换为 UTF-8 编码的 std::string。
 * @param w 宽字符串输入
 * @return UTF-8 编码的窄字符串，输入为空时返回空串
 */
    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string r(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), r.data(), n, nullptr, nullptr);
        return r;
    }

/**
 * @brief 将 UTF-8 编码的 std::string 转换为 std::wstring。
 * @param u UTF-8 编码的窄字符串输入
 * @return 宽字符串，输入为空时返回空串
 */
    std::wstring Utf8ToWide(const std::string& u)
    {
        if (u.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
        std::wstring r(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), r.data(), n);
        return r;
    }

    void CopyWideToUtf8Buffer(const std::wstring& text, char* buffer, size_t bufferSize)
    {
        if (!buffer || bufferSize == 0) return;
        std::string utf8 = WideToUtf8(text);
        std::strncpy(buffer, utf8.c_str(), bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
    }

    std::wstring TrimWide(std::wstring value)
    {
        auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(),
            [&](wchar_t ch) { return !isSpace(ch); }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
            [&](wchar_t ch) { return !isSpace(ch); }).base(), value.end());
        return value;
    }
}

/**
 * @brief 蓝色文字按钮辅助函数。
 *
 * 自动设置白色文字颜色，点击后恢复原始颜色。
 * @param label 按钮标签文本
 * @param size  按钮尺寸（可缺省，默认自适应）
 * @return true 按钮被点击
 */
static bool BlueButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    return clicked;
}

static bool SecondaryButton(const char* label,
    const ImVec2& size = ImVec2(0, 0))
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Button,
        style.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        style.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        style.Colors[ImGuiCol_FrameBgActive]);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

namespace
{
    bool IsHotkeyPhysicalKeyDown(UINT virtualKey)
    {
        return (GetAsyncKeyState(static_cast<int>(virtualKey)) &
            0x8000) != 0;
    }

    UINT ReadHotkeyModifierMask()
    {
        UINT modifiers = 0;
        if (IsHotkeyPhysicalKeyDown(VK_CONTROL))
            modifiers |= MOD_CONTROL;
        if (IsHotkeyPhysicalKeyDown(VK_MENU))
            modifiers |= MOD_ALT;
        if (IsHotkeyPhysicalKeyDown(VK_SHIFT))
            modifiers |= MOD_SHIFT;
        if (IsHotkeyPhysicalKeyDown(VK_LWIN) ||
            IsHotkeyPhysicalKeyDown(VK_RWIN))
            modifiers |= MOD_WIN;
        return modifiers;
    }

    bool IsHotkeyModifierVirtualKey(UINT virtualKey)
    {
        switch (virtualKey)
        {
        case VK_SHIFT:
        case VK_CONTROL:
        case VK_MENU:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
        }
    }

    UINT HotkeyModifierForVirtualKey(UINT virtualKey)
    {
        switch (virtualKey)
        {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
            return MOD_CONTROL;
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
            return MOD_ALT;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
            return MOD_SHIFT;
        case VK_LWIN:
        case VK_RWIN:
            return MOD_WIN;
        default:
            return 0;
        }
    }

    bool IsCapturableHotkeyVirtualKey(UINT virtualKey)
    {
        if (virtualKey <= VK_XBUTTON2 ||
            virtualKey == VK_ESCAPE ||
            virtualKey == VK_BACK ||
            virtualKey == VK_DELETE ||
            virtualKey == VK_PACKET)
            return false;
        return !IsHotkeyModifierVirtualKey(virtualKey);
    }

    const char* HotkeyTargetLabelKey(HotkeySettingTarget target)
    {
        switch (target)
        {
        case HotkeySettingTarget::QuickNavigation:
            return "app.settings.quick_navigation";
        case HotkeySettingTarget::DesktopPassthrough:
            return "app.settings.desktop_passthrough_hotkey";
        case HotkeySettingTarget::FloatingDock:
            return "app.settings.dock_bar";
        case HotkeySettingTarget::PagePrevious:
            return "app.settings.page_navigation_previous";
        case HotkeySettingTarget::PageNext:
            return "app.settings.page_navigation_next";
        case HotkeySettingTarget::None:
        default:
            return "";
        }
    }
}

void SettingsWindow::StartHotkeyCapture(
    HotkeySettingTarget target)
{
    CancelHotkeyCapture();
    hotkeyCaptureTarget_ = target;
    hotkeyCaptureModifiers_ = 0;
    hotkeyCapturePressedModifiers_ = 0;
    hotkeyCaptureVirtualKey_ = 0;
    hotkeyCapturePrimarySeen_ = false;
    hotkeyCapturePrimaryDown_ = false;
    hotkeyCaptureClearPending_ = false;
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kSettingsHotkeyCaptureTimerId,
            kSettingsHotkeyCaptureIntervalMs, nullptr);
        SetFocus(hwnd_);
    }
    renderRequested_ = true;
}

void SettingsWindow::CancelHotkeyCapture()
{
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kSettingsHotkeyCaptureTimerId);
    hotkeyCaptureTarget_ = HotkeySettingTarget::None;
    hotkeyCaptureModifiers_ = 0;
    hotkeyCapturePressedModifiers_ = 0;
    hotkeyCaptureVirtualKey_ = 0;
    hotkeyCapturePrimarySeen_ = false;
    hotkeyCapturePrimaryDown_ = false;
    hotkeyCaptureClearPending_ = false;
    renderRequested_ = true;
}

void SettingsWindow::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    if (!IsHotkeyCaptureActive() || virtualKey == 0)
        return;
    const HotkeySettingTarget target =
        hotkeyCaptureTarget_;
    CancelHotkeyCapture();
    CommitHotkeyCapture(
        target,
        modifiers &
            (MOD_CONTROL | MOD_ALT |
                MOD_SHIFT | MOD_WIN),
        virtualKey);
}

void SettingsWindow::CommitHotkeyCapture(
    HotkeySettingTarget target,
    UINT modifiers,
    UINT virtualKey)
{
    modifiers &=
        MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
    if (virtualKey == 0)
        modifiers = 0;

    switch (target)
    {
    case HotkeySettingTarget::QuickNavigation:
        navigationSettings_.modifiers = modifiers;
        navigationSettings_.virtualKey = virtualKey;
        navigationSettingsDirty_ = true;
        break;
    case HotkeySettingTarget::DesktopPassthrough:
        generalSettings_.desktopPassthroughHotkeyModifiers =
            modifiers;
        generalSettings_.desktopPassthroughHotkeyVirtualKey =
            virtualKey;
        generalSettingsDirty_ = true;
        break;
    case HotkeySettingTarget::FloatingDock:
        dockSettings_.floatingHotkeyModifiers = modifiers;
        dockSettings_.floatingHotkeyVirtualKey = virtualKey;
        dockSettingsDirty_ = true;
        dockSettingsSaveRequested_ = true;
        break;
    case HotkeySettingTarget::PagePrevious:
        generalSettings_.pageNavigationPreviousModifiers = modifiers;
        generalSettings_.pageNavigationPreviousVirtualKey = virtualKey;
        generalSettingsDirty_ = true;
        break;
    case HotkeySettingTarget::PageNext:
        generalSettings_.pageNavigationNextModifiers = modifiers;
        generalSettings_.pageNavigationNextVirtualKey = virtualKey;
        generalSettingsDirty_ = true;
        break;
    case HotkeySettingTarget::None:
        return;
    }
    renderRequested_ = true;
}

void SettingsWindow::UpdateHotkeyCapture()
{
    if (!IsHotkeyCaptureActive())
        return;

    if (IsHotkeyPhysicalKeyDown(VK_ESCAPE))
    {
        CancelHotkeyCapture();
        return;
    }

    const UINT currentModifiers = ReadHotkeyModifierMask();
    hotkeyCaptureModifiers_ |= currentModifiers;

    if (!hotkeyCapturePrimarySeen_ &&
        !hotkeyCaptureClearPending_)
    {
        if (IsHotkeyPhysicalKeyDown(VK_BACK) ||
            IsHotkeyPhysicalKeyDown(VK_DELETE))
        {
            hotkeyCaptureClearPending_ = true;
        }
        else
        {
            for (UINT virtualKey = 1;
                 virtualKey <= 0xFE;
                 ++virtualKey)
            {
                if (!IsCapturableHotkeyVirtualKey(virtualKey) ||
                    !IsHotkeyPhysicalKeyDown(virtualKey))
                    continue;
                hotkeyCaptureVirtualKey_ = virtualKey;
                hotkeyCapturePrimarySeen_ = true;
                hotkeyCapturePrimaryDown_ = true;
                break;
            }
        }
    }

    if (hotkeyCaptureClearPending_)
    {
        if (!IsHotkeyPhysicalKeyDown(VK_BACK) &&
            !IsHotkeyPhysicalKeyDown(VK_DELETE) &&
            currentModifiers == 0)
        {
            const HotkeySettingTarget target =
                hotkeyCaptureTarget_;
            CancelHotkeyCapture();
            CommitHotkeyCapture(target, 0, 0);
        }
        return;
    }

    if (!hotkeyCapturePrimarySeen_)
        return;

    if (!IsHotkeyPhysicalKeyDown(hotkeyCaptureVirtualKey_) &&
        currentModifiers == 0)
    {
        const HotkeySettingTarget target =
            hotkeyCaptureTarget_;
        const UINT modifiers = hotkeyCaptureModifiers_;
        const UINT virtualKey = hotkeyCaptureVirtualKey_;
        CancelHotkeyCapture();
        CommitHotkeyCapture(target, modifiers, virtualKey);
    }
}

void SettingsWindow::HandleHotkeyCaptureKeyMessage(
    UINT message, WPARAM virtualKeyValue)
{
    if (!IsHotkeyCaptureActive())
        return;

    const UINT virtualKey =
        static_cast<UINT>(virtualKeyValue);
    const bool keyDown =
        message == WM_KEYDOWN ||
        message == WM_SYSKEYDOWN;
    const UINT modifier =
        HotkeyModifierForVirtualKey(virtualKey);

    if (keyDown)
    {
        if (virtualKey == VK_ESCAPE)
        {
            CancelHotkeyCapture();
            return;
        }
        if (modifier != 0)
        {
            hotkeyCaptureModifiers_ |= modifier;
            hotkeyCapturePressedModifiers_ |= modifier;
            return;
        }
        if (virtualKey == VK_BACK ||
            virtualKey == VK_DELETE)
        {
            hotkeyCaptureClearPending_ = true;
            return;
        }
        if (!IsCapturableHotkeyVirtualKey(virtualKey))
            return;
        if (!hotkeyCapturePrimarySeen_)
        {
            hotkeyCaptureVirtualKey_ = virtualKey;
            hotkeyCapturePrimarySeen_ = true;
        }
        if (hotkeyCaptureVirtualKey_ == virtualKey)
            hotkeyCapturePrimaryDown_ = true;
        return;
    }

    if (modifier != 0)
        hotkeyCapturePressedModifiers_ &= ~modifier;
    if (hotkeyCaptureClearPending_ &&
        (virtualKey == VK_BACK ||
            virtualKey == VK_DELETE) &&
        hotkeyCapturePressedModifiers_ == 0 &&
        ReadHotkeyModifierMask() == 0)
    {
        const HotkeySettingTarget target =
            hotkeyCaptureTarget_;
        CancelHotkeyCapture();
        CommitHotkeyCapture(target, 0, 0);
        return;
    }
    if (hotkeyCapturePrimarySeen_ &&
        virtualKey == hotkeyCaptureVirtualKey_)
        hotkeyCapturePrimaryDown_ = false;

    if (hotkeyCapturePrimarySeen_ &&
        !hotkeyCapturePrimaryDown_ &&
        hotkeyCapturePressedModifiers_ == 0 &&
        ReadHotkeyModifierMask() == 0)
    {
        const HotkeySettingTarget target =
            hotkeyCaptureTarget_;
        const UINT modifiers = hotkeyCaptureModifiers_;
        const UINT capturedVirtualKey =
            hotkeyCaptureVirtualKey_;
        CancelHotkeyCapture();
        CommitHotkeyCapture(
            target, modifiers, capturedVirtualKey);
    }
}

HotkeySettingTarget SettingsWindow::FindInternalHotkeyConflict(
    HotkeySettingTarget target,
    UINT modifiers,
    UINT virtualKey) const
{
    if (virtualKey == 0)
        return HotkeySettingTarget::None;
    const UINT normalizedModifiers =
        modifiers &
        (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    const auto matches = [&](UINT otherModifiers,
        UINT otherVirtualKey) {
        return normalizedModifiers ==
                (otherModifiers &
                    (MOD_CONTROL | MOD_ALT |
                        MOD_SHIFT | MOD_WIN)) &&
            virtualKey == otherVirtualKey;
    };

    if (target != HotkeySettingTarget::QuickNavigation &&
        navigationSettings_.enabled &&
        matches(navigationSettings_.modifiers,
            navigationSettings_.virtualKey))
        return HotkeySettingTarget::QuickNavigation;
    if (target != HotkeySettingTarget::DesktopPassthrough &&
        generalSettings_.desktopPassthroughHotkeyEnabled &&
        matches(
            generalSettings_.desktopPassthroughHotkeyModifiers,
            generalSettings_.desktopPassthroughHotkeyVirtualKey))
        return HotkeySettingTarget::DesktopPassthrough;
    if (target != HotkeySettingTarget::FloatingDock &&
        dockEnabled_ &&
        dockSettings_.floatingShortcutMode &&
        matches(dockSettings_.floatingHotkeyModifiers,
            dockSettings_.floatingHotkeyVirtualKey))
        return HotkeySettingTarget::FloatingDock;
    if (target != HotkeySettingTarget::PagePrevious &&
        generalSettings_.pageNavigationKeyboardEnabled &&
        matches(
            generalSettings_.pageNavigationPreviousModifiers,
            generalSettings_.pageNavigationPreviousVirtualKey))
        return HotkeySettingTarget::PagePrevious;
    if (target != HotkeySettingTarget::PageNext &&
        generalSettings_.pageNavigationKeyboardEnabled &&
        matches(
            generalSettings_.pageNavigationNextModifiers,
            generalSettings_.pageNavigationNextVirtualKey))
        return HotkeySettingTarget::PageNext;
    return HotkeySettingTarget::None;
}

void SettingsWindow::DrawHotkeyRecorder(
    HotkeySettingTarget target,
    const char* label,
    const char* id,
    bool enabled,
    UINT modifiers,
    UINT virtualKey,
    UINT defaultModifiers,
    UINT defaultVirtualKey)
{
    if (!enabled && hotkeyCaptureTarget_ == target)
        CancelHotkeyCapture();

    const bool capturing = hotkeyCaptureTarget_ == target;
    const bool localDesktopHotkey =
        target == HotkeySettingTarget::PagePrevious ||
        target == HotkeySettingTarget::PageNext;
    const HotkeySettingTarget internalConflict =
        enabled && !capturing
            ? FindInternalHotkeyConflict(
                target, modifiers, virtualKey)
            : HotkeySettingTarget::None;
    const bool systemAvailable =
        localDesktopHotkey ||
        !enabled || capturing || virtualKey == 0 ||
        internalConflict != HotkeySettingTarget::None ||
        !hotkeyAvailabilityCallback_
            ? true
            : hotkeyAvailabilityCallback_(
                target, modifiers, virtualKey);

    NavigationSettings displaySettings;
    displaySettings.modifiers = modifiers;
    displaySettings.virtualKey = virtualKey;
    std::string displayText;
    if (capturing)
        displayText = _L("app.settings.hotkey_press");
    else if (virtualKey == 0)
        displayText = _L("app.settings.hotkey_not_set");
    else
        displayText = WideToUtf8(
            FormatNavigationHotkey(displaySettings));
    const std::string buttonLabel =
        displayText + id;
    const std::string resetLabel =
        std::string(_L("app.settings.restore_default")) +
        id + "Reset";

    std::string statusText;
    std::string statusDetails;
    ImVec4 statusColor;
    if (!enabled)
    {
        statusText = _L("app.settings.hotkey_status_disabled");
        statusColor = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    }
    else if (capturing)
    {
        statusText = _L("app.settings.hotkey_status_recording");
        statusDetails = _L("app.settings.hotkey_capture_active");
        statusColor = ImVec4(0.18f, 0.45f, 0.82f, 1.0f);
    }
    else if (virtualKey == 0)
    {
        statusText = _L("app.settings.hotkey_not_set");
        statusDetails = _L("app.settings.hotkey_not_set_warning");
        statusColor = ImVec4(0.82f, 0.46f, 0.08f, 1.0f);
    }
    else if (internalConflict != HotkeySettingTarget::None)
    {
        statusText = _L("app.settings.hotkey_status_conflict");
        statusDetails = _LF(
            "app.settings.hotkey_conflict_with",
            _L(HotkeyTargetLabelKey(internalConflict)));
        statusColor = ImVec4(0.80f, 0.12f, 0.12f, 1.0f);
    }
    else if (!systemAvailable)
    {
        statusText = _L("app.settings.hotkey_status_in_use");
        statusDetails = _L("app.settings.hotkey_conflict_system");
        statusColor = ImVec4(0.80f, 0.12f, 0.12f, 1.0f);
    }
    else if (modifiers == 0 && !localDesktopHotkey)
    {
        statusText = _L("app.settings.hotkey_status_no_modifier");
        statusDetails = _L("app.settings.hotkey_no_modifier_warning");
        statusColor = ImVec4(0.82f, 0.46f, 0.08f, 1.0f);
    }
    else
    {
        statusText = _L("app.settings.hotkey_status_available");
        statusDetails = _L("app.settings.hotkey_available");
        statusColor = ImVec4(0.12f, 0.58f, 0.30f, 1.0f);
    }

    const float controlWidth =
        kSettingControlWidthDip * dpiScale_;
    const float resetWidth =
        SettingButtonWidth(resetLabel.c_str());
    const float statusWidth =
        ImGui::CalcTextSize(statusText.c_str()).x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float fieldWidth = std::max(
        96.0f * dpiScale_,
        controlWidth - resetWidth - spacing);
    const float controlX = BeginSettingRow(
        label, controlWidth,
        _L("app.settings.hotkey_capture_help"));

    ImGui::SetCursorPosX(std::max(
        0.0f, controlX - statusWidth - spacing));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(statusColor, "%s", statusText.c_str());
    DrawHelpTooltip(statusDetails.c_str());
    ImGui::SameLine(controlX);

    ImGui::BeginDisabled(!enabled);
    if (capturing)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(0.18f, 0.45f, 0.82f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(0.20f, 0.49f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(0.16f, 0.40f, 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(1, 1, 1, 1));
    }
    else if (enabled &&
        (internalConflict != HotkeySettingTarget::None ||
            !systemAvailable))
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImVec4(1.0f, 0.88f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(1.0f, 0.82f, 0.82f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            ImVec4(0.97f, 0.76f, 0.76f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(0.54f, 0.08f, 0.08f, 1.0f));
    }
    else
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImGui::PushStyleColor(ImGuiCol_Button,
            style.Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            style.Colors[ImGuiCol_FrameBgHovered]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
            style.Colors[ImGuiCol_FrameBgActive]);
        ImGui::PushStyleColor(ImGuiCol_Text,
            style.Colors[ImGuiCol_Text]);
    }
    if (ImGui::Button(buttonLabel.c_str(),
            ImVec2(fieldWidth, 0)))
    {
        StartHotkeyCapture(target);
    }
    ImGui::PopStyleColor(4);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,
        ImVec4(1, 1, 1, 1));
    if (ImGui::Button(resetLabel.c_str(),
            ImVec2(resetWidth, 0)))
    {
        CancelHotkeyCapture();
        CommitHotkeyCapture(target,
            defaultModifiers, defaultVirtualKey);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
}

/**
 * @brief 绘制"布局备份"页面。
 *
 * 提供以下功能区域：
 * - 输入备份名称并保存当前布局
 * - 列出已有备份，每项提供"恢复"与"删除"按钮
 * - 恢复操作成功后触发 reloadCallback_ 通知外部重载
 */
void SettingsWindow::DrawBackupPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##BackupPageInner", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::SeparatorText(_L("app.settings.layout_backups"));
    ImGui::Spacing();

    const char* saveBackupLabel = _L("app.settings.save_backup");
    const char* openDataFolderLabel = _L("app.settings.open_data_folder");
    const float saveButtonW = std::max(
        84.0f * dpiScale_, SettingButtonWidth(saveBackupLabel));
    const float openDataButtonW = std::max(
        112.0f * dpiScale_, SettingButtonWidth(openDataFolderLabel));
    const float inputW = 180.0f * dpiScale_;
    const float controlW = inputW + saveButtonW + openDataButtonW +
        ImGui::GetStyle().ItemSpacing.x * 2.0f;
    BeginSettingRow(_L("app.settings.save_current_layout"), controlW);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputTextWithHint("##BackupName", _L("app.settings.backup_name_hint"), backupNameBuf_, sizeof(backupNameBuf_));

    ImGui::SameLine();
    if (BlueButton(saveBackupLabel, ImVec2(saveButtonW, 0)))
    {
        std::wstring name = Utf8ToWide(backupNameBuf_);
        if (name.empty()) name = MakeBackupTimestampName();
        if (SaveBackup(name))
        {
            backupNameBuf_[0] = '\0';
        }
    }
    ImGui::SameLine();
    if (BlueButton(openDataFolderLabel, ImVec2(openDataButtonW, 0)))
    {
        const std::wstring dataDir = GetDataDirectoryPath();
        ShellExecuteW(nullptr, L"open", dataDir.c_str(),
            nullptr, nullptr, SW_SHOW);
    }

    std::vector<LayoutBackup> backups = ListBackups();
    ImGui::Spacing();
    const float layoutBackupListHeight = backups.empty()
        ? 48.0f * dpiScale_
        : (std::min)(132.0f * dpiScale_,
            18.0f * dpiScale_ +
                static_cast<float>(backups.size()) *
                    ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginChild("##BackupList",
        ImVec2(0, layoutBackupListHeight), true);
    if (backups.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_backups"));
    }
    else
    {
        for (size_t i = 0; i < backups.size(); ++i)
        {
            const auto& b = backups[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = WideToUtf8(b.displayName);
            const float actionButtonW = 56.0f * dpiScale_;
            const float actionsW = actionButtonW * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            BeginSettingRow(label.c_str(), actionsW);
            if (BlueButton(_L("app.settings.restore"), ImVec2(actionButtonW, 0)))
            {
                if (RestoreBackup(b.filename) && reloadCallback_)
                    reloadCallback_();
            }
            ImGui::SameLine();
            if (BlueButton(_L("app.settings.delete"), ImVec2(actionButtonW, 0)))
            {
                DeleteBackup(b.filename);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.full_data_backups"));
    ImGui::Spacing();
    ImGui::TextWrapped("%s",
        _L("app.settings.full_data_backup_description"));
    ImGui::Spacing();

    const char* createFullLabel =
        _L("app.settings.create_full_backup");
    const char* importFullLabel =
        _L("app.settings.restore_from_backup_file");
    const char* openFullLabel =
        _L("app.settings.open_full_backup_folder");
    if (BlueButton(createFullLabel,
            ImVec2(SettingButtonWidth(createFullLabel), 0)))
    {
        CreateFullDataBackup();
    }
    ImGui::SameLine();
    if (BlueButton(importFullLabel,
            ImVec2(SettingButtonWidth(importFullLabel), 0)))
    {
        ImportFullDataBackup();
    }
    ImGui::SameLine();
    if (BlueButton(openFullLabel,
            ImVec2(SettingButtonWidth(openFullLabel), 0)))
    {
        auto manager = MakeFullBackupManager();
        std::error_code error;
        std::filesystem::create_directories(
            manager.BackupRoot(), error);
        ShellExecuteW(nullptr, L"open",
            manager.BackupRoot().c_str(), nullptr, nullptr, SW_SHOW);
    }

    const auto fullBackups = MakeFullBackupManager().List();
    ImGui::Spacing();
    const float fullBackupListHeight = fullBackups.empty()
        ? 48.0f * dpiScale_
        : (std::min)(172.0f * dpiScale_,
            18.0f * dpiScale_ +
                static_cast<float>(fullBackups.size()) *
                    ImGui::GetFrameHeightWithSpacing());
    ImGui::BeginChild("##FullBackupList",
        ImVec2(0, fullBackupListHeight), true);
    if (fullBackups.empty())
    {
        ImGui::TextDisabled("%s",
            _L("app.settings.no_full_data_backups"));
    }
    else
    {
        for (std::size_t index = 0;
            index < fullBackups.size(); ++index)
        {
            const auto& backup = fullBackups[index];
            ImGui::PushID(
                static_cast<int>(index) + 10000);
            const std::string timestamp = backup.createdAt.empty()
                ? _L("app.settings.full_backup_unknown_time")
                : backup.createdAt;
            const std::string label = backup.migrationRollback
                ? _LF("app.settings.migration_backup_item", timestamp)
                : _LF("app.settings.full_backup_item",
                    timestamp,
                    std::to_string(backup.fileCount),
                    FormatBackupSize(backup.totalBytes));
            const char* restoreLabel =
                _L("app.settings.restore");
            const char* exportLabel =
                _L("app.settings.export_backup");
            const char* openLabel =
                _L("app.settings.open");
            const char* deleteLabel =
                _L("app.settings.delete");
            const float restoreW =
                SettingButtonWidth(restoreLabel);
            const float exportW =
                SettingButtonWidth(exportLabel);
            const float openW =
                SettingButtonWidth(openLabel);
            const float deleteW =
                SettingButtonWidth(deleteLabel);
            const float actionsW =
                restoreW + exportW + openW + deleteW +
                ImGui::GetStyle().ItemSpacing.x * 3.0f;
            BeginSettingRow(label.c_str(), actionsW);
            if (BlueButton(restoreLabel, ImVec2(restoreW, 0)))
                RestoreFullDataBackup(backup);
            ImGui::SameLine();
            if (BlueButton(exportLabel, ImVec2(exportW, 0)))
                ExportFullDataBackup(backup);
            ImGui::SameLine();
            if (BlueButton(openLabel, ImVec2(openW, 0)))
            {
                ShellExecuteW(nullptr, L"open",
                    backup.root.c_str(), nullptr, nullptr, SW_SHOW);
            }
            ImGui::SameLine();
            if (BlueButton(deleteLabel, ImVec2(deleteW, 0)))
                DeleteFullDataBackup(backup);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.data_migration"));
    ImGui::Spacing();
    ImGui::TextWrapped("%s",
        _L("app.settings.data_migration_description"));
    ImGui::Spacing();
    if (BlueButton(_L("app.settings.migrate_all_data")))
        MigrateAllData();

    ImGui::EndChild();
}

/**
 * @brief 绘制"通用设置"页面。
 *
 * 提供以下配置项：
 * - 开机自启开关（通过 Windows 注册表 Run 键实现）
 * - 全局快捷导航开关、修饰键（Ctrl/Alt/Shift/Win）和主键组合选择
 * - 修改后的导航设置自动持久化并触发回调
 */
void SettingsWindow::DrawGeneralPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##GeneralPageInner", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    ImGui::SeparatorText(_L("app.settings.general_settings"));
    ImGui::Spacing();

    bool autoStart = IsAutoStartEnabled();
    if (DrawSettingCheckbox(_L("app.settings.auto_start"), "##AutoStart", &autoStart))
        SetAutoStart(autoStart);

    if (snowdesktop::deployment::IsPackaged())
    {
        const std::wstring otherAutoStart =
            ReadPortableAutoStartCommand();
        if (!otherAutoStart.empty())
        {
            ImGui::Spacing();
            const std::string warning = _LF(
                "app.settings.auto_start_other_version",
                WideToUtf8(otherAutoStart));
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(0.95f, 0.58f, 0.16f, 1.0f));
            ImGui::TextWrapped("%s", warning.c_str());
            ImGui::PopStyleColor();
            if (BlueButton(
                    _L("app.settings.auto_start_open_windows_settings")))
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            ImGui::Spacing();
        }
    }
    else
    {
        const auto installedState =
            snowdesktop::deployment::GetInstalledPackagedAutoStartState();
        if (snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                installedState))
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(
                ImGuiCol_Text, ImVec4(0.95f, 0.58f, 0.16f, 1.0f));
            ImGui::TextWrapped("%s",
                _L("app.settings.auto_start_installed_version_active"));
            ImGui::PopStyleColor();
            if (BlueButton(
                    _L("app.settings.auto_start_open_windows_settings")))
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            ImGui::Spacing();
        }
    }

    if (DrawSettingCheckbox(_L("app.settings.software_desktop"), "##SoftwareDesktopEnabled",
        &generalSettings_.softwareDesktopEnabled))
        generalSettingsDirty_ = true;

    {
        const float controlW = kSettingControlWidthDip * dpiScale_;
        BeginSettingRow(_L("app.settings.language"), controlW);
        std::vector<std::string> langNames{ "system" };
        std::vector<std::string> langLabels{ _L("app.settings.language_system") };
        for (const LanguageInfo& language :
            Locale::Instance().GetAvailableLanguages())
        {
            langNames.push_back(language.code);
            std::string label = language.displayName;
            const std::string localizedName =
                Locale::Instance().GetLocalizedLanguageName(language.code);
            if (!localizedName.empty() &&
                localizedName != language.displayName)
            {
                label += " (" + localizedName + ")";
            }
            langLabels.push_back(std::move(label));
        }
        std::vector<const char*> langLabelPointers;
        langLabelPointers.reserve(langLabels.size());
        for (const std::string& label : langLabels)
            langLabelPointers.push_back(label.c_str());
        int langIdx = 0;
        for (size_t index = 0; index < langNames.size(); ++index)
            if (langNames[index] == generalSettings_.language)
                langIdx = static_cast<int>(index);
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::Combo("##Language", &langIdx, langLabelPointers.data(),
            static_cast<int>(langLabelPointers.size())))
        {
            std::strncpy(generalSettings_.language,
                langNames[static_cast<size_t>(langIdx)].c_str(),
                sizeof(generalSettings_.language) - 1);
            generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
            Locale::Instance().SetLanguage(generalSettings_.language);
            generalSettingsDirty_ = true;
            if (languageChangedCallback_)
                languageChangedCallback_();
            renderRequested_ = true;
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.quick_navigation"));
    ImGui::Spacing();

    if (DrawSettingCheckbox(_L("app.settings.enable_global_navigation"), "##NavigationEnabled",
        &navigationSettings_.enabled))
        navigationSettingsDirty_ = true;

    DrawHotkeyRecorder(
        HotkeySettingTarget::QuickNavigation,
        _L("app.settings.hotkey"),
        "##QuickNavigationHotkeyRecorder",
        navigationSettings_.enabled,
        navigationSettings_.modifiers,
        navigationSettings_.virtualKey,
        MOD_CONTROL | MOD_ALT,
        VK_SPACE);

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.desktop_interact"));
    ImGui::Spacing();

    if (DrawSettingCheckbox(
            _L("app.settings.page_navigation_keyboard"),
            "##PageNavigationKeyboardEnabled",
            &generalSettings_.pageNavigationKeyboardEnabled))
        generalSettingsDirty_ = true;
    ImGui::Indent();
    DrawHotkeyRecorder(
        HotkeySettingTarget::PagePrevious,
        _L("app.settings.page_navigation_previous"),
        "##PageNavigationPreviousKey",
        generalSettings_.pageNavigationKeyboardEnabled,
        generalSettings_.pageNavigationPreviousModifiers,
        generalSettings_.pageNavigationPreviousVirtualKey,
        0, VK_PRIOR);
    DrawHotkeyRecorder(
        HotkeySettingTarget::PageNext,
        _L("app.settings.page_navigation_next"),
        "##PageNavigationNextKey",
        generalSettings_.pageNavigationKeyboardEnabled,
        generalSettings_.pageNavigationNextModifiers,
        generalSettings_.pageNavigationNextVirtualKey,
        0, VK_NEXT);
    ImGui::Unindent();
    ImGui::Spacing();

    if (DrawSettingCheckbox(
            _L("app.settings.desktop_passthrough_hotkey"),
            "##DesktopPassthroughHotkeyEnabled",
            &generalSettings_.desktopPassthroughHotkeyEnabled))
        generalSettingsDirty_ = true;
    if (generalSettings_.desktopPassthroughHotkeyEnabled)
    {
        ImGui::Indent();
        ImGui::TextDisabled(
            "%s",
            _L("app.settings.desktop_passthrough_hotkey_hint"));
        ImGui::Unindent();
    }

    ImGui::Indent();
    DrawHotkeyRecorder(
        HotkeySettingTarget::DesktopPassthrough,
        _L("app.settings.hotkey"),
        "##DesktopPassthroughHotkeyRecorder",
        generalSettings_.desktopPassthroughHotkeyEnabled,
        generalSettings_.desktopPassthroughHotkeyModifiers,
        generalSettings_.desktopPassthroughHotkeyVirtualKey,
        MOD_CONTROL | MOD_ALT,
        VK_OEM_3);
    ImGui::Unindent();
    ImGui::Spacing();

    if (DrawSettingCheckbox(_L("app.settings.double_click_hide"), "##DoubleClickHideDesktop",
        &generalSettings_.doubleClickHideDesktop))
        generalSettingsDirty_ = true;

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.dock_bar"));
    ImGui::Spacing();
    DrawDockPage();

    ImGui::EndChild();
}

/**
 * @brief 绘制通用页中的 Dock 设置区域。
 */
void SettingsWindow::DrawDockPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    const std::string thicknessResetLabel =
        std::string(_L("app.settings.restore_default")) + "##DockThicknessDefault";
    const float resetW = SettingButtonWidth(thicknessResetLabel.c_str());
    const float sliderActionW = controlW;
    const float actionSliderW = std::max(1.0f,
        sliderActionW - ImGui::GetStyle().ItemSpacing.x - resetW);
    auto markChanged = [&](bool saveImmediately) {
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        if (saveImmediately)
            dockSettingsSaveRequested_ = true;
    };

    if (DrawSettingCheckbox(_L("app.dock.enable"), "##DockEnabled", &dockEnabled_))
    {
        if (dockEnabledChangedCallback_)
            dockEnabledChangedCallback_(dockEnabled_);
    }

    ImGui::BeginDisabled(!dockEnabled_);
    ImGui::Spacing();
    if (DrawSettingCheckbox(_L("app.dock.floating_shortcut_mode"),
        "##DockFloatingShortcutMode",
        &dockSettings_.floatingShortcutMode))
        markChanged(true);
    if (dockSettings_.floatingShortcutMode)
    {
        ImGui::Indent();
        ImGui::TextDisabled("%s", _L("app.dock.floating_shortcut_hint"));
        ImGui::Unindent();
    }

    ImGui::Spacing();
    ImGui::Indent();
    DrawHotkeyRecorder(
        HotkeySettingTarget::FloatingDock,
        _L("app.settings.hotkey"),
        "##FloatingDockHotkeyRecorder",
        dockEnabled_ &&
            dockSettings_.floatingShortcutMode,
        dockSettings_.floatingHotkeyModifiers,
        dockSettings_.floatingHotkeyVirtualKey,
        MOD_CONTROL | MOD_ALT,
        'D');
    ImGui::Unindent();

    ImGui::Spacing();
    if (DrawSettingCheckbox(
            _L("app.dock.floating_edge_swipe"),
            "##DockFloatingEdgeSwipe",
            &dockSettings_.floatingEdgeSwipeEnabled))
        markChanged(true);
    if (dockSettings_.floatingEdgeSwipeEnabled)
    {
        ImGui::TextDisabled(
            "%s",
            _L("app.dock.floating_edge_swipe_hint"));
    }

    ImGui::Spacing();
    BeginSettingRow(_L("app.settings.dock_position"), controlW);
    const char* positionNames[] = { _L("app.dock.bottom"), _L("app.dock.top"), _L("app.dock.left"), _L("app.dock.right") };
    int position = std::clamp(static_cast<int>(dockSettings_.position), 0, 3);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockPosition", &position, positionNames, IM_ARRAYSIZE(positionNames)))
    {
        dockSettings_.position = static_cast<DockPosition>(position);
        markChanged(true);
    }

    BeginSettingRow(_L("app.settings.display_scope"), controlW);
    const char* monitorScopeNames[] = { _L("app.dock.first_screen"), _L("app.dock.last_screen"), _L("app.dock.all_screens") };
    int monitorScope = std::clamp(
        static_cast<int>(dockSettings_.monitorScope), 0, 2);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockMonitorScope", &monitorScope,
        monitorScopeNames, IM_ARRAYSIZE(monitorScopeNames)))
    {
        dockSettings_.monitorScope = static_cast<DockMonitorScope>(monitorScope);
        markChanged(true);
    }
    BeginSettingRow(_L("app.dock.layout"), controlW);
    const char* layoutNames[] = { _L("app.dock.island"), _L("app.dock.edge") };
    int layoutMode = dockSettings_.edgeAttached ? 1 : 0;
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockLayoutMode", &layoutMode, layoutNames, IM_ARRAYSIZE(layoutNames)))
    {
        dockSettings_.edgeAttached = layoutMode == 1;
        markChanged(true);
    }
    BeginSettingRow(_L("app.settings.dock_thickness"), sliderActionW,
        _L("app.settings.dock_thickness_hint"));
    ImGui::SetNextItemWidth(actionSliderW);
    int thicknessPercent = static_cast<int>(std::round(
        dockSettings_.thicknessScale * 100.0f));
    if (ImGui::SliderInt("##DockThickness", &thicknessPercent, 50, 100, "%d%%"))
    {
        dockSettings_.thicknessScale = thicknessPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        dockSettingsDirty_)
        dockSettingsSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton(thicknessResetLabel.c_str(), ImVec2(resetW, 0)))
    {
        dockSettings_.thicknessScale = 1.0f;
        markChanged(true);
    }

    if (DrawSettingCheckbox(_L("app.dock.show_windows_button"), "##DockShowWindowsButton",
        &dockSettings_.showWindowsButton))
        markChanged(true);

    if (DrawSettingCheckbox(_L("app.dock.show_frequent_items"), "##DockShowFrequentItems",
        &dockSettings_.showFrequentItems))
        markChanged(true);

    ImGui::BeginDisabled(!dockSettings_.showFrequentItems);
    BeginSettingRow(_L("app.settings.show_count"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::SliderInt("##DockFrequentItemCount",
        &dockSettings_.frequentItemCount, 1, 8, _L("app.settings.items_unit")))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        dockSettingsDirty_)
        dockSettingsSaveRequested_ = true;
    ImGui::EndDisabled();

    ImGui::EndDisabled();

}

/**
 * @brief 绘制外观页中的 Windows 系统任务栏设置区域。
 */
void SettingsWindow::DrawSystemTaskbarPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    auto markChanged = [&]() {
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        dockSettingsSaveRequested_ = true;
    };

    if (DrawSettingCheckbox(_L("app.settings.auto_hide_taskbar"), "##SystemTaskbarAutoHide",
        &dockSettings_.systemTaskbarAutoHide))
    {
        RequestSystemTaskbarAutoHideEnabled(
            dockSettings_.systemTaskbarAutoHide);
        markChanged();
    }

    BeginSettingRow(_L("app.settings.taskbar_alignment"), controlW,
        _L("app.settings.taskbar_alignment_hint"));
    const char* alignmentNames[] = { _L("app.settings.taskbar_left"), _L("app.settings.taskbar_center") };
    int alignment = std::clamp(dockSettings_.systemTaskbarAlignment, 0, 1);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##SystemTaskbarAlignment", &alignment,
        alignmentNames, IM_ARRAYSIZE(alignmentNames)))
    {
        dockSettings_.systemTaskbarAlignment = alignment;
        RequestSystemTaskbarAlignmentCentered(alignment == 1);
        markChanged();
    }

    const std::string restartExplorerLabel =
        std::string(_L("app.settings.restart_explorer")) + "##WindowsTheme";
    const float restartExplorerButtonW =
        SettingButtonWidth(restartExplorerLabel.c_str());
    const float windowsThemeComboW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - restartExplorerButtonW);
    BeginSettingRow(_L("app.settings.system_panel"), controlW,
        (std::string(_L("app.settings.system_panel_hint")) + " "
         + _L("app.settings.system_panel_hint2")).c_str());
    const char* windowsThemeNames[] = {
        _L("app.settings.light"), _L("app.settings.dark")
    };
    int windowsTheme = IsWindowsSystemLightThemeEnabled() ? 0 : 1;
    ImGui::SetNextItemWidth(windowsThemeComboW);
    if (ImGui::Combo("##WindowsSystemTheme", &windowsTheme,
        windowsThemeNames, IM_ARRAYSIZE(windowsThemeNames)))
    {
        RequestWindowsSystemLightThemeEnabled(windowsTheme == 0);
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        dockSettingsSaveRequested_ = true;
    }
    ImGui::SameLine();
    if (BlueButton(restartExplorerLabel.c_str()))
    {
        if (!RestartWindowsExplorer())
            MessageBoxW(hwnd_, _LW("app.interact.restart_explorer_fail"),
                L"SnowDesktop", MB_OK | MB_ICONWARNING);
    }
}

/**
 * @brief 绘制外观页中的图标显示设置区域。
 */
void SettingsWindow::DrawDisplayPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    const float sliderW = controlW;
    const float resetW = 84.0f * dpiScale_;
    const float sliderActionW = controlW;
    const float actionSliderW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - resetW);

    auto markChanged = [&]() {
        if (displaySettingsChangedCallback_)
            displaySettingsChangedCallback_();
    };

    auto notifyIconBeautify = [&](snowdesktop::IconBeautifyUpdateKind updateKind) {
        if (iconBeautifySettingsChangedCallback_)
            iconBeautifySettingsChangedCallback_(updateKind);
    };
    auto previewIconBeautify = [&]() {
        const auto action = snowdesktop::icon_beautify::
            AdvanceContinuousPreview(iconBeautifyContinuousState_,
                true, false, GetTickCount());
        if (action == snowdesktop::icon_beautify::InteractionAction::Preview)
            notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Preview);
    };
    auto commitContinuousIconBeautify = [&]() {
        const auto action = snowdesktop::icon_beautify::
            AdvanceContinuousPreview(iconBeautifyContinuousState_,
                false, ImGui::IsItemDeactivatedAfterEdit(), GetTickCount());
        if (action == snowdesktop::icon_beautify::InteractionAction::Commit)
            notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);
    };

    BeginSettingRow(_L("app.settings.layout_spacing"), sliderActionW,
        _L("app.settings.layout_spacing_hint"));
    ImGui::SetNextItemWidth(actionSliderW);
    const bool spacingChanged = ImGui::SliderInt(
        "##IconSpacing", &displaySpacingPct_, 50, 200,
        "%d%%", ImGuiSliderFlags_None);
    if (spacingChanged)
    {
        iconSpacingScale_ = displaySpacingPct_ / 100.0f;
        if (layoutSpacingChangedCallback_)
            layoutSpacingPreviewPending_ = true;
        else
            markChanged();
    }
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        layoutSpacingChangedCallback_)
    {
        layoutSpacingPreviewPending_ = false;
        layoutSpacingCommitPending_ = true;
    }
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##IconSpacingDefault").c_str(), ImVec2(resetW, 0)))
    {
        displaySpacingPct_ = 100;
        iconSpacingScale_ = 1.0f;
        if (layoutSpacingChangedCallback_)
        {
            layoutSpacingPreviewPending_ = false;
            layoutSpacingCommitPending_ = true;
        }
        else
            markChanged();
    }

    BeginSettingRow(_L("app.settings.icon_size"), sliderActionW,
        _L("app.settings.icon_size_hint"));
    ImGui::SetNextItemWidth(actionSliderW);
    const bool iconSizeChanged = ImGui::SliderInt(
        "##ItemIconSize", &displayIconSizePct_,
        static_cast<int>(std::round(
            kMinimumItemIconSizeScale * 100.0f)),
        static_cast<int>(std::round(
            kMaximumItemIconSizeScale * 100.0f)),
        "%d%%", ImGuiSliderFlags_None);
    if (iconSizeChanged)
    {
        itemIconSizeScale_ = displayIconSizePct_ / 100.0f;
        if (itemIconSizeChangedCallback_)
            itemIconSizePreviewPending_ = true;
        else
            markChanged();
    }
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        itemIconSizeChangedCallback_)
    {
        itemIconSizePreviewPending_ = false;
        itemIconSizeCommitPending_ = true;
    }
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ItemIconSizeDefault").c_str(), ImVec2(resetW, 0)))
    {
        displayIconSizePct_ = static_cast<int>(std::round(
            kDefaultItemIconSizeScale * 100.0f));
        itemIconSizeScale_ = kDefaultItemIconSizeScale;
        if (itemIconSizeChangedCallback_)
        {
            itemIconSizePreviewPending_ = false;
            itemIconSizeCommitPending_ = true;
        }
        else
            markChanged();
    }

    BeginSettingRow(_L("app.settings.title_font_size"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##ItemFontSize", &itemFontSizeCu_,
        kMinimumItemFontSizeCu, kMaximumItemFontSizeCu, "%.1f cu"))
        markChanged();
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ItemFontSizeDefault").c_str(), ImVec2(resetW, 0)))
    {
        itemFontSizeCu_ = kDefaultItemFontSizeCu;
        markChanged();
    }

    BeginSettingRow(_L("app.settings.list_font_size"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##ListItemFontSize", &listItemFontSizeCu_,
        kMinimumItemFontSizeCu, kMaximumItemFontSizeCu, "%.1f cu"))
        markChanged();
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ListItemFontSizeDefault").c_str(), ImVec2(resetW, 0)))
    {
        listItemFontSizeCu_ = kDefaultItemFontSizeCu;
        markChanged();
    }

    BeginSettingRow(_L("app.settings.title_font_weight"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##ItemFontWeight", &itemFontWeight_,
        100.0f, 900.0f, "%.0f"))
        markChanged();
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ItemFontWeightDefault").c_str(), ImVec2(resetW, 0)))
    {
        itemFontWeight_ = 600.0f;
        markChanged();
    }

    ImGui::Spacing();
    const char* shortcutArrowModeNames[] = {
        _L("app.settings.arrow_default"),
        _L("app.settings.arrow_hide_all"),
        _L("app.settings.arrow_show_all"),
    };
    BeginSettingRow(_L("app.settings.shortcut_arrow"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##ShortcutArrowMode", &shortcutArrowMode_,
        shortcutArrowModeNames, static_cast<int>(sizeof(shortcutArrowModeNames) / sizeof(shortcutArrowModeNames[0]))))
    {
        markChanged();
    }

    ImGui::Spacing();
    const char* presetNames[] = {
        _L("app.settings.beautify_preset_none"),
        _L("app.settings.beautify_preset_default"),
        _L("app.settings.custom"),
    };
    constexpr snowdesktop::IconBeautifyPreset presetIds[] = {
        snowdesktop::IconBeautifyPreset::None,
        snowdesktop::IconBeautifyPreset::DefaultBeautify,
        snowdesktop::IconBeautifyPreset::Custom,
    };
    int presetIndex = 0;
    for (int i = 0; i < IM_ARRAYSIZE(presetIds); ++i)
        if (presetIds[i] == iconBeautifySettings_.preset) { presetIndex = i; break; }
    BeginSettingRow(_L("app.settings.icon_beautify"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyPreset", &presetIndex,
            presetNames, IM_ARRAYSIZE(presetNames)))
    {
        iconBeautifySettings_.preset = presetIds[presetIndex];
        if (iconBeautifySettings_.preset == snowdesktop::IconBeautifyPreset::Custom)
            iconBeautifySettings_.enabled = true;
        else
            iconBeautifySettings_ = snowdesktop::icon_beautify::MakePreset(
                iconBeautifySettings_.preset);
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);
    }

    if (iconBeautifySettings_.preset == snowdesktop::IconBeautifyPreset::Custom)
    {
    ImGui::Spacing();
    ImGui::Indent(8.0f * dpiScale_);

    const char* beautifyModeNames[] = {
        _L("app.settings.beautify_smart"),
        _L("app.settings.beautify_shrink_bg"),
    };
    BeginSettingRow(_L("app.settings.beautify_mode"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyMode", &iconBeautifySettings_.mode,
            beautifyModeNames, IM_ARRAYSIZE(beautifyModeNames)))
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);

    float bgStart[3] = {
        iconBeautifySettings_.backgroundStartR,
        iconBeautifySettings_.backgroundStartG,
        iconBeautifySettings_.backgroundStartB };
    if (DrawSettingColorEdit3(_L("app.settings.default_bg"),
            "##IconBeautifyBgStart", bgStart))
    {
        iconBeautifySettings_.backgroundStartR = bgStart[0];
        iconBeautifySettings_.backgroundStartG = bgStart[1];
        iconBeautifySettings_.backgroundStartB = bgStart[2];
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    BeginSettingRow(_L("app.settings.bg_opacity_val"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int backgroundOpacityPercent = static_cast<int>(std::round(
        iconBeautifySettings_.backgroundOpacity * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyBgOpacity",
            &backgroundOpacityPercent, 0, 100, "%d%%"))
    {
        iconBeautifySettings_.backgroundOpacity =
            backgroundOpacityPercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    if (DrawSettingCheckbox(_L("app.settings.enable_gradient_bg"),
            "##IconBeautifyGradient",
            &iconBeautifySettings_.gradientEnabled))
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);

    ImGui::BeginDisabled(!iconBeautifySettings_.gradientEnabled);
    float bgEnd[3] = {
        iconBeautifySettings_.backgroundEndR,
        iconBeautifySettings_.backgroundEndG,
        iconBeautifySettings_.backgroundEndB };
    if (DrawSettingColorEdit3(_L("app.settings.gradient_end_color"),
            "##IconBeautifyBgEnd", bgEnd))
    {
        iconBeautifySettings_.backgroundEndR = bgEnd[0];
        iconBeautifySettings_.backgroundEndG = bgEnd[1];
        iconBeautifySettings_.backgroundEndB = bgEnd[2];
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    const char* directionNames[] = {
        _L("app.settings.beautify_gradient_updown"),
        _L("app.settings.beautify_gradient_leftright"),
        _L("app.settings.beautify_gradient_topleft_bottomright"),
        _L("app.settings.beautify_gradient_bottomleft_topright"),
    };
    BeginSettingRow(_L("app.settings.beautify_gradient_dir"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyGradientDirection",
            &iconBeautifySettings_.gradientDirection,
            directionNames, IM_ARRAYSIZE(directionNames)))
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);
    ImGui::EndDisabled();

    const char* shapeNames[] = {
        _L("app.settings.beautify_shape_legacy"),
        _L("app.settings.beautify_shape_continuous_rounded"),
        _L("app.settings.beautify_shape_soft_rounded"),
        _L("app.settings.beautify_shape_circle"),
        _L("app.settings.beautify_shape_pebble"),
    };
    constexpr snowdesktop::IconBeautifyShape shapeIds[] = {
        snowdesktop::IconBeautifyShape::LegacyRounded,
        snowdesktop::IconBeautifyShape::ContinuousRounded,
        snowdesktop::IconBeautifyShape::SoftRounded,
        snowdesktop::IconBeautifyShape::Circle,
        snowdesktop::IconBeautifyShape::Pebble,
    };
    int shapeIndex = 0;
    for (int i = 0; i < IM_ARRAYSIZE(shapeIds); ++i)
        if (shapeIds[i] == iconBeautifySettings_.shape) { shapeIndex = i; break; }
    BeginSettingRow(_L("app.settings.beautify_shape"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyShape", &shapeIndex,
            shapeNames, IM_ARRAYSIZE(shapeNames)))
    {
        iconBeautifySettings_.shape = shapeIds[shapeIndex];
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);
    }

    BeginSettingRow(_L("app.settings.beautify_content_scale"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int contentScalePercent = static_cast<int>(std::round(
        iconBeautifySettings_.contentScale * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyContentScale",
            &contentScalePercent, 50, 90, "%d%%"))
    {
        iconBeautifySettings_.contentScale = contentScalePercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    BeginSettingRow(_L("app.settings.beautify_texture_highlight"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int textureHighlightPercent = static_cast<int>(std::round(
        iconBeautifySettings_.textureHighlightStrength * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyTextureHighlight",
            &textureHighlightPercent, 0, 100, "%d%%"))
    {
        iconBeautifySettings_.textureHighlightStrength =
            textureHighlightPercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    BeginSettingRow(_L("app.settings.beautify_texture_highlight_size"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int textureHighlightSizePercent = static_cast<int>(std::round(
        iconBeautifySettings_.textureHighlightSize * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyTextureHighlightSize",
            &textureHighlightSizePercent, 10, 100, "%d%%"))
    {
        iconBeautifySettings_.textureHighlightSize =
            textureHighlightSizePercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    BeginSettingRow(_L("app.settings.beautify_texture_highlight_angle"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int textureHighlightAngle = static_cast<int>(std::round(
        iconBeautifySettings_.textureHighlightAngle * 45.0f));
    if (ImGui::SliderInt("##IconBeautifyTextureHighlightAngle",
            &textureHighlightAngle, -45, 45, "%d°"))
    {
        iconBeautifySettings_.textureHighlightAngle =
            textureHighlightAngle / 45.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    BeginSettingRow(_L("app.settings.beautify_texture_shade"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int textureShadePercent = static_cast<int>(std::round(
        iconBeautifySettings_.textureShadeStrength * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyTextureShade",
            &textureShadePercent, 0, 100, "%d%%"))
    {
        iconBeautifySettings_.textureShadeStrength =
            textureShadePercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    BeginSettingRow(_L("app.settings.beautify_texture_edge"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int textureEdgePercent = static_cast<int>(std::round(
        iconBeautifySettings_.textureEdgeHighlight * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyTextureEdge",
            &textureEdgePercent, 0, 100, "%d%%"))
    {
        iconBeautifySettings_.textureEdgeHighlight =
            textureEdgePercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    if (DrawSettingCheckbox(_L("app.settings.beautify_filter"),
            "##IconBeautifyFilterEnabled",
            &iconBeautifySettings_.filterEnabled))
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);
    if (iconBeautifySettings_.filterEnabled)
    {
        float filterTint[3] = {
            iconBeautifySettings_.filterTintR,
            iconBeautifySettings_.filterTintG,
            iconBeautifySettings_.filterTintB };
        if (DrawSettingColorEdit3(_L("app.settings.beautify_filter_color"),
                "##IconBeautifyFilterTint", filterTint))
        {
            iconBeautifySettings_.filterTintR = filterTint[0];
            iconBeautifySettings_.filterTintG = filterTint[1];
            iconBeautifySettings_.filterTintB = filterTint[2];
            previewIconBeautify();
        }
        commitContinuousIconBeautify();

        BeginSettingRow(_L("app.settings.beautify_filter_strength"), sliderW);
        ImGui::SetNextItemWidth(sliderW);
        int filterStrengthPercent = static_cast<int>(std::round(
            iconBeautifySettings_.filterStrength * 100.0f));
        if (ImGui::SliderInt("##IconBeautifyFilterStrength",
                &filterStrengthPercent, 0, 100, "%d%%"))
        {
            iconBeautifySettings_.filterStrength =
                filterStrengthPercent / 100.0f;
            previewIconBeautify();
        }
        commitContinuousIconBeautify();
    }

    BeginSettingRow(_L("app.settings.beautify_shadow_strength"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int shadowPercent = static_cast<int>(std::round(
        iconBeautifySettings_.shadowStrength * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyShadowStrength",
            &shadowPercent, 0, 100, "%d%%"))
    {
        iconBeautifySettings_.shadowStrength = shadowPercent / 100.0f;
        previewIconBeautify();
    }
    commitContinuousIconBeautify();

    if (DrawSettingCheckbox(_L("app.settings.beautify_outline"),
            "##IconBeautifyOutlineEnabled",
            &iconBeautifySettings_.outlineEnabled))
        notifyIconBeautify(snowdesktop::IconBeautifyUpdateKind::Commit);
    if (iconBeautifySettings_.outlineEnabled)
    {
        BeginSettingRow(_L("app.settings.beautify_outline_width"), sliderW);
        ImGui::SetNextItemWidth(sliderW);
        if (ImGui::SliderFloat("##IconBeautifyOutlineWidth",
                &iconBeautifySettings_.outlineWidth, 0.0f, 4.0f, "%.1f px"))
            previewIconBeautify();
        commitContinuousIconBeautify();

        BeginSettingRow(_L("app.settings.beautify_outline_opacity"), sliderW);
        ImGui::SetNextItemWidth(sliderW);
        int outlineOpacityPercent = static_cast<int>(std::round(
            iconBeautifySettings_.outlineOpacity * 100.0f));
        if (ImGui::SliderInt("##IconBeautifyOutlineOpacity",
                &outlineOpacityPercent, 0, 100, "%d%%"))
        {
            iconBeautifySettings_.outlineOpacity =
                outlineOpacityPercent / 100.0f;
            previewIconBeautify();
        }
        commitContinuousIconBeautify();

        float outlineColor[3] = {
            iconBeautifySettings_.outlineR,
            iconBeautifySettings_.outlineG,
            iconBeautifySettings_.outlineB };
        if (DrawSettingColorEdit3(_L("app.settings.beautify_outline_color"),
                "##IconBeautifyOutlineColor", outlineColor))
        {
            iconBeautifySettings_.outlineR = outlineColor[0];
            iconBeautifySettings_.outlineG = outlineColor[1];
            iconBeautifySettings_.outlineB = outlineColor[2];
            previewIconBeautify();
        }
        commitContinuousIconBeautify();
    }
    ImGui::Unindent(8.0f * dpiScale_);
    }
}

void SettingsWindow::DispatchPendingLayoutSpacingChange()
{
    const auto action = snowdesktop::layout_spacing_rules::
        ResolveDeferredChangeAction(
            layoutSpacingPreviewPending_,
            layoutSpacingCommitPending_);
    if (action == snowdesktop::layout_spacing_rules::
            DeferredChangeAction::None)
        return;

    // Clear before invoking application code so a synchronous desktop paint
    // cannot redispatch this value through a nested window message.
    layoutSpacingPreviewPending_ = false;
    layoutSpacingCommitPending_ = false;
    if (layoutSpacingChangedCallback_)
    {
        layoutSpacingChangedCallback_(
            iconSpacingScale_,
            action == snowdesktop::layout_spacing_rules::
                DeferredChangeAction::Commit);
    }
}

void SettingsWindow::DispatchPendingItemIconSizeChange()
{
    const auto action = snowdesktop::layout_spacing_rules::
        ResolveDeferredChangeAction(
            itemIconSizePreviewPending_,
            itemIconSizeCommitPending_);
    if (action == snowdesktop::layout_spacing_rules::
            DeferredChangeAction::None)
        return;

    itemIconSizePreviewPending_ = false;
    itemIconSizeCommitPending_ = false;
    if (itemIconSizeChangedCallback_)
    {
        itemIconSizeChangedCallback_(
            itemIconSizeScale_,
            action == snowdesktop::layout_spacing_rules::
                DeferredChangeAction::Commit);
    }
}

void SettingsWindow::SyncCategoryRuleBuffersFromSettings()
{
    categoryRuleBuffers_.clear();
    categoryRuleBuffers_.reserve(categorySettings_.rules.size());
    for (const CategoryRule& rule : categorySettings_.rules)
    {
        CategoryRuleEditBuffer buffer;
        buffer.id = rule.id;
        buffer.usesDefaultLabel =
            rule.customLabel.empty() && IsBuiltinCategoryRuleId(rule.id);
        CopyWideToUtf8Buffer(GetCategoryLabel(categorySettings_, rule.id),
            buffer.label, sizeof(buffer.label));
        CopyWideToUtf8Buffer(rule.extensions, buffer.extensions, sizeof(buffer.extensions));
        categoryRuleBuffers_.push_back(std::move(buffer));
    }
}

void SettingsWindow::NormalizeCategoryRuleBuffers()
{
    categorySettings_.rules.clear();
    categorySettings_.rules.reserve(categoryRuleBuffers_.size());
    for (const CategoryRuleEditBuffer& buffer : categoryRuleBuffers_)
    {
        CategoryRule rule;
        rule.id = buffer.id;
        const std::wstring editedLabel = TrimWide(Utf8ToWide(buffer.label));
        if (!buffer.usesDefaultLabel)
            rule.customLabel = editedLabel;
        if (rule.customLabel.empty() && !IsBuiltinCategoryRuleId(rule.id))
            rule.customLabel = _LW("widget.categories.unnamed");
        rule.extensions = NormalizeCategoryExtensionText(Utf8ToWide(buffer.extensions));
        categorySettings_.rules.push_back(std::move(rule));
    }
    SyncCategoryRuleBuffersFromSettings();
}

/**
 * @brief 绘制"分类设置"页面。
 */
void SettingsWindow::DrawCategorySettingsPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##CategorySettingsPageInner", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    const float inputW = kSettingControlWidthDip * dpiScale_;

    auto markChanged = [&]() {
        categorySettingsDirty_ = true;
        categorySettingsSavedTick_ = 0;
    };

    const float subsectionContentIndent = 16.0f * dpiScale_;
    auto drawSubsectionLabel = [](const char* label, const char* description) {
        ImGui::TextUnformatted(label);
        if (description && description[0])
            DrawHelpMarker(description);
    };

    ImGui::SeparatorText(_L("app.settings.category_settings"));
    ImGui::Spacing();

    drawSubsectionLabel(_L("app.settings.category_type"),
        _L("app.settings.category_hint"));
    ImGui::Indent(subsectionContentIndent);
    ImGui::Spacing();

    int deleteIndex = -1;
    for (size_t i = 0; i < categoryRuleBuffers_.size(); ++i)
    {
        CategoryRuleEditBuffer& buffer = categoryRuleBuffers_[i];
        ImGui::PushID(static_cast<int>(i));

        const float actionWidth = 56.0f * dpiScale_;
        const float nameInputW = std::max(1.0f,
            inputW - actionWidth - ImGui::GetStyle().ItemSpacing.x);
        BeginSettingRow(_L("app.settings.category_name"), inputW);
        ImGui::SetNextItemWidth(nameInputW);
        if (ImGui::InputText("##CategoryLabel", buffer.label, sizeof(buffer.label)))
        {
            buffer.usesDefaultLabel = false;
            markChanged();
        }

        ImGui::SameLine();
        if (BlueButton(_L("app.settings.delete"), ImVec2(56.0f * dpiScale_, 0)))
            deleteIndex = static_cast<int>(i);

        BeginSettingRow(_L("app.settings.category_extensions"), inputW);
        ImGui::SetNextItemWidth(inputW);
        if (ImGui::InputText("##CategoryExtensions", buffer.extensions, sizeof(buffer.extensions)))
            markChanged();

        ImGui::Spacing();
        ImGui::PopID();
    }

    if (deleteIndex >= 0 && static_cast<size_t>(deleteIndex) < categoryRuleBuffers_.size())
    {
        categoryRuleBuffers_.erase(categoryRuleBuffers_.begin() + deleteIndex);
        markChanged();
    }

    ImGui::Unindent(subsectionContentIndent);
    ImGui::Spacing();
    drawSubsectionLabel(_L("app.settings.add_category"), nullptr);
    ImGui::Indent(subsectionContentIndent);
    ImGui::Spacing();

    const float actionWidth = 56.0f * dpiScale_;
    const float nameInputW = std::max(1.0f,
        inputW - actionWidth - ImGui::GetStyle().ItemSpacing.x);
    BeginSettingRow(_L("app.settings.category_name"), inputW);
    ImGui::SetNextItemWidth(nameInputW);
    ImGui::InputText("##NewCategoryLabel", newCategoryLabelBuf_, sizeof(newCategoryLabelBuf_));

    ImGui::SameLine();
    if (BlueButton(_L("app.settings.add"), ImVec2(56.0f * dpiScale_, 0)))
    {
        CategoryRuleEditBuffer buffer;
        std::wstring id = L"custom-" + std::to_wstring(GetTickCount64());
        bool unique = false;
        int suffix = 2;
        while (!unique)
        {
            unique = true;
            for (const auto& existing : categoryRuleBuffers_)
            {
                if (existing.id == id)
                {
                    unique = false;
                    id = L"custom-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(suffix++);
                    break;
                }
            }
        }
        buffer.id = id;

        std::wstring label = TrimWide(Utf8ToWide(newCategoryLabelBuf_));
        if (label.empty())
            label = _LW("app.settings.new_category");
        std::wstring extensions = NormalizeCategoryExtensionText(Utf8ToWide(newCategoryExtensionsBuf_));
        CopyWideToUtf8Buffer(label, buffer.label, sizeof(buffer.label));
        CopyWideToUtf8Buffer(extensions, buffer.extensions, sizeof(buffer.extensions));
        categoryRuleBuffers_.push_back(std::move(buffer));
        newCategoryLabelBuf_[0] = '\0';
        newCategoryExtensionsBuf_[0] = '\0';
        markChanged();
    }

    BeginSettingRow(_L("app.settings.category_extensions"), inputW);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputText("##NewCategoryExtensions", newCategoryExtensionsBuf_, sizeof(newCategoryExtensionsBuf_));

    ImGui::Unindent(subsectionContentIndent);
    ImGui::Spacing();
    drawSubsectionLabel(_L("app.settings.save_settings"), nullptr);
    ImGui::Indent(subsectionContentIndent);
    ImGui::Spacing();

    const float applyButtonW = 80.0f * dpiScale_;
    const float restoreButtonW = 96.0f * dpiScale_;
    const float saveActionsW = applyButtonW + ImGui::GetStyle().ItemSpacing.x + restoreButtonW;
    BeginSettingRow(_L("app.settings.category_rules"), saveActionsW);
    if (BlueButton(_L("app.settings.apply"), ImVec2(applyButtonW, 0)))
    {
        categorySettingsDirty_ = true;
        categorySettingsSaveRequested_ = true;
    }
    ImGui::SameLine();
    if (BlueButton(_L("app.settings.restore_default"), ImVec2(restoreButtonW, 0)))
    {
        categorySettings_ = CategorySettings::Defaults();
        SyncCategoryRuleBuffersFromSettings();
        markChanged();
        categorySettingsSaveRequested_ = true;
    }

    if (categorySettingsDirty_)
    {
        DrawSettingValue(_L("app.settings.save_status"), _L("app.settings.save_unsaved"));
    }
    else if (categorySettingsSavedTick_ != 0 && GetTickCount() - categorySettingsSavedTick_ < 2500)
    {
        DrawSettingValue(_L("app.settings.save_status"), _L("app.settings.saved"));
    }

    ImGui::Unindent(subsectionContentIndent);
    ImGui::EndChild();
}

/**
 * @brief 绘制统一外观页面。
 *
 * 提供以下定制能力：
 * - 六种全局主题快速切换与自定义参数调整
 * - Dock 固定继承、快捷搜索自定义主题与系统任务栏覆盖
 * - 修改立即通知桌面预览；连续拖动结束后再持久化
 */
void SettingsWindow::DrawPersonalizationPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##PersonalizationPageInner", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    auto markChanged = [&](bool saveImmediately) {
        personalizationDirty_ = true;
        personalizationPreviewDirty_ = true;
        if (saveImmediately)
            personalizationSaveRequested_ = true;
    };

    const float controlW = kSettingControlWidthDip * dpiScale_;
    const float sliderW = controlW;
    const float resetW = 84.0f * dpiScale_;
    const float sliderActionW = controlW;
    const float actionSliderW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - resetW);
    ImGui::SeparatorText(_L("app.settings.global_theme"));
    ImGui::Spacing();

    auto presetForId = [](int id) { return MakeAppearancePreset(id); };

    const char* presetNames[] = {
        _L("app.settings.dark"),
        _L("app.settings.light"),
        _L("app.settings.dark_glass"),
        _L("app.settings.light_glass"),
        _L("app.settings.dark_acrylic"),
        _L("app.settings.light_acrylic"),
        _L("app.settings.custom")
    };
    constexpr int presetIds[] = {
        kAppearancePresetDark, kAppearancePresetLight,
        kAppearancePresetGlassDark, kAppearancePresetGlassLight,
        kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight,
        kAppearancePresetCustom
    };
    int presetIndex = 0;
    for (int i = 0; i < static_cast<int>(sizeof(presetIds) / sizeof(presetIds[0])); ++i)
    {
        if (presetIds[i] == personalization_.backgroundPreset)
        {
            presetIndex = i;
            break;
        }
    }
    BeginSettingRow(_L("app.settings.theme"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##WidgetBackgroundPreset", &presetIndex,
        presetNames, static_cast<int>(sizeof(presetNames) / sizeof(presetNames[0]))))
    {
        const int previousPreset = personalization_.backgroundPreset;
        const float cornerRadius = personalization_.cornerRadius;
        const float barHeight = personalization_.barHeight;
        const float categorizedTabHeight =
            personalization_.categorizedTabHeight;
        const bool showCategoryTabCounts =
            personalization_.showCategoryTabCounts;
        const int contextMenuStyle =
            personalization_.contextMenuStyle;
        if (presetIds[presetIndex] == kAppearancePresetCustom)
        {
            const int inheritedFourTheme =
                FourThemeSelectionFromAppearancePreset(
                    NormalizeAppearancePresetId(previousPreset));
            generalSettings_.quickNavTheme = inheritedFourTheme;
            generalSettings_.collectionPopupTheme = inheritedFourTheme;
            generalSettingsDirty_ = true;
            personalization_.backgroundPreset = kAppearancePresetCustom;
        }
        else
        {
            personalization_ = presetForId(presetIds[presetIndex]);
        }
        personalization_.cornerRadius = cornerRadius;
        personalization_.barHeight = barHeight;
        personalization_.categorizedTabHeight =
            categorizedTabHeight;
        personalization_.showCategoryTabCounts =
            showCategoryTabCounts;
        personalization_.contextMenuStyle = contextMenuStyle;
        markChanged(true);
    }

    if (personalization_.backgroundPreset == kAppearancePresetCustom)
    {
    ImGui::Spacing();
    ImGui::Indent(8.0f * dpiScale_);

    const char* quickNavThemeNames[] = {
        _L("app.settings.dark"), _L("app.settings.light"), _L("app.settings.dark_acrylic"), _L("app.settings.light_acrylic")
    };
    BeginSettingRow(_L("app.settings.quick_nav_theme"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##QuickNavTheme", &generalSettings_.quickNavTheme,
        quickNavThemeNames, IM_ARRAYSIZE(quickNavThemeNames)))
        generalSettingsDirty_ = true;

    BeginSettingRow(_L("app.settings.collection_popup_theme"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##CollectionPopupTheme",
        &generalSettings_.collectionPopupTheme,
        quickNavThemeNames, IM_ARRAYSIZE(quickNavThemeNames)))
        generalSettingsDirty_ = true;

    float bgColor[3] = { personalization_.widgetBgR, personalization_.widgetBgG, personalization_.widgetBgB };
    if (DrawSettingColorEdit3(_L("app.settings.component_bg"),
            "##WidgetBgColor", bgColor))
    {
        personalization_.widgetBgR = bgColor[0]; personalization_.widgetBgG = bgColor[1];
        personalization_.widgetBgB = bgColor[2];
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    float borderColor[3] = { personalization_.widgetBorderR, personalization_.widgetBorderG, personalization_.widgetBorderB };
    if (DrawSettingColorEdit3(_L("app.settings.component_border"),
            "##WidgetBorderColor", borderColor))
    {
        personalization_.widgetBorderR = borderColor[0]; personalization_.widgetBorderG = borderColor[1];
        personalization_.widgetBorderB = borderColor[2];
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    ImGui::Spacing();

    BeginSettingRow(_L("app.settings.bg_opacity"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int widgetAlphaPercent = static_cast<int>(std::round(personalization_.widgetAlpha * 100.0f));
    if (ImGui::SliderInt("##WidgetAlpha", &widgetAlphaPercent, 0, 100, "%d%%"))
    {
        personalization_.widgetAlpha = widgetAlphaPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    BeginSettingRow(_L("app.settings.border_opacity"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int widgetBorderAlphaPercent = static_cast<int>(std::round(
        personalization_.widgetBorderAlpha * 100.0f));
    if (ImGui::SliderInt("##WidgetBorderAlpha", &widgetBorderAlphaPercent, 0, 100, "%d%%"))
    {
        personalization_.widgetBorderAlpha = widgetBorderAlphaPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    bool gradientToggle = personalization_.gradientEndA > 0.001f;
    if (DrawSettingCheckbox(_L("app.settings.enable_gradient"), "##GradientToggle", &gradientToggle))
    {
        personalization_.gradientEndA = gradientToggle
            ? presetForId(personalization_.backgroundPreset).gradientEndA
            : 0.0f;
        markChanged(true);
    }

    ImGui::BeginDisabled(!gradientToggle);
    BeginSettingRow(_L("app.settings.gradient_end_alpha"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int gradientEndAlphaPercent = static_cast<int>(std::round(
        personalization_.gradientEndA * 100.0f));
    if (ImGui::SliderInt("##GradientEndAlpha", &gradientEndAlphaPercent, 0, 100, "%d%%"))
    {
        personalization_.gradientEndA = gradientEndAlphaPercent / 100.0f;
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::EndDisabled();

    if (DrawSettingCheckbox(_L("app.settings.glass_enabled"), "##WidgetGlassEnabled",
        &personalization_.glassEnabled))
        markChanged(true);

    ImGui::BeginDisabled(!personalization_.glassEnabled);
    BeginSettingRow(_L("app.settings.blur_radius"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::SliderFloat("##GlassBlurRadius", &personalization_.glassBlurRadius, 4.0f, 48.0f, "%.0f px"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    if (DrawSettingCheckbox(_L("app.settings.acrylic_noise"), "##WidgetAcrylicEnabled",
        &personalization_.acrylicEnabled))
        markChanged(true);
    ImGui::EndDisabled();

    BeginSettingRow(_L("app.settings.text_color"), controlW);
    const char* contentThemeNames[] = { _L("app.settings.light"), _L("app.settings.dark") };
    int contentTheme = personalization_.contentTheme;
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##ContentTheme", &contentTheme,
        contentThemeNames, IM_ARRAYSIZE(contentThemeNames)))
    {
        personalization_.contentTheme = contentTheme;
        markChanged(true);
    }

    ImGui::Unindent(8.0f * dpiScale_);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.context_menu_appearance"));
    ImGui::Spacing();

    const char* contextMenuStyleNames[] = {
        _L("app.settings.context_menu_follow_system"),
        _L("app.settings.context_menu_system_light_blur"),
        _L("app.settings.context_menu_system_dark_blur"),
        _L("app.settings.context_menu_opaque_light"),
        _L("app.settings.context_menu_opaque_dark")
    };
    BeginSettingRow(_L("app.settings.context_menu_style"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##ContextMenuStyle",
        &personalization_.contextMenuStyle,
        contextMenuStyleNames, IM_ARRAYSIZE(contextMenuStyleNames)))
    {
        personalization_.contextMenuStyle = std::clamp(
            personalization_.contextMenuStyle, 0, 4);
        markChanged(true);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.widget_layout"));
    ImGui::Spacing();

    BeginSettingRow(_L("app.settings.corner_radius"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##WidgetCornerRadius", &personalization_.cornerRadius,
        4.0f, 28.0f, "%.0f cu"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##WidgetCornerRadiusDefault").c_str(), ImVec2(resetW, 0)))
    {
        personalization_.cornerRadius = 12.0f;
        markChanged(true);
    }

    BeginSettingRow(_L("app.settings.bar_height"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##BarHeight", &personalization_.barHeight,
        16.0f, 48.0f, "%.0f cu"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##BarHeightDefault").c_str(), ImVec2(resetW, 0)))
    {
        personalization_.barHeight = 24.0f;
        markChanged(true);
    }

    BeginSettingRow(
        _L("app.settings.tab_height"),
        sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat(
            "##CategorizedTabHeight",
            &personalization_.categorizedTabHeight,
            24.0f, 48.0f, "%.0f cu"))
        markChanged(false);
    if (ImGui::IsItemDeactivatedAfterEdit() &&
        personalizationDirty_)
        personalizationSaveRequested_ = true;
    ImGui::SameLine();
    if (BlueButton(
            (std::string(
                _L("app.settings.restore_default")) +
                "##CategorizedTabHeightDefault").c_str(),
            ImVec2(resetW, 0)))
    {
        personalization_.categorizedTabHeight = 34.0f;
        markChanged(true);
    }

    if (DrawSettingCheckbox(_L("app.settings.category_show_count"),
            "##CategoryShowCount",
            &personalization_.showCategoryTabCounts))
        markChanged(true);

    auto presetSelectionForId = [&](int presetId) {
        const int normalized = NormalizeAppearancePresetId(presetId);
        for (int i = 0; i < IM_ARRAYSIZE(presetIds); ++i)
            if (presetIds[i] == normalized) return i;
        return 0;
    };
    auto drawOverrideAdvanced = [&](PersonalizationSettings& style,
        const char* id) {
        bool changed = false;
        ImGui::PushID(id);
        ImGui::Spacing();
        ImGui::Indent(8.0f * dpiScale_);
        float background[3] = { style.widgetBgR, style.widgetBgG, style.widgetBgB };
            if (DrawSettingColorEdit3(_L("app.settings.bg_color"),
                    "##Background", background))
            {
                style.widgetBgR = background[0];
                style.widgetBgG = background[1];
                style.widgetBgB = background[2];
                changed = true;
            }

            float border[3] = { style.widgetBorderR, style.widgetBorderG, style.widgetBorderB };
            if (DrawSettingColorEdit3(_L("app.settings.border_color"),
                    "##Border", border))
            {
                style.widgetBorderR = border[0];
                style.widgetBorderG = border[1];
                style.widgetBorderB = border[2];
                changed = true;
            }

    BeginSettingRow(_L("app.settings.bg_opacity"), sliderW);
            ImGui::SetNextItemWidth(sliderW);
            int backgroundAlphaPercent = static_cast<int>(std::round(style.widgetAlpha * 100.0f));
            if (ImGui::SliderInt("##BackgroundAlpha", &backgroundAlphaPercent, 0, 100, "%d%%"))
            {
                style.widgetAlpha = backgroundAlphaPercent / 100.0f;
                changed = true;
            }

            BeginSettingRow(_L("app.settings.border_opacity"), sliderW);
            ImGui::SetNextItemWidth(sliderW);
            int borderAlphaPercent = static_cast<int>(std::round(
                style.widgetBorderAlpha * 100.0f));
            if (ImGui::SliderInt("##BorderAlpha", &borderAlphaPercent, 0, 100, "%d%%"))
            {
                style.widgetBorderAlpha = borderAlphaPercent / 100.0f;
                changed = true;
            }

            if (DrawSettingCheckbox(_L("app.settings.glass_enabled"), "##GlassEnabled",
                &style.glassEnabled))
                changed = true;

            ImGui::BeginDisabled(!style.glassEnabled);
    BeginSettingRow(_L("app.settings.blur_radius"), controlW);
            ImGui::SetNextItemWidth(controlW);
            if (ImGui::SliderFloat("##GlassBlurRadius", &style.glassBlurRadius,
                4.0f, 48.0f, "%.0f px"))
                changed = true;

            if (DrawSettingCheckbox(_L("app.settings.acrylic_noise"), "##AcrylicEnabled",
                &style.acrylicEnabled))
                changed = true;
            ImGui::EndDisabled();

        ImGui::Unindent(8.0f * dpiScale_);
        ImGui::PopID();
        return changed;
    };

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.desktop_icons"));
    ImGui::Spacing();
    DrawDisplayPage();

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.system_appearance"));
    ImGui::Spacing();

    int taskbarThemeMode;
    if (!dockSettings_.systemTaskbarBackdropEnabled)
        taskbarThemeMode = 0;
    else if (dockSettings_.systemTaskbarFollowPersonalization)
        taskbarThemeMode = 1;
    else
    {
        const int preset = NormalizeAppearancePresetId(
            dockSettings_.systemTaskbarAppearance.backgroundPreset);
        if (preset == kAppearancePresetCustom)
            taskbarThemeMode = 8;
        else switch (preset)
        {
        case kAppearancePresetDark:        taskbarThemeMode = 2; break;
        case kAppearancePresetLight:       taskbarThemeMode = 3; break;
        case kAppearancePresetGlassDark:   taskbarThemeMode = 4; break;
        case kAppearancePresetGlassLight:  taskbarThemeMode = 5; break;
        case kAppearancePresetAcrylicDark: taskbarThemeMode = 6; break;
        case kAppearancePresetAcrylicLight: taskbarThemeMode = 7; break;
        case kAppearancePresetTaskbarTransparent: taskbarThemeMode = 9; break;
        default:                            taskbarThemeMode = 2; break;
        }
    }

    BeginSettingRow(_L("app.settings.taskbar_theme"), controlW,
        _L("app.settings.taskbar_theme_hint"));
    const char* taskbarThemeNames[] = {
        _L("app.settings.taskbar_windows_native"), _L("app.settings.taskbar_follow_global"),
        _L("app.settings.dark"), _L("app.settings.light"), _L("app.settings.dark_glass"), _L("app.settings.light_glass"),
        _L("app.settings.dark_acrylic"), _L("app.settings.light_acrylic"), _L("app.settings.custom"),
        _L("app.settings.taskbar_transparent")
    };
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##TaskbarThemeMode", &taskbarThemeMode,
        taskbarThemeNames, IM_ARRAYSIZE(taskbarThemeNames)))
    {
        switch (taskbarThemeMode)
        {
        case 0:
            dockSettings_.systemTaskbarBackdropEnabled = false;
            break;
        case 1:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = true;
            dockSettings_.systemTaskbarContentTheme = -1;
            break;
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = false;
            dockSettings_.systemTaskbarContentTheme = -1;
            {
                constexpr int modeToPreset[] = {
                    -1, -1,
                    kAppearancePresetDark,
                    kAppearancePresetLight,
                    kAppearancePresetGlassDark,
                    kAppearancePresetGlassLight,
                    kAppearancePresetAcrylicDark,
                    kAppearancePresetAcrylicLight
                };
                dockSettings_.systemTaskbarAppearance =
                    MakeAppearancePreset(
                        modeToPreset[taskbarThemeMode]);
            }
            break;
        case 8:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = false;
            dockSettings_.systemTaskbarAppearance.backgroundPreset =
                kAppearancePresetCustom;
            if (dockSettings_.systemTaskbarContentTheme < 0)
                dockSettings_.systemTaskbarContentTheme =
                    dockSettings_.systemTaskbarAppearance.contentTheme;
            break;
        case 9:
            dockSettings_.systemTaskbarBackdropEnabled = true;
            dockSettings_.systemTaskbarFollowPersonalization = false;
            dockSettings_.systemTaskbarContentTheme = -1;
            dockSettings_.systemTaskbarAppearance =
                MakeTransparentTaskbarAppearance();
            break;
        }
        dockSettingsDirty_ = true;
        dockSettingsPreviewDirty_ = true;
        dockSettingsSaveRequested_ = true;
    }

    const auto dynamicRuleNeedsHook =
        [](const SystemTaskbarDynamicRule& rule) {
            return rule.enabled &&
                rule.themeMode != SystemTaskbarThemeMode::Native;
        };
    if (taskbarThemeMode != 0 ||
        dynamicRuleNeedsHook(dockSettings_.systemTaskbarVisibleWindow) ||
        dynamicRuleNeedsHook(dockSettings_.systemTaskbarMaximizedWindow) ||
        dynamicRuleNeedsHook(dockSettings_.systemTaskbarShellUi))
    {
        const char* taskbarRuntimeStatus = nullptr;
        switch (GetSystemTaskbarBackdropRuntimeState())
        {
        case SystemTaskbarBackdropRuntimeState::Loading:
            taskbarRuntimeStatus = _L("app.settings.taskbar_connecting");
            break;
        case SystemTaskbarBackdropRuntimeState::Unsupported:
            taskbarRuntimeStatus = _L("app.settings.taskbar_unsupported");
            break;
        case SystemTaskbarBackdropRuntimeState::Failed:
            taskbarRuntimeStatus = _L("app.settings.taskbar_connect_failed");
            break;
        case SystemTaskbarBackdropRuntimeState::Disabled:
        case SystemTaskbarBackdropRuntimeState::Active:
        default:
            break;
        }
        if (taskbarRuntimeStatus)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                "%s", taskbarRuntimeStatus);
        }

        // Windows-native mode disables SnowDesktop's taskbar visual target, so
        // a foreground override would be stored but could not affect the taskbar.
        if (taskbarThemeMode != 0)
        {
            BeginSettingRow(
                _L("app.settings.taskbar_foreground_color"), controlW);
            if (taskbarThemeMode == 8)
            {
                const char* ctNames[] = {
                    _L("app.settings.light"), _L("app.settings.dark")
                };
                int ct = dockSettings_.systemTaskbarContentTheme;
                if (ct < 0)
                    ct = dockSettings_.systemTaskbarAppearance.contentTheme;
                ImGui::SetNextItemWidth(controlW);
                if (ImGui::Combo("##ContentThemeTaskbar", &ct,
                    ctNames, IM_ARRAYSIZE(ctNames)))
                {
                    dockSettings_.systemTaskbarContentTheme = ct;
                    dockSettingsDirty_ = true;
                    dockSettingsPreviewDirty_ = true;
                    dockSettingsSaveRequested_ = true;
                }
            }
            else
            {
                const char* ctNames[] = {
                    _L("app.settings.taskbar_foreground_auto"),
                    _L("app.settings.light"), _L("app.settings.dark")
                };
                int ct = dockSettings_.systemTaskbarContentTheme + 1;
                ImGui::SetNextItemWidth(controlW);
                if (ImGui::Combo("##ContentThemeTaskbar", &ct,
                    ctNames, IM_ARRAYSIZE(ctNames)))
                {
                    dockSettings_.systemTaskbarContentTheme = ct - 1;
                    dockSettingsDirty_ = true;
                    dockSettingsPreviewDirty_ = true;
                    dockSettingsSaveRequested_ = true;
                }
            }
        }

        if (taskbarThemeMode == 8)
        {
            ImGui::Indent(8.0f * dpiScale_);
            if (drawOverrideAdvanced(
                dockSettings_.systemTaskbarAppearance,
                "OverrideAdvanced"))
            {
                dockSettingsDirty_ = true;
                dockSettingsPreviewDirty_ = true;
                dockSettingsSaveRequested_ = true;
            }
            ImGui::Unindent(8.0f * dpiScale_);
        }
    }

    auto drawDynamicTaskbarRule = [&](const char* label, const char* id,
        SystemTaskbarDynamicRule& rule) {
        ImGui::PushID(id);
        ImGui::Spacing();
        if (DrawSettingCheckbox(label, "##Enabled", &rule.enabled))
        {
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }

        if (!rule.enabled)
        {
            ImGui::PopID();
            return;
        }

        ImGui::Indent(8.0f * dpiScale_);
        int mode = std::clamp(static_cast<int>(rule.themeMode),
            static_cast<int>(SystemTaskbarThemeMode::Native),
            static_cast<int>(SystemTaskbarThemeMode::Transparent));
        BeginSettingRow(_L("app.settings.taskbar_dynamic_theme"), controlW);
        ImGui::SetNextItemWidth(controlW);
        if (ImGui::Combo("##Theme", &mode, taskbarThemeNames,
            IM_ARRAYSIZE(taskbarThemeNames)))
        {
            const SystemTaskbarThemeMode previousMode = rule.themeMode;
            rule.themeMode = static_cast<SystemTaskbarThemeMode>(mode);
            if (rule.themeMode == SystemTaskbarThemeMode::Custom)
            {
                if (previousMode >= SystemTaskbarThemeMode::Dark &&
                    previousMode <= SystemTaskbarThemeMode::AcrylicLight)
                {
                    constexpr int modeToPreset[] = {
                        -1, -1,
                        kAppearancePresetDark,
                        kAppearancePresetLight,
                        kAppearancePresetGlassDark,
                        kAppearancePresetGlassLight,
                        kAppearancePresetAcrylicDark,
                        kAppearancePresetAcrylicLight
                    };
                    rule.appearance = MakeAppearancePreset(
                        modeToPreset[static_cast<int>(previousMode)]);
                }
                rule.appearance.backgroundPreset = kAppearancePresetCustom;
            }
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }

        // Native rules explicitly hand the taskbar back to Windows; foreground
        // color is meaningful only while a SnowDesktop appearance is active.
        if (rule.themeMode != SystemTaskbarThemeMode::Native)
        {
            const char* dynamicContentThemeNames[] = {
                _L("app.settings.taskbar_foreground_auto"),
                _L("app.settings.light"),
                _L("app.settings.dark")
            };
            int contentTheme = std::clamp(rule.contentTheme, -1, 1) + 1;
            BeginSettingRow(
                _L("app.settings.taskbar_foreground_color"), controlW);
            ImGui::SetNextItemWidth(controlW);
            if (ImGui::Combo("##ContentTheme", &contentTheme,
                dynamicContentThemeNames,
                IM_ARRAYSIZE(dynamicContentThemeNames)))
            {
                rule.contentTheme = contentTheme - 1;
                dockSettingsDirty_ = true;
                dockSettingsPreviewDirty_ = true;
                dockSettingsSaveRequested_ = true;
            }
        }

        if (rule.themeMode == SystemTaskbarThemeMode::Custom &&
            drawOverrideAdvanced(rule.appearance, "CustomAppearance"))
        {
            rule.appearance.backgroundPreset = kAppearancePresetCustom;
            dockSettingsDirty_ = true;
            dockSettingsPreviewDirty_ = true;
            dockSettingsSaveRequested_ = true;
        }
        ImGui::Unindent(8.0f * dpiScale_);
        ImGui::PopID();
    };

    drawDynamicTaskbarRule(
        _L("app.settings.taskbar_dynamic_visible_window"),
        "VisibleWindow", dockSettings_.systemTaskbarVisibleWindow);
    drawDynamicTaskbarRule(
        _L("app.settings.taskbar_dynamic_maximized_window"),
        "MaximizedWindow", dockSettings_.systemTaskbarMaximizedWindow);
    drawDynamicTaskbarRule(
        _L("app.settings.taskbar_dynamic_shell_ui"),
        "ShellUi", dockSettings_.systemTaskbarShellUi);

    ImGui::Spacing();
    DrawSystemTaskbarPage();

    ImGui::EndChild();
}

/**
 * @brief 打开组件编辑器界面。
 *
 * 设置当前编辑的组件索引、ID、名称和脚本路径，
 * 然后显示设置窗口（切换到编辑器页面）。
 * @param widgetIndex 在组件列表中的索引
 * @param widgetId    组件的唯一标识符
 * @param widgetName  组件的显示名称
 * @param scriptPath  组件脚本文件路径
 */
void SettingsWindow::ShowWidgetEditor(size_t widgetIndex,
    const wchar_t* widgetId, const wchar_t* widgetName, const wchar_t* scriptPath)
{
    editingWidgetIndex_ = widgetIndex;
    widgetEditorBackPending_ = false;
    editingWidgetId_ = widgetId;
    editingWidgetName_ = widgetName;
    editingScriptPath_ = scriptPath;
    Show();
}

/**
 * @brief 绘制组件编辑器页面。
 *
 * 页面顶部提供"返回主界面"按钮，显示当前正在编辑的组件名称。
 * 委托 WidgetEngine 进行具体编辑界面的渲染（调用
 * EnsureWidgetLoaded 和 RenderWidgetEditor）。
 */
void SettingsWindow::DrawWidgetEditorPage()
{
    const float pad = 14.0f * dpiScale_;
    const float toolbarH = 48.0f * dpiScale_;
    const ImVec2 toolbarPos = ImGui::GetCursorScreenPos();
    const float toolbarW = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(toolbarPos,
        ImVec2(toolbarPos.x + toolbarW, toolbarPos.y + toolbarH),
        IM_COL32(248, 248, 250, 255), 8.0f * dpiScale_);
    drawList->AddLine(ImVec2(toolbarPos.x, toolbarPos.y + toolbarH),
        ImVec2(toolbarPos.x + toolbarW, toolbarPos.y + toolbarH),
        IM_COL32(210, 210, 218, 255), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(toolbarPos.x + pad, toolbarPos.y + 8.0f * dpiScale_));
    const ImVec2 backSize(116.0f * dpiScale_, 32.0f * dpiScale_);
    const ImVec2 backPos = ImGui::GetCursorScreenPos();
    bool backClicked = ImGui::InvisibleButton("##WidgetEditorBack", backSize);
    bool backHovered = ImGui::IsItemHovered();
    drawList->AddRectFilled(backPos,
        ImVec2(backPos.x + backSize.x, backPos.y + backSize.y),
        backHovered ? IM_COL32(226, 234, 246, 255) : IM_COL32(238, 242, 248, 255),
        16.0f * dpiScale_);
    drawList->AddRect(backPos,
        ImVec2(backPos.x + backSize.x, backPos.y + backSize.y),
        backHovered ? IM_COL32(110, 145, 190, 255) : IM_COL32(198, 208, 222, 255),
        16.0f * dpiScale_, 0, 1.0f);
    drawList->AddText(ImVec2(backPos.x + 14.0f * dpiScale_, backPos.y + 7.0f * dpiScale_),
        IM_COL32(42, 52, 68, 255), "<");
    drawList->AddText(ImVec2(backPos.x + 34.0f * dpiScale_, backPos.y + 7.0f * dpiScale_),
        IM_COL32(42, 52, 68, 255), _L("app.settings.widget_editor_back"));

    std::string title = _L("app.settings.widget_editor");
    std::string name = WideToUtf8(editingWidgetName_);
    if (!name.empty())
        title += " / " + name;
    drawList->AddText(ImVec2(backPos.x + backSize.x + 18.0f * dpiScale_,
            toolbarPos.y + 15.0f * dpiScale_),
        IM_COL32(36, 39, 46, 255), title.c_str());

    ImGui::SetCursorScreenPos(ImVec2(toolbarPos.x, toolbarPos.y + toolbarH + 10.0f * dpiScale_));

    if (backClicked)
        widgetEditorBackPending_ = true;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##WidgetEditorScroll", ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();

    if (widgetEngine_ && !widgetEditorBackPending_)
    {
        // Make input cursor clearly black
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

        widgetEngine_->EnsureWidgetLoaded(editingWidgetId_, editingScriptPath_);
        bool sharedGlassSettingsChanged = false;
        bool sharedGlassSettingsSaveRequested = false;
        widgetEngine_->RenderWidgetEditor(editingWidgetId_, editingWidgetName_,
            personalization_, sharedGlassSettingsChanged,
            sharedGlassSettingsSaveRequested);
        if (sharedGlassSettingsChanged)
        {
            personalizationDirty_ = true;
            personalizationPreviewDirty_ = true;
        }
        if (sharedGlassSettingsSaveRequested && personalizationDirty_)
            personalizationSaveRequested_ = true;

        ImGui::PopStyleColor(1);
    }

    ImGui::EndChild();
}

void SettingsWindow::DrawWidgetPackagesPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##WidgetPackagesPage", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    const auto packages = WidgetEngine::ListWidgetPackages();
    const auto invalidPackages = WidgetEngine::ListInvalidWidgetPackages();
    const bool steamBridgeAvailable =
        WidgetEngine::IsSteamWorkshopBridgeAvailable();
    const bool workshopPublisherAvailable =
        steamBridgeAvailable && IsSteamWorkshopPublisherAvailable();
    const std::string currentLocale =
        Locale::Instance().GetEffectiveLanguage();
    const auto widgetWorkshopSubscriptions =
        WidgetEngine::CachedSteamWorkshopPackageAssociations();
    const auto widgetWorkshopInstallFailures =
        WidgetEngine::CachedSteamWorkshopInstallFailures();
    auto localizedManifest = [&](snowdesktop::widget::PackageManifest manifest)
    {
        return snowdesktop::widget::LocalizePackageManifest(
            std::move(manifest), currentLocale);
    };
    auto permissionLabel = [](const std::string& permission)
    {
        if (const char* key = snowdesktop::widget::
                WidgetPermissionLabelLocalizationKey(permission))
            return _L(key);
        return permission.c_str();
    };
    auto permissionSummary = [&](
        const std::vector<std::string>& permissions)
    {
        const bool desktopAccess =
            std::find(permissions.begin(), permissions.end(),
                "desktop.read") != permissions.end() ||
            std::find(permissions.begin(), permissions.end(),
                "desktop.action") != permissions.end();
        const bool mediaAccess =
            std::find(permissions.begin(), permissions.end(),
                "media.read") != permissions.end() ||
            std::find(permissions.begin(), permissions.end(),
                "media.action") != permissions.end();
        std::string summary;
        for (const auto& permission : permissions)
        {
            if (permission == "ui.input" ||
                permission == "ui.contextMenu" ||
                permission == "desktop.read" ||
                permission == "desktop.action" ||
                permission == "media.read" ||
                permission == "media.action")
                continue;
            if (!summary.empty()) summary += ", ";
            summary += permissionLabel(permission);
        }
        if (desktopAccess)
        {
            if (!summary.empty()) summary += ", ";
            summary += _L("app.settings.widgets_permission_desktop");
        }
        if (mediaAccess)
        {
            if (!summary.empty()) summary += ", ";
            summary += _L("app.settings.widgets_permission_media");
        }
        return summary;
    };
    auto permissionStateLabel = [](snowdesktop::widget::
            PermissionDecisionState state)
    {
        using snowdesktop::widget::PermissionDecisionState;
        switch (state)
        {
        case PermissionDecisionState::LegacyImplicit:
            return _L("app.settings.widgets_permission_state_legacy");
        case PermissionDecisionState::Pending:
            return _L("app.settings.widgets_permission_state_pending");
        case PermissionDecisionState::Granted:
            return _L("app.settings.widgets_permission_state_granted");
        case PermissionDecisionState::Denied:
            return _L("app.settings.widgets_permission_state_denied");
        }
        return _L("app.settings.widgets_permission_state_pending");
    };
    auto effectivePermissionStateLabel = [&](const auto& package)
    {
        const auto grant = snowdesktop::widget::WidgetPermissionBroker::
            Evaluate(package.permissionState,
                package.manifest.permissions,
                package.manifest.optionalPermissions,
                package.manifest.networkDomains,
                package.grantedPermissions,
                package.grantedNetworkDomains);
        if (grant.runtimeBlock == snowdesktop::widget::
                PermissionRuntimeBlock::MissingRequired)
        {
            return _L(
                "app.settings.widgets_permission_state_missing_required");
        }
        return permissionStateLabel(package.permissionState);
    };
    auto canManagePermissions = [](const auto& package)
    {
        return !snowdesktop::widget::DeclaredPermissions(
            package.manifest).empty();
    };
    auto canRevokePermissions = [](const auto& package)
    {
        using snowdesktop::widget::PermissionDecisionState;
        const auto state = package.permissionState;
        return (state == PermissionDecisionState::Granted ||
                state == PermissionDecisionState::LegacyImplicit) &&
            !snowdesktop::widget::PermissionsRequiringConsent(
                snowdesktop::widget::DeclaredPermissions(
                    package.manifest)).empty();
    };
    auto revokePermissions = [&](const auto& package)
    {
        std::string error;
        const std::wstring packageId = Utf8ToWide(package.manifest.id);
        const bool revoked = widgetEngine_
            ? widgetEngine_->ApplyWidgetPermissionDecision(packageId,
                snowdesktop::widget::PermissionDecisionState::Denied,
                {}, {}, error)
            : WidgetEngine::SetWidgetPermissionDecision(packageId,
                snowdesktop::widget::PermissionDecisionState::Denied,
                {}, {}, error);
        if (revoked)
        {
            widgetPackageStatus_ = _L(
                "app.settings.widgets_permissions_revoked_ok");
            if (reloadCallback_) reloadCallback_();
        }
        else
            widgetPackageStatus_ = error;
        return revoked;
    };
    bool openPermissionEditorRequested = false;
    auto openPermissionEditor = [&](const auto& package)
    {
        using snowdesktop::widget::PermissionDecisionState;
        widgetPermissionEditorPackageId_ = package.manifest.id;
        widgetPermissionEditorPackageVersion_ = package.manifest.version;
        widgetPermissionEditorSource_ = package.source;
        widgetPermissionEditorScopeFingerprint_ =
            snowdesktop::widget::WidgetPermissionBroker::ScopeFingerprint(
                package.manifest.permissions,
                package.manifest.optionalPermissions,
                package.manifest.networkDomains);
        widgetPermissionEditorGrants_ = package.grantedPermissions;
        if (package.permissionState == PermissionDecisionState::Pending ||
            package.permissionState == PermissionDecisionState::Denied)
        {
            widgetPermissionEditorGrants_ = package.manifest.permissions;
            for (const auto& permission :
                package.manifest.optionalPermissions)
            {
                if (!snowdesktop::widget::PermissionRequiresConsent(
                        permission))
                {
                    widgetPermissionEditorGrants_.push_back(permission);
                }
            }
        }
        openPermissionEditorRequested = true;
    };
    auto needsInstallConfirmation = [](const std::wstring& message)
    {
        return message.find(L"requires explicit confirmation") !=
                std::wstring::npos ||
            message.find(L"requests a new permission") !=
                std::wstring::npos ||
            message.find(L"expands network access") != std::wstring::npos;
    };
    auto reloadPackageInstances = [&](const std::string& packageId)
    {
        if (!widgetEngine_ || packageId.empty()) return;
        std::vector<std::wstring> instances;
        for (const auto& widget : widgetEngine_->GetWidgets())
            if (widget.packageId == packageId)
                instances.push_back(widget.widgetId);
        for (const auto& instance : instances)
            widgetEngine_->ReloadWidget(instance);
    };
    auto finishInstallAttempt = [&](bool ok, const std::wstring& installError,
        const std::filesystem::path& localPath,
        const std::string& developmentId)
    {
        if (ok)
        {
            if (!developmentId.empty())
            {
                std::string overrideError;
                if (!WidgetEngine::SetWidgetDevelopmentOverride(
                        developmentId, false, overrideError))
                {
                    widgetPackageStatus_ = overrideError;
                }
                else
                {
                    reloadPackageInstances(developmentId);
                    widgetPackageStatus_ = _L(
                        "app.settings.widgets_install_managed_ok");
                }
            }
            else
                widgetPackageStatus_ = _L("app.settings.widgets_install_ok");
            pendingWidgetInstallKind_ = PendingWidgetInstallKind::None;
            pendingWidgetInstallPath_.clear();
            pendingWidgetInstallReason_.clear();
            pendingWidgetInstallDevelopmentId_.clear();
            if (reloadCallback_) reloadCallback_();
            return;
        }
        widgetPackageStatus_ = WideToUtf8(installError);
        if (!needsInstallConfirmation(installError))
        {
            pendingWidgetInstallDevelopmentId_.clear();
            return;
        }
        pendingWidgetInstallKind_ = PendingWidgetInstallKind::Local;
        pendingWidgetInstallPath_ = localPath.wstring();
        pendingWidgetInstallReason_ = installError;
        pendingWidgetInstallDevelopmentId_ = developmentId;
    };
    auto installLocalPackage = [&](const std::filesystem::path& path,
        bool confirmed, const std::string& developmentId)
    {
        std::wstring installError;
        const bool ok = widgetEngine_ &&
            widgetEngine_->InstallAndVerifyWidgetPackage(path.wstring(),
                installError, confirmed, confirmed);
        finishInstallAttempt(ok, installError, path, developmentId);
    };

    ImGui::SeparatorText(_L("app.settings.widgets_my_components"));
    static constexpr COMDLG_FILTERSPEC packageFilters[] = {
        { L"SnowDesktop Component", L"*.snowwidget;widget.json" },
        { L"All Files", L"*.*" },
    };
    if (SecondaryButton(_L("app.settings.widgets_install_package")))
    {
        if (const auto selected = PickSettingsFile(hwnd_,
            _LW("app.settings.widgets_install_package"),
            packageFilters,
            static_cast<UINT>(std::size(packageFilters))))
        {
            installLocalPackage(*selected, false, {});
        }
    }
    if (steamBridgeAvailable)
    {
        ImGui::SameLine();
        if (BlueButton(_L("app.settings.widgets_open_steam_workshop")))
        {
            OpenSteamUrlWithWebFallback(
                snowdesktop::SnowDesktopSteamWorkshopClientUrl(),
                snowdesktop::SnowDesktopSteamWorkshopUrl());
        }
    }
    ImGui::SameLine();
    const char* selfDevelopLabel =
        _L("app.settings.widgets_self_develop");
    const bool developerToolsEnabled =
        generalSettings_.widgetDeveloperToolsEnabled;
    const bool toggleDeveloperTools = developerToolsEnabled
        ? BlueButton(selfDevelopLabel)
        : SecondaryButton(selfDevelopLabel);
    if (toggleDeveloperTools)
    {
        generalSettings_.widgetDeveloperToolsEnabled =
            !developerToolsEnabled;
        generalSettingsDirty_ = true;
        if (generalSettings_.widgetDeveloperToolsEnabled)
            activePage_ = 9;
    }
    if (steamBridgeAvailable)
    {
        ImGui::TextDisabled("%s",
            _L("app.settings.widgets_workshop_auto_sync_hint"));
    }
    ImGui::Spacing();

    struct PackageGroup
    {
        std::string id;
        const snowdesktop::widget::InstalledPackage* builtin = nullptr;
        const snowdesktop::widget::InstalledPackage* managed = nullptr;
        const snowdesktop::widget::InstalledPackage* development = nullptr;
        std::vector<const snowdesktop::widget::InvalidPackage*>
            invalidBuiltin;
        std::vector<const snowdesktop::widget::InvalidPackage*>
            invalidManaged;
        std::vector<const snowdesktop::widget::InvalidPackage*>
            invalidDevelopment;
        std::vector<const snowdesktop::widget::SteamWorkshopInstallFailure*>
            workshopInstallFailures;
    };
    std::vector<PackageGroup> packageGroups;
    std::unordered_map<std::string, std::size_t> packageGroupIndexes;
    for (const auto& package : packages)
    {
        if (!package.active && !package.selected && !package.development)
            continue;
        auto [it, inserted] = packageGroupIndexes.emplace(
            package.manifest.id, packageGroups.size());
        if (inserted)
            packageGroups.push_back(PackageGroup{ package.manifest.id });
        PackageGroup& group = packageGroups[it->second];
        if (package.development)
            group.development = &package;
        else if (package.builtin)
            group.builtin = &package;
        else if (package.selected)
            group.managed = &package;
    }
    for (const auto& package : invalidPackages)
    {
        const std::string groupId = !package.packageId.empty()
            ? package.packageId
            : "invalid:" + WideToUtf8(package.root.wstring());
        auto [it, inserted] = packageGroupIndexes.emplace(
            groupId, packageGroups.size());
        if (inserted)
            packageGroups.push_back(PackageGroup{ groupId });
        PackageGroup& group = packageGroups[it->second];
        if (package.development)
            group.invalidDevelopment.push_back(&package);
        else if (package.builtin)
            group.invalidBuiltin.push_back(&package);
        else
            group.invalidManaged.push_back(&package);
    }
    for (const auto& failure : widgetWorkshopInstallFailures)
    {
        const std::string groupId = !failure.packageId.empty()
            ? failure.packageId : failure.manifest.id;
        if (groupId.empty()) continue;
        auto [it, inserted] = packageGroupIndexes.emplace(
            groupId, packageGroups.size());
        if (inserted)
            packageGroups.push_back(PackageGroup{ groupId });
        packageGroups[it->second].workshopInstallFailures.push_back(
            &failure);
    }
    auto displayPackage = [](const PackageGroup& group)
        -> const snowdesktop::widget::InstalledPackage*
    {
        if (group.development && group.development->active)
            return group.development;
        if (group.managed) return group.managed;
        if (group.development) return group.development;
        return group.builtin;
    };
    auto workshopExternalItemIdFor = [&](const PackageGroup& group)
    {
        if (group.managed &&
            group.managed->source.providerId == "steam-workshop")
        {
            return group.managed->source.externalItemId;
        }
        for (const auto* package : group.invalidManaged)
        {
            if (package->source.providerId == "steam-workshop")
                return package->source.externalItemId;
        }
        if (!group.workshopInstallFailures.empty())
            return group.workshopInstallFailures.front()->externalItemId;
        if (const auto subscribed =
                widgetWorkshopSubscriptions.find(group.id);
            subscribed != widgetWorkshopSubscriptions.end())
        {
            return subscribed->second;
        }
        return std::string{};
    };
    auto foldSearchText = [](std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char ch)
            {
                return ch < 0x80
                    ? static_cast<char>(std::tolower(ch))
                    : static_cast<char>(ch);
            });
        return value;
    };
    const std::string searchText = foldSearchText(widgetPackageSearch_);
    auto matchesSearch = [&](const PackageGroup& group)
    {
        if (searchText.empty()) return true;
        const auto* package = displayPackage(group);
        std::string searchable = group.id;
        if (package)
        {
            const auto manifest = localizedManifest(package->manifest);
            searchable += "\n" + manifest.name + "\n" +
                manifest.description + "\n" + manifest.author + "\n" +
                manifest.id + "\n" + manifest.version;
        }
        const auto appendInvalid = [&](const auto& candidates)
        {
            for (const auto* invalid : candidates)
            {
                const auto manifest = localizedManifest(invalid->manifest);
                searchable += "\n" + manifest.name + "\n" +
                    manifest.description + "\n" + manifest.author + "\n" +
                    manifest.id + "\n" + manifest.version + "\n" +
                    WideToUtf8(invalid->root.filename().wstring());
                for (const auto& issue : invalid->report.issues)
                    searchable += "\n" + issue.code + "\n" + issue.message;
            }
        };
        appendInvalid(group.invalidBuiltin);
        appendInvalid(group.invalidManaged);
        appendInvalid(group.invalidDevelopment);
        for (const auto* failure : group.workshopInstallFailures)
        {
            const auto manifest = localizedManifest(failure->manifest);
            searchable += "\n" + manifest.name + "\n" +
                manifest.description + "\n" + manifest.author + "\n" +
                manifest.id + "\n" + manifest.version + "\n" +
                failure->error;
        }
        return foldSearchText(std::move(searchable)).find(searchText) !=
            std::string::npos;
    };

    int listedPackageCount = 0;
    int builtinPackageCount = 0;
    int installedPackageCount = 0;
    int developmentPackageCount = 0;
    for (const auto& group : packageGroups)
    {
        const bool userPackage = group.managed || group.development ||
            !group.invalidManaged.empty() ||
            !group.invalidDevelopment.empty() ||
            !group.workshopInstallFailures.empty();
        if (userPackage) ++listedPackageCount;
        if (group.managed || !group.invalidManaged.empty() ||
            !group.workshopInstallFailures.empty())
            ++installedPackageCount;
        if (group.development || !group.invalidDevelopment.empty())
            ++developmentPackageCount;
        if ((group.builtin || !group.invalidBuiltin.empty()) && !userPackage)
            ++builtinPackageCount;
    }
    if ((widgetPackageFilter_ == 1 && installedPackageCount == 0) ||
        (widgetPackageFilter_ == 2 && developmentPackageCount == 0) ||
        widgetPackageFilter_ > 2)
        widgetPackageFilter_ = 0;

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##WidgetPackageSearch",
        _L("app.settings.widgets_search_hint"), widgetPackageSearch_,
        sizeof(widgetPackageSearch_));

    auto drawFilterTag = [&](int filter, const char* label, int count)
    {
        ImGui::PushID(filter);
        std::string text = label;
        text += " ";
        text += std::to_string(count);
        const bool selected = widgetPackageFilter_ == filter;
        if ((selected ? BlueButton(text.c_str())
                      : SecondaryButton(text.c_str())))
        {
            widgetPackageFilter_ = filter;
        }
        ImGui::PopID();
    };
    drawFilterTag(0, _L("app.settings.widgets_filter_all"),
        listedPackageCount);
    if (installedPackageCount > 0)
    {
        ImGui::SameLine();
        drawFilterTag(1,
            _L("app.settings.widgets_filter_installed"),
            installedPackageCount);
    }
    if (developmentPackageCount > 0)
    {
        ImGui::SameLine();
        drawFilterTag(2,
            _L("app.settings.widgets_filter_development"),
            developmentPackageCount);
    }

    ImGui::Spacing();
    ImGui::BeginChild("##WidgetPackagesScroll", ImVec2(0, 0),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);

    std::vector<const PackageGroup*> visiblePackageGroups;
    for (const auto& group : packageGroups)
    {
        const bool hasInstalled = group.managed ||
            !group.invalidManaged.empty() ||
            !group.workshopInstallFailures.empty();
        const bool hasDevelopment = group.development ||
            !group.invalidDevelopment.empty();
        if (!hasInstalled && !hasDevelopment) continue;
        if (widgetPackageFilter_ == 1 && !hasInstalled) continue;
        if (widgetPackageFilter_ == 2 && !hasDevelopment) continue;
        if (!matchesSearch(group)) continue;
        visiblePackageGroups.push_back(&group);
    }
    if (visiblePackageGroups.empty())
        ImGui::TextDisabled("%s",
            _L("app.settings.widgets_filter_empty"));
    if (!visiblePackageGroups.empty())
    {
        const float cardWidth = ImGui::GetContentRegionAvail().x;
        const float cardGap = 6.0f * dpiScale_;
        for (std::size_t groupIndex = 0;
            groupIndex < visiblePackageGroups.size(); ++groupIndex)
        {
            const PackageGroup& group = *visiblePackageGroups[groupIndex];
            const float cardHeight = (220.0f + 38.0f *
                static_cast<float>(group.invalidManaged.size() +
                    group.invalidDevelopment.size() +
                    group.workshopInstallFailures.size())) * dpiScale_;
            const auto* package = displayPackage(group);
            const snowdesktop::widget::InvalidPackage* displayInvalid =
                nullptr;
            const auto selectInvalid = [&](const auto& candidates)
            {
                if (displayInvalid) return;
                const auto selected = std::find_if(candidates.begin(),
                    candidates.end(), [](const auto* candidate)
                    {
                        return candidate->selected;
                    });
                if (selected != candidates.end()) displayInvalid = *selected;
                else if (!candidates.empty()) displayInvalid = candidates.front();
            };
            selectInvalid(group.invalidDevelopment);
            selectInvalid(group.invalidManaged);
            if (!package && !displayInvalid &&
                group.workshopInstallFailures.empty())
                continue;
            const auto manifest = localizedManifest(package
                ? package->manifest
                : displayInvalid
                    ? displayInvalid->manifest
                    : group.workshopInstallFailures.front()->manifest);
            std::string displayName = !manifest.name.empty()
                ? manifest.name : group.id;
            std::vector<const snowdesktop::widget::InstalledPackage*>
                olderVersions;
            if (group.managed)
            {
                for (const auto& candidate : packages)
                {
                    if (candidate.manifest.id == group.id &&
                        !candidate.selected && !candidate.builtin &&
                        !candidate.development)
                    {
                        olderVersions.push_back(&candidate);
                    }
                }
                std::sort(olderVersions.begin(), olderVersions.end(),
                    [](const auto* left, const auto* right)
                    {
                        return snowdesktop::widget::WidgetPackageValidator::
                            IsNewerSemVer(left->manifest.version,
                                right->manifest.version);
                    });
            }
            const bool hasManagedVersion = group.development &&
                std::any_of(packages.begin(), packages.end(),
                    [&](const auto& candidate)
                    {
                        return candidate.manifest.id == group.id &&
                            !candidate.builtin && !candidate.development &&
                            candidate.manifest.version ==
                                group.development->manifest.version;
                    });
            const std::string workshopExternalItemId =
                workshopExternalItemIdFor(group);
            const std::string workshopItemId =
                snowdesktop::widget::SteamPublishedFileId(
                    workshopExternalItemId);
            const auto* workshopInstallFailure =
                group.workshopInstallFailures.empty()
                ? nullptr : group.workshopInstallFailures.front();
            if (manifest.name.empty() && workshopInstallFailure &&
                !workshopItemId.empty())
            {
                char detectedItemName[256]{};
                std::snprintf(detectedItemName, sizeof(detectedItemName),
                    _L("app.settings.widgets_workshop_detected_item"),
                    workshopItemId.c_str());
                displayName = detectedItemName;
            }
            const auto openWorkshopItem = [&]()
            {
                if (workshopItemId.empty()) return;
                OpenSteamUrlWithWebFallback(
                    snowdesktop::SnowDesktopSteamCommunityItemClientUrl(
                        workshopItemId),
                    snowdesktop::SnowDesktopSteamCommunityItemUrl(
                        workshopItemId));
            };
            const auto retryWorkshopInstall = [&]()
            {
                if (!widgetEngine_ || !workshopInstallFailure) return;
                std::wstring error;
                if (widgetEngine_->InstallAndVerifyWidgetPackageFromSource(
                        "steam-workshop",
                        workshopInstallFailure->externalItemId,
                        workshopInstallFailure->manifest.version, error,
                        false, false))
                {
                    reloadPackageInstances(group.id);
                    widgetPackageStatus_.clear();
                    if (reloadCallback_) reloadCallback_();
                }
                else
                {
                    widgetPackageStatus_ = WideToUtf8(error);
                }
            };
            ImGui::PushID(group.id.c_str());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                ImVec2(10.0f * dpiScale_, 9.0f * dpiScale_));
            ImGui::BeginChild("##InstalledWidgetListItem",
                ImVec2(cardWidth, cardHeight),
                ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::PopStyleVar();

            ImGui::TextUnformatted(displayName.c_str());
            const float moreButtonWidth = 30.0f * dpiScale_;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
                ImGui::GetWindowContentRegionMax().x - moreButtonWidth));
            if (SecondaryButton("...", ImVec2(moreButtonWidth, 0)))
                ImGui::OpenPopup("##WidgetPackageMore");

            if (!manifest.version.empty())
            {
                char versionText[128]{};
                std::snprintf(versionText, sizeof(versionText),
                    _L("app.settings.widgets_version"),
                    manifest.version.c_str());
                ImGui::TextDisabled("%s", versionText);
            }
            else
            {
                ImGui::Spacing();
            }

            ImGui::BeginChild("##Description", ImVec2(0,
                    ImGui::GetTextLineHeightWithSpacing() * 2.0f),
                ImGuiChildFlags_None,
                ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse);
            if (!manifest.description.empty())
                ImGui::TextWrapped("%s", manifest.description.c_str());
            ImGui::EndChild();

            if (group.managed)
            {
                const char* installedSource = !workshopItemId.empty()
                    ? _L("app.settings.widgets_source_steam")
                    : _L("app.settings.widgets_source_local");
                ImGui::TextDisabled("%s · %s · %s · %s", installedSource,
                    group.managed->manifest.version.c_str(),
                    _L("app.settings.valid"), group.managed->enabled
                        ? _L("app.settings.widgets_active")
                        : _L("app.settings.widgets_disabled"));
            }
            if (group.development)
            {
                const char* developmentState = group.development->active
                    ? _L("app.settings.widgets_development_active")
                    : group.managed
                        ? _L("app.settings.widgets_development_using_installed")
                        : _L("app.settings.widgets_development_inactive");
                ImGui::TextDisabled("%s · %s · %s · %s",
                    _L("app.settings.widgets_filter_development"),
                    group.development->manifest.version.c_str(),
                    _L("app.settings.valid"), developmentState);
            }
            if (package == group.builtin && !group.managed &&
                !group.development)
            {
                ImGui::TextDisabled("%s · %s",
                    _L("app.settings.widgets_filter_builtin"),
                    _L("app.settings.valid"));
            }
            const auto drawInvalidSources = [&](const auto& candidates,
                    const char* fallbackSource)
            {
                for (const auto* invalid : candidates)
                {
                    const char* invalidSource = fallbackSource;
                    if (invalid->source.providerId == "steam-workshop")
                        invalidSource =
                            _L("app.settings.widgets_source_steam");
                    ImGui::TextColored(
                        ImVec4(0.82f, 0.30f, 0.27f, 1.0f),
                        "%s · %s · %s", invalidSource,
                        invalid->manifest.version.c_str(),
                        _L("app.settings.invalid"));
                    const auto issue = std::find_if(
                        invalid->report.issues.begin(),
                        invalid->report.issues.end(),
                        [](const auto& candidate)
                        {
                            return candidate.severity ==
                                snowdesktop::widget::ValidationSeverity::Error;
                        });
                    if (issue != invalid->report.issues.end())
                    {
                        ImGui::TextDisabled("[%s] %s", issue->code.c_str(),
                            issue->message.c_str());
                    }
                }
            };
            drawInvalidSources(group.invalidManaged,
                _L("app.settings.widgets_source_local"));
            drawInvalidSources(group.invalidDevelopment,
                _L("app.settings.widgets_filter_development"));
            for (const auto* failure : group.workshopInstallFailures)
            {
                if (failure->manifest.version.empty())
                {
                    ImGui::TextColored(
                        ImVec4(0.82f, 0.30f, 0.27f, 1.0f),
                        "%s · %s",
                        _L("app.settings.widgets_source_steam"),
                        _L("app.settings.widgets_workshop_install_failed"));
                }
                else
                {
                    ImGui::TextColored(
                        ImVec4(0.82f, 0.30f, 0.27f, 1.0f),
                        "%s · %s · %s",
                        _L("app.settings.widgets_source_steam"),
                        failure->manifest.version.c_str(),
                        _L("app.settings.widgets_workshop_install_failed"));
                }
                if (!failure->error.empty())
                    ImGui::TextDisabled("%s", failure->error.c_str());
            }
            if (!manifest.author.empty())
            {
                ImGui::TextDisabled("%s: %s",
                    _L("app.settings.author"), manifest.author.c_str());
            }
            if (!package)
            {
                if (ImGui::BeginPopup("##WidgetPackageMore"))
                {
                    ImGui::TextUnformatted(displayName.c_str());
                    ImGui::Separator();
                    ImGui::TextDisabled("%s: %s",
                        _L("app.settings.widgets_package_id"),
                        group.id.c_str());
                    if (!workshopItemId.empty() && ImGui::MenuItem(_L(
                            "app.settings.widgets_open_workshop_item")))
                    {
                        openWorkshopItem();
                    }
                    if (!group.invalidManaged.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            ImVec4(0.86f, 0.22f, 0.20f, 1.0f));
                        if (ImGui::MenuItem(
                                _L("app.settings.widgets_uninstall")))
                        {
                            pendingWidgetPackageUninstall_ = group.id;
                            pendingWidgetPackageUninstallWorkshopItem_ =
                                workshopItemId.empty()
                                ? std::string{} : workshopExternalItemId;
                        }
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndPopup();
                }
                const float frameHeight = ImGui::GetFrameHeight();
                const float footerStart = cardHeight - frameHeight -
                    10.0f * dpiScale_;
                ImGui::SetCursorPosY(std::max(
                    ImGui::GetCursorPosY(), footerStart));
                const float actionGap = ImGui::GetStyle().ItemSpacing.x;
                const float retryWidth = ImGui::CalcTextSize(_L(
                    "app.settings.widgets_retry_install")).x +
                    24.0f * dpiScale_;
                const float workshopWidth = ImGui::CalcTextSize(_L(
                    "app.settings.widgets_open_workshop_item")).x +
                    24.0f * dpiScale_;
                float visibleActionsWidth = 0.0f;
                if (workshopInstallFailure)
                    visibleActionsWidth += retryWidth;
                if (!workshopItemId.empty())
                {
                    if (visibleActionsWidth > 0.0f)
                        visibleActionsWidth += actionGap;
                    visibleActionsWidth += workshopWidth;
                }
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                    ImGui::GetWindowContentRegionMax().x -
                        visibleActionsWidth));
                if (workshopInstallFailure && SecondaryButton(_L(
                        "app.settings.widgets_retry_install"),
                        ImVec2(retryWidth, 0)))
                {
                    retryWorkshopInstall();
                }
                if (workshopInstallFailure && !workshopItemId.empty())
                    ImGui::SameLine();
                if (!workshopItemId.empty() && SecondaryButton(_L(
                        "app.settings.widgets_open_workshop_item"),
                        ImVec2(workshopWidth, 0)))
                {
                    openWorkshopItem();
                }
                ImGui::EndChild();
                ImGui::Separator();
                ImGui::Dummy(ImVec2(0, cardGap));
                ImGui::PopID();
                continue;
            }
            const auto& permissions = package->grantedPermissions;
            const std::string permissionsText =
                permissionSummary(permissions);
            ImGui::TextDisabled("%s: %s",
                _L("app.settings.widgets_permission_state"),
                effectivePermissionStateLabel(*package));
            if (permissionsText.empty())
                ImGui::TextDisabled("%s",
                    _L("app.settings.widgets_no_permissions"));
            else
            {
                ImGui::TextDisabled("%s: %s",
                    _L("app.settings.widgets_permissions"),
                    permissionsText.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(
                        ImGui::GetCursorPosX() + 360.0f * dpiScale_);
                    ImGui::TextWrapped("%s: %s",
                        _L("app.settings.widgets_permissions"),
                        permissionsText.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }

            const auto toggleManagedPackage = [&]()
            {
                if (!group.managed) return;
                std::string error;
                const bool enabled = !group.managed->enabled;
                if (WidgetEngine::SetWidgetPackageEnabled(
                        group.id, enabled, error))
                {
                    widgetPackageStatus_ = _L(enabled
                        ? "app.settings.widgets_enabled_ok"
                        : "app.settings.widgets_disabled_ok");
                    if (!enabled && widgetEngine_)
                    {
                        std::vector<std::wstring> instances;
                        for (const auto& widget : widgetEngine_->GetWidgets())
                        {
                            if (widget.packageId == group.id)
                                instances.push_back(widget.widgetId);
                        }
                        for (const auto& instance : instances)
                            widgetEngine_->UnloadWidget(instance);
                    }
                    if (reloadCallback_) reloadCallback_();
                }
                else
                {
                    widgetPackageStatus_ = error;
                }
            };
            const auto toggleDevelopmentPackage = [&]()
            {
                if (!group.development) return;
                std::string error;
                const bool active = !group.development->active;
                if (WidgetEngine::SetWidgetDevelopmentOverride(
                        group.id, active, error))
                {
                    reloadPackageInstances(group.id);
                    widgetPackageStatus_ = _L(active
                        ? "app.settings.widgets_development_activated_ok"
                        : "app.settings.widgets_development_deactivated_ok");
                    if (reloadCallback_) reloadCallback_();
                }
                else
                {
                    widgetPackageStatus_ = error;
                }
            };

            if (ImGui::BeginPopup("##WidgetPackageMore"))
            {
                ImGui::TextUnformatted(manifest.name.c_str());
                ImGui::Separator();
                ImGui::TextDisabled("%s: %s",
                    _L("app.settings.widgets_package_id"),
                    group.id.c_str());
                ImGui::TextDisabled("%s: %s",
                    _L("app.settings.widgets_provider_id"),
                    package->source.providerId.empty()
                        ? _L("app.settings.widgets_source_local")
                        : package->source.providerId.c_str());
                ImGui::TextDisabled("%s: %s",
                    _L("app.settings.widgets_permission_state"),
                    effectivePermissionStateLabel(*package));
                if (!permissionsText.empty())
                {
                    ImGui::PushTextWrapPos(
                        ImGui::GetCursorPosX() + 360.0f * dpiScale_);
                    ImGui::TextWrapped("%s: %s",
                        _L("app.settings.widgets_permissions"),
                        permissionsText.c_str());
                    ImGui::PopTextWrapPos();
                }
                if (canManagePermissions(*package))
                {
                    ImGui::Separator();
                    if (ImGui::MenuItem(_L(
                            "app.settings.widgets_manage_permissions")))
                    {
                        openPermissionEditor(*package);
                        ImGui::CloseCurrentPopup();
                    }
                }
                if (group.managed)
                {
                    ImGui::Separator();
                    if (ImGui::MenuItem(group.managed->enabled
                            ? _L("app.settings.widgets_disable")
                            : _L("app.settings.widgets_enable")))
                    {
                        toggleManagedPackage();
                    }
                    if (!group.development &&
                        group.invalidDevelopment.empty() &&
                        !workshopItemId.empty() &&
                        ImGui::MenuItem(_L(
                            "app.settings.widgets_create_development")))
                    {
                        std::filesystem::path projectRoot;
                        std::string error;
                        if (WidgetEngine::CreateWidgetDevelopmentProject(
                                group.id, projectRoot, error))
                        {
                            generalSettings_.widgetDeveloperToolsEnabled = true;
                            generalSettingsDirty_ = true;
                            widgetPackageStatus_ = _L(
                                "app.settings.widgets_create_development_ok");
                            activePage_ = 9;
                            ShellExecuteW(nullptr, L"open",
                                projectRoot.c_str(), nullptr, nullptr,
                                SW_SHOWNORMAL);
                            if (reloadCallback_) reloadCallback_();
                            ImGui::CloseCurrentPopup();
                        }
                        else widgetPackageStatus_ = error;
                    }
                    if (!olderVersions.empty() && ImGui::BeginMenu(
                            _L("app.settings.widgets_old_versions")))
                    {
                        for (const auto* olderVersion : olderVersions)
                        {
                            if (ImGui::MenuItem(
                                    olderVersion->manifest.version.c_str()))
                            {
                                std::string error;
                                if (WidgetEngine::RollbackWidgetPackage(
                                        group.id,
                                        olderVersion->manifest.version,
                                        error))
                                {
                                    reloadPackageInstances(group.id);
                                    widgetPackageStatus_ = _L(
                                        "app.settings.widgets_rollback_ok");
                                    if (reloadCallback_) reloadCallback_();
                                }
                                else
                                    widgetPackageStatus_ = error;
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImVec4(0.86f, 0.22f, 0.20f, 1.0f));
                    if (ImGui::MenuItem(
                            _L("app.settings.widgets_uninstall")))
                    {
                        pendingWidgetPackageUninstall_ = group.id;
                        pendingWidgetPackageUninstallWorkshopItem_ =
                            workshopItemId.empty()
                                ? std::string{}
                                : workshopExternalItemId;
                    }
                    ImGui::PopStyleColor();
                }
                if (group.development)
                {
                    ImGui::Separator();
                    if (ImGui::MenuItem(group.development->active
                            ? _L("app.settings.widgets_development_deactivate")
                            : _L("app.settings.widgets_development_activate")))
                    {
                        toggleDevelopmentPackage();
                    }
                    ImGui::BeginDisabled(hasManagedVersion);
                    if (ImGui::MenuItem(hasManagedVersion
                            ? _L("app.settings.widgets_installed")
                            : _L("app.settings.widgets_install_managed")))
                    {
                        installLocalPackage(group.development->root, false,
                            group.id);
                    }
                    ImGui::EndDisabled();
                    if (workshopPublisherAvailable && ImGui::MenuItem(
                            _L("app.settings.widgets_upload_steam")))
                    {
                        if (LaunchSteamWorkshopPublisher(
                                WidgetEngine::GetWidgetPackagePaths().development,
                                group.development->root))
                            widgetPackageStatus_.clear();
                        else
                            widgetPackageStatus_ = _L(
                                "app.settings.widgets_publisher_launch_failed");
                    }
                }
                if (!workshopItemId.empty())
                {
                    ImGui::Separator();
                    if (ImGui::MenuItem(_L(
                            "app.settings.widgets_open_workshop_item")))
                    {
                        openWorkshopItem();
                    }
                }
                if (!group.managed && !group.invalidManaged.empty())
                {
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImVec4(0.86f, 0.22f, 0.20f, 1.0f));
                    if (ImGui::MenuItem(
                            _L("app.settings.widgets_uninstall")))
                    {
                        pendingWidgetPackageUninstall_ = group.id;
                        pendingWidgetPackageUninstallWorkshopItem_ =
                            workshopItemId.empty()
                            ? std::string{} : workshopExternalItemId;
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::EndPopup();
            }

            const float frameHeight = ImGui::GetFrameHeight();
            const float footerStart = cardHeight -
                frameHeight - 10.0f * dpiScale_;
            ImGui::SetCursorPosY(std::max(
                ImGui::GetCursorPosY(), footerStart));
            const char* controlLabel = group.development
                ? (group.development->active
                    ? _L("app.settings.widgets_development_deactivate")
                    : _L("app.settings.widgets_development_activate"))
                : group.managed
                    ? (group.managed->enabled
                        ? _L("app.settings.widgets_disable")
                        : _L("app.settings.widgets_enable"))
                    : nullptr;
            const auto compactButtonWidth = [&](const char* label)
            {
                return std::min(190.0f * dpiScale_,
                    ImGui::CalcTextSize(label).x + 24.0f * dpiScale_);
            };
            const float permissionWidth = compactButtonWidth(_L(
                "app.settings.widgets_manage_permissions"));
            const float controlWidth = controlLabel
                ? compactButtonWidth(controlLabel) : 0.0f;
            const float retryWidth = compactButtonWidth(_L(
                "app.settings.widgets_retry_install"));
            const float workshopWidth = compactButtonWidth(_L(
                "app.settings.widgets_open_workshop_item"));
            const float addWidth = std::min(180.0f * dpiScale_,
                compactButtonWidth(_L("app.widget_preview.add_to_desktop")));
            bool showPermissionAction = canManagePermissions(*package);
            bool showControlAction = controlLabel != nullptr;
            bool showRetryAction = workshopInstallFailure != nullptr;
            bool showWorkshopAction = !workshopItemId.empty();
            const float actionGap = ImGui::GetStyle().ItemSpacing.x;
            const float availableActionWidth =
                ImGui::GetWindowContentRegionMax().x -
                ImGui::GetWindowContentRegionMin().x;
            const auto totalActionWidth = [&]()
            {
                float total = addWidth;
                int count = 1;
                const auto append = [&](bool visible, float width)
                {
                    if (!visible) return;
                    total += width;
                    ++count;
                };
                append(showPermissionAction, permissionWidth);
                append(showControlAction, controlWidth);
                append(showRetryAction, retryWidth);
                append(showWorkshopAction, workshopWidth);
                return total + actionGap * static_cast<float>(count - 1);
            };
            if (totalActionWidth() > availableActionWidth)
                showPermissionAction = false;
            if (totalActionWidth() > availableActionWidth)
                showWorkshopAction = false;
            if (totalActionWidth() > availableActionWidth)
                showControlAction = false;
            const float visibleActionsWidth = totalActionWidth();
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                ImGui::GetWindowContentRegionMax().x -
                    visibleActionsWidth));
            bool drewAction = false;
            const auto nextAction = [&]()
            {
                if (drewAction) ImGui::SameLine();
                drewAction = true;
            };
            if (showPermissionAction)
            {
                nextAction();
                if (SecondaryButton(_L(
                        "app.settings.widgets_manage_permissions"),
                        ImVec2(permissionWidth, 0)))
                {
                    openPermissionEditor(*package);
                }
            }
            if (showControlAction)
            {
                nextAction();
                if (SecondaryButton(controlLabel,
                        ImVec2(controlWidth, 0)))
                {
                    if (group.development)
                        toggleDevelopmentPackage();
                    else
                        toggleManagedPackage();
                }
            }
            if (showRetryAction)
            {
                nextAction();
                if (SecondaryButton(_L(
                        "app.settings.widgets_retry_install"),
                        ImVec2(retryWidth, 0)))
                {
                    retryWorkshopInstall();
                }
            }
            if (showWorkshopAction)
            {
                nextAction();
                if (SecondaryButton(_L(
                        "app.settings.widgets_open_workshop_item"),
                        ImVec2(workshopWidth, 0)))
                {
                    openWorkshopItem();
                }
            }
            const bool canAdd = package->active && package->enabled &&
                addWidgetToDesktopCallback_;
            nextAction();
            ImGui::BeginDisabled(!canAdd);
            if (BlueButton(_L("app.widget_preview.add_to_desktop"),
                    ImVec2(addWidth, 0)))
            {
                widgetPackageStatus_ = addWidgetToDesktopCallback_(
                    Utf8ToWide(group.id))
                    ? _L("app.settings.widgets_add_to_desktop_ok")
                    : _L("app.settings.widgets_add_to_desktop_failed");
            }
            ImGui::EndDisabled();

            ImGui::EndChild();
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, cardGap));
            ImGui::PopID();
        }
    }

    std::vector<const PackageGroup*> visibleBuiltinGroups;
    for (const auto& group : packageGroups)
    {
        if ((!group.builtin && group.invalidBuiltin.empty()) ||
            group.managed || group.development ||
            !group.invalidManaged.empty() ||
            !group.invalidDevelopment.empty() ||
            !group.workshopInstallFailures.empty())
            continue;
        if (matchesSearch(group)) visibleBuiltinGroups.push_back(&group);
    }
    if (!visibleBuiltinGroups.empty())
    {
        ImGui::Spacing();
        std::string builtinHeader =
            _L("app.settings.widgets_filter_builtin");
        builtinHeader += " ";
        builtinHeader += std::to_string(visibleBuiltinGroups.size());
        builtinHeader += "###BuiltinWidgetPackages";
        if (!searchText.empty())
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (ImGui::CollapsingHeader(builtinHeader.c_str()))
        {
            for (const PackageGroup* group : visibleBuiltinGroups)
            {
                const auto* package = displayPackage(*group);
                if (!package)
                {
                    const auto* invalid = group->invalidBuiltin.empty()
                        ? nullptr : group->invalidBuiltin.front();
                    if (!invalid) continue;
                    const auto invalidManifest =
                        localizedManifest(invalid->manifest);
                    const std::string name = !invalidManifest.name.empty()
                        ? invalidManifest.name : group->id;
                    ImGui::PushID(("builtin-invalid-" + group->id).c_str());
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(
                        ImVec4(0.82f, 0.30f, 0.27f, 1.0f), "%s · %s",
                        _L("app.settings.widgets_filter_builtin"),
                        _L("app.settings.invalid"));
                    const auto issue = std::find_if(
                        invalid->report.issues.begin(),
                        invalid->report.issues.end(),
                        [](const auto& candidate)
                        {
                            return candidate.severity == snowdesktop::widget::
                                ValidationSeverity::Error;
                        });
                    if (issue != invalid->report.issues.end())
                        ImGui::TextDisabled("[%s] %s", issue->code.c_str(),
                            issue->message.c_str());
                    ImGui::Separator();
                    ImGui::PopID();
                    continue;
                }
                const auto manifest = localizedManifest(package->manifest);
                ImGui::PushID(("builtin-" + group->id).c_str());
                const float rowHeight = 70.0f * dpiScale_;
                ImGui::BeginChild("##BuiltinWidgetRow",
                    ImVec2(0, rowHeight), ImGuiChildFlags_None);
                const float addWidth = std::min(
                    150.0f * dpiScale_,
                    ImGui::CalcTextSize(
                        _L("app.widget_preview.add_to_desktop")).x +
                        24.0f * dpiScale_);
                ImGui::TextUnformatted(manifest.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled(_L("app.settings.widgets_version"),
                    manifest.version.c_str());
                if (!manifest.description.empty())
                {
                    ImGui::PushTextWrapPos(
                        ImGui::GetWindowContentRegionMax().x - addWidth -
                        12.0f * dpiScale_);
                    ImGui::TextDisabled("%s", manifest.description.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::TextDisabled("%s: %s",
                    _L("app.settings.widgets_permission_state"),
                    effectivePermissionStateLabel(*package));
                const bool canManage = canManagePermissions(*package);
                const float manageWidth = canManage
                    ? std::min(160.0f * dpiScale_,
                        ImGui::CalcTextSize(_L(
                            "app.settings.widgets_manage_permissions")).x +
                            24.0f * dpiScale_)
                    : 0.0f;
                const float actionGap = canManage
                    ? ImGui::GetStyle().ItemSpacing.x : 0.0f;
                ImGui::SetCursorPos(ImVec2(
                    ImGui::GetWindowContentRegionMax().x - addWidth -
                        manageWidth - actionGap,
                    (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
                if (canManage && SecondaryButton(
                        _L("app.settings.widgets_manage_permissions"),
                        ImVec2(manageWidth, 0)))
                {
                    openPermissionEditor(*package);
                }
                if (canManage) ImGui::SameLine();
                ImGui::BeginDisabled(!addWidgetToDesktopCallback_);
                if (SecondaryButton(
                        _L("app.widget_preview.add_to_desktop"),
                        ImVec2(addWidth, 0)))
                {
                    widgetPackageStatus_ = addWidgetToDesktopCallback_(
                        Utf8ToWide(group->id))
                        ? _L("app.settings.widgets_add_to_desktop_ok")
                        : _L("app.settings.widgets_add_to_desktop_failed");
                }
                ImGui::EndDisabled();
                ImGui::EndChild();
                ImGui::Separator();
                ImGui::PopID();
            }
        }
    }

    ImGui::EndChild();

    const std::string permissionEditorPopup =
        std::string(_L("app.settings.widgets_manage_permissions")) +
        "##WidgetPermissionEditor";
    if (openPermissionEditorRequested)
        ImGui::OpenPopup(permissionEditorPopup.c_str());
    ImGui::SetNextWindowSize(
        ImVec2(560.0f * dpiScale_, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(permissionEditorPopup.c_str(), nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        const auto permissionPackage = std::find_if(
            packages.begin(), packages.end(), [&](const auto& candidate) {
                return candidate.manifest.id ==
                        widgetPermissionEditorPackageId_ &&
                    candidate.manifest.version ==
                        widgetPermissionEditorPackageVersion_ &&
                    candidate.source == widgetPermissionEditorSource_ &&
                    candidate.selected;
            });
        if (permissionPackage == packages.end())
        {
            ImGui::TextWrapped("%s",
                _L("app.settings.widgets_permissions_scope_changed"));
            if (SecondaryButton(_L("app.settings.cancel")))
            {
                widgetPermissionEditorPackageId_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        else
        {
            const auto manifest = localizedManifest(
                permissionPackage->manifest);
            const std::string currentFingerprint =
                snowdesktop::widget::WidgetPermissionBroker::
                    ScopeFingerprint(
                        permissionPackage->manifest.permissions,
                        permissionPackage->manifest.optionalPermissions,
                        permissionPackage->manifest.networkDomains);
            const bool scopeChanged =
                currentFingerprint !=
                    widgetPermissionEditorScopeFingerprint_;
            ImGui::TextUnformatted(manifest.name.c_str());
            ImGui::TextDisabled("%s: %s",
                _L("app.settings.widgets_permission_state"),
                effectivePermissionStateLabel(*permissionPackage));
            ImGui::Spacing();

            auto drawPermissionToggle = [&](const std::string& permission,
                std::string_view idPrefix)
            {
                bool selected = std::find(
                    widgetPermissionEditorGrants_.begin(),
                    widgetPermissionEditorGrants_.end(), permission) !=
                    widgetPermissionEditorGrants_.end();
                std::string label = permissionLabel(permission);
                label += "##";
                label += idPrefix;
                label += permission;
                if (ImGui::Checkbox(label.c_str(), &selected))
                {
                    if (selected)
                    {
                        widgetPermissionEditorGrants_.push_back(
                            permission);
                    }
                    else
                    {
                        std::erase(widgetPermissionEditorGrants_,
                            permission);
                    }
                }
            };

            if (!permissionPackage->manifest.permissions.empty())
            {
                ImGui::TextUnformatted(_L(
                    "app.widget_permission.permissions_required_heading"));
                ImGui::TextDisabled("%s", _L(
                    "app.settings.widgets_permission_required_notice"));
                for (const auto& permission :
                    permissionPackage->manifest.permissions)
                {
                    drawPermissionToggle(permission, "required-");
                }
            }
            if (!permissionPackage->manifest.optionalPermissions.empty())
            {
                ImGui::Spacing();
                ImGui::TextUnformatted(_L(
                    "app.widget_permission.permissions_optional_heading"));
                for (const auto& permission :
                    permissionPackage->manifest.optionalPermissions)
                {
                    drawPermissionToggle(permission, "optional-");
                }
            }
            if (!permissionPackage->manifest.networkDomains.empty())
            {
                ImGui::Spacing();
                ImGui::TextUnformatted(_L(
                    "app.widget_permission.domains_heading"));
                for (const auto& domain :
                    permissionPackage->manifest.networkDomains)
                {
                    ImGui::BulletText("%s", domain.c_str());
                }
            }
            if (scopeChanged)
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.86f, 0.44f, 0.12f, 1.0f));
                ImGui::TextWrapped("%s", _L(
                    "app.settings.widgets_permissions_scope_changed"));
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::BeginDisabled(scopeChanged);
            if (BlueButton(_L("app.settings.apply")))
            {
                std::vector<std::string> grantedPermissions;
                for (const auto& permission :
                    snowdesktop::widget::DeclaredPermissions(
                        permissionPackage->manifest))
                {
                    if (std::find(widgetPermissionEditorGrants_.begin(),
                            widgetPermissionEditorGrants_.end(),
                            permission) !=
                        widgetPermissionEditorGrants_.end())
                    {
                        grantedPermissions.push_back(permission);
                    }
                }
                std::vector<std::string> grantedDomains;
                if (std::find(grantedPermissions.begin(),
                        grantedPermissions.end(), "network.http") !=
                    grantedPermissions.end())
                {
                    grantedDomains =
                        permissionPackage->manifest.networkDomains;
                }
                std::string permissionError;
                const std::wstring packageId = Utf8ToWide(
                    permissionPackage->manifest.id);
                const bool applied = widgetEngine_
                    ? widgetEngine_->ApplyWidgetPermissionDecision(
                        packageId,
                        snowdesktop::widget::PermissionDecisionState::
                            Granted,
                        grantedPermissions, grantedDomains,
                        permissionError)
                    : WidgetEngine::SetWidgetPermissionDecision(
                        packageId,
                        snowdesktop::widget::PermissionDecisionState::
                            Granted,
                        grantedPermissions, grantedDomains,
                        permissionError);
                if (applied)
                {
                    widgetPackageStatus_ = _L(
                        "app.settings.widgets_permissions_updated_ok");
                    widgetPermissionEditorPackageId_.clear();
                    if (reloadCallback_) reloadCallback_();
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    widgetPackageStatus_ = permissionError;
                }
            }
            ImGui::EndDisabled();
            if (canRevokePermissions(*permissionPackage))
            {
                ImGui::SameLine();
                if (SecondaryButton(_L(
                        "app.settings.widgets_revoke_permissions")))
                {
                    if (revokePermissions(*permissionPackage))
                    {
                        widgetPermissionEditorPackageId_.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::SameLine();
            if (SecondaryButton(_L("app.settings.cancel")))
            {
                widgetPermissionEditorPackageId_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    if (pendingWidgetInstallKind_ != PendingWidgetInstallKind::None)
        ImGui::OpenPopup("##ConfirmWidgetPackageInstall");
    if (ImGui::BeginPopupModal("##ConfirmWidgetPackageInstall", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s",
            _L("app.settings.widgets_install_confirm"));
        if (!pendingWidgetInstallReason_.empty())
        {
            const std::string rawReason =
                WideToUtf8(pendingWidgetInstallReason_);
            const std::string permissionMarker =
                "requests a new permission: ";
            const std::string domainMarker =
                "expands network access to: ";
            if (const auto position = rawReason.find(permissionMarker);
                position != std::string::npos)
            {
                const std::string permission = rawReason.substr(
                    position + permissionMarker.size());
                ImGui::TextWrapped(
                    _L("app.settings.widgets_new_permission"),
                    permissionLabel(permission));
            }
            else if (const auto domainPosition = rawReason.find(domainMarker);
                domainPosition != std::string::npos)
            {
                ImGui::TextWrapped(
                    _L("app.settings.widgets_new_website"),
                    rawReason.substr(
                        domainPosition + domainMarker.size()).c_str());
            }
            else
            {
                ImGui::TextWrapped("%s",
                    _L("app.settings.widgets_source_change"));
            }
            if (ImGui::CollapsingHeader(
                _L("app.settings.widgets_technical_details")))
            {
                ImGui::TextWrapped("%s", rawReason.c_str());
            }
        }
        if (BlueButton(_L("app.settings.widgets_confirm_install")))
        {
            const std::filesystem::path path =
                pendingWidgetInstallPath_;
            const std::string developmentId =
                pendingWidgetInstallDevelopmentId_;
            pendingWidgetInstallKind_ = PendingWidgetInstallKind::None;
            installLocalPackage(path, true, developmentId);
            if (pendingWidgetInstallKind_ ==
                PendingWidgetInstallKind::None)
                ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(_L("app.settings.cancel")))
        {
            pendingWidgetInstallKind_ = PendingWidgetInstallKind::None;
            pendingWidgetInstallPath_.clear();
            pendingWidgetInstallReason_.clear();
            pendingWidgetInstallDevelopmentId_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!pendingWidgetPackageUninstall_.empty())
        ImGui::OpenPopup("##ConfirmWidgetPackageUninstall");
    if (ImGui::IsPopupOpen("##ConfirmWidgetPackageUninstall"))
    {
        const float uninstallPopupWidth = std::clamp(
            440.0f * dpiScale_, 320.0f * dpiScale_,
            std::max(320.0f * dpiScale_,
                ImGui::GetIO().DisplaySize.x - 48.0f * dpiScale_));
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(uninstallPopupWidth, 0.0f),
            ImVec2(uninstallPopupWidth,
                std::max(160.0f * dpiScale_,
                    ImGui::GetIO().DisplaySize.y - 48.0f * dpiScale_)));
    }
    if (ImGui::BeginPopupModal("##ConfirmWidgetPackageUninstall", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("%s",
            _L(pendingWidgetPackageUninstallWorkshopItem_.empty()
                ? "app.settings.widgets_uninstall_confirm"
                : "app.settings.widgets_uninstall_workshop_confirm"));
        if (BlueButton(_L(
            pendingWidgetPackageUninstallWorkshopItem_.empty()
                ? "app.settings.widgets_uninstall"
                : "app.settings.widgets_unsubscribe_and_uninstall")))
        {
            std::string error;
            const std::string packageId = pendingWidgetPackageUninstall_;
            bool mayUninstall = true;
            if (!pendingWidgetPackageUninstallWorkshopItem_.empty() &&
                !WidgetEngine::UnsubscribeSteamWorkshopItem(
                    pendingWidgetPackageUninstallWorkshopItem_, error))
            {
                char message[1024]{};
                std::snprintf(message, sizeof(message),
                    _L("app.settings.widgets_unsubscribe_failed"),
                    error.c_str());
                widgetPackageStatus_ = message;
                mayUninstall = false;
            }

            std::vector<std::wstring> instances;
            if (mayUninstall && widgetEngine_)
            {
                for (const auto& widget : widgetEngine_->GetWidgets())
                    if (widget.packageId == packageId)
                        instances.push_back(widget.widgetId);
                for (const auto& instance : instances)
                    widgetEngine_->UnloadWidget(instance);
            }
            if (mayUninstall &&
                WidgetEngine::UninstallWidgetPackage(packageId, error))
            {
                if (widgetEngine_)
                    widgetEngine_->RevokeFilesystemHandlesForPackage(
                        packageId);
                widgetPackageStatus_ = _L(
                    pendingWidgetPackageUninstallWorkshopItem_.empty()
                        ? "app.settings.widgets_uninstall_ok"
                        : "app.settings.widgets_uninstall_workshop_ok");
                pendingWidgetPackageUninstall_.clear();
                pendingWidgetPackageUninstallWorkshopItem_.clear();
                ImGui::CloseCurrentPopup();
                if (reloadCallback_) reloadCallback_();
            }
            else if (mayUninstall)
            {
                for (const auto& instance : instances)
                    if (widgetEngine_)
                        (void)widgetEngine_->ReloadWidget(instance);
                widgetPackageStatus_ = error;
            }
        }
        ImGui::SameLine();
        if (SecondaryButton(_L("app.settings.cancel")))
        {
            pendingWidgetPackageUninstall_.clear();
            pendingWidgetPackageUninstallWorkshopItem_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

void SettingsWindow::RefreshAgentSkillStatuses()
{
    agentSkillStatuses_.clear();
    agentSkillStatusError_.clear();
    const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
    const auto bundledSkill =
        packagePaths.builtin / L"snowdesktop-lua-widget";
    const auto bundledCli = bundledSkill / L"bin" / L"snowwidget.exe";
    for (auto target :
        snowdesktop::steam_bridge::DefaultAgentSkillTargets())
    {
        std::string error;
        auto status = snowdesktop::steam_bridge::InspectAgentSkill(
            bundledSkill, bundledCli, std::move(target), error);
        if (!error.empty())
        {
            if (!agentSkillStatusError_.empty())
                agentSkillStatusError_ += "\n";
            agentSkillStatusError_ += status.agent.id + ": " + error;
        }
        agentSkillStatuses_.push_back(std::move(status));
    }
}

void SettingsWindow::DrawWidgetDeveloperTools()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##WidgetAuthoringPage", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::PopStyleVar();

    const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
    std::error_code createError;
    std::filesystem::create_directories(
        packagePaths.development, createError);
    if (agentSkillStatuses_.empty()) RefreshAgentSkillStatuses();

    DrawSettingSection(_L("app.settings.widgets_agent_skill"));
    ImGui::TextWrapped("%s",
        _L("app.settings.widgets_agent_skill_description"));
    ImGui::Spacing();

    const auto stateLabel = [](snowdesktop::steam_bridge::SkillInstallState state)
    {
        using snowdesktop::steam_bridge::SkillInstallState;
        switch (state)
        {
        case SkillInstallState::NotInstalled:
            return _L("app.settings.widgets_skill_not_installed");
        case SkillInstallState::UpdateAvailable:
            return _L("app.settings.widgets_skill_update_available");
        case SkillInstallState::Current:
            return _L("app.settings.widgets_skill_current");
        default:
            return _L("app.settings.widgets_skill_unavailable");
        }
    };
    const auto targetLabel = [](snowdesktop::steam_bridge::AgentSkillTargetKind kind)
    {
        using snowdesktop::steam_bridge::AgentSkillTargetKind;
        switch (kind)
        {
        case AgentSkillTargetKind::Shared:
            return _L("app.settings.widgets_skill_shared");
        case AgentSkillTargetKind::Codex: return "Codex";
        case AgentSkillTargetKind::ClaudeCode: return "Claude Code";
        case AgentSkillTargetKind::Cursor: return "Cursor";
        case AgentSkillTargetKind::GitHubCopilot: return "GitHub Copilot";
        case AgentSkillTargetKind::GeminiCli: return "Gemini CLI";
        default: return "Agent Skills";
        }
    };
    const auto needsInstall = [](const auto& status)
    {
        using snowdesktop::steam_bridge::SkillInstallState;
        return status.state == SkillInstallState::NotInstalled ||
            status.state == SkillInstallState::UpdateAvailable;
    };
    const auto targetBit = [](const auto& status)
    {
        return 1 << static_cast<int>(status.agent.kind);
    };
    const auto targetSelected = [&](const auto& status)
    {
        return (generalSettings_.agentSkillTargetMask &
            targetBit(status)) != 0;
    };
    const auto targetInstalled = [](const auto& status)
    {
        using snowdesktop::steam_bridge::SkillInstallState;
        if (status.state == SkillInstallState::Current ||
            status.state == SkillInstallState::UpdateAvailable)
            return true;
        std::error_code error;
        return std::filesystem::exists(status.target, error) && !error;
    };
    const auto needsApply = [&](const auto& status)
    {
        return targetSelected(status)
            ? needsInstall(status)
            : targetInstalled(status);
    };
    const int pendingTargets = static_cast<int>(std::count_if(
        agentSkillStatuses_.begin(), agentSkillStatuses_.end(),
        needsApply));
    const bool selectedTargetUnavailable = std::any_of(
        agentSkillStatuses_.begin(), agentSkillStatuses_.end(),
        [&](const auto& status)
        {
            return targetSelected(status) && status.state ==
                snowdesktop::steam_bridge::SkillInstallState::Unavailable;
        });
    const bool selectionApplied = !agentSkillStatuses_.empty() &&
        pendingTargets == 0 && !selectedTargetUnavailable;
    const char* syncLabel = pendingTargets > 0
        ? _L("app.settings.widgets_skill_sync_all")
        : selectionApplied
        ? _L("app.settings.widgets_skill_all_current")
        : _L("app.settings.widgets_skill_unavailable");
    const char* checkLabel = _L("app.settings.widgets_skill_check_updates");
    const float skillActions = SettingButtonWidth(syncLabel) +
        SettingButtonWidth(checkLabel) + ImGui::GetStyle().ItemSpacing.x;
    BeginSettingRow(_L("app.settings.widgets_skill_all_agents"),
        skillActions);
    ImGui::BeginDisabled(pendingTargets == 0);
    if (BlueButton(syncLabel))
    {
        int installed = 0;
        int removed = 0;
        int failed = 0;
        std::string firstError;
        for (const auto& status : agentSkillStatuses_)
        {
            if (!needsApply(status)) continue;
            std::string error;
            const bool selected = targetSelected(status);
            const bool ok = selected
                ? snowdesktop::steam_bridge::InstallOrUpdateAgentSkill(
                    status, error)
                : snowdesktop::steam_bridge::UninstallAgentSkill(
                    status, error);
            if (ok)
            {
                selected ? ++installed : ++removed;
                continue;
            }
            ++failed;
            if (firstError.empty()) firstError = std::move(error);
        }
        RefreshAgentSkillStatuses();
        char message[512]{};
        std::snprintf(message, sizeof(message),
            failed == 0
                ? _L("app.settings.widgets_skill_sync_success")
                : _L("app.settings.widgets_skill_sync_partial"),
            installed, removed, failed);
        agentSkillActionStatus_ = message;
        if (!firstError.empty())
            agentSkillActionStatus_ += "\n" + firstError;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (SecondaryButton(checkLabel))
    {
        RefreshAgentSkillStatuses();
        agentSkillActionStatus_ =
            _L("app.settings.widgets_skill_check_complete");
    }

    for (const auto& status : agentSkillStatuses_)
    {
        ImGui::PushID(status.agent.id.c_str());
        bool selected = targetSelected(status);
        BeginSettingRow(targetLabel(status.agent.kind),
            ImGui::GetFrameHeight());
        if (ImGui::Checkbox("##AgentSkillTarget", &selected))
        {
            const int bit = targetBit(status);
            if (selected)
                generalSettings_.agentSkillTargetMask |= bit;
            else
                generalSettings_.agentSkillTargetMask &= ~bit;
            generalSettingsDirty_ = true;
        }
        ImGui::TextDisabled("%s · %s", stateLabel(status.state),
            WideToUtf8(status.target.wstring()).c_str());
        ImGui::PopID();
    }
    if (!agentSkillStatusError_.empty())
        ImGui::TextWrapped("%s", agentSkillStatusError_.c_str());
    if (!agentSkillActionStatus_.empty())
        ImGui::TextWrapped("%s", agentSkillActionStatus_.c_str());

    ImGui::Spacing();
    DrawSettingSection(_L("app.settings.widgets_authoring_workspace"));
    const char* openDevelopment =
        _L("app.settings.widgets_open_development_folder");
    BeginSettingRow(_L("app.settings.widgets_authoring_folder"),
        SettingButtonWidth(openDevelopment));
    if (SecondaryButton(openDevelopment))
    {
        ShellExecuteW(nullptr, L"open",
            packagePaths.development.c_str(), nullptr, nullptr, SW_SHOW);
    }
    ImGui::TextDisabled("%s",
        WideToUtf8(packagePaths.development.wstring()).c_str());

    ImGui::Spacing();
    DrawSettingSection(_L("app.settings.widgets_component_cli"));
    const auto bundledSkill =
        packagePaths.builtin / L"snowdesktop-lua-widget";
    const auto cli = bundledSkill / L"bin" / L"snowwidget.exe";
    const std::string quotedCli = "\"" + WideToUtf8(cli.wstring()) + "\"";
    const std::string quotedProject = "\"" +
        WideToUtf8((packagePaths.development / L"my-widget").wstring()) +
        "\"";
    const std::string quotedOutput = "\"" + WideToUtf8(
        (packagePaths.development / L"my-widget.snowwidget").wstring()) +
        "\"";
    const auto commandRow = [&](const char* label,
        const std::string& command, const char* stableId)
    {
        ImGui::PushID(stableId);
        const char* copyLabel = _L("app.settings.widgets_copy_command");
        const float copyWidth = SettingButtonWidth(copyLabel);
        const float controlWidth = kSettingControlWidthDip * dpiScale_;
        BeginSettingRow(label, controlWidth);
        ImGui::SetNextItemWidth(std::max(80.0f * dpiScale_,
            controlWidth - copyWidth - ImGui::GetStyle().ItemSpacing.x));
        std::vector<char> buffer(command.begin(), command.end());
        buffer.push_back('\0');
        ImGui::InputText("##Command", buffer.data(), buffer.size(),
            ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (SecondaryButton(copyLabel, ImVec2(copyWidth, 0)))
        {
            ImGui::SetClipboardText(command.c_str());
            agentSkillActionStatus_ =
                _L("app.settings.widgets_command_copied");
        }
        ImGui::PopID();
    };
    commandRow(_L("app.settings.widgets_cli_capabilities"),
        quotedCli + " capabilities", "AuthoringCapabilities");
    commandRow(_L("app.settings.widgets_cli_validate"),
        quotedCli + " validate " + quotedProject, "AuthoringValidate");
    commandRow(_L("app.settings.widgets_cli_pack"),
        quotedCli + " pack " + quotedProject + " " + quotedOutput,
        "AuthoringPack");
    ImGui::TextDisabled("%s", WideToUtf8(cli.wstring()).c_str());

    if (IsSteamWorkshopPublisherAvailable())
    {
        ImGui::Spacing();
        DrawSettingSection(_L("app.settings.widgets_authoring_publish"));
        ImGui::TextWrapped("%s",
            _L("app.settings.widgets_authoring_publish_description"));
        if (BlueButton(_L("app.settings.widgets_publish_steam")))
        {
            if (!LaunchSteamWorkshopPublisher(packagePaths.development))
                agentSkillActionStatus_ =
                    _L("app.settings.widgets_publisher_launch_failed");
        }
    }

    ImGui::Spacing();
    DrawSettingSection(_L("app.settings.widgets_authoring_reference"));
    ImGui::TextWrapped("%s",
        _L("app.settings.widgets_authoring_reference_description"));
    ImGui::Spacing();
    DrawSettingSection(_L("app.settings.widgets_icon_reference"));

    const auto drawIconGrid = [&](const char* titleKey,
        const char* hintKey, const char* notFoundKey,
        const char* countKey, const char* tooltipKey,
        const char* childId, const char* itemIdPrefix,
        ImFont* font, std::vector<unsigned int>& codepoints,
        std::initializer_list<std::pair<unsigned int, unsigned int>> ranges) {
        if (!DrawCollapsingHeaderWithHelp(_L(titleKey), _L(hintKey)))
            return;
        if (font && codepoints.empty())
        {
            for (const auto& [first, last] : ranges)
            {
                for (unsigned int codepoint = first;
                    codepoint <= last; ++codepoint)
                {
                    if (font->IsGlyphInFont(
                            static_cast<ImWchar>(codepoint)))
                        codepoints.push_back(codepoint);
                }
            }
        }
        if (!font || codepoints.empty())
        {
            ImGui::TextDisabled("%s", _L(notFoundKey));
            ImGui::Spacing();
            return;
        }

        ImGui::Text(_L(countKey), static_cast<int>(codepoints.size()));
        const float buttonSize = 38.0f * dpiScale_;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, static_cast<int>(
            ImGui::GetContentRegionAvail().x / (buttonSize + spacing)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(8.0f * dpiScale_, 8.0f * dpiScale_));
        ImGui::BeginChild(childId, ImVec2(0, 220.0f * dpiScale_),
            ImGuiChildFlags_Borders |
                ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        const int rowCount = static_cast<int>(
            (codepoints.size() + static_cast<size_t>(columns) - 1) /
            static_cast<size_t>(columns));
        ImGuiListClipper clipper;
        clipper.Begin(rowCount,
            buttonSize + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart;
                row < clipper.DisplayEnd; ++row)
            {
                for (int column = 0; column < columns; ++column)
                {
                    const size_t i = static_cast<size_t>(
                        row * columns + column);
                    if (i >= codepoints.size())
                        break;
                    const unsigned int codepoint = codepoints[i];
                    const std::string glyph = CodepointToUtf8(codepoint);
                    const std::string buttonLabel = glyph + itemIdPrefix +
                        std::to_string(codepoint);
                    ImGui::PushFont(font, 18.0f * dpiScale_);
                    const bool clicked = ImGui::Button(buttonLabel.c_str(),
                        ImVec2(buttonSize, buttonSize));
                    ImGui::PopFont();
                    if (clicked)
                        ImGui::SetClipboardText(glyph.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(_L(tooltipKey), codepoint);
                    if (column + 1 < columns && i + 1 < codepoints.size())
                        ImGui::SameLine();
                }
            }
        }
        ImGui::EndChild();
        ImGui::Spacing();
    };

    drawIconGrid("app.settings.fluent_icons",
        "app.settings.fluent_icon_hint",
        "app.settings.fluent_not_found",
        "app.settings.fluent_valid_chars",
        "app.settings.fluent_copy_tooltip",
        "##FluentRegularGlyphs", "##fluent",
        fluentDebugFont_, fluentDebugCodepoints_,
        { { 0xE000, 0xF8FF }, { 0xF0000, 0xF0CCE } });
    drawIconGrid("app.settings.fa_icons",
        "app.settings.fa_icon_hint", "app.settings.fa_not_found",
        "app.settings.fa_valid_chars",
        "app.settings.fa_copy_tooltip",
        "##FontAwesomeGlyphs", "##fa",
        faDebugFont_, faDebugCodepoints_, { { 0xE000, 0xF8FF } });

    ImGui::Spacing();
    DrawSettingSection(
        _L("app.settings.widgets_runtime_diagnostics"));
    std::vector<WidgetErrorEntry> errors;
    if (widgetEngine_)
        errors = widgetEngine_->GetWidgetErrors();
    char errorLabel[256]{};
    std::snprintf(errorLabel, sizeof(errorLabel),
        _L("app.settings.error_count"), static_cast<int>(errors.size()));
    const char* copyAllLabel = _L("app.settings.copy_all");
    const char* clearAllLabel = _L("app.settings.clear_all");
    const float errorActionsWidth = SettingButtonWidth(copyAllLabel) +
        SettingButtonWidth(clearAllLabel) +
        ImGui::GetStyle().ItemSpacing.x;
    BeginSettingRow(errorLabel, errorActionsWidth);
    ImGui::BeginDisabled(errors.empty());
    if (SecondaryButton(_L("app.settings.copy_all")))
    {
        std::string copyText;
        for (const auto& e : errors)
        {
            copyText += "[" + e.key + "]\n";
            copyText += e.message;
            copyText += "\n\n";
        }
        ImGui::SetClipboardText(copyText.c_str());
    }
    ImGui::SameLine();
    if (SecondaryButton(_L("app.settings.clear_all")))
    {
        if (widgetEngine_)
            widgetEngine_->ClearWidgetErrors();
        errors.clear();
    }
    ImGui::EndDisabled();

    if (errors.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_widget_errors"));
    }
    else
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(8.0f * dpiScale_, 8.0f * dpiScale_));
        ImGui::BeginChild("##WidgetErrorLog",
            ImVec2(0, 160.0f * dpiScale_),
            ImGuiChildFlags_Borders |
                ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleVar();
        for (std::size_t errorIndex = 0;
            errorIndex < errors.size(); ++errorIndex)
        {
            const auto& e = errors[errorIndex];
            ImGui::PushID(static_cast<int>(errorIndex));
            std::string itemText = "[" + e.key + "]\n" + e.message;
            if (ImGui::Selectable(itemText.c_str(), false,
                ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0)))
                ImGui::SetClipboardText(itemText.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _L("app.settings.copy_error"));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    std::vector<WidgetDiagnosticEntry> diagnostics;
    if (widgetEngine_)
        diagnostics = widgetEngine_->GetWidgetDiagnostics();
    char diagnosticLabel[256]{};
    std::snprintf(diagnosticLabel, sizeof(diagnosticLabel),
        _L("app.settings.widgets_diagnostic_count"),
        static_cast<int>(diagnostics.size()));
    const char* copyDiagnosticLabel = _L("app.settings.copy_diag");
    BeginSettingRow(diagnosticLabel,
        SettingButtonWidth(copyDiagnosticLabel));
    ImGui::BeginDisabled(diagnostics.empty());
    if (SecondaryButton(copyDiagnosticLabel))
    {
        std::string text;
        for (const auto& d : diagnostics)
        {
            text += "[" + WideToUtf8(d.widgetId) + "] " + d.name + "\n";
            text += std::string("valid=") +
                (d.valid ? "true" : "false") + ", manifest=" +
                (d.hasManifest ? "true" : "false") + "\n";
            text += "permissions=";
            for (size_t i = 0; i < d.permissions.size(); ++i)
            {
                if (i > 0) text += ",";
                text += d.permissions[i];
            }
            text += "\n";
            if (!d.lastError.empty())
                text += "lastError=" + d.lastError + "\n";
            for (const auto& log : d.logs)
                text += log.level + ": " + log.message + "\n";
            const auto appendViewNodes = [&](std::string_view surface,
                                             const auto& nodes) {
                for (const auto& node : nodes)
                {
                    text += "view[" + std::string(surface) + "] depth=" +
                        std::to_string(node.depth) + ", type=" +
                        snowdesktop::widget_runtime::ViewNodeTypeName(
                            node.type) + ", key=" + node.key;
                    if (!node.debugName.empty())
                        text += ", debugName=" + node.debugName;
                    if (!node.testId.empty())
                        text += ", testId=" + node.testId;
                    text += ", frame=" + std::to_string(node.frame.x) +
                        "," + std::to_string(node.frame.y) + "," +
                        std::to_string(node.frame.width) + "," +
                        std::to_string(node.frame.height) + "\n";
                }
            };
            appendViewNodes("desktop", d.desktopViewNodes);
            appendViewNodes(d.auxiliarySurface.empty()
                    ? std::string_view("auxiliary")
                    : std::string_view(d.auxiliarySurface),
                d.auxiliaryViewNodes);
            text += "\n";
        }
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::EndDisabled();

    if (diagnostics.empty())
        ImGui::TextDisabled("%s", _L("app.settings.no_widgets_loaded"));
    else
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(8.0f * dpiScale_, 8.0f * dpiScale_));
        ImGui::BeginChild("##WidgetDiagnostics",
            ImVec2(0, 180.0f * dpiScale_),
            ImGuiChildFlags_Borders |
                ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleVar();
        for (auto& d : diagnostics)
        {
            const std::string widgetId = WideToUtf8(d.widgetId);
            std::string header = "[" + widgetId + "] " + d.name +
                "###WidgetDiagnostic" + widgetId;
            if (ImGui::CollapsingHeader(header.c_str(),
                ImGuiTreeNodeFlags_SpanAvailWidth |
                    ImGuiTreeNodeFlags_FramePadding))
            {
                ImGui::Text(_L("app.settings.debug_script"),
                    WideToUtf8(d.scriptPath).c_str());
                ImGui::Text(_L("app.settings.debug_status_manifest"),
                    d.valid ? _L("app.settings.valid") : _L("app.settings.invalid"),
                    d.hasManifest ? _L("app.settings.yes") : _L("app.settings.no"));
                std::string perms;
                for (size_t i = 0; i < d.permissions.size(); ++i)
                {
                    if (i > 0) perms += ", ";
                    perms += d.permissions[i];
                }
                ImGui::Text(_L("app.settings.debug_permissions"),
                    perms.empty() ? _L("app.settings.none") : perms.c_str());
                if (!d.lastError.empty())
                    ImGui::TextWrapped(_L("app.settings.debug_last_error"),
                        d.lastError.c_str());
                if (SecondaryButton((std::string(
                    _L("app.settings.reload")) + "##" +
                    WideToUtf8(d.widgetId)).c_str(),
                    ImVec2(96.0f * dpiScale_, 0)))
                {
                    if (widgetEngine_)
                        widgetEngine_->ReloadWidget(d.widgetId);
                    if (invalidateCallback_)
                        invalidateCallback_();
                }
                if (!d.logs.empty())
                {
                    ImGui::Text("%s", _L("app.settings.recent_logs"));
                    for (const auto& log : d.logs)
                        ImGui::TextWrapped("[%s] %s", log.level.c_str(),
                            log.message.c_str());
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void SettingsWindow::DrawDebugPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##DebugPageInner", pageSize,
        ImGuiChildFlags_Borders |
            ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    ImGui::Text("%s", _L("app.settings.debug_page"));
    ImGui::Separator();
    ImGui::Spacing();
    if (DrawSettingCheckbox(_L("app.settings.demo_mode"),
            "##DemoModeEnabled", &generalSettings_.demoModeEnabled))
        generalSettingsDirty_ = true;
    ImGui::Indent();
    ImGui::TextDisabled("%s", _L("app.settings.demo_mode_hint"));
    ImGui::Unindent();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Checkbox(
            _L("app.settings.animation_diagnostics"),
            &animationDiagnosticsEnabled_))
    {
        if (animationDiagnosticsToggleCallback_)
        {
            animationDiagnosticsToggleCallback_(
                animationDiagnosticsEnabled_);
        }
    }
    ImGui::TextWrapped(
        "%s", _L("app.settings.animation_diagnostics_desc"));
    if (animationDiagnosticsEnabled_ &&
        animationDiagnosticsProvider_)
    {
        const std::wstring status =
            animationDiagnosticsProvider_();
        if (!status.empty())
        {
            const std::string utf8 = WideToUtf8(status);
            ImGui::TextWrapped("%s", utf8.c_str());
        }
    }
    ImGui::Spacing();
    if (DrawCollapsingHeaderWithHelp(
        _L("app.settings.crash_test"),
        _L("app.settings.crash_test_desc")))
    {
        ImGui::Spacing();
        if (BlueButton(_L("app.settings.trigger_crash")))
            TriggerCrashForTesting();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

/**
 * @brief 绘制"关于"页面。
 *
 * 显示应用简介、作者信息、社交主页链接（Bilibili / GitHub / 抖音 / 小红书）。
 * 版本号支持彩蛋点击 —— 连续点击 5 次可解锁调试页面（debugUnlocked_）。
 */
void SettingsWindow::PerformUpdateCheck()
{
    if (updateCheckRequestId_ != 0)
        return;

    updateCheckStatus_ = "checking";
    updateCheckStatusKey_.clear();
    updateCheckStatusArgument_.clear();
    updateAvailable_ = false;
    latestVersion_.clear();
    downloadUrl_.clear();

    if (snowdesktop::deployment::IsPackaged())
    {
        const std::wstring storeUri =
            snowdesktop::deployment::GetStoreProductPageUri();
        if (!storeUri.empty())
        {
            ShellExecuteW(nullptr, L"open", storeUri.c_str(),
                nullptr, nullptr, SW_SHOW);
        }
        updateCheckStatusKey_ =
            L10N_KEY("app.settings.store_managed_updates");
        updateCheckStatus_ =
            _L("app.settings.store_managed_updates");
        return;
    }

    if (!updateHttpService_)
        updateHttpService_ = std::make_unique<AsyncHttpService>();

    HttpRequestOptions options;
    options.widgetId = L"SnowDesktop.UpdateCheck";
    options.url =
        L"https://api.github.com/repos/FreeFallingSnow/"
        L"SnowDesktop_Release/releases/latest";
    options.headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    options.timeoutMs = 8000;
    options.allowedDomains = { "api.github.com" };
    updateCheckRequestId_ = updateHttpService_->Submit(std::move(options));
    if (updateCheckRequestId_ == 0)
    {
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_http_init_failed");
        updateCheckStatus_ = _L("app.settings.update_http_init_failed");
    }
}

void SettingsWindow::PollUpdateCheck()
{
    if (!updateHttpService_ || updateCheckRequestId_ == 0)
        return;

    for (HttpResponse& response : updateHttpService_->Drain())
    {
        if (response.id != updateCheckRequestId_)
            continue;

        updateCheckRequestId_ = 0;
        if (!response.error.empty() || response.status < 200 ||
            response.status >= 300)
        {
            if (response.error.find("WinHttpOpen") != std::string::npos)
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_http_init_failed");
            else if (response.error.find("WinHttpConnect") !=
                std::string::npos)
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_connect_failed");
            else if (response.error.find("Invalid URL") !=
                std::string::npos)
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_url_parse_failed");
            else
                updateCheckStatusKey_ =
                    L10N_KEY("app.settings.update_receive_failed");
            updateCheckStatus_ =
                Locale::Instance().Tr(updateCheckStatusKey_.c_str());
            return;
        }

        if (response.body.empty())
        {
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.update_empty_response");
            updateCheckStatus_ =
                _L("app.settings.update_empty_response");
            return;
        }

        auto extractJsonString = [](const std::string& json,
                                     const char* field) -> std::string {
            const std::string key = "\"" + std::string(field) + "\"";
            size_t pos = json.find(key);
            if (pos == std::string::npos) return {};
            pos += key.size();
            pos = json.find(':', pos);
            if (pos == std::string::npos) return {};
            ++pos;
            while (pos < json.size() &&
                (json[pos] == ' ' || json[pos] == '\t' ||
                 json[pos] == '\r' || json[pos] == '\n'))
                ++pos;
            if (pos >= json.size() || json[pos] != '"') return {};
            ++pos;
            size_t end = json.find('"', pos);
            if (end == std::string::npos) return {};
            return json.substr(pos, end - pos);
        };

        std::string tag =
            extractJsonString(response.body, "tag_name");
        if (tag.empty())
        {
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.update_parse_failed");
            updateCheckStatus_ =
                _L("app.settings.update_parse_failed");
            return;
        }

        if (tag[0] == 'v' || tag[0] == 'V')
            tag.erase(0, 1);

        const std::string htmlUrl =
            extractJsonString(response.body, "html_url");

        auto compareVersion = [](const std::string& a,
                                  const std::string& b) -> int {
            std::istringstream sa(a), sb(b);
            std::string pa, pb;
            for (int i = 0; i < 4; ++i)
            {
                int va = 0, vb = 0;
                if (std::getline(sa, pa, '.')) va = std::atoi(pa.c_str());
                if (std::getline(sb, pb, '.')) vb = std::atoi(pb.c_str());
                if (va != vb) return va < vb ? -1 : 1;
            }
            return 0;
        };

        latestVersion_ = tag;
        downloadUrl_ = htmlUrl;

        if (compareVersion(SNOWDESKTOP_VERSION, tag) >= 0)
        {
            updateAvailable_ = false;
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.already_latest");
            updateCheckStatusArgument_.clear();
            updateCheckStatus_ =
                _L("app.settings.already_latest");
        }
        else
        {
            updateAvailable_ = true;
            updateCheckStatusKey_ =
                L10N_KEY("app.settings.new_version");
            updateCheckStatusArgument_ = tag;
            updateCheckStatus_ =
                _LF("app.settings.new_version", tag);
        }
        return;
    }
}

void SettingsWindow::DrawAboutPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##AboutPageInner", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    ImGui::SeparatorText(_L("app.settings.about_snowdesktop"));
    ImGui::Spacing();

    ImGui::TextWrapped("%s", _L("app.settings.about_description"));

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.author"));
    ImGui::Spacing();
    ImGui::Text("    逍遥飘雪（郭云哲）"); // l10n-allow: author name is intentionally fixed
    ImGui::Spacing();

    ImGui::SeparatorText(_L("app.settings.copyright"));
    ImGui::Spacing();
    ImGui::TextWrapped("    %s", _L("app.settings.copyright_notice"));
    ImGui::TextWrapped("    %s", _L("app.settings.license_notice"));
    ImGui::Spacing();

    auto LinkButton = [](const char* label, const char* url) {
        ImGui::Text("    ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.30f, 0.60f, 0.95f, 1.00f), label);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s", url);
        }
        if (ImGui::IsItemClicked())
        {
            ShellExecuteW(nullptr, L"open", Utf8ToWide(url).c_str(), nullptr, nullptr, SW_SHOW);
        }
    };

    ImGui::SeparatorText(_L("app.settings.personal_homepages"));
    ImGui::Spacing();
    LinkButton("Bilibili", "https://space.bilibili.com/32837853");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton("GitHub", "https://github.com/FreeFallingSnow/");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton(_L("app.settings.douyin"), "https://www.douyin.com/user/MS4wLjABAAAA-O94bwF3BK2sj9JOwM2R2zRlTOiYf4BbaSyIF9DZPyM");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton(_L("app.settings.xiaohongshu"), "https://www.xiaohongshu.com/user/profile/6819eed7000000000403bf0e");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.project_url"));
    ImGui::Spacing();
    LinkButton("GitHub (Release)", "https://github.com/FreeFallingSnow/SnowDesktop_Release");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton("GitHub (Source)", "https://github.com/FreeFallingSnow/SnowDesktop");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.community"));
    ImGui::Spacing();
    LinkButton(_L("app.settings.join_qq"), "https://qm.qq.com/q/HyazkCIRig");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.version"));
    ImGui::TextDisabled("SnowDesktop v" SNOWDESKTOP_VERSION);
    if (ImGui::IsItemClicked())
    {
        if (!debugUnlocked_)
        {
            ++versionClickCount_;
            if (versionClickCount_ >= 5)
            {
                debugUnlocked_ = true;
                activePage_ = 7;
            }
        }
    }

    if (snowdesktop::deployment::IsPackaged())
    {
        ImGui::SameLine();
        if (!updateCheckStatus_.empty())
        {
            if (updateCheckStatus_ == "checking")
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", _L("app.settings.checking"));
            }
            else if (updateAvailable_)
            {
                ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.40f, 1.0f), "%s", updateCheckStatus_.c_str());
            }
            else
            {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", updateCheckStatus_.c_str());
            }
        }

        float updateButtonW = SettingButtonWidth(_L("app.settings.check_update")) + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - updateButtonW);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.45f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.55f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::BeginDisabled(updateCheckRequestId_ != 0);
        if (ImGui::Button(_L("app.settings.check_update"), ImVec2(updateButtonW, 0)))
        {
            PerformUpdateCheck();
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.third_party_libs"));
    ImGui::Spacing();

    LinkButton("Everything SDK", "https://www.voidtools.com/support/everything/sdk/");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (C) 2016 David Carpenter");

    LinkButton("Dear ImGui", "https://github.com/ocornut/imgui");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2014-2025 Omar Cornut");

    LinkButton("Lua", "https://www.lua.org/");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (C) 1994-2024 Lua.org, PUC-Rio");

    LinkButton("spdlog", "https://github.com/gabime/spdlog");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2016-present, Gabi Melman");

    LinkButton("pinyin-data", "https://github.com/mozillazg/pinyin-data");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2016 mozillazg");

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.reference_programs"));
    ImGui::Spacing();

    LinkButton("TranslucentTB (modified portions)",
        "https://github.com/TranslucentTB/TranslucentTB/tree/322e2b7395a51975150126276308b415970e080b");
    ImGui::SameLine();
    ImGui::TextDisabled("(GPL-3.0-only)");
    ImGui::TextDisabled("        Copyright (c) TranslucentTB contributors");
    ImGui::TextDisabled("        Modified for SnowDesktop from upstream commit 322e2b7");

    ImGui::EndChild();
}

// ════════════════════════════════════════════════════════════════
//  布局备份、完整数据备份与数据迁移
// ════════════════════════════════════════════════════════════════

/**
 * @brief 清除当前设置页的待写状态并重启，以应用已排队的数据替换。
 */
void SettingsWindow::RestartAfterDataReplacement(
    const char* successMessageKey)
{
    // Prevent the current settings frame from writing old values over the
    // replacement data while the application is restarting.
    personalizationDirty_ = false;
    personalizationPreviewDirty_ = false;
    personalizationSaveRequested_ = false;
    dockSettingsDirty_ = false;
    dockSettingsPreviewDirty_ = false;
    dockSettingsSaveRequested_ = false;
    navigationSettingsDirty_ = false;
    generalSettingsDirty_ = false;
    categorySettingsDirty_ = false;
    categorySettingsSaveRequested_ = false;

    MessageBoxW(hwnd_, _LW(successMessageKey),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONINFORMATION);
    if (restartCallback_)
        restartCallback_();
}

void SettingsWindow::CreateFullDataBackup()
{
    auto manager = MakeFullBackupManager();
    const auto result = manager.Create();
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.create_full_backup_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(hwnd_,
        _LW("app.settings.create_full_backup_success"),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::ExportFullDataBackup(
    const snowdesktop::backup::BackupInfo& backup)
{
    const COMDLG_FILTERSPEC filters[] = {
        {
            _LW("app.settings.snowbackup_file_type"),
            L"*.snowbackup",
        },
        {
            _LW("app.settings.zip_file_type"),
            L"*.zip",
        },
    };
    std::wstring defaultName = L"SnowDesktop-";
    defaultName += backup.id.empty() ? L"Backup" : backup.id;
    defaultName += L".snowbackup";
    const auto selected = SaveSettingsFile(hwnd_,
        _LW("app.settings.export_backup"),
        defaultName.c_str(), L"snowbackup",
        filters, static_cast<UINT>(std::size(filters)));
    if (!selected)
        return;

    const auto result =
        MakeFullBackupManager().Export(backup, *selected);
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.export_backup_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    MessageBoxW(hwnd_,
        _LW("app.settings.export_backup_success"),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::ImportFullDataBackup()
{
    const COMDLG_FILTERSPEC filters[] = {
        {
            _LW("app.settings.backup_archive_file_type"),
            L"*.snowbackup;*.zip",
        },
    };
    const auto selected = PickSettingsFile(hwnd_,
        _LW("app.settings.restore_from_backup_file"),
        filters, static_cast<UINT>(std::size(filters)));
    if (!selected)
        return;
    if (MessageBoxW(hwnd_,
            _LW("app.settings.restore_backup_file_confirm"),
            _LW("app.settings.full_data_backups"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    const auto result =
        MakeFullBackupManager().ImportAndQueue(*selected);
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.restore_backup_file_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    RestartAfterDataReplacement(
        L10N_KEY("app.settings.restore_backup_file_success"));
}

void SettingsWindow::RestoreFullDataBackup(
    const snowdesktop::backup::BackupInfo& backup)
{
    if (MessageBoxW(hwnd_,
            _LW("app.settings.restore_full_backup_confirm"),
            _LW("app.settings.full_data_backups"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }
    const auto result =
        MakeFullBackupManager().QueueRestore(backup);
    if (!result.ok)
    {
        std::wstring message =
            _LW("app.settings.restore_full_backup_failed");
        if (!result.error.empty())
            message += L"\n\n" + Utf8ToWide(result.error);
        MessageBoxW(hwnd_, message.c_str(),
            _LW("app.settings.full_data_backups"),
            MB_OK | MB_ICONERROR);
        return;
    }
    RestartAfterDataReplacement(
        L10N_KEY("app.settings.restore_full_backup_success"));
}

void SettingsWindow::DeleteFullDataBackup(
    const snowdesktop::backup::BackupInfo& backup)
{
    if (MessageBoxW(hwnd_,
            _LW("app.settings.delete_full_backup_confirm"),
            _LW("app.settings.full_data_backups"),
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }
    const auto result =
        MakeFullBackupManager().Delete(backup);
    if (result.ok)
        return;

    std::wstring message =
        _LW("app.settings.delete_full_backup_failed");
    if (!result.error.empty())
        message += L"\n\n" + Utf8ToWide(result.error);
    MessageBoxW(hwnd_, message.c_str(),
        _LW("app.settings.full_data_backups"),
        MB_OK | MB_ICONERROR);
}

/**
 * @brief 从用户选择的其他 SnowDesktop 目录迁入全部数据。
 *
 * 用户既可选择 SnowDesktop 程序目录，也可直接选择其中的 data 目录。迁移
 * 时先把来源完整复制到当前状态目录的暂存目录，发布待处理事务后重启。
 * 新进程会在打开任何数据文件前将当前 data 原子备份并换入暂存数据。
 */
void SettingsWindow::MigrateAllData()
{
    const wchar_t* title = _LW("app.settings.data_migration");

    ComPtr<IFileOpenDialog> dialog;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result))
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options)))
    {
        dialog->SetOptions(options | FOS_PICKFOLDERS |
            FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(_LW("app.settings.select_migration_data"));

    result = dialog->Show(hwnd_);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (FAILED(result))
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    ComPtr<IShellItem> selectedItem;
    if (FAILED(dialog->GetResult(&selectedItem)) || !selectedItem)
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    PWSTR selectedPath = nullptr;
    if (FAILED(selectedItem->GetDisplayName(
            SIGDN_FILESYSPATH, &selectedPath)) || !selectedPath)
    {
        if (selectedPath)
            CoTaskMemFree(selectedPath);
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_failed"),
            title, MB_OK | MB_ICONERROR);
        return;
    }

    std::filesystem::path selectedDirectory(selectedPath);
    CoTaskMemFree(selectedPath);

    std::filesystem::path sourceData = selectedDirectory;
    const std::filesystem::path nestedData =
        selectedDirectory / L"data";
    if (LooksLikeSnowDesktopDataDirectory(nestedData))
        sourceData = nestedData;

    const std::filesystem::path targetData(GetDataDirectoryPath());
    if (!LooksLikeSnowDesktopDataDirectory(sourceData) ||
        PathsOverlap(sourceData, targetData))
    {
        MessageBoxW(hwnd_, _LW("app.settings.migrate_data_invalid"),
            title, MB_OK | MB_ICONWARNING);
        return;
    }

    if (MessageBoxW(hwnd_, _LW("app.settings.migrate_data_confirm"),
            title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        return;
    }

    std::filesystem::path stateRoot =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (stateRoot.empty())
        stateRoot = targetData.parent_path();

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t token[96]{};
    swprintf_s(token, L"%04u%02u%02u-%02u%02u%02u-%lu-%llu",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId(),
        static_cast<unsigned long long>(GetTickCount64()));

    const std::filesystem::path migrationRoot =
        stateRoot / L"TempState" / L"PortableMigration";
    const std::filesystem::path stagingData =
        migrationRoot / (std::wstring(L"staging-") + token);

    const auto copyResult =
        snowdesktop::migration::CopyDataTree(sourceData, stagingData);
    bool staged = copyResult.ok;
    std::string stageError = copyResult.error;
    if (!staged ||
        !LooksLikeSnowDesktopDataDirectory(stagingData))
    {
        if (staged && stageError.empty())
            stageError = "staged data validation failed";
        OutputDebugStringA(("SnowDesktop: data migration staging failed: " +
            stageError + "\n").c_str());
        std::error_code cleanupError;
        std::filesystem::remove_all(stagingData, cleanupError);
        std::wstring message =
            _LW("app.settings.migrate_data_failed");
        if (!stageError.empty())
            message += L"\n\n" + Utf8ToWide(stageError);
        MessageBoxW(hwnd_, message.c_str(), title,
            MB_OK | MB_ICONERROR);
        return;
    }

    std::string queueError;
    if (!snowdesktop::migration::Queue(
            stateRoot, token, queueError))
    {
        OutputDebugStringA(("SnowDesktop: cannot queue data "
            "migration: " + queueError + "\n").c_str());
        std::error_code cleanupError;
        std::filesystem::remove_all(stagingData, cleanupError);
        std::wstring message =
            _LW("app.settings.migrate_data_failed");
        message += L"\n\n" + Utf8ToWide(queueError);
        MessageBoxW(hwnd_, message.c_str(), title,
            MB_OK | MB_ICONERROR);
        return;
    }

    RestartAfterDataReplacement(
        L10N_KEY("app.settings.migrate_data_success"));
}

/**
 * @brief 获取备份文件存储目录路径。
 *
 * 目录位于当前部署数据目录下的 backups 子文件夹。
 * @return 备份目录的完整宽字符串路径
 */
std::wstring SettingsWindow::GetBackupDir() const
{
    return GetDataSubdirectoryPath(L"backups");
}

/**
 * @brief 列举所有已有备份。
 *
 * 扫描备份目录下所有 *.json 文件，解析文件名和最后写入时间，
 * 组装为 LayoutBackup 条目并按照时间倒序（最新在前）排序。
 * @return 备份条目列表，可能为空
 */
std::vector<LayoutBackup> SettingsWindow::ListBackups() const
{
    std::vector<LayoutBackup> result;
    std::wstring dir = GetBackupDir();
    std::wstring search = dir + L"\\*.json";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring filename = fd.cFileName;
        // Skip storage companion files
        if (filename.size() > 13 && filename.substr(filename.size() - 13) == L".storage.json")
            continue;

        LayoutBackup b;
        b.filename = filename;
        b.timestamp = fd.ftLastWriteTime;

        // Parse display name from filename: remove .json and format timestamp
        std::wstring name = filename;
        if (name.size() > 5 && name.substr(name.size() - 5) == L".json")
            name = name.substr(0, name.size() - 5);

        SYSTEMTIME st;
        FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
        wchar_t timeStr[64]{};
        swprintf_s(timeStr, L"%04d-%02d-%02d %02d:%02d",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

        b.displayName = name + L"  [" + timeStr + L"]";
        result.push_back(b);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    // Sort by timestamp descending (newest first)
    std::sort(result.begin(), result.end(), [](const LayoutBackup& a, const LayoutBackup& b) {
        return CompareFileTime(&a.timestamp, &b.timestamp) > 0;
    });

    return result;
}

/**
 * @brief 保存当前布局文件到备份目录。
 *
 * 将 data\SnowDesktop.layout.json 复制到 data\backups\ 下，
 * 备份文件名中不允许出现 : / \\ 字符（替换为 _），
 * 同名文件存在时自动在末尾追加递增序号。
 * @param name 备份名称
 * @return true 复制成功
 */
bool SettingsWindow::SaveBackup(const std::wstring& name)
{
    std::wstring backupDir = GetBackupDir();
    CreateDirectoryW(backupDir.c_str(), nullptr);

    std::wstring layoutPath = GetDataFilePath(L"SnowDesktop.layout.json");
    std::wstring storagePath = GetDataFilePath(L"SnowDesktop.storage.json");

    // Sanitize: remove colons for filename safety
    std::wstring safeName = name;
    for (auto& c : safeName) { if (c == L':' || c == L'\\' || c == L'/') c = L'_'; }

    std::wstring backupLayout = backupDir + L"\\" + safeName + L".json";
    std::wstring backupStorage = backupDir + L"\\" + safeName + L".storage.json";

    // Find existing file with same name, increment count
    int counter = 1;
    while (GetFileAttributesW(backupLayout.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        backupLayout = backupDir + L"\\" + safeName + L"(" + std::to_wstring(counter) + L").json";
        backupStorage = backupDir + L"\\" + safeName + L"(" + std::to_wstring(counter) + L").storage.json";
        ++counter;
    }

    bool ok = CopyFileW(layoutPath.c_str(), backupLayout.c_str(), FALSE) != FALSE;
    if (GetFileAttributesW(storagePath.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(storagePath.c_str(), backupStorage.c_str(), FALSE);
    return ok;
}

/**
 * @brief 基于当前系统时间生成备份文件名（含年月日时分秒）。
 * @return 格式为 "YYYY-MM-DD hh-mm-ss" 的宽字符串
 */
std::wstring SettingsWindow::MakeBackupTimestampName() const
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[64]{};
    swprintf_s(name, L"%04d-%02d-%02d %02d-%02d-%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return name;
}

/**
 * @brief 从备份文件恢复布局。
 *
 * 恢复前自动调用 SaveBackup() 生成一份"恢复前备份"快照。
 * @param filename 备份文件名（仅文件名，不含路径）
 * @return true 复制成功
 */
bool SettingsWindow::RestoreBackup(const std::wstring& filename)
{
    std::wstring layoutPath = GetDataFilePath(L"SnowDesktop.layout.json");
    std::wstring storagePath = GetDataFilePath(L"SnowDesktop.storage.json");

    std::wstring backupPath = GetBackupDir() + L"\\" + filename;

    // Derive storage backup filename: replace .json with .storage.json
    std::wstring storageFilename = filename;
    if (storageFilename.size() > 5 && storageFilename.substr(storageFilename.size() - 5) == L".json")
        storageFilename = storageFilename.substr(0, storageFilename.size() - 5) + L".storage.json";
    std::wstring storageBackupPath = GetBackupDir() + L"\\" + storageFilename;

    // First save current layout before restoring.
    SaveBackup(MakeBackupTimestampName() +
        _LW("app.settings.backup_before_restore_suffix"));

    bool ok = CopyFileW(backupPath.c_str(), layoutPath.c_str(), FALSE) != FALSE;
    if (GetFileAttributesW(storageBackupPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(storageBackupPath.c_str(), storagePath.c_str(), FALSE);
    return ok;
}

/**
 * @brief 删除指定的备份文件。
 * @param filename 要删除的备份文件名
 * @return true 删除成功
 */
bool SettingsWindow::DeleteBackup(const std::wstring& filename)
{
    std::wstring backupPath = GetBackupDir() + L"\\" + filename;

    std::wstring storageFilename = filename;
    if (storageFilename.size() > 5 && storageFilename.substr(storageFilename.size() - 5) == L".json")
        storageFilename = storageFilename.substr(0, storageFilename.size() - 5) + L".storage.json";
    std::wstring storageBackupPath = GetBackupDir() + L"\\" + storageFilename;
    if (GetFileAttributesW(storageBackupPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        DeleteFileW(storageBackupPath.c_str());

    return DeleteFileW(backupPath.c_str()) != FALSE;
}

// ════════════════════════════════════════════════════════════════
//  交换链：创建与清理
// ════════════════════════════════════════════════════════════════

/**
 * @brief 创建 DirectX 交换链及渲染目标视图。
 *
 * 根据窗口当前客户区尺寸调整已有交换链，
 * 仅在尚未创建或调整失败时重新创建，并同步 ImGui DisplaySize。
 * @return true 创建成功
 */
bool SettingsWindow::CreateSwapChain()
{
    RECT cr;
    if (!hwnd_ || !GetClientRect(hwnd_, &cr))
    {
        LogSettingsWindowFailure(
            L"GetClientRect",
            LastSettingsWindowError());
        return false;
    }
    windowWidth_ = (cr.right - cr.left > 1) ? (cr.right - cr.left) : 1;
    windowHeight_ = (cr.bottom - cr.top > 1) ? (cr.bottom - cr.top) : 1;

    if (!device_)
    {
        LogSettingsWindowFailure(L"CreateSwapChain device", E_POINTER);
        return false;
    }
    if (!context_)
        device_->GetImmediateContext(&context_);
    if (!context_)
    {
        LogSettingsWindowFailure(
            L"CreateSwapChain context", E_FAIL);
        return false;
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();

    if (swapChain_)
    {
        const HRESULT resizeResult = swapChain_->ResizeBuffers(0,
            static_cast<UINT>(windowWidth_), static_cast<UINT>(windowHeight_),
            DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeResult))
        {
            LogSettingsWindowFailure(
                L"ResizeBuffers", resizeResult);
            swapChain_.Reset();
        }
    }

    if (!swapChain_)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT result = device_.As(&dxgiDevice);
        if (FAILED(result))
        {
            LogSettingsWindowFailure(L"Query IDXGIDevice", result);
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        result = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(result))
        {
            LogSettingsWindowFailure(L"GetAdapter", result);
            return false;
        }
        ComPtr<IDXGIFactory2> factory;
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
        if (FAILED(result))
        {
            LogSettingsWindowFailure(L"Get DXGI factory", result);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = static_cast<UINT>(windowWidth_);
        desc.Height = static_cast<UINT>(windowHeight_);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        result = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_,
            &desc, nullptr, nullptr, &swapChain_);
        if (FAILED(result))
        {
            LogSettingsWindowFailure(
                L"CreateSwapChainForHwnd", result);
            return false;
        }
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT result = swapChain_->GetBuffer(
        0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(result))
    {
        LogSettingsWindowFailure(L"Get swap-chain buffer", result);
        return false;
    }
    result = device_->CreateRenderTargetView(
        backBuffer.Get(), nullptr, &rtv_);
    if (FAILED(result))
    {
        LogSettingsWindowFailure(L"CreateRenderTargetView", result);
        return false;
    }

    if (imguiContext_)
    {
        ImGui::SetCurrentContext(imguiContext_);
        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(windowWidth_), static_cast<float>(windowHeight_));
    }
    return true;
}

/**
 * @brief 释放交换链和渲染目标视图的 COM 资源。
 */
void SettingsWindow::CleanupSwapChain()
{
    rtv_.Reset();
    swapChain_.Reset();
}

// ════════════════════════════════════════════════════════════════
//  字体：加载系统字体
// ════════════════════════════════════════════════════════════════

/**
 * @brief 加载系统字体用于 ImGui 渲染。
 *
 * 从 C:\\Windows\\Fonts\\msyh.ttc 加载微软雅黑字体，
 * 字体大小根据 DPI 缩放系数调整，
 * 并包含简体中文常用字形和韩文字形范围。
 */
void SettingsWindow::SetupFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    std::string fontPath = "C:\\Windows\\Fonts\\msyh.ttc";
    if (FILE* f = fopen(fontPath.c_str(), "rb"))
    {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f * dpiScale_, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    else
    {
        io.Fonts->AddFontDefault();
    }

    const std::string koreanFontPath =
        "C:\\Windows\\Fonts\\malgun.ttf";
    if (FILE* f = fopen(koreanFontPath.c_str(), "rb"))
    {
        fclose(f);
        ImFontConfig koreanConfig;
        koreanConfig.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(koreanFontPath.c_str(),
            16.0f * dpiScale_, &koreanConfig,
            io.Fonts->GetGlyphRangesKorean());
    }

    HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_FA_FONT), RT_RCDATA);
    HGLOBAL resourceHandle = resource ? LoadResource(instance_, resource) : nullptr;
    void* fontData = resourceHandle ? LockResource(resourceHandle) : nullptr;
    DWORD fontSize = resource ? SizeofResource(instance_, resource) : 0;
    if (fontData && fontSize > 0)
    {
        static const ImWchar iconRanges[] = { 0xE000, 0xF8FF, 0 };
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        strcpy_s(config.Name, "Font Awesome 6 Free Solid");
        faDebugFont_ = io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(fontSize),
            18.0f * dpiScale_, &config, iconRanges);
    }

    resource = FindResourceW(instance_,
        MAKEINTRESOURCEW(IDR_FLUENT_REGULAR_FONT), RT_RCDATA);
    resourceHandle = resource ? LoadResource(instance_, resource) : nullptr;
    fontData = resourceHandle ? LockResource(resourceHandle) : nullptr;
    fontSize = resource ? SizeofResource(instance_, resource) : 0;
    if (fontData && fontSize > 0)
    {
        static const ImWchar iconRanges[] = {
            0xE000, 0xF8FF, 0xF0000, 0xF0CCE, 0,
        };
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        strcpy_s(config.Name, "FluentSystemIcons-Regular");
        fluentDebugFont_ = io.Fonts->AddFontFromMemoryTTF(fontData,
            static_cast<int>(fontSize), 18.0f * dpiScale_,
            &config, iconRanges);
    }
}

// ════════════════════════════════════════════════════════════════
//  开机自启：查询与设置（通过 Windows 注册表 Run 键）
// ════════════════════════════════════════════════════════════════

/**
 * @brief 检查当前是否已启用开机自启。
 *
 * 读取 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run
 * 下 "SnowDesktop" 条目是否存在。
 * @return true 已启用开机自启
 */
bool SettingsWindow::IsAutoStartEnabled() const
{
    if (snowdesktop::deployment::IsPackaged())
    {
        if (!packagedAutoStartStateKnown_)
        {
            const auto state =
                snowdesktop::deployment::GetPackagedAutoStartState();
            packagedAutoStartEnabled_ =
                snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                    state);
            packagedAutoStartStateKnown_ = true;
        }
        return packagedAutoStartEnabled_;
    }

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    wchar_t value[256]{};
    DWORD size = sizeof(value);
    DWORD type = REG_SZ;
    LONG result = RegQueryValueExW(key, L"SnowDesktop", nullptr, &type,
        reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

/**
 * @brief 设置或取消开机自启。
 *
 * 在 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run
 * 下创建或删除 "SnowDesktop" 条目。
 * @param enable true 添加注册表项启用自启，false 删除
 */
void SettingsWindow::SetAutoStart(bool enable) const
{
    if (snowdesktop::deployment::IsPackaged())
    {
        using snowdesktop::deployment::PackagedAutoStartState;
        const std::wstring otherAutoStart =
            ReadPortableAutoStartCommand();
        if (enable && !otherAutoStart.empty())
        {
            const std::wstring confirmation = _LFW(
                "app.settings.auto_start_portable_conflict",
                otherAutoStart);
            if (MessageBoxW(hwnd_, confirmation.c_str(),
                    _LW("app.settings.auto_start"),
                    MB_YESNO | MB_ICONWARNING) == IDYES)
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            return;
        }

        const PackagedAutoStartState state =
            snowdesktop::deployment::SetPackagedAutoStartEnabled(enable);
        if (state != PackagedAutoStartState::Unavailable)
        {
            packagedAutoStartStateKnown_ = true;
            packagedAutoStartEnabled_ =
                snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                    state);
        }

        if (enable && !packagedAutoStartEnabled_)
        {
            if (state == PackagedAutoStartState::DisabledByUser)
            {
                if (MessageBoxW(hwnd_,
                        _LW("app.settings.auto_start_manual_required"),
                        _LW("app.settings.auto_start"),
                        MB_YESNO | MB_ICONINFORMATION) == IDYES)
                {
                    ShellExecuteW(nullptr, L"open",
                        L"ms-settings:startupapps",
                        nullptr, nullptr, SW_SHOW);
                }
            }
            else if (state == PackagedAutoStartState::DisabledByPolicy)
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_policy_disabled"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONWARNING);
            }
            else
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_enable_failed"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONERROR);
            }
        }
        else if (!enable && packagedAutoStartEnabled_)
        {
            if (state == PackagedAutoStartState::EnabledByPolicy)
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_policy_enabled"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONWARNING);
            }
            else
            {
                MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_disable_failed"),
                    _LW("app.settings.auto_start"),
                    MB_OK | MB_ICONERROR);
            }
        }
        return;
    }

    if (enable)
    {
        const auto installedState =
            snowdesktop::deployment::GetInstalledPackagedAutoStartState();
        if (snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
                installedState))
        {
            if (MessageBoxW(hwnd_,
                    _LW("app.settings.auto_start_installed_conflict"),
                    _LW("app.settings.auto_start"),
                    MB_YESNO | MB_ICONWARNING) == IDYES)
            {
                ShellExecuteW(nullptr, L"open",
                    L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOW);
            }
            return;
        }
    }

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        kAutoStartRunSubKey,
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (enable)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        RegSetValueExW(key, kAutoStartRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path),
            static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, kAutoStartRunValue);
    }
    RegCloseKey(key);
}

// ════════════════════════════════════════════════════════════════
//  窗口过程：消息处理
// ════════════════════════════════════════════════════════════════

/**
 * @brief 静态窗口过程函数，处理设置窗口的 Windows 消息。
 *
 * 处理的消息包括：
 * - ESC 键按下时请求关闭窗口
 * - 将输入事件转发给 ImGui 的 Win32 处理器
 * - WM_MOUSEACTIVATE：确保鼠标激活
 * - WM_SIZE：窗口尺寸变化时重建交换链并重绘
 * - WM_DPICHANGED：DPI 变化时更新缩放系数和建议尺寸
 * - WM_GETMINMAXINFO：设置最小窗口尺寸（500x350）
 * - WM_CLOSE：请求关闭而非直接销毁
 * @param hwnd   窗口句柄
 * @param msg    消息 ID
 * @param wParam 消息参数 WPARAM
 * @param lParam 消息参数 LPARAM
 * @return 消息处理结果（0 表示已处理，否则返回 DefWindowProcW）
 */
LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // 只有设置窗口自身的消息才会请求新帧。桌面窗口的鼠标、拖拽与
    // 定时器消息不再连带触发 ImGui 重建和交换链 Present。
    if (g_settingsWindow != nullptr && msg != WM_TIMER)
        g_settingsWindow->renderRequested_ = true;

    if (g_settingsWindow != nullptr &&
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        wParam == VK_ESCAPE)
    {
        if (g_settingsWindow->IsHotkeyCaptureActive())
        {
            g_settingsWindow->CancelHotkeyCapture();
            return 0;
        }
        g_settingsWindow->RequestClose();
        return 0;
    }

    if (g_settingsWindow != nullptr &&
        g_settingsWindow->IsHotkeyCaptureActive() &&
        (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN ||
            msg == WM_KEYUP || msg == WM_SYSKEYUP))
    {
        g_settingsWindow->HandleHotkeyCaptureKeyMessage(
            msg, wParam);
    }

    if (g_settingsWindow != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_TIMER:
        if (g_settingsWindow != nullptr &&
            wParam == kSettingsHotkeyCaptureTimerId)
        {
            g_settingsWindow->UpdateHotkeyCapture();
            g_settingsWindow->renderRequested_ = true;
            return 0;
        }
        if (g_settingsWindow != nullptr && wParam == kSettingsRefreshTimerId)
        {
            // 低频更新后端状态、调试采样和文本光标等动态内容。
            g_settingsWindow->renderRequested_ = true;
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (g_settingsWindow != nullptr &&
            g_settingsWindow->IsHotkeyCaptureActive())
        {
            g_settingsWindow->UpdateHotkeyCapture();
            if (g_settingsWindow->IsHotkeyCaptureActive() &&
                !g_settingsWindow->hotkeyCapturePrimarySeen_ &&
                !g_settingsWindow->hotkeyCaptureClearPending_)
            {
                g_settingsWindow->CancelHotkeyCapture();
            }
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SIZE:
        if (g_settingsWindow != nullptr && wParam != SIZE_MINIMIZED)
        {
            if (g_settingsWindow->CreateSwapChain())
            {
                g_settingsWindow->renderRequested_ = true;
                // 交互式拖动边框时，系统的尺寸调整循环会暂停外层
                // GetMessage 循环，因此需要在 WM_SIZE 内立即 Present。
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
                    g_settingsWindow->Render();
            }
        }
        return 0;
    case WM_DPICHANGED:
    {
        if (g_settingsWindow != nullptr)
        {
            g_settingsWindow->dpiScale_ = static_cast<float>(LOWORD(wParam)) / 96.0f;
        }
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 500;
        mmi->ptMinTrackSize.y = 350;
        return 0;
    }
    case WM_CLOSE:
        g_settingsWindow->RequestClose();
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
