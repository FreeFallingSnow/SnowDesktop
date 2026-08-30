// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include "bridge_json.h"
#include "manager_localization.h"
#include "package_tool.h"
#include "preview_cache.h"
#include "steam_app_identity.h"
#include "steam_workshop_core.h"
#include "workshop_localization.h"
#include "workshop_project.h"
#include "resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

constexpr float kSidebarWidthDip = 176.0f;
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

float BeginSettingRow(const char* label, float controlWidth)
{
    const float rowStart = ImGui::GetCursorPosX();
    const float rowRight = rowStart + ImGui::GetContentRegionAvail().x;
    const float controlX = std::max(rowStart,
        rowRight - std::max(1.0f, controlWidth));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(controlX);
    return controlX;
}

float ButtonWidth(const char* label)
{
    return ImGui::CalcTextSize(label).x +
        ImGui::GetStyle().FramePadding.x * 2.0f;
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
          previewCache_(managerRoot_ / L"preview-cache"),
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
        if (const DWORD attributes = GetFileAttributesW(stagingRoot.c_str());
            attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            SetMessage(false, "Workshop Manager staging directory is unsafe");
        }
        else
        {
            std::filesystem::remove_all(stagingRoot, directoryError);
            if (!directoryError)
                std::filesystem::create_directories(
                    stagingRoot, directoryError);
            if (directoryError)
                SetMessage(false,
                    "cannot reset Workshop Manager data staging directory");
        }
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
        if (steam_.Status().compiled) RefreshPublished(1);
    }

    ~WorkshopManagerApp()
    {
        worker_ = std::jthread{};
    }

    bool CanClose() const
    {
        return !submitStarted_.load();
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
            selectedLocalId_ = project->localId;
            activePage_ = 0;
            SetMessageUnlocked(true, T("已添加本地项目", "Local project added"));
        }
        else SetMessageUnlocked(false, error);
    }

    void Render(HWND window)
    {
        RefreshLanguageFromMainSettings(window);
        previewCache_.Pump(gDevice.Get());
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("SnowDesktop Workshop Manager", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        RenderHeader(window);
        ImGui::Spacing();
        const float sidebarPadding = 8.0f * gDpiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(sidebarPadding, sidebarPadding));
        ImGui::BeginChild("manager-sidebar",
            ImVec2(kSidebarWidthDip * gDpiScale, 0),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        RenderSidebar();
        ImGui::EndChild();
        ImGui::SameLine();
        const float contentPadding = 16.0f * gDpiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
            ImVec2(contentPadding, contentPadding));
        ImGui::BeginChild("manager-content", ImVec2(0, 0),
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::PopStyleVar();
        switch (activePage_)
        {
        case 0: RenderLocalProjects(window); break;
        case 1: RenderWorkshop(); break;
        default: activePage_ = 0; break;
        }
        ImGui::EndChild();
        ImGui::End();
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

    PublishedItem* FindWorkshopItemUnlocked(std::uint64_t id)
    {
        const auto found = std::find_if(published_.begin(), published_.end(),
            [&](const PublishedItem& item)
            { return item.publishedFileId == id; });
        return found == published_.end() ? nullptr : &*found;
    }

    void SetMessage(bool success, std::string value)
    {
        std::lock_guard lock(mutex_);
        SetMessageUnlocked(success, std::move(value));
    }

    void SetMessageUnlocked(bool success, std::string value)
    {
        messageSuccess_ = success;
        message_ = std::move(value);
    }

    template<typename Work>
    void StartWork(Work&& work)
    {
        if (busy_.exchange(true)) return;
        if (worker_.joinable()) worker_.join();
        worker_ = std::jthread([this, task = std::forward<Work>(work)]
            (std::stop_token) mutable
        {
            task();
            busy_.store(false);
        });
    }

    void RenderHeader(HWND window)
    {
        (void)window;
        ImGui::TextUnformatted(T("SnowDesktop 创意工坊创作者管理器",
            "SnowDesktop Workshop Creator Manager"));
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", SNOWDESKTOP_VERSION);
        ImGui::SameLine();
        const SteamStatus steamStatus = steam_.Status();
        ImGui::TextDisabled("%s", steamStatus.initialized && steamStatus.loggedOn
            ? T("Steam 已连接", "Steam connected")
            : T("Steam 未连接", "Steam disconnected"));
        std::lock_guard lock(mutex_);
        if (!message_.empty())
        {
            const ImVec4 color = messageSuccess_ ?
                ImVec4(0.35f, 0.85f, 0.52f, 1.0f) :
                ImVec4(1.0f, 0.42f, 0.38f, 1.0f);
            ImGui::TextColored(color, "%s", message_.c_str());
        }
        if (busy_.load())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T("正在处理…", "Working…"));
        }
    }

    void RenderSidebar()
    {
        if (ImGui::Selectable((std::string(T("发布组件", "Publish component")) +
                "###PublishComponentPage").c_str(), activePage_ == 0))
            activePage_ = 0;
        if (ImGui::Selectable((std::string(T("我的 Workshop", "My Workshop")) +
                "###MyWorkshopPage").c_str(), activePage_ == 1))
            activePage_ = 1;
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (SecondaryButton(T("打开创意工坊", "Open Workshop"),
                ImVec2(-1, 0)))
            OpenSteamUrlWithWebFallback(
                SteamWorkshopClientUrl(), SteamWorkshopHomeUrl());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("本地项目", "Local Projects"));
        ImGui::Spacing();

        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##local-search",
            T("搜索名称或路径", "Search name or path"),
            localSearch_, std::size(localSearch_));
        ImGui::Spacing();
        std::lock_guard lock(mutex_);
        if (store_.Projects().empty())
        {
            ImGui::TextWrapped("%s", T(
                "添加包含 widget.json 的组件目录，或把目录拖入此窗口。",
                "Add a component directory containing widget.json, or drop it onto this window."));
        }
        else
        {
            for (const auto& project : store_.Projects())
            {
                const std::string name = WideToUtf8(
                    project.sourceDirectory.filename().wstring());
                const std::string path = WideToUtf8(
                    project.sourceDirectory.wstring());
                const std::string haystack = name + " " +
                    project.packageId + " " + project.localId + " " + path;
                if (localSearch_[0] && haystack.find(localSearch_) ==
                    std::string::npos)
                    continue;
                if (ImGui::Selectable((name + "##" +
                        project.localId).c_str(),
                        selectedLocalId_ == project.localId))
                {
                    selectedLocalId_ = project.localId;
                    tagsBuffer_ = JoinTags(project.tags);
                    titleBuffer_.clear();
                    descriptionBuffer_.clear();
                    localizationStateProjectId_.clear();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", path.c_str());
            }
        }
    }

    void RenderLocalProjects(HWND window)
    {
        ImGui::SeparatorText(T("发布组件", "Publish component"));
        ImGui::Spacing();
        const char* openRoot = T("打开目录", "Open folder");
        const std::string openRootButton =
            std::string(openRoot) + "###OpenDevelopmentRoot";
        const char* addFolder = T("添加组件目录", "Add component folder");
        if (SecondaryButton(openRootButton.c_str()))
            OpenDirectory(developmentRoot_);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy_.load());
        if (BlueButton(addFolder))
        {
            if (const auto path = PickPath(window, true)) AddDroppedDirectory(*path);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", WideToUtf8(developmentRoot_.wstring()).c_str());
        ImGui::Spacing();
        std::lock_guard lock(mutex_);
        if (store_.Projects().empty())
        {
            ImGui::TextWrapped("%s", T(
                "添加包含 widget.json 的组件目录，或把目录拖入此窗口。",
                "Add a component directory containing widget.json, or drop it onto this window."));
            return;
        }
        WorkshopProject* project = FindProjectUnlocked(selectedLocalId_);
        if (!project)
        {
            ImGui::TextWrapped("%s", T(
                "添加包含 widget.json 的组件目录，或把目录拖入此窗口。",
                "Add a component directory containing widget.json, or drop it onto this window."));
            return;
        }
        ImGui::PushID(project->localId.c_str());
        RenderProjectDetailsUnlocked(window, *project);
        ImGui::PopID();
    }

    void RefreshLanguageFromMainSettings(HWND window)
    {
        if (settingsFile_.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextLanguageCheck_) return;
        nextLanguageCheck_ = now + std::chrono::seconds(1);

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
    }

    void RenderProjectDetailsUnlocked(HWND window, WorkshopProject& project)
    {
        ImGui::SeparatorText(WideToUtf8(
            project.sourceDirectory.filename().wstring()).c_str());
        ImGui::TextWrapped("%s", WideToUtf8(project.sourceDirectory.wstring()).c_str());
        ImGui::Spacing();
        const char* openLabel = T("打开目录", "Open folder");
        const std::string openProjectButton =
            std::string(openLabel) + "###OpenProjectDirectory";
        const char* validateLabel = T("校验", "Validate");
        const char* packLabel = T("校验并打包", "Validate and pack");
        if (SecondaryButton(openProjectButton.c_str()))
            OpenDirectory(project.sourceDirectory);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy_.load());
        if (SecondaryButton(validateLabel))
        {
            const std::string id = project.localId;
            StartInspect(id, false);
        }
        ImGui::SameLine();
        if (BlueButton(packLabel))
        {
            const std::string id = project.localId;
            StartInspect(id, true);
        }
        ImGui::EndDisabled();
        if (!project.lastPublishedVersion.empty())
        {
            BeginSettingRow(T("最后发布版本", "Last published version"),
                kSettingControlWidthDip * gDpiScale);
            ImGui::TextDisabled("%s",
                project.lastPublishedVersion.c_str());
        }

        ImGui::Spacing();
        ImGui::SeparatorText(T("主预览", "Primary preview"));
        ImGui::TextDisabled("%s", project.primaryPreview.empty() ?
            T("未绑定", "Not bound") :
            WideToUtf8(project.primaryPreview.wstring()).c_str());
        if (!project.primaryPreview.empty())
        {
            const std::uint64_t previewKey = StableKey(project.localId);
            previewCache_.RequestLocal(previewKey, project.primaryPreview);
            const PreviewTexture texture = previewCache_.Get(previewKey);
            if (texture.view)
            {
                const float scale = std::min(
                    (360.0f * gDpiScale) / texture.width,
                    (180.0f * gDpiScale) / texture.height);
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(texture.view))),
                    ImVec2(texture.width * scale, texture.height * scale));
            }
            else if (!texture.error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                    "%s", texture.error.c_str());
        }
        if (SecondaryButton(T("选择主预览", "Choose primary preview")))
        {
            if (const auto path = PickPath(window, false))
            {
                project.primaryPreview = *path;
                std::string error;
                if (!store_.Save(error)) SetMessageUnlocked(false, error);
                else SetMessageUnlocked(true,
                    T("已保存主预览", "Primary preview saved"));
            }
        }
        if (tagsBuffer_.empty() && !project.tags.empty())
            tagsBuffer_ = JoinTags(project.tags);
        std::array<char, 1024> tags{};
        std::copy_n(tagsBuffer_.c_str(),
            std::min(tagsBuffer_.size(), tags.size() - 1), tags.data());
        BeginSettingRow(T("标签（逗号分隔）", "Tags (comma-separated)"),
            kSettingControlWidthDip * gDpiScale);
        ImGui::SetNextItemWidth(kSettingControlWidthDip * gDpiScale);
        if (ImGui::InputText("##project-tags", tags.data(), tags.size()))
            tagsBuffer_ = tags.data();
        BeginSettingRow("", ButtonWidth(T("保存标签", "Save tags")));
        if (SecondaryButton(T("保存标签", "Save tags")))
        {
            project.tags = SplitTags(tagsBuffer_);
            std::string error;
            if (!store_.Save(error)) SetMessageUnlocked(false, error);
            else SetMessageUnlocked(true, T("标签已保存", "Tags saved"));
        }
        const bool creating = !project.publishedFileId.has_value();
        if (localizationStateProjectId_ != project.localId ||
            localizationStateCreating_ != creating)
        {
            localizationStateProjectId_ = project.localId;
            localizationStateCreating_ = creating;
            syncPackageLocalization_ =
                project.publishPreferences.textSource ==
                WorkshopTextSource::Package;
            updatePreview_ = project.publishPreferences.previewSource ==
                WorkshopAssetSource::Local;
            updateTags_ = project.publishPreferences.tagsSource ==
                WorkshopAssetSource::Local;
            titleBuffer_ =
                project.publishPreferences.manualEnglishTitle;
            descriptionBuffer_ =
                project.publishPreferences.manualEnglishDescription;
            localizationPreviewProjectId_.clear();
            localizationPreview_.clear();
            localizationPreviewError_.clear();
            localizationPreviewLoading_ = false;
        }
        ImGui::SeparatorText(T("发布", "Publish"));
        if (creating)
        {
            ImGui::TextWrapped("%s", T(
                "首次发布默认复用组件包内的多语言标题和说明，并以私有状态创建项目。关闭复用后可手动填写英文回退文案。",
                "Creation uses the component package's localized titles and descriptions by default and creates the item private. Disable reuse to enter an English fallback manually."));
        }
        else
        {
            ImGui::TextWrapped("%s", T(
                "更新默认保留网页端标题和说明。开启包内文案复用后，会同步组件包中 Steam 支持的全部本地化。",
                "Updates preserve web-managed titles and descriptions by default. Enable package reuse to synchronize every supported localization from the component package."));
        }
        BeginSettingRow(T(
            "复用组件包内的标题和说明",
            "Reuse component package titles and descriptions"),
            ImGui::GetFrameHeight());
        if (ImGui::Checkbox("##package-localization",
                &syncPackageLocalization_))
        {
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
            ImGui::BeginDisabled(busy_.load());
            if (SecondaryButton(T(
                    "刷新文案预览", "Refresh text preview")))
            {
                localizationPreviewProjectId_ = project.localId;
                localizationPreview_.clear();
                localizationPreviewError_.clear();
                localizationPreviewLoading_ = true;
                StartLocalizationPreview(project.localId);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", T(
                "发布时会重新读取组件包内的最新文案",
                "The latest package text is read again when publishing"));
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
                const std::string previewLabel = T(
                    "多语言文案预览（%zu 种 Steam 语言）",
                    "Localized text preview (%zu Steam languages)",
                    localizationPreview_.size());
                if (ImGui::CollapsingHeader(previewLabel.c_str(),
                        ImGuiTreeNodeFlags_DefaultOpen) &&
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
                kSettingControlWidthDip * gDpiScale);
            ImGui::SetNextItemWidth(kSettingControlWidthDip * gDpiScale);
            if (ImGui::InputText("##initial-title", title.data(), title.size()))
                titleBuffer_ = title.data();
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
                kSettingControlWidthDip * gDpiScale);
            if (ImGui::InputTextMultiline("##initial-description",
                    description.data(), description.size(),
                    ImVec2(kSettingControlWidthDip * gDpiScale,
                        90.0f * gDpiScale)))
                descriptionBuffer_ = description.data();
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
            if (ImGui::Checkbox("##update-preview", &updatePreview_))
            {
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
            }
            BeginSettingRow(T("同时更新标签", "Also update tags"),
                ImGui::GetFrameHeight());
            if (ImGui::Checkbox("##update-tags", &updateTags_))
            {
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
            }
        }
        const bool missingManualTitle = creating &&
            !syncPackageLocalization_ && titleBuffer_.empty();
        ImGui::BeginDisabled(busy_.load() || submitStarted_.load() ||
            project.packageId.empty() || missingManualTitle);
        if (BlueButton(creating ? T("创建私有项目并上传", "Create private item and upload") :
                T("上传新版本", "Upload new version")))
            StartPublish(project.localId);
        ImGui::EndDisabled();
        if (project.publishedFileId)
        {
            ImGui::SameLine();
            if (SecondaryButton(T("打开 Steam Owner Controls", "Open Steam Owner Controls")))
                OpenSteamUrlWithWebFallback(
                    SteamCommunityItemClientUrl(*project.publishedFileId),
                    CommunityItemUrl(*project.publishedFileId));
        }
        if (submitStarted_.load())
        {
            ImGui::ProgressBar(progressFraction_.load(), ImVec2(-1, 0));
            ImGui::TextWrapped("%s", T(
                "Steam 已开始 SubmitItemUpdate；按 Valve 限制，此阶段不可取消或关闭窗口。",
                "Steam has started SubmitItemUpdate; this stage cannot be cancelled or closed."));
        }
        ImGui::Spacing();
        if (SecondaryButton(T("仅移除本地记录", "Remove local record only")))
        {
            const std::string id = project.localId;
            std::string error;
            if (store_.Remove(id, error) && store_.Save(error))
            {
                selectedLocalId_.clear();
                SetMessageUnlocked(true, T(
                    "已移除记录；源码和 Workshop 内容未删除",
                    "Record removed; source and Workshop content were not deleted"));
            }
            else SetMessageUnlocked(false, error);
        }
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
            if (pack) success = T("打包完成：%s", "Package complete: %s",
                artifact.sha256.c_str());
            std::lock_guard lock(mutex_);
            if (auto* project = FindProjectUnlocked(localId))
            {
                project->packageId = inspection.packageId;
                if (project->primaryPreview.empty())
                    project->primaryPreview = inspection.preview;
                if (!store_.Save(error)) SetMessageUnlocked(false, error);
                else SetMessageUnlocked(true, std::move(success));
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
        });
    }

    void StartPublish(std::string localId)
    {
        const std::string title = titleBuffer_;
        const std::string description = descriptionBuffer_;
        const bool syncPackageLocalization = syncPackageLocalization_;
        const bool updatePreview = updatePreview_;
        const bool updateTags = updateTags_;
        StartWork([this, localId = std::move(localId), title, description,
                   syncPackageLocalization, updatePreview, updateTags]
        {
            WorkshopProject snapshot;
            {
                std::lock_guard lock(mutex_);
                const auto* project = FindProjectUnlocked(localId);
                if (!project) return;
                snapshot = *project;
            }
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
            const bool creating = !snapshot.publishedFileId.has_value();
            std::vector<SteamWorkshopLocalization> localizations;
            if (syncPackageLocalization)
            {
                localizations = BuildSteamWorkshopLocalizations(
                    inspection.name, inspection.description,
                    inspection.localizations);
            }
            else if (creating)
            {
                localizations.push_back(
                    SteamWorkshopLocalization{ "english", title,
                        description });
            }
            if (creating && (localizations.empty() ||
                    localizations.front().language != "english" ||
                    localizations.front().title.empty()))
            {
                SetMessage(false, T(
                    "组件包必须提供英文标题，或关闭复用后手动填写英文标题。",
                    "The component package must provide an English title, or disable reuse and enter an English title manually."));
                return;
            }
            PublishRequest request;
            request.package = artifact.packagePath;
            request.publishedFileId = snapshot.publishedFileId;
            if (!localizations.empty())
            {
                request.title = localizations.front().title;
                if (!localizations.front().description.empty())
                    request.description = localizations.front().description;
                request.language = localizations.front().language;
            }
            request.metadata = BuildWorkshopMetadata(
                inspection.packageId, inspection.version);
            if (creating || updatePreview)
            {
                const auto preview = snapshot.primaryPreview.empty() ?
                    inspection.preview : snapshot.primaryPreview;
                if (!preview.empty()) request.preview = preview;
            }
            if (creating || updateTags) request.tags = snapshot.tags;
            CoreError coreError;
            const auto progress = [this, &localId](
                const PublishProgress& value)
            {
                if (value.stage == PublishStage::Created)
                {
                    std::string saveError;
                    std::lock_guard lock(mutex_);
                    if (auto* project = FindProjectUnlocked(localId))
                    {
                        project->publishedFileId = value.publishedFileId;
                        if (!store_.Save(saveError))
                            SetMessageUnlocked(false, saveError);
                    }
                }
                submitStarted_.store(value.submitStarted);
                const float fraction = value.total == 0 ? 0.0f :
                    static_cast<float>(static_cast<double>(value.processed) /
                        static_cast<double>(value.total));
                progressFraction_.store(std::clamp(fraction, 0.0f, 1.0f));
            };
            auto result = steam_.Publish(request,
                progress, coreError);
            submitStarted_.store(false);
            progressFraction_.store(0.0f);
            if (!result)
            {
                SetMessage(false, coreError.code + ": " + coreError.message);
                return;
            }
            {
                std::lock_guard lock(mutex_);
                if (auto* project = FindProjectUnlocked(localId))
                {
                    project->packageId = artifact.packageId;
                    project->publishedFileId = result->publishedFileId;
                    project->lastPublishedVersion = artifact.version;
                    project->lastPublishedSha256 = artifact.sha256;
                    project->lastPublishedAt = NowIso8601();
                    if (!store_.Save(error))
                    {
                        SetMessageUnlocked(false, error);
                        return;
                    }
                }
            }
            bool needsLegalAgreement = result->needsLegalAgreement;
            for (std::size_t index = 1; index < localizations.size(); ++index)
            {
                const auto& localized = localizations[index];
                PublishRequest localizedRequest;
                localizedRequest.updateContent = false;
                localizedRequest.publishedFileId = result->publishedFileId;
                localizedRequest.title = localized.title;
                if (!localized.description.empty())
                    localizedRequest.description = localized.description;
                localizedRequest.language = localized.language;
                CoreError localizedError;
                const auto localizedResult = steam_.Publish(
                    localizedRequest, progress, localizedError);
                submitStarted_.store(false);
                progressFraction_.store(0.0f);
                if (!localizedResult)
                {
                    const std::string detail = localizedError.code + ": " +
                        localizedError.message;
                    SetMessage(false, T(
                        "Steam 本地化 %s 提交失败：%s",
                        "Workshop localization %s failed: %s",
                        localized.language.c_str(), detail.c_str()));
                    OpenSteamUrlWithWebFallback(
                        SteamCommunityItemClientUrl(result->publishedFileId),
                        result->communityUrl);
                    return;
                }
                needsLegalAgreement = needsLegalAgreement ||
                    localizedResult->needsLegalAgreement;
            }
            {
                std::lock_guard lock(mutex_);
                localizationStateCreating_ = false;
                SetMessageUnlocked(true, needsLegalAgreement ?
                    T("上传成功；请在 Steam 页面接受创意工坊协议",
                      "Upload complete; accept the Workshop agreement on Steam") :
                    T("上传成功；已打开 Steam 项目页面",
                      "Upload complete; opening the Steam item page"));
            }
            OpenSteamUrlWithWebFallback(
                SteamCommunityItemClientUrl(result->publishedFileId),
                result->communityUrl);
        });
    }

    void RenderWorkshop()
    {
        ImGui::SeparatorText(T("我的 Workshop", "My Workshop"));
        ImGui::Spacing();
        ImGui::BeginDisabled(busy_.load());
        if (BlueButton(T("刷新我的项目", "Refresh my items")))
            RefreshPublished(publishedPage_);
        ImGui::SameLine();
        if (SecondaryButton("<") && publishedPage_ > 1)
            RefreshPublished(publishedPage_ - 1);
        ImGui::SameLine();
        if (SecondaryButton(">") && (publishedTotalPages_ == 0 ||
                publishedPage_ < publishedTotalPages_))
            RefreshPublished(publishedPage_ + 1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text(T("第 %u / %u 页，共 %u 项", "Page %u / %u, %u items"),
            publishedPage_, publishedTotalPages_, publishedTotalResults_);
        std::lock_guard lock(mutex_);
        if (ImGui::BeginTable("published-items", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, 300.0f * gDpiScale)))
        {
            ImGui::TableSetupColumn(T("项目", "Item"));
            ImGui::TableSetupColumn(T("更新时间", "Updated"));
            ImGui::TableSetupColumn(T("订阅", "Subs"));
            ImGui::TableSetupColumn(T("收藏", "Favs"));
            ImGui::TableSetupColumn(T("浏览", "Views"));
            ImGui::TableSetupColumn(T("评论", "Comments"));
            ImGui::TableSetupColumn(T("状态", "Status"));
            ImGui::TableHeadersRow();
            for (const auto& item : published_)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable((item.title + "##" +
                        std::to_string(item.publishedFileId)).c_str(),
                        selectedWorkshopId_ == item.publishedFileId,
                        ImGuiSelectableFlags_SpanAllColumns))
                    selectedWorkshopId_ = item.publishedFileId;
                ImGui::TableSetColumnIndex(1);
                const std::string updated = FormatLocalTime(item.updatedAt);
                ImGui::TextUnformatted(updated.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%llu", item.subscriptions);
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", item.favorites);
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%llu", item.views);
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%llu", item.comments);
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(item.banned ? T("已封禁", "Banned") :
                    T("正常", "Ready"));
            }
            ImGui::EndTable();
        }
        PublishedItem* item = FindWorkshopItemUnlocked(selectedWorkshopId_);
        if (!item)
        {
            ImGui::TextWrapped("%s", T(
                "刷新后选择一个 Workshop 项目查看详情。主预览将从 Steam HTTPS 地址缓存加载。",
                "Refresh and select a Workshop item for details. Primary previews are cached from Steam HTTPS URLs."));
            return;
        }
        ImGui::SeparatorText(T("项目详情", "Item details"));
        ImGui::Text("%s", item->title.c_str());
        if (!item->previewUrl.empty())
        {
            previewCache_.Request(item->publishedFileId, item->previewUrl);
            const PreviewTexture texture =
                previewCache_.Get(item->publishedFileId);
            if (texture.view)
            {
                const float maximumWidth = 420.0f * gDpiScale;
                const float maximumHeight = 220.0f * gDpiScale;
                const float scale = std::min(maximumWidth / texture.width,
                    maximumHeight / texture.height);
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(texture.view))),
                    ImVec2(texture.width * scale, texture.height * scale));
            }
            else if (texture.loading)
                ImGui::TextDisabled("%s", T("正在下载主预览…",
                    "Downloading primary preview…"));
            else if (!texture.error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                    "%s", texture.error.c_str());
        }
        if (SecondaryButton(T("打开 Steam Owner Controls", "Open Steam Owner Controls")))
            OpenSteamUrlWithWebFallback(
                SteamCommunityItemClientUrl(item->publishedFileId),
                CommunityItemUrl(item->publishedFileId));
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedLocalId_.empty());
        if (BlueButton(T("绑定到当前本地项目", "Bind to selected local project")))
        {
            WorkshopProject* project = FindProjectUnlocked(selectedLocalId_);
            std::string error;
            std::uint64_t currentSteamId = 0;
            const SteamStatus status = steam_.Status();
            if (const auto parsed = ParseItemId(status.steamId))
                currentSteamId = *parsed;
            if (project && CanBindWorkshopItem(*project, item->metadata,
                    item->ownerSteamId, currentSteamId, item->consumerAppId,
                    status.appId, error))
            {
                project->publishedFileId = item->publishedFileId;
                if (store_.Save(error)) SetMessageUnlocked(true,
                    T("Workshop 项目已绑定", "Workshop item bound"));
                else SetMessageUnlocked(false, error);
            }
            else SetMessageUnlocked(false, error);
        }
        ImGui::EndDisabled();
    }

    void RefreshPublished(std::uint32_t page)
    {
        StartWork([this, page]
        {
            CoreError error;
            const auto result = steam_.ListPublished(page, error);
            if (!result)
            {
                SetMessage(false, error.code + ": " + error.message);
                return;
            }
            const SteamStatus status = steam_.Status();
            const std::uint64_t steamId = ParseItemId(status.steamId).value_or(0);
            std::string saveError;
            std::lock_guard lock(mutex_);
            published_ = result->items;
            publishedPage_ = result->page;
            publishedTotalPages_ = result->totalPages;
            publishedTotalResults_ = result->totalResults;
            bool changed = false;
            for (const auto& item : published_)
            {
                std::string parseError;
                const auto metadata = ParseWorkshopMetadata(
                    item.metadata, parseError);
                if (!metadata) continue;
                for (auto& project : store_.Projects())
                {
                    if (!project.publishedFileId &&
                        project.packageId == metadata->packageId &&
                        item.ownerSteamId == steamId &&
                        item.consumerAppId == status.appId)
                    {
                        project.publishedFileId = item.publishedFileId;
                        changed = true;
                    }
                }
            }
            if (changed && !store_.Save(saveError))
                SetMessageUnlocked(false, saveError);
            else SetMessageUnlocked(true,
                T("已加载作者项目", "Published items loaded"));
        });
    }

    ManagerLocalization localization_;
    std::filesystem::path managerRoot_;
    std::filesystem::path developmentRoot_;
    std::filesystem::path projectDirectory_;
    std::filesystem::path settingsFile_;
    std::string currentLanguage_;
    int activePage_ = 0;
    std::optional<std::filesystem::file_time_type> settingsWriteTime_;
    std::chrono::steady_clock::time_point nextLanguageCheck_{};
    ProjectStore store_;
    PackageTool packageTool_;
    PreviewCache previewCache_;
    SteamWorkshopCore steam_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::atomic_bool busy_ = false;
    std::atomic_bool submitStarted_ = false;
    std::atomic<float> progressFraction_ = 0.0f;
    std::string selectedLocalId_;
    std::uint64_t selectedWorkshopId_ = 0;
    std::vector<PublishedItem> published_;
    std::uint32_t publishedPage_ = 1;
    std::uint32_t publishedTotalPages_ = 0;
    std::uint32_t publishedTotalResults_ = 0;
    char localSearch_[256]{};
    std::string tagsBuffer_;
    std::string titleBuffer_;
    std::string descriptionBuffer_;
    std::string localizationStateProjectId_;
    std::string localizationPreviewProjectId_;
    std::vector<SteamWorkshopLocalization> localizationPreview_;
    std::string localizationPreviewError_;
    bool localizationStateCreating_ = false;
    bool localizationPreviewLoading_ = false;
    bool syncPackageLocalization_ = true;
    bool updatePreview_ = false;
    bool updateTags_ = false;
    bool messageSuccess_ = true;
    std::string message_;
};

WorkshopManagerApp* gApp = nullptr;
bool gRenderingFrame = false;
constexpr UINT_PTR kLiveResizeTimer = 0x5344;

void RenderManagerFrame(HWND window)
{
    if (!gApp || gRenderingFrame || !ImGui::GetCurrentContext() ||
        !gDevice.Get() || !gContext.Get() || !gSwapChain.Get() ||
        !gRenderTarget.Get())
        return;

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
    gSwapChain->Present(1, 0);
    gRenderingFrame = false;
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

LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
        return TRUE;
    switch (message)
    {
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
            RenderManagerFrame(window);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        SetTimer(window, kLiveResizeTimer, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(window, kLiveResizeTimer);
        RenderManagerFrame(window);
        return 0;
    case WM_TIMER:
        if (wParam == kLiveResizeTimer)
        {
            RenderManagerFrame(window);
            return 0;
        }
        break;
    case WM_DPICHANGED:
        if (const RECT* rect = reinterpret_cast<const RECT*>(lParam))
            SetWindowPos(window, nullptr, rect->left, rect->top,
                rect->right - rect->left, rect->bottom - rect->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_CLOSE:
        if (gApp && !gApp->CanClose())
        {
            MessageBeep(MB_ICONWARNING);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
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
        result.dataDirectory = ExecutableDirectory() / L"data";
    if (result.developmentRoot.empty())
        result.developmentRoot = ExecutableDirectory() / L"data" /
            L"widgets" / L"dev";
    if (!settingsSpecified && !languageSpecified)
        result.settingsFile = ExecutableDirectory() / L"data" /
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
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE,
        L"Local\\SnowDesktopWorkshopManager");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS)
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const UINT dpi = GetDpiForWindow(window);
    const float scale = std::max(1.0f, static_cast<float>(dpi) / 96.0f);
    gDpiScale = scale;
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
    WorkshopManagerApp app(ReadArguments(), ExecutableDirectory() / L"lang");
    const std::wstring localizedWindowTitle = Utf8ToWide(app.WindowTitle());
    SetWindowTextW(window, localizedWindowTitle.c_str());
    gApp = &app;
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    bool running = true;
    while (running)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) running = false;
        }
        if (!running) break;
        RenderManagerFrame(window);
    }
    gApp = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    gRenderTarget.Reset();
    gSwapChain.Reset();
    gContext.Reset();
    gDevice.Reset();
    DestroyWindow(window);
    UnregisterClassW(className, instance);
    if (smallIcon) DestroyIcon(smallIcon);
    CloseHandle(singleInstance);
    CoUninitialize();
    return 0;
}
