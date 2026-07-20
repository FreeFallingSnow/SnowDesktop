/**
 * @file app_glass.h
 * @brief DesktopApp 的毛玻璃（苹果 Dock 风格）背景模块
 * @details 快照内容分两层：
 *          - 静态层：通过 IDesktopWallpaper 按显示器查询当前壁纸与铺放方式，
 *            WIC 解码后按桌面规则合成到 0.5x 位图（签名不变不重建）；
 *          - 动态层：检测到 Wallpaper Engine 后，在其 Present/Present1
 *            入口复制 BackBuffer 到共享 D3D11 纹理，再由本进程直接拼合。
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

/** @brief 动态壁纸窗口检测间隔（毫秒）。 */
constexpr DWORD kGlassDetectIntervalMs = 10000;

/**
 * @brief 使毛玻璃背景快照失效。
 * @details 在壁纸/显示器拓扑/窗口几何变化、玻璃参数调整等时机调用，
 *          下次绘制时 EnsureGlassBackdrop 会重新评估签名并视情况重合成。
 */
inline void DesktopApp::InvalidateGlassBackdrop()
{
    glassBackdropDirty_ = true;
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

    // 候选集合变化时重置 Hook 失败标记（给新引擎一次机会）；
    // 同一候选集合下每 30 秒也重试一次（引擎启动初期可能暂时黑屏）
    const bool changed = found.size() != dynamicWallpaperWindows_.size() ||
        !std::equal(found.begin(), found.end(), dynamicWallpaperWindows_.begin(),
            [](const DynamicWallpaperWindow& a, const DynamicWallpaperWindow& b) {
                return a.hwnd == b.hwnd;
            });
    const bool retryDue = dynamicWallpaperIncompatible_ &&
        now - dynamicWallpaperIncompatibleTick_ >= 30000;
    if (changed || retryDue)
    {
        if (wallpaperEngineCapture_)
            wallpaperEngineCapture_->Stop();
        wallpaperEngineCapture_.reset();
        dynamicWallpaperIncompatible_ = false;
        dynamicWallpaperIncompatibleTick_ = 0;
        dynamicWallpaperCaptureError_.clear();
    }

    dynamicWallpaperWindows_ = std::move(found);
    if (dynamicWallpaperWindows_.empty() && wallpaperEngineCapture_)
    {
        wallpaperEngineCapture_->Stop();
        wallpaperEngineCapture_.reset();
    }

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
            L"Dynamic wallpaper source count=%u class=%s backend=DXGI-Present-Hook engine=%s",
            static_cast<unsigned>(dynamicWallpaperWindows_.size()), className,
            dynamicWallpaperEngine_.empty() ? L"unknown" : dynamicWallpaperEngine_.c_str());
        WriteCrashLogEntry(message);
    }
}

/**
 * @brief 通过 Wallpaper Engine DXGI Hook 共享纹理取得动态壁纸 GPU 帧。
 * @details 各显示器帧保持在 D3D11/DXGI 上，直接降采样拼合成 0.5x D2D
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

    auto fail = [this](const std::wstring& stage) {
        dynamicWallpaperCaptureError_ = stage;
        return false;
    };
    auto defer = [this](const std::wstring& stage) {
        dynamicWallpaperCaptureDeferred_ = true;
        dynamicWallpaperCaptureError_ = stage;
        if (!glassRefreshTimerActive_ && hwnd_ && IsWindow(hwnd_))
        {
            SetTimer(hwnd_, kGlassRefreshTimerId, kGlassRefreshRealtimeIntervalMs, nullptr);
            glassRefreshTimerActive_ = true;
        }
        return false;
    };

    if (!glassEffectContext_ || !d3dDevice_)
        return fail(L"D3D/D2D 设备未就绪");
    if (dynamicWallpaperWindows_.empty())
        return fail(L"动态壁纸候选已消失");

    DWORD rendererPid = 0;
    bool wallpaperEngineWindow = dynamicWallpaperEngine_ == L"Wallpaper Engine";
    for (const auto& window : dynamicWallpaperWindows_)
    {
        wchar_t className[64]{};
        GetClassNameW(window.hwnd, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"WPEDesktopDX11Window") == 0 ||
            _wcsicmp(className, L"WPECloneView") == 0)
        {
            wallpaperEngineWindow = true;
            rendererPid = window.rendererPid;
            if (!rendererPid)
                GetWindowThreadProcessId(window.hwnd, &rendererPid);
            break;
        }
    }
    if (!wallpaperEngineWindow || !rendererPid)
        return fail(L"DXGI Present Hook 当前仅支持 Wallpaper Engine");

    const int mode = std::clamp(refreshMode, 0, 3);
    DWORD intervalMs = 0;
    if (mode == 1)
        intervalMs = kGlassRefreshLowMs;
    else if (mode == 2)
        intervalMs = kGlassRefreshMidMs;
    else if (mode == 3)
        intervalMs = kGlassRefreshRealtimeIntervalMs;

    if (!wallpaperEngineCapture_)
        wallpaperEngineCapture_ = std::make_unique<WallpaperEngineCaptureSession>();
    if (!wallpaperEngineCapture_->EnsureStarted(rendererPid, d3dDevice_.Get(),
            glassEffectContext_.Get(), intervalMs))
    {
        const std::wstring error = wallpaperEngineCapture_->LastError();
        if (error.find(L"等待") != std::wstring::npos)
            return defer(error);
        const std::wstring reason = error.empty()
            ? L"Wallpaper Engine Hook 连接中断"
            : error;
        wallpaperEngineCapture_->RequestReconnect(reason);
        return defer(L"DXGI Hook 中断，等待自动重连：" + reason);
    }

    std::vector<WallpaperEngineFrame> frames;
    const WallpaperEngineFrameState frameState =
        wallpaperEngineCapture_->TryAcquireLatestFrames(frames);
    if (frameState == WallpaperEngineFrameState::pending)
    {
        wallpaperEngineCapture_->RequestFrame();
        return defer(L"等待 Wallpaper Engine 下一次 Present");
    }
    if (frameState == WallpaperEngineFrameState::error)
    {
        std::wstring reason = wallpaperEngineCapture_->LastError();
        if (reason.empty())
            reason = L"Wallpaper Engine 共享帧读取失败";
        wallpaperEngineCapture_->RequestReconnect(reason);
        return defer(L"DXGI Hook 中断，等待自动重连：" + reason);
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
    {
        wallpaperEngineCapture_->ReleaseFrames(false);
        return fail(L"主窗口客户区尺寸无效");
    }
    const UINT sampleW = std::max<UINT>(1,
        static_cast<UINT>(std::lround(width * kGlassBackdropScale)));
    const UINT sampleH = std::max<UINT>(1,
        static_cast<UINT>(std::lround(height * kGlassBackdropScale)));
    if (glassDynamicLayerBitmap_ &&
        (glassDynamicLayerBitmap_->GetPixelSize().width != sampleW ||
            glassDynamicLayerBitmap_->GetPixelSize().height != sampleH))
        glassDynamicLayerBitmap_.Reset();
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
            wallpaperEngineCapture_->ReleaseFrames(false);
            return fail(L"共享纹理动态层创建失败");
        }
    }

    POINT origin{};
    MapWindowPoints(hwnd_, nullptr, &origin, 1);
    const float scale = kGlassBackdropScale;
    glassEffectContext_->SetTarget(glassDynamicLayerBitmap_.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    glassEffectContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    auto drawFrameAtRect = [this, origin, scale, sampleW, sampleH](
        const WallpaperEngineFrame& frame, const RECT& desktopRect) {
        if (!frame.bitmap)
            return;
        const D2D1_SIZE_U sourceSize = frame.bitmap->GetPixelSize();
        D2D1_RECT_F destination = D2D1::RectF(
            (desktopRect.left - origin.x) * scale,
            (desktopRect.top - origin.y) * scale,
            (desktopRect.right - origin.x) * scale,
            (desktopRect.bottom - origin.y) * scale);
        if (destination.right <= destination.left || destination.bottom <= destination.top)
        {
            destination = D2D1::RectF(0.0f, 0.0f,
                static_cast<float>(sampleW), static_cast<float>(sampleH));
        }
        glassEffectContext_->DrawBitmap(frame.bitmap, destination, 1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(sourceSize.width),
                static_cast<float>(sourceSize.height)));
    };
    for (const auto& frame : frames)
        drawFrameAtRect(frame, frame.desktopRect);

    // WPECloneView 没有独立 Present；它由 Wallpaper Engine 把主交换链克隆到
    // 另一显示器。只有在 Hook 未发布该 HWND 的独立交换链时才复用首帧。
    for (const auto& window : dynamicWallpaperWindows_)
    {
        wchar_t className[64]{};
        GetClassNameW(window.hwnd, className, static_cast<int>(std::size(className)));
        if (_wcsicmp(className, L"WPECloneView") != 0)
            continue;
        const bool hasOwnSwapChain = std::any_of(frames.begin(), frames.end(),
            [&window](const WallpaperEngineFrame& frame) {
                return frame.outputWindow == window.hwnd;
            });
        if (!hasOwnSwapChain && !frames.empty())
            drawFrameAtRect(frames.front(), window.rect);
    }
    const HRESULT composeHr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    wallpaperEngineCapture_->ReleaseFrames(SUCCEEDED(composeHr));
    if (FAILED(composeHr))
    {
        wchar_t message[128]{};
        swprintf_s(message, L"Wallpaper Engine 共享帧拼合失败（0x%08X）",
            static_cast<unsigned>(composeHr));
        return fail(message);
    }

    if (mode != 3 && glassRefreshTimerActive_)
    {
        KillTimer(hwnd_, kGlassRefreshTimerId);
        glassRefreshTimerActive_ = false;
    }
    dynamicWallpaperCaptureError_.clear();
    dynamicWallpaperCaptureDeferred_ = false;
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

/**
 * @brief 将 0.5x 位图高斯模糊到 glassBackdropBitmap_。
 * @details 同一源帧按半径缓存输出，使不同 Lua 组件可以使用独立半径，
 *          同时避免每个面板重复执行高斯模糊。
 * @return 成功返回 true
 */
inline bool DesktopApp::BlurGlassBitmapToBackdrop(ID2D1Bitmap1* source, float blurRadius)
{
    if (!source || !glassEffectContext_) return false;

    const D2D1_SIZE_U size = source->GetPixelSize();
    blurRadius = std::clamp(blurRadius, 4.0f, 48.0f);
    const int radiusKey = static_cast<int>(std::lround(blurRadius));
    blurRadius = static_cast<float>(radiusKey);
    auto cached = glassBackdropRadiusCache_.find(radiusKey);
    if (cached != glassBackdropRadiusCache_.end())
    {
        const D2D1_SIZE_U cachedSize = cached->second->GetPixelSize();
        if (cachedSize.width == size.width && cachedSize.height == size.height)
        {
            glassBackdropBitmap_ = cached->second;
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

    ComPtr<ID2D1Effect> blur;
    if (FAILED(glassEffectContext_->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) || !blur)
        return false;
    blur->SetInput(0, source);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
        blurRadius * kGlassBackdropScale);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
        D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);

    glassEffectContext_->SetTarget(target.Get());
    glassEffectContext_->SetTransform(D2D1::Matrix3x2F::Identity());
    glassEffectContext_->BeginDraw();
    glassEffectContext_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    glassEffectContext_->DrawImage(blur.Get());
    HRESULT hr = glassEffectContext_->EndDraw();
    glassEffectContext_->SetTarget(nullptr);
    if (FAILED(hr))
        return false;

    if (glassBackdropRadiusCache_.size() >= kGlassRadiusCacheLimit)
        glassBackdropRadiusCache_.erase(glassBackdropRadiusCache_.begin());
    glassBackdropRadiusCache_[radiusKey] = target;
    glassBackdropBitmap_ = std::move(target);
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

    if (staticSig != glassStaticSignature_ || !glassStaticLayerBitmap_)
    {
        if (!ComposeGlassStaticLayer(sources, position, bgColor, sampleW, sampleH))
            return false;
        glassBackdropBitmap_.Reset();
        glassBackdropRadiusCache_.clear();
        glassStaticSignature_ = staticSig;
    }

    // ── 2. 动态/静态分支 → 模糊输出 ────────────────────────
    const bool dynamicActive = !dynamicWallpaperWindows_.empty();

    if (!dynamicActive)
    {
        if (glassWasDynamic_)
        {
            glassBackdropBitmap_.Reset();
            glassBackdropRadiusCache_.clear();
        }
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
            if (wallpaperEngineCapture_)
                wallpaperEngineCapture_->Stop();
            wallpaperEngineCapture_.reset();
            std::wstring message = L"Dynamic wallpaper DXGI Hook failed: ";
            message += dynamicWallpaperCaptureError_.empty()
                ? L"unknown stage"
                : dynamicWallpaperCaptureError_;
            WriteCrashLogEntry(message.c_str());
            glassBackdropBitmap_.Reset();
            glassBackdropRadiusCache_.clear();
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
                WriteCrashLogEntry(L"Dynamic wallpaper DXGI Hook failed: D2D compose");
                return false;
            }
            // glassComposeBitmap_ 已写入新帧，所有半径的旧模糊结果均失效。
            glassBackdropBitmap_.Reset();
            glassBackdropRadiusCache_.clear();
            if (!BlurGlassBitmapToBackdrop(glassComposeBitmap_.Get(), blurRadius))
            {
                dynamicWallpaperCaptureError_ = L"D2D 高斯模糊失败";
                WriteCrashLogEntry(L"Dynamic wallpaper DXGI Hook failed: D2D blur");
                return false;
            }
            frame.Reset();

            if (!glassWasDynamic_)
                WriteCrashLogEntry(L"Dynamic wallpaper DXGI shared texture active");
            glassBackdropSignature_ = finalSig;
            glassWasDynamic_ = true;
        }
    }

    glassBackdropDirty_ = false;
    glassLastCaptureTick_ = GetTickCount();
    return glassBackdropBitmap_ != nullptr;
}

/**
 * @brief 在面板矩形内绘制毛玻璃背景采样。
 * @details 目标矩形与源矩形按 kGlassBackdropScale 换算一一对应；
 *          radius > 0.5 时用圆角几何裁剪（角落不露出方形背景）。
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
    ctx->DrawBitmap(glassBackdropBitmap_.Get(), dest, 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    if (clipped) ctx->PopLayer();
}

/**
 * @brief 创建毛玻璃边缘光渐变描边画刷。
 * @details 竖直方向线性渐变：顶部为完整边框色（受光边缘），底部衰减到
 *          30% 不透明度，模拟 macOS Dock 玻璃边缘的折射高光。
 * @param ctx   D2D 设备上下文
 * @param frame 面板矩形（决定渐变起止点）
 * @param color 边框颜色（含 alpha）
 * @return 渐变画刷；失败返回空 ComPtr
 */
inline ComPtr<ID2D1LinearGradientBrush> DesktopApp::CreateGlassBorderBrush(
    ID2D1DeviceContext* ctx, RECT frame, D2D1_COLOR_F color)
{
    ComPtr<ID2D1LinearGradientBrush> brush;
    if (!ctx || color.a <= 0.0f) return brush;

    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, D2D1::ColorF(color.r, color.g, color.b, color.a) },
        { 1.0f, D2D1::ColorF(color.r, color.g, color.b, color.a * 0.30f) },
    };
    ComPtr<ID2D1GradientStopCollection> stopCollection;
    if (FAILED(ctx->CreateGradientStopCollection(stops, 2,
            D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stopCollection)) || !stopCollection)
        return brush;

    ctx->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0.0f, static_cast<float>(frame.top)),
            D2D1::Point2F(0.0f, static_cast<float>(frame.bottom))),
        stopCollection.Get(), &brush);
    return brush;
}

/**
 * @brief 获取动态壁纸状态文本（设置界面只读状态行）。
 */
inline std::wstring DesktopApp::GetDynamicWallpaperStatusText() const
{
    if (dynamicWallpaperWindows_.empty())
        return L"未检测到动态壁纸引擎";
    if (dynamicWallpaperIncompatible_)
    {
        if (!dynamicWallpaperCaptureError_.empty())
            return L"DXGI Hook 捕获失败：" + dynamicWallpaperCaptureError_;
        return L"DXGI Hook 捕获失败";
    }
    if (dynamicWallpaperCaptureDeferred_)
    {
        if (dynamicWallpaperCaptureError_.find(L"重连") != std::wstring::npos)
            return dynamicWallpaperCaptureError_;
        return L"DXGI Hook 已连接，等待 Wallpaper Engine 下一帧";
    }
    if (!dynamicWallpaperEngine_.empty())
        return L"已通过 DXGI 共享纹理捕获 " + dynamicWallpaperEngine_;
    return L"已通过 DXGI 共享纹理捕获动态壁纸";
}

/**
 * @brief 按当前设置维护毛玻璃刷新状态。
 * @details 每帧 OnPaint 调用一次：
 *          - 玻璃整体关闭时释放快照/静态层/壁纸缓存并停止实时定时器；
 *          - 隐藏图标期间置脏，恢复显示时强制重评估；
 *          - 低频/中频档按时间戳做周期兜底失效（仅在发生重绘时生效）；
 *          - 实时档维护约 15fps 的重绘定时器（静态场景签名不变时重绘零成本，
 *            动态壁纸场景则逐帧重捕）。
 */
inline void DesktopApp::UpdateGlassRefreshState()
{
    const bool panelGlassActive = glassRequestedByPanels_;
    const int panelRefreshMode = glassRequestedRefreshMode_;
    glassRequestedByPanels_ = false;
    glassRequestedRefreshMode_ = 0;

    bool globalGlassActive = false;
    bool dockGlassActive = false;
    int globalRefreshMode = 0;
    if (settingsWindow_)
    {
        const auto& global = settingsWindow_->GetPersonalization();
        globalGlassActive = global.glassEnabled;
        dockGlassActive = settingsWindow_->GetDockAppearance().glassEnabled;
        globalRefreshMode = std::clamp(global.glassRefreshMode, 0, 3);
    }
    const bool glassActive = globalGlassActive || dockGlassActive || panelGlassActive;

    if (!glassActive)
    {
        if (glassBackdropBitmap_)
            glassBackdropBitmap_.Reset();
        glassBackdropRadiusCache_.clear();
        if (glassStaticLayerBitmap_)
            glassStaticLayerBitmap_.Reset();
        if (glassComposeBitmap_)
            glassComposeBitmap_.Reset();
        if (glassDynamicLayerBitmap_)
            glassDynamicLayerBitmap_.Reset();
        glassWallpaperCache_.clear();
        glassBackdropSignature_.clear();
        glassStaticSignature_.clear();
        glassBackdropDirty_ = true;
        glassWasDynamic_ = false;
        if (wallpaperEngineCapture_)
            wallpaperEngineCapture_->Stop();
        wallpaperEngineCapture_.reset();
        dynamicWallpaperWindows_.clear();
        dynamicWallpaperEngine_.clear();
        dynamicWallpaperCaptureError_.clear();
        dynamicWallpaperCaptureDeferred_ = false;
        dynamicWallpaperIncompatible_ = false;
        glassEffectiveRefreshMode_ = 0;
        glassLastCaptureAttemptSerial_ = std::numeric_limits<std::uint64_t>::max();
        glassLastDetectTick_ = 0;
        if (glassRefreshTimerActive_)
        {
            if (hwnd_ && IsWindow(hwnd_))
                KillTimer(hwnd_, kGlassRefreshTimerId);
            glassRefreshTimerActive_ = false;
        }
        return;
    }

    if (desktopIconsHidden_)
        glassBackdropDirty_ = true;

    int mode = panelGlassActive ? std::clamp(panelRefreshMode, 0, 3) : 0;
    if (globalGlassActive || dockGlassActive)
        mode = std::max(mode, globalRefreshMode);
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
        // 实时档：每次重绘都重新评估（静态场景签名不变零成本，动态场景逐帧重捕）
        glassBackdropDirty_ = true;
    }

    const bool wantTimer = mode == 3 && !desktopIconsHidden_ &&
        hwnd_ && IsWindow(hwnd_);
    if (wantTimer && !glassRefreshTimerActive_)
    {
        SetTimer(hwnd_, kGlassRefreshTimerId, kGlassRefreshRealtimeIntervalMs, nullptr);
        glassRefreshTimerActive_ = true;
    }
    else if (!wantTimer && glassRefreshTimerActive_)
    {
        if (hwnd_ && IsWindow(hwnd_))
            KillTimer(hwnd_, kGlassRefreshTimerId);
        glassRefreshTimerActive_ = false;
    }
}
