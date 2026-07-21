/**
 * @file app_glass.h
 * @brief DesktopApp 的毛玻璃（苹果 Dock 风格）背景模块
 * @details 快照内容分两层：
 *          - 静态层：通过 IDesktopWallpaper 按显示器查询当前壁纸与铺放方式，
 *            WIC 解码后按桌面规则合成到 0.5x 位图（签名不变不重建）；
 *          - 动态层：检测到 Wallpaper Engine 后，在其 DXGI/D3D9 Present
 *            入口复制 BackBuffer 到共享 GPU 纹理，再由本进程直接拼合。
 *          合成结果经 Direct2D 高斯模糊后作为整窗背景快照；各组件面板按自身
 *          矩形裁剪采样，叠加半透明色调与渐变描边，形成 macOS Dock 式的
 *          磨砂玻璃观感。渲染窗保持原有 Progman 子窗口层级；采样源来自
 *          Wallpaper Engine 自身交换链，因此不会采样 SnowDesktop。
 *          本头文件由 app.h 末尾 #include 引入，成员均为 DesktopApp 内联实现。
 */

#pragma once

/** @brief 毛玻璃快照降采样比例（模糊后画质损失不可感知，内存与模糊开销降低约 4 倍）。 */
constexpr float kGlassBackdropScale = 0.5f;

/** @brief 同一源帧最多保留的模糊半径数量，限制 GPU 位图占用。 */
constexpr size_t kGlassRadiusCacheLimit = 6;

/** @brief 液态玻璃边缘自适应使用的低分辨率亮度图尺寸。 */
constexpr UINT kGlassLuminanceWidth = 32;
constexpr UINT kGlassLuminanceHeight = 18;

/** @brief 动态壁纸窗口检测间隔（毫秒）。 */
constexpr DWORD kGlassDetectIntervalMs = 10000;

inline std::wstring WallpaperProcessName(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!queried)
        return {};
    std::wstring name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name;
}

inline std::wstring WallpaperProcessCommandLine(DWORD processId)
{
    using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG,
        PULONG);
    struct NativeUnicodeString {
        USHORT length;
        USHORT maximumLength;
        PWSTR buffer;
    };
    const auto query = reinterpret_cast<NtQueryInformationProcessFn>(GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!query)
        return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return {};
    ULONG required = 0;
    query(process, 60, nullptr, 0, &required); // ProcessCommandLineInformation
    if (required < sizeof(NativeUnicodeString))
    {
        CloseHandle(process);
        return {};
    }
    std::vector<std::byte> buffer(required);
    const LONG status = query(process, 60, buffer.data(), required, &required);
    CloseHandle(process);
    if (status < 0)
        return {};
    const auto* command = reinterpret_cast<const NativeUnicodeString*>(buffer.data());
    if (!command->buffer || !command->length)
        return {};
    std::wstring result(command->buffer, command->length / sizeof(wchar_t));
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

inline bool IsWallpaperWebProcess(const std::wstring& name)
{
    return name == L"webwallpaper32.exe" ||
        name == L"webwallpaper64.exe" ||
        name == L"edgewallpaper32.exe" ||
        name == L"edgewallpaper64.exe" ||
        name == L"msedgewebview2.exe";
}

inline DWORD ResolveWallpaperOwnerProcess(DWORD processId)
{
    if (!processId || !IsWallpaperWebProcess(WallpaperProcessName(processId)))
        return processId;
    struct ProcessEntry {
        DWORD processId = 0;
        DWORD parentId = 0;
    };
    std::vector<ProcessEntry> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return processId;
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry))
    {
        do {
            processes.push_back({ entry.th32ProcessID, entry.th32ParentProcessID });
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    DWORD owner = processId;
    for (int depth = 0; depth < 8; ++depth)
    {
        const auto current = std::find_if(processes.begin(), processes.end(),
            [owner](const ProcessEntry& process) {
                return process.processId == owner;
            });
        if (current == processes.end() || !current->parentId)
            break;
        if (!IsWallpaperWebProcess(WallpaperProcessName(current->parentId)))
            break;
        owner = current->parentId;
    }
    return owner;
}

inline std::vector<DWORD> ResolveWallpaperCaptureProcesses(DWORD ownerPid)
{
    ownerPid = ResolveWallpaperOwnerProcess(ownerPid);
    const std::wstring ownerName = WallpaperProcessName(ownerPid);
    const bool webRenderer = ownerName == L"webwallpaper32.exe" ||
        ownerName == L"webwallpaper64.exe" ||
        ownerName == L"edgewallpaper32.exe" ||
        ownerName == L"edgewallpaper64.exe";
    if (!webRenderer)
        return { ownerPid };

    struct ProcessEntry {
        DWORD processId = 0;
        DWORD parentId = 0;
    };
    std::vector<ProcessEntry> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry{ sizeof(entry) };
        if (Process32FirstW(snapshot, &entry))
        {
            do {
                processes.push_back({ entry.th32ProcessID, entry.th32ParentProcessID });
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    std::unordered_set<DWORD> descendants{ ownerPid };
    for (int pass = 0; pass < 4; ++pass)
    {
        for (const auto& process : processes)
        {
            if (descendants.contains(process.parentId))
                descendants.insert(process.processId);
        }
    }

    std::vector<DWORD> gpuProcesses;
    for (DWORD processId : descendants)
    {
        if (processId == ownerPid)
            continue;
        const std::wstring name = WallpaperProcessName(processId);
        if (name != L"webwallpaper32.exe" &&
            name != L"webwallpaper64.exe" &&
            name != L"edgewallpaper32.exe" &&
            name != L"edgewallpaper64.exe" &&
            name != L"msedgewebview2.exe")
            continue;
        const std::wstring commandLine = WallpaperProcessCommandLine(processId);
        if (commandLine.find(L"--type=gpu-process") != std::wstring::npos)
            gpuProcesses.push_back(processId);
    }
    if (gpuProcesses.empty())
        gpuProcesses.push_back(ownerPid);
    return gpuProcesses;
}

inline bool IsGlassWallpaperWindowClass(HWND window)
{
    if (!window)
        return false;
    wchar_t className[64]{};
    if (!GetClassNameW(window, className, static_cast<int>(std::size(className))))
        return false;
    return _wcsicmp(className, L"WPEDesktopDX11Window") == 0 ||
        _wcsicmp(className, L"WPEDesktopCEFWindow") == 0 ||
        _wcsicmp(className, L"WPEVideoWallpaper") == 0 ||
        _wcsicmp(className, L"WPECloneView") == 0 ||
        _wcsicmp(className, L"EVRFullscreenVideo") == 0 ||
        _wcsicmp(className, L"Intermediate D3D Window") == 0;
}

/**
 * @brief 判断当前前台是否为其他进程的最大化或全屏窗口。
 * @details 排除 SnowDesktop 自身与桌面 Shell 窗口，避免打开设置面板或右键菜单时
 *          误触发毛玻璃的全屏节流策略。
 */
inline bool IsGlassForegroundMaximizedOrFullscreen()
{
    HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground))
        return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == GetCurrentProcessId())
        return false;
    wchar_t className[64]{};
    GetClassNameW(foreground, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Progman") == 0 ||
        _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Shell_TrayWnd") == 0)
        return false;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(foreground, DWMWA_CLOAKED,
            &cloaked, sizeof(cloaked))) && cloaked)
        return false;
    if (IsZoomed(foreground))
        return true;
    RECT bounds{};
    if (FAILED(DwmGetWindowAttribute(foreground,
            DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
        !GetWindowRect(foreground, &bounds))
        return false;
    HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{ sizeof(info) };
    if (!monitor || !GetMonitorInfoW(monitor, &info))
        return false;
    return bounds.left <= info.rcMonitor.left + 8 &&
        bounds.top <= info.rcMonitor.top + 8 &&
        bounds.right >= info.rcMonitor.right - 8 &&
        bounds.bottom >= info.rcMonitor.bottom - 8;
}

inline void CALLBACK DesktopApp::GlassWallpaperWinEventProc(HWINEVENTHOOK,
    DWORD event, HWND window, LONG objectId, LONG childId, DWORD, DWORD)
{
    if (!window || objectId != OBJID_WINDOW || childId != CHILDID_SELF)
        return;
    if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE)
        return;
    if (event != EVENT_OBJECT_DESTROY && !IsGlassWallpaperWindowClass(window))
        return;
    const HWND target = glassWallpaperEventTarget_;
    if (target && IsWindow(target))
        PostMessageW(target, kWallpaperWindowEventMessage,
            static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(window));
}

inline void DesktopApp::StartGlassWallpaperEventMonitor()
{
    if (glassWallpaperEventHook_ || !hwnd_ || !IsWindow(hwnd_))
        return;
    glassWallpaperEventTarget_ = hwnd_;
    glassWallpaperEventHook_ = SetWinEventHook(EVENT_OBJECT_CREATE,
        EVENT_OBJECT_HIDE, nullptr, &DesktopApp::GlassWallpaperWinEventProc,
        0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!glassWallpaperEventHook_)
        glassWallpaperEventTarget_ = nullptr;
}

inline void DesktopApp::StopGlassWallpaperEventMonitor()
{
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kWallpaperEventDebounceTimerId);
    if (glassWallpaperEventHook_)
    {
        UnhookWinEvent(glassWallpaperEventHook_);
        glassWallpaperEventHook_ = nullptr;
    }
    if (glassWallpaperEventTarget_ == hwnd_)
        glassWallpaperEventTarget_ = nullptr;
}

inline void DesktopApp::ScheduleGlassWallpaperEventRefresh(UINT event,
    HWND sourceWindow)
{
    if (!glassWallpaperEventHook_ || !hwnd_ || !IsWindow(hwnd_))
        return;
    const bool tracked = std::any_of(dynamicWallpaperWindows_.begin(),
        dynamicWallpaperWindows_.end(), [sourceWindow](const auto& source) {
            return source.hwnd == sourceWindow;
        });
    if (event == EVENT_OBJECT_DESTROY && !tracked)
        return;
    if (event != EVENT_OBJECT_DESTROY && !tracked &&
        !IsGlassWallpaperWindowClass(sourceWindow))
        return;
    SetTimer(hwnd_, kWallpaperEventDebounceTimerId,
        kWallpaperEventDebounceMs, nullptr);
}

/**
 * @brief 使毛玻璃背景快照失效。
 * @details 在壁纸/显示器拓扑/窗口几何变化、玻璃参数调整等时机调用，
 *          下次绘制时 EnsureGlassBackdrop 会重新评估签名并视情况重合成。
 */
inline void DesktopApp::InvalidateGlassBackdrop()
{
    glassBackdropDirty_ = true;
}

inline void DesktopApp::StopWallpaperEngineCaptures()
{
    for (auto& [_, capture] : wallpaperEngineCaptures_)
    {
        if (capture)
            capture->Stop();
    }
    wallpaperEngineCaptures_.clear();
    wallpaperCaptureProcessCache_.clear();
    wallpaperCaptureProcessCacheTick_ = 0;
    glassDynamicWaitStartTick_ = 0;
}

/**
 * @brief 收集各显示器壁纸来源、铺放方式与桌面背景色。
 * @details 首选 IDesktopWallpaper（支持多显示器独立壁纸与轮播当前项）；
 *          不可用时回退到注册表（HKCU\Control Panel\Desktop 的 Wallpaper /
 *          WallpaperStyle / TileWallpaper）+ 显示器枚举。
 *          铺放方式取值与 DESKTOP_WALLPAPER_POSITION 一致：
 *          0=居中 1=平铺 2=拉伸 3=适应 4=填充 22=跨区。
 * @return true 至少收集到一台显示器；false 完全失败
 */
inline bool DesktopApp::QueryGlassWallpaperSources(
    std::vector<GlassWallpaperSource>& sources, int& position, D2D1_COLOR_F& bgColor)
{
    sources.clear();
    position = 4; // 默认填充
    bgColor = D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f);

    // 桌面背景色（纯色背景或适应模式的留边区域）
    wchar_t colorBuf[64]{};
    DWORD colorSize = sizeof(colorBuf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background",
            RRF_RT_REG_SZ, nullptr, colorBuf, &colorSize) == ERROR_SUCCESS)
    {
        int r = 0, g = 0, b = 0;
        if (swscanf_s(colorBuf, L"%d %d %d", &r, &g, &b) == 3)
            bgColor = D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }

    POINT origin{ 0, 0 };
    MapWindowPoints(hwnd_, nullptr, &origin, 1);

    if (!desktopWallpaper_)
    {
        CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&desktopWallpaper_));
    }

    if (desktopWallpaper_)
    {
        UINT count = 0;
        if (SUCCEEDED(desktopWallpaper_->GetMonitorDevicePathCount(&count)))
        {
            DESKTOP_WALLPAPER_POSITION pos;
            if (SUCCEEDED(desktopWallpaper_->GetPosition(&pos)))
                position = static_cast<int>(pos);

            for (UINT i = 0; i < count; ++i)
            {
                LPWSTR id = nullptr;
                if (FAILED(desktopWallpaper_->GetMonitorDevicePathAt(i, &id)) || !id)
                    continue;
                RECT rc{};
                if (SUCCEEDED(desktopWallpaper_->GetMonitorRECT(id, &rc)))
                {
                    GlassWallpaperSource src{};
                    src.rect = D2D1::RectF(
                        static_cast<float>(rc.left - origin.x),
                        static_cast<float>(rc.top - origin.y),
                        static_cast<float>(rc.right - origin.x),
                        static_cast<float>(rc.bottom - origin.y));
                    LPWSTR wp = nullptr;
                    if (SUCCEEDED(desktopWallpaper_->GetWallpaper(id, &wp)) && wp)
                    {
                        src.path = wp;
                        CoTaskMemFree(wp);
                    }
                    sources.push_back(std::move(src));
                }
                CoTaskMemFree(id);
            }
        }
    }

    if (!sources.empty())
        return true;

    // ── 回退：注册表单壁纸 + 显示器枚举 ─────────────────────
    std::wstring fallbackPath;
    wchar_t wpPath[MAX_PATH]{};
    DWORD wpSize = sizeof(wpPath);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"Wallpaper",
            RRF_RT_REG_SZ, nullptr, wpPath, &wpSize) == ERROR_SUCCESS && wpPath[0])
        fallbackPath = wpPath;

    wchar_t styleBuf[16]{};
    DWORD styleSize = sizeof(styleBuf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle",
            RRF_RT_REG_SZ, nullptr, styleBuf, &styleSize) == ERROR_SUCCESS)
    {
        const int style = _wtoi(styleBuf);
        wchar_t tileBuf[16]{};
        DWORD tileSize = sizeof(tileBuf);
        int tile = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper",
                RRF_RT_REG_SZ, nullptr, tileBuf, &tileSize) == ERROR_SUCCESS)
            tile = _wtoi(tileBuf);
        if (tile == 1) position = 1;
        else if (style == 2) position = 2;
        else if (style == 6) position = 3;
        else if (style == 10) position = 4;
        else if (style == 22) position = 22;
        else position = 0;
    }

    struct EnumCtx {
        std::vector<GlassWallpaperSource>* sources;
        POINT origin;
        const std::wstring* path;
    } enumCtx{ &sources, origin, &fallbackPath };
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rc, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lp);
            GlassWallpaperSource src{};
            src.rect = D2D1::RectF(
                static_cast<float>(rc->left - c->origin.x),
                static_cast<float>(rc->top - c->origin.y),
                static_cast<float>(rc->right - c->origin.x),
                static_cast<float>(rc->bottom - c->origin.y));
            src.path = *c->path;
            c->sources->push_back(std::move(src));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&enumCtx));
    return !sources.empty();
}

/**
 * @brief 解码壁纸文件为 D2D 位图并按路径缓存。
 * @details WIC 解码统一转为 32bppPBGRA；缓存满 12 项时整体清空
 *          （壁纸数量少，简单淘汰足够）。
 * @return 成功返回位图指针（缓存持有，勿释放）；失败返回 nullptr
 */
inline ID2D1Bitmap1* DesktopApp::LoadGlassWallpaperBitmap(const std::wstring& path)
{
    if (path.empty() || !glassEffectContext_) return nullptr;
    auto it = glassWallpaperCache_.find(path);
    if (it != glassWallpaperCache_.end())
        return it->second.Get();

    if (glassWallpaperCache_.size() >= 12)
        glassWallpaperCache_.clear();

    if (!glassWicFactory_)
    {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&glassWicFactory_))) || !glassWicFactory_)
            return nullptr;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(glassWicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder)
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame)
        return nullptr;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(glassWicFactory_->CreateFormatConverter(&converter)) || !converter)
        return nullptr;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return nullptr;

    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(glassEffectContext_->CreateBitmapFromWicBitmap(
            converter.Get(), nullptr, &bitmap)) || !bitmap)
        return nullptr;

    auto slot = glassWallpaperCache_.emplace(path, std::move(bitmap));
    return slot.first->second.Get();
}

/**
 * @brief 检测动态壁纸渲染窗口及其桌面合成层。
 * @details 枚举 WorkerW/Progman 的子窗口，筛选「非本进程、非 Explorer、
 *          可见、覆盖任一显示器面积 ≥60%」的动态壁纸渲染窗口。窗口信息
 *          用于确定待注入进程，并与 Hook 发布的交换链窗口矩形对应。候选按
 *          z 序自底向上排序。10 秒防抖；候选变化时重置 Hook 失败标记。
 * @param force 跳过防抖强制重检
 */
inline void DesktopApp::DetectDynamicWallpaperWindows(bool force)
{
    const DWORD now = GetTickCount();
    if (!force && glassLastDetectTick_ != 0 &&
        now - glassLastDetectTick_ < kGlassDetectIntervalMs)
        return;
    glassLastDetectTick_ = now;

    std::vector<DynamicWallpaperWindow> found;

    HWND progman = FindWindowW(L"Progman", nullptr);
    DWORD shellPid = 0;
    if (progman)
    {
        GetWindowThreadProcessId(progman, &shellPid);
        // 触发壁纸专用 WorkerW 显露（幂等，业界标准技巧）
        DWORD_PTR dummy = 0;
        SendMessageTimeoutW(progman, 0x052C, 0xD, 1, SMTO_NORMAL, 100, &dummy);
    }

    // 收集桌面层宿主（WorkerW / Progman 顶层窗口）
    std::vector<HWND> hosts;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* list = reinterpret_cast<std::vector<HWND>*>(lp);
        wchar_t className[64]{};
        if (GetClassNameW(hwnd, className, 64) &&
            (_wcsicmp(className, L"WorkerW") == 0 || _wcsicmp(className, L"Progman") == 0))
            list->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hosts));

    const DWORD ourPid = GetCurrentProcessId();
    for (HWND host : hosts)
    {
        struct ChildCtx {
            HWND host;
            DWORD ourPid;
            DWORD shellPid;
            std::vector<DynamicWallpaperWindow>* found;
        } childCtx{ host, ourPid, shellPid, &found };

        EnumChildWindows(host, [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<ChildCtx*>(lp);
            if (!IsWindowVisible(hwnd)) return TRUE;

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->ourPid || (c->shellPid != 0 && pid == c->shellPid))
                return TRUE;
            pid = ResolveWallpaperOwnerProcess(pid);

            wchar_t className[64]{};
            if (GetClassNameW(hwnd, className, 64))
            {
                if (_wcsicmp(className, L"SHELLDLL_DefView") == 0 ||
                    _wcsicmp(className, L"SysListView32") == 0 ||
                    _wcsicmp(className, L"DirectUIHWND") == 0 ||
                    _wcsicmp(className, L"WorkerW") == 0 ||
                    _wcsicmp(className, L"Progman") == 0)
                    return TRUE;
            }

            RECT rc{};
            if (!GetWindowRect(hwnd, &rc)) return TRUE;
            if (rc.right - rc.left < 200 || rc.bottom - rc.top < 200) return TRUE;

            HMONITOR mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof(mi) };
            bool covers = false;
            if (mon && GetMonitorInfoW(mon, &mi))
            {
                RECT inter{};
                if (IntersectRect(&inter, &rc, &mi.rcMonitor))
                {
                    const double monArea =
                        double(mi.rcMonitor.right - mi.rcMonitor.left) *
                        double(mi.rcMonitor.bottom - mi.rcMonitor.top);
                    const double interArea =
                        double(inter.right - inter.left) * double(inter.bottom - inter.top);
                    covers = monArea > 0.0 && interArea >= monArea * 0.6;
                }
            }
            if (!covers) return TRUE;

            auto existing = std::find_if(c->found->begin(), c->found->end(),
                [hwnd](const DynamicWallpaperWindow& item) {
                    return item.hwnd == hwnd;
                });
            if (existing == c->found->end())
                c->found->push_back(DynamicWallpaperWindow{
                    hwnd, rc, pid });
            else
            {
                if (existing->rendererPid == 0)
                    existing->rendererPid = pid;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&childCtx));
    }

    // EnumChildWindows 按 z 序（顶→底）枚举，反转为自底向上用于叠加合成。
    std::reverse(found.begin(), found.end());

    // CEF and EVR both expose nested full-screen child windows. Treat windows
    // from the same renderer that cover the same display area as one output;
    // otherwise one web/video monitor is mistaken for several capture sources.
    auto windowPriority = [](HWND window) {
        wchar_t className[64]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"WPEDesktopCEFWindow") == 0)
            return 0;
        if (_wcsicmp(className, L"WPEDesktopDX11Window") == 0 ||
            _wcsicmp(className, L"WPEVideoWallpaper") == 0 ||
            _wcsicmp(className, L"WPECloneView") == 0)
            return 4;
        if (_wcsicmp(className, L"Intermediate D3D Window") == 0 ||
            _wcsicmp(className, L"EVRFullscreenVideo") == 0)
            return 3;
        return 1;
    };
    auto isWebProxyWindow = [](HWND window) {
        wchar_t className[64]{};
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        return _wcsicmp(className, L"WPEDesktopCEFWindow") == 0;
    };
    std::vector<DynamicWallpaperWindow> outputs;
    for (const auto& candidate : found)
    {
        const std::int64_t candidateArea =
            static_cast<std::int64_t>(candidate.rect.right - candidate.rect.left) *
            static_cast<std::int64_t>(candidate.rect.bottom - candidate.rect.top);
        bool merged = false;
        for (auto& output : outputs)
        {
            if (output.rendererPid != candidate.rendererPid &&
                !isWebProxyWindow(output.hwnd) &&
                !isWebProxyWindow(candidate.hwnd))
                continue;
            RECT overlap{};
            if (!IntersectRect(&overlap, &output.rect, &candidate.rect))
                continue;
            const std::int64_t outputArea =
                static_cast<std::int64_t>(output.rect.right - output.rect.left) *
                static_cast<std::int64_t>(output.rect.bottom - output.rect.top);
            const std::int64_t overlapArea =
                static_cast<std::int64_t>(overlap.right - overlap.left) *
                static_cast<std::int64_t>(overlap.bottom - overlap.top);
            const std::int64_t smallerArea = std::min(outputArea, candidateArea);
            const std::int64_t largerArea = std::max(outputArea, candidateArea);
            if (smallerArea <= 0 || overlapArea * 100 < smallerArea * 90 ||
                largerArea * 100 > smallerArea * 110)
                continue;
            if (windowPriority(candidate.hwnd) > windowPriority(output.hwnd) ||
                (windowPriority(candidate.hwnd) == windowPriority(output.hwnd) &&
                    candidateArea > outputArea))
                output = candidate;
            merged = true;
            break;
        }
        if (!merged)
            outputs.push_back(candidate);
    }
    found = std::move(outputs);

    // 候选集合变化时重置 Hook 失败标记（给新引擎一次机会）；
    // 同一候选集合下每 30 秒也重试一次（引擎启动初期可能暂时黑屏）
    const bool changed = found.size() != dynamicWallpaperWindows_.size() ||
        !std::equal(found.begin(), found.end(), dynamicWallpaperWindows_.begin(),
            [](const DynamicWallpaperWindow& a, const DynamicWallpaperWindow& b) {
                return a.hwnd == b.hwnd &&
                    a.rendererPid == b.rendererPid &&
                    EqualRect(&a.rect, &b.rect) != FALSE;
            });
    const bool retryDue = dynamicWallpaperIncompatible_ &&
        now - dynamicWallpaperIncompatibleTick_ >= 30000;
    if (changed || retryDue)
    {
        StopWallpaperEngineCaptures();
        glassDynamicLayerBitmap_.Reset();
        glassCapturedDynamicWindows_.clear();
        dynamicWallpaperIncompatible_ = false;
        dynamicWallpaperIncompatibleTick_ = 0;
        dynamicWallpaperCaptureError_.clear();
    }

    dynamicWallpaperWindows_ = std::move(found);
    if (dynamicWallpaperWindows_.empty())
        StopWallpaperEngineCaptures();

    // 引擎识别（诊断与设置界面状态行）
    dynamicWallpaperEngine_.clear();
    for (const auto& win : dynamicWallpaperWindows_)
    {
        DWORD pid = win.rendererPid;
        if (pid == 0)
            GetWindowThreadProcessId(win.hwnd, &pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) continue;
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, path, &size))
        {
            const wchar_t* fileName = PathFindFileNameW(path);
            std::wstring lower = fileName ? fileName : L"";
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.find(L"wallpaper") != std::wstring::npos)
                dynamicWallpaperEngine_ = L"Wallpaper Engine";
            else if (lower.find(L"lively") != std::wstring::npos)
                dynamicWallpaperEngine_ = L"Lively Wallpaper";
            else if (lower.find(L"yuanqi") != std::wstring::npos)
                dynamicWallpaperEngine_ = L"元气壁纸";
            else if (lower.find(L"upupoo") != std::wstring::npos)
                dynamicWallpaperEngine_ = L"UPUPOO";
        }
        CloseHandle(hProc);
        if (!dynamicWallpaperEngine_.empty()) break;
    }

    if (changed && !dynamicWallpaperWindows_.empty())
    {
        wchar_t className[64]{};
        GetClassNameW(dynamicWallpaperWindows_.front().hwnd, className, 64);
        wchar_t message[256]{};
        wsprintfW(message,
            L"Dynamic wallpaper source count=%u class=%s backend=GPU-Present-Hook engine=%s",
            static_cast<unsigned>(dynamicWallpaperWindows_.size()), className,
            dynamicWallpaperEngine_.empty() ? L"unknown" : dynamicWallpaperEngine_.c_str());
        WriteCrashLogEntry(message);
    }
}

/**
 * @brief 通过 Wallpaper Engine GPU Hook 共享纹理取得动态壁纸帧。
 * @details 场景/网页使用 DXGI，视频使用 D3D9Ex；各显示器独立选择输出，
 *          直接降采样拼合成 0.5x D2D
 *          动态层，不执行 GDI 全屏复制、CPU 逐像素处理，也不隐藏/恢复窗口。
 *          本函数没有其他捕获后端；失败原因会直接写入诊断状态。
 * @param refreshMode 请求该帧的刷新档位
 * @param[out] outBitmap 捕获帧的 D2D 位图视图
 * @return true 成功取得当前共享帧；false Hook 尚未就绪或发生错误
 */
inline bool DesktopApp::CaptureDynamicWallpaperLayer(int refreshMode, ID2D1Bitmap1** outBitmap)
{
    if (!outBitmap)
        return false;
    *outBitmap = nullptr;
    dynamicWallpaperCaptureDeferred_ = false;

    auto scheduleFrameRetry = [this]() {
        if (glassRefreshThrottled_ || !hwnd_ || !IsWindow(hwnd_))
            return;
        const DWORD now = GetTickCount();
        if (glassDynamicWaitStartTick_ == 0)
            glassDynamicWaitStartTick_ = now;
        const DWORD waitMs = now - glassDynamicWaitStartTick_;
        UINT retryInterval = kGlassRefreshRealtimeIntervalMs;
        if (waitMs >= 5000)
            retryInterval = 500;
        else if (waitMs >= 1000)
            retryInterval = 250;
        if (glassRefreshTimerActive_ &&
            glassRefreshTimerIntervalMs_ != retryInterval)
        {
            KillTimer(hwnd_, kGlassRefreshTimerId);
            glassRefreshTimerActive_ = false;
        }
        if (!glassRefreshTimerActive_)
        {
            SetTimer(hwnd_, kGlassRefreshTimerId, retryInterval, nullptr);
            glassRefreshTimerActive_ = true;
            glassRefreshTimerIntervalMs_ = retryInterval;
        }
    };
    auto fail = [this](const std::wstring& stage) {
        dynamicWallpaperCaptureError_ = stage;
        return false;
    };
    auto defer = [this, &scheduleFrameRetry](const std::wstring& stage) {
        dynamicWallpaperCaptureDeferred_ = true;
        dynamicWallpaperCaptureError_ = stage;
        scheduleFrameRetry();
        return false;
    };

    if (!glassEffectContext_ || !d3dDevice_)
        return fail(L"D3D/D2D 设备未就绪");
    if (dynamicWallpaperWindows_.empty())
        return fail(L"动态壁纸候选已消失");

    std::unordered_set<DWORD> ownerPids;
    for (const auto& window : dynamicWallpaperWindows_)
    {
        DWORD processId = window.rendererPid;
        if (!processId)
            GetWindowThreadProcessId(window.hwnd, &processId);
        if (processId)
            ownerPids.insert(processId);
    }

    const DWORD processCacheNow = GetTickCount();
    const bool refreshProcessCache = wallpaperCaptureProcessCacheTick_ == 0 ||
        processCacheNow - wallpaperCaptureProcessCacheTick_ >= 2000;
    for (auto it = wallpaperCaptureProcessCache_.begin();
        it != wallpaperCaptureProcessCache_.end();)
    {
        if (!ownerPids.contains(it->first))
            it = wallpaperCaptureProcessCache_.erase(it);
        else
            ++it;
    }
    for (DWORD ownerPid : ownerPids)
    {
        if (refreshProcessCache ||
            !wallpaperCaptureProcessCache_.contains(ownerPid))
            wallpaperCaptureProcessCache_[ownerPid] =
                ResolveWallpaperCaptureProcesses(ownerPid);
    }
    if (refreshProcessCache)
        wallpaperCaptureProcessCacheTick_ = processCacheNow;

    std::unordered_set<DWORD> rendererPids;
    std::unordered_map<DWORD, DWORD> captureOwnerPids;
    for (DWORD ownerPid : ownerPids)
    {
        for (DWORD capturePid : wallpaperCaptureProcessCache_[ownerPid])
        {
            rendererPids.insert(capturePid);
            captureOwnerPids[capturePid] = ownerPid;
        }
    }
    if (dynamicWallpaperEngine_ != L"Wallpaper Engine" || rendererPids.empty())
        return fail(L"GPU Hook 当前仅支持 Wallpaper Engine");

    const int mode = std::clamp(refreshMode, 0, 3);
    DWORD intervalMs = 0;
    if (mode == 1)
        intervalMs = kGlassRefreshLowMs;
    else if (mode == 2)
        intervalMs = kGlassRefreshMidMs;
    else if (mode == 3)
        intervalMs = kGlassRefreshRealtimeIntervalMs;

    for (auto it = wallpaperEngineCaptures_.begin();
        it != wallpaperEngineCaptures_.end();)
    {
        if (!rendererPids.contains(it->first))
        {
            if (it->second)
                it->second->Stop();
            it = wallpaperEngineCaptures_.erase(it);
        }
        else
            ++it;
    }

    std::vector<WallpaperEngineFrame> frames;
    std::vector<WallpaperEngineCaptureSession*> acquiredSessions;
    bool waitingForSource = false;
    std::wstring waitingReason;
    for (DWORD rendererPid : rendererPids)
    {
        auto& capture = wallpaperEngineCaptures_[rendererPid];
        if (!capture)
            capture = std::make_unique<WallpaperEngineCaptureSession>();
        if (!capture->EnsureStarted(rendererPid, d3dDevice_.Get(),
                glassEffectContext_.Get(), intervalMs))
        {
            std::wstring error = capture->LastError();
            if (error.empty())
                error = L"Wallpaper Engine Hook 连接中断";
            if (error.find(L"等待") == std::wstring::npos)
                capture->RequestReconnect(error);
            waitingForSource = true;
            if (waitingReason.empty())
                waitingReason = error;
            continue;
        }

        std::vector<WallpaperEngineFrame> processFrames;
        const WallpaperEngineFrameState frameState =
            capture->TryAcquireLatestFrames(processFrames);
        if (frameState == WallpaperEngineFrameState::pending)
        {
            capture->RequestFrame();
            waitingForSource = true;
            if (waitingReason.empty())
            {
                waitingReason = capture->LastError();
                if (waitingReason.empty())
                    waitingReason = L"等待 Wallpaper Engine 下一次 Present";
            }
            continue;
        }
        if (frameState == WallpaperEngineFrameState::error)
        {
            std::wstring reason = capture->LastError();
            if (reason.empty())
                reason = L"Wallpaper Engine 共享帧读取失败";
            capture->RequestReconnect(reason);
            waitingForSource = true;
            if (waitingReason.empty())
                waitingReason = reason;
            continue;
        }
        acquiredSessions.push_back(capture.get());
        frames.insert(frames.end(), processFrames.begin(), processFrames.end());
    }

    if (frames.empty())
    {
        return defer(waitingReason.empty()
            ? L"等待 Wallpaper Engine 下一次 Present"
            : waitingReason);
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        for (auto* capture : acquiredSessions)
            capture->ReleaseFrames(false);
        return fail(L"主窗口客户区尺寸无效");
    }
    const UINT sampleW = std::max<UINT>(1,
        static_cast<UINT>(std::lround(width * kGlassBackdropScale)));
    const UINT sampleH = std::max<UINT>(1,
        static_cast<UINT>(std::lround(height * kGlassBackdropScale)));
    if (glassDynamicLayerBitmap_ &&
        (glassDynamicLayerBitmap_->GetPixelSize().width != sampleW ||
            glassDynamicLayerBitmap_->GetPixelSize().height != sampleH))
    {
        glassDynamicLayerBitmap_.Reset();
        glassCapturedDynamicWindows_.clear();
    }
    bool dynamicLayerCreated = false;
    if (!glassDynamicLayerBitmap_)
    {
        const D2D1_BITMAP_PROPERTIES1 targetProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT createHr = glassEffectContext_->CreateBitmap(
            D2D1::SizeU(sampleW, sampleH), nullptr, 0, &targetProps,
            &glassDynamicLayerBitmap_);
        if (FAILED(createHr) || !glassDynamicLayerBitmap_)
        {
            for (auto* capture : acquiredSessions)
                capture->ReleaseFrames(false);
            return fail(L"共享纹理动态层创建失败");
        }
        dynamicLayerCreated = true;
    }

    POINT origin{};
    MapWindowPoints(hwnd_, nullptr, &origin, 1);
    const float scale = kGlassBackdropScale;
    glassEffectContext_->SetTarget(glassDynamicLayerBitmap_.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    if (dynamicLayerCreated)
        glassEffectContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    auto validRect = [](const RECT& rect) {
        return rect.right > rect.left && rect.bottom > rect.top;
    };
    auto intersectionArea = [validRect](const RECT& left, const RECT& right) {
        RECT intersection{};
        if (!validRect(left) || !validRect(right) ||
            !IntersectRect(&intersection, &left, &right))
            return std::int64_t{ 0 };
        return static_cast<std::int64_t>(intersection.right - intersection.left) *
            static_cast<std::int64_t>(intersection.bottom - intersection.top);
    };
    auto drawFrameForOutput = [this, origin, scale, sampleW, sampleH, validRect](
        const WallpaperEngineFrame& frame, const RECT& outputRect) {
        if (!frame.bitmap)
            return;
        const D2D1_SIZE_U sourceSize = frame.bitmap->GetPixelSize();
        RECT drawRect = outputRect;
        D2D1_RECT_F source = D2D1::RectF(0.0f, 0.0f,
            static_cast<float>(sourceSize.width),
            static_cast<float>(sourceSize.height));

        // A renderer may publish one swap chain spanning multiple displays. Crop
        // the corresponding source portion for this output instead of scaling the
        // whole virtual-desktop frame independently onto every monitor.
        if (validRect(frame.desktopRect) && validRect(outputRect))
        {
            RECT intersection{};
            if (IntersectRect(&intersection, &frame.desktopRect, &outputRect))
            {
                const float frameWidth = static_cast<float>(
                    frame.desktopRect.right - frame.desktopRect.left);
                const float frameHeight = static_cast<float>(
                    frame.desktopRect.bottom - frame.desktopRect.top);
                source = D2D1::RectF(
                    (intersection.left - frame.desktopRect.left) / frameWidth *
                        sourceSize.width,
                    (intersection.top - frame.desktopRect.top) / frameHeight *
                        sourceSize.height,
                    (intersection.right - frame.desktopRect.left) / frameWidth *
                        sourceSize.width,
                    (intersection.bottom - frame.desktopRect.top) / frameHeight *
                        sourceSize.height);
                drawRect = intersection;
            }
        }
        D2D1_RECT_F destination = D2D1::RectF(
            (drawRect.left - origin.x) * scale,
            (drawRect.top - origin.y) * scale,
            (drawRect.right - origin.x) * scale,
            (drawRect.bottom - origin.y) * scale);
        if (destination.right <= destination.left || destination.bottom <= destination.top)
        {
            destination = D2D1::RectF(0.0f, 0.0f,
                static_cast<float>(sampleW), static_cast<float>(sampleH));
        }
        glassEffectContext_->DrawBitmap(frame.bitmap, destination, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
    };

    // Select one source for every dynamic output. This keeps a native wallpaper
    // monitor untouched, allows scene/video/web renderers to coexist, and avoids
    // a black DXGI compositor surface overriding an EVR D3D9 video frame.
    for (const auto& window : dynamicWallpaperWindows_)
    {
        wchar_t className[64]{};
        GetClassNameW(window.hwnd, className, static_cast<int>(std::size(className)));
        const bool videoOutput = _wcsicmp(className, L"WPEVideoWallpaper") == 0 ||
            _wcsicmp(className, L"EVRFullscreenVideo") == 0;
        const bool cloneOutput = _wcsicmp(className, L"WPECloneView") == 0;
        const WallpaperEngineFrame* selected = nullptr;
        std::int64_t selectedScore = std::numeric_limits<std::int64_t>::min();
        for (const auto& frame : frames)
        {
            const auto owner = captureOwnerPids.find(frame.processId);
            const DWORD frameOwnerPid = owner != captureOwnerPids.end()
                ? owner->second
                : frame.processId;
            const bool sameOwner = frameOwnerPid == window.rendererPid;
            const bool exactWindow = frame.outputWindow == window.hwnd;
            const bool relatedWindow = frame.outputWindow && window.hwnd &&
                (IsChild(window.hwnd, frame.outputWindow) ||
                    IsChild(frame.outputWindow, window.hwnd));
            const std::int64_t overlap = intersectionArea(
                frame.desktopRect, window.rect);
            if (!exactWindow && !relatedWindow && overlap == 0 &&
                !(cloneOutput && sameOwner))
                continue;
            if (!sameOwner && !exactWindow && !relatedWindow)
                continue;

            std::int64_t score = sameOwner ? 1000000000ll : 0ll;
            if (exactWindow)
                score += 4000000000000ll;
            else if (relatedWindow)
                score += 3000000000000ll;
            if (overlap > 0)
            {
                const std::int64_t outputArea =
                    static_cast<std::int64_t>(window.rect.right - window.rect.left) *
                    static_cast<std::int64_t>(window.rect.bottom - window.rect.top);
                if (outputArea > 0)
                    score += std::min<std::int64_t>(2000000000000ll,
                        overlap * 2000000000000ll / outputArea);
            }
            if (frame.d3d9Video == videoOutput)
                score += 8000000000000ll;
            if (score > selectedScore)
            {
                selected = &frame;
                selectedScore = score;
            }
        }

        if (selected)
        {
            drawFrameForOutput(*selected, window.rect);
            glassCapturedDynamicWindows_.insert(window.hwnd);
        }
        else if (!glassCapturedDynamicWindows_.contains(window.hwnd))
        {
            waitingForSource = true;
            if (waitingReason.empty())
                waitingReason = L"等待 Wallpaper Engine 当前显示器输出帧";
        }
    }
    const HRESULT composeHr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    for (auto* capture : acquiredSessions)
        capture->ReleaseFrames(SUCCEEDED(composeHr));
    if (FAILED(composeHr))
    {
        wchar_t message[128]{};
        swprintf_s(message, L"Wallpaper Engine 共享帧拼合失败（0x%08X）",
            static_cast<unsigned>(composeHr));
        return fail(message);
    }

    if (waitingForSource)
    {
        dynamicWallpaperCaptureDeferred_ = true;
        dynamicWallpaperCaptureError_ = waitingReason;
        scheduleFrameRetry();
    }
    else
    {
        dynamicWallpaperCaptureError_.clear();
        dynamicWallpaperCaptureDeferred_ = false;
        glassDynamicWaitStartTick_ = 0;
    }
    *outBitmap = glassDynamicLayerBitmap_.Get();
    (*outBitmap)->AddRef();
    return true;
}

/**
 * @brief 合成 0.5x 静态壁纸层（不含模糊）。
 * @details 结果写入 glassStaticLayerBitmap_（尺寸匹配时复用）；铺放规则与
 *          桌面一致：居中/平铺/拉伸/适应/填充/跨区，背景色打底。
 * @return 成功返回 true
 */
inline bool DesktopApp::ComposeGlassStaticLayer(
    const std::vector<GlassWallpaperSource>& sources, int position,
    D2D1_COLOR_F bgColor, UINT sampleW, UINT sampleH)
{
    if (!glassEffectContext_) return false;

    if (glassStaticLayerBitmap_ &&
        (glassStaticLayerBitmap_->GetPixelSize().width != sampleW ||
            glassStaticLayerBitmap_->GetPixelSize().height != sampleH))
        glassStaticLayerBitmap_.Reset();

    if (!glassStaticLayerBitmap_)
    {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(glassEffectContext_->CreateBitmap(D2D1::SizeU(sampleW, sampleH),
                nullptr, 0, &props, &glassStaticLayerBitmap_)) || !glassStaticLayerBitmap_)
            return false;
    }

    const float s = kGlassBackdropScale;
    const D2D1_RECT_F canvas = D2D1::RectF(0.0f, 0.0f,
        static_cast<float>(sampleW), static_cast<float>(sampleH));

    // 按铺放方式把一张壁纸绘制到目标矩形（采样坐标）
    auto drawFitted = [&](ID2D1Bitmap1* bmp, const D2D1_RECT_F& monRect, int mode) {
        const D2D1_SIZE_F img = bmp->GetSize();
        if (img.width < 1.0f || img.height < 1.0f) return;
        const float mw = monRect.right - monRect.left;
        const float mh = monRect.bottom - monRect.top;
        if (mw < 1.0f || mh < 1.0f) return;
        float dw = 0.0f, dh = 0.0f;
        switch (mode)
        {
        case 2: // 拉伸
            dw = mw; dh = mh;
            break;
        case 3: // 适应（等比缩到矩形内）
        {
            const float k = std::min(mw / img.width, mh / img.height);
            dw = img.width * k; dh = img.height * k;
            break;
        }
        case 4: // 填充（等比覆盖矩形，裁掉溢出）
        {
            const float k = std::max(mw / img.width, mh / img.height);
            dw = img.width * k; dh = img.height * k;
            break;
        }
        default: // 居中（原生大小）
            dw = img.width * s; dh = img.height * s;
            break;
        }
        const D2D1_RECT_F dest = D2D1::RectF(
            monRect.left + (mw - dw) * 0.5f, monRect.top + (mh - dh) * 0.5f,
            monRect.left + (mw + dw) * 0.5f, monRect.top + (mh + dh) * 0.5f);
        glassEffectContext_->DrawBitmap(bmp, dest, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    };

    glassEffectContext_->SetTarget(glassStaticLayerBitmap_.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    glassEffectContext_->Clear(bgColor);

    if (position == 1)
    {
        // 平铺：以画布原点为起点 wrap 填充（与桌面平铺行为一致）
        for (const auto& src : sources)
        {
            if (src.path.empty()) continue;
            ID2D1Bitmap1* bmp = LoadGlassWallpaperBitmap(src.path);
            if (!bmp) continue;
            ComPtr<ID2D1BitmapBrush> tileBrush;
            if (SUCCEEDED(glassEffectContext_->CreateBitmapBrush(bmp,
                    D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP,
                        D2D1_EXTEND_MODE_WRAP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                    D2D1::BrushProperties(1.0f, D2D1::Matrix3x2F::Scale(s, s)),
                    &tileBrush)) && tileBrush)
            {
                glassEffectContext_->FillRectangle(canvas, tileBrush.Get());
            }
            break;
        }
    }
    else if (position == 22)
    {
        // 跨区：整幅虚拟画布按“填充”铺一张图
        for (const auto& src : sources)
        {
            if (src.path.empty()) continue;
            if (ID2D1Bitmap1* bmp = LoadGlassWallpaperBitmap(src.path))
            {
                drawFitted(bmp, canvas, 4);
                break;
            }
        }
    }
    else
    {
        for (const auto& src : sources)
        {
            if (src.path.empty()) continue;
            if (ID2D1Bitmap1* bmp = LoadGlassWallpaperBitmap(src.path))
            {
                const D2D1_RECT_F monRect = D2D1::RectF(
                    src.rect.left * s, src.rect.top * s,
                    src.rect.right * s, src.rect.bottom * s);
                drawFitted(bmp, monRect, position);
            }
        }
    }

    HRESULT hr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    return SUCCEEDED(hr);
}

/**
 * @brief 在静态层之上叠加已拼合的显示器动态层，写入 glassComposeBitmap_。
 * @details 静态层整幅打底，动态层的显示器区域覆盖其上；虚拟桌面中未被
 *          任何显示器占用的空洞保持透明并露出静态层。
 * @param frame 捕获并拼合的 0.5x 动态层
 * @return 成功返回 true
 */
inline bool DesktopApp::ComposeGlassDynamicFrame(ID2D1Bitmap1* frame)
{
    if (!frame || !glassEffectContext_ || !glassStaticLayerBitmap_) return false;

    const D2D1_SIZE_U size = glassStaticLayerBitmap_->GetPixelSize();
    if (glassComposeBitmap_ &&
        (glassComposeBitmap_->GetPixelSize().width != size.width ||
            glassComposeBitmap_->GetPixelSize().height != size.height))
        glassComposeBitmap_.Reset();

    if (!glassComposeBitmap_)
    {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(glassEffectContext_->CreateBitmap(size, nullptr, 0, &props,
                &glassComposeBitmap_)) || !glassComposeBitmap_)
            return false;
    }

    glassEffectContext_->SetTarget(glassComposeBitmap_.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    glassEffectContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    glassEffectContext_->DrawBitmap(glassStaticLayerBitmap_.Get(),
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(size.width),
            static_cast<float>(size.height)),
        1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);

    const auto frameSize = frame->GetPixelSize();
    glassEffectContext_->DrawBitmap(frame,
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(size.width),
            static_cast<float>(size.height)),
        1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(frameSize.width),
            static_cast<float>(frameSize.height)));

    HRESULT hr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    return SUCCEEDED(hr);
}

inline void DesktopApp::ClearGlassBackdropTransition()
{
    glassPreviousBackdropBitmap_.Reset();
    glassPreviousBackdropRadiusCache_.clear();
    glassTransitionStartTick_ = 0;
    glassTransitionDurationMs_ = 0;
    if (glassTransitionTimerActive_)
    {
        if (hwnd_ && IsWindow(hwnd_))
            KillTimer(hwnd_, kGlassTransitionTimerId);
        glassTransitionTimerActive_ = false;
    }
}

inline void DesktopApp::BeginGlassBackdropTransition(int refreshMode)
{
    refreshMode = std::clamp(refreshMode, 0, 3);
    const bool canTransition = (refreshMode == 1 || refreshMode == 2) &&
        !glassBackdropRadiusCache_.empty();
    if (!canTransition)
    {
        ClearGlassBackdropTransition();
        glassBackdropBitmap_.Reset();
        glassBackdropRadiusCache_.clear();
        return;
    }

    glassPreviousBackdropRadiusCache_ = std::move(glassBackdropRadiusCache_);
    glassPreviousBackdropBitmap_ = glassBackdropBitmap_;
    glassBackdropBitmap_.Reset();
    glassTransitionStartTick_ = GetTickCount();
    glassTransitionDurationMs_ = refreshMode == 1
        ? kGlassTransitionLowMs
        : kGlassTransitionMidMs;
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kGlassTransitionTimerId,
            kGlassTransitionFrameMs, nullptr);
        glassTransitionTimerActive_ = true;
    }
}

/**
 * @brief 将 0.5x 位图高斯模糊到 glassBackdropBitmap_。
 * @details 同一源帧按共享半径缓存输出，避免每个面板重复执行高斯模糊。
 *          每台物理显示器先独立裁剪，
 *          使用硬边界扩展完成模糊，再拼回原区域，禁止跨屏采样颜色。
 * @return 成功返回 true
 */
inline bool DesktopApp::BlurGlassBitmapToBackdrop(ID2D1Bitmap1* source, float blurRadius)
{
    if (!source || !glassEffectContext_) return false;

    const D2D1_SIZE_U size = source->GetPixelSize();
    blurRadius = std::clamp(blurRadius, 4.0f, 48.0f);
    const int radiusKey = static_cast<int>(std::lround(blurRadius));
    blurRadius = static_cast<float>(radiusKey);
    auto selectPreviousRadius = [this, radiusKey]() {
        const auto previous = glassPreviousBackdropRadiusCache_.find(radiusKey);
        if (previous != glassPreviousBackdropRadiusCache_.end())
            glassPreviousBackdropBitmap_ = previous->second;
        else
            glassPreviousBackdropBitmap_.Reset();
    };
    auto cached = glassBackdropRadiusCache_.find(radiusKey);
    if (cached != glassBackdropRadiusCache_.end())
    {
        const D2D1_SIZE_U cachedSize = cached->second->GetPixelSize();
        if (cachedSize.width == size.width && cachedSize.height == size.height)
        {
            glassBackdropBitmap_ = cached->second;
            selectPreviousRadius();
            return true;
        }
        glassBackdropRadiusCache_.erase(cached);
    }

    ComPtr<ID2D1Bitmap1> target;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(glassEffectContext_->CreateBitmap(size, nullptr, 0, &props,
            &target)) || !target)
        return false;

    POINT windowOrigin{};
    SetLastError(ERROR_SUCCESS);
    if (!MapWindowPoints(hwnd_, nullptr, &windowOrigin, 1))
    {
        const DWORD mapError = GetLastError();
        if (mapError != ERROR_SUCCESS)
            return false;
    }
    struct MonitorRegionContext {
        std::vector<D2D1_RECT_F>* regions = nullptr;
        POINT origin{};
        D2D1_SIZE_U bitmapSize{};
    } monitorContext{};
    std::vector<D2D1_RECT_F> monitorRegions;
    monitorContext.regions = &monitorRegions;
    monitorContext.origin = windowOrigin;
    monitorContext.bitmapSize = size;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT monitorRect, LPARAM parameter) -> BOOL {
            auto* context = reinterpret_cast<MonitorRegionContext*>(parameter);
            D2D1_RECT_F region = D2D1::RectF(
                (monitorRect->left - context->origin.x) * kGlassBackdropScale,
                (monitorRect->top - context->origin.y) * kGlassBackdropScale,
                (monitorRect->right - context->origin.x) * kGlassBackdropScale,
                (monitorRect->bottom - context->origin.y) * kGlassBackdropScale);
            region.left = std::clamp(region.left, 0.0f,
                static_cast<float>(context->bitmapSize.width));
            region.top = std::clamp(region.top, 0.0f,
                static_cast<float>(context->bitmapSize.height));
            region.right = std::clamp(region.right, 0.0f,
                static_cast<float>(context->bitmapSize.width));
            region.bottom = std::clamp(region.bottom, 0.0f,
                static_cast<float>(context->bitmapSize.height));
            if (region.right > region.left && region.bottom > region.top)
            {
                const bool duplicate = std::any_of(context->regions->begin(),
                    context->regions->end(), [&region](const D2D1_RECT_F& existing) {
                        return existing.left == region.left &&
                            existing.top == region.top &&
                            existing.right == region.right &&
                            existing.bottom == region.bottom;
                    });
                if (!duplicate)
                    context->regions->push_back(region);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitorContext));
    if (monitorRegions.empty())
        return false;

    struct MonitorBlurEffect {
        D2D1_RECT_F destination{};
        D2D1_RECT_F imageBounds{};
        ComPtr<ID2D1Effect> crop;
        ComPtr<ID2D1Effect> blur;
        ComPtr<ID2D1Image> output;
    };
    std::vector<MonitorBlurEffect> monitorEffects;
    monitorEffects.reserve(monitorRegions.size());
    for (const auto& region : monitorRegions)
    {
        MonitorBlurEffect effect{};
        effect.destination = region;
        if (FAILED(glassEffectContext_->CreateEffect(CLSID_D2D1Crop,
                &effect.crop)) || !effect.crop ||
            FAILED(glassEffectContext_->CreateEffect(CLSID_D2D1GaussianBlur,
                &effect.blur)) || !effect.blur)
            return false;
        effect.crop->SetInput(0, source);
        effect.crop->SetValue(D2D1_CROP_PROP_RECT, region);
        effect.crop->SetValue(D2D1_CROP_PROP_BORDER_MODE,
            D2D1_BORDER_MODE_HARD);
        effect.blur->SetInputEffect(0, effect.crop.Get());
        effect.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
            blurRadius * kGlassBackdropScale);
        effect.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
            D2D1_BORDER_MODE_HARD);
        effect.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
            D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
        effect.blur->GetOutput(&effect.output);
        if (!effect.output ||
            FAILED(glassEffectContext_->GetImageLocalBounds(effect.output.Get(),
                &effect.imageBounds)) ||
            effect.imageBounds.right <= effect.imageBounds.left ||
            effect.imageBounds.bottom <= effect.imageBounds.top)
            return false;
        monitorEffects.push_back(std::move(effect));
    }

    glassEffectContext_->SetTarget(target.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    glassEffectContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    for (const auto& effect : monitorEffects)
    {
        glassEffectContext_->PushAxisAlignedClip(effect.destination,
            D2D1_ANTIALIAS_MODE_ALIASED);
        glassEffectContext_->DrawImage(effect.output.Get(),
            D2D1::Point2F(effect.destination.left, effect.destination.top),
            effect.imageBounds, D2D1_INTERPOLATION_MODE_LINEAR,
            D2D1_COMPOSITE_MODE_SOURCE_COPY);
        glassEffectContext_->PopAxisAlignedClip();
    }
    HRESULT hr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    if (FAILED(hr))
        return false;

    ScheduleGlassLuminanceReadback(target.Get());

    if (glassBackdropRadiusCache_.size() >= kGlassRadiusCacheLimit)
        glassBackdropRadiusCache_.erase(glassBackdropRadiusCache_.begin());
    glassBackdropRadiusCache_[radiusKey] = target;
    glassBackdropBitmap_ = std::move(target);
    selectPreviousRadius();
    return true;
}

/**
 * @brief 确保毛玻璃背景快照可用。
 * @details 编排：检测动态壁纸窗口（10s 防抖）→ 静态层按需重建 →
 *          动态模式时捕获动态帧并叠加 → 高斯模糊输出。
 *          静态签名（尺寸/铺放/背景色/各屏路径与矩形）不变时静态层
 *          直接复用；动态模式在脏标记触发时总是重捕帧（画面逐帧变化）。
 *          动态捕获失败时直接返回失败并保留具体 Hook 阶段，不切换捕获后端
 *          或静态壁纸，以免掩盖兼容性问题。
 * @return true 快照可绘制；false 捕获或合成失败
 */
inline bool DesktopApp::EnsureGlassBackdrop(float blurRadius, int refreshMode)
{
    if (!d2dDevice_ || !hwnd_ || !IsWindow(hwnd_)) return false;
    blurRadius = std::clamp(blurRadius, 4.0f, 48.0f);
    refreshMode = std::clamp(refreshMode, 0, 3);

    // 本函数会被每个玻璃面板各调用一次。缓存有效时必须在壁纸 COM 查询、
    // 路径签名构建和动态窗口枚举之前返回，否则一次 hover 重绘会按面板数
    // 重复执行整套桌面查询，持续挤占 UI 线程。
    if (!glassBackdropDirty_)
    {
        if (!dynamicWallpaperWindows_.empty() && dynamicWallpaperIncompatible_)
            return false;
        ID2D1Bitmap1* source = glassWasDynamic_
            ? glassComposeBitmap_.Get()
            : glassStaticLayerBitmap_.Get();
        if (source)
            return BlurGlassBitmapToBackdrop(source, blurRadius);
    }

    // 同一桌面绘制帧中的首个面板已经请求过 Present 时，其余面板直接复用
    // 上一张有效动态帧；否则组件数量会放大 Hook 请求和桌面查询开销。
    if (dynamicWallpaperCaptureDeferred_ &&
        glassLastCaptureAttemptSerial_ == glassPaintSerial_ &&
        glassWasDynamic_ && glassComposeBitmap_)
        return BlurGlassBitmapToBackdrop(glassComposeBitmap_.Get(), blurRadius);

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = static_cast<int>(client.right - client.left);
    const int height = static_cast<int>(client.bottom - client.top);
    if (width < 8 || height < 8) return false;

    if (!glassEffectContext_)
    {
        if (FAILED(d2dDevice_->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &glassEffectContext_)) ||
            !glassEffectContext_)
            return false;
        glassEffectContext_->SetDpi(96.0f, 96.0f);
        glassEffectContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    }

    DetectDynamicWallpaperWindows(false);
    if (!dynamicWallpaperWindows_.empty() && dynamicWallpaperIncompatible_)
        return false;

    // ── 1. 静态层（签名不变不重建）─────────────────────────
    std::vector<GlassWallpaperSource> sources;
    int position = 4;
    D2D1_COLOR_F bgColor{};
    if (!QueryGlassWallpaperSources(sources, position, bgColor))
        return false;

    std::wstring staticSig;
    staticSig.reserve(512);
    staticSig += std::to_wstring(width); staticSig += L'x';
    staticSig += std::to_wstring(height); staticSig += L'|';
    staticSig += std::to_wstring(position); staticSig += L'|';
    staticSig += std::to_wstring(static_cast<int>(std::lround(bgColor.r * 255.0f)));
    staticSig += L',';
    staticSig += std::to_wstring(static_cast<int>(std::lround(bgColor.g * 255.0f)));
    staticSig += L',';
    staticSig += std::to_wstring(static_cast<int>(std::lround(bgColor.b * 255.0f)));
    staticSig += L'|';
    for (const auto& src : sources)
    {
        staticSig += src.path; staticSig += L'@';
        staticSig += std::to_wstring(static_cast<int>(src.rect.left)); staticSig += L',';
        staticSig += std::to_wstring(static_cast<int>(src.rect.top)); staticSig += L',';
        staticSig += std::to_wstring(static_cast<int>(src.rect.right)); staticSig += L',';
        staticSig += std::to_wstring(static_cast<int>(src.rect.bottom)); staticSig += L';';
    }

    const UINT sampleW = std::max<UINT>(1,
        static_cast<UINT>(std::lround(width * kGlassBackdropScale)));
    const UINT sampleH = std::max<UINT>(1,
        static_cast<UINT>(std::lround(height * kGlassBackdropScale)));

    bool staticLayerChanged = false;
    if (staticSig != glassStaticSignature_ || !glassStaticLayerBitmap_)
    {
        if (!ComposeGlassStaticLayer(sources, position, bgColor, sampleW, sampleH))
            return false;
        glassStaticSignature_ = staticSig;
        staticLayerChanged = true;
    }

    // ── 2. 动态/静态分支 → 模糊输出 ────────────────────────
    const bool dynamicActive = !dynamicWallpaperWindows_.empty();

    if (!dynamicActive)
    {
        if (staticLayerChanged || glassWasDynamic_)
            BeginGlassBackdropTransition(refreshMode);
        if (!BlurGlassBitmapToBackdrop(glassStaticLayerBitmap_.Get(), blurRadius))
            return false;
        glassBackdropSignature_ = staticSig;
        glassWasDynamic_ = false;
    }
    else
    {
        // 动态候选签名：窗口集合或位置变化时重建（画面内容变化靠脏标记驱动）
        std::wstring finalSig = staticSig;
        finalSig += L"#D#";
        for (const auto& win : dynamicWallpaperWindows_)
        {
            finalSig += std::to_wstring(reinterpret_cast<UINT_PTR>(win.hwnd));
            finalSig += L'@';
            finalSig += std::to_wstring(static_cast<int>(win.rect.left));
            finalSig += L',';
            finalSig += std::to_wstring(static_cast<int>(win.rect.top));
            finalSig += L',';
            finalSig += std::to_wstring(static_cast<int>(win.rect.right));
            finalSig += L',';
            finalSig += std::to_wstring(static_cast<int>(win.rect.bottom));
            finalSig += L';';
        }

        ComPtr<ID2D1Bitmap1> frame;
        glassLastCaptureAttemptSerial_ = glassPaintSerial_;
        if (!CaptureDynamicWallpaperLayer(refreshMode, &frame))
        {
            if (dynamicWallpaperCaptureDeferred_)
            {
                // 下一次 Present 尚未到达不是捕获故障；短周期定时器会继续
                // 驱动重绘；已有动态帧继续显示，不让玻璃闪成透明一帧。
                if (glassWasDynamic_ && glassComposeBitmap_)
                    return BlurGlassBitmapToBackdrop(glassComposeBitmap_.Get(), blurRadius);
                return false;
            }
            dynamicWallpaperIncompatible_ = true;
            dynamicWallpaperIncompatibleTick_ = GetTickCount();
            StopWallpaperEngineCaptures();
            std::wstring message = L"Dynamic wallpaper GPU Hook failed: ";
            message += dynamicWallpaperCaptureError_.empty()
                ? L"unknown stage"
                : dynamicWallpaperCaptureError_;
            WriteCrashLogEntry(message.c_str());
            glassBackdropBitmap_.Reset();
            glassLuminanceMap_.clear();
            glassLuminanceReadbackPending_.fill(false);
            glassBackdropRadiusCache_.clear();
            ClearGlassBackdropTransition();
            glassBackdropSignature_.clear();
            glassWasDynamic_ = false;
            glassBackdropDirty_ = false;
            glassLastCaptureTick_ = GetTickCount();
            return false;
        }
        else
        {
            if (!ComposeGlassDynamicFrame(frame.Get()))
            {
                dynamicWallpaperCaptureError_ = L"D2D 动态帧合成失败";
                WriteCrashLogEntry(L"Dynamic wallpaper GPU Hook failed: D2D compose");
                return false;
            }
            // glassComposeBitmap_ 已写入新帧。低/中频保留上一组模糊结果，
            // 由绘制路径在短时间内做 GPU 交叉淡化；其他档直接替换。
            BeginGlassBackdropTransition(refreshMode);
            if (!BlurGlassBitmapToBackdrop(glassComposeBitmap_.Get(), blurRadius))
            {
                dynamicWallpaperCaptureError_ = L"D2D 高斯模糊失败";
                WriteCrashLogEntry(L"Dynamic wallpaper GPU Hook failed: D2D blur");
                return false;
            }
            frame.Reset();

            if (!glassWasDynamic_)
                WriteCrashLogEntry(L"Dynamic wallpaper GPU shared texture active");
            glassBackdropSignature_ = finalSig;
            glassWasDynamic_ = true;
        }
    }

    glassBackdropDirty_ = false;
    glassLastCaptureTick_ = GetTickCount();
    if ((refreshMode == 1 || refreshMode == 2) &&
        glassRefreshTimerActive_ && hwnd_ && IsWindow(hwnd_))
    {
        const UINT interval = refreshMode == 1
            ? static_cast<UINT>(kGlassRefreshLowMs)
            : static_cast<UINT>(kGlassRefreshMidMs);
        SetTimer(hwnd_, kGlassRefreshTimerId, interval, nullptr);
        glassRefreshTimerIntervalMs_ = interval;
    }
    return glassBackdropBitmap_ != nullptr;
}

/**
 * @brief 在面板矩形内绘制毛玻璃背景采样。
 * @details 目标矩形与源矩形按 kGlassBackdropScale 换算一一对应；
 *          radius > 0.5 时用圆角几何裁剪（角落不露出方形背景）。基础背景
 *          绘制后，在 2–4 px 边缘带内向面板中心偏移采样，模拟轻微折射。
 * @param ctx    主渲染 D2D 设备上下文
 * @param frame  面板矩形（客户区坐标）
 * @param radius 面板圆角半径
 */
inline void DesktopApp::DrawGlassBackdropRegion(ID2D1DeviceContext* ctx, RECT frame, float radius)
{
    if (!ctx || !glassBackdropBitmap_ || IsRectEmptyRect(frame)) return;

    const D2D1_RECT_F dest = ToD2DRect(frame);
    const D2D1_RECT_F src = D2D1::RectF(
        frame.left * kGlassBackdropScale, frame.top * kGlassBackdropScale,
        frame.right * kGlassBackdropScale, frame.bottom * kGlassBackdropScale);

    bool clipped = false;
    ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
    if (radius > 0.5f && d2dFactory_)
    {
        if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(dest, radius, radius), &clipGeo)) && clipGeo)
        {
            ctx->PushLayer(D2D1::LayerParameters(dest, clipGeo.Get()), nullptr);
            clipped = true;
        }
    }

    const float width = dest.right - dest.left;
    const float height = dest.bottom - dest.top;
    const float rimWidth = std::min(
        std::clamp(2.0f + radius * 0.08f, 2.0f, 4.0f),
        std::max(0.0f, std::min(width, height) * 0.45f));
    const float displacement = std::clamp(rimWidth * 0.46f, 0.9f, 1.8f);
    auto drawRefractedRim = [&](ID2D1Bitmap1* bitmap, float opacity,
        ComPtr<ID2D1Bitmap1>& cachedSource,
        ComPtr<ID2D1BitmapBrush1>& cachedBrush) {
        if (!bitmap || rimWidth < 0.5f || opacity <= 0.001f)
            return;
        if (cachedSource.Get() != bitmap || !cachedBrush)
        {
            cachedSource = bitmap;
            cachedBrush.Reset();
            const D2D1_BITMAP_BRUSH_PROPERTIES1 bitmapProperties =
                D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP,
                    D2D1_EXTEND_MODE_CLAMP,
                    D2D1_INTERPOLATION_MODE_LINEAR);
            if (FAILED(ctx->CreateBitmapBrush(bitmap, &bitmapProperties,
                    nullptr, &cachedBrush)) || !cachedBrush)
                return;
        }
        const float sourceLeft = (frame.left + displacement) * kGlassBackdropScale;
        const float sourceTop = (frame.top + displacement) * kGlassBackdropScale;
        const float sourceWidth = std::max(0.5f,
            (width - displacement * 2.0f) * kGlassBackdropScale);
        const float sourceHeight = std::max(0.5f,
            (height - displacement * 2.0f) * kGlassBackdropScale);
        const float scaleX = width / sourceWidth;
        const float scaleY = height / sourceHeight;
        cachedBrush->SetTransform(D2D1::Matrix3x2F(
            scaleX, 0.0f, 0.0f, scaleY,
            dest.left - sourceLeft * scaleX,
            dest.top - sourceTop * scaleY));
        cachedBrush->SetOpacity(opacity);

        const float halfRim = rimWidth * 0.5f;
        const D2D1_RECT_F rimRect = D2D1::RectF(
            dest.left + halfRim, dest.top + halfRim,
            dest.right - halfRim, dest.bottom - halfRim);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(rimRect,
            std::max(0.0f, radius - halfRim),
            std::max(0.0f, radius - halfRim)), cachedBrush.Get(), rimWidth);
    };

    float currentOpacity = 1.0f;
    bool drawPrevious = glassPreviousBackdropBitmap_ &&
        glassTransitionStartTick_ != 0 && glassTransitionDurationMs_ != 0;
    if (drawPrevious)
    {
        const D2D1_SIZE_U currentSize = glassBackdropBitmap_->GetPixelSize();
        const D2D1_SIZE_U previousSize = glassPreviousBackdropBitmap_->GetPixelSize();
        drawPrevious = currentSize.width == previousSize.width &&
            currentSize.height == previousSize.height;
    }
    if (drawPrevious)
    {
        const DWORD elapsed = GetTickCount() - glassTransitionStartTick_;
        currentOpacity = std::clamp(
            static_cast<float>(elapsed) /
                static_cast<float>(glassTransitionDurationMs_),
            0.0f, 1.0f);
        ctx->DrawBitmap(glassPreviousBackdropBitmap_.Get(), dest, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        drawRefractedRim(glassPreviousBackdropBitmap_.Get(), 0.62f,
            glassRefractionPreviousSource_, glassRefractionPreviousBrush_);
    }
    ctx->DrawBitmap(glassBackdropBitmap_.Get(), dest, currentOpacity,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    drawRefractedRim(glassBackdropBitmap_.Get(), 0.62f * currentOpacity,
        glassRefractionCurrentSource_, glassRefractionCurrentBrush_);
    if (clipped) ctx->PopLayer();
}

inline void DesktopApp::TryConsumeGlassLuminanceReadbacks()
{
    if (!d3dImmediateContext_)
        return;
    for (size_t index = 0; index < glassLuminanceReadbacks_.size(); ++index)
    {
        if (!glassLuminanceReadbackPending_[index] ||
            !glassLuminanceReadbacks_[index])
            continue;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = d3dImmediateContext_->Map(
            glassLuminanceReadbacks_[index].Get(), 0,
            D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
            continue;
        glassLuminanceReadbackPending_[index] = false;
        if (FAILED(hr))
            continue;

        glassLuminanceMap_.resize(
            static_cast<size_t>(kGlassLuminanceWidth) * kGlassLuminanceHeight);
        for (UINT y = 0; y < kGlassLuminanceHeight; ++y)
        {
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < kGlassLuminanceWidth; ++x)
            {
                const auto* pixel = row + static_cast<size_t>(x) * 4;
                const float b = pixel[0] / 255.0f;
                const float g = pixel[1] / 255.0f;
                const float r = pixel[2] / 255.0f;
                glassLuminanceMap_[static_cast<size_t>(y) * kGlassLuminanceWidth + x] =
                    std::clamp(r * 0.2126f + g * 0.7152f + b * 0.0722f,
                        0.0f, 1.0f);
            }
        }
        d3dImmediateContext_->Unmap(glassLuminanceReadbacks_[index].Get(), 0);
    }
}

inline void DesktopApp::ScheduleGlassLuminanceReadback(ID2D1Bitmap1* source)
{
    if (!source || !glassEffectContext_ || !d3dDevice_ || !d3dImmediateContext_)
        return;

    TryConsumeGlassLuminanceReadbacks();
    size_t freeIndex = glassLuminanceReadbacks_.size();
    for (size_t index = 0; index < glassLuminanceReadbacks_.size(); ++index)
    {
        if (!glassLuminanceReadbackPending_[index])
        {
            freeIndex = index;
            break;
        }
    }
    if (freeIndex >= glassLuminanceReadbacks_.size())
        return;

    if (!glassLuminanceBitmap_)
    {
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(glassEffectContext_->CreateBitmap(
                D2D1::SizeU(kGlassLuminanceWidth, kGlassLuminanceHeight),
                nullptr, 0, &properties, &glassLuminanceBitmap_)) ||
            !glassLuminanceBitmap_)
            return;
    }

    const D2D1_SIZE_U sourceSize = source->GetPixelSize();
    glassEffectContext_->SetTarget(glassLuminanceBitmap_.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    glassEffectContext_->Clear(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f));
    glassEffectContext_->DrawBitmap(source,
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(kGlassLuminanceWidth),
            static_cast<float>(kGlassLuminanceHeight)),
        1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(sourceSize.width),
            static_cast<float>(sourceSize.height)));
    const HRESULT drawHr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    if (FAILED(drawHr))
        return;

    ComPtr<IDXGISurface> surface;
    ComPtr<ID3D11Texture2D> gpuTexture;
    if (FAILED(glassLuminanceBitmap_->GetSurface(&surface)) || !surface ||
        FAILED(surface.As(&gpuTexture)) || !gpuTexture)
        return;

    if (!glassLuminanceReadbacks_[freeIndex])
    {
        D3D11_TEXTURE2D_DESC description{};
        gpuTexture->GetDesc(&description);
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        if (FAILED(d3dDevice_->CreateTexture2D(&description, nullptr,
                &glassLuminanceReadbacks_[freeIndex])) ||
            !glassLuminanceReadbacks_[freeIndex])
            return;
    }

    d3dImmediateContext_->CopyResource(glassLuminanceReadbacks_[freeIndex].Get(),
        gpuTexture.Get());
    glassLuminanceReadbackPending_[freeIndex] = true;
    // 静态壁纸的“仅事件”档也需要一个后续绘制帧来非阻塞消费回读。
    InvalidateGlassRequestedRegions();
}

inline float DesktopApp::SampleGlassBorderLuminance(RECT frame)
{
    TryConsumeGlassLuminanceReadbacks();
    if (glassLuminanceMap_.size() !=
            static_cast<size_t>(kGlassLuminanceWidth) * kGlassLuminanceHeight ||
        !hwnd_ || !IsWindow(hwnd_))
        return 0.5f;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const float clientWidth = static_cast<float>(std::max<LONG>(1, client.right));
    const float clientHeight = static_cast<float>(std::max<LONG>(1, client.bottom));
    auto mapX = [&](LONG value) {
        return std::clamp(static_cast<int>(std::floor(
            value / clientWidth * kGlassLuminanceWidth)),
            0, static_cast<int>(kGlassLuminanceWidth) - 1);
    };
    auto mapY = [&](LONG value) {
        return std::clamp(static_cast<int>(std::floor(
            value / clientHeight * kGlassLuminanceHeight)),
            0, static_cast<int>(kGlassLuminanceHeight) - 1);
    };
    const int left = mapX(frame.left);
    const int right = mapX(std::max(frame.left, frame.right - 1));
    const int top = mapY(frame.top);
    const int bottom = mapY(std::max(frame.top, frame.bottom - 1));
    const int edgeX = std::max(1, (right - left + 1) / 4);
    const int edgeY = std::max(1, (bottom - top + 1) / 4);
    float sum = 0.0f;
    int count = 0;
    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            if (x >= left + edgeX && x <= right - edgeX &&
                y >= top + edgeY && y <= bottom - edgeY)
                continue;
            sum += glassLuminanceMap_[static_cast<size_t>(y) *
                kGlassLuminanceWidth + static_cast<size_t>(x)];
            ++count;
        }
    }
    return count > 0 ? std::clamp(sum / count, 0.0f, 1.0f) : 0.5f;
}

/**
 * @brief 绘制内容自适应的液态玻璃边缘。
 * @details 左上角使用柔亮斜向高光，右下角收暗；外侧增加低强度光晕，
 *          内侧增加暗边形成厚度。明暗强度由异步回读的面板边缘背景亮度
 *          调节，因此亮壁纸强化暗内缘、暗壁纸强化亮外缘。
 */
inline bool DesktopApp::DrawGlassBorder(ID2D1DeviceContext* ctx, RECT frame,
    float radius, D2D1_COLOR_F color, float strokeWidth)
{
    if (!ctx || color.a <= 0.0f || IsRectEmptyRect(frame))
        return false;

    const float luminance = SampleGlassBorderLuminance(frame);
    const float lightAdapt = 1.15f - luminance * 0.48f;
    auto mixWhite = [](float value, float amount) {
        return std::clamp(value + (1.0f - value) * amount, 0.0f, 1.0f);
    };
    const D2D1_COLOR_F bright = D2D1::ColorF(
        mixWhite(color.r, 0.58f), mixWhite(color.g, 0.58f),
        mixWhite(color.b, 0.58f),
        std::clamp(color.a * lightAdapt, 0.0f, 1.0f));
    const D2D1_COLOR_F middle = D2D1::ColorF(color.r, color.g, color.b,
        std::clamp(color.a * (0.72f - luminance * 0.10f), 0.0f, 1.0f));
    const D2D1_COLOR_F dim = D2D1::ColorF(
        color.r * 0.72f, color.g * 0.72f, color.b * 0.72f,
        std::clamp(color.a * (0.24f + (1.0f - luminance) * 0.08f),
            0.0f, 1.0f));
    const D2D1_GRADIENT_STOP outerStops[] = {
        { 0.0f, bright },
        { 0.38f, middle },
        { 0.72f, D2D1::ColorF(color.r, color.g, color.b, color.a * 0.42f) },
        { 1.0f, dim },
    };
    ComPtr<ID2D1GradientStopCollection> outerCollection;
    ComPtr<ID2D1LinearGradientBrush> outerBrush;
    if (FAILED(ctx->CreateGradientStopCollection(outerStops,
            static_cast<UINT32>(std::size(outerStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &outerCollection)) || !outerCollection ||
        FAILED(ctx->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(static_cast<float>(frame.left),
                    static_cast<float>(frame.top)),
                D2D1::Point2F(static_cast<float>(frame.right),
                    static_cast<float>(frame.bottom))),
            outerCollection.Get(), &outerBrush)) || !outerBrush)
        return false;

    const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(ToD2DRect(frame),
        radius, radius);
    outerBrush->SetOpacity(0.24f);
    ctx->DrawRoundedRectangle(outer, outerBrush.Get(), strokeWidth + 1.35f);
    outerBrush->SetOpacity(1.0f);
    ctx->DrawRoundedRectangle(outer, outerBrush.Get(), strokeWidth);

    const float inset = std::max(0.85f, strokeWidth * 0.85f);
    const D2D1_RECT_F innerRect = D2D1::RectF(
        frame.left + inset, frame.top + inset,
        frame.right - inset, frame.bottom - inset);
    if (innerRect.right > innerRect.left && innerRect.bottom > innerRect.top)
    {
        const float darkAlpha = std::clamp(
            color.a * (0.18f + luminance * 0.68f), 0.025f, 0.24f);
        ComPtr<ID2D1SolidColorBrush> innerBrush;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, darkAlpha),
                &innerBrush)) && innerBrush)
        {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(innerRect,
                std::max(0.0f, radius - inset),
                std::max(0.0f, radius - inset)), innerBrush.Get(),
                std::max(0.65f, strokeWidth * 0.65f));
        }
    }
    return true;
}

/**
 * @brief 获取动态壁纸状态文本（设置界面只读状态行）。
 */
inline std::wstring DesktopApp::GetDynamicWallpaperStatusText() const
{
    std::wstring status;
    if (dynamicWallpaperWindows_.empty())
        status = L"未检测到动态壁纸引擎";
    else if (dynamicWallpaperIncompatible_)
    {
        if (!dynamicWallpaperCaptureError_.empty())
            status = L"GPU Hook 捕获失败：" + dynamicWallpaperCaptureError_;
        else
            status = L"GPU Hook 捕获失败";
    }
    else if (dynamicWallpaperCaptureDeferred_)
    {
        if (!dynamicWallpaperCaptureError_.empty())
            status = dynamicWallpaperCaptureError_;
        else
            status = L"GPU Hook 已连接，等待 Wallpaper Engine 下一帧";
    }
    else if (!dynamicWallpaperEngine_.empty())
        status = L"已通过 DXGI 共享纹理捕获 " + dynamicWallpaperEngine_;
    else
        status = L"已通过 DXGI 共享纹理捕获动态壁纸";

    status += L"\n玻璃面板：";
    status += std::to_wstring(glassRequestedRegions_.size());
    status += glassRefreshThrottled_
        ? L"；检测到前台最大化/全屏应用，所有档位临时切为仅事件刷新"
        : L"；按所选档位刷新";
    return status;
}

inline void DesktopApp::InvalidateGlassRequestedRegions()
{
    if (!hwnd_ || !IsWindow(hwnd_))
        return;
    if (glassRequestedRegions_.empty())
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    for (RECT region : glassRequestedRegions_)
    {
        InflateRect(&region, 2, 2);
        RECT clipped{};
        if (IntersectRect(&clipped, &region, &client))
            InvalidateRect(hwnd_, &clipped, FALSE);
    }
}

/**
 * @brief 按当前设置维护毛玻璃刷新状态。
 * @details 每帧 OnPaint 调用一次：
 *          - 玻璃整体关闭时释放快照/静态层/壁纸缓存并停止实时定时器；
 *          - 隐藏图标期间置脏，恢复显示时强制重评估；
 *          - 仅事件档用 WinEvent 感知动态壁纸窗口生命周期，系统消息感知原生壁纸；
 *          - 低频/中频/实时档分别维护 3s/1s/66ms 重绘定时器；
 *          - 静态场景签名不变时重绘零成本，动态场景按档位请求新共享帧。
 */
inline void DesktopApp::UpdateGlassRefreshState()
{
    const bool panelGlassActive = glassRequestedByPanels_;
    const int panelRefreshMode = glassRequestedRefreshMode_;
    glassRequestedByPanels_ = false;
    glassRequestedRefreshMode_ = 0;
    glassRequestedRegions_.clear();

    bool globalGlassActive = false;
    bool dockGlassActive = false;
    int sharedRefreshMode = 0;
    if (settingsWindow_)
    {
        const auto& global = settingsWindow_->GetPersonalization();
        globalGlassActive = global.glassEnabled;
        const auto dock = settingsWindow_->GetDockAppearance();
        dockGlassActive = dock.glassEnabled;
        sharedRefreshMode = std::clamp(global.glassRefreshMode, 0, 3);
    }
    const bool glassActive = globalGlassActive || dockGlassActive || panelGlassActive;

    if (!glassActive)
    {
        if (glassBackdropBitmap_)
            glassBackdropBitmap_.Reset();
        glassLuminanceBitmap_.Reset();
        for (auto& readback : glassLuminanceReadbacks_)
            readback.Reset();
        glassLuminanceReadbackPending_.fill(false);
        glassLuminanceMap_.clear();
        glassRefractionCurrentSource_.Reset();
        glassRefractionCurrentBrush_.Reset();
        glassRefractionPreviousSource_.Reset();
        glassRefractionPreviousBrush_.Reset();
        glassBackdropRadiusCache_.clear();
        ClearGlassBackdropTransition();
        if (glassStaticLayerBitmap_)
            glassStaticLayerBitmap_.Reset();
        if (glassComposeBitmap_)
            glassComposeBitmap_.Reset();
        if (glassDynamicLayerBitmap_)
            glassDynamicLayerBitmap_.Reset();
        glassCapturedDynamicWindows_.clear();
        glassWallpaperCache_.clear();
        glassBackdropSignature_.clear();
        glassStaticSignature_.clear();
        glassBackdropDirty_ = true;
        glassWasDynamic_ = false;
        StopWallpaperEngineCaptures();
        dynamicWallpaperWindows_.clear();
        dynamicWallpaperEngine_.clear();
        dynamicWallpaperCaptureError_.clear();
        dynamicWallpaperCaptureDeferred_ = false;
        dynamicWallpaperIncompatible_ = false;
        glassEffectiveRefreshMode_ = 0;
        glassRefreshThrottled_ = false;
        glassLastCaptureAttemptSerial_ = std::numeric_limits<std::uint64_t>::max();
        glassLastDetectTick_ = 0;
        StopGlassWallpaperEventMonitor();
        if (glassRefreshTimerActive_)
        {
            if (hwnd_ && IsWindow(hwnd_))
                KillTimer(hwnd_, kGlassRefreshTimerId);
            glassRefreshTimerActive_ = false;
            glassRefreshTimerIntervalMs_ = 0;
        }
        return;
    }

    StartGlassWallpaperEventMonitor();

    if (desktopIconsHidden_)
        glassBackdropDirty_ = true;

    int mode = panelGlassActive ? std::clamp(panelRefreshMode, 0, 3) : 0;
    if (globalGlassActive || dockGlassActive)
        mode = std::max(mode, sharedRefreshMode);

    const bool previousThrottled = glassRefreshThrottled_;
    glassRefreshThrottled_ = IsGlassForegroundMaximizedOrFullscreen();
    if (glassRefreshThrottled_)
        mode = 0;
    if (previousThrottled != glassRefreshThrottled_)
        glassBackdropDirty_ = true;

    glassEffectiveRefreshMode_ = mode;
    if (mode == 1 || mode == 2)
    {
        const DWORD interval = (mode == 1) ? kGlassRefreshLowMs : kGlassRefreshMidMs;
        if (glassLastCaptureTick_ != 0 &&
            GetTickCount() - glassLastCaptureTick_ >= interval)
            glassBackdropDirty_ = true;
    }
    else if (mode == 3)
    {
        glassBackdropDirty_ = true;
    }

    UINT desiredInterval = 0;
    if (!desktopIconsHidden_)
    {
        // 仅用轻量轮询发现前台窗口退出最大化/全屏；轮询本身不请求新壁纸帧。
        if (glassRefreshThrottled_)
            desiredInterval = static_cast<UINT>(kGlassRefreshMidMs);
        else if (mode == 1)
            desiredInterval = static_cast<UINT>(kGlassRefreshLowMs);
        else if (mode == 2)
            desiredInterval = static_cast<UINT>(kGlassRefreshMidMs);
        else if (mode == 3)
            desiredInterval = kGlassRefreshRealtimeIntervalMs;
    }
    const bool canUseTimer = desiredInterval != 0 && hwnd_ && IsWindow(hwnd_);
    if (glassRefreshTimerActive_ &&
        (!canUseTimer || glassRefreshTimerIntervalMs_ != desiredInterval))
    {
        if (hwnd_ && IsWindow(hwnd_))
            KillTimer(hwnd_, kGlassRefreshTimerId);
        glassRefreshTimerActive_ = false;
        glassRefreshTimerIntervalMs_ = 0;
    }
    if (canUseTimer && !glassRefreshTimerActive_)
    {
        SetTimer(hwnd_, kGlassRefreshTimerId, desiredInterval, nullptr);
        glassRefreshTimerActive_ = true;
        glassRefreshTimerIntervalMs_ = desiredInterval;
    }
}
