#include "settings_window.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <shlwapi.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

SettingsWindow* g_settingsWindow = nullptr;

// ── Light theme colors ──────────────────────────────────────────
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
    c[ImGuiCol_Border]               = ImVec4(0.82f, 0.82f, 0.85f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.93f, 0.93f, 0.94f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.82f, 0.82f, 0.85f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.30f, 0.55f, 0.90f, 0.35f);
    c[ImGuiCol_Header]               = ImVec4(0.90f, 0.90f, 0.92f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.84f, 0.84f, 0.87f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.78f, 0.78f, 0.82f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.30f, 0.55f, 0.90f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.35f, 0.60f, 0.95f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.25f, 0.50f, 0.85f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.30f, 0.55f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.25f, 0.50f, 0.85f, 1.00f);
    c[ImGuiCol_Tab]                  = ImVec4(0.90f, 0.90f, 0.92f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.80f, 0.80f, 0.83f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
}

SettingsWindow::~SettingsWindow()
{
    Shutdown();
}

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
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SnowDesktopSettingsWindow";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"SnowDesktop 设置",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth_, windowHeight_,
        nullptr, nullptr, instance, this);

    if (hwnd_ == nullptr) return false;

    if (!CreateSwapChain()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    SetupLightTheme();

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());

    SetupFonts();

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

void SettingsWindow::Shutdown()
{
    g_settingsWindow = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupSwapChain();
    if (hwnd_ != nullptr) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void SettingsWindow::Show()
{
    if (hwnd_ != nullptr)
    {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
}

void SettingsWindow::Render()
{
    if (hwnd_ == nullptr || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) return;
    if (swapChain_ == nullptr) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
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

    DrawTitleBar();

    // Sidebar + Content layout
    const float sidebarW = 160.0f;
    ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, 0), true);
    DrawSidebar();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##Content", ImVec2(0, 0), ImGuiChildFlags_None);
    switch (activePage_)
    {
    case 0: DrawBackupPage(); break;
    case 1: DrawGeneralPage(); break;
    case 2: DrawAboutPage(); break;
    }
    ImGui::EndChild();

    ImGui::End();

    ImGui::Render();
    const float clearColor[4] = { 0.94f, 0.94f, 0.96f, 1.0f };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(1, 0);
}

// ── Title Bar ───────────────────────────────────────────────────
void SettingsWindow::DrawTitleBar()
{
    const float titleH = 32.0f;
    ImGui::BeginChild("##TitleBar", ImVec2(0, titleH), ImGuiChildFlags_None);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.y = titleH;

    // Background
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
        ImColor(0.94f, 0.94f, 0.96f));

    // Title text
    dl->AddText(ImVec2(p.x + 12, p.y + 7),
        ImColor(0.12f, 0.12f, 0.15f), "SnowDesktop 设置");

    // Close button
    float btnX = p.x + size.x - 40.0f;
    ImVec2 btnMin(btnX, p.y + 4);
    ImVec2 btnMax(btnX + 28, p.y + titleH - 4);
    bool btnHovered = ImGui::IsMouseHoveringRect(btnMin, btnMax);
    dl->AddRectFilled(btnMin, btnMax,
        btnHovered ? ImColor(0.90f, 0.20f, 0.20f) : ImColor(0, 0, 0, 0), 4.0f);
    dl->AddText(ImVec2(btnX + 8, p.y + 6), ImColor(0.40f, 0.40f, 0.45f), "X");

    if (btnHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        ShowWindow(hwnd_, SW_HIDE);

    // Drag region
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton("##TitleDrag", ImVec2(size.x - 44, titleH));
    if (ImGui::IsItemActive())
    {
        static POINT dragStart = {};
        static RECT dragStartRect = {};
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            GetCursorPos(&dragStart);
            GetWindowRect(hwnd_, &dragStartRect);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            POINT pt;
            GetCursorPos(&pt);
            SetWindowPos(hwnd_, nullptr,
                dragStartRect.left + pt.x - dragStart.x,
                dragStartRect.top + pt.y - dragStart.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
    }

    ImGui::EndChild();
}

// ── Sidebar ──────────────────────────────────────────────────────
void SettingsWindow::DrawSidebar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.85f, 0.88f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.80f, 0.84f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.15f, 0.15f, 0.18f, 1.0f));

    ImGui::Dummy(ImVec2(0, 4));

    auto SideButton = [&](int idx, const char* label) {
        bool active = (activePage_ == idx);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.80f, 0.84f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.10f, 0.14f, 1.0f));
        }
        if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 32))) {
            activePage_ = idx;
        }
        if (active) ImGui::PopStyleColor(2);
    };

    SideButton(0, "布局备份");
    SideButton(1, "通用");
    SideButton(2, "关于");

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
}

// ── UTF helpers ──────────────────────────────────────────────────
namespace {
    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string r(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), r.data(), n, nullptr, nullptr);
        return r;
    }

    std::wstring Utf8ToWide(const std::string& u)
    {
        if (u.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
        std::wstring r(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), r.data(), n);
        return r;
    }
}

// ── Backup Page ─────────────────────────────────────────────────
void SettingsWindow::DrawBackupPage()
{
    ImGui::Text("布局备份与恢复");
    ImGui::Separator();
    ImGui::Spacing();

    // Save section
    ImGui::Text("保存当前布局");
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##BackupName", "备份名称（可选）", backupNameBuf_, sizeof(backupNameBuf_));

    ImGui::SameLine();
    if (ImGui::Button("保存备份"))
    {
        std::wstring name = Utf8ToWide(backupNameBuf_);
        if (name.empty()) name = L"auto";
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

            if (ImGui::Button("恢复", ImVec2(56, 0)))
            {
                if (RestoreBackup(b.filename) && reloadCallback_)
                    reloadCallback_();
            }
            ImGui::SameLine();
            if (ImGui::Button("删除", ImVec2(56, 0)))
            {
                DeleteBackup(b.filename);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

void SettingsWindow::DrawGeneralPage()
{
    ImGui::Text("通用设置");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("更多设置即将推出...");
}

void SettingsWindow::DrawAboutPage()
{
    ImGui::Text("关于 SnowDesktop");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("SnowDesktop - Windows 桌面增强工具");
    ImGui::Spacing();
    ImGui::TextWrapped("基于 Dear ImGui 框架构建，提供自定义桌面布局、组件管理等功能。");
}

// ── Backup Implementation ────────────────────────────────────────
std::wstring SettingsWindow::GetBackupDir() const
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"backups");
    return path;
}

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

        LayoutBackup b;
        b.filename = fd.cFileName;
        b.timestamp = fd.ftLastWriteTime;

        // Parse display name from filename: remove .json and format timestamp
        std::wstring name = fd.cFileName;
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

bool SettingsWindow::SaveBackup(const std::wstring& name)
{
    std::wstring backupDir = GetBackupDir();
    CreateDirectoryW(backupDir.c_str(), nullptr);

    wchar_t layoutPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, layoutPath, static_cast<DWORD>(std::size(layoutPath)));
    PathRemoveFileSpecW(layoutPath);
    PathAppendW(layoutPath, L"SnowDesktop.layout.json");

    // Sanitize: remove colons for filename safety
    std::wstring safeName = name;
    for (auto& c : safeName) { if (c == L':' || c == L'\\' || c == L'/') c = L'_'; }

    std::wstring backupPath = backupDir + L"\\" + safeName + L".json";

    // Find existing file with same name, increment count
    int counter = 1;
    while (GetFileAttributesW(backupPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        backupPath = backupDir + L"\\" + safeName + L"(" + std::to_wstring(counter) + L").json";
        ++counter;
    }

    return CopyFileW(layoutPath, backupPath.c_str(), FALSE) != FALSE;
}

bool SettingsWindow::RestoreBackup(const std::wstring& filename)
{
    wchar_t layoutPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, layoutPath, static_cast<DWORD>(std::size(layoutPath)));
    PathRemoveFileSpecW(layoutPath);
    PathAppendW(layoutPath, L"SnowDesktop.layout.json");

    std::wstring backupPath = GetBackupDir() + L"\\" + filename;

    // First save current layout as auto-backup before restoring
    SaveBackup(L"[恢复前自动备份]");

    return CopyFileW(backupPath.c_str(), layoutPath, FALSE) != FALSE;
}

bool SettingsWindow::DeleteBackup(const std::wstring& filename)
{
    std::wstring backupPath = GetBackupDir() + L"\\" + filename;
    return DeleteFileW(backupPath.c_str()) != FALSE;
}

// ── Swap Chain ───────────────────────────────────────────────────
bool SettingsWindow::CreateSwapChain()
{
    RECT cr;
    GetClientRect(hwnd_, &cr);
    windowWidth_ = std::max(1L, cr.right - cr.left);
    windowHeight_ = std::max(1L, cr.bottom - cr.top);

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

void SettingsWindow::CleanupSwapChain()
{
    rtv_.Reset();
    swapChain_.Reset();
}

// ── Fonts ────────────────────────────────────────────────────────
void SettingsWindow::SetupFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    std::string fontPath = "C:\\Windows\\Fonts\\msyh.ttc";
    if (FILE* f = fopen(fontPath.c_str(), "rb"))
    {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
}

// ── WndProc ──────────────────────────────────────────────────────
LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_settingsWindow != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_settingsWindow != nullptr && wParam != SIZE_MINIMIZED)
        {
            g_settingsWindow->CreateSwapChain();
            g_settingsWindow->Render();
        }
        return 0;
    case WM_DPICHANGED:
    {
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr,
            suggested->left, suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
