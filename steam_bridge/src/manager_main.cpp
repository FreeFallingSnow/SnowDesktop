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
#include "steam_workshop_core.h"
#include "workshop_project.h"

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

void OpenUrl(std::string_view url)
{
    const auto wide = Utf8ToWide(url);
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
        SW_SHOWNORMAL);
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
    std::filesystem::path developmentRoot;
    std::filesystem::path settingsFile;
    std::string language;
};

class WorkshopManagerApp
{
public:
    explicit WorkshopManagerApp(ManagerArguments arguments,
        std::filesystem::path languageDirectory)
        : developmentRoot_(std::move(arguments.developmentRoot)),
          settingsFile_(std::move(arguments.settingsFile)),
          currentLanguage_(std::move(arguments.language))
    {
        std::string error;
        if (!localization_.Load(languageDirectory, currentLanguage_, error))
            SetMessage(false, error);
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
        if (!store_.Projects().empty())
            selectedLocalId_ = store_.Projects().front().localId;
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
            ImGuiWindowFlags_NoSavedSettings);
        RenderHeader(window);
        ImGui::Separator();
        if (ImGui::BeginTabBar("manager-tabs"))
        {
            if (ImGui::BeginTabItem(T("本地项目", "Local Projects")))
            {
                RenderLocalProjects(window);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("我的 Workshop", "My Workshop")))
            {
                RenderWorkshop();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(T("诊断", "Diagnostics")))
            {
                RenderDiagnostics();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
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
        ImGui::TextUnformatted(T("SnowDesktop 创意工坊创作者管理器",
            "SnowDesktop Workshop Creator Manager"));
        ImGui::SameLine();
        ImGui::TextDisabled("v%s", SNOWDESKTOP_VERSION);

        // Keep actions on their own responsive row. The old fixed right offset
        // clipped the buttons at narrow window widths and under DPI scaling.
        ImGui::BeginDisabled(busy_.load());
        if (ImGui::Button(T("添加组件目录", "Add component folder")))
        {
            if (const auto path = PickPath(window, true)) AddDroppedDirectory(*path);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T("打开网页工坊", "Open Workshop")))
            OpenUrl("https://steamcommunity.com/workshop/");
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

    void RenderLocalProjects(HWND window)
    {
        std::lock_guard lock(mutex_);
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputTextWithHint("##local-search",
            T("搜索名称、UUID 或路径", "Search name, UUID, or path"),
            localSearch_, std::size(localSearch_));
        ImGui::BeginChild("local-list", ImVec2(330, 0), true);
        for (const auto& project : store_.Projects())
        {
            const std::string label = project.packageId.empty()
                ? WideToUtf8(project.sourceDirectory.filename().wstring())
                : project.packageId;
            const std::string haystack = label + " " + project.localId + " " +
                WideToUtf8(project.sourceDirectory.wstring());
            if (localSearch_[0] && haystack.find(localSearch_) ==
                std::string::npos)
                continue;
            if (ImGui::Selectable((label + "##" + project.localId).c_str(),
                    selectedLocalId_ == project.localId))
            {
                selectedLocalId_ = project.localId;
                tagsBuffer_ = JoinTags(project.tags);
                titleBuffer_.clear();
                descriptionBuffer_.clear();
            }
            ImGui::TextDisabled("%s", WideToUtf8(
                project.sourceDirectory.filename().wstring()).c_str());
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("local-detail", ImVec2(0, 0), true);
        WorkshopProject* project = FindProjectUnlocked(selectedLocalId_);
        if (!project)
        {
            ImGui::TextWrapped("%s", T(
                "添加包含 widget.json 的组件目录，或把目录拖入此窗口。",
                "Add a component directory containing widget.json, or drop it onto this window."));
            ImGui::EndChild();
            return;
        }
        RenderProjectDetailsUnlocked(window, *project);
        ImGui::EndChild();
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
        ImGui::Text("%s", WideToUtf8(project.sourceDirectory.filename().wstring()).c_str());
        ImGui::TextDisabled("Local UUID: %s", project.localId.c_str());
        ImGui::TextWrapped("%s", WideToUtf8(project.sourceDirectory.wstring()).c_str());
        if (ImGui::Button(T("打开目录", "Open folder")))
            OpenDirectory(project.sourceDirectory);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy_.load());
        if (ImGui::Button(T("校验", "Validate")))
        {
            const std::string id = project.localId;
            StartInspect(id, false);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("校验并打包", "Validate and pack")))
        {
            const std::string id = project.localId;
            StartInspect(id, true);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::Text("Package ID: %s", project.packageId.empty() ?
            T("尚未校验", "Not inspected") : project.packageId.c_str());
        ImGui::Text("PublishedFileId: %s", project.publishedFileId ?
            std::to_string(*project.publishedFileId).c_str() :
            T("未绑定", "Not bound"));
        ImGui::Text("%s: %s", T("最后发布版本", "Last published version"),
            project.lastPublishedVersion.empty() ? "-" :
            project.lastPublishedVersion.c_str());
        ImGui::Text("SHA-256: %s", project.lastPublishedSha256.empty() ?
            "-" : project.lastPublishedSha256.c_str());
        ImGui::Text("%s: %s", T("主预览", "Primary preview"),
            project.primaryPreview.empty() ? "-" :
            WideToUtf8(project.primaryPreview.wstring()).c_str());
        if (!project.primaryPreview.empty())
        {
            const std::uint64_t previewKey = StableKey(project.localId);
            previewCache_.RequestLocal(previewKey, project.primaryPreview);
            const PreviewTexture texture = previewCache_.Get(previewKey);
            if (texture.view)
            {
                const float scale = std::min(360.0f / texture.width,
                    180.0f / texture.height);
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(
                    reinterpret_cast<std::uintptr_t>(texture.view))),
                    ImVec2(texture.width * scale, texture.height * scale));
            }
            else if (!texture.error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                    "%s", texture.error.c_str());
        }
        if (ImGui::Button(T("选择主预览", "Choose primary preview")))
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
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText(T("标签（逗号分隔）", "Tags (comma-separated)"),
                tags.data(), tags.size()))
            tagsBuffer_ = tags.data();
        if (ImGui::Button(T("保存标签", "Save tags")))
        {
            project.tags = SplitTags(tagsBuffer_);
            std::string error;
            if (!store_.Save(error)) SetMessageUnlocked(false, error);
            else SetMessageUnlocked(true, T("标签已保存", "Tags saved"));
        }
        ImGui::SeparatorText(T("发布", "Publish"));
        const bool creating = !project.publishedFileId.has_value();
        if (creating)
        {
            ImGui::TextWrapped("%s", T(
                "首次创建必须填写初始标题和说明，项目会以私有状态创建。之后请在 Steam 网页维护资料与公开状态。",
                "Creation requires an initial title and description. The item is created private; maintain its page and visibility on Steam afterward."));
            std::array<char, 256> title{};
            std::copy_n(titleBuffer_.c_str(),
                std::min(titleBuffer_.size(), title.size() - 1), title.data());
            if (ImGui::InputText(T("初始标题", "Initial title"), title.data(),
                    title.size())) titleBuffer_ = title.data();
            std::array<char, 4096> description{};
            std::copy_n(descriptionBuffer_.c_str(),
                std::min(descriptionBuffer_.size(), description.size() - 1),
                description.data());
            if (ImGui::InputTextMultiline(T("初始说明", "Initial description"),
                    description.data(), description.size(), ImVec2(-1, 90)))
                descriptionBuffer_ = description.data();
        }
        else
        {
            ImGui::TextWrapped("%s", T(
                "更新默认只上传 package.snowwidget 和关联 metadata，不覆盖网页端标题、说明或可见性。",
                "Updates upload only package.snowwidget and association metadata; web-managed title, description, and visibility are preserved."));
            ImGui::Checkbox(T("同时更新主预览", "Also update primary preview"),
                &updatePreview_);
            ImGui::Checkbox(T("同时更新标签", "Also update tags"),
                &updateTags_);
        }
        ImGui::BeginDisabled(busy_.load() || submitStarted_.load() ||
            project.packageId.empty());
        if (ImGui::Button(creating ? T("创建私有项目并上传", "Create private item and upload") :
                T("上传新版本", "Upload new version")))
            StartPublish(project.localId);
        ImGui::EndDisabled();
        if (project.publishedFileId)
        {
            ImGui::SameLine();
            if (ImGui::Button(T("打开 Steam Owner Controls", "Open Steam Owner Controls")))
                OpenUrl(CommunityItemUrl(*project.publishedFileId));
        }
        if (submitStarted_.load())
        {
            ImGui::ProgressBar(progressFraction_.load(), ImVec2(-1, 0));
            ImGui::TextWrapped("%s", T(
                "Steam 已开始 SubmitItemUpdate；按 Valve 限制，此阶段不可取消或关闭窗口。",
                "Steam has started SubmitItemUpdate; this stage cannot be cancelled or closed."));
        }
        ImGui::Separator();
        if (ImGui::Button(T("仅移除本地记录", "Remove local record only")))
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

    void StartPublish(std::string localId)
    {
        const std::string title = titleBuffer_;
        const std::string description = descriptionBuffer_;
        const bool updatePreview = updatePreview_;
        const bool updateTags = updateTags_;
        StartWork([this, localId = std::move(localId), title, description,
                   updatePreview, updateTags]
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
            PublishRequest request;
            request.package = artifact.packagePath;
            request.publishedFileId = snapshot.publishedFileId;
            request.title = creating ? title : std::string{};
            if (creating) request.description = description;
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
            auto result = steam_.Publish(request,
                [this, &localId](const PublishProgress& value)
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
                }, coreError);
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
                SetMessageUnlocked(true, result->needsLegalAgreement ?
                    T("上传成功；请在 Steam 页面接受创意工坊协议",
                      "Upload complete; accept the Workshop agreement on Steam") :
                    T("上传成功；已打开 Steam 项目页面",
                      "Upload complete; opening the Steam item page"));
            }
            OpenUrl(result->communityUrl);
        });
    }

    void RenderWorkshop()
    {
        ImGui::BeginDisabled(busy_.load());
        if (ImGui::Button(T("刷新我的项目", "Refresh my items")))
            RefreshPublished(publishedPage_);
        ImGui::SameLine();
        if (ImGui::Button("<") && publishedPage_ > 1)
            RefreshPublished(publishedPage_ - 1);
        ImGui::SameLine();
        if (ImGui::Button(">") && (publishedTotalPages_ == 0 ||
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
            ImVec2(0, 300)))
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
                ImGui::Text("%u", item.updatedAt);
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
        ImGui::Text("PublishedFileId: %llu", item->publishedFileId);
        ImGui::Text("Owner: %llu  App ID: %u", item->ownerSteamId,
            item->consumerAppId);
        ImGui::TextWrapped("Preview: %s", item->previewUrl.c_str());
        if (!item->previewUrl.empty())
        {
            previewCache_.Request(item->publishedFileId, item->previewUrl);
            const PreviewTexture texture =
                previewCache_.Get(item->publishedFileId);
            if (texture.view)
            {
                const float maximumWidth = 420.0f;
                const float maximumHeight = 220.0f;
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
        ImGui::TextWrapped("Metadata: %s", item->metadata.c_str());
        if (ImGui::Button(T("打开 Steam Owner Controls", "Open Steam Owner Controls")))
            OpenUrl(CommunityItemUrl(item->publishedFileId));
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedLocalId_.empty());
        if (ImGui::Button(T("绑定到当前本地项目", "Bind to selected local project")))
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

    void RenderDiagnostics()
    {
        const SteamStatus status = steam_.Status();
        ImGui::Text("Steamworks compiled: %s", status.compiled ? "yes" : "no");
        ImGui::Text("Steam initialized: %s", status.initialized ? "yes" : "no");
        ImGui::Text("Steam logged on: %s", status.loggedOn ? "yes" : "no");
        ImGui::Text("App ID: %u", status.appId);
        ImGui::Text("Steam ID: %s", status.steamId.c_str());
        ImGui::TextWrapped("Diagnostic: %s", status.diagnostic.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("Project store: %s",
            WideToUtf8(store_.StorePath().wstring()).c_str());
        ImGui::TextWrapped("snowwidget.exe: %s",
            WideToUtf8(packageTool_.Executable().wstring()).c_str());
        ImGui::TextWrapped("Preview cache: %s",
            WideToUtf8(previewCache_.Root().wstring()).c_str());
        ImGui::TextWrapped("%s", T(
            "SDK-free 构建仍可添加、校验和打包本地组件；Steam 查询和上传会显示不可用诊断。",
            "SDK-free builds still add, validate, and package local components; Steam queries and uploads show an unavailable diagnostic."));
        if (eulaKnown_.load())
            ImGui::Text("Workshop agreement: %s",
                eulaAccepted_.load() ? "accepted" : "action required");
        ImGui::BeginDisabled(busy_.load());
        if (ImGui::Button(T("初始化 Steam", "Initialize Steam")))
        {
            StartWork([this]
            {
                CoreError error;
                if (steam_.Initialize(error))
                    SetMessage(true, T("Steam 初始化成功", "Steam initialized"));
                else SetMessage(false, error.code + ": " + error.message);
            });
        }
        ImGui::SameLine();
        if (ImGui::Button(T("检查创意工坊协议", "Check Workshop agreement")))
        {
            StartWork([this]
            {
                CoreError error;
                const auto status = steam_.GetEulaStatus(error);
                if (!status)
                {
                    SetMessage(false, error.code + ": " + error.message);
                    return;
                }
                eulaKnown_.store(true);
                eulaAccepted_.store(status->accepted && !status->needsAction);
                SetMessage(true, status->accepted && !status->needsAction ?
                    T("创意工坊协议已接受", "Workshop agreement accepted") :
                    T("需要在 Steam 页面处理创意工坊协议",
                      "Workshop agreement requires action on Steam"));
            });
        }
        ImGui::EndDisabled();
    }

    ManagerLocalization localization_;
    std::filesystem::path developmentRoot_;
    std::filesystem::path settingsFile_;
    std::string currentLanguage_;
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
    std::atomic_bool eulaKnown_ = false;
    std::atomic_bool eulaAccepted_ = false;
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
    bool updatePreview_ = false;
    bool updateTags_ = false;
    bool messageSuccess_ = true;
    std::string message_;
};

WorkshopManagerApp* gApp = nullptr;

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
        }
        return 0;
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
    WNDCLASSEXW windowClass{ sizeof(windowClass), CS_CLASSDC,
        WindowProcedure, 0, 0, instance, nullptr, LoadCursorW(nullptr, IDC_ARROW),
        nullptr, nullptr, className, nullptr };
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
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.WindowRounding = 8.0f * scale;
    style.FrameRounding = 5.0f * scale;
    wchar_t windowsDirectory[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windowsDirectory,
            static_cast<UINT>(std::size(windowsDirectory))))
        wcscpy_s(windowsDirectory, L"C:\\Windows");
    const std::filesystem::path fonts =
        std::filesystem::path(windowsDirectory) / L"Fonts";
    const auto chineseFont = fonts / L"msyh.ttc";
    const auto regularFont = fonts / L"segoeui.ttf";
    const auto font = std::filesystem::exists(chineseFont) ?
        chineseFont : regularFont;
    io.Fonts->AddFontFromFileTTF(WideToUtf8(font.wstring()).c_str(),
        17.0f * scale, nullptr, io.Fonts->GetGlyphRangesChineseFull());
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
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.Render(window);
        ImGui::Render();
        const float clear[4] = { 0.055f, 0.065f, 0.085f, 1.0f };
        gContext->OMSetRenderTargets(1, gRenderTarget.GetAddressOf(), nullptr);
        gContext->ClearRenderTargetView(gRenderTarget.Get(), clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        gSwapChain->Present(1, 0);
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
    CloseHandle(singleInstance);
    CoUninitialize();
    return 0;
}
