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
#include "widget_engine.h"
#include "l10n.h"
#include "resource.h"
#include "crashlog.h"
#include "data_paths.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

SettingsWindow* g_settingsWindow = nullptr;

namespace {
constexpr UINT_PTR kSettingsRefreshTimerId = 1;
constexpr UINT kSettingsRefreshIntervalMs = 500;
constexpr float kSettingControlWidthDip = 300.0f;

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
    instance_ = instance;
    device_ = device;
    device_->GetImmediateContext(&context_);

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
    RegisterClassExW(&wc);

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

    if (hwnd_ == nullptr) return false;
    if (wc.hIcon)
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    if (wc.hIconSm)
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    if (!CreateSwapChain()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    SetupLightTheme();

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());

    SetupFonts();

    LoadPersonalization(GetPersonalizationPath().c_str(), personalization_);
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
        KillTimer(hwnd_, kSettingsRefreshTimerId);

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
        if (dockSettingsChangedCallback_)
            dockSettingsChangedCallback_();
    }
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
    g_settingsWindow = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupSwapChain();
    if (hwnd_ != nullptr) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    renderRequested_ = false;
}

/**
 * @brief 显示设置窗口（若尚未初始化则先初始化再显示）。
 *
 * 将窗口置于前台并设置焦点。
 */
void SettingsWindow::Show()
{
    if (hwnd_ == nullptr)
    {
        if (!Init(instance_, device_.Get()))
            return;
    }
    dockSettings_.systemTaskbarAutoHide = IsSystemTaskbarAutoHideEnabled();
    dockSettings_.systemTaskbarAlignment = IsSystemTaskbarAlignmentCentered() ? 1 : 0;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetTimer(hwnd_, kSettingsRefreshTimerId, kSettingsRefreshIntervalMs, nullptr);
    renderRequested_ = true;
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
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

void SettingsWindow::ShowDockSettings()
{
    activePage_ = 0;
    Show();
}

/**
 * @brief 显示退出确认对话框。
 *
 * 设置 showExitConfirm_ 标记后调用 Show()，
 * 在下一帧 Render() 中弹出模态确认框。
 */
void SettingsWindow::ShowExitConfirm()
{
    showExitConfirm_ = true;
    Show();
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

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Feed current mouse position so first click works without prior WM_MOUSEMOVE
    POINT mp;
    GetCursorPos(&mp);
    ScreenToClient(hwnd_, &mp);
    ImGui::GetIO().MousePos = ImVec2((float)mp.x, (float)mp.y);

    ImGui::NewFrame();

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

    if (dockSettingsDirty_)
    {
        SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
        dockSettingsDirty_ = false;
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
        bool active = (activePage_ == idx);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.80f, 0.85f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.12f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
            activePage_ = idx;
        }
        if (active) ImGui::PopStyleColor(2);
    };

    SideButton(0, _L("app.settings.general"));
    SideButton(1, _L("app.settings.appearance"));
    SideButton(4, _L("app.settings.category"));
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

/**
 * @brief 描述一个快捷键选项（虚拟键码 + 显示文本）。
 */
struct HotkeyOption
{
    UINT virtualKey;
    const char* label;
};

/**
 * @brief 获取全局导航快捷键可选项列表。
 *
 * 包含 Space、Tab、Enter、反引号、字母 A-Z、功能键 F1-F12。
 * @param count 输出参数，接收选项总数
 * @return 指向静态常量 HotkeyOption 数组的指针
 */
static const HotkeyOption* NavigationHotkeyOptions(size_t& count)
{
    static const HotkeyOption options[] = {
        { VK_SPACE, "Space" },
        { VK_TAB, "Tab" },
        { VK_RETURN, "Enter" },
        { VK_OEM_3, "`" },
        { 'A', "A" }, { 'B', "B" }, { 'C', "C" }, { 'D', "D" },
        { 'E', "E" }, { 'F', "F" }, { 'G', "G" }, { 'H', "H" },
        { 'I', "I" }, { 'J', "J" }, { 'K', "K" }, { 'L', "L" },
        { 'M', "M" }, { 'N', "N" }, { 'O', "O" }, { 'P', "P" },
        { 'Q', "Q" }, { 'R', "R" }, { 'S', "S" }, { 'T', "T" },
        { 'U', "U" }, { 'V', "V" }, { 'W', "W" }, { 'X', "X" },
        { 'Y', "Y" }, { 'Z', "Z" },
        { VK_F1, "F1" }, { VK_F2, "F2" }, { VK_F3, "F3" }, { VK_F4, "F4" },
        { VK_F5, "F5" }, { VK_F6, "F6" }, { VK_F7, "F7" }, { VK_F8, "F8" },
        { VK_F9, "F9" }, { VK_F10, "F10" }, { VK_F11, "F11" }, { VK_F12, "F12" },
    };
    count = sizeof(options) / sizeof(options[0]);
    return options;
}

/**
 * @brief 根据虚拟键码查找 NavigationHotkeyOptions 中的索引。
 * @param virtualKey 要查找的 Windows 虚拟键码
 * @return 匹配的选项索引，未找到时返回 0（Space）
 */
static int NavigationHotkeyOptionIndex(UINT virtualKey)
{
    size_t count = 0;
    const HotkeyOption* options = NavigationHotkeyOptions(count);
    for (size_t i = 0; i < count; ++i)
    {
        if (options[i].virtualKey == virtualKey)
            return static_cast<int>(i);
    }
    return 0;
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
    ImGui::SeparatorText(_L("app.settings.backup_page_title"));
    ImGui::Spacing();

    const float controlW = kSettingControlWidthDip * dpiScale_;
    const float saveButtonW = 84.0f * dpiScale_;
    const float inputW = std::max(1.0f,
        controlW - ImGui::GetStyle().ItemSpacing.x - saveButtonW);
    BeginSettingRow(_L("app.settings.save_current_layout"), controlW);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputTextWithHint("##BackupName", _L("app.settings.backup_name_hint"), backupNameBuf_, sizeof(backupNameBuf_));

    ImGui::SameLine();
    if (BlueButton(_L("app.settings.save_backup"), ImVec2(saveButtonW, 0)))
    {
        std::wstring name = Utf8ToWide(backupNameBuf_);
        if (name.empty()) name = MakeBackupTimestampName();
        if (SaveBackup(name))
        {
            backupNameBuf_[0] = '\0';
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.saved_backups"));
    ImGui::Spacing();

    std::vector<LayoutBackup> backups = ListBackups();
    if (backups.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_backups"));
    }
    else
    {
        ImGui::BeginChild("##BackupList", ImVec2(0, 0), true);

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
        ImGui::EndChild();
    }

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

    if (DrawSettingCheckbox(_L("app.settings.software_desktop"), "##SoftwareDesktopEnabled",
        &generalSettings_.softwareDesktopEnabled))
        generalSettingsDirty_ = true;

    ImGui::Spacing();

    {
        const float controlW = kSettingControlWidthDip * dpiScale_;
        BeginSettingRow(_L("app.settings.language"), controlW);
        std::vector<std::string> langNames{ "system" };
        std::vector<std::string> langLabels{ _L("app.settings.language_system") };
        for (const LanguageInfo& language :
            Locale::Instance().GetAvailableLanguages())
        {
            langNames.push_back(language.code);
            langLabels.push_back(language.displayName);
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

    ImGui::BeginDisabled(!navigationSettings_.enabled);

    bool ctrl = (navigationSettings_.modifiers & MOD_CONTROL) != 0;
    bool alt = (navigationSettings_.modifiers & MOD_ALT) != 0;
    bool shift = (navigationSettings_.modifiers & MOD_SHIFT) != 0;
    bool win = (navigationSettings_.modifiers & MOD_WIN) != 0;
    bool modifiersChanged = false;
    const float modifierWidth = 244.0f * dpiScale_;
    BeginSettingRow(_L("app.settings.modifier_keys"), modifierWidth);
    modifiersChanged |= ImGui::Checkbox("Ctrl", &ctrl);
    ImGui::SameLine();
    modifiersChanged |= ImGui::Checkbox("Alt", &alt);
    ImGui::SameLine();
    modifiersChanged |= ImGui::Checkbox("Shift", &shift);
    ImGui::SameLine();
    modifiersChanged |= ImGui::Checkbox("Win", &win);
    if (modifiersChanged)
    {
        navigationSettings_.modifiers = 0;
        if (ctrl) navigationSettings_.modifiers |= MOD_CONTROL;
        if (alt) navigationSettings_.modifiers |= MOD_ALT;
        if (shift) navigationSettings_.modifiers |= MOD_SHIFT;
        if (win) navigationSettings_.modifiers |= MOD_WIN;
        navigationSettingsDirty_ = true;
    }

    size_t optionCount = 0;
    const HotkeyOption* options = NavigationHotkeyOptions(optionCount);
    int selected = NavigationHotkeyOptionIndex(navigationSettings_.virtualKey);
    const float hotkeyControlW = kSettingControlWidthDip * dpiScale_;
    BeginSettingRow(_L("app.settings.primary_key"), hotkeyControlW);
    ImGui::SetNextItemWidth(hotkeyControlW);
    if (ImGui::Combo("##NavigationMainKey", &selected,
        [](void* data, int idx, const char** outText) {
            auto* opts = static_cast<const HotkeyOption*>(data);
            *outText = opts[idx].label;
            return true;
        }, const_cast<HotkeyOption*>(options), static_cast<int>(optionCount)))
    {
        navigationSettings_.virtualKey = options[selected].virtualKey;
        navigationSettingsDirty_ = true;
    }

    std::wstring hotkeyText = FormatNavigationHotkey(navigationSettings_);
    const std::string hotkeyTextUtf8 = WideToUtf8(hotkeyText);
    DrawSettingValue(_L("app.settings.current_hotkey"), hotkeyTextUtf8.c_str());

    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.desktop_interact"));
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
    auto markChanged = [&]() { dockSettingsDirty_ = true; };

    if (DrawSettingCheckbox(_L("app.dock.enable"), "##DockEnabled", &dockEnabled_))
    {
        if (dockEnabledChangedCallback_)
            dockEnabledChangedCallback_(dockEnabled_);
    }

    ImGui::BeginDisabled(!dockEnabled_);
    ImGui::Spacing();
    BeginSettingRow(_L("app.settings.dock_position"), controlW);
    const char* positionNames[] = { _L("app.dock.bottom"), _L("app.dock.top"), _L("app.dock.left"), _L("app.dock.right") };
    int position = std::clamp(static_cast<int>(dockSettings_.position), 0, 3);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockPosition", &position, positionNames, IM_ARRAYSIZE(positionNames)))
    {
        dockSettings_.position = static_cast<DockPosition>(position);
        markChanged();
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
        markChanged();
    }
    BeginSettingRow(_L("app.dock.layout"), controlW);
    const char* layoutNames[] = { _L("app.dock.island"), _L("app.dock.edge") };
    int layoutMode = dockSettings_.edgeAttached ? 1 : 0;
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##DockLayoutMode", &layoutMode, layoutNames, IM_ARRAYSIZE(layoutNames)))
    {
        dockSettings_.edgeAttached = layoutMode == 1;
        markChanged();
    }
    BeginSettingRow(_L("app.settings.dock_thickness"), sliderActionW,
        _L("app.settings.dock_thickness_hint"));
    ImGui::SetNextItemWidth(actionSliderW);
    int thicknessPercent = static_cast<int>(std::round(
        dockSettings_.thicknessScale * 100.0f));
    if (ImGui::SliderInt("##DockThickness", &thicknessPercent, 50, 100, "%d%%"))
    {
        dockSettings_.thicknessScale = thicknessPercent / 100.0f;
        markChanged();
    }
    ImGui::SameLine();
    if (BlueButton(thicknessResetLabel.c_str(), ImVec2(resetW, 0)))
    {
        dockSettings_.thicknessScale = 1.0f;
        markChanged();
    }

    if (DrawSettingCheckbox(_L("app.dock.show_windows_button"), "##DockShowWindowsButton",
        &dockSettings_.showWindowsButton))
        markChanged();

    if (DrawSettingCheckbox(_L("app.dock.show_running_area"), "##DockShowRunningApps",
        &dockSettings_.showRunningApps))
        markChanged();

    if (DrawSettingCheckbox(_L("app.dock.show_frequent_items"), "##DockShowFrequentItems",
        &dockSettings_.showFrequentItems))
        markChanged();

    ImGui::BeginDisabled(!dockSettings_.showFrequentItems);
    BeginSettingRow(_L("app.settings.show_count"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::SliderInt("##DockFrequentItemCount",
        &dockSettings_.frequentItemCount, 1, 8, _L("app.settings.items_unit")))
        markChanged();
    ImGui::EndDisabled();

    ImGui::EndDisabled();

}

/**
 * @brief 绘制外观页中的 Windows 系统任务栏设置区域。
 */
void SettingsWindow::DrawSystemTaskbarPage()
{
    const float controlW = kSettingControlWidthDip * dpiScale_;
    auto markChanged = [&]() { dockSettingsDirty_ = true; };

    if (DrawSettingCheckbox(_L("app.settings.auto_hide_taskbar"), "##SystemTaskbarAutoHide",
        &dockSettings_.systemTaskbarAutoHide))
        markChanged();

    BeginSettingRow(_L("app.settings.taskbar_alignment"), controlW,
        _L("app.settings.taskbar_alignment_hint"));
    const char* alignmentNames[] = { _L("app.settings.taskbar_left"), _L("app.settings.taskbar_center") };
    int alignment = std::clamp(dockSettings_.systemTaskbarAlignment, 0, 1);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##SystemTaskbarAlignment", &alignment,
        alignmentNames, IM_ARRAYSIZE(alignmentNames)))
    {
        dockSettings_.systemTaskbarAlignment = alignment;
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
        SetWindowsSystemLightThemeEnabled(windowsTheme == 0);
        dockSettingsDirty_ = true;
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

    auto applyIconBeautifyPreset = [&](int preset) {
        iconBeautifyBgPreset_ = preset;
        switch (preset)
        {
        case 2:
            iconBeautifyBgOpacity_ = 0.50f;
            iconBeautifyGradientEnabled_ = false;
            iconBeautifyGradientDirection_ = 0;
            iconBeautifyBgStartR_ = 255.0f / 255.0f;
            iconBeautifyBgStartG_ = 255.0f / 255.0f;
            iconBeautifyBgStartB_ = 255.0f / 255.0f;
            iconBeautifyBgEndR_ = iconBeautifyBgStartR_;
            iconBeautifyBgEndG_ = iconBeautifyBgStartG_;
            iconBeautifyBgEndB_ = iconBeautifyBgStartB_;
            break;
        case 3:
            iconBeautifyBgOpacity_ = 0.82f;
            iconBeautifyGradientEnabled_ = true;
            iconBeautifyGradientDirection_ = 2;
            iconBeautifyBgStartR_ = 156.0f / 255.0f;
            iconBeautifyBgStartG_ = 216.0f / 255.0f;
            iconBeautifyBgStartB_ = 255.0f / 255.0f;
            iconBeautifyBgEndR_ = 74.0f / 255.0f;
            iconBeautifyBgEndG_ = 128.0f / 255.0f;
            iconBeautifyBgEndB_ = 255.0f / 255.0f;
            break;
        case 4:
            iconBeautifyBgOpacity_ = 0.78f;
            iconBeautifyGradientEnabled_ = true;
            iconBeautifyGradientDirection_ = 3;
            iconBeautifyBgStartR_ = 255.0f / 255.0f;
            iconBeautifyBgStartG_ = 218.0f / 255.0f;
            iconBeautifyBgStartB_ = 138.0f / 255.0f;
            iconBeautifyBgEndR_ = 255.0f / 255.0f;
            iconBeautifyBgEndG_ = 122.0f / 255.0f;
            iconBeautifyBgEndB_ = 164.0f / 255.0f;
            break;
        case 5:
            iconBeautifyBgOpacity_ = 0.70f;
            iconBeautifyGradientEnabled_ = true;
            iconBeautifyGradientDirection_ = 1;
            iconBeautifyBgStartR_ = 24.0f / 255.0f;
            iconBeautifyBgStartG_ = 32.0f / 255.0f;
            iconBeautifyBgStartB_ = 48.0f / 255.0f;
            iconBeautifyBgEndR_ = 87.0f / 255.0f;
            iconBeautifyBgEndG_ = 105.0f / 255.0f;
            iconBeautifyBgEndB_ = 135.0f / 255.0f;
            break;
        default:
            iconBeautifyBgPreset_ = 1;
            iconBeautifyBgOpacity_ = 0.65f;
            iconBeautifyGradientEnabled_ = false;
            iconBeautifyGradientDirection_ = 0;
            iconBeautifyBgStartR_ = 232.0f / 255.0f;
            iconBeautifyBgStartG_ = 236.0f / 255.0f;
            iconBeautifyBgStartB_ = 244.0f / 255.0f;
            iconBeautifyBgEndR_ = 222.0f / 255.0f;
            iconBeautifyBgEndG_ = 228.0f / 255.0f;
            iconBeautifyBgEndB_ = 240.0f / 255.0f;
            break;
        }
    };

    BeginSettingRow(_L("app.settings.icon_spacing"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderInt("##IconSpacing", &displaySpacingPct_, 50, 200, "%d%%", ImGuiSliderFlags_None))
    {
        iconSpacingScale_ = displaySpacingPct_ / 100.0f;
        markChanged();
    }
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##IconSpacingDefault").c_str(), ImVec2(resetW, 0)))
    {
        displaySpacingPct_ = 100;
        iconSpacingScale_ = 1.0f;
        markChanged();
    }
    BeginSettingRow(_L("app.settings.title_font_size"), sliderActionW);
    ImGui::SetNextItemWidth(actionSliderW);
    if (ImGui::SliderFloat("##ItemFontSize", &itemFontSize_,
        10.0f, 24.0f, "%.1f pt"))
        markChanged();
    ImGui::SameLine();
    if (BlueButton((std::string(_L("app.settings.restore_default")) +
        "##ItemFontSizeDefault").c_str(), ImVec2(resetW, 0)))
    {
        itemFontSize_ = 14.0f;
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
    if (DrawSettingCheckbox(_L("app.settings.icon_beautify"), "##IconBeautifyEnabled",
        &iconBeautifyEnabled_))
        markChanged();

    ImGui::BeginDisabled(!iconBeautifyEnabled_);
    const char* beautifyModeNames[] = {
        _L("app.settings.beautify_smart"),
        _L("app.settings.beautify_shrink_bg"),
    };
    BeginSettingRow(_L("app.settings.beautify_mode"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyMode", &iconBeautifyMode_,
        beautifyModeNames, static_cast<int>(sizeof(beautifyModeNames) / sizeof(beautifyModeNames[0]))))
    {
        markChanged();
    }

    const char* presetNames[] = {
        _L("app.settings.beautify_preset_default_gray"),
        _L("app.settings.beautify_preset_white_glow"),
        _L("app.settings.beautify_preset_blue_gradient"),
        _L("app.settings.beautify_preset_warm_gradient"),
        _L("app.settings.beautify_preset_dark_glass"),
        _L("app.settings.custom"),
    };
    constexpr int presetValues[] = { 1, 2, 3, 4, 5, 0 };
    int presetSelection = 0;
    for (int i = 0; i < IM_ARRAYSIZE(presetValues); ++i)
    {
        if (presetValues[i] == iconBeautifyBgPreset_)
        {
            presetSelection = i;
            break;
        }
    }
    BeginSettingRow(_L("app.settings.beautify_bg_preset"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyBgPreset", &presetSelection,
        presetNames, static_cast<int>(sizeof(presetNames) / sizeof(presetNames[0]))))
    {
        iconBeautifyBgPreset_ = presetValues[presetSelection];
        if (iconBeautifyBgPreset_ > 0)
            applyIconBeautifyPreset(iconBeautifyBgPreset_);
        markChanged();
    }

    if (iconBeautifyBgPreset_ == 0)
    {
    BeginSettingRow(_L("app.settings.default_bg"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    float bgStart[3] = { iconBeautifyBgStartR_, iconBeautifyBgStartG_, iconBeautifyBgStartB_ };
    if (ImGui::ColorEdit3("##IconBeautifyBgStart", bgStart, ImGuiColorEditFlags_NoInputs))
    {
        iconBeautifyBgPreset_ = 0;
        iconBeautifyBgStartR_ = bgStart[0];
        iconBeautifyBgStartG_ = bgStart[1];
        iconBeautifyBgStartB_ = bgStart[2];
        markChanged();
    }

    BeginSettingRow(_L("app.settings.bg_opacity_val"), sliderW);
    ImGui::SetNextItemWidth(sliderW);
    int bgOpacityPercent = static_cast<int>(std::round(iconBeautifyBgOpacity_ * 100.0f));
    if (ImGui::SliderInt("##IconBeautifyBgOpacity", &bgOpacityPercent, 0, 100, "%d%%"))
    {
        iconBeautifyBgOpacity_ = bgOpacityPercent / 100.0f;
        iconBeautifyBgPreset_ = 0;
        markChanged();
    }

    if (DrawSettingCheckbox(_L("app.settings.enable_gradient_bg"), "##IconBeautifyGradient",
        &iconBeautifyGradientEnabled_))
    {
        iconBeautifyBgPreset_ = 0;
        markChanged();
    }

    BeginSettingRow(_L("app.settings.gradient_end_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    ImGui::BeginDisabled(!iconBeautifyGradientEnabled_);
    float bgEnd[3] = { iconBeautifyBgEndR_, iconBeautifyBgEndG_, iconBeautifyBgEndB_ };
    if (ImGui::ColorEdit3("##IconBeautifyBgEnd", bgEnd, ImGuiColorEditFlags_NoInputs))
    {
        iconBeautifyBgPreset_ = 0;
        iconBeautifyBgEndR_ = bgEnd[0];
        iconBeautifyBgEndG_ = bgEnd[1];
        iconBeautifyBgEndB_ = bgEnd[2];
        markChanged();
    }

    const char* directionNames[] = {
        _L("app.settings.beautify_gradient_updown"),
        _L("app.settings.beautify_gradient_leftright"),
        _L("app.settings.beautify_gradient_topleft_bottomright"),
        _L("app.settings.beautify_gradient_bottomleft_topright"),
    };
    BeginSettingRow(_L("app.settings.beautify_gradient_dir"), controlW);
    ImGui::SetNextItemWidth(controlW);
    if (ImGui::Combo("##IconBeautifyGradientDirection", &iconBeautifyGradientDirection_,
        directionNames, static_cast<int>(sizeof(directionNames) / sizeof(directionNames[0]))))
    {
        iconBeautifyBgPreset_ = 0;
        markChanged();
    }
    ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
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

    ImGui::SeparatorText(_L("app.settings.category_settings"));
    ImGui::Spacing();

    const float tabFontWidth = inputW;
    BeginSettingRow(_L("app.settings.tab_font_size"), tabFontWidth);
    ImGui::SetNextItemWidth(tabFontWidth);
    if (ImGui::SliderFloat("##CategoryTabFontSize", &categorySettings_.tabFontSize, 10.0f, 22.0f, "%.0f"))
        markChanged();

    ImGui::Spacing();
    DrawSettingSection(_L("app.settings.category_type"),
        _L("app.settings.category_hint"));
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

    ImGui::SeparatorText(_L("app.settings.add_category"));
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

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.save_settings"));
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
        if (presetIds[presetIndex] == kAppearancePresetCustom)
        {
            switch (NormalizeAppearancePresetId(previousPreset))
            {
            case kAppearancePresetLight: generalSettings_.quickNavTheme = 1; break;
            case kAppearancePresetAcrylicDark: generalSettings_.quickNavTheme = 2; break;
            case kAppearancePresetAcrylicLight: generalSettings_.quickNavTheme = 3; break;
            default: generalSettings_.quickNavTheme = 0; break;
            }
            generalSettingsDirty_ = true;
            personalization_.backgroundPreset = kAppearancePresetCustom;
        }
        else
        {
            personalization_ = presetForId(presetIds[presetIndex]);
        }
        personalization_.cornerRadius = cornerRadius;
        personalization_.barHeight = barHeight;
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

    BeginSettingRow(_L("app.settings.component_bg"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    float bgColor[3] = { personalization_.widgetBgR, personalization_.widgetBgG, personalization_.widgetBgB };
    if (ImGui::ColorEdit3("##WidgetBgColor", bgColor, ImGuiColorEditFlags_NoInputs))
    {
        personalization_.widgetBgR = bgColor[0]; personalization_.widgetBgG = bgColor[1];
        personalization_.widgetBgB = bgColor[2];
        markChanged(false);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && personalizationDirty_)
        personalizationSaveRequested_ = true;

    BeginSettingRow(_L("app.settings.component_border"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
    float borderColor[3] = { personalization_.widgetBorderR, personalization_.widgetBorderG, personalization_.widgetBorderB };
    if (ImGui::ColorEdit3("##WidgetBorderColor", borderColor, ImGuiColorEditFlags_NoInputs))
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
            BeginSettingRow(_L("app.settings.bg_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
            if (ImGui::ColorEdit3("##Background", background, ImGuiColorEditFlags_NoInputs))
            {
                style.widgetBgR = background[0];
                style.widgetBgG = background[1];
                style.widgetBgB = background[2];
                changed = true;
            }

            float border[3] = { style.widgetBorderR, style.widgetBorderG, style.widgetBorderB };
            BeginSettingRow(_L("app.settings.border_color"), ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x);
            if (ImGui::ColorEdit3("##Border", border, ImGuiColorEditFlags_NoInputs))
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
        default:                            taskbarThemeMode = 2; break;
        }
    }

    BeginSettingRow(_L("app.settings.taskbar_theme"), controlW,
        _L("app.settings.taskbar_theme_hint"));
    const char* taskbarThemeNames[] = {
        _L("app.settings.taskbar_no_beautify"), _L("app.settings.taskbar_follow_global"),
        _L("app.settings.dark"), _L("app.settings.light"), _L("app.settings.dark_glass"), _L("app.settings.light_glass"),
        _L("app.settings.dark_acrylic"), _L("app.settings.light_acrylic"), _L("app.settings.custom")
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
        }
        dockSettingsDirty_ = true;
    }

    if (taskbarThemeMode != 0)
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

    BeginSettingRow(_L("app.settings.widget_content_theme"), controlW);
        if (taskbarThemeMode == 8)
        {
            const char* ctNames[] = { _L("app.settings.light"), _L("app.settings.dark") };
            int ct = dockSettings_.systemTaskbarContentTheme;
            if (ct < 0)
                ct = dockSettings_.systemTaskbarAppearance.contentTheme;
            ImGui::SetNextItemWidth(controlW);
            if (ImGui::Combo("##ContentThemeTaskbar", &ct,
                ctNames, IM_ARRAYSIZE(ctNames)))
            {
                dockSettings_.systemTaskbarContentTheme = ct;
                dockSettingsDirty_ = true;
            }
        }
        else
        {
            const char* ctNames[] = { _L("app.settings.taskbar_follow_theme"), _L("app.settings.light"), _L("app.settings.dark") };
            int ct = dockSettings_.systemTaskbarContentTheme + 1;
            ImGui::SetNextItemWidth(controlW);
            if (ImGui::Combo("##ContentThemeTaskbar", &ct,
                ctNames, IM_ARRAYSIZE(ctNames)))
            {
                dockSettings_.systemTaskbarContentTheme = ct - 1;
                dockSettingsDirty_ = true;
            }
        }

        if (taskbarThemeMode == 8)
        {
            ImGui::Indent(8.0f * dpiScale_);
            if (drawOverrideAdvanced(
                dockSettings_.systemTaskbarAppearance,
                "OverrideAdvanced"))
                dockSettingsDirty_ = true;
            ImGui::Unindent(8.0f * dpiScale_);
        }
    }
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

/**
 * @brief 绘制"调试"页面。
 *
 * 提供以下功能：
 * - 组件错误记录列表（支持复制全部 / 清空全部 / 逐条点击复制）
 * - 组件诊断信息（列出已加载的 Lua 组件，显示状态、权限、最近错误与日志）
 * - 每项诊断支持重新加载组件按钮
 */
void SettingsWindow::DrawDebugPage()
{
    const float pad = 16.0f * dpiScale_;
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x);
    pageSize.y = std::max(1.0f, pageSize.y);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##DebugPageInner", pageSize,
        ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    ImGui::Text("%s", _L("app.settings.debug_page"));
    ImGui::Separator();
    ImGui::Spacing();

    if (DrawCollapsingHeaderWithHelp(_L("app.settings.crash_test"),
        _L("app.settings.crash_test_desc")))
    {
        ImGui::Spacing();
        if (BlueButton(_L("app.settings.trigger_crash")))
            TriggerCrashForTesting();
        ImGui::Spacing();
    }

    if (BlueButton(_L("app.settings.open_widget_folder")))
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        PathRemoveFileSpecW(exePath);
        PathAppendW(exePath, L"widgets");
        ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOW);
    }
    ImGui::Spacing();

    if (widgetEngine_)
    {
        const std::string snapshotError = widgetEngine_->GetSystemSnapshotError();
        if (ImGui::CollapsingHeader(_L("app.settings.audio_devices")))
        {
            if (snapshotError.empty())
                ImGui::TextDisabled("%s", _L("app.settings.snapshot_service_ok"));
            else
                ImGui::TextWrapped(_L("app.settings.snapshot_recent_error"),
                    snapshotError.c_str());
        }
        ImGui::Spacing();
    }

    if (DrawCollapsingHeaderWithHelp(_L("app.settings.fa_icons"),
        _L("app.settings.fa_icon_hint")))
    {
        if (faDebugFont_ && faDebugCodepoints_.empty())
        {
            for (unsigned int codepoint = 0xE000; codepoint <= 0xF8FF; ++codepoint)
            {
                if (faDebugFont_->IsGlyphInFont(static_cast<ImWchar>(codepoint)))
                    faDebugCodepoints_.push_back(codepoint);
            }
        }

        if (!faDebugFont_ || faDebugCodepoints_.empty())
        {
            ImGui::TextDisabled("%s", _L("app.settings.fa_not_found"));
        }
        else
        {
            ImGui::Text(_L("app.settings.fa_valid_chars"),
                static_cast<int>(faDebugCodepoints_.size()));
            const float buttonSize = 38.0f * dpiScale_;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const int columns = std::max(1, static_cast<int>(
                ImGui::GetContentRegionAvail().x / (buttonSize + spacing)));

            ImGui::BeginChild("##FontAwesomeGlyphs", ImVec2(0, 220.0f * dpiScale_), true);
            for (size_t i = 0; i < faDebugCodepoints_.size(); ++i)
            {
                unsigned int codepoint = faDebugCodepoints_[i];
                wchar_t wide[2] = { static_cast<wchar_t>(codepoint), L'\0' };
                std::string glyph = WideToUtf8(wide);
                std::string buttonLabel = glyph + "##fa" + std::to_string(codepoint);

                ImGui::PushFont(faDebugFont_, 18.0f * dpiScale_);
                bool clicked = ImGui::Button(buttonLabel.c_str(), ImVec2(buttonSize, buttonSize));
                ImGui::PopFont();
                if (clicked)
                    ImGui::SetClipboardText(glyph.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(_L("app.settings.fa_copy_tooltip"), codepoint);

                if ((static_cast<int>(i) + 1) % columns != 0)
                    ImGui::SameLine();
            }
            ImGui::EndChild();
        }
        ImGui::Spacing();
    }

    ImGui::Separator();

    std::vector<WidgetErrorEntry> errors;
    if (widgetEngine_)
        errors = widgetEngine_->GetWidgetErrors();
    ImGui::Text(_L("app.settings.error_count"), static_cast<int>(errors.size()));
    ImGui::SameLine();
    if (BlueButton(_L("app.settings.copy_all")))
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
    if (BlueButton(_L("app.settings.clear_all")))
    {
        if (widgetEngine_)
            widgetEngine_->ClearWidgetErrors();
        errors.clear();
    }

    ImGui::Spacing();

    if (errors.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_widget_errors"));
        ImGui::Spacing();
    }
    else
    {
        ImGui::BeginChild("##DebugScroll", ImVec2(0, 160.0f * dpiScale_), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& e : errors)
        {
            std::string itemText = "[" + e.key + "]\n" + e.message;
            if (ImGui::Selectable(itemText.c_str(), false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 0)))
                ImGui::SetClipboardText(itemText.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _L("app.settings.copy_error"));
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Text("%s", _L("app.settings.widget_diagnostic"));

    std::vector<WidgetDiagnosticEntry> diagnostics;
    if (widgetEngine_)
        diagnostics = widgetEngine_->GetWidgetDiagnostics();

    if (diagnostics.empty())
    {
        ImGui::TextDisabled("%s", _L("app.settings.no_widgets_loaded"));
    }
    else
    {
        if (BlueButton(_L("app.settings.copy_diag")))
        {
            std::string text;
            for (const auto& d : diagnostics)
            {
                text += "[" + WideToUtf8(d.widgetId) + "] " + d.name + "\n";
                text += std::string("valid=") + (d.valid ? "true" : "false") +
                    ", manifest=" + (d.hasManifest ? "true" : "false") + "\n";
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
                text += "\n";
            }
            ImGui::SetClipboardText(text.c_str());
        }

        ImGui::BeginChild("##WidgetDiagnostics", ImVec2(0, 180.0f * dpiScale_), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto& d : diagnostics)
        {
            std::string header = "[" + WideToUtf8(d.widgetId) + "] " + d.name;
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
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
                if (BlueButton((std::string(_L("app.settings.reload")) + "##" +
                    WideToUtf8(d.widgetId)).c_str(), ImVec2(96, 0)))
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
                        ImGui::TextWrapped("[%s] %s", log.level.c_str(), log.message.c_str());
                }
            }
            ImGui::Separator();
        }
        ImGui::EndChild();
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
    updateCheckStatus_ = "checking";
    updateCheckStatusKey_.clear();
    updateCheckStatusArgument_.clear();
    updateAvailable_ = false;

    HINTERNET session = WinHttpOpen(L"SnowDesktop/" SNOWDESKTOP_VERSION,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session)
    {
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_http_init_failed");
        updateCheckStatus_ = _L("app.settings.update_http_init_failed");
        return;
    }
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength   = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength  = (DWORD)-1;
    std::wstring apiUrl = L"https://api.github.com/repos/FreeFallingSnow/SnowDesktop_Release/releases/latest";
    if (!WinHttpCrackUrl(apiUrl.c_str(), 0, 0, &urlComp))
    {
        WinHttpCloseHandle(session);
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_url_parse_failed");
        updateCheckStatus_ = _L("app.settings.update_url_parse_failed");
        return;
    }

    HINTERNET connect = WinHttpConnect(session,
        std::wstring(urlComp.lpszHostName, urlComp.dwHostNameLength).c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_connect_failed");
        updateCheckStatus_ = _L("app.settings.update_connect_failed");
        return;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET",
        std::wstring(urlComp.lpszUrlPath, urlComp.dwUrlPathLength).c_str(),
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_request_failed");
        updateCheckStatus_ = _L("app.settings.update_request_failed");
        return;
    }

    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nUser-Agent: SnowDesktop\r\n";
    if (!WinHttpSendRequest(request, headers, (DWORD)wcslen(headers), nullptr, 0, 0, 0))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_send_failed");
        updateCheckStatus_ = _L("app.settings.update_send_failed");
        return;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_receive_failed");
        updateCheckStatus_ = _L("app.settings.update_receive_failed");
        return;
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::vector<char> chunk(available + 1);
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        body.append(chunk.data(), read);
        if (body.size() > 128 * 1024) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (body.empty())
    {
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_empty_response");
        updateCheckStatus_ = _L("app.settings.update_empty_response");
        return;
    }

    auto extractJsonString = [](const std::string& json, const char* field) -> std::string {
        std::string key = "\"" + std::string(field) + "\":\"";
        size_t pos = json.find(key);
        if (pos == std::string::npos) return {};
        pos += key.size();
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    };

    std::string tag = extractJsonString(body, "tag_name");
    if (tag.empty())
    {
        updateCheckStatusKey_ = L10N_KEY("app.settings.update_parse_failed");
        updateCheckStatus_ = _L("app.settings.update_parse_failed");
        return;
    }

    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
        tag = tag.substr(1);

    std::string htmlUrl = extractJsonString(body, "html_url");

    auto compareVersion = [](const std::string& a, const std::string& b) -> int {
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

    int cmp = compareVersion(SNOWDESKTOP_VERSION, tag);
    if (cmp >= 0)
    {
        updateAvailable_ = false;
        updateCheckStatusKey_ = L10N_KEY("app.settings.already_latest");
        updateCheckStatus_ = _L("app.settings.already_latest");
    }
    else
    {
        updateAvailable_ = true;
        updateCheckStatusKey_ = L10N_KEY("app.settings.new_version");
        updateCheckStatusArgument_ = tag;
        updateCheckStatus_ = _LF("app.settings.new_version", tag);
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

    ImGui::Dummy(ImVec2(0, 4));
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

    if (!updateCheckStatus_.empty())
    {
        ImGui::SameLine();
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

    ImGui::SameLine();
    float updateButtonW = SettingButtonWidth(_L("app.settings.check_update")) + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - updateButtonW);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.45f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.55f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(_L("app.settings.check_update"), ImVec2(updateButtonW, 0)))
    {
        PerformUpdateCheck();
    }
    ImGui::PopStyleColor(4);

    if (!updateCheckStatus_.empty() &&
        updateCheckStatus_ != "checking" && updateAvailable_)
    {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", _L("app.settings.download_latest_hint"));
        ImGui::Spacing();
        ImGui::Text("    ");
        ImGui::SameLine();
        std::string dlLabel = downloadUrl_.empty() ?
            "https://github.com/FreeFallingSnow/SnowDesktop_Release/releases" : downloadUrl_;
        ImGui::TextColored(ImVec4(0.30f, 0.60f, 0.95f, 1.00f), "%s", dlLabel.c_str());
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("%s", dlLabel.c_str());
        }
        if (ImGui::IsItemClicked())
        {
            ShellExecuteW(nullptr, L"open", Utf8ToWide(dlLabel).c_str(), nullptr, nullptr, SW_SHOW);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText(_L("app.settings.third_party_libs"));
    ImGui::Spacing();

    ImGui::Text("    Everything SDK");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (C) 2016 David Carpenter");

    ImGui::Text("    Dear ImGui");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2014-2025 Omar Cornut");

    ImGui::Text("    Lua");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (C) 1994-2024 Lua.org, PUC-Rio");

    ImGui::Text("    spdlog");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2016-present, Gabi Melman");

    ImGui::Text("    pinyin-data");
    ImGui::SameLine();
    ImGui::TextDisabled("(MIT)");
    ImGui::TextDisabled("        Copyright (c) 2016 mozillazg");

    ImGui::EndChild();
}

// ════════════════════════════════════════════════════════════════
//  布局备份：目录、列举、保存、恢复、删除
// ════════════════════════════════════════════════════════════════

/**
 * @brief 获取备份文件存储目录路径。
 *
 * 目录位于可执行文件所在目录下 data\backups 子文件夹。
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
    GetClientRect(hwnd_, &cr);
    windowWidth_ = (cr.right - cr.left > 1) ? (cr.right - cr.left) : 1;
    windowHeight_ = (cr.bottom - cr.top > 1) ? (cr.bottom - cr.top) : 1;

    if (!context_)
        device_->GetImmediateContext(&context_);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();

    if (swapChain_)
    {
        const HRESULT resizeResult = swapChain_->ResizeBuffers(0,
            static_cast<UINT>(windowWidth_), static_cast<UINT>(windowHeight_),
            DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(resizeResult))
            swapChain_.Reset();
    }

    if (!swapChain_)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device_.As(&dxgiDevice))) return false;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = static_cast<UINT>(windowWidth_);
        desc.Height = static_cast<UINT>(windowHeight_);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_,
                &desc, nullptr, nullptr, &swapChain_)))
            return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    if (FAILED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_))) return false;

    if (ImGui::GetCurrentContext() != nullptr)
    {
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
 * 并包含简体中文常用字形范围。
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
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    if (enable)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        RegSetValueExW(key, L"SnowDesktop", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(path),
            static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, L"SnowDesktop");
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

    if (g_settingsWindow != nullptr && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE)
    {
        g_settingsWindow->RequestClose();
        return 0;
    }

    if (g_settingsWindow != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_TIMER:
        if (g_settingsWindow != nullptr && wParam == kSettingsRefreshTimerId)
        {
            // 低频更新后端状态、调试采样和文本光标等动态内容。
            g_settingsWindow->renderRequested_ = true;
            return 0;
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
