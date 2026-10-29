// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "bridge_json.h"
#include "component_workshop_publish.h"
#include "manager_frame_scheduler.h"
#include "manager_localization.h"
#include "manager_publish_task.h"
#include "manager_prepared_preview.h"
#include "manager_package_export.h"
#include "manager_workspace_state.h"
#include "manager_workshop_item.h"
#include "package_tool.h"
#include "preview_cache.h"
#include "steam_app_identity.h"
#include "steam_workshop_core.h"
#include "workshop_localization.h"
#include "workshop_project.h"
#include "workshop_staging.h"
#include "resources/resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace snowdesktop::steam_bridge;

extern LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace
{
ComPtr<ID3D11Device> gDevice;
ComPtr<ID3D11DeviceContext> gContext;
ComPtr<IDXGISwapChain> gSwapChain;
ComPtr<ID3D11RenderTargetView> gRenderTarget;
float gDpiScale = 1.0f;
ManagerFrameScheduler gFrameScheduler;
std::atomic<HWND> gManagerWindow = nullptr;
std::atomic<DWORD> gManagerThreadId = 0;

constexpr UINT kRequestFrameMessage = WM_APP + 0x534;

void WakeManagerForPendingFrame() noexcept
{
    const HWND window = gManagerWindow.load(std::memory_order_acquire);
    if (!window || !gFrameScheduler.IsFrameRequested() ||
        !gFrameScheduler.TryQueueWake())
        return;
    if (!PostMessageW(window, kRequestFrameMessage, 0, 0))
        gFrameScheduler.AcknowledgeWake();
}

void RequestManagerFrame() noexcept
{
    const HWND window = gManagerWindow.load(std::memory_order_acquire);
    if (!window) return;
    gFrameScheduler.RequestFrame();
    if (GetCurrentThreadId() !=
        gManagerThreadId.load(std::memory_order_acquire))
        WakeManagerForPendingFrame();
}

constexpr float kSidebarWidthDip = 224.0f;
constexpr float kSettingControlWidthDip = 320.0f;

void SetupLightTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.FrameRounding = 4.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.ScrollbarSize = 10.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.78f, 0.78f, 0.82f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.88f, 0.88f, 0.91f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.82f, 0.82f, 0.87f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
    colors[ImGuiCol_Text] = ImVec4(0.10f, 0.10f, 0.14f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.25f, 0.55f, 0.90f, 0.35f);
    colors[ImGuiCol_InputTextCursor] = ImVec4(0.08f, 0.08f, 0.12f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.84f, 0.84f, 0.88f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.78f, 0.78f, 0.83f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.50f, 0.92f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.56f, 0.96f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.42f, 0.84f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.18f, 0.50f, 0.92f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.55f, 0.92f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.60f, 0.96f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.84f, 0.84f, 0.88f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.78f, 0.78f, 0.83f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.88f, 0.88f, 0.91f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.78f, 0.78f, 0.83f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(1.00f, 1.00f, 1.00f, 0.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.80f, 0.80f, 0.84f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.70f, 0.70f, 0.75f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.60f, 0.60f, 0.65f, 1.00f);
}

bool BlueButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    return clicked;
}

bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_Text]);
    ImGui::PushStyleColor(ImGuiCol_Button, style.Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        style.Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        style.Colors[ImGuiCol_FrameBgActive]);
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool SidebarButton(const char* label, bool primary = false)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float wrapWidth = std::max(1.0f, width - style.FramePadding.x * 2.0f);
    const std::string_view text(label);
    const auto suffix = text.find("##");
    const char* end = suffix == std::string_view::npos ? label + text.size() : label + suffix;
    const ImVec2 textSize = ImGui::CalcTextSize(label, end, false, wrapWidth);
    ImGui::PushID(label);
    const ImVec2 size(width, textSize.y + style.FramePadding.y * 2.0f);
    const bool clicked = primary ? BlueButton("##sidebar-action", size) :
        SecondaryButton("##sidebar-action", size);
    const ImVec2 origin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(nullptr, 0.0f,
        ImVec2(origin.x + std::max(style.FramePadding.x, (width - textSize.x) * 0.5f),
            origin.y + style.FramePadding.y),
        ImGui::GetColorU32(primary ? ImVec4(1, 1, 1, 1) : style.Colors[ImGuiCol_Text]),
        label, end, wrapWidth);
    ImGui::PopID();
    return clicked;
}

float BeginSettingRow(const char* label, float controlWidth)
{
    const float rowStart = ImGui::GetCursorPosX();
    const float rowRight = rowStart + ImGui::GetContentRegionAvail().x;
    const float controlX = std::max(rowStart,
        rowRight - std::max(1.0f, controlWidth));
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(std::max(rowStart + 1.0f,
        controlX - ImGui::GetStyle().ItemSpacing.x));
    ImGui::TextWrapped("%s", label);
    ImGui::PopTextWrapPos();
    ImGui::SameLine(controlX);
    return controlX;
}

float SettingControlWidth()
{
    return std::min(kSettingControlWidthDip * gDpiScale,
        ImGui::GetContentRegionAvail().x);
}

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
}

void ContinueButtonRow(const char* label)
{
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x +
            ButtonWidth(label) <= ImGui::GetCursorScreenPos().x +
                ImGui::GetContentRegionAvail().x)
        ImGui::SameLine();
}

bool TwoLineProjectSelectable(const std::string& label, bool selected,
    const std::string& metadata, const std::string& version = {})
{
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float lineHeight = ImGui::GetTextLineHeight();
    const float gap = 3.0f * gDpiScale;
    const float height = lineHeight * 2.0f + gap;
    const bool pressed = ImGui::Selectable(label.c_str(), selected, 0,
        ImVec2(width, height));
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float versionWidth = version.empty() ? 0.0f :
        ImGui::CalcTextSize(version.c_str()).x;
    const float metadataRight = origin.x + width - versionWidth -
        (version.empty() ? 0.0f : ImGui::GetStyle().ItemSpacing.x);
    draw->PushClipRect(origin, ImVec2(std::max(origin.x, metadataRight),
        origin.y + height), true);
    draw->AddText(ImVec2(origin.x, origin.y + lineHeight + gap), color,
        metadata.c_str());
    draw->PopClipRect();
    if (!version.empty())
    {
        draw->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
        draw->AddText(ImVec2(origin.x + width - versionWidth,
            origin.y + lineHeight + gap), color, version.c_str());
        draw->PopClipRect();
    }
    return pressed;
}

bool BeginProjectTab(const char* label, bool select)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(14.0f * gDpiScale, 8.0f * gDpiScale));
    const bool active = ImGui::BeginTabItem(label, nullptr,
        select ? ImGuiTabItemFlags_SetSelected : 0);
    ImGui::PopStyleVar();
    return active;
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr,
        nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr,
        nullptr) != length)
        return {};
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length) != length)
        return {};
    return result;
}

std::string NowIso8601()
{
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string FormatLocalTime(std::uint32_t timestamp)
{
    if (timestamp == 0) return "-";
    const std::time_t value = static_cast<std::time_t>(timestamp);
    std::tm local{};
    if (localtime_s(&local, &value) != 0) return "-";
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

bool OpenUrl(std::string_view url)
{
    const auto wide = Utf8ToWide(url);
    if (wide.empty()) return false;
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", wide.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(opened) > 32;
}

void OpenSteamUrlWithWebFallback(
    std::string_view steamUrl, std::string_view webUrl)
{
    if (!OpenUrl(steamUrl)) OpenUrl(webUrl);
}

void OpenDirectory(const std::filesystem::path& path)
{
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
        SW_SHOWNORMAL);
}

bool OpenFileLocation(const std::filesystem::path& path)
{
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)))
    {
        PIDLIST_ABSOLUTE parent = ILCloneFull(item);
        if (parent && ILRemoveLastID(parent))
        {
            PCUITEMID_CHILD children[] = { ILFindLastID(item) };
            const HRESULT result = SHOpenFolderAndSelectItems(parent, 1, children, 0);
            ILFree(parent);
            CoTaskMemFree(item);
            if (SUCCEEDED(result)) return true;
        }
        else
        {
            ILFree(parent);
            CoTaskMemFree(item);
        }
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.parent_path().c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::optional<std::filesystem::path> PickPath(HWND owner, bool folder)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return std::nullopt;
    if (folder)
    {
        FILEOPENDIALOGOPTIONS options{};
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST);
    }
    else
    {
        const COMDLG_FILTERSPEC filters[] = {
            { L"Preview images", L"*.png;*.jpg;*.jpeg;*.gif" },
            { L"All files", L"*.*" },
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
            FOS_PATHMUSTEXIST);
    }
    if (FAILED(dialog->Show(owner))) return std::nullopt;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return std::nullopt;
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::vector<std::string> SplitTags(std::string_view text)
{
    std::vector<std::string> tags;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t end = text.find(',', begin);
        std::string tag(text.substr(begin,
            end == std::string_view::npos ? text.size() - begin : end - begin));
        while (!tag.empty() && std::isspace(
            static_cast<unsigned char>(tag.front()))) tag.erase(tag.begin());
        while (!tag.empty() && std::isspace(
            static_cast<unsigned char>(tag.back()))) tag.pop_back();
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end())
            tags.push_back(std::move(tag));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return tags;
}

std::string JoinTags(const std::vector<std::string>& tags)
{
    std::string output;
    for (const auto& tag : tags)
    {
        if (!output.empty()) output += ", ";
        output += tag;
    }
    return output;
}

std::optional<std::uint64_t> ParseItemId(std::string_view text)
{
    try
    {
        std::size_t consumed = 0;
        const auto value = std::stoull(std::string(text), &consumed);
        if (value == 0 || consumed != text.size()) return std::nullopt;
        return value;
    }
    catch (...) { return std::nullopt; }
}

std::uint64_t StableKey(std::string_view value)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return (hash == 0 ? 1 : hash) | (1ull << 63);
}

std::string SystemLanguage()
{
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    return GetUserDefaultLocaleName(localeName,
        static_cast<int>(std::size(localeName)))
        ? WideToUtf8(localeName) : std::string("en-US");
}

std::optional<std::string> ReadMainLanguageSetting(
    const std::filesystem::path& settingsFile)
{
    std::ifstream input(settingsFile, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    JsonValue root;
    std::string error;
    if (!input || !ParseJson(text, root, error) || !root.IsObject())
        return std::nullopt;
    const auto configured = JsonString(root, "language");
    if (!configured) return std::nullopt;
    return configured->empty() || *configured == "system"
        ? std::optional<std::string>(SystemLanguage()) : configured;
}

struct ManagerArguments
{
    std::filesystem::path dataDirectory;
    std::filesystem::path developmentRoot;
    std::filesystem::path projectDirectory;
    std::filesystem::path settingsFile;
    std::string language;
};

struct PreparedManagerPublish
{
    std::string localId;
    WorkshopProject snapshot;
    WidgetInspection inspection;
    PackagedWidget artifact;
    ComponentPublishPlan plan;
    // Released before artifact so its directory is empty when the package owner cleans up.
    std::shared_ptr<ManagerPreparedPreview> frozenPreview;
};

class WorkshopManagerApp
{
public:
    explicit WorkshopManagerApp(ManagerArguments arguments,
        std::filesystem::path languageDirectory)
        : managerRoot_(WorkshopManagerDataRoot(arguments.dataDirectory)),
          developmentRoot_(std::move(arguments.developmentRoot)),
          projectDirectory_(std::move(arguments.projectDirectory)),
          settingsFile_(std::move(arguments.settingsFile)),
          currentLanguage_(std::move(arguments.language)),
          store_(managerRoot_),
          packageTool_({}, managerRoot_ / L"staging" / L"packages"),
          previewCache_(managerRoot_ / L"preview-cache",
              [] { RequestManagerFrame(); }),
          steam_(managerRoot_ / L"staging" / L"uploads")
    {
        std::string error;
        if (!localization_.Load(languageDirectory, currentLanguage_, error))
            SetMessage(false, error);
        error.clear();
        if (!MigrateWorkshopManagerDataOnce(managerRoot_, error))
            SetMessage(false, error);
        error.clear();
        std::error_code directoryError;
        const auto stagingRoot = managerRoot_ / L"staging";
        if (!PrepareWorkshopStaging(stagingRoot, error)) SetMessage(false, error);
        directoryError.clear();
        if (!developmentRoot_.empty())
            std::filesystem::create_directories(
                developmentRoot_, directoryError);
        const bool loaded = store_.Load(error);
        if (!loaded) SetMessage(false, error);
        if (loaded && !developmentRoot_.empty())
        {
            std::size_t added = 0;
            if (store_.Discover(developmentRoot_, added, error) && added > 0)
            {
                if (!store_.Save(error)) SetMessage(false, error);
                else SetMessage(true, T("已发现 %zu 个开发组件",
                    "Discovered %zu development components", added));
            }
            else if (!error.empty()) SetMessage(false, error);
        }
        if (loaded && !projectDirectory_.empty())
        {
            WorkshopProject* focusedProject = nullptr;
            error.clear();
            if (store_.AddDirectory(
                    projectDirectory_, focusedProject, error) &&
                focusedProject)
            {
                selectedLocalId_ = focusedProject->localId;
                if (!store_.Save(error)) SetMessage(false, error);
            }
            else if (!error.empty()) SetMessage(false, error);
        }
        if (selectedLocalId_.empty() && !store_.Projects().empty())
            selectedLocalId_ = store_.Projects().front().localId;
        worker_ = std::jthread([this](std::stop_token stop)
        {
            RunWorker(stop);
        });
    }

    ~WorkshopManagerApp()
    {
        worker_.request_stop();
        workChanged_.notify_all();
        if (worker_.joinable()) worker_.join();
        SaveDraftPreferencesUnlocked();
    }

    bool CanClose() const
    {
        return !publishInProgress_.load();
    }

    void ExplainCloseBlocked()
    {
        closeBlocked_ = true;
        taskDetailsOpen_ = true;
        RequestManagerFrame();
    }

    void CheckConnection()
    {
        connectionCheckRequested_.store(true);
        workChanged_.notify_all();
        RequestManagerFrame();
    }

    const char* WindowTitle() const
    {
        return T("SnowDesktop 创意工坊创作者管理器",
            "SnowDesktop Workshop Creator Manager");
    }

    void AddDroppedDirectory(const std::filesystem::path& path)
    {
        std::lock_guard lock(mutex_);
        WorkshopProject* project = nullptr;
        std::string error;
        if (store_.AddDirectory(path, project, error) && store_.Save(error))
        {
            InvalidatePreparedPublishUnlocked();
            SelectLocalProjectUnlocked(*project);
            activePage_ = 0;
            requestedPage_ = 0;
            SetMessageUnlocked(true, T("已添加本地项目", "Local project added"));
        }
        else SetMessageUnlocked(false, error);
    }

    void Render(HWND window)
    {
        previewCache_.Pump(gDevice.Get());
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("SnowDesktop Workshop Manager", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        RenderHeader(window);
        ImGui::Spacing();
        bool hasTask;
        bool runningTask;
        {
            std::lock_guard lock(mutex_);
            hasTask = publishTask_.Snapshot().HasTask() &&
                (!taskResultDismissed_ || publishTask_.Snapshot().Running());
            runningTask = publishTask_.Snapshot().Running();
        }
        const float splitterHeight = hasTask ? 8.0f * gDpiScale : 0.0f;
        const float maximumFooterHeight = std::max(40.0f * gDpiScale,
            ImGui::GetContentRegionAvail().y - 280.0f * gDpiScale - splitterHeight);
        const float naturalHeight = std::max(runningTask ? 104.0f : 70.0f,
            taskPanelNaturalHeightDip_);
        const float footerHeight = hasTask ? std::clamp(
            (taskPanelUserSized_ ? taskPanelHeightDip_ : naturalHeight) * gDpiScale,
            40.0f * gDpiScale, maximumFooterHeight) : 0.0f;
        const float mainHeight = std::max(1.0f,
            ImGui::GetContentRegionAvail().y - footerHeight - splitterHeight -
                ImGui::GetStyle().ItemSpacing.y * (hasTask ? 2.0f : 0.0f));
        const float sidebarPadding = 8.0f * gDpiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(sidebarPadding, sidebarPadding));
        ImGui::BeginChild("manager-sidebar",
            ImVec2(std::min(kSidebarWidthDip * gDpiScale,
                ImGui::GetContentRegionAvail().x * 0.30f), mainHeight),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        RenderSidebar(window);
        ImGui::EndChild();
        ImGui::SameLine();
        const float contentPadding = 16.0f * gDpiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(contentPadding, contentPadding));
        ImGui::BeginChild("manager-content", ImVec2(0, mainHeight),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        switch (activePage_)
        {
        case 0: RenderLocalProjects(window); break;
        case 1: RenderWorkshop(); break;
        default: activePage_ = 0; break;
        }
        ImGui::EndChild();
        if (hasTask)
        {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("task-panel-resize", ImVec2(width, splitterHeight));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.y != 0)
            {
                taskPanelUserSized_ = true;
                taskPanelHeightDip_ = std::clamp(
                    (footerHeight - ImGui::GetIO().MouseDelta.y) / gDpiScale,
                    40.0f, maximumFooterHeight / gDpiScale);
                RequestManagerFrame();
            }
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(origin.x + width * 0.5f - 24.0f * gDpiScale,
                    origin.y + splitterHeight * 0.5f),
                ImVec2(origin.x + width * 0.5f + 24.0f * gDpiScale,
                    origin.y + splitterHeight * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Border), 3.0f * gDpiScale);
        }
        if (hasTask)
        {
        ImGui::BeginChild("manager-task", ImVec2(0, footerHeight),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        RenderTaskArea();
        const float naturalTaskHeight = (ImGui::GetCursorPosY() +
            ImGui::GetStyle().WindowPadding.y) / gDpiScale;
        if (std::abs(naturalTaskHeight - taskPanelNaturalHeightDip_) > 1.0f)
        {
            taskPanelNaturalHeightDip_ = naturalTaskHeight;
            if (hasTask && !taskPanelUserSized_) RequestManagerFrame();
        }
        ImGui::EndChild();
        }
        ImGui::End();
    }

    void PollMainSettings(HWND window)
    {
        RefreshLanguageFromMainSettings(window);
        {
            std::lock_guard lock(mutex_);
            for (auto it = feedback_.begin(); it != feedback_.end();)
            {
                if (it->second.success && it->second.expires != std::chrono::steady_clock::time_point::max())
                {
                    RequestManagerFrame();
                    if (std::chrono::steady_clock::now() >= it->second.expires)
                    { it = feedback_.erase(it); continue; }
                }
                ++it;
            }
        }
        const auto revision = steam_.Status().connectionRevision;
        if (revision != observedConnectionRevision_)
        {
            observedConnectionRevision_ = revision;
            RequestManagerFrame();
        }
        if (!busy_.load() && publishedRefreshRequested_.exchange(false))
            RefreshPublished(1);
    }

private:
    const char* T(const char* chinese, const char* english) const
    {
        return localization_.Translate(english, chinese);
    }

    template<typename... Args>
    std::string T(const char* chinese, const char* english, Args... args) const
    {
        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer), T(chinese, english), args...);
        return buffer;
    }

    WorkshopProject* FindProjectUnlocked(std::string_view id)
    {
        const auto found = std::find_if(store_.Projects().begin(),
            store_.Projects().end(), [&](const WorkshopProject& project)
            { return project.localId == id; });
        return found == store_.Projects().end() ? nullptr : &*found;
    }

    const PublishedItem* FindWorkshopItemUnlocked(std::uint64_t id)
    {
        const auto& items = publishedPages_.Snapshot().items;
        const auto found = std::find_if(items.begin(), items.end(),
            [&](const PublishedItem& item)
            { return item.publishedFileId == id; });
        return found == items.end() ? nullptr : &*found;
    }

    void SetMessage(bool success, std::string value)
    {
        std::lock_guard lock(mutex_);
        SetMessageUnlocked(success, std::move(value));
    }

    enum class WorkKind { Local, TextPreview, Prepare, Publish, Published, Binding };
    struct WorkTarget
    {
        WorkKind kind = WorkKind::Local;
        int page = 0;
        std::string localId;
    };
    struct Feedback
    {
        bool success = false;
        std::string text;
        std::chrono::steady_clock::time_point expires{};
        std::filesystem::path outputFile;
    };

    void SetMessageUnlocked(bool success, std::string value,
        std::filesystem::path outputFile = {}, bool retain = false)
    {
        WorkTarget target;
        if (std::this_thread::get_id() == worker_.get_id())
        {
            std::lock_guard workLock(workMutex_);
            target = runningWork_;
        }
        else
        {
            target.page = activePage_;
            target.localId = activePage_ == 0 ? selectedLocalId_ : std::string{};
        }
        // Publication results already have a persistent task view.
        if (target.kind == WorkKind::Publish && success) return;
        feedback_[{target.page, target.localId}] = {success, std::move(value),
            success && outputFile.empty() && !retain ? std::chrono::steady_clock::now() + std::chrono::seconds(5) :
                std::chrono::steady_clock::time_point::max(), std::move(outputFile)};
        RequestManagerFrame();
    }

    bool ShowSteamRecovery(const CoreError& error)
    {
        const auto problem = ClassifySteamConnectionProblem(
            error.code, error.steamInitResult);
        if (problem == SteamConnectionProblem::None) return false;
        std::lock_guard lock(mutex_);
        steamConnectionProblem_ = problem;
        SetMessageUnlocked(false, error.code + ": " + error.message);
        RequestManagerFrame();
        return true;
    }

    void InvalidatePreparedPublishUnlocked()
    {
        ++publishInputsRevision_;
        preparedPublish_.reset();
    }

    template<typename Work>
    bool StartWork(Work&& work, WorkKind kind = WorkKind::Local,
        std::string localId = {})
    {
        if (busy_.exchange(true)) return false;
        {
            std::lock_guard lock(workMutex_);
            auto task = std::make_shared<std::decay_t<Work>>(
                std::forward<Work>(work));
            pendingWork_ = [task] { (*task)(); };
            runningWork_ = {kind, (kind == WorkKind::Published || kind == WorkKind::Binding) ? 1 : 0,
                kind == WorkKind::Published ? std::string{} :
                    (localId.empty() ? selectedLocalId_ : std::move(localId))};
        }
        workChanged_.notify_all();
        RequestManagerFrame();
        return true;
    }

    void RunWorker(std::stop_token stop)
    {
        std::string previousAccount;
        bool previouslyConnected = false;
        std::uint64_t previousRevision = 0;
        while (!stop.stop_requested())
        {
            std::function<void()> task;
            {
                std::unique_lock lock(workMutex_);
                workChanged_.wait_for(lock, stop,
                    std::chrono::milliseconds(50), [this]
                    { return pendingWork_ || connectionCheckRequested_.load(); });
                if (stop.stop_requested()) break;
                task = std::move(pendingWork_);
                pendingWork_ = {};
            }
            if (task)
            {
                try { task(); }
                catch (const std::exception& error)
                {
                    std::lock_guard lock(mutex_);
                    WorkKind kind;
                    {
                        std::lock_guard workLock(workMutex_);
                        kind = runningWork_.kind;
                    }
                    if (kind == WorkKind::Publish)
                    {
                        publishTask_.Finish({}, false,
                            CoreError{kSteamOperationFailed, "publish_interrupted", error.what()});
                        submitStarted_.store(false);
                        publishInProgress_.store(false);
                    }
                    if (kind == WorkKind::TextPreview) localizationPreviewLoading_ = false;
                    SetMessageUnlocked(false, error.what());
                }
                busy_.store(false);
                RequestManagerFrame();
            }
            CoreError error;
            steam_.PollConnection(error,
                connectionCheckRequested_.exchange(false));
            const SteamStatus status = steam_.Status();
            const bool connected = status.initialized && status.loggedOn;
            const bool accountChanged = !previousAccount.empty() &&
                previousAccount != status.steamId;
            if (status.connectionRevision != previousRevision)
            {
                std::lock_guard lock(mutex_);
                steamConnectionProblem_ = status.connectionProblem;
                if (accountChanged)
                {
                    publishedPages_.ResetForAccount(status.steamId);
                    bindingTargetId_.clear();
                    feedback_.erase({1, {}});
                    InvalidatePreparedPublishUnlocked();
                }
                previousRevision = status.connectionRevision;
                RequestManagerFrame();
            }
            if (connected && (!previouslyConnected || accountChanged) &&
                !stop.stop_requested())
            {
                publishedRefreshRequested_.store(true);
                RequestManagerFrame();
            }
            previouslyConnected = connected;
            previousAccount = status.steamId;
        }
        steam_.Shutdown();
    }

    void RenderHeader(HWND window)
    {
        (void)window;
        const SteamStatus status = steam_.Status();
        const bool connected = status.initialized && status.loggedOn;
        const char* state = T("Steam 未连接", "Steam disconnected");
        if (connected) state = T("Steam 已连接", "Steam connected");
        else if (status.connectionState == ManagerConnectionState::Connecting)
            state = T("正在连接 Steam…", "Connecting to Steam…");
        else if (status.connectionState == ManagerConnectionState::Offline)
            state = T("Steam 离线", "Steam offline");
        const char* check = T("检查连接", "Check connection");
        const char* details = T("连接详情", "Connection details");
        const float rightWidth = ImGui::CalcTextSize(state).x + ButtonWidth(check) +
            ButtonWidth(details) + ImGui::GetStyle().ItemSpacing.x * 2.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
        const bool headerTable = ImGui::BeginTable("manager-header", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                ImGuiTableFlags_NoPadOuterX);
        if (headerTable)
        {
            ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("connection", ImGuiTableColumnFlags_WidthFixed, rightWidth);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s   v%s", T("SnowDesktop 创意工坊创作者管理器",
            "SnowDesktop Workshop Creator Manager"), SNOWDESKTOP_VERSION);
        if (headerTable) ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(connected ?
            ImVec4(0.18f, 0.58f, 0.32f, 1.0f) :
            ImVec4(0.64f, 0.38f, 0.06f, 1.0f), "%s", state);
        ImGui::SameLine();
        ImGui::BeginDisabled(!status.compiled || busy_.load());
        if (SecondaryButton(check)) CheckConnection();
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool openDetails = SecondaryButton(details);
        if (headerTable) ImGui::EndTable();
        ImGui::PopStyleVar();
        if (openDetails) ImGui::OpenPopup("connection-details");
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
            ImVec2(std::min(620.0f * gDpiScale,
                ImGui::GetIO().DisplaySize.x - 24.0f * gDpiScale), FLT_MAX));
        if (ImGui::BeginPopup("connection-details"))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                std::min(560.0f * gDpiScale,
                    ImGui::GetIO().DisplaySize.x - 64.0f * gDpiScale));
            ImGui::TextUnformatted(state);
            if (!status.steamId.empty())
                ImGui::Text("Steam ID: %s", status.steamId.c_str());
            SteamConnectionProblem problem = status.connectionProblem;
            if (problem == SteamConnectionProblem::None)
            {
                std::lock_guard lock(mutex_);
                problem = steamConnectionProblem_;
            }
            if (problem != SteamConnectionProblem::None)
            {
                const auto feedback = ConnectionFeedback(problem);
                ImGui::TextWrapped("%s", T(feedback.chinese, feedback.english));
                if (feedback.offerOpenSteam &&
                    BlueButton(T("打开 Steam", "Open Steam")) &&
                    !OpenUrl(SteamClientHomeUrl()))
                    SetMessage(false, T("无法打开 Steam。请手动启动 Steam 后重试。",
                        "Steam could not be opened. Start it manually and try again."));
            }
            if (!status.diagnostic.empty())
                ImGui::TextWrapped("%s", status.diagnostic.c_str());
            if (!connected)
                ImGui::TextWrapped("%s", T("启动并登录 Steam 后，可在此检查连接。",
                    "Refresh connection after starting or signing in to Steam."));
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
        }
        ImGui::Spacing();
        PushNavigationStyle();
        int visiblePage = activePage_;
        if (ImGui::BeginTabBar("workspace-pages", ImGuiTabBarFlags_DrawSelectedOverline))
        {
            if (BeginProjectTab((std::string(T("本地项目", "Local projects")) +
                    "###PublishComponentPage").c_str(), requestedPage_ == 0))
            {
                visiblePage = 0;
                ImGui::EndTabItem();
            }
            if (BeginProjectTab((std::string(T("我的 Workshop", "My Workshop")) +
                    "###MyWorkshopPage").c_str(), requestedPage_ == 1))
            {
                visiblePage = 1;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (requestedPage_ >= 0)
        {
            activePage_ = requestedPage_;
            if (visiblePage == requestedPage_) requestedPage_ = -1;
            else RequestManagerFrame(); // ImGui queues programmatic tab selection for the next frame.
        }
        else activePage_ = visiblePage;
        PopNavigationStyle();
    }

    void PushNavigationStyle()
    {
        const ImVec4 selectedFill(0.84f, 0.90f, 0.99f, 1.0f);
        const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_Button];
        ImGui::PushStyleColor(ImGuiCol_TabSelected, selectedFill);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, selectedFill);
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, accent);
        ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, accent);
        ImGui::PushStyleVar(ImGuiStyleVar_TabBarOverlineSize, 3.0f * gDpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * gDpiScale, 8.0f * gDpiScale));
    }

    void PopNavigationStyle()
    {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    }

    const char* ItemTypeLabel(ManagerWorkshopItemKind kind) const
    {
        switch (kind)
        {
        case ManagerWorkshopItemKind::Widget: return T("组件", "Component");
        case ManagerWorkshopItemKind::Theme: return T("主题", "Theme");
        case ManagerWorkshopItemKind::IconPack: return T("图标包", "Icon pack");
        default: return T("未知类型", "Unknown type");
        }
    }

    void RenderSidebar(HWND window)
    {
        if (activePage_ == 0)
        {
            ImGui::BeginDisabled(busy_.load());
            if (SidebarButton(T("添加组件目录", "Add component folder"), true))
                if (const auto path = PickPath(window, true)) AddDroppedDirectory(*path);
            ImGui::EndDisabled();
            if (SidebarButton((std::string(T("打开目录", "Open folder")) +
                    "###OpenDevelopmentRoot").c_str()))
                OpenDirectory(developmentRoot_);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s",
                T("本地项目工作区", "Local project workspace"),
                WideToUtf8(developmentRoot_.wstring()).c_str());
        }
        else
        {
            const SteamStatus connection = steam_.Status();
            ImGui::BeginDisabled(busy_.load() || !connection.loggedOn);
            if (SidebarButton(T("刷新我的项目", "Refresh my items"), true))
            {
                std::uint32_t page;
                { std::lock_guard lock(mutex_); page = publishedPages_.Snapshot().page; }
                RefreshPublished(page);
            }
            ImGui::EndDisabled();
        }
        if (SidebarButton(T("打开创意工坊", "Open Workshop")))
            OpenSteamUrlWithWebFallback(SteamWorkshopClientUrl(), SteamWorkshopHomeUrl());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        char* search = activePage_ == 0 ? localSearch_ : publishedSearch_;
        ImGui::InputTextWithHint("##project-search", activePage_ == 0 ?
            T("搜索名称或路径", "Search name or path") : T("搜索本页项目", "Search this page"),
            search, std::size(localSearch_));
        ImGui::Spacing();
        ImGui::Separator();
        const float pagerHeight = activePage_ == 1 ?
            ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                ImGui::GetStyle().ItemSpacing.y * 2.0f : 0.0f;
        ImGui::BeginChild(activePage_ == 0 ? "local-items" : "published-items",
            ImVec2(0, std::max(1.0f, ImGui::GetContentRegionAvail().y - pagerHeight)));
        {
            std::lock_guard lock(mutex_);
            bool matched = false;
            if (activePage_ == 1)
            {
                for (const auto& item : publishedPages_.Snapshot().items)
                {
                if (search[0] && item.title.find(search) == std::string::npos) continue;
                    matched = true;
                    const auto identity = DescribeManagerWorkshopItem(item.metadata);
                    if (TwoLineProjectSelectable(item.title + "##" +
                            std::to_string(item.publishedFileId),
                            publishedPages_.Snapshot().selectedId == item.publishedFileId,
                            std::string(ItemTypeLabel(identity.kind)) + " · " + FormatLocalTime(item.updatedAt)))
                    {
                        publishedPages_.Select(item.publishedFileId);
                        bindingTargetId_.clear();
                        RequestManagerFrame();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.title.c_str());
                    ImGui::Spacing();
                }
            }
            else
            {
                for (const auto& project : store_.Projects())
                {
                    const std::string name = WideToUtf8(project.sourceDirectory.filename().wstring());
                    const std::string path = WideToUtf8(project.sourceDirectory.wstring());
                    const std::string haystack = name + " " + project.packageId + " " + project.localId + " " + path;
                    if (search[0] && haystack.find(search) == std::string::npos) continue;
                    matched = true;
                    if (TwoLineProjectSelectable(name + "##" + project.localId,
                            selectedLocalId_ == project.localId,
                            project.publishedFileId ? T("已绑定 Workshop", "Workshop bound") :
                                T("未绑定", "Not bound"), project.lastPublishedVersion))
                        SelectLocalProjectUnlocked(project);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
                    ImGui::Spacing();
                }
            }
            if (!matched)
            {
                const auto& online = publishedPages_.Snapshot();
                ImGui::TextWrapped("%s", search[0] ? T("没有匹配的项目", "No matching projects") :
                    activePage_ == 0 ? T("没有本地项目", "No local projects") :
                    online.loading ? T("正在加载项目…", "Loading items…") :
                    !steam_.Status().loggedOn ? T("Steam 未连接", "Steam disconnected") :
                    online.loaded ? T("没有已发布的项目", "No published items") :
                    T("选择一个项目", "Select an item"));
            }
        }
        ImGui::EndChild();
        if (activePage_ == 1)
        {
            std::uint32_t page, totalPages, totalResults;
            { std::lock_guard lock(mutex_); page = publishedPages_.Snapshot().page;
              totalPages = publishedPages_.Snapshot().totalPages; totalResults = publishedPages_.Snapshot().totalResults; }
            ImGui::Separator();
            ImGui::TextDisabled("%s", T("共 %u 项", "%u items", totalResults).c_str());
            if (ImGui::BeginTable("published-pager", 3, ImGuiTableFlags_SizingFixedFit |
                    ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
            {
                const float buttonWidth = ImGui::GetFrameHeight();
                ImGui::TableSetupColumn("previous", ImGuiTableColumnFlags_WidthFixed, buttonWidth);
                ImGui::TableSetupColumn("page", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("next", ImGuiTableColumnFlags_WidthFixed, buttonWidth);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::BeginDisabled(busy_.load() || page <= 1 || !steam_.Status().loggedOn);
                if (SecondaryButton("<", ImVec2(buttonWidth, 0))) RefreshPublished(page - 1);
                ImGui::EndDisabled();
                ImGui::TableSetColumnIndex(1);
                const std::string pageText = totalPages == 0 ? "—" :
                    std::to_string(page) + " / " + std::to_string(totalPages);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f,
                    (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(pageText.c_str()).x) * 0.5f));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(pageText.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::BeginDisabled(busy_.load() || totalPages == 0 || page >= totalPages || !steam_.Status().loggedOn);
                if (SecondaryButton(">", ImVec2(buttonWidth, 0))) RefreshPublished(page + 1);
                ImGui::EndDisabled();
                ImGui::EndTable();
            }
        }
    }

    void SaveDraftPreferencesUnlocked()
    {
        auto* project = FindProjectUnlocked(selectedLocalId_);
        if (!project) return;
        auto& draft = drafts_.ForProject(*project, JoinTags(project->tags));
        if (!draft.preferencesDirty) return;
        draft.ApplyPreferences(*project);
        std::string error;
        if (store_.Save(error)) draft.PreferencesSaved();
        else SetMessageUnlocked(false, error);
    }

    void SelectLocalProjectUnlocked(const WorkshopProject& project)
    {
        if (selectedLocalId_ == project.localId) return;
        SaveDraftPreferencesUnlocked();
        InvalidatePreparedPublishUnlocked();
        selectedLocalId_ = project.localId;
        localizationStateProjectId_.clear();
        RequestManagerFrame();
    }

    void RenderTaskArea()
    {
        ManagerPublishTaskSnapshot task;
        {
            std::lock_guard lock(mutex_);
            task = publishTask_.Snapshot();
        }
        if (!task.HasTask())
        {
            if (busy_.load()) ImGui::TextDisabled("%s", T("正在处理…", "Working…"));
            else ImGui::TextDisabled("%s", T("正常", "Ready"));
            return;
        }
        const char* returnToProject = T("返回任务项目", "Back to task project");
        const char* detailLabel = T("任务详情", "Task details");
        const char* openItem = T("打开 Steam 项目管理页", "Open Steam item controls");
        const bool showItem = !task.Running() && task.publishedFileId != 0;
        if (task.Running())
            ImGui::TextWrapped("%s", T("正在发布：%s", "Publishing: %s", task.name.c_str()).c_str());
        else ImGui::TextWrapped("%s", task.name.c_str());
        if (SecondaryButton(returnToProject))
        {
            std::lock_guard lock(mutex_);
            if (const auto* project = FindProjectUnlocked(task.localId))
            {
                SelectLocalProjectUnlocked(*project);
                activePage_ = 0;
                requestedPage_ = 0;
                taskResultDismissed_ = false;
                localSearch_[0] = 0;
                RequestManagerFrame();
            }
        }
        ContinueButtonRow(detailLabel);
        if (SecondaryButton(detailLabel)) taskDetailsOpen_ = !taskDetailsOpen_;
        if (showItem)
        {
            ContinueButtonRow(openItem);
            if (SecondaryButton(openItem))
                OpenSteamUrlWithWebFallback(SteamCommunityItemClientUrl(task.publishedFileId),
                    CommunityItemUrl(task.publishedFileId));
        }
        if (!task.Running())
        {
            ContinueButtonRow(T("关闭结果", "Close result"));
            if (SecondaryButton(T("关闭结果", "Close result")))
            { taskResultDismissed_ = true; RequestManagerFrame(); }
        }
        const char* phase = T("正在准备提交", "Preparing submission");
        if (!task.Running())
        {
            switch (task.outcome)
            {
            case ManagerPublishOutcome::Completed: phase = T("更新完成", "Update complete"); break;
            case ManagerPublishOutcome::Partial: phase = T("部分完成", "Partially complete"); break;
            case ManagerPublishOutcome::Unknown: phase = T("结果待确认", "Result unconfirmed"); break;
            case ManagerPublishOutcome::Failed: phase = T("发布失败", "Publishing failed"); break;
            case ManagerPublishOutcome::NeedsLegalAgreement:
                phase = T("上传完成，需接受协议", "Upload complete; agreement required"); break;
            default: break;
            }
        }
        else if (task.stage == PublishStage::UploadingContent)
            phase = T("正在上传组件内容", "Uploading component content");
        else if (task.stage == PublishStage::UploadingPreview)
            phase = T("正在上传预览", "Uploading preview");
        else if (task.stage == PublishStage::PreparingContent)
            phase = T("正在处理内容文件", "Processing content files");
        else if (task.submissionIndex > 1 && !task.submitStarted)
            phase = T("正在同步发布文案", "Synchronizing listing text");
        else if (task.stage == PublishStage::CommittingChanges ||
            task.stage == PublishStage::Completed)
            phase = T("等待 Steam 确认", "Waiting for Steam confirmation");
        if (!task.Running()) ImGui::PushStyleColor(ImGuiCol_Text,
            task.outcome == ManagerPublishOutcome::Completed ?
                ImVec4(0.18f, 0.58f, 0.32f, 1) :
                ImVec4(0.64f, 0.38f, 0.06f, 1));
        ImGui::TextWrapped("%s", phase);
        if (!task.Running()) ImGui::PopStyleColor();
        if (task.Running() && (task.stage == PublishStage::CommittingChanges ||
                task.stage == PublishStage::Completed))
            ImGui::TextDisabled("%s", T("已等待：%llu 秒", "Waiting: %llu s",
                static_cast<unsigned long long>(task.StageElapsed().count())).c_str());
        if (task.submissionTotal > 0)
        {
            const std::string count = T("已完成 %zu/%zu 次提交", "Completed %zu of %zu submissions",
                task.confirmedSubmissions, task.submissionTotal);
            ContinueButtonRow(count.c_str());
            ImGui::TextDisabled("%s", count.c_str());
            if (task.Running() && !task.language.empty())
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", task.language.c_str());
            }
        }
        const bool byteStage = task.stage == PublishStage::UploadingContent ||
            task.stage == PublishStage::UploadingPreview;
        if (task.Running() && byteStage)
        {
            if (const auto fraction = task.StageFraction())
            {
                const std::string overlay = T("当前阶段：%.0f%%", "Current stage: %.0f%%",
                    static_cast<double>(*fraction) * 100.0);
                char bytes[96]{};
                std::snprintf(bytes, sizeof(bytes), "%.1f / %.1f MiB",
                    static_cast<double>(task.processed) / 1048576.0,
                    static_cast<double>(task.total) / 1048576.0);
                const std::string label = overlay + "   " + bytes;
                ImGui::ProgressBar(*fraction, ImVec2(-1, 0), label.c_str());
            }
            else ImGui::TextDisabled("%s", T("正在处理…", "Working…"));
        }
        if (task.submitStarted && closeBlocked_)
            ImGui::TextWrapped("%s", T("Steam 正在提交，请保持此窗口打开，可以最小化。",
                "Steam is submitting. Keep this window open; you can minimize it."));
        else if (task.Running() && closeBlocked_)
            ImGui::TextWrapped("%s", T("正在发布，请保持此窗口打开，可以最小化。",
                "Publishing is in progress. Keep this window open; you can minimize it."));
        if (task.outcome == ManagerPublishOutcome::Unknown)
            ImGui::TextWrapped("%s", T("请先核对 Steam 项目再重试，本次提交可能已完成。",
                "Check the Steam item before retrying; the submission may already have completed."));
        if (taskDetailsOpen_)
        {
            ImGui::Separator();
            if (task.publishedFileId != 0)
                ImGui::Text("%s: %llu", T("已绑定 Workshop", "Workshop bound"), task.publishedFileId);
            if (!task.failedLanguage.empty())
                ImGui::TextWrapped("%s: %s", T("Steam 语言", "Steam language"), task.failedLanguage.c_str());
            if (!task.errorMessage.empty())
                ImGui::TextWrapped("%s: %s", task.errorCode.c_str(), task.errorMessage.c_str());
            if (task.Running() && task.publishedFileId != 0 &&
                SecondaryButton(T("打开 Steam 项目管理页", "Open Steam item controls")))
                OpenSteamUrlWithWebFallback(SteamCommunityItemClientUrl(task.publishedFileId),
                    CommunityItemUrl(task.publishedFileId));
        }
    }

    void RenderOperationFeedbackUnlocked()
    {
        WorkTarget work;
        { std::lock_guard workLock(workMutex_); work = runningWork_; }
        if (busy_.load() && work.kind != WorkKind::Publish &&
            work.kind != WorkKind::TextPreview && work.page == activePage_ &&
            (work.localId.empty() || (activePage_ == 0 ? work.localId == selectedLocalId_ :
                work.localId == "item:" + std::to_string(publishedPages_.Snapshot().selectedId))))
            ImGui::TextDisabled("%s", work.kind == WorkKind::Published ?
                T("正在加载项目…", "Loading items…") : work.kind == WorkKind::Prepare ? T("正在准备组件包", "Preparing package") : T("正在处理…", "Working…"));
        const auto render = [&](const std::pair<int, std::string>& key)
        {
            auto found = feedback_.find(key);
            if (found == feedback_.end()) return;
            if (found->second.success && std::chrono::steady_clock::now() >= found->second.expires)
            { feedback_.erase(found); return; }
            ImGui::Spacing();
            ImGui::PushID(found->first.second.c_str());
            const auto outputFile = found->second.outputFile;
            const char* openLabel = T("打开所在位置", "Open file location");
            const float actionsWidth = ButtonWidth("X") + (outputFile.empty() ? 0.0f :
                ButtonWidth(openLabel) + ImGui::GetStyle().ItemSpacing.x);
            bool dismiss = false;
            bool open = false;
            if (ImGui::BeginTable("operation-feedback", 2, ImGuiTableFlags_SizingFixedFit |
                    ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
            {
                ImGui::TableSetupColumn("message", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, found->second.success ?
                    ImVec4(0.18f, 0.58f, 0.32f, 1) : ImVec4(0.78f, 0.20f, 0.18f, 1));
                ImGui::TextWrapped("%s", found->second.text.c_str());
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1);
                if (!outputFile.empty())
                {
                    open = BlueButton(openLabel);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",
                        WideToUtf8(outputFile.wstring()).c_str());
                    ImGui::SameLine();
                }
                dismiss = BlueButton("X##dismiss-feedback");
                ImGui::EndTable();
            }
            if (dismiss) feedback_.erase(found);
            else if (open && !OpenFileLocation(outputFile))
                SetMessageUnlocked(false, T("无法打开所在位置。", "Could not open the file location."), outputFile);
            ImGui::PopID();
        };
        render({activePage_, {}});
        if (activePage_ == 0 && !selectedLocalId_.empty()) render({0, selectedLocalId_});
        if (activePage_ == 1) render({1, "item:" + std::to_string(publishedPages_.Snapshot().selectedId)});
    }

    void RenderLocalProjects(HWND window)
    {
        std::lock_guard lock(mutex_);
        if (store_.Projects().empty())
        {
            ImGui::BeginChild("local-empty-body", ImVec2(0, 0));
            RenderOperationFeedbackUnlocked();
            ImGui::TextWrapped("%s", T(
                "添加包含 widget.json 的组件目录，或把目录拖入此窗口。",
                "Add a component directory containing widget.json, or drop it onto this window."));
            ImGui::EndChild();
            return;
        }
        WorkshopProject* project = FindProjectUnlocked(selectedLocalId_);
        if (!project)
        {
            ImGui::BeginChild("local-unselected-body", ImVec2(0, 0));
            ImGui::TextWrapped("%s", T(
                "添加包含 widget.json 的组件目录，或把目录拖入此窗口。",
                "Add a component directory containing widget.json, or drop it onto this window."));
            RenderOperationFeedbackUnlocked();
            ImGui::EndChild();
            return;
        }
        ImGui::PushID(project->localId.c_str());
        RenderProjectDetailsUnlocked(window, *project);
        ImGui::PopID();
    }

    void RefreshLanguageFromMainSettings(HWND window)
    {
        if (settingsFile_.empty()) return;
        std::error_code error;
        const auto writeTime = std::filesystem::last_write_time(
            settingsFile_, error);
        if (error || (settingsWriteTime_ && *settingsWriteTime_ == writeTime))
            return;

        const auto effective = ReadMainLanguageSetting(settingsFile_);
        if (!effective) return;
        settingsWriteTime_ = writeTime;
        if (*effective == currentLanguage_) return;
        currentLanguage_ = *effective;
        localization_.SelectLanguage(currentLanguage_);
        const std::wstring title = Utf8ToWide(WindowTitle());
        SetWindowTextW(window, title.c_str());
        RequestManagerFrame();
    }

    void RenderProjectPreviewUnlocked(const WorkshopProject& project)
    {
        if (!project.primaryPreview.empty())
        {
            const std::uint64_t previewKey = StableKey(project.localId);
            previewCache_.RequestLocal(previewKey, project.primaryPreview);
            const PreviewTexture texture = previewCache_.Get(previewKey);
            if (texture.view)
            {
                const float scale = std::min(
                    std::min(220.0f * gDpiScale,
                        ImGui::GetContentRegionAvail().x) / texture.width,
                    (220.0f * gDpiScale) / texture.height);
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(texture.view))),
                    ImVec2(texture.width * scale, texture.height * scale));
            }
            else if (!texture.error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                    "%s", texture.error.c_str());
        }
    }

    void RenderProjectOverviewUnlocked(WorkshopProject& project)
    {
        const auto details = [&]
        {
            const bool facts = ImGui::BeginTable("project-facts", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                    ImGuiTableFlags_NoPadOuterX);
            if (facts)
            {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                    std::min(156.0f * gDpiScale, ImGui::GetContentRegionAvail().x * 0.4f));
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                const auto row = [](const char* label, const std::string& value)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextWrapped("%s", label);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", value.c_str());
                };
                row(T("状态", "Status"), project.publishedFileId ?
                    std::string(T("已绑定 Workshop", "Workshop bound")) + "  " +
                        std::to_string(*project.publishedFileId) :
                    T("本地草稿 · 尚未创建 Workshop 项目", "Local draft · no Workshop item yet"));
                if (!project.lastPublishedVersion.empty())
                    row(T("最后发布版本", "Last published version"), project.lastPublishedVersion);
                if (!project.packageId.empty()) row("Package ID", project.packageId);
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("%s", WideToUtf8(project.sourceDirectory.wstring()).c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (SecondaryButton(T("打开目录", "Open folder"))) OpenDirectory(project.sourceDirectory);
            const char* validate = T("校验", "Validate");
            ContinueButtonRow(validate);
            ImGui::BeginDisabled(busy_.load());
            if (SecondaryButton(validate)) StartInspect(project.localId, false);
            const char* pack = T("校验并打包", "Validate and pack");
            ContinueButtonRow(pack);
            if (SecondaryButton(pack)) StartInspect(project.localId, true);
            ImGui::EndDisabled();
            RenderOperationFeedbackUnlocked();
        };
        const float previewWidth = 220.0f * gDpiScale;
        const bool columns = !project.primaryPreview.empty() &&
            ImGui::GetContentRegionAvail().x >= previewWidth + 280.0f * gDpiScale +
                ImGui::GetStyle().CellPadding.x * 4.0f;
        if (columns && ImGui::BeginTable("project-overview", 2,
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                    ImGuiTableFlags_NoPadOuterX))
        {
            ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthFixed, previewWidth);
            ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            RenderProjectPreviewUnlocked(project);
            ImGui::TableSetColumnIndex(1);
            details();
            ImGui::EndTable();
        }
        else
        {
            RenderProjectPreviewUnlocked(project);
            ImGui::Spacing();
            details();
        }
    }

    void RenderProjectDetailsUnlocked(HWND window, WorkshopProject& project)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextWrapped("%s", WideToUtf8(project.sourceDirectory.filename().wstring()).c_str());
        const char* more = T("更多", "More");
        const float moreX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ButtonWidth(more);
        if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x <
                ImGui::GetWindowPos().x + moreX)
            ImGui::SameLine(moreX);
        else ImGui::SetCursorPosX(moreX);
        if (SecondaryButton(more)) ImGui::OpenPopup("local-project-actions");
        bool removed = false;
        if (ImGui::BeginPopup("local-project-actions"))
        {
            ImGui::BeginDisabled(busy_.load());
            if (SecondaryButton(T("仅移除本地记录", "Remove local record only")))
            {
                const std::string id = project.localId;
                std::string error;
                if (store_.Remove(id, error))
                {
                    InvalidatePreparedPublishUnlocked();
                    selectedLocalId_.clear();
                    drafts_.Erase(id);
                    if (!store_.Projects().empty()) selectedLocalId_ = store_.Projects().front().localId;
                    localizationStateProjectId_.clear();
                    removed = true;
                    if (store_.Save(error))
                        SetMessageUnlocked(true, T("已移除记录；源码和 Workshop 内容未删除",
                            "Record removed; source and Workshop content were not deleted"));
                    else SetMessageUnlocked(false, error);
                }
                else SetMessageUnlocked(false, error);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        if (removed) return;
        auto& draft = drafts_.ForProject(project, JoinTags(project.tags));
        auto& tagsBuffer_ = draft.tagsText;
        auto& titleBuffer_ = draft.manualEnglishTitle;
        auto& descriptionBuffer_ = draft.manualEnglishDescription;
        auto& publishPublic_ = draft.publishPublic;
        auto& syncPackageLocalization_ = draft.reusePackageText;
        auto& updatePreview_ = draft.updatePreview;
        auto& updateTags_ = draft.updateTags;
        const bool creating = !project.publishedFileId.has_value();
        if (localizationStateProjectId_ != project.localId ||
            localizationStateCreating_ != creating)
        {
            localizationStateProjectId_ = project.localId;
            localizationStateCreating_ = creating;
            localizationPreviewProjectId_.clear();
            localizationPreview_.clear();
            localizationPreviewError_.clear();
            localizationPreviewLoading_ = false;
        }
        const int requestedTab = std::exchange(requestedProjectTab_, -1);
        PushNavigationStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
            ImVec2(14.0f * gDpiScale, 8.0f * gDpiScale));
        const bool tabs = ImGui::BeginTabBar("project-sections",
            ImGuiTabBarFlags_DrawSelectedOverline);
        ImGui::PopStyleVar();
        if (!tabs)
        {
            PopNavigationStyle();
            return;
        }
        if (BeginProjectTab((std::string(T("概览", "Overview")) + "###overview").c_str(), requestedTab == 0))
        {
            ImGui::BeginChild("overview-body", ImVec2(0, 0));
            ImGui::Spacing();
            RenderProjectOverviewUnlocked(project);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (BeginProjectTab((std::string(T("发布资料", "Publishing details")) + "###publishing").c_str(), requestedTab == 1))
        {
            ImGui::BeginChild("publishing-body", ImVec2(0, 0));
            ImGui::BeginDisabled(busy_.load());
            ImGui::Spacing();
            BeginSettingRow(T("主预览", "Primary preview"),
                ButtonWidth(T("选择主预览", "Choose primary preview")));
            if (SecondaryButton(T("选择主预览", "Choose primary preview")))
            {
                if (const auto path = PickPath(window, false))
                {
                    InvalidatePreparedPublishUnlocked();
                    project.primaryPreview = *path;
                    std::string error;
                    if (!store_.Save(error)) SetMessageUnlocked(false, error);
                    else SetMessageUnlocked(true,
                        T("已保存主预览", "Primary preview saved"));
                }
            }
            if (ImGui::IsItemHovered() && !project.primaryPreview.empty())
                ImGui::SetTooltip("%s", WideToUtf8(project.primaryPreview.wstring()).c_str());
            RenderProjectPreviewUnlocked(project);
            std::array<char, 1024> tags{};
            std::copy_n(tagsBuffer_.c_str(),
                std::min(tagsBuffer_.size(), tags.size() - 1), tags.data());
            bool saveTags = false;
            if (ImGui::BeginTable("tags-row", 3, ImGuiTableFlags_SizingFixedFit |
                    ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
            {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthFixed,
                    std::min(SettingControlWidth(), ImGui::GetContentRegionAvail().x * 0.38f));
                ImGui::TableSetupColumn("save", ImGuiTableColumnFlags_WidthFixed,
                    ButtonWidth(T("保存标签", "Save tags")));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextWrapped("%s", T("标签（逗号分隔）", "Tags (comma-separated)"));
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputText("##project-tags", tags.data(), tags.size())) draft.EditTags(tags.data());
                ImGui::TableSetColumnIndex(2);
                saveTags = SecondaryButton(T("保存标签", "Save tags"));
                ImGui::EndTable();
            }
            if (saveTags)
            {
                InvalidatePreparedPublishUnlocked();
                project.tags = SplitTags(tagsBuffer_);
                std::string error;
                if (!store_.Save(error)) SetMessageUnlocked(false, error);
                else { draft.TagsSaved(); SetMessageUnlocked(true, T("标签已保存", "Tags saved")); }
            }
            if (creating)
            {
                BeginSettingRow(T("公开发布", "Publish publicly"), ImGui::GetFrameHeight());
                bool value = publishPublic_;
                if (ImGui::Checkbox("##publish-publicly", &value))
                { draft.SetPublishPublic(value); InvalidatePreparedPublishUnlocked(); }
            }
            BeginSettingRow(T(
                "复用组件包内的标题和说明",
                "Reuse component package titles and descriptions"),
                ImGui::GetFrameHeight());
            bool reuseText = syncPackageLocalization_;
            if (ImGui::Checkbox("##package-localization", &reuseText))
            {
                draft.SetReusePackageText(reuseText);
                const WorkshopTextSource previous =
                    project.publishPreferences.textSource;
                project.publishPreferences.textSource =
                    syncPackageLocalization_ ? WorkshopTextSource::Package :
                    (creating ? WorkshopTextSource::ManualEnglish :
                        WorkshopTextSource::Steam);
                std::string error;
                if (!store_.Save(error))
                {
                    project.publishPreferences.textSource = previous;
                    syncPackageLocalization_ = previous ==
                        WorkshopTextSource::Package;
                    SetMessageUnlocked(false, error);
                }
                else if (syncPackageLocalization_)
                    localizationPreviewProjectId_.clear();
                if (project.publishPreferences.textSource != previous)
                    InvalidatePreparedPublishUnlocked();
            }
            if (syncPackageLocalization_)
            {
                if (localizationPreviewProjectId_ != project.localId &&
                    !busy_.load())
                {
                    localizationPreviewProjectId_ = project.localId;
                    localizationPreview_.clear();
                    localizationPreviewError_.clear();
                    localizationPreviewLoading_ = true;
                    StartLocalizationPreview(project.localId);
                }
                bool previewOpen = false;
                bool refreshPreview = false;
                const std::string previewLabel = T(
                    "多语言文案预览（%zu 种 Steam 语言）",
                    "Localized text preview (%zu Steam languages)", localizationPreview_.size());
                if (ImGui::BeginTable("localization-preview-heading", 2, ImGuiTableFlags_SizingFixedFit |
                        ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
                {
                    ImGui::TableSetupColumn("heading", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("refresh", ImGuiTableColumnFlags_WidthFixed,
                        ButtonWidth(T("刷新文案预览", "Refresh text preview")));
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    previewOpen = ImGui::CollapsingHeader((previewLabel + "###localization-preview").c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", previewLabel.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::BeginDisabled(busy_.load());
                    refreshPreview = SecondaryButton(T("刷新文案预览", "Refresh text preview"));
                    ImGui::EndDisabled();
                    ImGui::EndTable();
                }
                if (refreshPreview)
                {
                    localizationPreviewProjectId_ = project.localId;
                    localizationPreview_.clear();
                    localizationPreviewError_.clear();
                    localizationPreviewLoading_ = true;
                    StartLocalizationPreview(project.localId);
                }
                if (localizationPreviewLoading_)
                {
                    ImGui::TextDisabled("%s", T(
                        "正在读取多语言文案…",
                        "Loading localized text…"));
                }
                else if (!localizationPreviewError_.empty())
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                        "%s: %s", T("文案预览失败", "Text preview failed"),
                        localizationPreviewError_.c_str());
                }
                else if (localizationPreview_.empty())
                {
                    ImGui::TextDisabled("%s", T(
                        "组件包没有可发布到 Steam 的多语言文案",
                        "The component package has no localized text publishable to Steam"));
                }
                else
                {
                    if (previewOpen &&
                        ImGui::BeginTable("package-localization-preview", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_ScrollY,
                            ImVec2(0, 280.0f * gDpiScale)))
                    {
                        ImGui::TableSetupColumn(T("Steam 语言", "Steam language"),
                            ImGuiTableColumnFlags_WidthFixed,
                            110.0f * gDpiScale);
                        ImGui::TableSetupColumn(T("标题", "Title"),
                            ImGuiTableColumnFlags_WidthStretch, 0.8f);
                        ImGui::TableSetupColumn(T("说明", "Description"),
                            ImGuiTableColumnFlags_WidthStretch, 2.2f);
                        ImGui::TableHeadersRow();
                        for (const auto& localized : localizationPreview_)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(localized.language.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextWrapped("%s", localized.title.c_str());
                            ImGui::TableSetColumnIndex(2);
                            if (localized.description.empty())
                                ImGui::TextDisabled("—");
                            else
                                ImGui::TextWrapped("%s",
                                    localized.description.c_str());
                        }
                        ImGui::EndTable();
                    }
                }
            }
            if (creating && !syncPackageLocalization_)
            {
                std::array<char, 256> title{};
                std::copy_n(titleBuffer_.c_str(),
                    std::min(titleBuffer_.size(), title.size() - 1), title.data());
                BeginSettingRow(T("初始标题", "Initial title"),
                    SettingControlWidth());
                ImGui::SetNextItemWidth(SettingControlWidth());
                if (ImGui::InputText("##initial-title", title.data(), title.size()))
                {
                    draft.EditTitle(title.data());
                    draft.ApplyPreferences(project);
                    InvalidatePreparedPublishUnlocked();
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    project.publishPreferences.manualEnglishTitle = titleBuffer_;
                    std::string error;
                    if (!store_.Save(error)) SetMessageUnlocked(false, error);
                }
                std::array<char, 4096> description{};
                std::copy_n(descriptionBuffer_.c_str(),
                    std::min(descriptionBuffer_.size(), description.size() - 1),
                    description.data());
                BeginSettingRow(T("初始说明", "Initial description"),
                    SettingControlWidth());
                if (ImGui::InputTextMultiline("##initial-description",
                        description.data(), description.size(),
                        ImVec2(SettingControlWidth(),
                            90.0f * gDpiScale)))
                {
                    draft.EditDescription(description.data());
                    draft.ApplyPreferences(project);
                    InvalidatePreparedPublishUnlocked();
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    project.publishPreferences.manualEnglishDescription =
                        descriptionBuffer_;
                    std::string error;
                    if (!store_.Save(error)) SetMessageUnlocked(false, error);
                }
            }
            if (!creating)
            {
                BeginSettingRow(T("同时更新主预览", "Also update primary preview"),
                    ImGui::GetFrameHeight());
                bool previewValue = updatePreview_;
                if (ImGui::Checkbox("##update-preview", &previewValue))
                {
                    draft.SetUpdatePreview(previewValue);
                    const WorkshopAssetSource previous =
                        project.publishPreferences.previewSource;
                    project.publishPreferences.previewSource = updatePreview_ ?
                        WorkshopAssetSource::Local : WorkshopAssetSource::Steam;
                    std::string error;
                    if (!store_.Save(error))
                    {
                        project.publishPreferences.previewSource = previous;
                        updatePreview_ = previous == WorkshopAssetSource::Local;
                        SetMessageUnlocked(false, error);
                    }
                    else InvalidatePreparedPublishUnlocked();
                }
                BeginSettingRow(T("同时更新标签", "Also update tags"),
                    ImGui::GetFrameHeight());
                bool tagsValue = updateTags_;
                if (ImGui::Checkbox("##update-tags", &tagsValue))
                {
                    draft.SetUpdateTags(tagsValue);
                    const WorkshopAssetSource previous =
                        project.publishPreferences.tagsSource;
                    project.publishPreferences.tagsSource = updateTags_ ?
                        WorkshopAssetSource::Local : WorkshopAssetSource::Steam;
                    std::string error;
                    if (!store_.Save(error))
                    {
                        project.publishPreferences.tagsSource = previous;
                        updateTags_ = previous == WorkshopAssetSource::Local;
                        SetMessageUnlocked(false, error);
                    }
                    else InvalidatePreparedPublishUnlocked();
                }
            }
            ImGui::EndDisabled();
            RenderOperationFeedbackUnlocked();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (BeginProjectTab((std::string(T("核对发布", "Review and publish")) + "###review").c_str(), requestedTab == 2))
        {
            ImGui::BeginChild("review-body", ImVec2(0, 0));
            const bool missingManualTitle = creating &&
                !syncPackageLocalization_ && titleBuffer_.empty();
            ImGui::Spacing();
            PreparedManagerPublish* prepared = preparedPublish_ &&
                preparedPublish_->localId == project.localId ?
                &*preparedPublish_ : nullptr;
            if (prepared)
            {
                const ComponentPublishPlan& plan = prepared->plan;
                const char* action = T("创建项目", "Create item");
                if (plan.action == ComponentPublishAction::UpdateContent)
                    action = T("更新组件内容", "Update component content");
                else if (plan.action == ComponentPublishAction::UpdateMetadata)
                    action = T("仅更新资料", "Update listing metadata only");
                ImGui::Text("%s: %s", T("操作", "Action"), action);
                if (plan.action == ComponentPublishAction::Create)
                    ImGui::Text("%s: %s", T("可见性", "Visibility"),
                        plan.visibility == 0 ? T("公开", "Public") : T("私有", "Private"));
                ImGui::Text("%s: %s", T("版本", "Version"),
                    plan.version.c_str());
                ImGui::TextWrapped("SHA-256: %s", plan.sha256.c_str());
                ImGui::Text("%s: %s", T("组件内容", "Component content"),
                    plan.updateContent ? T("将上传", "Upload") :
                        T("未变化，不上传", "Unchanged; do not upload"));
                if (plan.localizations.empty())
                    ImGui::Text("%s: %s", T("标题和说明", "Title and description"),
                        T("保留 Steam 当前内容", "Preserve current Steam text"));
                else
                {
                    ImGui::Text(T("标题和说明：提交 %zu 种 Steam 语言",
                        "Title and description: submit %zu Steam languages",
                        plan.localizations.size()).c_str());
                    const std::string listingLabel = T(
                        "本次提交的多语言文案（%zu 种）",
                        "Localized text in this submission (%zu)",
                        plan.localizations.size());
                    if (ImGui::CollapsingHeader(listingLabel.c_str()) &&
                        ImGui::BeginTable("prepared-localization-plan", 3,
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_ScrollY,
                            ImVec2(0, 280.0f * gDpiScale)))
                    {
                        ImGui::TableSetupColumn(T("Steam 语言", "Steam language"),
                            ImGuiTableColumnFlags_WidthFixed,
                            110.0f * gDpiScale);
                        ImGui::TableSetupColumn(T("标题", "Title"),
                            ImGuiTableColumnFlags_WidthStretch, 0.8f);
                        ImGui::TableSetupColumn(T("说明", "Description"),
                            ImGuiTableColumnFlags_WidthStretch, 2.2f);
                        ImGui::TableHeadersRow();
                        for (const auto& localized : plan.localizations)
                        {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::TextUnformatted(localized.language.c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextWrapped("%s", localized.title.c_str());
                            ImGui::TableSetColumnIndex(2);
                            if (localized.description.empty())
                                ImGui::TextDisabled("—");
                            else
                                ImGui::TextWrapped("%s",
                                    localized.description.c_str());
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::Text("%s: %s", T("主预览", "Primary preview"),
                    plan.preview ? T("使用本地预览", "Use local preview") :
                        T("保留 Steam 当前预览", "Preserve Steam preview"));
                if (plan.preview)
                {
                    const auto key = StableKey(WideToUtf8(plan.preview->wstring()));
                    previewCache_.RequestLocal(key, *plan.preview);
                    const auto texture = previewCache_.Get(key);
                    if (texture.view)
                    {
                        const float scale = std::min(140.0f * gDpiScale / texture.width,
                            140.0f * gDpiScale / texture.height);
                        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture.view))),
                            ImVec2(texture.width * scale, texture.height * scale));
                    }
                    else if (!texture.error.empty()) ImGui::TextWrapped("%s", texture.error.c_str());
                }
                ImGui::Text("%s: %s", T("标签", "Tags"),
                    plan.tags ? T("提交本地标签", "Submit local tags") :
                        T("保留 Steam 当前标签", "Preserve Steam tags"));
                if (plan.tags)
                {
                    const std::string plannedTags = JoinTags(*plan.tags);
                    ImGui::TextWrapped("%s", plannedTags.empty() ?
                        T("（无标签）", "(no tags)") : plannedTags.c_str());
                }
            }
            const bool hasPreparedPlan = prepared != nullptr;
            const ComponentPublishAction preparedAction = hasPreparedPlan ?
                prepared->plan.action : ComponentPublishAction::Create;
            ImGui::BeginDisabled(busy_.load() || submitStarted_.load() ||
                missingManualTitle);
            if (SecondaryButton(hasPreparedPlan ?
                    T("重新准备计划", "Prepare again") :
                    T("准备发布计划", "Prepare publish plan")))
                StartPreparePublish(project.localId);
            if (hasPreparedPlan)
            {
                const char* confirm = T("确认创建项目", "Confirm creation");
                if (preparedAction == ComponentPublishAction::UpdateContent)
                    confirm = T("确认更新内容", "Confirm content update");
                else if (preparedAction == ComponentPublishAction::UpdateMetadata)
                    confirm = T("确认更新资料", "Confirm metadata update");
                ContinueButtonRow(confirm);
                const SteamStatus connection = steam_.Status();
                ImGui::BeginDisabled(!connection.initialized || !connection.loggedOn);
                if (BlueButton(confirm))
                    StartPreparedPublishUnlocked(project.localId);
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
            if (project.publishedFileId)
            {
                ContinueButtonRow(T("打开 Steam 项目管理页", "Open Steam item controls"));
                if (SecondaryButton(T("打开 Steam 项目管理页", "Open Steam item controls")))
                    OpenSteamUrlWithWebFallback(
                        SteamCommunityItemClientUrl(*project.publishedFileId),
                        CommunityItemUrl(*project.publishedFileId));
            }
            RenderOperationFeedbackUnlocked();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        PopNavigationStyle();
    }

    void StartInspect(std::string localId, bool pack)
    {
        StartWork([this, localId = std::move(localId), pack]
        {
            std::filesystem::path source;
            {
                std::lock_guard lock(mutex_);
                const auto* project = FindProjectUnlocked(localId);
                if (!project) return;
                source = project->sourceDirectory;
            }
            WidgetInspection inspection;
            std::string error;
            if (!packageTool_.Inspect(source, inspection, error))
            {
                SetMessage(false, error);
                return;
            }
            std::string success = T("组件校验通过", "Component validation passed");
            PackagedWidget artifact;
            if (pack && !packageTool_.Pack(source, inspection, artifact, error))
            {
                SetMessage(false, error);
                return;
            }
            std::filesystem::path exported;
            if (pack)
            {
                if (!ExportManagerPackage(artifact, managerRoot_ / L"exports", exported, error))
                { SetMessage(false, error); return; }
                success = T("打包完成：%s", "Package complete: %s", inspection.version.c_str());
            }
            std::lock_guard lock(mutex_);
            if (auto* project = FindProjectUnlocked(localId))
            {
                InvalidatePreparedPublishUnlocked();
                project->packageId = inspection.packageId;
                if (project->primaryPreview.empty())
                    project->primaryPreview = inspection.preview;
                if (!store_.Save(error)) SetMessageUnlocked(false, error, exported);
                else SetMessageUnlocked(true, std::move(success), exported, true);
            }
        });
    }

    void StartLocalizationPreview(std::string localId)
    {
        StartWork([this, localId = std::move(localId)]
        {
            std::filesystem::path source;
            {
                std::lock_guard lock(mutex_);
                const auto* project = FindProjectUnlocked(localId);
                if (!project) return;
                source = project->sourceDirectory;
            }
            WidgetInspection inspection;
            std::string error;
            std::vector<SteamWorkshopLocalization> preview;
            if (packageTool_.Inspect(source, inspection, error))
            {
                preview = BuildSteamWorkshopLocalizations(
                    inspection.name, inspection.description,
                    inspection.localizations);
            }
            std::lock_guard lock(mutex_);
            if (localizationPreviewProjectId_ != localId) return;
            localizationPreview_ = std::move(preview);
            localizationPreviewError_ = std::move(error);
            localizationPreviewLoading_ = false;
        }, WorkKind::TextPreview);
    }

    void StartPreparePublish(std::string localId)
    {
        preparedPublish_.reset();
        auto* current = FindProjectUnlocked(localId);
        if (!current) return;
        auto& draft = drafts_.ForProject(*current, JoinTags(current->tags));
        const std::string title = draft.manualEnglishTitle;
        const std::string description = draft.manualEnglishDescription;
        const bool syncPackageLocalization = draft.reusePackageText;
        const bool updatePreview = draft.updatePreview;
        const bool updateTags = draft.updateTags;
        const bool publishPublic = draft.publishPublic;
        const std::uint64_t inputRevision = publishInputsRevision_;
        StartWork([this, localId = std::move(localId), title, description,
                   syncPackageLocalization, updatePreview, updateTags,
                   inputRevision, publishPublic]
        {
            WorkshopProject snapshot;
            {
                std::lock_guard lock(mutex_);
                const auto* project = FindProjectUnlocked(localId);
                if (!project) return;
                snapshot = *project;
            }
            const bool creating = !snapshot.publishedFileId.has_value();
            snapshot.publishPreferences.textSource =
                syncPackageLocalization ? WorkshopTextSource::Package :
                (creating ? WorkshopTextSource::ManualEnglish :
                    WorkshopTextSource::Steam);
            snapshot.publishPreferences.previewSource = updatePreview ?
                WorkshopAssetSource::Local : WorkshopAssetSource::Steam;
            snapshot.publishPreferences.tagsSource = updateTags ?
                WorkshopAssetSource::Local : WorkshopAssetSource::Steam;
            snapshot.publishPreferences.manualEnglishTitle = title;
            snapshot.publishPreferences.manualEnglishDescription =
                description;

            WidgetInspection inspection;
            PackagedWidget artifact;
            std::string error;
            if (!packageTool_.Inspect(snapshot.sourceDirectory,
                    inspection, error) ||
                !packageTool_.Pack(snapshot.sourceDirectory,
                    inspection, artifact, error))
            {
                SetMessage(false, error);
                return;
            }
            ComponentPublishPlan plan;
            ComponentPublishOptions options;
            if (creating) options.visibility = publishPublic ? 0 : 2;
            if (!BuildComponentPublishPlan(snapshot, inspection, artifact,
                    options, plan, error))
            {
                SetMessage(false, error);
                return;
            }

            std::shared_ptr<ManagerPreparedPreview> frozenPreview;
            if (plan.preview)
            {
                frozenPreview = FreezeManagerPreview(*plan.preview, artifact.temporaryDirectory, error);
                if (!frozenPreview) { SetMessage(false, error); return; }
                plan.preview = frozenPreview->Path();
            }
            std::lock_guard lock(mutex_);
            if (publishInputsRevision_ != inputRevision ||
                selectedLocalId_ != localId)
            {
                SetMessageUnlocked(false, T(
                    "发布设置已变化，请重新准备计划",
                    "Publishing settings changed; prepare the plan again"));
                return;
            }
            auto* project = FindProjectUnlocked(localId);
            if (!project) return;
            project->packageId = inspection.packageId;
            project->publishPreferences = snapshot.publishPreferences;
            if (!store_.Save(error))
            {
                SetMessageUnlocked(false, error);
                return;
            }
            PreparedManagerPublish prepared;
            prepared.localId = localId;
            prepared.snapshot = std::move(snapshot);
            prepared.inspection = std::move(inspection);
            prepared.artifact = std::move(artifact);
            prepared.plan = std::move(plan);
            prepared.frozenPreview = std::move(frozenPreview);
            preparedPublish_ = std::move(prepared);
            RequestManagerFrame();
        }, WorkKind::Prepare);
    }

    void StartPreparedPublishUnlocked(std::string localId)
    {
        if (busy_.load() || !preparedPublish_ || preparedPublish_->localId != localId)
            return;
        PreparedManagerPublish prepared =
            std::move(*preparedPublish_);
        preparedPublish_.reset();
        publishTask_.Begin(localId, WideToUtf8(
            prepared.snapshot.sourceDirectory.filename().wstring()));
        publishInProgress_.store(true);
        taskResultDismissed_ = false;
        taskDetailsOpen_ = false;
        closeBlocked_ = false;
        StartWork([this, localId = std::move(localId),
                   prepared = std::move(prepared)]() mutable
        {
            ComponentPublishResult result;
            CoreError coreError;
            const bool published = ExecuteComponentPublishPlan(
                prepared.plan, steam_,
                [this, &localId](const ComponentPublishProgress& progress)
                {
                    const PublishProgress& value = progress.steam;
                    steam_.SampleActiveConnection();
                    std::lock_guard lock(mutex_);
                    publishTask_.Update(progress);
                    if (value.stage == PublishStage::Created)
                    {
                        std::string saveError;
                        if (auto* project = FindProjectUnlocked(localId))
                        {
                            project->publishedFileId =
                                value.publishedFileId;
                            if (!store_.Save(saveError))
                                SetMessageUnlocked(false, saveError);
                        }
                    }
                    submitStarted_.store(value.submitStarted);
                    RequestManagerFrame();
                }, result, coreError, ComponentPublishPersistence{
                    [this, &localId] {
                        std::string failure; std::lock_guard lock(mutex_);
                        const bool saved = store_.BeginCreation(localId, failure);
                        if (!saved) SetMessageUnlocked(false, failure);
                        return saved;
                    },
                    [this, &localId](std::uint64_t id) {
                        std::string failure; std::lock_guard lock(mutex_);
                        const bool saved = store_.PersistCreatedItem(localId, id, failure);
                        if (!saved) SetMessageUnlocked(false, failure);
                        return saved;
                    }});
            std::string error;
            {
                std::lock_guard lock(mutex_);
                publishTask_.Finish(result, published, coreError);
                submitStarted_.store(false);
                publishInProgress_.store(false);
                RequestManagerFrame();
                if (auto* project = FindProjectUnlocked(localId))
                {
                    if (result.publishedFileId)
                        project->publishedFileId =
                            result.publishedFileId;
                    if (result.baseSubmitted)
                    {
                        project->packageId = prepared.plan.packageId;
                        project->lastPublishedVersion =
                            prepared.plan.version;
                        project->lastPublishedSha256 =
                            prepared.plan.sha256;
                        project->lastPublishedAt = NowIso8601();
                    }
                    if (!store_.Save(error))
                    {
                        SetMessageUnlocked(false, error + (result.created && result.publishedFileId
                            ? " (Workshop PublishedFileId: " + std::to_string(result.publishedFileId) + ")" : ""));
                        return;
                    }
                }
            }
            if (!published)
            {
                if (ShowSteamRecovery(coreError)) return;
                const std::string detail = coreError.code + ": " +
                    coreError.message;
                if (!result.failedLanguage.empty())
                {
                    SetMessage(false, T(
                        "Steam 本地化 %s 提交失败：%s",
                        "Workshop localization %s failed: %s",
                        result.failedLanguage.c_str(), detail.c_str()));
                }
                else SetMessage(false, detail);
                return;
            }
            std::lock_guard lock(mutex_);
            if (localizationStateProjectId_ == localId) localizationStateCreating_ = false;
            steamConnectionProblem_ = SteamConnectionProblem::None;
            ++publishInputsRevision_;
            RequestManagerFrame();
        }, WorkKind::Publish);
    }

    const char* BindingIssueText(ManagerWorkshopBindingIssue issue) const
    {
        switch (issue)
        {
        case ManagerWorkshopBindingIssue::ConnectionUnavailable: return T("Steam 未连接", "Steam disconnected");
        case ManagerWorkshopBindingIssue::DifferentOwner: return T("只能绑定当前 Steam 账号拥有的项目。", "Only items owned by the current Steam account can be bound.");
        case ManagerWorkshopBindingIssue::AlreadyUsed: return T("此项目已绑定到另一个本地组件。", "This item is already bound to another local component.");
        case ManagerWorkshopBindingIssue::Banned: return T("已封禁", "Banned");
        case ManagerWorkshopBindingIssue::LocalIdentityUnverified:
        case ManagerWorkshopBindingIssue::IdentityMismatch: return T("没有匹配的本地组件", "No matching local component");
        default: return T("不支持的元数据", "Unsupported metadata");
        }
    }

    bool StartBindWorkshopItem(std::string localId, PublishedItem item,
        bool replacementConfirmed)
    {
        const std::string targetId = "item:" + std::to_string(item.publishedFileId);
        const std::string startingAccount = steam_.Status().steamId;
        return StartWork([this, localId = std::move(localId), item = std::move(item),
            replacementConfirmed, startingAccount]
        {
            WorkshopProject snapshot;
            {
                std::lock_guard lock(mutex_);
                const auto* project = FindProjectUnlocked(localId);
                if (!project) return;
                snapshot = *project;
            }
            WidgetInspection inspection;
            std::string error;
            if (!packageTool_.Inspect(snapshot.sourceDirectory, inspection, error))
            { SetMessage(false, error); return; }
            // Re-query the selected item before writing a durable association.
            CoreError steamError;
            const auto currentItem = steam_.FindItem(item.publishedFileId, steamError);
            CoreError samplingError;
            steam_.PollConnection(samplingError, true);
            const SteamStatus status = steam_.Status();
            if (status.steamId != startingAccount)
            { SetMessage(false, BindingIssueText(ManagerWorkshopBindingIssue::DifferentOwner)); return; }
            if (!currentItem)
            {
                if (!ShowSteamRecovery(steamError)) SetMessage(false, steamError.code + ": " + steamError.message);
                return;
            }
            std::lock_guard lock(mutex_);
            auto* project = FindProjectUnlocked(localId);
            if (!project || project->publishedFileId != snapshot.publishedFileId ||
                project->sourceDirectory != snapshot.sourceDirectory) return;
            WorkshopProject candidate = *project;
            if (candidate.packageId.empty()) candidate.packageId = inspection.packageId;
            const auto decision = EvaluateManagerWidgetBinding(candidate, inspection.packageId,
                *currentItem, status, store_.Projects(), replacementConfirmed);
            if (!decision.Allowed())
            { SetMessageUnlocked(false, BindingIssueText(decision.issue)); return; }
            const WorkshopProject previous = *project;
            project->packageId = inspection.packageId;
            project->publishedFileId = currentItem->publishedFileId;
            if (!store_.Save(error))
            { *project = previous; SetMessageUnlocked(false, error); return; }
            if (project->localId == selectedLocalId_) InvalidatePreparedPublishUnlocked();
            SetMessageUnlocked(true, T("Workshop 项目已绑定", "Workshop item bound"));
        }, WorkKind::Binding, targetId);
    }

    void RenderWorkshopBindingUnlocked(const PublishedItem& item,
        const ManagerWorkshopItemDescriptor& descriptor)
    {
        const SteamStatus status = steam_.Status();
        if (!descriptor.Complete())
        {
            ImGui::TextDisabled("%s", T("不支持的元数据", "Unsupported metadata"));
            return;
        }
        if (descriptor.kind == ManagerWorkshopItemKind::Theme)
        {
            ImGui::TextWrapped("%s", T("主题项目请在 SnowDesktop 主题管理器中管理。",
                "Theme items are managed in SnowDesktop's Theme Manager."));
            return;
        }
        if (descriptor.kind == ManagerWorkshopItemKind::IconPack)
        {
            ImGui::TextWrapped("%s", T("图标包请在 SnowDesktop 图标库中管理。",
                "Icon packs are managed in SnowDesktop's Icon Library."));
            return;
        }
        if (descriptor.kind != ManagerWorkshopItemKind::Widget || item.banned ||
            item.ownerSteamId != ParseItemId(status.steamId).value_or(0) || item.consumerAppId != status.appId)
            return;
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted(T("本地组件", "Local component"));
        std::vector<const WorkshopProject*> candidates;
        for (const auto& project : store_.Projects())
            if (project.packageId.empty() || project.packageId == descriptor.identity)
                candidates.push_back(&project);
        if (candidates.empty())
        { ImGui::TextWrapped("%s", T("没有匹配的本地组件", "No matching local component")); return; }
        auto selected = std::find_if(candidates.begin(), candidates.end(),
            [&](const auto* project) { return project->localId == bindingTargetId_; });
        if (selected == candidates.end())
        {
            selected = std::find_if(candidates.begin(), candidates.end(),
                [&](const auto* project) { return project->publishedFileId == item.publishedFileId; });
            // Only select automatically when there is a single compatible local candidate.
            if (selected == candidates.end() && candidates.size() == 1) selected = candidates.begin();
            bindingTargetId_ = selected == candidates.end() ? std::string{} : (*selected)->localId;
        }
        const auto* target = selected == candidates.end() ? nullptr : *selected;
        ImGui::BeginDisabled(busy_.load());
        ImGui::SetNextItemWidth(std::min(420.0f * gDpiScale, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##binding-target", target ?
                WideToUtf8(target->sourceDirectory.filename().wstring()).c_str() :
                T("本地组件", "Local component")))
        {
            for (const auto* candidate : candidates)
            {
                const std::string label = WideToUtf8(candidate->sourceDirectory.filename().wstring()) + "##" + candidate->localId;
                if (ImGui::Selectable(label.c_str(), bindingTargetId_ == candidate->localId))
                {
                    bindingTargetId_ = candidate->localId; target = candidate;
                    feedback_.erase({1, "item:" + std::to_string(item.publishedFileId)});
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",
                    WideToUtf8(candidate->sourceDirectory.wstring()).c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (target && target->publishedFileId == item.publishedFileId)
        {
            ImGui::TextDisabled("%s", T("已绑定 Workshop", "Workshop bound"));
            return;
        }
        const bool replacing = target && target->publishedFileId.has_value();
        const bool occupied = std::any_of(store_.Projects().begin(), store_.Projects().end(),
            [&](const WorkshopProject& project) { return target && project.localId != target->localId &&
                project.publishedFileId == item.publishedFileId; });
        if (occupied) ImGui::TextWrapped("%s", BindingIssueText(ManagerWorkshopBindingIssue::AlreadyUsed));
        ImGui::BeginDisabled(!target || occupied || busy_.load() || !status.initialized || !status.loggedOn);
        if (BlueButton(replacing ? T("替换绑定", "Replace binding") : T("绑定此组件", "Bind this component")))
        {
            if (replacing) ImGui::OpenPopup("replace-workshop-binding");
            else StartBindWorkshopItem(target->localId, item, false);
        }
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("replace-workshop-binding"))
        {
            if (target && target->publishedFileId)
            {
                ImGui::TextWrapped("%s", WideToUtf8(target->sourceDirectory.filename().wstring()).c_str());
                ImGui::Text("Workshop: %llu -> %llu", *target->publishedFileId, item.publishedFileId);
                ImGui::BeginDisabled(busy_.load() || occupied || !status.initialized || !status.loggedOn);
                if (BlueButton(T("替换绑定", "Replace binding")) &&
                    StartBindWorkshopItem(target->localId, item, true)) ImGui::CloseCurrentPopup();
                ImGui::EndDisabled();
            }
            ContinueButtonRow(T("取消", "Cancel"));
            if (SecondaryButton(T("取消", "Cancel"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void RenderWorkshop()
    {
        std::lock_guard lock(mutex_);
        const auto& page = publishedPages_.Snapshot();
        const PublishedItem* item = FindWorkshopItemUnlocked(page.selectedId);
        if (!item)
        {
            ImGui::BeginChild("workshop-empty-body", ImVec2(0, 0));
            if (page.loading) ImGui::TextDisabled("%s", T("正在加载项目…", "Loading items…"));
            else if (!steam_.Status().loggedOn) ImGui::TextWrapped("%s", T("Steam 未连接", "Steam disconnected"));
            else ImGui::TextWrapped("%s", page.loaded && page.items.empty() ?
                T("没有已发布的项目", "No published items") : T("选择一个项目", "Select an item"));
            RenderOperationFeedbackUnlocked();
            ImGui::EndChild();
            return;
        }
        ImGui::TextWrapped("%s", item->title.c_str());
        ImGui::Separator();
        ImGui::PushID(std::to_string(item->publishedFileId).c_str());
        ImGui::BeginChild("workshop-detail-body", ImVec2(0, 0));
        if (page.error || (!steam_.Status().loggedOn && page.loaded))
            ImGui::TextDisabled("%s", T("正在显示之前加载的项目", "Showing previously loaded items"));
        const auto descriptor = DescribeManagerWorkshopItem(item->metadata);
        const auto preview = [&]
        {
            if (item->previewUrl.empty()) return;
            previewCache_.Request(item->publishedFileId, item->previewUrl);
            const PreviewTexture texture = previewCache_.Get(item->publishedFileId);
            if (texture.view)
            {
                const float scale = std::min(std::min(220.0f * gDpiScale,
                    ImGui::GetContentRegionAvail().x) / texture.width,
                    220.0f * gDpiScale / texture.height);
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture.view))),
                    ImVec2(texture.width * scale, texture.height * scale));
            }
            else if (texture.loading) ImGui::TextDisabled("%s", T("正在下载主预览…", "Downloading primary preview…"));
            else if (!texture.error.empty()) ImGui::TextWrapped("%s", texture.error.c_str());
        };
        const auto details = [&]
        {
            if (ImGui::BeginTable("workshop-facts", 2, ImGuiTableFlags_SizingFixedFit |
                    ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
            {
                ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed,
                    std::min(156.0f * gDpiScale, ImGui::GetContentRegionAvail().x * 0.4f));
                ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
                const auto row = [](const char* label, const std::string& value)
                {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                    ImGui::TextWrapped("%s", label);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", value.c_str());
                };
                row(T("类型", "Type"), ItemTypeLabel(descriptor.kind));
                row(T("状态", "Status"), item->banned ? T("已封禁", "Banned") : T("正常", "Ready"));
                row("Workshop ID", std::to_string(item->publishedFileId));
                row(T("更新时间", "Updated"), FormatLocalTime(item->updatedAt));
                if (!descriptor.identity.empty()) row(descriptor.kind == ManagerWorkshopItemKind::Theme ?
                    T("主题 ID", "Theme ID") : descriptor.kind == ManagerWorkshopItemKind::IconPack ?
                    T("图标组 ID", "Icon group ID") : "Package ID", descriptor.identity);
                if (!descriptor.version.empty()) row(T("版本", "Version"), descriptor.version);
                row(T("订阅", "Subs"), std::to_string(item->subscriptions));
                row(T("收藏", "Favs"), std::to_string(item->favorites));
                row(T("浏览", "Views"), std::to_string(item->views));
                row(T("评论", "Comments"), std::to_string(item->comments));
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (SecondaryButton(T("打开 Steam 项目管理页", "Open Steam item controls")))
                OpenSteamUrlWithWebFallback(SteamCommunityItemClientUrl(item->publishedFileId), CommunityItemUrl(item->publishedFileId));
            RenderWorkshopBindingUnlocked(*item, descriptor);
        };
        if (!item->previewUrl.empty() && ImGui::GetContentRegionAvail().x >= 540.0f * gDpiScale &&
            ImGui::BeginTable("workshop-overview", 2, ImGuiTableFlags_SizingFixedFit |
                ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoPadOuterX))
        {
            ImGui::TableSetupColumn("preview", ImGuiTableColumnFlags_WidthFixed, 220.0f * gDpiScale);
            ImGui::TableSetupColumn("details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); preview();
            ImGui::TableSetColumnIndex(1); details(); ImGui::EndTable();
        }
        else { preview(); ImGui::Spacing(); details(); }
        RenderOperationFeedbackUnlocked();
        ImGui::EndChild();
        ImGui::PopID();
    }

    void RefreshPublished(std::uint32_t page)
    {
        if (busy_.load()) return;
        ManagerPublishedPageRequest request;
        {
            std::lock_guard lock(mutex_);
            request = publishedPages_.BeginRequest(steam_.Status().steamId, page);
        }
        StartWork([this, request]() mutable
        {
            // A shrinking last page requires a real fetch of the new last page.
            // Bound repeated churn, retaining the previous committed view on failure.
            for (unsigned attempt = 0; attempt != 3; ++attempt)
            {
                CoreError error;
                auto result = steam_.ListPublished(request.page, error);
                CoreError samplingError;
                if (!steam_.PollConnection(samplingError, true) && result)
                {
                    result.reset();
                    error = std::move(samplingError);
                }
                const SteamStatus status = steam_.Status();
                if (!result)
                {
                    {
                        std::lock_guard lock(mutex_);
                        if (!publishedPages_.Fail(request, status.steamId, error)) return;
                    }
                    if (!ShowSteamRecovery(error)) SetMessage(false, error.code + ": " + error.message);
                    return;
                }
                ManagerPublishedPageApply applied;
                {
                    std::lock_guard lock(mutex_);
                    const auto previousSelection = publishedPages_.Snapshot().selectedId;
                    applied = publishedPages_.Apply(request, status.steamId, std::move(*result));
                    if (applied.kind == ManagerPublishedPageApplyKind::RetryAtPage)
                        request = publishedPages_.BeginRequest(status.steamId, applied.retryPage);
                    else if (applied.kind == ManagerPublishedPageApplyKind::Applied)
                    {
                        steamConnectionProblem_ = SteamConnectionProblem::None;
                        feedback_.erase({1, {}});
                        if (previousSelection != publishedPages_.Snapshot().selectedId) bindingTargetId_.clear();
                    }
                    else if (applied.kind == ManagerPublishedPageApplyKind::InvalidResponse)
                        SetMessageUnlocked(false, publishedPages_.Snapshot().error->message);
                }
                if (applied.kind != ManagerPublishedPageApplyKind::RetryAtPage) return;
            }
            const CoreError error{kSteamOperationFailed, "published_page_changed", "Steam published pages changed repeatedly; refresh again"};
            {
                std::lock_guard lock(mutex_);
                publishedPages_.Fail(request, steam_.Status().steamId, error);
            }
            SetMessage(false, error.message);
        }, WorkKind::Published);
    }

    ManagerLocalization localization_;
    std::filesystem::path managerRoot_;
    std::filesystem::path developmentRoot_;
    std::filesystem::path projectDirectory_;
    std::filesystem::path settingsFile_;
    std::string currentLanguage_;
    int activePage_ = 0;
    int requestedPage_ = -1;
    std::optional<std::filesystem::file_time_type> settingsWriteTime_;
    std::uint64_t observedConnectionRevision_ = 0;
    ProjectStore store_;
    PackageTool packageTool_;
    PreviewCache previewCache_;
    SteamWorkshopCore steam_;
    mutable std::mutex mutex_;
    std::mutex workMutex_;
    std::condition_variable_any workChanged_;
    std::function<void()> pendingWork_;
    WorkTarget runningWork_;
    std::jthread worker_;
    std::atomic_bool connectionCheckRequested_ = false;
    std::atomic_bool publishedRefreshRequested_ = false;
    std::atomic_bool busy_ = false;
    std::atomic_bool submitStarted_ = false;
    std::atomic_bool publishInProgress_ = false;
    ManagerPublishTask publishTask_;
    bool taskDetailsOpen_ = false;
    bool taskResultDismissed_ = false;
    bool taskPanelUserSized_ = false;
    float taskPanelHeightDip_ = 104.0f;
    float taskPanelNaturalHeightDip_ = 0.0f;
    bool closeBlocked_ = false;
    int requestedProjectTab_ = -1;
    std::string selectedLocalId_;
    ManagerPublishedPages publishedPages_;
    ManagerProjectDrafts drafts_;
    std::string bindingTargetId_;
    char localSearch_[256]{};
    char publishedSearch_[256]{};
    std::optional<PreparedManagerPublish> preparedPublish_;
    std::uint64_t publishInputsRevision_ = 0;
    std::string localizationStateProjectId_;
    std::string localizationPreviewProjectId_;
    std::vector<SteamWorkshopLocalization> localizationPreview_;
    std::string localizationPreviewError_;
    bool localizationStateCreating_ = false;
    bool localizationPreviewLoading_ = false;
    SteamConnectionProblem steamConnectionProblem_ = SteamConnectionProblem::None;
    std::map<std::pair<int, std::string>, Feedback> feedback_;
};

WorkshopManagerApp* gApp = nullptr;
bool gRenderingFrame = false;
bool gSwapChainOccluded = false;
UINT gInteractiveFrameInterval = 0;

void SetInteractiveFrameTimer(HWND window, UINT interval)
{
    if (gInteractiveFrameInterval == interval) return;
    if (gInteractiveFrameInterval != 0)
        KillTimer(window, kManagerInteractiveFrameTimer);
    gInteractiveFrameInterval = 0;
    if (interval != 0 &&
        SetTimer(window, kManagerInteractiveFrameTimer, interval, nullptr) != 0)
        gInteractiveFrameInterval = interval;
}

void UpdateInteractiveFrameTimer(HWND window)
{
    const ImGuiIO& io = ImGui::GetIO();
    bool mouseDown = false;
    for (const bool down : io.MouseDown) mouseDown = mouseDown || down;
    const ManagerInteractiveFrameState state{
        GetForegroundWindow() == window,
        IsWindowVisible(window) != FALSE,
        IsIconic(window) != FALSE,
        mouseDown,
        io.WantTextInput,
    };
    SetInteractiveFrameTimer(window,
        ManagerInteractiveFrameInterval(state));
}

void RenderManagerFrame(HWND window)
{
    if (!gApp || gRenderingFrame || !ImGui::GetCurrentContext() ||
        !gDevice.Get() || !gContext.Get() || !gSwapChain.Get() ||
        !gRenderTarget.Get())
        return;
    const bool canRender = IsWindowVisible(window) && !IsIconic(window) &&
        !gSwapChainOccluded;
    if (!gFrameScheduler.BeginFrame(canRender)) return;

    gRenderingFrame = true;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    gApp->Render(window);
    ImGui::Render();
    const float clear[4] = { 0.96f, 0.96f, 0.97f, 1.0f };
    gContext->OMSetRenderTargets(1, gRenderTarget.GetAddressOf(), nullptr);
    gContext->ClearRenderTargetView(gRenderTarget.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT presented = gSwapChain->Present(1, 0);
    gRenderingFrame = false;

    if (presented == DXGI_STATUS_OCCLUDED)
    {
        gSwapChainOccluded = true;
        gFrameScheduler.RequestFrame();
        SetInteractiveFrameTimer(window, 0);
        if (SetTimer(window, kManagerOcclusionProbeTimer,
                250, nullptr) == 0)
            gSwapChainOccluded = false;
        return;
    }
    UpdateInteractiveFrameTimer(window);
    WakeManagerForPendingFrame();
}

bool CreateDevice(HWND window)
{
    DXGI_SWAP_CHAIN_DESC swap{};
    swap.BufferCount = 2;
    swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.OutputWindow = window;
    swap.SampleDesc.Count = 1;
    swap.Windowed = TRUE;
    swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL selected{};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr,
            D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
            static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &swap,
            &gSwapChain, &gDevice, &selected, &gContext)))
        return false;
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        return false;
    return SUCCEEDED(gDevice->CreateRenderTargetView(
        backBuffer.Get(), nullptr, &gRenderTarget));
}

void RecreateRenderTarget()
{
    gRenderTarget.Reset();
    ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        gDevice->CreateRenderTargetView(backBuffer.Get(), nullptr,
            &gRenderTarget);
}

bool IsManagerInteractionMessage(UINT message)
{
    switch (message)
    {
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_CHAR:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_SYSCHAR:
    case WM_INPUTLANGCHANGE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_CAPTURECHANGED:
    case WM_DROPFILES:
    case WM_SIZE:
    case WM_DPICHANGED:
    case WM_PAINT:
        return true;
    default:
        return false;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    if (IsManagerInteractionMessage(message)) RequestManagerFrame();
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return TRUE;
    switch (message)
    {
    case WM_ACTIVATE:
        if (gApp && LOWORD(wParam) != WA_INACTIVE) gApp->CheckConnection();
        break;
    case WM_GETMINMAXINFO:
        if (auto* limits = reinterpret_cast<MINMAXINFO*>(lParam))
        {
            const float scale = std::max(1.0f,
                static_cast<float>(GetDpiForWindow(window)) / 96.0f);
            MONITORINFO monitor{ sizeof(monitor) };
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            {
                limits->ptMinTrackSize.x = std::min(static_cast<LONG>(900.0f * scale),
                    monitor.rcWork.right - monitor.rcWork.left);
                limits->ptMinTrackSize.y = std::min(static_cast<LONG>(620.0f * scale),
                    monitor.rcWork.bottom - monitor.rcWork.top);
            }
        }
        return 0;
    case kRequestFrameMessage:
        gFrameScheduler.AcknowledgeWake();
        return 0;
    case WM_DROPFILES:
        if (gApp)
        {
            const HDROP drop = reinterpret_cast<HDROP>(wParam);
            wchar_t path[32768]{};
            if (DragQueryFileW(drop, 0, path,
                    static_cast<UINT>(std::size(path))))
                gApp->AddDroppedDirectory(path);
            DragFinish(drop);
        }
        return 0;
    case WM_SIZE:
        if (gSwapChain && wParam != SIZE_MINIMIZED)
        {
            gRenderTarget.Reset();
            gSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            RecreateRenderTarget();
            gSwapChainOccluded = false;
            KillTimer(window, kManagerOcclusionProbeTimer);
            RenderManagerFrame(window);
        }
        else if (wParam == SIZE_MINIMIZED)
        {
            SetInteractiveFrameTimer(window, 0);
            KillTimer(window, kManagerOcclusionProbeTimer);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        SetTimer(window, kManagerLiveResizeTimer, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(window, kManagerLiveResizeTimer);
        RequestManagerFrame();
        RenderManagerFrame(window);
        return 0;
    case WM_TIMER:
        switch (ClassifyManagerTimer(
            static_cast<std::uintptr_t>(wParam)))
        {
        case ManagerTimerAction::RenderFrame:
            RequestManagerFrame();
            RenderManagerFrame(window);
            return 0;
        case ManagerTimerAction::PollLanguage:
            if (gApp && !gRenderingFrame)
                gApp->PollMainSettings(window);
            return 0;
        case ManagerTimerAction::RequestFrame:
            if (gRenderingFrame || GetForegroundWindow() != window)
            {
                SetInteractiveFrameTimer(window, 0);
                return 0;
            }
            RequestManagerFrame();
            return 0;
        case ManagerTimerAction::ProbeOcclusion:
            if (gSwapChainOccluded && gSwapChain && !IsIconic(window))
            {
                const HRESULT result = gSwapChain->Present(
                    0, DXGI_PRESENT_TEST);
                if (result != DXGI_STATUS_OCCLUDED && SUCCEEDED(result))
                {
                    gSwapChainOccluded = false;
                    KillTimer(window, kManagerOcclusionProbeTimer);
                    RequestManagerFrame();
                }
            }
            return 0;
        case ManagerTimerAction::None:
            break;
        }
        break;
    case WM_DPICHANGED:
        if (const RECT* rect = reinterpret_cast<const RECT*>(lParam))
            SetWindowPos(window, nullptr, rect->left, rect->top,
                rect->right - rect->left, rect->bottom - rect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        if (gApp && !gApp->CanClose())
        {
            gApp->ExplainCloseBlocked();
            RequestManagerFrame();
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        SetInteractiveFrameTimer(window, 0);
        KillTimer(window, kManagerLiveResizeTimer);
        KillTimer(window, kManagerLanguagePollTimer);
        KillTimer(window, kManagerOcclusionProbeTimer);
        gManagerWindow.store(nullptr, std::memory_order_release);
        gManagerThreadId.store(0, std::memory_order_release);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

bool IsPlainFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_DIRECTORY |
            FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)) == 0;
}

std::filesystem::path DefaultDataDirectory()
{
    const std::filesystem::path executableDirectory = ExecutableDirectory();
    if (executableDirectory.empty()) return {};

    // Steam launch options and old shortcuts have sometimes pointed directly
    // at an executable under distribution/ or a versioned runtime.  Keep the
    // no-argument Manager compatible without writing mutable data into either
    // immutable payload tree.
    const std::filesystem::path distributionRoot =
        executableDirectory.parent_path();
    if (executableDirectory.filename() == L"distribution" &&
        IsPlainFile(distributionRoot / L"SnowDesktopLauncher.exe") &&
        IsPlainFile(distributionRoot / L"SnowDesktop.steam.json"))
    {
        return distributionRoot / L"data";
    }

    const std::filesystem::path runtimeRoot =
        executableDirectory.parent_path().parent_path().parent_path();
    if (IsPlainFile(executableDirectory /
            L"SnowDesktop.runtime-context.json") &&
        IsPlainFile(runtimeRoot / L"SnowDesktopLauncher.exe") &&
        IsPlainFile(runtimeRoot / L"SnowDesktop.steam.json"))
    {
        return runtimeRoot / L"data";
    }
    return executableDirectory / L"data";
}

ManagerArguments ReadArguments()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    ManagerArguments result;
    result.language = SystemLanguage();
    bool languageSpecified = false;
    bool settingsSpecified = false;
    if (!arguments) return result;
    for (int index = 1; index + 1 < count; ++index)
    {
        if (std::wstring_view(arguments[index]) == L"--development-root")
        {
            result.developmentRoot = arguments[++index];
        }
        else if (std::wstring_view(arguments[index]) == L"--data-directory")
        {
            result.dataDirectory = arguments[++index];
        }
        else if (std::wstring_view(arguments[index]) ==
            L"--project-directory")
        {
            result.projectDirectory = arguments[++index];
        }
        else if (std::wstring_view(arguments[index]) == L"--language")
        {
            result.language = WideToUtf8(arguments[++index]);
            languageSpecified = true;
        }
        else if (std::wstring_view(arguments[index]) == L"--settings-file")
        {
            result.settingsFile = arguments[++index];
            settingsSpecified = true;
        }
    }
    LocalFree(arguments);
    if (result.dataDirectory.empty())
        result.dataDirectory = DefaultDataDirectory();
    if (result.developmentRoot.empty())
        result.developmentRoot = result.dataDirectory /
            L"widgets" / L"dev";
    if (!settingsSpecified && !languageSpecified)
        result.settingsFile = result.dataDirectory /
            L"SnowDesktop.general.json";
    if (!result.settingsFile.empty())
    {
        if (const auto configured =
                ReadMainLanguageSetting(result.settingsFile))
            result.language = *configured;
    }
    return result;
}
}

namespace
{
void ShowManagerStartupFailure(const wchar_t* stage, DWORD error)
{
    snowdesktop::steam_bridge::ManagerLocalization localization;
    std::string ignored;
    localization.Load(ExecutableDirectory() / L"lang", ReadArguments().language, ignored);
    std::wstring message = Utf8ToWide(localization.Translate(
        "SnowDesktop could not start. See the details below.",
        "SnowDesktop 无法启动，请查看下方详情。"));
    message += L"\n\n" + std::wstring(stage) + L"\n" + std::to_wstring(error);
    wchar_t description[1024]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, description, static_cast<DWORD>(std::size(description)), nullptr);
    message += L"\n" + std::wstring(description);
    MessageBoxW(nullptr, message.c_str(), L"SnowDesktop Workshop Manager", MB_OK | MB_ICONERROR);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    if (!EnsureExpectedSteamEnvironmentIfMissing())
    {
        MessageBoxW(nullptr,
            L"Unable to create the local Steam App ID context.",
            L"SnowDesktop Workshop Manager", MB_OK | MB_ICONERROR);
        return 1;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { ShowManagerStartupFailure(L"COM", static_cast<DWORD>(com)); return 1; }
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE,
        L"Local\\SnowDesktopWorkshopManager");
    const DWORD mutexError = GetLastError();
    if (!singleInstance)
    {
        ShowManagerStartupFailure(L"CreateMutex", mutexError);
        CoUninitialize();
        return 1;
    }
    if (mutexError == ERROR_ALREADY_EXISTS)
    {
        if (singleInstance) CloseHandle(singleInstance);
        MessageBoxW(nullptr, L"SnowDesktop Workshop Manager is already open.",
            L"SnowDesktop Workshop Manager", MB_OK | MB_ICONINFORMATION);
        CoUninitialize();
        return 0;
    }
    const wchar_t* className = L"SnowDesktopWorkshopManagerWindow";
    HICON largeIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    HICON smallIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_APPICON_SMALL), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    WNDCLASSEXW windowClass{ sizeof(windowClass), CS_CLASSDC,
        WindowProcedure, 0, 0, instance, largeIcon,
        LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, className,
        smallIcon };
    RegisterClassExW(&windowClass);
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, className,
        L"SnowDesktop Workshop Creator Manager",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
        nullptr, nullptr, instance, nullptr);
    if (!window || !CreateDevice(window))
    {
        const DWORD error = window ? ERROR_GEN_FAILURE : GetLastError();
        ShowManagerStartupFailure(window ? L"Direct3D" : L"CreateWindow", error);
        if (window) DestroyWindow(window);
        UnregisterClassW(className, instance);
        CloseHandle(singleInstance);
        CoUninitialize();
        return 1;
    }
    DragAcceptFiles(window, TRUE);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const UINT dpi = GetDpiForWindow(window);
    const float scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
    gDpiScale = scale;
    MONITORINFO monitor{ sizeof(monitor) };
    RECT initialBounds{};
    if (GetWindowRect(window, &initialBounds) && GetMonitorInfoW(
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        const LONG width = std::min(static_cast<LONG>(1180.0f * scale),
            monitor.rcWork.right - monitor.rcWork.left);
        const LONG height = std::min(static_cast<LONG>(760.0f * scale),
            monitor.rcWork.bottom - monitor.rcWork.top);
        SetWindowPos(window, nullptr,
            std::clamp(initialBounds.left, monitor.rcWork.left, monitor.rcWork.right - width),
            std::clamp(initialBounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height),
            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetupLightTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 5.0f * scale;
    wchar_t windowsDirectory[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windowsDirectory,
            static_cast<UINT>(std::size(windowsDirectory))))
        wcscpy_s(windowsDirectory, L"C:\\Windows");
    const std::filesystem::path fonts =
        std::filesystem::path(windowsDirectory) / L"Fonts";
    const auto chineseFont = fonts / L"msyh.ttc";
    const auto koreanFont = fonts / L"malgun.ttf";
    const auto regularFont = fonts / L"segoeui.ttf";
    const auto font = std::filesystem::exists(chineseFont) ?
        chineseFont : regularFont;
    ImFont* managerFont = io.Fonts->AddFontFromFileTTF(
        WideToUtf8(font.wstring()).c_str(),
        17.0f * scale, nullptr, io.Fonts->GetGlyphRangesChineseFull());
    if (!managerFont) managerFont = io.Fonts->AddFontDefault();
    if (managerFont && std::filesystem::exists(koreanFont))
    {
        ImFontConfig koreanConfig{};
        koreanConfig.MergeMode = true;
        koreanConfig.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(
            WideToUtf8(koreanFont.wstring()).c_str(),
            17.0f * scale, &koreanConfig,
            io.Fonts->GetGlyphRangesKorean());
    }
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(gDevice.Get(), gContext.Get());
    gManagerThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    gManagerWindow.store(window, std::memory_order_release);
    WorkshopManagerApp app(ReadArguments(), ExecutableDirectory() / L"lang");
    const std::wstring localizedWindowTitle = Utf8ToWide(app.WindowTitle());
    SetWindowTextW(window, localizedWindowTitle.c_str());
    gApp = &app;
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    SetTimer(window, kManagerLanguagePollTimer, 1000, nullptr);
    gFrameScheduler.RequestFrame();
    WakeManagerForPendingFrame();
    bool messageLoopFailed = false;
    MSG message{};
    for (;;)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) break;
        if (result == -1)
        {
            ShowManagerStartupFailure(L"GetMessage", GetLastError());
            messageLoopFailed = true;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        RenderManagerFrame(window);
    }
    gManagerWindow.store(nullptr, std::memory_order_release);
    gManagerThreadId.store(0, std::memory_order_release);
    gApp = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    gRenderTarget.Reset();
    gSwapChain.Reset();
    gContext.Reset();
    gDevice.Reset();
    if (IsWindow(window)) DestroyWindow(window);
    UnregisterClassW(className, instance);
    if (smallIcon) DestroyIcon(smallIcon);
    CloseHandle(singleInstance);
    CoUninitialize();
    return messageLoopFailed ? 1 : 0;
}
