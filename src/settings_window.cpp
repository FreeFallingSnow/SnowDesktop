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
#include "resource.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

SettingsWindow* g_settingsWindow = nullptr;

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
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.96f, 0.96f, 0.97f, 1.00f);
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
        L"SnowDesktop 设置",
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
    LoadNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);

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
    g_settingsWindow = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupSwapChain();
    if (hwnd_ != nullptr) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
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
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    BringWindowToTop(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
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
    pendingClose_ = true;
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
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, 0), true);
        DrawSidebar();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##Content", ImVec2(0, 0), true);
        switch (activePage_)
        {
        case 0: DrawGeneralPage(); break;
        case 1: DrawPersonalizationPage(); break;
        case 2: DrawBackupPage(); break;
        case 4: DrawAboutPage(); break;
        case 3:
            if (debugUnlocked_)
                DrawDebugPage();
            else
                DrawAboutPage();
            break;
        }
        ImGui::EndChild();
    }

    ImGui::End();

    if (personalizationDirty_)
    {
        SavePersonalization(GetPersonalizationPath().c_str(), personalization_);
        personalizationDirty_ = false;
    }

    if (navigationSettingsDirty_)
    {
        SaveNavigationSettings(GetNavigationSettingsPath().c_str(), navigationSettings_);
        navigationSettingsDirty_ = false;
        if (navigationSettingsChangedCallback_)
            navigationSettingsChangedCallback_();
    }

    if (invalidateCallback_)
        invalidateCallback_();

    // Exit confirmation modal
    if (showExitConfirm_)
    {
        ImGui::OpenPopup("退出确认");
        if (ImGui::BeginPopupModal("退出确认", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("确定要退出 SnowDesktop 吗？");
            ImGui::Text("退出后将恢复 Windows 原生桌面。");
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool okClicked = ImGui::Button("确定退出", ImVec2(120, 0));
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
            bool cancelClicked = ImGui::Button("取消", ImVec2(80, 0));
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
    swapChain_->Present(1, 0);
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

    SideButton(0, "通用");
    SideButton(1, "个性化");
    SideButton(2, "布局备份");
    SideButton(4, "关于");
    if (debugUnlocked_)
        SideButton(3, "调试");

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
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x - pad);
    pageSize.y = std::max(1.0f, pageSize.y - pad);
    ImGui::BeginChild("##BackupPageInner", pageSize, ImGuiChildFlags_None);
    ImGui::Text("布局备份与恢复");
    ImGui::Separator();
    ImGui::Spacing();

    // Save section
    ImGui::Text("保存当前布局");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##BackupName", "备份名称（可选）", backupNameBuf_, sizeof(backupNameBuf_));

    ImGui::SameLine();
    if (BlueButton("保存备份"))
    {
        std::wstring name = Utf8ToWide(backupNameBuf_);
        if (name.empty()) name = MakeBackupTimestampName();
        if (SaveBackup(name))
        {
            backupNameBuf_[0] = '\0';
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // List existing backups
    ImGui::Text("已保存的备份");
    ImGui::Spacing();

    std::vector<LayoutBackup> backups = ListBackups();
    if (backups.empty())
    {
        ImGui::TextDisabled("暂无备份");
    }
    else
    {
        ImGui::BeginChild("##BackupList", ImVec2(0, 0), true);

        for (size_t i = 0; i < backups.size(); ++i)
        {
            const auto& b = backups[i];
            std::string label = WideToUtf8(b.displayName) + "##" + std::to_string(i);

            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowOverlap))
            {
                // Click to select
            }

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130);
            ImGui::PushID(static_cast<int>(i));

            if (BlueButton("恢复", ImVec2(56, 0)))
            {
                if (RestoreBackup(b.filename) && reloadCallback_)
                    reloadCallback_();
            }
            ImGui::SameLine();
            if (BlueButton("删除", ImVec2(56, 0)))
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
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x - pad);
    pageSize.y = std::max(1.0f, pageSize.y - pad);
    ImGui::BeginChild("##GeneralPageInner", pageSize, ImGuiChildFlags_None);

    ImGui::Text("通用设置");
    ImGui::Separator();
    ImGui::Spacing();

    // Auto-start
    bool autoStart = IsAutoStartEnabled();
    if (ImGui::Checkbox("开机自启", &autoStart))
    {
        SetAutoStart(autoStart);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(随 Windows 启动 SnowDesktop)");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("快捷导航");
    ImGui::Spacing();

    if (ImGui::Checkbox("启用全局快捷导航", &navigationSettings_.enabled))
        navigationSettingsDirty_ = true;

    ImGui::BeginDisabled(!navigationSettings_.enabled);

    bool ctrl = (navigationSettings_.modifiers & MOD_CONTROL) != 0;
    bool alt = (navigationSettings_.modifiers & MOD_ALT) != 0;
    bool shift = (navigationSettings_.modifiers & MOD_SHIFT) != 0;
    bool win = (navigationSettings_.modifiers & MOD_WIN) != 0;
    bool modifiersChanged = false;
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
    ImGui::SetNextItemWidth(160.0f * dpiScale_);
    if (ImGui::Combo("主键", &selected, [](void* data, int idx, const char** outText) {
            auto* opts = static_cast<const HotkeyOption*>(data);
            *outText = opts[idx].label;
            return true;
        }, const_cast<HotkeyOption*>(options), static_cast<int>(optionCount)))
    {
        navigationSettings_.virtualKey = options[selected].virtualKey;
        navigationSettingsDirty_ = true;
    }

    std::wstring hotkeyText = FormatNavigationHotkey(navigationSettings_);
    ImGui::TextDisabled("当前快捷键: %s", WideToUtf8(hotkeyText).c_str());
    ImGui::EndDisabled();

    ImGui::EndChild();
}

/**
 * @brief 绘制"个性化设置"页面。
 *
 * 提供以下定制能力：
 * - 预设快速切换（恢复默认暗色 / 浅色预设）
 * - 组件背景色与边框颜色选取
 * - 整体不透明度滑条
 * - 底部渐变开关与渐变结束透明度控制
 * - 所有修改标记 personalizationDirty_，在 Render() 末尾持久化
 */
void SettingsWindow::DrawPersonalizationPage()
{
    const float pad = 16.0f * dpiScale_;
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x - pad);
    pageSize.y = std::max(1.0f, pageSize.y - pad);
    ImGui::BeginChild("##PersonalizationPageInner", pageSize, ImGuiChildFlags_None);

    auto nearlyEqual = [](float a, float b) {
        return std::fabs(a - b) < 0.001f;
    };
    auto sameSettings = [&](const PersonalizationSettings& a, const PersonalizationSettings& b) {
        return nearlyEqual(a.widgetBgR, b.widgetBgR) &&
            nearlyEqual(a.widgetBgG, b.widgetBgG) &&
            nearlyEqual(a.widgetBgB, b.widgetBgB) &&
            nearlyEqual(a.widgetBorderR, b.widgetBorderR) &&
            nearlyEqual(a.widgetBorderG, b.widgetBorderG) &&
            nearlyEqual(a.widgetBorderB, b.widgetBorderB) &&
            nearlyEqual(a.widgetAlpha, b.widgetAlpha) &&
            nearlyEqual(a.gradientEndA, b.gradientEndA);
    };
    auto percentText = [](float value) {
        return std::to_string(static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * 100.0f))) + "%";
    };
    auto drawSectionTitle = [](const char* title) {
        ImGui::Spacing();
        ImGui::Text("%s", title);
        ImGui::Separator();
        ImGui::Spacing();
    };

    const bool modifiedFromDefault = !sameSettings(personalization_, PersonalizationSettings::DarkPreset());

    ImGui::Text("个性化设置");
    if (modifiedFromDefault)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("已修改");
    }
    ImGui::Separator();
    ImGui::Spacing();

    drawSectionTitle("预设");
    if (BlueButton("恢复默认", ImVec2(96, 0)))
    {
        personalization_ = PersonalizationSettings::DarkPreset();
        personalizationDirty_ = true;
    }
    ImGui::SameLine();
    if (BlueButton("浅色预设", ImVec2(96, 0)))
    {
        personalization_ = PersonalizationSettings::LightPreset();
        personalizationDirty_ = true;
    }

    drawSectionTitle("组件颜色");
    const float labelW = 110.0f * dpiScale_;
    const float colorW = 210.0f * dpiScale_;

    ImGui::Text("组件背景");
    ImGui::SameLine(labelW);
    float bgColor[3] = { personalization_.widgetBgR, personalization_.widgetBgG, personalization_.widgetBgB };
    ImGui::SetNextItemWidth(colorW);
    if (ImGui::ColorEdit3("##WidgetBgColor", bgColor, ImGuiColorEditFlags_NoInputs))
    {
        personalization_.widgetBgR = bgColor[0]; personalization_.widgetBgG = bgColor[1];
        personalization_.widgetBgB = bgColor[2];
        personalizationDirty_ = true;
    }

    ImGui::Text("组件边框");
    float borderColor[3] = { personalization_.widgetBorderR, personalization_.widgetBorderG, personalization_.widgetBorderB };
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(colorW);
    if (ImGui::ColorEdit3("##WidgetBorderColor", borderColor, ImGuiColorEditFlags_NoInputs))
    {
        personalization_.widgetBorderR = borderColor[0]; personalization_.widgetBorderG = borderColor[1];
        personalization_.widgetBorderB = borderColor[2];
        personalizationDirty_ = true;
    }

    drawSectionTitle("透明度与渐变");
    const float sliderW = 260.0f * dpiScale_;

    ImGui::Text("整体不透明度");
    ImGui::SameLine(labelW);
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("##WidgetAlpha", &personalization_.widgetAlpha, 0.0f, 1.0f, ""))
        personalizationDirty_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", percentText(personalization_.widgetAlpha).c_str());

    bool gradientEnabled = personalization_.gradientEndA > 0.001f;
    bool gradientToggle = gradientEnabled;
    if (ImGui::Checkbox("启用底部渐变", &gradientToggle))
    {
        personalization_.gradientEndA = gradientToggle ? PersonalizationSettings::DarkPreset().gradientEndA : 0.0f;
        personalizationDirty_ = true;
        gradientEnabled = gradientToggle;
    }

    ImGui::Text("渐变结束透明度");
    ImGui::SameLine(labelW);
    ImGui::BeginDisabled(!gradientEnabled);
    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("##GradientEndAlpha", &personalization_.gradientEndA, 0.0f, 1.0f, ""))
        personalizationDirty_ = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", percentText(personalization_.gradientEndA).c_str());

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
    // Back button — white text on blue
    if (BlueButton("返回主界面", ImVec2(100, 0)))
    {
        editingWidgetIndex_ = static_cast<size_t>(-1);
        return;
    }

    ImGui::SameLine();
    ImGui::Text("组件编辑: %s", WideToUtf8(editingWidgetName_).c_str());
    ImGui::Separator();

    if (widgetEngine_)
    {
        // Make input cursor clearly black
        ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

        widgetEngine_->EnsureWidgetLoaded(editingWidgetId_, editingScriptPath_);
        widgetEngine_->RenderWidgetEditor(editingWidgetId_, editingWidgetName_);

        ImGui::PopStyleColor(1);
    }
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
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x - pad);
    pageSize.y = std::max(1.0f, pageSize.y - pad);
    ImGui::BeginChild("##DebugPageInner", pageSize, ImGuiChildFlags_None);

    ImGui::Text("调试页");
    ImGui::Separator();
    ImGui::Spacing();

    if (BlueButton("打开组件文件夹"))
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
        if (ImGui::CollapsingHeader("系统与媒体采样"))
        {
            if (snapshotError.empty())
                ImGui::TextDisabled("采样服务运行正常。");
            else
                ImGui::TextWrapped("最近采样错误:\n%s", snapshotError.c_str());
        }
        ImGui::Spacing();
    }

    if (ImGui::CollapsingHeader("Font Awesome 图标字符"))
    {
        ImGui::TextDisabled("点击图标复制字符，可直接粘贴到 Lua 菜单项的 icon 字段。");

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
            ImGui::TextDisabled("未找到可用的 Font Awesome 字形。");
        }
        else
        {
            ImGui::Text("可用字符: %d", static_cast<int>(faDebugCodepoints_.size()));
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
                    ImGui::SetTooltip("U+%04X\n点击复制", codepoint);

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
    ImGui::Text("错误记录: %d", static_cast<int>(errors.size()));
    ImGui::SameLine();
    if (BlueButton("复制全部"))
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
    if (BlueButton("清空全部"))
    {
        if (widgetEngine_)
            widgetEngine_->ClearWidgetErrors();
        errors.clear();
    }

    ImGui::Spacing();

    if (errors.empty())
    {
        ImGui::TextDisabled("当前没有组件错误记录。");
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
                ImGui::SetTooltip("点击复制这一条错误");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Text("组件诊断");

    std::vector<WidgetDiagnosticEntry> diagnostics;
    if (widgetEngine_)
        diagnostics = widgetEngine_->GetWidgetDiagnostics();

    if (diagnostics.empty())
    {
        ImGui::TextDisabled("当前没有已加载的 Lua 组件。");
    }
    else
    {
        if (BlueButton("复制诊断信息"))
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
                ImGui::Text("脚本: %s", WideToUtf8(d.scriptPath).c_str());
                ImGui::Text("状态: %s | Manifest: %s", d.valid ? "有效" : "无效",
                    d.hasManifest ? "是" : "否");
                std::string perms;
                for (size_t i = 0; i < d.permissions.size(); ++i)
                {
                    if (i > 0) perms += ", ";
                    perms += d.permissions[i];
                }
                ImGui::Text("权限: %s", perms.empty() ? "(无)" : perms.c_str());
                if (!d.lastError.empty())
                    ImGui::TextWrapped("最近错误: %s", d.lastError.c_str());
                if (BlueButton(("重新加载##" + WideToUtf8(d.widgetId)).c_str(), ImVec2(96, 0)))
                {
                    if (widgetEngine_)
                        widgetEngine_->ReloadWidget(d.widgetId);
                    if (invalidateCallback_)
                        invalidateCallback_();
                }
                if (!d.logs.empty())
                {
                    ImGui::Text("最近日志");
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

    HINTERNET session = WinHttpOpen(L"SnowDesktop/" SNOWDESKTOP_VERSION,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!session)
    {
        updateCheckStatus_ = "网络连接失败（无法初始化 HTTP）";
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
        updateCheckStatus_ = "URL 解析失败";
        return;
    }

    HINTERNET connect = WinHttpConnect(session,
        std::wstring(urlComp.lpszHostName, urlComp.dwHostNameLength).c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        updateCheckStatus_ = "连接服务器失败";
        return;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET",
        std::wstring(urlComp.lpszUrlPath, urlComp.dwUrlPathLength).c_str(),
        nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        updateCheckStatus_ = "创建请求失败";
        return;
    }

    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nUser-Agent: SnowDesktop\r\n";
    if (!WinHttpSendRequest(request, headers, (DWORD)wcslen(headers), nullptr, 0, 0, 0))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        updateCheckStatus_ = "发送请求失败";
        return;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        updateCheckStatus_ = "接收响应失败";
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
        updateCheckStatus_ = "服务器未返回数据";
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
        updateCheckStatus_ = "无法解析版本信息";
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
        updateCheckStatus_ = "已是最新版本";
    }
    else
    {
        updateAvailable_ = true;
        updateCheckStatus_ = "发现新版本 v" + tag;
    }
}

void SettingsWindow::DrawAboutPage()
{
    const float pad = 16.0f * dpiScale_;
    ImGui::SetCursorPos(ImVec2(pad, pad));
    ImVec2 pageSize = ImGui::GetContentRegionAvail();
    pageSize.x = std::max(1.0f, pageSize.x - pad);
    pageSize.y = std::max(1.0f, pageSize.y - pad);
    ImGui::BeginChild("##AboutPageInner", pageSize, ImGuiChildFlags_None);

    ImGui::Text("关于 SnowDesktop");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("SnowDesktop 是一款 Windows 桌面增强工具，提供自定义桌面布局、"
        "图标网格管理、集合组件、桌面文件分类等功能，让桌面更整洁高效。");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("作者");
    ImGui::Spacing();
    ImGui::Text("    逍遥飘雪（郭云哲）");
    ImGui::Spacing();
    ImGui::Text("个人主页：");
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
    LinkButton("抖音", "https://www.douyin.com/user/MS4wLjABAAAA-O94bwF3BK2sj9JOwM2R2zRlTOiYf4BbaSyIF9DZPyM");
    ImGui::Dummy(ImVec2(0, 2));
    LinkButton("小红书", "https://www.xiaohongshu.com/user/profile/6819eed7000000000403bf0e");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("项目地址：");
    ImGui::Spacing();
    LinkButton("SnowDesktop_Release", "https://github.com/FreeFallingSnow/SnowDesktop_Release");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("SnowDesktop v" SNOWDESKTOP_VERSION);
    if (ImGui::IsItemClicked())
    {
        if (!debugUnlocked_)
        {
            ++versionClickCount_;
            if (versionClickCount_ >= 5)
            {
                debugUnlocked_ = true;
                activePage_ = 3;
            }
        }
    }

    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.45f, 0.90f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.55f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.35f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("检查更新", ImVec2(120 * dpiScale_, 0)))
    {
        PerformUpdateCheck();
    }
    ImGui::PopStyleColor(4);

    if (!updateCheckStatus_.empty())
    {
        ImGui::SameLine();
        if (updateCheckStatus_ == "checking")
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "检查中...");
        }
        else if (updateAvailable_)
        {
            ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.40f, 1.0f), "%s", updateCheckStatus_.c_str());
            ImGui::Spacing();
            ImGui::TextWrapped("请前往项目地址下载最新版本：");
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
        else
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", updateCheckStatus_.c_str());
        }
    }

    ImGui::EndChild();
}

// ════════════════════════════════════════════════════════════════
//  布局备份：目录、列举、保存、恢复、删除
// ════════════════════════════════════════════════════════════════

/**
 * @brief 获取备份文件存储目录路径。
 *
 * 目录位于可执行文件所在目录下的 "backups" 子文件夹。
 * @return 备份目录的完整宽字符串路径
 */
std::wstring SettingsWindow::GetBackupDir() const
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"backups");
    return path;
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
 * 将 SnowDesktop.layout.json 复制到 backups/ 下，
 * 备份文件名中不允许出现 : / \\ 字符（替换为 _），
 * 同名文件存在时自动在末尾追加递增序号。
 * @param name 备份名称
 * @return true 复制成功
 */
bool SettingsWindow::SaveBackup(const std::wstring& name)
{
    std::wstring backupDir = GetBackupDir();
    CreateDirectoryW(backupDir.c_str(), nullptr);

    wchar_t exeDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeDir, static_cast<DWORD>(std::size(exeDir)));
    PathRemoveFileSpecW(exeDir);

    std::wstring layoutPath = std::wstring(exeDir) + L"\\SnowDesktop.layout.json";
    std::wstring storagePath = std::wstring(exeDir) + L"\\SnowDesktop.storage.json";

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
    wchar_t exeDir[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exeDir, static_cast<DWORD>(std::size(exeDir)));
    PathRemoveFileSpecW(exeDir);

    std::wstring layoutPath = std::wstring(exeDir) + L"\\SnowDesktop.layout.json";
    std::wstring storagePath = std::wstring(exeDir) + L"\\SnowDesktop.storage.json";

    std::wstring backupPath = GetBackupDir() + L"\\" + filename;

    // Derive storage backup filename: replace .json with .storage.json
    std::wstring storageFilename = filename;
    if (storageFilename.size() > 5 && storageFilename.substr(storageFilename.size() - 5) == L".json")
        storageFilename = storageFilename.substr(0, storageFilename.size() - 5) + L".storage.json";
    std::wstring storageBackupPath = GetBackupDir() + L"\\" + storageFilename;

    // First save current layout before restoring.
    SaveBackup(MakeBackupTimestampName() + L"（恢复前备份）");

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
 * 根据窗口当前客户区尺寸重新创建交换链，
 * 同时更新 ImGui 的 DisplaySize。
 * 调用前会清理旧的交换链资源。
 * @return true 创建成功
 */
bool SettingsWindow::CreateSwapChain()
{
    RECT cr;
    GetClientRect(hwnd_, &cr);
    windowWidth_ = (cr.right - cr.left > 1) ? (cr.right - cr.left) : 1;
    windowHeight_ = (cr.bottom - cr.top > 1) ? (cr.bottom - cr.top) : 1;

    CleanupSwapChain();
    device_->GetImmediateContext(&context_);

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

    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_)))
        return false;

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
    if (g_settingsWindow != nullptr && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE)
    {
        g_settingsWindow->RequestClose();
        return 0;
    }

    if (g_settingsWindow != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_SIZE:
        if (g_settingsWindow != nullptr && wParam != SIZE_MINIMIZED)
        {
            g_settingsWindow->CreateSwapChain();
            g_settingsWindow->Render();
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
