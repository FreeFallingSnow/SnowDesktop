#pragma once
#include "resource.h"
#include "utils.h"
#include "settings_window.h"
#include "widget_engine.h"

#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SnowDesktopApp : public IDropTarget, public IDropSource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        if (riid == IID_IDropSource)
        {
            *object = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG count = InterlockedDecrement(&refCount_);
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override
    {
        if (effect == nullptr) return E_POINTER;
        if (selfDragActive_)
        {
            DebugLog(L"SELFDRAG DragEnter");
            selfDragReturned_ = true;
            draggingItems_ = true;
            POINT client = ScreenPointToClient(point);
            dragCurrentPoint_ = client;
            dragTargetCell_ = FindBestDropCell(CellFromPoint(GetDragTargetPoint(client)));
            dragHint_ = MakeInternalDragHint(client);
            ShowDragHintWindow(client, dragHint_);
            *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            InvalidateRect(hwnd_, nullptr, TRUE);
            return S_OK;
        }

        externalDragActive_ = true;
        POINT oldPoint = externalDragPoint_;
        externalDragPoint_ = ScreenPointToClient(point);
        externalDragHint_ = MakeExternalDragHint(externalDragPoint_);
        ShowDragHintWindow(externalDragPoint_, externalDragHint_);
        *effect = ChooseDropEffect(keyState, *effect);
        InvalidateFast(UnionCopy(GetExternalDragDirtyRect(oldPoint), GetExternalDragDirtyRect(externalDragPoint_)));
        UNREFERENCED_PARAMETER(dataObject);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override
    {
        if (effect == nullptr) return E_POINTER;
        if (selfDragActive_)
        {
            POINT client = ScreenPointToClient(point);
            dragCurrentPoint_ = client;
            dragTargetCell_ = FindBestDropCell(CellFromPoint(GetDragTargetPoint(client)));
            dragHint_ = MakeInternalDragHint(client);
            ShowDragHintWindow(client, dragHint_);
            *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            InvalidateRect(hwnd_, nullptr, TRUE);
            return S_OK;
        }

        RECT oldDirty = GetExternalDragDirtyRect(externalDragPoint_);
        externalDragActive_ = true;
        externalDragPoint_ = ScreenPointToClient(point);
        externalDragHint_ = MakeExternalDragHint(externalDragPoint_);
        ShowDragHintWindow(externalDragPoint_, externalDragHint_);
        *effect = ChooseDropEffect(keyState, *effect);
        InvalidateFast(UnionCopy(oldDirty, GetExternalDragDirtyRect(externalDragPoint_)));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (selfDragActive_)
        {
            draggingItems_ = false;
            dragHint_.clear();
            HideDragHintWindow();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return S_OK;
        }
        RECT oldDirty = GetExternalDragDirtyRect(externalDragPoint_);
        externalDragActive_ = false;
        externalDragHint_.clear();
        HideDragHintWindow();
        InvalidateFast(oldDirty);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override
    {
        if (escapePressed)
        {
            return DRAGDROP_S_CANCEL;
        }
        if ((keyState & (MK_LBUTTON | MK_RBUTTON)) == 0)
        {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
    {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override
    {
        if (effect == nullptr)
        {
            return E_POINTER;
        }

        if (selfDragActive_)
        {
            DebugLog(L"SELFDROP hit, moving items");
            selfDragActive_ = false;
            selfDragReturned_ = true;
            draggingItems_ = false;
            HideDragHintWindow();
            dragHint_.clear();
            POINT clientPoint = ScreenPointToClient(point);
            DesktopHit dropHit = HitTestDesktop(clientPoint);
            bool handled = false;

            // Within same widget → reorder
            if (draggingCollectionMember_ && collectionDragSourceWidget_ < widgets_.size() &&
                dropHit.kind == DesktopHitKind::WidgetMember &&
                dropHit.widgetIndex == collectionDragSourceWidget_)
            {
                size_t insertIdx = GetWidgetMemberInsertIndex(collectionDragSourceWidget_, clientPoint);
                ReorderFileCategoryWidget(collectionDragSourceWidget_, insertIdx);
                handled = true;
            }
            // Handoff to another widget's member or desktop item
            else if (dropHit.kind == DesktopHitKind::WidgetMember &&
                dropHit.itemIndex < items_.size() && !items_[dropHit.itemIndex].selected &&
                IsPointInIconDropTarget(dropHit.bounds, clientPoint))
            {
                ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
                if (dataObj)
                {
                    DWORD localEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                    DropDataObjectToWidgetMember(dropHit, dataObj.Get(), clientPoint, MK_LBUTTON, &localEffect);
                }
                ReloadItems();
                handled = true;
            }
            // Handoff to desktop item
            else if (dropHit.kind == DesktopHitKind::Item && dropHit.itemIndex < items_.size() &&
                !items_[dropHit.itemIndex].selected &&
                IsPointInIconDropTarget(items_[dropHit.itemIndex].bounds, clientPoint))
            {
                ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
                if (dataObj)
                {
                    DWORD localEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                    DropDataObjectAt(dataObj.Get(), clientPoint, MK_LBUTTON, &localEffect);
                }
                ReloadItems();
                handled = true;
            }
            // Drop onto a different widget surface (or WidgetMember without icon-target handoff)
            if (!handled && dropHit.widgetIndex < widgets_.size() &&
                dropHit.widgetIndex != collectionDragSourceWidget_ &&
                (IsWidgetDropSurface(dropHit) || dropHit.kind == DesktopHitKind::WidgetMember ||
                 dropHit.kind == DesktopHitKind::WidgetContent))
            {
                const DesktopWidget& targetWidget = widgets_[dropHit.widgetIndex];
                if (targetWidget.type == DesktopWidgetType::Collection)
                {
                    size_t insertIndex = GetCollectionInsertIndex(dropHit.widgetIndex, clientPoint, false);
                    MoveSelectedItemsToCollection(dropHit.widgetIndex, insertIndex, collectionDragSourceWidget_);
                }
                else if (targetWidget.type == DesktopWidgetType::FileCategories)
                {
                    if (draggingCollectionMember_ && collectionDragSourceWidget_ < widgets_.size())
                        RemoveSelectedItemsFromCollections(collectionDragSourceWidget_);
                    AddSelectedItemsToFileCategoryWidget(dropHit.widgetIndex);
                }
                else if (targetWidget.type == DesktopWidgetType::FolderMapping)
                {
                    ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
                    if (dataObj)
                        DropExternalFilesToFolderMapping(dropHit.widgetIndex, dataObj.Get(), 0, nullptr);
                }
                LayoutItems();
                SaveLayoutSlots();
                handled = true;
            }

            // Default: place on desktop grid
            if (!handled)
            {
                if (draggingCollectionMember_ && collectionDragSourceWidget_ < widgets_.size())
                {
                    RemoveSelectedItemsFromCollections(collectionDragSourceWidget_);
                    SaveLayoutSlots();
                }
                MoveSelectedItemsToCell(FindBestDropCell(CellFromPoint(clientPoint)));
                LayoutItems();
            }
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        RECT oldDirty = GetExternalDragDirtyRect(externalDragPoint_);
        externalDragActive_ = false;
        externalDragHint_.clear();
        HideDragHintWindow();
        POINT clientPoint = ScreenPointToClient(point);
        DesktopHit memberHit = HitTestDesktop(clientPoint);
        if (memberHit.kind == DesktopHitKind::WidgetMember && dataObject != nullptr)
        {
            DWORD memberEffect = *effect;
            HRESULT hr = DropDataObjectToWidgetMember(memberHit, dataObject, clientPoint, keyState, &memberEffect);
            *effect = memberEffect;
            InvalidateFast(oldDirty);
            ReloadItems();
            if (memberHit.widgetIndex < widgets_.size() &&
                widgets_[memberHit.widgetIndex].type == DesktopWidgetType::FolderMapping)
            {
                RefreshFolderMappingWidget(memberHit.widgetIndex);
            }
            return hr;
        }

        size_t fmIndex = FindFolderMappingAtPoint(clientPoint);
        if (fmIndex < widgets_.size() && dataObject != nullptr)
        {
            DropExternalFilesToFolderMapping(fmIndex, dataObject, keyState, effect);
            InvalidateFast(oldDirty);
            RefreshFolderMappingWidget(fmIndex);
            return S_OK;
        }

        DWORD localEffect = *effect;
        HRESULT hr;
        if (!selfDragOutKeys_.empty() && dataObject != nullptr)
        {
            std::vector<std::wstring> paths = GetDropPaths(dataObject);
            if (!paths.empty())
            {
                std::unordered_set<std::wstring> pathSet(paths.begin(), paths.end());
                bool allCached = true;
                for (const auto& k : selfDragOutKeys_)
                {
                    size_t idx = FindItemIndexByKey(k);
                    if (idx == static_cast<size_t>(-1)) { allCached = false; break; }
                    std::wstring itemPath = items_[idx].parsingName;
                    if (!pathSet.contains(itemPath)) { allCached = false; break; }
                }
                if (allCached)
                {
                    DebugLog(L"EXTCACHE matched, internal move");
                    RemoveSelectedItemsFromCollections(static_cast<size_t>(-1));
                    MoveSelectedItemsToCell(FindBestDropCell(CellFromPoint(clientPoint)));
                    LayoutItems();
                    selfDragOutKeys_.clear();
                    *effect = DROPEFFECT_MOVE;
                    InvalidateFast(oldDirty);
                    return S_OK;
                }
            }
            selfDragOutKeys_.clear();
        }
        hr = DropDataObjectAt(dataObject, clientPoint, keyState, &localEffect);
        *effect = localEffect;
        InvalidateFast(oldDirty);
        ReloadItems();
        return hr;
    }

    int Run(HINSTANCE instance, int showCommand, UINT smokeTestMs = 0)
    {
        instance_ = instance;

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr))
        {
            MessageBoxW(nullptr, L"OleInitialize failed.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        DebugLog(L"=== SnowDesktop started ===");
        wchar_t startMessage[256]{};
        swprintf_s(startMessage, L"Process id=%lu showCommand=%d smokeTestMs=%u", GetCurrentProcessId(), showCommand, smokeTestMs);
        DebugLog(startMessage);

        if (!InitializeGraphics())
        {
            wchar_t message[256]{};
            swprintf_s(
                message,
                L"Unable to initialize Direct2D/DirectComposition.\nHRESULT=0x%08lX",
                static_cast<unsigned long>(lastGraphicsError_));
            MessageBoxW(nullptr, message, L"SnowDesktop", MB_ICONERROR);
            OleUninitialize();
            return 1;
        }

        const auto cleanup = std::unique_ptr<void, void (*)(void*)>(
            this,
            [](void* ctx)
            {
                auto* self = static_cast<SnowDesktopApp*>(ctx);
                self->UnregisterOleDropTarget();
                self->RestoreExplorerIcons();
                OleUninitialize();
            });

        if (!InitializeShell())
        {
            MessageBoxW(nullptr, L"Unable to initialize the Shell desktop folder.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }
        LoadLayoutSlots();

        desktopWindows_ = FindDesktopWindows();
        DebugLogDesktopWindows(L"Run FindDesktopWindows", desktopWindows_);
        if (desktopWindows_.listView != nullptr)
        {
            bool hiddenByPreviousRun = GetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp) != nullptr;
            wchar_t iconLayerMessage[256]{};
            swprintf_s(
                iconLayerMessage,
                L"Explorer icon layer before hide hiddenByPreviousRun=%s visible=%s",
                DebugBool(hiddenByPreviousRun),
                DebugBool(IsWindowVisible(desktopWindows_.listView) != FALSE));
            DebugLog(iconLayerMessage);
            if (hiddenByPreviousRun && !IsWindowVisible(desktopWindows_.listView))
            {
                ShowWindow(desktopWindows_.listView, SW_SHOW);
                DebugLog(L"Explorer icon layer restored before re-hide");
            }

            desktopWindows_.listViewWasVisible = IsWindowVisible(desktopWindows_.listView) != FALSE || hiddenByPreviousRun;
            if (desktopWindows_.listViewWasVisible)
            {
                SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1));
                ShowWindow(desktopWindows_.listView, SW_HIDE);
                DebugLog(L"Explorer icon layer hidden by SnowDesktop");
            }
        }
        else
        {
            DebugLog(L"Explorer icon layer not found during startup");
        }

        if (!RegisterWindowClass())
        {
            MessageBoxW(nullptr, L"Unable to register the SnowDesktop window class.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        taskbarRestartMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
        wchar_t taskbarMessage[128]{};
        swprintf_s(taskbarMessage, L"RegisterWindowMessage(TaskbarCreated)=0x%X", taskbarRestartMsg_);
        DebugLog(taskbarMessage);

        if (!CreateControlWindow())
        {
            MessageBoxW(nullptr, L"Unable to create the SnowDesktop control window.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        if (!CreateDesktopWindow(showCommand))
        {
            wchar_t message[256]{};
            if (FAILED(lastGraphicsError_))
            {
                swprintf_s(
                    message,
                    L"Unable to create the SnowDesktop composition target.\nHRESULT=0x%08lX",
                    static_cast<unsigned long>(lastGraphicsError_));
            }
            else
            {
                swprintf_s(
                    message,
                    L"Unable to create the SnowDesktop desktop window.\nGetLastError=%lu",
                    lastCreateWindowError_);
            }
            MessageBoxW(nullptr, message, L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        if (smokeTestMs > 0)
        {
            SetTimer(hwnd_, 1, smokeTestMs, nullptr);
        }

        RegisterOleDropTarget();
        AddTrayIcon();
        ReloadItems();
        RegisterShellChangeNotifications();

        SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
        DebugLog(L"Timers started: recycle bin poll and desktop host watch");

        // Init settings window
        settingsWindow_ = std::make_unique<SettingsWindow>();
        if (!settingsWindow_->Init(instance, d3dDevice_.Get()))
        {
            DebugLog(L"SettingsWindow Init failed");
        }
        else
        {
            settingsWindow_->SetReloadCallback([this]() { ReloadItems(); });
            settingsWindow_->SetExitCallback([this]() { DoExit(); });
            settingsWindow_->SetInvalidateCallback([this]() {
                if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            });
        }

        // Init Lua widget engine
        widgetEngine_ = std::make_unique<WidgetEngine>();
        if (!widgetEngine_->Init(bitmapContext_.Get(), dwriteFactory_.Get()))
        {
            DebugLog(L"WidgetEngine Init failed");
        }

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (settingsWindow_ && settingsWindow_->IsVisible())
                settingsWindow_->Render();
        }

        return static_cast<int>(msg.wParam);
    }

private:
    enum class DesktopHitKind
    {
        None,
        Item,
        Widget,
        WidgetMember,
        WidgetCategory,
        WidgetAllButton,
        WidgetFolderToggle,
        WidgetFolderOpen,
        WidgetContent,
        PopupMember,
    };

    struct DesktopHit
    {
        DesktopHitKind kind = DesktopHitKind::None;
        size_t itemIndex = static_cast<size_t>(-1);
        size_t widgetIndex = static_cast<size_t>(-1);
        size_t memberIndex = static_cast<size_t>(-1);
        RECT bounds{};
    };

    enum WidgetResizeEdge
    {
        kResizeNone = 0,
        kResizeLeft = 1,
        kResizeRight = 2,
        kResizeTop = 4,
        kResizeBottom = 8,
    };

    static constexpr int kCollectionPopupPaddingX = 18;
    static constexpr int kCollectionPopupHeaderHeight = 54;
    static constexpr int kCollectionPopupBottomPadding = 18;

    bool InitializeGraphics()
    {
        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
        };

        D3D_FEATURE_LEVEL actualFeatureLevel{};
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &actualFeatureLevel,
            nullptr);

        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &d3dDevice_,
                &actualFeatureLevel,
                nullptr);
        }
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = d3dDevice_.As(&dxgiDevice);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        D2D1_FACTORY_OPTIONS factoryOptions{};
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &factoryOptions,
            reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &bitmapContext_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        bitmapContext_->SetDpi(96.0f, 96.0f);
        bitmapContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

        hr = DCompositionCreateDevice2(
            d2dDevice_.Get(),
            __uuidof(IDCompositionDesktopDevice),
            reinterpret_cast<void**>(dcompDevice_.GetAddressOf()));
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            15.0f,
            L"",
            &itemTextFormat_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        itemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        itemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        itemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        itemTextFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f, 13.0f);

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"",
            &collectionItemTextFormat_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        collectionItemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        collectionItemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        collectionItemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.0f,
            L"",
            &listItemTextFormat_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        listItemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        listItemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        listItemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.0f,
            L"",
            &statusTextFormat_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        statusTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        statusTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        statusTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        // Font Awesome initialization
        faFontHandle_ = LoadFontAwesome();
        if (faFontHandle_ != nullptr)
        {
            faTextFormat_ = ComPtr<IDWriteTextFormat>(
                CreateFaTextFormat(dwriteFactory_.Get(), 14.0f));

            HDC screenDc = GetDC(nullptr);
            int menuFontHeight = -MulDiv(
                GetSystemMetrics(SM_CXMENUCHECK) * 3 / 8,
                GetDeviceCaps(screenDc, LOGPIXELSY),
                72);
            ReleaseDC(nullptr, screenDc);
            faMenuFont_ = CreateFontW(
                menuFontHeight,
                0, 0, 0,
                FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Font Awesome 6 Free Solid");
        }

        UpdateTextFormats();

        D2D1_STROKE_STYLE_PROPERTIES dottedProps{};
        dottedProps.startCap = D2D1_CAP_STYLE_FLAT;
        dottedProps.endCap = D2D1_CAP_STYLE_FLAT;
        dottedProps.dashCap = D2D1_CAP_STYLE_FLAT;
        dottedProps.lineJoin = D2D1_LINE_JOIN_MITER;
        dottedProps.miterLimit = 10.0f;
        dottedProps.dashStyle = D2D1_DASH_STYLE_DOT;
        dottedProps.dashOffset = 0.0f;
        d2dFactory_->CreateStrokeStyle(dottedProps, nullptr, 0, &dottedStrokeStyle_);

        lastGraphicsError_ = S_OK;
        return true;
    }

    void UpdateTextFormats()
    {
        const float s = gapScale_;

        HRESULT hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f * s, L"", &itemTextFormat_);
        if (SUCCEEDED(hr) && itemTextFormat_)
        {
            itemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            itemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            itemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            itemTextFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f * s, 13.0f * s);
        }

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f * s, L"", &collectionItemTextFormat_);
        if (SUCCEEDED(hr) && collectionItemTextFormat_)
        {
            collectionItemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            collectionItemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            collectionItemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f * s, L"", &listItemTextFormat_);
        if (SUCCEEDED(hr) && listItemTextFormat_)
        {
            listItemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            listItemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            listItemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14.0f * s, L"", &statusTextFormat_);
        if (SUCCEEDED(hr) && statusTextFormat_)
        {
            statusTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            statusTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            statusTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        faTextFormat_.Reset();
        if (faFontHandle_ != nullptr)
        {
            faTextFormat_ = ComPtr<IDWriteTextFormat>(
                CreateFaTextFormat(dwriteFactory_.Get(), 14.0f * s));
        }
    }

    bool InitializeShell()
    {
        if (FAILED(SHGetDesktopFolder(&desktopFolder_)))
        {
            return false;
        }

        PIDLIST_ABSOLUTE desktopPidl = nullptr;
        if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desktopPidl)))
        {
            return false;
        }

        desktopPidl_.reset(desktopPidl);

        SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&sysImageList_));

        return true;
    }

    bool RegisterWindowClass()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SnowDesktopApp::WindowProcThunk;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClassName;
        wc.style = CS_DBLCLKS;

        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW control{};
        control.cbSize = sizeof(control);
        control.lpfnWndProc = &SnowDesktopApp::ControlWindowProcThunk;
        control.hInstance = instance_;
        control.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        control.hbrBackground = nullptr;
        control.lpszClassName = kControlWindowClassName;

        if (RegisterClassExW(&control) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW hint{};
        hint.cbSize = sizeof(hint);
        hint.lpfnWndProc = DefWindowProcW;
        hint.hInstance = instance_;
        hint.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hint.hbrBackground = nullptr;
        hint.lpszClassName = kHintWindowClassName;

        return RegisterClassExW(&hint) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool CreateDesktopWindow(int showCommand)
    {
        UNREFERENCED_PARAMETER(showCommand);

        virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        UpdateLayoutWorkArea();

        HWND parent = desktopWindows_.host != nullptr ? desktopWindows_.host : GetDesktopWindow();
        POINT origin{ virtualLeft_, virtualTop_ };
        ScreenToClient(parent, &origin);

        wchar_t createMessage[512]{};
        swprintf_s(
            createMessage,
            L"CreateDesktopWindow virtual=(%d,%d,%d,%d) parent=%p origin=(%ld,%ld)",
            virtualLeft_,
            virtualTop_,
            virtualWidth_,
            virtualHeight_,
            parent,
            origin.x,
            origin.y);
        DebugLog(createMessage);
        DebugLogDesktopWindows(L"CreateDesktopWindow desktopWindows_", desktopWindows_);

        DWORD exStyle = WS_EX_TOOLWINDOW;
        hwnd_ = CreateWindowExW(
            exStyle,
            kWindowClassName,
            L"SnowDesktop",
            WS_CHILD | WS_VISIBLE,
            origin.x,
            origin.y,
            virtualWidth_,
            virtualHeight_,
            parent,
            nullptr,
            instance_,
            this);

        if (hwnd_ == nullptr)
        {
            lastCreateWindowError_ = GetLastError();
            wchar_t createError[256]{};
            swprintf_s(createError, L"CreateWindowExW child failed GetLastError=%lu", lastCreateWindowError_);
            DebugLog(createError);

            hwnd_ = CreateWindowExW(
                exStyle,
                kWindowClassName,
                L"SnowDesktop",
                WS_POPUP | WS_VISIBLE,
                virtualLeft_,
                virtualTop_,
                virtualWidth_,
                virtualHeight_,
                nullptr,
                nullptr,
                instance_,
                this);

            if (hwnd_ == nullptr)
            {
                lastCreateWindowError_ = GetLastError();
                swprintf_s(createError, L"CreateWindowExW popup fallback failed GetLastError=%lu", lastCreateWindowError_);
                DebugLog(createError);
                return false;
            }
            DebugLogWindow(L"CreateWindowExW popup fallback created", hwnd_);

            if (parent != nullptr && parent != GetDesktopWindow())
            {
                SetLastError(ERROR_SUCCESS);
                SetParent(hwnd_, parent);
                DWORD setParentError = GetLastError();
                swprintf_s(createError, L"CreateDesktopWindow fallback SetParent parent=%p GetLastError=%lu", parent, setParentError);
                DebugLog(createError);
                if (setParentError == ERROR_SUCCESS)
                {
                    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
                    style &= ~WS_POPUP;
                    style |= WS_CHILD | WS_VISIBLE;
                    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
                    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                }
            }
        }
        else
        {
            DebugLogWindow(L"CreateWindowExW child created", hwnd_);
        }

        RestoreDesktopWindowLayer();
        DebugLogWindow(L"CreateDesktopWindow after RestoreDesktopWindowLayer", hwnd_);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);

        HICON appIcon = LoadAppIcon();
        if (appIcon != nullptr)
        {
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
        }
        return CreateCompositionTarget();
    }

    bool CreateControlWindow()
    {
        if (controlHwnd_ != nullptr && IsWindow(controlHwnd_))
        {
            return true;
        }

        controlHwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kControlWindowClassName,
            L"SnowDesktopControl",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            instance_,
            this);
        if (controlHwnd_ == nullptr)
        {
            wchar_t message[256]{};
            swprintf_s(message, L"CreateControlWindow failed GetLastError=%lu", GetLastError());
            DebugLog(message);
            return false;
        }

        DebugLogWindow(L"CreateControlWindow created", controlHwnd_);
        return true;
    }

    HWND TrayOwnerWindow() const
    {
        if (controlHwnd_ != nullptr && IsWindow(controlHwnd_))
        {
            return controlHwnd_;
        }
        return hwnd_;
    }

    void ResetDesktopWindowResources()
    {
        dcompTarget_.Reset();
        dcompVisual_.Reset();
        dcompSurface_.Reset();
        compositionWidth_ = 0;
        compositionHeight_ = 0;
        dropTargetRegistered_ = false;
        hwnd_ = nullptr;
    }

    bool IsDesktopWindowChild() const
    {
        return hwnd_ != nullptr && (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CHILD) != 0;
    }

    void RestoreDesktopWindowLayer()
    {
        if (hwnd_ == nullptr || !IsWindow(hwnd_))
        {
            return;
        }

        if (IsDesktopWindowChild())
        {
            HWND parent = GetParent(hwnd_);
            POINT origin{ virtualLeft_, virtualTop_ };
            if (parent != nullptr)
            {
                ScreenToClient(parent, &origin);
            }
            BOOL ok = SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
            if (!ok)
            {
                wchar_t message[256]{};
                swprintf_s(message, L"RestoreDesktopWindowLayer child SetWindowPos failed GetLastError=%lu parent=%p", GetLastError(), parent);
                DebugLog(message);
            }
            return;
        }

        BOOL ok = SetWindowPos(hwnd_, HWND_BOTTOM, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
        if (!ok)
        {
            wchar_t message[256]{};
            swprintf_s(message, L"RestoreDesktopWindowLayer popup SetWindowPos failed GetLastError=%lu", GetLastError());
            DebugLog(message);
        }
    }

    void AttachWindowToDesktopHost(HWND host)
    {
        if (hwnd_ == nullptr || !IsWindow(hwnd_) || host == nullptr || !IsWindow(host))
        {
            wchar_t message[256]{};
            swprintf_s(
                message,
                L"AttachWindowToDesktopHost skipped hwnd=%p hwndValid=%s host=%p hostValid=%s",
                hwnd_,
                DebugBool(hwnd_ != nullptr && IsWindow(hwnd_)),
                host,
                DebugBool(host != nullptr && IsWindow(host)));
            DebugLog(message);
            return;
        }

        DebugLogWindow(L"AttachWindowToDesktopHost before hwnd", hwnd_);
        DebugLogWindow(L"AttachWindowToDesktopHost host", host);

        if (GetParent(hwnd_) != host)
        {
            SetLastError(ERROR_SUCCESS);
            SetParent(hwnd_, host);
            DWORD error = GetLastError();
            wchar_t message[256]{};
            swprintf_s(message, L"AttachWindowToDesktopHost SetParent host=%p GetLastError=%lu", host, error);
            DebugLog(message);
            if (error != ERROR_SUCCESS)
            {
                return;
            }
        }
        else
        {
            DebugLog(L"AttachWindowToDesktopHost parent already matches current host");
        }

        LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
        style &= ~WS_POPUP;
        style |= WS_CHILD | WS_VISIBLE;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
        RestoreDesktopWindowLayer();
        ShowWindow(hwnd_, customDesktopVisible_ ? SW_SHOWNOACTIVATE : SW_HIDE);
        DebugLogWindow(L"AttachWindowToDesktopHost after hwnd", hwnd_);
    }

    void RecoverDesktopHostAfterExplorerRestart()
    {
        DebugLog(L"RecoverDesktopHostAfterExplorerRestart begin");
        DesktopWindows windows = FindDesktopWindows();
        DebugLogDesktopWindows(L"RecoverDesktopHostAfterExplorerRestart FindDesktopWindows", windows);
        if (windows.host == nullptr || !IsWindow(windows.host))
        {
            DebugLog(L"RecoverDesktopHostAfterExplorerRestart no valid host");
            return;
        }

        desktopWindows_ = windows;
        const bool desktopWindowValid = hwnd_ != nullptr && IsWindow(hwnd_);
        if (desktopWindowValid)
        {
            AttachWindowToDesktopHost(desktopWindows_.host);
        }
        else
        {
            DebugLog(L"RecoverDesktopHostAfterExplorerRestart recreating desktop window");
            ResetDesktopWindowResources();
            if (!CreateDesktopWindow(SW_SHOWNOACTIVATE))
            {
                wchar_t message[256]{};
                swprintf_s(
                    message,
                    L"RecoverDesktopHostAfterExplorerRestart CreateDesktopWindow failed createError=%lu graphicsHr=0x%08lX",
                    lastCreateWindowError_,
                    static_cast<unsigned long>(lastGraphicsError_));
                DebugLog(message);
                return;
            }
            RegisterOleDropTarget();
            RegisterShellChangeNotifications();
            SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);
        }

        if (customDesktopVisible_)
        {
            HideExplorerIcons();
            ReloadItems();
        }

        AddTrayIcon(true);
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
        DebugLogWindow(L"RecoverDesktopHostAfterExplorerRestart end hwnd", hwnd_);
    }

    void WatchDesktopHost()
    {
        if (hwnd_ == nullptr || !IsWindow(hwnd_))
        {
            DebugLog(L"WatchDesktopHost hwnd invalid - recovering");
            RecoverDesktopHostAfterExplorerRestart();
            return;
        }

        HWND parent = GetParent(hwnd_);
        DesktopWindows windows = FindDesktopWindows();
        HWND currentHost = windows.host;
        const bool parentMissing = parent == nullptr || !IsWindow(parent);
        const bool knownHostMissing = desktopWindows_.host == nullptr || !IsWindow(desktopWindows_.host);
        const bool knownListViewMissing = customDesktopVisible_ &&
            (desktopWindows_.listView == nullptr || !IsWindow(desktopWindows_.listView));
        const bool hostChanged = currentHost != nullptr && IsWindow(currentHost) && currentHost != desktopWindows_.host;
        const bool parentDetached = currentHost != nullptr && IsWindow(currentHost) && parent != currentHost;

        if (parentMissing || knownHostMissing || knownListViewMissing || hostChanged || parentDetached)
        {
            wchar_t message[512]{};
            swprintf_s(
                message,
                L"WatchDesktopHost check parent=%p currentHost=%p cachedHost=%p parentMissing=%s knownHostMissing=%s knownListViewMissing=%s hostChanged=%s parentDetached=%s",
                parent,
                currentHost,
                desktopWindows_.host,
                DebugBool(parentMissing),
                DebugBool(knownHostMissing),
                DebugBool(knownListViewMissing),
                DebugBool(hostChanged),
                DebugBool(parentDetached));
            DebugLog(message);
            DebugLogDesktopWindows(L"WatchDesktopHost current desktop windows", windows);
            DebugLogWindow(L"WatchDesktopHost hwnd before recovery", hwnd_);
        }

        if (parentMissing || knownHostMissing || knownListViewMissing || hostChanged || parentDetached)
        {
            DebugLog(L"Desktop host watch - recovering");
            desktopWindows_ = windows;
            AttachWindowToDesktopHost(desktopWindows_.host);
            if (customDesktopVisible_)
            {
                HideExplorerIcons();
                ReloadItems();
            }
            AddTrayIcon(true);
            DebugLogWindow(L"WatchDesktopHost hwnd after recovery", hwnd_);
        }
    }

    void RequestExit()
    {
        if (settingsWindow_)
        {
            settingsWindow_->ShowExitConfirm();
            return;
        }
        DoExit();
    }

    void DoExit()
    {

        DebugLogWindow(L"RequestExit hwnd", hwnd_);
        exitRequested_ = true;
        if (hwnd_ != nullptr && IsWindow(hwnd_))
        {
            DestroyWindow(hwnd_);
        }
        else
        {
            KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
            RemoveTrayIcon();
            RestoreExplorerIcons();
            if (controlHwnd_ != nullptr && IsWindow(controlHwnd_))
            {
                DestroyWindow(controlHwnd_);
                controlHwnd_ = nullptr;
            }
            PostQuitMessage(0);
        }
    }

    void RelaunchSelfAfterExplorerRestart()
    {
        if (restartLaunched_)
        {
            DebugLog(L"RelaunchSelfAfterExplorerRestart skipped: already launched");
            return;
        }

        wchar_t selfPath[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0)
        {
            DebugLog(L"RelaunchSelfAfterExplorerRestart GetModuleFileNameW failed");
            return;
        }

        HINSTANCE result = ShellExecuteW(
            nullptr,
            L"open",
            selfPath,
            L"--wait-for-desktop-host",
            nullptr,
            SW_SHOWNORMAL);
        wchar_t message[512]{};
        swprintf_s(
            message,
            L"RelaunchSelfAfterExplorerRestart ShellExecute result=%p selfPath=%s",
            result,
            selfPath);
        DebugLog(message);
        if (reinterpret_cast<INT_PTR>(result) > 32)
        {
            restartLaunched_ = true;
        }
    }

    bool CreateCompositionTarget()
    {
        if (!dcompDevice_)
        {
            return false;
        }

        HRESULT hr = dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dcompDevice_->CreateVisual(&dcompVisual_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = CreateOrResizeCompositionSurface();
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dcompTarget_->SetRoot(dcompVisual_.Get());
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dcompDevice_->Commit();
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        lastGraphicsError_ = S_OK;
        return true;
    }

    HRESULT CreateOrResizeCompositionSurface()
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));

        if (dcompSurface_ && compositionWidth_ == width && compositionHeight_ == height)
        {
            return S_OK;
        }

        ComPtr<IDCompositionSurface> surface;
        HRESULT hr = dcompDevice_->CreateSurface(
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED,
            &surface);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = dcompVisual_->SetContent(surface.Get());
        if (FAILED(hr))
        {
            return hr;
        }

        dcompSurface_ = surface;
        compositionWidth_ = width;
        compositionHeight_ = height;
        return S_OK;
    }

    void HideExplorerIcons()
    {
        if (desktopWindows_.listView != nullptr && IsWindow(desktopWindows_.listView))
        {
            DebugLogWindow(L"HideExplorerIcons listView before", desktopWindows_.listView);
            desktopWindows_.listViewWasVisible = true;
            SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1));
            ShowWindow(desktopWindows_.listView, SW_HIDE);
            DebugLogWindow(L"HideExplorerIcons listView after", desktopWindows_.listView);
        }
        else
        {
            DebugLog(L"HideExplorerIcons skipped: listView invalid");
        }
    }

    void RegisterShellChangeNotifications()
    {
        if (hwnd_ == nullptr || !IsWindow(hwnd_))
        {
            DebugLog(L"RegisterShellChangeNotifications skipped: hwnd invalid");
            return;
        }

        if (shellChangeRegId_ != 0)
        {
            SHChangeNotifyDeregister(shellChangeRegId_);
            shellChangeRegId_ = 0;
        }

        SHChangeNotifyEntry entries[2]{};
        entries[0].pidl = desktopPidl_.get();
        entries[0].fRecursive = FALSE;
        if (recycleBinPidl_.get() == nullptr)
        {
            PIDLIST_ABSOLUTE rbPidl = nullptr;
            if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &rbPidl)))
            {
                recycleBinPidl_.reset(rbPidl);
            }
        }
        if (recycleBinPidl_.get() != nullptr)
        {
            entries[1].pidl = recycleBinPidl_.get();
            entries[1].fRecursive = TRUE;
        }

        const int entryCount = recycleBinPidl_.get() != nullptr ? 2 : 1;
        shellChangeRegId_ = SHChangeNotifyRegister(
            hwnd_,
            SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
            SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
            SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
            SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_ASSOCCHANGED,
            kShellChangeMessage,
            entryCount,
            entries);

        wchar_t message[256]{};
        swprintf_s(message, L"RegisterShellChangeNotifications hwnd=%p regId=%lu", hwnd_, shellChangeRegId_);
        DebugLog(message);
    }

    void RestoreExplorerIcons()
    {
        if (desktopWindows_.listView != nullptr && IsWindow(desktopWindows_.listView) && desktopWindows_.listViewWasVisible)
        {
            DebugLogWindow(L"RestoreExplorerIcons listView before", desktopWindows_.listView);
            ShowWindow(desktopWindows_.listView, SW_SHOW);
            RemovePropW(desktopWindows_.listView, kHiddenBySnowDesktopProp);
            DebugLogWindow(L"RestoreExplorerIcons listView after", desktopWindows_.listView);
        }
        else
        {
            wchar_t message[256]{};
            swprintf_s(
                message,
                L"RestoreExplorerIcons skipped listView=%p valid=%s listViewWasVisible=%s",
                desktopWindows_.listView,
                DebugBool(desktopWindows_.listView != nullptr && IsWindow(desktopWindows_.listView)),
                DebugBool(desktopWindows_.listViewWasVisible));
            DebugLog(message);
        }
    }

    void SwitchToNativeDesktop()
    {
        SaveLayoutSlots();
        HideDragHintWindow();
        RestoreExplorerIcons();
        if (hwnd_ != nullptr)
        {
            ShowWindow(hwnd_, SW_HIDE);
        }
        customDesktopVisible_ = false;
    }

    void SwitchToCustomDesktop()
    {
        HideExplorerIcons();
        if (hwnd_ != nullptr)
        {
            ShowWindow(hwnd_, SW_SHOW);
        }
        customDesktopVisible_ = true;
        ReloadItems();
    }

    bool EnsureDragHintWindow()
    {
        if (hintHwnd_ != nullptr)
        {
            return true;
        }

        hintHwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            kHintWindowClassName,
            L"",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            instance_,
            nullptr);
        return hintHwnd_ != nullptr;
    }

    void HideDragHintWindow()
    {
        if (hintHwnd_ != nullptr)
        {
            ShowWindow(hintHwnd_, SW_HIDE);
        }
    }

    void DestroyDragHintWindow()
    {
        if (hintHwnd_ != nullptr)
        {
            DestroyWindow(hintHwnd_);
            hintHwnd_ = nullptr;
        }
    }

    void ShowDragHintWindow(POINT clientPoint, const std::wstring& text)
    {
        if (text.empty() || !EnsureDragHintWindow())
        {
            HideDragHintWindow();
            return;
        }

        POINT screenPoint = clientPoint;
        ClientToScreen(hwnd_, &screenPoint);

        HDC screenDc = GetDC(nullptr);
        HFONT font = CreateFontW(
            -15,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        HGDIOBJ oldScreenFont = SelectObject(screenDc, font);
        SIZE textSize{};
        GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
        SelectObject(screenDc, oldScreenFont);

        int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
        int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
        POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

        HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo))
        {
            windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8, monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
            windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8, monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr)
        {
            DeleteObject(font);
            ReleaseDC(nullptr, screenDc);
            HideDragHintWindow();
            return;
        }

        auto* pixels = static_cast<std::uint32_t*>(bits);
        auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
            return (static_cast<std::uint32_t>(a) << 24) |
                (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b);
        };

        const std::uint32_t background = argb(255, 255, 255, 255);
        const std::uint32_t border = argb(255, 205, 211, 220);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                bool isBorder = x == 0 || y == 0 || x == width - 1 || y == height - 1;
                pixels[(y * width) + x] = isBorder ? border : background;
            }
        }

        HDC memoryDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        HGDIOBJ oldFont = SelectObject(memoryDc, font);
        SetBkMode(memoryDc, TRANSPARENT);
        SetTextColor(memoryDc, RGB(25, 32, 42));

        RECT textRect{ 10, 0, width - 10, height };
        DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        for (int i = 0; i < width * height; ++i)
        {
            std::uint32_t pixel = pixels[i];
            std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
            std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
            std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
            pixels[i] = argb(255, r, g, b);
        }

        POINT sourcePoint{ 0, 0 };
        SIZE windowSize{ width, height };
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
        ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

        SelectObject(memoryDc, oldFont);
        SelectObject(memoryDc, oldBitmap);
        DeleteDC(memoryDc);
        DeleteObject(bitmap);
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
    }

    void AddTrayIcon(bool force = false)
    {
        HWND owner = TrayOwnerWindow();
        if (owner == nullptr || !IsWindow(owner))
        {
            DebugLog(L"AddTrayIcon skipped: owner invalid");
            return;
        }
        if (trayIconAdded_ && !force)
        {
            DebugLog(L"AddTrayIcon skipped: already added");
            return;
        }

        if (force && trayIconAdded_)
        {
            NOTIFYICONDATAW removeData{};
            removeData.cbSize = sizeof(removeData);
            removeData.hWnd = trayIconOwnerHwnd_ != nullptr ? trayIconOwnerHwnd_ : owner;
            removeData.uID = kTrayIconId;
            BOOL removeOk = Shell_NotifyIconW(NIM_DELETE, &removeData);
            wchar_t removeMessage[256]{};
            swprintf_s(removeMessage, L"AddTrayIcon force delete ok=%s GetLastError=%lu", DebugBool(removeOk != FALSE), GetLastError());
            DebugLog(removeMessage);
            trayIconAdded_ = false;
            trayIconOwnerHwnd_ = nullptr;
        }

        if (trayIcon_ == nullptr)
        {
            trayIcon_ = static_cast<HICON>(LoadImageW(
                GetModuleHandleW(nullptr),
                MAKEINTRESOURCEW(IDI_APPICON_SMALL),
                IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON),
                LR_DEFAULTSIZE));
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = trayIcon_;
        wcscpy_s(data.szTip, L"SnowDesktop 桌面验证");

        BOOL addOk = Shell_NotifyIconW(NIM_ADD, &data);
        trayIconAdded_ = addOk != FALSE;
        wchar_t addMessage[256]{};
        swprintf_s(
            addMessage,
            L"AddTrayIcon NIM_ADD force=%s ok=%s hwnd=%p GetLastError=%lu",
            DebugBool(force),
            DebugBool(addOk != FALSE),
            owner,
            GetLastError());
        DebugLog(addMessage);
        if (trayIconAdded_)
        {
            trayIconOwnerHwnd_ = owner;
            data.uVersion = NOTIFYICON_VERSION_4;
            BOOL versionOk = Shell_NotifyIconW(NIM_SETVERSION, &data);
            wchar_t versionMessage[256]{};
            swprintf_s(versionMessage, L"AddTrayIcon NIM_SETVERSION ok=%s GetLastError=%lu", DebugBool(versionOk != FALSE), GetLastError());
            DebugLog(versionMessage);
        }
    }

    void RegisterOleDropTarget()
    {
        if (hwnd_ != nullptr && IsWindow(hwnd_) && !dropTargetRegistered_)
        {
            dropTargetRegistered_ = SUCCEEDED(RegisterDragDrop(hwnd_, static_cast<IDropTarget*>(this)));
            wchar_t message[256]{};
            swprintf_s(message, L"RegisterOleDropTarget hwnd=%p success=%s", hwnd_, DebugBool(dropTargetRegistered_));
            DebugLog(message);
        }
    }

    void UnregisterOleDropTarget()
    {
        if (hwnd_ != nullptr && IsWindow(hwnd_) && dropTargetRegistered_)
        {
            RevokeDragDrop(hwnd_);
            dropTargetRegistered_ = false;
        }
        else
        {
            dropTargetRegistered_ = false;
        }
    }

    void RemoveTrayIcon()
    {
        HWND owner = trayIconOwnerHwnd_ != nullptr ? trayIconOwnerHwnd_ : TrayOwnerWindow();
        if (trayIconAdded_ && owner != nullptr)
        {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = owner;
            data.uID = kTrayIconId;
            Shell_NotifyIconW(NIM_DELETE, &data);
            trayIconAdded_ = false;
            trayIconOwnerHwnd_ = nullptr;
        }

        if (trayIcon_ != nullptr)
        {
            DestroyIcon(trayIcon_);
            trayIcon_ = nullptr;
        }
    }

    bool IsClsidCurrentlyVisible(const std::wstring& clsid)
    {
        auto it = settingsIconVisibility_.find(clsid);
        if (it != settingsIconVisibility_.end())
        {
            return it->second;
        }

        DWORD value = 0;
        if (TryReadDesktopIconRegistryValueAnyRoot(clsid, value))
        {
            return value == 0;
        }

        static const std::unordered_map<std::wstring, bool> defaultVisibility = {
            { kDesktopIconClsidThisPC, false },
            { kDesktopIconClsidUserFiles, false },
            { kDesktopIconClsidNetwork, false },
            { kDesktopIconClsidControlPanel, false },
            { kDesktopIconClsidRecycleBin, true },
        };

        auto found = defaultVisibility.find(clsid);
        if (found != defaultVisibility.end())
        {
            return found->second;
        }

        return false;
    }

    void ToggleDesktopIconVisibility(UINT command)
    {
        static const std::unordered_map<UINT, std::wstring> commandToClsid = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin },
        };

        auto it = commandToClsid.find(command);
        if (it == commandToClsid.end())
        {
            return;
        }

        bool currentVisible = IsClsidCurrentlyVisible(it->second);
        bool newVisible = !currentVisible;
        if (newVisible)
            settingsIconVisibility_[it->second] = true;
        else
            settingsIconVisibility_.erase(it->second);
        WriteDesktopIconRegistryValue(it->second, newVisible);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSH, nullptr, nullptr);
        ReloadItems();
    }

    HBITMAP CreateMenuIconBitmap(const wchar_t* text)
    {
        const int cx = GetSystemMetrics(SM_CXMENUCHECK);
        const int cy = GetSystemMetrics(SM_CYMENUCHECK);
        if (cx <= 0 || cy <= 0) return nullptr;

        HDC screenDc = GetDC(nullptr);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = cx;
        bmi.bmiHeader.biHeight = -cy;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp == nullptr)
        {
            ReleaseDC(nullptr, screenDc);
            return nullptr;
        }

        // Fill transparent
        std::fill_n(static_cast<std::uint32_t*>(bits), cx * cy, 0u);

        HDC memDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldBmp = SelectObject(memDc, bmp);
        HGDIOBJ defaultFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(memDc, faMenuFont_ != nullptr ? faMenuFont_ : defaultFont);

        const int pad = std::max(1, cx / 10);
        RECT rc = {pad, pad, cx - pad, cy - pad};
        SetBkMode(memDc, TRANSPARENT);
        // Draw in white first, then convert to black with alpha
        SetTextColor(memDc, RGB(255, 255, 255));
        DrawTextW(memDc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDc, oldFont);
        SelectObject(memDc, oldBmp);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);

        // Convert white pixels to opaque black
        auto* pixels = static_cast<std::uint32_t*>(bits);
        const size_t count = static_cast<size_t>(cx) * static_cast<size_t>(cy);
        for (size_t i = 0; i < count; ++i)
        {
            std::uint32_t p = pixels[i];
            if ((p & 0x00FFFFFF) != 0)
            {
                // Use the luminance from the white glyph as alpha
                std::uint8_t lum = static_cast<std::uint8_t>(
                    std::max({(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF}));
                pixels[i] = (static_cast<std::uint32_t>(lum) << 24);
            }
        }

        return bmp;
    }

    void SetMenuItemIcon(HMENU menu, UINT_PTR command, const wchar_t* text)
    {
        HBITMAP icon = CreateMenuIconBitmap(text);
        if (icon == nullptr) return;

        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = icon;
        SetMenuItemInfoW(menu, command, FALSE, &mii);
        menuIconPool_.push_back(icon);
    }

    void ShowTrayMenu(POINT screenPoint)
    {
        wchar_t trayMessage[256]{};
        swprintf_s(trayMessage, L"ShowTrayMenu begin point=(%ld,%ld)", screenPoint.x, screenPoint.y);
        DebugLog(trayMessage);
        DebugLogWindow(L"ShowTrayMenu hwnd before", hwnd_);
        HWND owner = TrayOwnerWindow();
        DebugLogWindow(L"ShowTrayMenu owner before", owner);

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            DebugLog(L"ShowTrayMenu CreatePopupMenu failed");
            return;
        }

        HMENU iconSettingsMenu = CreatePopupMenu();
        if (iconSettingsMenu != nullptr)
        {
            struct IconSetting
            {
                UINT command;
                const wchar_t* clsid;
                const wchar_t* label;
            };
            const IconSetting settings[] = {
                { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC, L"计算机" },
                { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles, L"用户的文件" },
                { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork, L"网络" },
                { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel, L"控制面板" },
                { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin, L"回收站" },
            };

            for (const auto& setting : settings)
            {
                UINT flags = MF_STRING;
                if (IsClsidCurrentlyVisible(setting.clsid))
                {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(iconSettingsMenu, flags, setting.command, setting.label);
            }

            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconSettingsMenu), L"桌面图标设置");
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, customDesktopVisible_ ? MF_STRING : (MF_STRING | MF_GRAYED), kTraySwitchNativeCommand, L"切换原生桌面");
        AppendMenuW(menu, customDesktopVisible_ ? (MF_STRING | MF_GRAYED) : MF_STRING, kTraySwitchCustomCommand, L"切换自定义桌面");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTraySettingsCommand, L"设置");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出软件");

        menuIconPool_.clear();
        SetMenuItemIcon(menu, kTrayExitCommand, L"");

        SetForegroundWindow(owner);
        DebugLogWindow(L"ShowTrayMenu after SetForegroundWindow owner", owner);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            owner,
            nullptr);

        swprintf_s(trayMessage, L"ShowTrayMenu TrackPopupMenuEx command=%u GetLastError=%lu", command, GetLastError());
        DebugLog(trayMessage);
        RestoreDesktopWindowLayer();
        DebugLogWindow(L"ShowTrayMenu after RestoreDesktopWindowLayer", hwnd_);

        switch (command)
        {
        case kTrayReloadCommand:
            ReloadItems();
            break;
        case kTraySortByNameCommand:
            SortIconsByName();
            break;
        case kTraySortByTypeCommand:
            SortIconsByType();
            break;
        case kTraySwitchNativeCommand:
            SwitchToNativeDesktop();
            break;
        case kTraySwitchCustomCommand:
            SwitchToCustomDesktop();
            break;
        case kTrayExitCommand:
            RequestExit();
            break;
        case kTraySettingsCommand:
            if (settingsWindow_)
                settingsWindow_->Show();
            break;
        case kTrayDesktopIconThisPC:
        case kTrayDesktopIconUserFiles:
        case kTrayDesktopIconNetwork:
        case kTrayDesktopIconControlPanel:
        case kTrayDesktopIconRecycleBin:
            ToggleDesktopIconVisibility(command);
            break;
        default:
            break;
        }

        if (iconSettingsMenu != nullptr)
        {
            DestroyMenu(iconSettingsMenu);
        }
        DestroyMenu(menu);
        for (HBITMAP bmp : menuIconPool_) { DeleteObject(bmp); }
        menuIconPool_.clear();
        RestoreDesktopWindowLayer();
        DebugLogWindow(L"ShowTrayMenu end", hwnd_);
    }

    void ReloadItems(bool reloadLayoutFromDisk = true)
    {
        if (reloading_)
        {
            return;
        }
        reloading_ = true;

        if (reloadLayoutFromDisk)
        {
            LoadLayoutSlots();
        }

        items_.clear();
        d2dIconCache_.clear();
        selectedCount_ = 0;

        ComPtr<IEnumIDList> enumItems;
        if (FAILED(desktopFolder_->EnumObjects(
                hwnd_,
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
                &enumItems)) ||
            !enumItems)
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
            reloading_ = false;
            return;
        }

        wchar_t userDesktopPath[MAX_PATH]{};
        wchar_t commonDesktopPath[MAX_PATH]{};
        wchar_t userProfilePath[MAX_PATH]{};
        SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
        SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
        SHGetSpecialFolderPathW(nullptr, userProfilePath, CSIDL_PROFILE, FALSE);
        size_t userDesktopLen = wcslen(userDesktopPath);
        size_t commonDesktopLen = wcslen(commonDesktopPath);

        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        std::unordered_set<std::wstring> seenKeys;
        while (enumItems->Next(1, &child, &fetched) == S_OK)
        {
            PIDLIST_ABSOLUTE absolute = ILCombine(desktopPidl_.get(), child);
            if (absolute == nullptr)
            {
                ILFree(child);
                continue;
            }

            std::wstring parsingName = StrRetToString(
                desktopFolder_.Get(),
                reinterpret_cast<PCUITEMID_CHILD>(child),
                SHGDN_FORPARSING);
            wchar_t itemPathBuffer[MAX_PATH]{};
            std::wstring itemPath;
            if (SHGetPathFromIDListW(absolute, itemPathBuffer) && itemPathBuffer[0] != L'\0')
            {
                itemPath = itemPathBuffer;
            }

            std::wstring desktopIconClsid = ResolveDesktopIconClsid(parsingName, itemPath, userProfilePath);
            bool isDesktopIcon = !desktopIconClsid.empty();

            if (!isDesktopIcon)
            {
                SFGAOF attrs = SFGAO_HIDDEN | SFGAO_NONENUMERATED;
                LPCITEMIDLIST childConst = child;
                if (SUCCEEDED(desktopFolder_->GetAttributesOf(1, &childConst, &attrs)))
                {
                    if ((attrs & SFGAO_HIDDEN) != 0 || (attrs & SFGAO_NONENUMERATED) != 0)
                    {
                        ILFree(absolute);
                        ILFree(child);
                        continue;
                    }
                }
            }

            SHFILEINFOW info{};
            SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(absolute),
                0,
                &info,
                sizeof(info),
                SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME | SHGFI_TYPENAME);

            if (!IsVisibleByDesktopIconSettings(desktopIconClsid, settingsIconVisibility_))
            {
                ILFree(absolute);
                ILFree(child);
                continue;
            }

            if (!isDesktopIcon)
            {
                if (!itemPath.empty())
                {
                    bool underUser = itemPath.size() > userDesktopLen &&
                        _wcsnicmp(itemPath.c_str(), userDesktopPath, userDesktopLen) == 0 &&
                        itemPath[userDesktopLen] == L'\\';
                    bool underCommon = itemPath.size() > commonDesktopLen &&
                        _wcsnicmp(itemPath.c_str(), commonDesktopPath, commonDesktopLen) == 0 &&
                        itemPath[commonDesktopLen] == L'\\';
                    if (!underUser && !underCommon)
                    {
                        ILFree(absolute);
                        ILFree(child);
                        continue;
                    }
                }
            }

            DesktopItem item;
            item.absolutePidl.reset(absolute);
            item.childPidl.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(child));
            item.parsingName = std::move(parsingName);
            item.desktopIconClsid = std::move(desktopIconClsid);
            item.name = info.szDisplayName[0] != L'\0'
                ? info.szDisplayName
                : StrRetToString(desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()), SHGDN_NORMAL);
            item.typeName = info.szTypeName;
            item.iconBitmap = GetHighResolutionShellIconBitmap(item.absolutePidl.get(), info.iIcon, item.iconBitmapSize);
            ClampAlphaToColorKey(item.iconBitmap, kTransparentKey);
            item.sysIconIndex = info.iIcon;
            item.shortcutArrow = false;
            {
                std::wstring upper = item.parsingName;
                for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
                if (upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".LNK") == 0)
                {
                    wchar_t lnkPath[MAX_PATH]{};
                    if (SHGetPathFromIDListW(item.absolutePidl.get(), lnkPath))
                    {
                        ComPtr<IShellLinkW> shellLink;
                        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                            IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                        {
                            ComPtr<IPersistFile> persistFile;
                            if (SUCCEEDED(shellLink.As(&persistFile)) &&
                                SUCCEEDED(persistFile->Load(lnkPath, STGM_READ)))
                            {
                                wchar_t target[MAX_PATH]{};
                                if (SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, 0)))
                                {
                                    std::wstring t(target);
                                    for (auto& c : t) c = static_cast<wchar_t>(towupper(c));
                                    if (t.size() < 4 || t.compare(t.size() - 4, 4, L".EXE") != 0)
                                        item.shortcutArrow = true;
                                }
                            }
                        }
                    }
                }
            }
            if (item.shortcutArrow && item.iconBitmap != nullptr)
            {
                ApplyShortcutArrowToBitmap(item.iconBitmap, item.iconBitmapSize);
            }
            std::wstring key = GetStableLayoutKey(item.absolutePidl.get(), item.parsingName, item.desktopIconClsid);
            item.layoutKey = key;
            if (seenKeys.contains(key))
            {
                continue;
            }
            seenKeys.insert(key);
            auto knownRecord = layoutRecords_.find(key);
            if (knownRecord != layoutRecords_.end() && knownRecord->second.hasGrid)
            {
                item.gridCell = knownRecord->second.cell;
                item.gridSpan = knownRecord->second.span;
                item.slot = SlotFromCell(item.gridCell);
            }
            else if (knownRecord != layoutRecords_.end() && knownRecord->second.legacySlot >= 0)
            {
                item.gridCell = CellFromSlot(knownRecord->second.legacySlot);
                item.gridSpan = knownRecord->second.span;
                item.slot = SlotFromCell(item.gridCell);
            }
            else
            {
                item.slot = -1;
            }
            if (!gridPages_.empty() && item.gridCell.pageId.empty())
            {
                item.gridCell.pageId = gridPages_.front().id;
            }
            items_.push_back(std::move(item));
        }

        std::stable_sort(items_.begin(), items_.end(), [](const DesktopItem& a, const DesktopItem& b) {
            bool aIsDesktopIcon = !a.desktopIconClsid.empty();
            bool bIsDesktopIcon = !b.desktopIconClsid.empty();
            if (aIsDesktopIcon != bIsDesktopIcon) return aIsDesktopIcon;
            if (aIsDesktopIcon)
            {
                int cmp = ToUpperInvariant(a.typeName).compare(ToUpperInvariant(b.typeName));
                if (cmp != 0) return cmp < 0;
            }
            return ToUpperInvariant(a.name) < ToUpperInvariant(b.name);
        });

        CleanWidgetItemKeys();
        ApplyAutoCollectFileCategoryWidgets();

        for (auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::FolderMapping && !widget.sourceFolderPath.empty())
            {
                EnumerateFolderMappingEntries(widget);
            }
        }

        std::unordered_set<std::wstring> usedSlots;
        NormalizeWidgetsInGrid(usedSlots);

        int nextTailSlot = 0;
        for (auto& item : items_)
        {
            if (!IsTopLevelItem(item))
            {
                item.slot = -1;
                item.bounds = {};
                continue;
            }
            item.gridSpan.columns = std::max(1, item.gridSpan.columns);
            item.gridSpan.rows = std::max(1, item.gridSpan.rows);
            if (item.slot < 0)
            {
                continue;
            }
            item.slot = SlotFromCell(item.gridCell);
            if (!item.gridCell.pageId.empty() && !HasGridPage(item.gridCell.pageId))
            {
                continue;
            }
            if (item.slot >= 0 && IsGridAreaValid(item.gridCell, item.gridSpan) && !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
            {
                MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                nextTailSlot = std::max(nextTailSlot, item.slot + 1);
            }
            else
            {
                item.slot = -1;
            }
        }

        std::vector<DesktopItem*> unslotted;
        for (auto& item : items_)
        {
            if (IsTopLevelItem(item) && item.slot < 0)
            {
                unslotted.push_back(&item);
            }
        }

        std::sort(unslotted.begin(), unslotted.end(), [](const DesktopItem* a, const DesktopItem* b) {
            bool aIsDesktopIcon = !a->desktopIconClsid.empty();
            bool bIsDesktopIcon = !b->desktopIconClsid.empty();
            if (aIsDesktopIcon != bIsDesktopIcon)
            {
                return aIsDesktopIcon;
            }
            if (aIsDesktopIcon)
            {
                int cmp = ToUpperInvariant(a->typeName).compare(ToUpperInvariant(b->typeName));
                if (cmp != 0)
                {
                    return cmp < 0;
                }
            }
            return ToUpperInvariant(a->name) < ToUpperInvariant(b->name);
        });

        for (auto* item : unslotted)
        {
            GridCell nextCell = CellFromSequentialIndex(nextTailSlot);
            while (AreGridSlotsMarked(usedSlots, nextCell, item->gridSpan) || !IsGridAreaValid(nextCell, item->gridSpan))
            {
                ++nextTailSlot;
                nextCell = CellFromSequentialIndex(nextTailSlot);
            }
            item->gridCell = nextCell;
            item->slot = SlotFromCell(item->gridCell);
            MarkGridArea(usedSlots, item->gridCell, item->gridSpan);
            nextTailSlot = item->slot + 1;
        }

        std::sort(items_.begin(), items_.end(), [](const DesktopItem& a, const DesktopItem& b) {
            if (a.gridCell.pageId != b.gridCell.pageId)
            {
                return a.gridCell.pageId < b.gridCell.pageId;
            }
            if (a.gridCell.column != b.gridCell.column)
            {
                return a.gridCell.column < b.gridCell.column;
            }
            return a.gridCell.row < b.gridCell.row;
        });

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        reloading_ = false;
    }

    std::wstring GetLayoutPath() const
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        PathRemoveFileSpecW(modulePath);
        PathAppendW(modulePath, L"SnowDesktop.layout.json");
        return modulePath;
    }

    std::wstring NormalizeLayoutKey(const std::wstring& key) const
    {
        return ToUpperInvariant(key);
    }

    std::wstring GetStableLayoutKey(
        PCIDLIST_ABSOLUTE pidl,
        const std::wstring& parsingName,
        const std::wstring& desktopIconClsid = {}) const
    {
        if (!desktopIconClsid.empty())
        {
            return ToUpperInvariant(desktopIconClsid);
        }

        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, path) && path[0] != L'\0')
        {
            return ToUpperInvariant(path);
        }
        return ToUpperInvariant(parsingName);
    }

    bool ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const
    {
        std::string marker = std::string("\"") + fieldName + "\"";
        size_t name = objectText.find(marker);
        if (name == std::string::npos)
        {
            return false;
        }

        size_t colon = objectText.find(':', name + marker.size());
        size_t quote = objectText.find('"', colon == std::string::npos ? name + marker.size() : colon + 1);
        size_t end = 0;
        return quote != std::string::npos && ParseJsonStringAt(objectText, quote, value, end);
    }

    bool ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const
    {
        std::string marker = std::string("\"") + fieldName + "\"";
        size_t name = objectText.find(marker);
        if (name == std::string::npos)
        {
            return false;
        }

        size_t colon = objectText.find(':', name + marker.size());
        size_t numberStart = objectText.find_first_of("-0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
        if (numberStart == std::string::npos)
        {
            return false;
        }

        value = std::atoi(objectText.c_str() + numberStart);
        return true;
    }

    bool ReadJsonBoolField(const std::string& objectText, const char* fieldName, bool& value) const
    {
        std::string marker = std::string("\"") + fieldName + "\"";
        size_t name = objectText.find(marker);
        if (name == std::string::npos)
        {
            return false;
        }

        size_t colon = objectText.find(':', name + marker.size());
        size_t valueStart = objectText.find_first_not_of(" \t\r\n", colon == std::string::npos ? name + marker.size() : colon + 1);
        if (valueStart == std::string::npos)
        {
            return false;
        }

        if (objectText.compare(valueStart, 4, "true") == 0)
        {
            value = true;
            return true;
        }
        if (objectText.compare(valueStart, 5, "false") == 0)
        {
            value = false;
            return true;
        }
        return false;
    }

    size_t FindJsonContainerEnd(const std::string& text, size_t start, char open, char close) const
    {
        if (start >= text.size() || text[start] != open)
        {
            return std::string::npos;
        }

        int depth = 1;
        bool inString = false;
        for (size_t i = start + 1; i < text.size(); ++i)
        {
            char ch = text[i];
            if (ch == '"' && (i == 0 || text[i - 1] != '\\'))
            {
                inString = !inString;
            }
            else if (!inString)
            {
                if (ch == open)
                {
                    ++depth;
                }
                else if (ch == close)
                {
                    --depth;
                    if (depth == 0)
                    {
                        return i;
                    }
                }
            }
        }
        return std::string::npos;
    }

    size_t FindJsonObjectEnd(const std::string& text, size_t start) const
    {
        return FindJsonContainerEnd(text, start, '{', '}');
    }

    size_t FindJsonArrayEnd(const std::string& text, size_t start) const
    {
        return FindJsonContainerEnd(text, start, '[', ']');
    }

    bool ReadJsonStringArrayField(const std::string& objectText, const char* fieldName, std::vector<std::wstring>& values) const
    {
        values.clear();
        std::string marker = std::string("\"") + fieldName + "\"";
        size_t name = objectText.find(marker);
        if (name == std::string::npos)
        {
            return false;
        }

        size_t colon = objectText.find(':', name + marker.size());
        size_t arrayStart = objectText.find('[', colon == std::string::npos ? name + marker.size() : colon + 1);
        if (arrayStart == std::string::npos)
        {
            return false;
        }

        size_t arrayEnd = FindJsonArrayEnd(objectText, arrayStart);
        if (arrayEnd == std::string::npos)
        {
            return false;
        }

        size_t pos = arrayStart + 1;
        while (pos < arrayEnd)
        {
            size_t quote = objectText.find('"', pos);
            if (quote == std::string::npos || quote >= arrayEnd)
            {
                break;
            }

            std::string utf8;
            size_t end = 0;
            if (!ParseJsonStringAt(objectText, quote, utf8, end))
            {
                break;
            }
            values.push_back(Utf8ToWide(utf8));
            pos = end;
        }
        return true;
    }

    std::wstring WidgetTypeToJson(DesktopWidgetType type) const
    {
        switch (type)
        {
        case DesktopWidgetType::FileCategories:
            return L"fileCategories";
        case DesktopWidgetType::FolderMapping:
            return L"folderMapping";
        case DesktopWidgetType::LuaScript:
            return L"lua";
        case DesktopWidgetType::Collection:
        default:
            return L"collection";
        }
    }

    DesktopWidgetType WidgetTypeFromJson(const std::wstring& type) const
    {
        std::wstring normalized = ToUpperInvariant(type);
        if (normalized == L"FILECATEGORIES" || normalized == L"FILE_CATEGORIES")
        {
            return DesktopWidgetType::FileCategories;
        }
        if (normalized == L"FOLDERMAPPING" || normalized == L"FOLDER_MAPPING")
        {
            return DesktopWidgetType::FolderMapping;
        }
        if (normalized == L"LUA" || normalized == L"LUASCRIPT" || normalized == L"LUA_SCRIPT")
        {
            return DesktopWidgetType::LuaScript;
        }
        if (normalized == L"COLLECTION")
        {
            return DesktopWidgetType::Collection;
        }
        return DesktopWidgetType::Collection;
    }

    void LoadLayoutSlots()
    {
        layoutRecords_.clear();
        widgets_.clear();
        savedPageIds_.clear();

        std::ifstream file(GetLayoutPath(), std::ios::binary);
        if (!file)
        {
            ApplyPageMapping();
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string text = buffer.str();

        std::string firstPageMonitorUtf8;
        if (ReadJsonStringField(text, "firstPageMonitor", firstPageMonitorUtf8))
        {
            firstPageMonitorId_ = Utf8ToWide(firstPageMonitorUtf8);
        }

        LoadSavedPagesFromJson(text);

        size_t pos = 0;
        while ((pos = text.find("\"key\"", pos)) != std::string::npos)
        {
            size_t objectStart = text.rfind('{', pos);
            if (objectStart == std::string::npos)
            {
                break;
            }
            size_t objectEnd = FindJsonObjectEnd(text, objectStart);
            if (objectEnd == std::string::npos || objectEnd <= objectStart)
            {
                break;
            }

            std::string objectText = text.substr(objectStart, objectEnd - objectStart + 1);
            std::string keyUtf8;
            if (!ReadJsonStringField(objectText, "key", keyUtf8))
            {
                pos = objectEnd + 1;
                continue;
            }

            LayoutRecord record;
            std::string pageUtf8;
            int x = 0;
            int y = 0;
            int w = 1;
            int h = 1;
            if (ReadJsonStringField(objectText, "page", pageUtf8) &&
                ReadJsonIntField(objectText, "x", x) &&
                ReadJsonIntField(objectText, "y", y))
            {
                record.cell.pageId = Utf8ToWide(pageUtf8);
                record.cell.column = x;
                record.cell.row = y;
                RememberSavedPageId(record.cell.pageId);
                ReadJsonIntField(objectText, "w", w);
                ReadJsonIntField(objectText, "h", h);
                record.span.columns = std::max(1, w);
                record.span.rows = std::max(1, h);
                record.hasGrid = true;
                record.legacySlot = SlotFromCell(record.cell);
            }
            else
            {
                int slot = -1;
                if (!ReadJsonIntField(objectText, "slot", slot))
                {
                    pos = objectEnd + 1;
                    continue;
                }
                record.legacySlot = slot;
                record.cell = CellFromSlot(slot);
                record.span = {};
            }

            layoutRecords_[NormalizeLayoutKey(Utf8ToWide(keyUtf8))] = record;
            pos = objectEnd + 1;
        }

        LoadWidgetsFromJson(text);

        ApplyPageMapping();
        ApplySavedGridDimensions();
    }

    void LoadWidgetsFromJson(const std::string& text)
    {
        size_t widgetsName = text.find("\"widgets\"");
        if (widgetsName == std::string::npos)
        {
            return;
        }

        size_t arrayStart = text.find('[', widgetsName);
        if (arrayStart == std::string::npos)
        {
            return;
        }

        size_t arrayEnd = FindJsonArrayEnd(text, arrayStart);
        if (arrayEnd == std::string::npos || arrayEnd <= arrayStart)
        {
            return;
        }

        size_t pos = arrayStart + 1;
        while ((pos = text.find('{', pos)) != std::string::npos && pos < arrayEnd)
        {
            size_t objectEnd = FindJsonObjectEnd(text, pos);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd)
            {
                break;
            }

            std::string objectText = text.substr(pos, objectEnd - pos + 1);
            std::string idUtf8;
            std::string typeUtf8;
            std::string titleUtf8;
            std::string sourceUtf8;
            std::string activeCategoryUtf8;
            std::string pageUtf8;
            int x = 0;
            int y = 0;
            int w = 1;
            int h = 1;
            int scrollOffset = 0;
            bool autoCollect = false;
            bool listMode = false;
            if (!ReadJsonStringField(objectText, "id", idUtf8) ||
                !ReadJsonStringField(objectText, "page", pageUtf8) ||
                !ReadJsonIntField(objectText, "x", x) ||
                !ReadJsonIntField(objectText, "y", y))
            {
                pos = objectEnd + 1;
                continue;
            }

            ReadJsonStringField(objectText, "type", typeUtf8);
            ReadJsonStringField(objectText, "title", titleUtf8);
            ReadJsonStringField(objectText, "sourceFolderPath", sourceUtf8);
            std::string scriptUtf8;
            ReadJsonStringField(objectText, "scriptPath", scriptUtf8);
            ReadJsonStringField(objectText, "activeCategory", activeCategoryUtf8);
            ReadJsonIntField(objectText, "w", w);
            ReadJsonIntField(objectText, "h", h);
            ReadJsonIntField(objectText, "scrollOffset", scrollOffset);
            ReadJsonBoolField(objectText, "autoCollect", autoCollect);
            ReadJsonBoolField(objectText, "listMode", listMode);

            DesktopWidget widget;
            widget.id = Utf8ToWide(idUtf8);
            widget.type = WidgetTypeFromJson(Utf8ToWide(typeUtf8));
            widget.title = titleUtf8.empty()
                ? (widget.type == DesktopWidgetType::FileCategories ? L"桌面文件"
                   : widget.type == DesktopWidgetType::FolderMapping ? L"文件夹映射"
                   : L"集合")
                : Utf8ToWide(titleUtf8);
            widget.sourceFolderPath = Utf8ToWide(sourceUtf8);
            widget.scriptPath = Utf8ToWide(scriptUtf8);
            widget.gridCell.pageId = Utf8ToWide(pageUtf8);
            widget.gridCell.column = x;
            widget.gridCell.row = y;
            widget.gridSpan.columns = std::max(1, w);
            widget.gridSpan.rows = std::max(1, h);
            widget.autoCollect = autoCollect;
            widget.listMode = listMode;
            widget.scrollOffset = std::max(0, scrollOffset);
            widget.activeCategoryId = Utf8ToWide(activeCategoryUtf8);
            ReadJsonStringArrayField(objectText, "items", widget.itemKeys);
            // Deduplicate normalized keys
            {
                std::unordered_set<std::wstring> seen;
                std::vector<std::wstring> unique;
                for (auto& key : widget.itemKeys)
                {
                    key = NormalizeLayoutKey(key);
                    if (seen.insert(key).second)
                        unique.push_back(key);
                }
                widget.itemKeys = std::move(unique);
            }
            RememberSavedPageId(widget.gridCell.pageId);
            widgets_.push_back(std::move(widget));
            if (widgets_.back().type == DesktopWidgetType::FolderMapping && !widgets_.back().sourceFolderPath.empty())
            {
                EnumerateFolderMappingEntries(widgets_.back());
            }
            pos = objectEnd + 1;
        }
    }

    bool IsItemKeyInAnyCollection(const std::wstring& key) const
    {
        if (key.empty())
        {
            return false;
        }

        std::wstring normalized = NormalizeLayoutKey(key);
        for (const auto& widget : widgets_)
        {
            if (std::find(widget.itemKeys.begin(), widget.itemKeys.end(), normalized) != widget.itemKeys.end())
            {
                return true;
            }
        }
        return false;
    }

    bool IsTopLevelItem(const DesktopItem& item) const
    {
        return !IsItemKeyInAnyCollection(item.layoutKey);
    }

    size_t FindItemIndexByKey(const std::wstring& key) const
    {
        std::wstring normalized = NormalizeLayoutKey(key);
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (NormalizeLayoutKey(items_[i].layoutKey) == normalized)
            {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }

    void SaveLayoutSlots()
    {
        layoutRecords_.clear();
        for (const auto& item : items_)
        {
            if (!item.parsingName.empty() && IsTopLevelItem(item))
            {
                RememberSavedPageId(item.gridCell.pageId);

                LayoutRecord record;
                record.cell = item.gridCell;
                record.span = item.gridSpan;
                record.hasGrid = true;
                record.legacySlot = item.slot;
                layoutRecords_[item.layoutKey] = record;
            }
        }

        std::vector<const DesktopItem*> sortedItems;
        sortedItems.reserve(items_.size());
        for (const auto& item : items_)
        {
            if (IsTopLevelItem(item))
            {
                sortedItems.push_back(&item);
            }
        }
        std::sort(sortedItems.begin(), sortedItems.end(), [](const DesktopItem* left, const DesktopItem* right) {
            if (left->gridCell.pageId != right->gridCell.pageId)
            {
                return left->gridCell.pageId < right->gridCell.pageId;
            }
            if (left->gridCell.column != right->gridCell.column)
            {
                return left->gridCell.column < right->gridCell.column;
            }
            return left->gridCell.row < right->gridCell.row;
        });

        std::ofstream file(GetLayoutPath(), std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return;
        }

        std::vector<std::wstring> pagesToWrite = savedPageIds_;
        if (pagesToWrite.empty() && !gridPages_.empty())
        {
            pagesToWrite.push_back(gridPages_.front().id);
        }

        // Persist current live page dimensions before writing
        for (const auto& page : gridPages_)
        {
            savedPageColumns_[page.id] = page.columns;
            savedPageRows_[page.id] = page.rows;
        }

        file << "{\n  \"firstPageMonitor\": \"" << JsonEscapeUtf8(firstPageMonitorId_) << "\",\n  \"pages\": [\n";
        for (size_t i = 0; i < pagesToWrite.size(); ++i)
        {
            const GridPage* page = FindExactGridPage(pagesToWrite[i]);
            file << "    { \"id\": \"" << JsonEscapeUtf8(pagesToWrite[i]) << "\", \"monitor\": \"";
            file << JsonEscapeUtf8(page != nullptr ? page->monitorId : L"");
            int columns = page != nullptr ? page->columns : 0;
            int rows = page != nullptr ? page->rows : 0;
            if (page == nullptr)
            {
                auto colIt = savedPageColumns_.find(pagesToWrite[i]);
                auto rowIt = savedPageRows_.find(pagesToWrite[i]);
                if (colIt != savedPageColumns_.end()) columns = colIt->second;
                if (rowIt != savedPageRows_.end()) rows = rowIt->second;
            }
            file << "\", \"columns\": " << std::max(2, columns) <<
                ", \"rows\": " << std::max(2, rows) << " }";
            file << (i + 1 == pagesToWrite.size() ? "\n" : ",\n");
        }
        file << "  ],\n  \"items\": [\n";
        for (size_t i = 0; i < sortedItems.size(); ++i)
        {
            const DesktopItem* item = sortedItems[i];
            file << "    { \"key\": \"" << JsonEscapeUtf8(item->layoutKey) <<
                "\", \"page\": \"" << JsonEscapeUtf8(item->gridCell.pageId) <<
                "\", \"x\": " << item->gridCell.column <<
                ", \"y\": " << item->gridCell.row <<
                ", \"w\": " << std::max(1, item->gridSpan.columns) <<
                ", \"h\": " << std::max(1, item->gridSpan.rows) <<
                ", \"slot\": " << item->slot << " }";
            file << (i + 1 == sortedItems.size() ? "\n" : ",\n");
        }
        file << "  ],\n  \"widgets\": [\n";
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            const DesktopWidget& widget = widgets_[i];
            file << "    { \"id\": \"" << JsonEscapeUtf8(widget.id) <<
                "\", \"type\": \"" << JsonEscapeUtf8(WidgetTypeToJson(widget.type)) <<
                "\", \"title\": \"" << JsonEscapeUtf8(widget.title) <<
                "\", \"sourceFolderPath\": \"" << JsonEscapeUtf8(widget.sourceFolderPath) <<
                "\", \"scriptPath\": \"" << JsonEscapeUtf8(widget.scriptPath) <<
                "\", \"activeCategory\": \"" << JsonEscapeUtf8(widget.activeCategoryId) <<
                "\", \"page\": \"" << JsonEscapeUtf8(widget.gridCell.pageId) <<
                "\", \"x\": " << widget.gridCell.column <<
                ", \"y\": " << widget.gridCell.row <<
                ", \"w\": " << std::max(1, widget.gridSpan.columns) <<
                ", \"h\": " << std::max(1, widget.gridSpan.rows) <<
                ", \"autoCollect\": " << (widget.autoCollect ? "true" : "false") <<
                ", \"listMode\": " << (widget.listMode ? "true" : "false") <<
                ", \"scrollOffset\": " << std::max(0, widget.scrollOffset) <<
                ", \"items\": [";
            for (size_t j = 0; j < widget.itemKeys.size(); ++j)
            {
                file << "\"" << JsonEscapeUtf8(widget.itemKeys[j]) << "\"";
                if (j + 1 != widget.itemKeys.size())
                {
                    file << ", ";
                }
            }
            file << "] }";
            file << (i + 1 == widgets_.size() ? "\n" : ",\n");
        }
        file << "  ]\n}\n";
    }

    void PlaceSortedTopLevelItems(const std::vector<size_t>& order)
    {
        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
        {
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }

        for (size_t itemIndex : order)
        {
            if (itemIndex >= items_.size() || !IsTopLevelItem(items_[itemIndex]))
            {
                continue;
            }

            GridCell freeCell;
            if (TryFindFreeCell(items_[itemIndex].gridSpan, usedSlots, freeCell))
            {
                items_[itemIndex].gridCell = freeCell;
                items_[itemIndex].slot = SlotFromCell(freeCell);
                MarkGridArea(usedSlots, freeCell, items_[itemIndex].gridSpan);
            }
        }
    }

    void SortIconsByName()
    {
        std::vector<size_t> order;
        order.reserve(items_.size());
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (IsTopLevelItem(items_[i]))
            {
                order.push_back(i);
            }
        }

        std::sort(order.begin(), order.end(), [this](size_t left, size_t right) {
            std::wstring leftName = ToUpperInvariant(items_[left].name);
            std::wstring rightName = ToUpperInvariant(items_[right].name);
            return leftName < rightName;
        });

        PlaceSortedTopLevelItems(order);

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void SortIconsByType()
    {
        std::vector<size_t> order;
        order.reserve(items_.size());
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (IsTopLevelItem(items_[i]))
            {
                order.push_back(i);
            }
        }

        std::sort(order.begin(), order.end(), [this](size_t left, size_t right) {
            bool leftIsDesktopIcon = !items_[left].desktopIconClsid.empty();
            bool rightIsDesktopIcon = !items_[right].desktopIconClsid.empty();
            if (leftIsDesktopIcon != rightIsDesktopIcon)
            {
                return leftIsDesktopIcon;
            }
            int cmp = ToUpperInvariant(items_[left].typeName).compare(ToUpperInvariant(items_[right].typeName));
            if (cmp != 0)
            {
                return cmp < 0;
            }
            return ToUpperInvariant(items_[left].name) < ToUpperInvariant(items_[right].name);
        });

        PlaceSortedTopLevelItems(order);

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void LayoutItems()
    {
        for (auto& item : items_)
        {
            if (!IsTopLevelItem(item))
            {
                item.bounds = {};
                item.slot = -1;
                continue;
            }
            if (!gridPages_.empty() && item.gridCell.pageId.empty())
            {
                item.gridCell.pageId = gridPages_.front().id;
            }
            const GridPage* page = FindExactGridPage(item.gridCell.pageId);
            if (page != nullptr)
            {
                item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
                item.gridSpan.rows = std::clamp(item.gridSpan.rows, 1, std::max(1, page->rows));
                item.gridCell.column = std::clamp(item.gridCell.column, 0, std::max(0, page->columns - item.gridSpan.columns));
                item.gridCell.row = std::clamp(item.gridCell.row, 0, std::max(0, page->rows - item.gridSpan.rows));
            }
            item.slot = SlotFromCell(item.gridCell);
            item.bounds = GetGridRect(item.gridCell, item.gridSpan);
        }
        LayoutWidgets();
    }

    void LayoutWidgets()
    {
        for (auto& widget : widgets_)
        {
            if (!gridPages_.empty() && widget.gridCell.pageId.empty())
            {
                widget.gridCell.pageId = gridPages_.front().id;
            }
            const GridPage* page = FindExactGridPage(widget.gridCell.pageId);
            if (page != nullptr)
            {
                widget.gridSpan.columns = std::clamp(widget.gridSpan.columns, 1, std::max(1, page->columns));
                widget.gridSpan.rows = std::clamp(widget.gridSpan.rows, 1, std::max(1, page->rows));
                widget.gridCell.column = std::clamp(widget.gridCell.column, 0, std::max(0, page->columns - widget.gridSpan.columns));
                widget.gridCell.row = std::clamp(widget.gridCell.row, 0, std::max(0, page->rows - widget.gridSpan.rows));
            }
            widget.bounds = GetGridRect(widget.gridCell, widget.gridSpan);
        }
    }

    void UpdateLayoutWorkArea()
    {
        layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);
        gridPages_.clear();

        MonitorEnumContext context{};
        context.virtualLeft = virtualLeft_;
        context.virtualTop = virtualTop_;
        context.pages = &gridPages_;
        EnumDisplayMonitors(nullptr, nullptr, EnumGridPageMonitorProc, reinterpret_cast<LPARAM>(&context));

        if (gridPages_.empty())
        {
            GridPage fallback;
            fallback.id = L"Primary";
            fallback.monitorId = fallback.id;
            fallback.isPrimary = true;
            fallback.bounds = layoutWorkArea_;
            fallback.workArea = layoutWorkArea_;
            gridPages_.push_back(fallback);
        }

        std::sort(gridPages_.begin(), gridPages_.end(), [](const GridPage& left, const GridPage& right) {
            if (left.bounds.left != right.bounds.left)
            {
                return left.bounds.left < right.bounds.left;
            }
            return left.bounds.top < right.bounds.top;
        });

        for (auto& page : gridPages_)
        {
            page.workArea.left = std::clamp<LONG>(page.workArea.left, 0, static_cast<LONG>(virtualWidth_));
            page.workArea.top = std::clamp<LONG>(page.workArea.top, 0, static_cast<LONG>(virtualHeight_));
            page.workArea.right = std::clamp<LONG>(page.workArea.right, page.workArea.left, static_cast<LONG>(virtualWidth_));
            page.workArea.bottom = std::clamp<LONG>(page.workArea.bottom, page.workArea.top, static_cast<LONG>(virtualHeight_));
            ConfigureGridPage(page);
        }

        std::wstring detectedPrimaryMonitorId;
        for (const auto& page : gridPages_)
        {
            if (page.isPrimary)
            {
                detectedPrimaryMonitorId = page.monitorId;
                break;
            }
        }
        if (detectedPrimaryMonitorId.empty() && !gridPages_.empty())
        {
            detectedPrimaryMonitorId = gridPages_.front().monitorId;
        }
        const bool hadPrimaryMonitor = !primaryMonitorId_.empty();
        if (primaryMonitorId_ != detectedPrimaryMonitorId)
        {
            primaryMonitorId_ = detectedPrimaryMonitorId;
            if (hadPrimaryMonitor || firstPageMonitorId_.empty())
            {
                firstPageMonitorId_ = primaryMonitorId_;
            }
            pageOffset_ = 0;
        }
        if (firstPageMonitorId_.empty())
        {
            firstPageMonitorId_ = primaryMonitorId_;
        }
        if (!firstPageMonitorId_.empty())
        {
            bool firstMonitorStillExists = false;
            for (const auto& page : gridPages_)
            {
                if (page.monitorId == firstPageMonitorId_)
                {
                    firstMonitorStillExists = true;
                    break;
                }
            }
            if (!firstMonitorStillExists)
            {
                firstPageMonitorId_ = primaryMonitorId_;
                pageOffset_ = 0;
            }
        }
        ApplyPageMapping();
        ApplySavedGridDimensions();

        if (!gridPages_.empty())
        {
            layoutWorkArea_ = gridPages_.front().workArea;
        }
    }

    void ApplySavedGridDimensions()
    {
        for (auto& page : gridPages_)
        {
            auto colIt = savedPageColumns_.find(page.id);
            auto rowIt = savedPageRows_.find(page.id);
            if (colIt != savedPageColumns_.end() && rowIt != savedPageRows_.end() &&
                colIt->second >= 2 && rowIt->second >= 2)
            {
                page.columns = colIt->second;
                page.rows = rowIt->second;
                ApplyGapScaleToPage(page);
            }
        }
    }

    void ApplyGapScaleToPage(GridPage& page)
    {
        const int usableW = std::max(1, static_cast<int>(page.workArea.right - page.workArea.left) - (page.marginX * 2));
        const int usableH = std::max(1, static_cast<int>(page.workArea.bottom - page.workArea.top) - (page.marginY * 2));
        const float cellRefW = static_cast<float>(usableW) / static_cast<float>(std::max(1, page.columns));
        const float cellRefH = static_cast<float>(usableH) / static_cast<float>(std::max(1, page.rows));
        const int targetGapX = std::max(0, static_cast<int>(cellRefW * kGapPercentX / gapScale_));
        const int targetGapY = std::max(0, static_cast<int>(cellRefH * kGapPercentY / gapScale_));

        page.cellWidth = page.columns > 1
            ? std::max(kIconSize, (usableW - (page.columns - 1) * targetGapX) / page.columns)
            : usableW;
        page.cellHeight = page.rows > 1
            ? std::max(kMinCellHeight / 2, (usableH - (page.rows - 1) * targetGapY) / page.rows)
            : usableH;
        page.gapX = page.columns > 1 ? (usableW - page.columns * page.cellWidth) / (page.columns - 1) : 0;
        page.gapY = page.rows > 1 ? (usableH - page.rows * page.cellHeight) / (page.rows - 1) : 0;
    }

    void SetZoom(float value)
    {
        float clamped = std::clamp(value, 0.5f, 2.0f);
        if (clamped == gapScale_)
        {
            return;
        }
        gapScale_ = clamped;
        for (auto& page : gridPages_)
        {
            ApplyGapScaleToPage(page);
        }
        UpdateTextFormats();
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void AdjustZoom(float delta)
    {
        float newVal = std::clamp(gapScale_ + delta, 0.5f, 2.0f);
        if (newVal == gapScale_)
        {
            return;
        }
        gapScale_ = newVal;
        for (auto& page : gridPages_)
        {
            ApplyGapScaleToPage(page);
        }
        UpdateTextFormats();
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void AdjustGridRows(int delta)
    {
        if (gridPages_.empty())
        {
            return;
        }
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* found = GridPageFromPoint(clientPoint);
        if (found == nullptr)
        {
            return;
        }
        GridPage* targetPage = nullptr;
        for (auto& page : gridPages_)
        {
            if (page.id == found->id) { targetPage = &page; break; }
        }
        if (targetPage == nullptr)
        {
            return;
        }

        constexpr int kMinRows = 1;
        constexpr int kMaxRows = 50;
        const int newRows = std::clamp(targetPage->rows + delta, kMinRows, kMaxRows);
        if (newRows == targetPage->rows)
        {
            return;
        }

        targetPage->rows = newRows;
        ApplyGapScaleToPage(*targetPage);

        savedPageColumns_[targetPage->id] = targetPage->columns;
        savedPageRows_[targetPage->id] = targetPage->rows;
        SaveLayoutSlots();
        ReloadItems(false);
    }

    void AdjustGridColumns(int delta)
    {
        if (gridPages_.empty())
        {
            return;
        }
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* found = GridPageFromPoint(clientPoint);
        if (found == nullptr)
        {
            return;
        }
        GridPage* targetPage = nullptr;
        for (auto& page : gridPages_)
        {
            if (page.id == found->id) { targetPage = &page; break; }
        }
        if (targetPage == nullptr)
        {
            return;
        }

        constexpr int kMinColumns = 1;
        constexpr int kMaxColumns = 50;
        const int newColumns = std::clamp(targetPage->columns + delta, kMinColumns, kMaxColumns);
        if (newColumns == targetPage->columns)
        {
            return;
        }

        targetPage->columns = newColumns;
        ApplyGapScaleToPage(*targetPage);

        savedPageColumns_[targetPage->id] = targetPage->columns;
        savedPageRows_[targetPage->id] = targetPage->rows;
        SaveLayoutSlots();
        ReloadItems(false);
    }

    void ConfigureGridPage(GridPage& page) const
    {
        const int cellW = static_cast<int>(kCellWidth * gapScale_);
        const int cellH = static_cast<int>(kMinCellHeight * gapScale_);
        const int width = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
        const int height = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
        const int usableWidth = std::max(1, width - (page.marginX * 2));
        const int usableHeight = std::max(1, height - (page.marginY * 2));

        page.columns = std::max(4, usableWidth / cellW);
        page.rows = std::max(3, usableHeight / cellH);
        page.cellWidth = cellW;
        page.cellHeight = cellH;
        page.gapX = page.columns > 1 ? std::max(0, (usableWidth - (page.columns * page.cellWidth)) / (page.columns - 1)) : 0;
        page.gapY = page.rows > 1 ? std::max(0, (usableHeight - (page.rows * page.cellHeight)) / (page.rows - 1)) : 0;
    }

    bool IsGeneratedExtraPageId(const std::wstring& pageId) const
    {
        return pageId.rfind(L"__extra:", 0) == 0;
    }

    std::wstring MakeExtraPageId(const std::wstring& monitorId) const
    {
        return L"__extra:" + monitorId;
    }

    void RememberSavedPageId(const std::wstring& pageId)
    {
        if (pageId.empty())
        {
            return;
        }

        if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
        {
            savedPageIds_.push_back(pageId);
        }
    }

    void LoadSavedPagesFromJson(const std::string& text)
    {
        size_t pagesName = text.find("\"pages\"");
        if (pagesName == std::string::npos)
        {
            return;
        }

        size_t arrayStart = text.find('[', pagesName);
        size_t arrayEnd = text.find(']', arrayStart == std::string::npos ? pagesName : arrayStart + 1);
        if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart)
        {
            return;
        }

        size_t pos = arrayStart + 1;
        while ((pos = text.find('{', pos)) != std::string::npos && pos < arrayEnd)
        {
            size_t objectEnd = text.find('}', pos);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd)
            {
                break;
            }

            std::string objectText = text.substr(pos, objectEnd - pos + 1);
            std::string pageUtf8;
            if (ReadJsonStringField(objectText, "id", pageUtf8))
            {
                std::wstring pageId = Utf8ToWide(pageUtf8);
                RememberSavedPageId(pageId);
                int columns = 0;
                int rows = 0;
                if (ReadJsonIntField(objectText, "columns", columns) && columns > 0)
                {
                    savedPageColumns_[pageId] = columns;
                }
                if (ReadJsonIntField(objectText, "rows", rows) && rows > 0)
                {
                    savedPageRows_[pageId] = rows;
                }
            }
            pos = objectEnd + 1;
        }
    }

    size_t FirstMonitorOrderIndex() const
    {
        if (gridPages_.empty())
        {
            return 0;
        }

        for (size_t i = 0; i < gridPages_.size(); ++i)
        {
            if (!firstPageMonitorId_.empty() && gridPages_[i].monitorId == firstPageMonitorId_)
            {
                return i;
            }
        }

        for (size_t i = 0; i < gridPages_.size(); ++i)
        {
            if (!primaryMonitorId_.empty() && gridPages_[i].monitorId == primaryMonitorId_)
            {
                return i;
            }
        }

        return 0;
    }

    std::vector<size_t> BuildMonitorRenderOrder() const
    {
        std::vector<size_t> order;
        if (gridPages_.empty())
        {
            return order;
        }

        order.reserve(gridPages_.size());
        const size_t first = FirstMonitorOrderIndex();
        for (size_t offset = 0; offset < gridPages_.size(); ++offset)
        {
            order.push_back((first + offset) % gridPages_.size());
        }
        return order;
    }

    int MaxPageOffset() const
    {
        if (savedPageIds_.empty() || gridPages_.empty())
        {
            return 0;
        }

        const int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
        return std::max(0, static_cast<int>(savedPageIds_.size()) - visiblePageCount);
    }

    void ApplyPageMapping()
    {
        lastMonitorPageId_.clear();
        if (gridPages_.empty())
        {
            return;
        }

        if (savedPageIds_.empty())
        {
            for (const auto& page : gridPages_)
            {
                RememberSavedPageId(page.monitorId);
            }
        }

        pageOffset_ = std::clamp(pageOffset_, 0, MaxPageOffset());
        std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
        const size_t numMonitors = monitorOrder.size();
        for (size_t i = 0; i < numMonitors; ++i)
        {
            GridPage& page = gridPages_[monitorOrder[i]];
            const bool isLast = (i == numMonitors - 1);
            const size_t pageIdx = i + (isLast ? static_cast<size_t>(pageOffset_) : 0);
            if (pageIdx < savedPageIds_.size())
            {
                page.id = savedPageIds_[pageIdx];
            }
            else
            {
                page.id = MakeExtraPageId(page.monitorId);
            }
            if (isLast)
            {
                lastMonitorPageId_ = page.id;
            }
        }
        if (!lastMonitorPageId_.empty() && savedPageIds_.size() <= gridPages_.size())
        {
            lastMonitorPageId_.clear();
        }
    }

    const GridPage* FindExactGridPage(const std::wstring& pageId) const
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == pageId)
            {
                return &page;
            }
        }
        return nullptr;
    }

    const GridPage* FindGridPage(const std::wstring& pageId) const
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == pageId)
            {
                return &page;
            }
        }
        return gridPages_.empty() ? nullptr : &gridPages_.front();
    }

    bool HasGridPage(const std::wstring& pageId) const
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == pageId)
            {
                return true;
            }
        }
        return false;
    }

    const GridPage* GridPageFromPoint(POINT point) const
    {
        const GridPage* fallback = gridPages_.empty() ? nullptr : &gridPages_.front();
        for (const auto& page : gridPages_)
        {
            if (PtInRect(&page.bounds, point) || PtInRect(&page.workArea, point))
            {
                return &page;
            }
        }
        return fallback;
    }

    int SlotFromCell(const GridCell& cell) const
    {
        const GridPage* page = FindGridPage(cell.pageId);
        const int rows = page != nullptr ? page->rows : GetRowsPerColumn();
        return std::max(0, cell.column) * std::max(1, rows) + std::max(0, cell.row);
    }

    GridCell CellFromSlot(int slot, const std::wstring& pageId = L"") const
    {
        const GridPage* page = FindGridPage(pageId);
        const int rows = page != nullptr ? page->rows : GetRowsPerColumn();
        GridCell cell;
        cell.pageId = page != nullptr ? page->id : pageId;
        cell.column = std::max(0, slot) / std::max(1, rows);
        cell.row = std::max(0, slot) % std::max(1, rows);
        return cell;
    }

    GridCell CellFromSequentialIndex(int index) const
    {
        int remaining = std::max(0, index);
        for (const auto& page : gridPages_)
        {
            if (IsGeneratedExtraPageId(page.id))
            {
                continue;
            }

            const int capacity = std::max(1, page.columns * page.rows);
            if (remaining < capacity)
            {
                GridCell cell;
                cell.pageId = page.id;
                cell.column = remaining / std::max(1, page.rows);
                cell.row = remaining % std::max(1, page.rows);
                return cell;
            }
            remaining -= capacity;
        }

        GridCell cell;
        if (!gridPages_.empty())
        {
            const GridPage* page = nullptr;
            for (auto it = gridPages_.rbegin(); it != gridPages_.rend(); ++it)
            {
                if (!IsGeneratedExtraPageId(it->id))
                {
                    page = &(*it);
                    break;
                }
            }
            if (page == nullptr)
            {
                page = &gridPages_.back();
            }
            cell.pageId = page->id;
            cell.column = std::max(0, page->columns - 1);
            cell.row = std::max(0, page->rows - 1);
        }
        return cell;
    }

    int GetGridAxisOffset(const GridPage& page, int index, bool horizontal) const
    {
        const int count = horizontal ? page.columns : page.rows;
        if (count <= 1)
        {
            return 0;
        }

        const int cellSize = horizontal ? page.cellWidth : page.cellHeight;
        const int margin = horizontal ? page.marginX : page.marginY;
        const int areaSize = horizontal
            ? static_cast<int>(page.workArea.right - page.workArea.left)
            : static_cast<int>(page.workArea.bottom - page.workArea.top);
        const int usable = std::max(1, areaSize - (margin * 2));
        const int travel = std::max(0, usable - cellSize);
        const int denominator = std::max(1, count - 1);
        return static_cast<int>((static_cast<long long>(travel) * std::clamp(index, 0, count - 1) + denominator / 2) / denominator);
    }

    int GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal) const
    {
        const int count = horizontal ? page.columns : page.rows;
        if (count <= 1)
        {
            return 0;
        }

        const int cellSize = horizontal ? page.cellWidth : page.cellHeight;
        const int margin = horizontal ? page.marginX : page.marginY;
        const int origin = horizontal ? page.workArea.left : page.workArea.top;
        int bestIndex = 0;
        int bestDistance = INT_MAX;
        for (int i = 0; i < count; ++i)
        {
            const int left = origin + margin + GetGridAxisOffset(page, i, horizontal);
            const int center = left + cellSize / 2;
            const int distance = std::abs(coordinate - center);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }
        return bestIndex;
    }

    int GetDesktopCellWidth(const std::wstring& pageId = {}) const
    {
        if (!pageId.empty())
        {
            const GridPage* page = FindExactGridPage(pageId);
            if (page != nullptr) return page->cellWidth;
        }
        if (!gridPages_.empty())
            return gridPages_.front().cellWidth;
        return static_cast<int>(kCellWidth * gapScale_);
    }

    int GetDesktopCellHeight(const std::wstring& pageId = {}) const
    {
        if (!pageId.empty())
        {
            const GridPage* page = FindExactGridPage(pageId);
            if (page != nullptr) return page->cellHeight;
        }
        if (!gridPages_.empty())
            return gridPages_.front().cellHeight;
        return static_cast<int>(kMinCellHeight * gapScale_);
    }

    RECT GetGridRect(const GridCell& cell, GridSpan span = {}) const
    {
        const GridPage* page = FindExactGridPage(cell.pageId);
        if (page == nullptr)
        {
            return MakeRect(0, 0, 0, 0);
        }

        const int column = std::clamp(cell.column, 0, std::max(0, page->columns - 1));
        const int row = std::clamp(cell.row, 0, std::max(0, page->rows - 1));
        const int spanColumns = std::clamp(span.columns, 1, std::max(1, page->columns - column));
        const int spanRows = std::clamp(span.rows, 1, std::max(1, page->rows - row));
        const int x = page->workArea.left + page->marginX + GetGridAxisOffset(*page, column, true);
        const int y = page->workArea.top + page->marginY + GetGridAxisOffset(*page, row, false);
        const int right = page->workArea.left + page->marginX + GetGridAxisOffset(*page, column + spanColumns - 1, true) + page->cellWidth;
        const int bottom = page->workArea.top + page->marginY + GetGridAxisOffset(*page, row + spanRows - 1, false) + page->cellHeight;
        return MakeRect(x, y, right, bottom);
    }

    RECT GetSlotRect(int slot) const
    {
        return GetGridRect(CellFromSlot(slot));
    }

    GridCell CellFromPoint(POINT point) const
    {
        const GridPage* page = GridPageFromPoint(point);
        GridCell cell;
        if (page == nullptr)
        {
            return cell;
        }

        cell.pageId = page->id;
        cell.column = GetGridAxisIndexFromPoint(*page, point.x, true);
        cell.row = GetGridAxisIndexFromPoint(*page, point.y, false);
        return cell;
    }

    int SlotFromPoint(POINT point) const
    {
        return SlotFromCell(CellFromPoint(point));
    }

    POINT GetDragTargetPoint(POINT current) const
    {
        return {
            dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
            dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
        };
    }

    void UpdateDragGroupOrigin(size_t anchorIndex)
    {
        if (anchorIndex >= items_.size())
        {
            return;
        }

        int minCol = INT_MAX;
        int minRow = INT_MAX;
        for (const auto& item : items_)
        {
            if (item.selected)
            {
                minCol = std::min(minCol, item.gridCell.column);
                minRow = std::min(minRow, item.gridCell.row);
            }
        }

        GridCell groupOrigin = items_[anchorIndex].gridCell;
        if (minCol != INT_MAX)
        {
            groupOrigin.column = minCol;
            groupOrigin.row = minRow;
        }

        RECT groupRect = GetGridRect(groupOrigin, {1, 1});
        dragGroupOriginX_ = groupRect.left;
        dragGroupOriginY_ = groupRect.top;
    }

    RECT GetTargetRectAt(POINT point) const
    {
        if (IsPointInsideOpenPopup(point))
        {
            return GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        }

        DesktopHit desktopHit = HitTestDesktop(point);
        if ((desktopHit.kind == DesktopHitKind::Widget ||
            desktopHit.kind == DesktopHitKind::WidgetMember ||
            desktopHit.kind == DesktopHitKind::WidgetContent ||
            desktopHit.kind == DesktopHitKind::WidgetAllButton) &&
            desktopHit.widgetIndex < widgets_.size())
        {
            return GetWidgetSelectionRect(widgets_[desktopHit.widgetIndex]);
        }

        int hit = HitTest(point);
        if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
        {
            return GetItemSelectionRect(items_[static_cast<size_t>(hit)], true);
        }

        return GetGridRect(CellFromPoint(point));
    }

    RECT GetSelectedDragBoundsAt(POINT point) const
    {
        RECT bounds{};
        bool hasBounds = false;
        int dx = point.x - mouseDownPoint_.x;
        int dy = point.y - mouseDownPoint_.y;

        for (const auto& item : items_)
        {
            if (!item.selected)
            {
                continue;
            }
            RECT sourceBounds = item.bounds;
            if (IsRectEmptyRect(sourceBounds) &&
                draggingCollectionMember_ &&
                mouseDownHit_ >= 0 &&
                &item == &items_[static_cast<size_t>(mouseDownHit_)])
            {
                sourceBounds = collectionDragStartBounds_;
            }
            if (IsRectEmptyRect(sourceBounds))
            {
                continue;
            }

            RECT moved = GetItemSelectionRect(sourceBounds, true);
            OffsetRect(&moved, dx, dy);
            bounds = hasBounds ? UnionCopy(bounds, moved) : moved;
            hasBounds = true;
        }

        return hasBounds ? bounds : RECT{};
    }

    RECT GetInternalDragDirtyRect(POINT point) const
    {
        RECT dirty = GetSelectedDragBoundsAt(point);
        dirty = UnionCopy(dirty, GetGridRect(dragTargetCell_));
        for (const RECT& rect : GetSelectedMovePreviewRectsForCell(dragTargetCell_))
        {
            dirty = UnionCopy(dirty, rect);
        }
        return InflateCopy(dirty, 16);
    }

    std::vector<RECT> GetSelectedMovePreviewRectsForCell(GridCell cell) const
    {
        std::vector<RECT> rects;
        std::vector<PendingGridMove> moves = BuildSelectedMove(cell);
        rects.reserve(moves.size());
        for (const PendingGridMove& move : moves)
        {
            RECT bounds = GetGridRect(move.cell, items_[move.index].gridSpan);
            rects.push_back(GetItemSelectionRect(bounds, true));
        }
        return rects;
    }

    std::vector<RECT> GetSelectedMovePreviewRects(POINT point) const
    {
        std::vector<RECT> rects;
        std::vector<PendingGridMove> moves = BuildSelectedMove(CellFromPoint(point));
        rects.reserve(moves.size());
        for (const PendingGridMove& move : moves)
        {
            RECT bounds = GetGridRect(move.cell, items_[move.index].gridSpan);
            rects.push_back(GetItemSelectionRect(bounds, true));
        }
        return rects;
    }

    RECT GetExternalDragDirtyRect(POINT point) const
    {
        RECT dirty = GetTargetRectAt(point);
        return InflateCopy(dirty, 16);
    }

    bool IsPointInsideOpenPopup(POINT point) const
    {
        if (popupWidgetIndex_ >= widgets_.size())
        {
            return false;
        }

        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        return PtInRect(&popup, point) != FALSE;
    }

    void InvalidateFast(RECT dirty)
    {
        if (hwnd_ == nullptr || IsRectEmptyRect(dirty))
        {
            return;
        }

        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT clipped{};
        if (!IntersectRect(&clipped, &dirty, &client))
        {
            return;
        }

        RedrawWindow(hwnd_, &clipped, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    std::wstring MakeInternalDragHint(POINT point) const
    {
        if (IsPointInsideOpenPopup(point))
        {
            return L"释放：调整集合内位置";
        }

        DesktopHit desktopHit = HitTestDesktop(point);
        if (desktopHit.kind == DesktopHitKind::PopupMember &&
            desktopHit.widgetIndex < widgets_.size() &&
            widgets_[desktopHit.widgetIndex].type == DesktopWidgetType::Collection)
        {
            return L"释放：加入集合「" + widgets_[desktopHit.widgetIndex].title + L"」";
        }
        if (desktopHit.kind == DesktopHitKind::WidgetMember)
        {
            if (desktopHit.itemIndex < items_.size() && !items_[desktopHit.itemIndex].selected &&
                IsPointInIconDropTarget(desktopHit.bounds, point))
            {
                return L"释放：交给「" + items_[desktopHit.itemIndex].name + L"」处理";
            }
            if (desktopHit.widgetIndex < widgets_.size() &&
                widgets_[desktopHit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                desktopHit.memberIndex < widgets_[desktopHit.widgetIndex].folderEntries.size() &&
                IsPointInIconDropTarget(desktopHit.bounds, point))
            {
                return L"释放：交给「" + widgets_[desktopHit.widgetIndex].folderEntries[desktopHit.memberIndex].name + L"」处理";
            }
        }
        if (IsWidgetDropSurface(desktopHit))
        {
            const DesktopWidget& widget = widgets_[desktopHit.widgetIndex];
            if (widget.type == DesktopWidgetType::Collection)
            {
                return L"释放：加入集合「" + widget.title + L"」";
            }
            if (widget.type == DesktopWidgetType::FileCategories)
            {
                return L"释放：加入桌面文件「" + widget.title + L"」";
            }
            if (widget.type == DesktopWidgetType::FolderMapping)
            {
                bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                return altDown
                    ? L"释放：创建快捷方式到「" + widget.title + L"」"
                    : L"释放：复制到映射文件夹「" + widget.title + L"」";
            }
        }

        int hit = HitTestForDropTarget(point);
        if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
        {
            return L"释放：交给「" + items_[static_cast<size_t>(hit)].name + L"」处理";
        }

        GridCell bestCell = FindBestDropCell(CellFromPoint(GetDragTargetPoint(point)));
        if (BuildSelectedMove(bestCell).empty())
        {
            return L"释放：当前位置已有图标";
        }

        return L"释放：移动到此空位";
    }

    std::wstring MakeExternalDragHint(POINT point) const
    {
        DesktopHit desktopHit = HitTestDesktop(point);
        if (desktopHit.kind == DesktopHitKind::WidgetMember)
        {
            if (desktopHit.itemIndex < items_.size())
            {
                return L"释放：拖入「" + items_[desktopHit.itemIndex].name + L"」";
            }
            if (desktopHit.widgetIndex < widgets_.size() &&
                widgets_[desktopHit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                desktopHit.memberIndex < widgets_[desktopHit.widgetIndex].folderEntries.size())
            {
                return L"释放：拖入「" + widgets_[desktopHit.widgetIndex].folderEntries[desktopHit.memberIndex].name + L"」";
            }
        }

        size_t folderMapping = FindFolderMappingAtPoint(point);
        if (folderMapping < widgets_.size())
        {
            return L"释放：拖入映射文件夹「" + widgets_[folderMapping].title + L"」";
        }

        int hit = HitTest(point);
        if (hit >= 0)
        {
            return L"释放：拖入「" + items_[static_cast<size_t>(hit)].name + L"」";
        }

        return L"释放：拖入桌面空白";
    }

    int HitTest(POINT point) const
    {
        for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i)
        {
            if (IsRectEmptyRect(items_[static_cast<size_t>(i)].bounds))
            {
                continue;
            }
            RECT hitRect = GetItemHitRect(items_[static_cast<size_t>(i)]);
            if (PtInRect(&hitRect, point))
            {
                return i;
            }
        }
        return -1;
    }

    int HitTestForDropTarget(POINT point) const
    {
        for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i)
        {
            if (IsRectEmptyRect(items_[static_cast<size_t>(i)].bounds))
            {
                continue;
            }
            RECT dropRect = GetItemDropTargetRect(items_[static_cast<size_t>(i)]);
            if (PtInRect(&dropRect, point))
            {
                return i;
            }
        }
        return -1;
    }

    static bool IsWidgetDropSurfaceKind(DesktopHitKind kind)
    {
        return kind == DesktopHitKind::Widget ||
            kind == DesktopHitKind::WidgetMember ||
            kind == DesktopHitKind::WidgetContent ||
            kind == DesktopHitKind::WidgetAllButton;
    }

    bool IsWidgetDropSurface(const DesktopHit& hit) const
    {
        return hit.widgetIndex < widgets_.size() && IsWidgetDropSurfaceKind(hit.kind);
    }

    DesktopHit HitTestDesktop(POINT point) const
    {
        if (popupWidgetIndex_ < widgets_.size())
        {
            const DesktopWidget& popupWidget = widgets_[popupWidgetIndex_];
            RECT popup = GetCollectionPopupRect(popupWidget);
            if (PtInRect(&popup, point))
            {
                std::vector<std::wstring> popupKeys = GetPopupItemKeys(popupWidget);
                for (size_t i = 0; i < popupKeys.size(); ++i)
                {
                    RECT itemRect = GetCollectionPopupItemRect(popup, i);
                    if (PtInRect(&itemRect, point))
                    {
                        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                        if (itemIndex != static_cast<size_t>(-1))
                        {
                            return { DesktopHitKind::PopupMember, itemIndex, popupWidgetIndex_, i, itemRect };
                        }
                    }
                }
                return { DesktopHitKind::Widget, static_cast<size_t>(-1), popupWidgetIndex_, static_cast<size_t>(-1), popup };
            }
        }

        for (int i = static_cast<int>(widgets_.size()) - 1; i >= 0; --i)
        {
            const auto& widget = widgets_[static_cast<size_t>(i)];

            if (widget.type == DesktopWidgetType::FolderMapping)
            {
                RECT toggleRect = GetFolderMappingToggleRect(widget);
                if (PtInRect(&toggleRect, point))
                {
                    return { DesktopHitKind::WidgetFolderToggle, static_cast<size_t>(-1), static_cast<size_t>(i), 0, toggleRect };
                }
                RECT openRect = GetFolderMappingOpenRect(widget);
                if (PtInRect(&openRect, point))
                {
                    return { DesktopHitKind::WidgetFolderOpen, static_cast<size_t>(-1), static_cast<size_t>(i), 0, openRect };
                }

                RECT moveHandle = GetWidgetMoveHandleRect(widget);
                if (PtInRect(&moveHandle, point))
                {
                    return { DesktopHitKind::Widget, static_cast<size_t>(-1), static_cast<size_t>(i), static_cast<size_t>(-1), moveHandle };
                }

                RECT content = GetFolderMappingContentRect(widget);
                for (size_t slot = 0; slot < widget.folderEntries.size(); ++slot)
                {
                    RECT itemRect = GetFolderMappingItemRect(widget, slot);
                    RECT clipped = itemRect;
                    clipped.top = std::max(clipped.top, content.top);
                    clipped.bottom = std::min(clipped.bottom, content.bottom);
                    if (clipped.bottom > content.top && clipped.top < content.bottom && PtInRect(&clipped, point))
                    {
                        return { DesktopHitKind::WidgetMember, static_cast<size_t>(-1), static_cast<size_t>(i), slot, itemRect };
                    }
                }
                if (PtInRect(&content, point))
                {
                    return { DesktopHitKind::WidgetContent, static_cast<size_t>(-1), static_cast<size_t>(i), static_cast<size_t>(-1), content };
                }
            }
            else if (widget.type == DesktopWidgetType::FileCategories)
            {
                RECT fcToggle = GetFileCategoryToggleRect(widget);
                if (PtInRect(&fcToggle, point))
                {
                    return { DesktopHitKind::WidgetFolderToggle, static_cast<size_t>(-1), static_cast<size_t>(i), 0, fcToggle };
                }

                RECT moveHandle = GetWidgetMoveHandleRect(widget);
                if (PtInRect(&moveHandle, point))
                {
                    return { DesktopHitKind::Widget, static_cast<size_t>(-1), static_cast<size_t>(i), static_cast<size_t>(-1), moveHandle };
                }

                std::vector<std::wstring> categoryIds = GetVisibleFileCategoryIds(widget);
                for (size_t slot = 0; slot < categoryIds.size(); ++slot)
                {
                    RECT tab = GetFileCategoryTabRect(widget, slot);
                    if (PtInRect(&tab, point))
                    {
                        return { DesktopHitKind::WidgetCategory, static_cast<size_t>(-1), static_cast<size_t>(i), slot, tab };
                    }
                }

                std::vector<std::wstring> keys = GetFileCategoryKeys(widget, GetActiveFileCategoryId(widget));
                RECT content = GetFileCategoryContentRect(widget);
                for (size_t slot = 0; slot < keys.size(); ++slot)
                {
                    RECT itemRect = GetFileCategoryItemRect(widget, slot);
                    RECT clipped = itemRect;
                    clipped.top = std::max(clipped.top, content.top);
                    clipped.bottom = std::min(clipped.bottom, content.bottom);
                    if (clipped.bottom > content.top && clipped.top < content.bottom && PtInRect(&clipped, point))
                    {
                        size_t itemIndex = FindItemIndexByKey(keys[slot]);
                        if (itemIndex != static_cast<size_t>(-1))
                        {
                            return { DesktopHitKind::WidgetMember, itemIndex, static_cast<size_t>(i), slot, itemRect };
                        }
                    }
                }
                if (PtInRect(&content, point))
                {
                    return { DesktopHitKind::WidgetContent, static_cast<size_t>(-1), static_cast<size_t>(i), static_cast<size_t>(-1), content };
                }
            }
            else
            {
                RECT moveHandle = GetWidgetMoveHandleRect(widget);
                if (PtInRect(&moveHandle, point))
                {
                    return { DesktopHitKind::Widget, static_cast<size_t>(-1), static_cast<size_t>(i), static_cast<size_t>(-1), moveHandle };
                }
            }

            if (widget.type == DesktopWidgetType::Collection)
            {
                const bool compact = widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
                if (!compact)
                {
                    const size_t inlineCapacity = std::min(GetCollectionInlineCapacity(widget), widget.itemKeys.size());
                    for (size_t slot = 0; slot < inlineCapacity; ++slot)
                    {
                        RECT slotRect = GetCollectionPreviewSlotRect(widget, slot);
                        if (PtInRect(&slotRect, point))
                        {
                            size_t itemIndex = FindItemIndexByKey(widget.itemKeys[slot]);
                            if (itemIndex != static_cast<size_t>(-1))
                            {
                                return { DesktopHitKind::WidgetMember, itemIndex, static_cast<size_t>(i), slot, slotRect };
                            }
                        }
                    }
                }

                if (!compact)
                {
                    size_t allSlot = GetCollectionAllButtonSlot(widget);
                    if (allSlot != static_cast<size_t>(-1))
                    {
                        RECT allRect = GetCollectionPreviewSlotRect(widget, allSlot);
                        if (PtInRect(&allRect, point))
                        {
                            return { DesktopHitKind::WidgetAllButton, static_cast<size_t>(-1), static_cast<size_t>(i), allSlot, allRect };
                        }
                    }
                }
            }

            RECT hitRect = GetWidgetHitRect(widget);
            if (PtInRect(&hitRect, point))
            {
                return { DesktopHitKind::Widget, static_cast<size_t>(-1), static_cast<size_t>(i), static_cast<size_t>(-1), hitRect };
            }
        }

        int itemHit = HitTest(point);
        if (itemHit >= 0)
        {
            return { DesktopHitKind::Item, static_cast<size_t>(itemHit), static_cast<size_t>(-1), static_cast<size_t>(-1), GetItemHitRect(items_[static_cast<size_t>(itemHit)]) };
        }
        return {};
    }

    RECT GetItemIconRect(RECT bounds) const
    {
        const int cellW = bounds.right - bounds.left;
        const int cellH = bounds.bottom - bounds.top;
        if (cellH < 50)
        {
            // List mode: icon on the left, small
            const int iconSz = std::min(32, cellH - 4);
            return MakeRect(
                bounds.left + 4,
                bounds.top + (cellH - iconSz) / 2,
                bounds.left + 4 + iconSz,
                bounds.top + (cellH + iconSz) / 2);
        }
        const int maxIconW = std::max(16, cellW - 8);
        const int maxIconH = std::max(16, cellH - kTextHeight - 8);
        const int iconSz = std::min(maxIconW, maxIconH);
        const int iconX = bounds.left + (cellW - iconSz) / 2;
        const int iconY = bounds.top + 2;
        return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
    }

    RECT GetItemTextRect(RECT bounds, bool expanded) const
    {
        RECT iconRect = GetItemIconRect(bounds);
        const int textTop = iconRect.bottom + 2;
        const int textH = expanded ? kTextExpandedHeight : kTextCollapsedHeight;
        return MakeRect(
            bounds.left + 4,
            textTop,
            bounds.right - 4,
            textTop + textH);
    }

    RECT GetItemTextRect(const DesktopItem& item, bool expanded) const
    {
        return GetItemTextRect(item.bounds, expanded);
    }

    RECT GetItemSelectionRect(RECT bounds, bool expanded) const
    {
        RECT textRect = GetItemTextRect(bounds, expanded);
        RECT selection = UnionCopy(GetItemIconRect(bounds), textRect);
        selection.left = std::max(bounds.left + 3, selection.left - 4);
        selection.top = std::max(bounds.top, selection.top - 2);
        selection.right = std::min(bounds.right - 3, selection.right + 4);
        selection.bottom = std::min(bounds.bottom - 2, textRect.bottom);
        return selection;
    }

    RECT GetItemSelectionRect(const DesktopItem& item, bool expanded) const
    {
        return GetItemSelectionRect(item.bounds, expanded);
    }

    RECT GetItemHitRect(const DesktopItem& item) const
    {
        return GetItemSelectionRect(item, item.selected);
    }

    RECT GetItemDropTargetRect(const DesktopItem& item) const
    {
        RECT iconRect = GetItemIconRect(item.bounds);
        iconRect.left -= 4;
        iconRect.top -= 2;
        iconRect.right += 4;
        iconRect.bottom += 4;
        return iconRect;
    }

    RECT GetIconDropTargetRect(RECT bounds) const
    {
        RECT iconRect = GetItemIconRect(bounds);
        iconRect.left -= 4;
        iconRect.top -= 2;
        iconRect.right += 4;
        iconRect.bottom += 4;
        return iconRect;
    }

    bool IsPointInIconDropTarget(RECT bounds, POINT point) const
    {
        RECT dropRect = GetIconDropTargetRect(bounds);
        return PtInRect(&dropRect, point) != FALSE;
    }

    void ClearSelection()
    {
        for (auto& item : items_)
        {
            item.selected = false;
        }
        for (auto& widget : widgets_)
        {
            widget.selected = false;
        }
        selectedWidgetIndex_ = static_cast<size_t>(-1);
        selectedCount_ = 0;
    }

    void SelectOnly(size_t index)
    {
        ClearSelection();
        items_[index].selected = true;
        selectedCount_ = 1;
    }

    void ToggleSelection(size_t index)
    {
        if (selectedWidgetIndex_ != static_cast<size_t>(-1))
        {
            for (auto& widget : widgets_)
            {
                widget.selected = false;
            }
            selectedWidgetIndex_ = static_cast<size_t>(-1);
        }
        items_[index].selected = !items_[index].selected;
        selectedCount_ += items_[index].selected ? 1 : -1;
    }

    void SelectWidgetOnly(size_t index)
    {
        ClearSelection();
        for (auto& w : widgets_)
            for (auto& e : w.folderEntries) e.selected = false;
        if (index < widgets_.size())
        {
            widgets_[index].selected = true;
            selectedWidgetIndex_ = index;
        }
    }

    void ClearFolderEntrySelection()
    {
        for (auto& widget : widgets_)
        {
            for (auto& entry : widget.folderEntries)
            {
                entry.selected = false;
            }
        }
    }

    void SelectFolderEntryOnly(size_t widgetIndex, size_t memberIndex)
    {
        ClearSelection();
        ClearFolderEntrySelection();
        if (widgetIndex < widgets_.size() &&
            widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
            memberIndex < widgets_[widgetIndex].folderEntries.size())
        {
            widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
        }
    }

    bool FindSingleSelectedFolderEntry(size_t& widgetIndex, size_t& memberIndex) const
    {
        bool found = false;
        for (size_t wi = 0; wi < widgets_.size(); ++wi)
        {
            if (widgets_[wi].type != DesktopWidgetType::FolderMapping)
            {
                continue;
            }
            for (size_t mi = 0; mi < widgets_[wi].folderEntries.size(); ++mi)
            {
                if (!widgets_[wi].folderEntries[mi].selected)
                {
                    continue;
                }
                if (found)
                {
                    return false;
                }
                widgetIndex = wi;
                memberIndex = mi;
                found = true;
            }
        }
        return found;
    }

    std::wstring GridSlotKey(const GridCell& cell) const
    {
        return cell.pageId + L":" + std::to_wstring(cell.column) + L":" + std::to_wstring(cell.row);
    }

    bool IsGridAreaValid(const GridCell& cell, GridSpan span) const
    {
        const GridPage* page = FindExactGridPage(cell.pageId);
        if (page == nullptr)
        {
            return false;
        }

        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        return cell.column >= 0 &&
            cell.row >= 0 &&
            cell.column + columns <= page->columns &&
            cell.row + rows <= page->rows;
    }

    bool AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span) const
    {
        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        for (int y = 0; y < rows; ++y)
        {
            for (int x = 0; x < columns; ++x)
            {
                GridCell occupied = cell;
                occupied.column += x;
                occupied.row += y;
                if (usedSlots.contains(GridSlotKey(occupied)))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span) const
    {
        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        for (int y = 0; y < rows; ++y)
        {
            for (int x = 0; x < columns; ++x)
            {
                GridCell occupied = cell;
                occupied.column += x;
                occupied.row += y;
                usedSlots.insert(GridSlotKey(occupied));
            }
        }
    }

    bool TryFindFreeCell(
        GridSpan span,
        const std::unordered_set<std::wstring>& usedSlots,
        GridCell& result,
        const std::wstring& preferredPageId = L"",
        int preferredStartSlot = 0) const
    {
        auto tryPage = [&](const GridPage& page, int startSlot, GridCell& found) -> bool {
            const int capacity = std::max(1, page.columns * page.rows);
            for (int slot = std::clamp(startSlot, 0, capacity - 1); slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = page.id;
                candidate.column = slot / std::max(1, page.rows);
                candidate.row = slot % std::max(1, page.rows);
                if (IsGridAreaValid(candidate, span) && !AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    found = candidate;
                    return true;
                }
            }
            return false;
        };

        if (!preferredPageId.empty())
        {
            for (const auto& page : gridPages_)
            {
                if (page.id == preferredPageId && tryPage(page, preferredStartSlot, result))
                {
                    return true;
                }
            }
        }

        for (const auto& page : gridPages_)
        {
            if (!preferredPageId.empty() && page.id == preferredPageId)
            {
                continue;
            }
            if (tryPage(page, 0, result))
            {
                return true;
            }
        }

        if (!preferredPageId.empty())
        {
            for (const auto& page : gridPages_)
            {
                if (page.id == preferredPageId && tryPage(page, 0, result))
                {
                    return true;
                }
            }
        }

        return false;
    }

    void CleanWidgetItemKeys()
    {
        std::unordered_set<std::wstring> available;
        for (const auto& item : items_)
        {
            if (!item.layoutKey.empty())
            {
                available.insert(NormalizeLayoutKey(item.layoutKey));
            }
        }

        std::unordered_set<std::wstring> assigned;
        for (auto& widget : widgets_)
        {
            std::vector<std::wstring> cleaned;
            cleaned.reserve(widget.itemKeys.size());
            for (const auto& rawKey : widget.itemKeys)
            {
                std::wstring key = NormalizeLayoutKey(rawKey);
                if (key.empty() || !available.contains(key) || assigned.contains(key))
                {
                    continue;
                }
                cleaned.push_back(key);
                assigned.insert(key);
            }
            widget.itemKeys = std::move(cleaned);
        }
    }

    void NormalizeWidgetsInGrid(std::unordered_set<std::wstring>& usedSlots)
    {
        for (auto& widget : widgets_)
        {
            widget.gridSpan.columns = std::max(1, widget.gridSpan.columns);
            widget.gridSpan.rows = std::max(1, widget.gridSpan.rows);
            if (!gridPages_.empty() && widget.gridCell.pageId.empty())
            {
                widget.gridCell.pageId = gridPages_.front().id;
            }

            bool valid = IsGridAreaValid(widget.gridCell, widget.gridSpan) &&
                !AreGridSlotsMarked(usedSlots, widget.gridCell, widget.gridSpan);
            if (!valid)
            {
                GridCell freeCell;
                if (TryFindFreeCell(widget.gridSpan, usedSlots, freeCell))
                {
                    widget.gridCell = freeCell;
                }
                else if (!gridPages_.empty())
                {
                    widget.gridCell.pageId = gridPages_.front().id;
                    widget.gridCell.column = 0;
                    widget.gridCell.row = 0;
                    widget.gridSpan.columns = 1;
                    widget.gridSpan.rows = 1;
                }
            }

            RememberSavedPageId(widget.gridCell.pageId);
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }
    }

    bool IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
    {
        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        for (const auto& item : items_)
        {
            if (!IsTopLevelItem(item))
            {
                continue;
            }
            if (item.selected || item.gridCell.pageId != cell.pageId)
            {
                continue;
            }

            const int itemRight = item.gridCell.column + std::max(1, item.gridSpan.columns);
            const int itemBottom = item.gridCell.row + std::max(1, item.gridSpan.rows);
            const int areaRight = cell.column + columns;
            const int areaBottom = cell.row + rows;
            if (cell.column < itemRight &&
                areaRight > item.gridCell.column &&
                cell.row < itemBottom &&
                areaBottom > item.gridCell.row)
            {
                return true;
            }
        }

        for (const auto& widget : widgets_)
        {
            if (widget.selected || widget.gridCell.pageId != cell.pageId)
            {
                continue;
            }

            const int widgetRight = widget.gridCell.column + std::max(1, widget.gridSpan.columns);
            const int widgetBottom = widget.gridCell.row + std::max(1, widget.gridSpan.rows);
            const int areaRight = cell.column + columns;
            const int areaBottom = cell.row + rows;
            if (cell.column < widgetRight &&
                areaRight > widget.gridCell.column &&
                cell.row < widgetBottom &&
                areaBottom > widget.gridCell.row)
            {
                return true;
            }
        }
        return false;
    }

    bool IsSlotOccupiedByUnselected(int slot) const
    {
        GridCell cell = CellFromSlot(slot);
        return IsGridAreaOccupiedByUnselected(cell, {});
    }

    bool IsCellOccupiedByUnselected(const GridCell& cell, GridSpan span = {}) const
    {
        return IsGridAreaOccupiedByUnselected(cell, span);
    }

    bool IsSlotOccupiedByUnselectedLegacy(int slot) const
    {
        for (const auto& item : items_)
        {
            if (item.slot == slot && !item.selected)
            {
                return true;
            }
        }
        return false;
    }

    GridCell FindBestDropCell(GridCell targetCell) const
    {
        if (!BuildSelectedMove(targetCell).empty()) return targetCell;

        const GridPage* page = FindExactGridPage(targetCell.pageId);
        if (!page) return targetCell;
        const int maxCol = page->columns - 1;
        const int maxRow = page->rows - 1;

        int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
        int dy = dragCurrentPoint_.y - mouseDownPoint_.y;
        int primaryCol = 0, primaryRow = 0;
        if (std::abs(dx) >= std::abs(dy))
            primaryCol = (dx >= 0) ? 1 : -1;
        else
            primaryRow = (dy >= 0) ? 1 : -1;
        if (primaryCol == 0 && primaryRow == 0) primaryCol = 1;

        // Try straight in primary direction
        for (int dist = 1; dist <= 8; ++dist)
        {
            GridCell probe = targetCell;
            probe.column += primaryCol * dist;
            probe.row += primaryRow * dist;
            if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow)
                break;
            if (!BuildSelectedMove(probe).empty()) return probe;
        }

        // Try the opposite direction
        int oppCol = -primaryCol, oppRow = -primaryRow;
        for (int dist = 1; dist <= 8; ++dist)
        {
            GridCell probe = targetCell;
            probe.column += oppCol * dist;
            probe.row += oppRow * dist;
            if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow)
                break;
            if (!BuildSelectedMove(probe).empty()) return probe;
        }

        // Spiral outward: try all adjacent cells in increasing distance
        for (int dist = 1; dist <= 6; ++dist)
        {
            for (int dc = -dist; dc <= dist; ++dc)
            {
                for (int dr = -dist; dr <= dist; ++dr)
                {
                    if (std::abs(dc) != dist && std::abs(dr) != dist) continue;
                    GridCell probe = targetCell;
                    probe.column += dc;
                    probe.row += dr;
                    if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow)
                        continue;
                    if (!BuildSelectedMove(probe).empty()) return probe;
                }
            }
        }

        return targetCell;
    }

    std::vector<PendingGridMove> BuildSelectedMove(GridCell targetCell) const
    {
        std::vector<PendingGridMove> moves;
        if (selectedCount_ <= 0)
        {
            return moves;
        }

        std::vector<size_t> selectedIndexes;
        selectedIndexes.reserve(static_cast<size_t>(selectedCount_));
        int minColumn = INT_MAX;
        int minRow = INT_MAX;
        int maxColumn = 0;
        int maxRow = 0;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
            {
                selectedIndexes.push_back(i);
                minColumn = std::min(minColumn, items_[i].gridCell.column);
                minRow = std::min(minRow, items_[i].gridCell.row);
                maxColumn = std::max(maxColumn, items_[i].gridCell.column + std::max(1, items_[i].gridSpan.columns));
                maxRow = std::max(maxRow, items_[i].gridCell.row + std::max(1, items_[i].gridSpan.rows));
            }
        }

        if (selectedIndexes.empty())
        {
            return moves;
        }

        const GridPage* page = FindExactGridPage(targetCell.pageId);
        if (page == nullptr)
        {
            return moves;
        }

        const int groupColumns = std::max(1, maxColumn - minColumn);
        const int groupRows = std::max(1, maxRow - minRow);
        const bool stacked = (groupColumns == 1 && groupRows == 1 && selectedIndexes.size() > 1);
        const int maxCol = page->columns - 1;
        const int maxPageRow = page->rows - 1;
        const int spreadCols = stacked ? std::min(static_cast<int>(selectedIndexes.size()), page->columns) : groupColumns;
        targetCell.column = std::clamp(targetCell.column, 0, std::max(0, page->columns - spreadCols));
        targetCell.row = std::clamp(targetCell.row, 0, std::max(0, page->rows - groupRows));

        int seqIndex = 0;
        std::unordered_set<std::wstring> usedSlots;
        for (size_t itemIndex : selectedIndexes)
        {
            GridCell movedCell = targetCell;
            if (stacked)
            {
                for (int attempt = 0; attempt < page->columns * page->rows; ++attempt)
                {
                    int col = seqIndex / page->rows;
                    int row = seqIndex % page->rows;
                    GridCell probe = targetCell;
                    probe.column += col;
                    probe.row += row;
                    ++seqIndex;
                    std::wstring slotKey = probe.pageId + L":" + std::to_wstring(SlotFromCell(probe));
                    if (probe.column <= maxCol && probe.row <= maxPageRow &&
                        !usedSlots.contains(slotKey) &&
                        !IsGridAreaOccupiedByUnselected(probe, items_[itemIndex].gridSpan))
                    {
                        movedCell = probe;
                        usedSlots.insert(slotKey);
                        break;
                    }
                }
            }
            else
            {
                movedCell.column += items_[itemIndex].gridCell.column - minColumn;
                movedCell.row += items_[itemIndex].gridCell.row - minRow;
            }

            if (!IsGridAreaValid(movedCell, items_[itemIndex].gridSpan) ||
                IsGridAreaOccupiedByUnselected(movedCell, items_[itemIndex].gridSpan))
            {
                moves.clear();
                return moves;
            }

            moves.push_back({ itemIndex, movedCell });
        }
        return moves;
    }

    void MoveSelectedItemsToCell(GridCell targetCell)
    {
        std::vector<PendingGridMove> moves = BuildSelectedMove(std::move(targetCell));
        if (moves.empty())
        {
            return;
        }

        for (const PendingGridMove& move : moves)
        {
            items_[move.index].gridCell = move.cell;
            items_[move.index].slot = SlotFromCell(move.cell);
        }
        LayoutItems();
        SaveLayoutSlots();
    }

    void MoveSelectedItemsToSlot(int targetSlot)
    {
        MoveSelectedItemsToCell(CellFromSlot(targetSlot));
    }

    bool GridAreasOverlap(const GridCell& leftCell, GridSpan leftSpan, const GridCell& rightCell, GridSpan rightSpan) const
    {
        if (leftCell.pageId != rightCell.pageId)
        {
            return false;
        }

        const int leftRight = leftCell.column + std::max(1, leftSpan.columns);
        const int leftBottom = leftCell.row + std::max(1, leftSpan.rows);
        const int rightRight = rightCell.column + std::max(1, rightSpan.columns);
        const int rightBottom = rightCell.row + std::max(1, rightSpan.rows);
        return leftCell.column < rightRight &&
            leftRight > rightCell.column &&
            leftCell.row < rightBottom &&
            leftBottom > rightCell.row;
    }

    void AddOccupiedExcept(
        std::unordered_set<std::wstring>& usedSlots,
        size_t movingWidget,
        const std::unordered_set<size_t>& displacedItems,
        const std::unordered_set<size_t>& displacedWidgets) const
    {
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!IsTopLevelItem(items_[i]) || displacedItems.contains(i))
            {
                continue;
            }
            MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
        }

        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (i == movingWidget || displacedWidgets.contains(i))
            {
                continue;
            }
            MarkGridArea(usedSlots, widgets_[i].gridCell, widgets_[i].gridSpan);
        }
    }

    void PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan)
    {
        if (widgetIndex >= widgets_.size())
        {
            return;
        }

        const GridPage* page = FindExactGridPage(targetCell.pageId);
        if (page == nullptr)
        {
            return;
        }

        targetSpan.columns = std::clamp(targetSpan.columns, 1, std::max(1, page->columns));
        targetSpan.rows = std::clamp(targetSpan.rows, 1, std::max(1, page->rows));
        targetCell.column = std::clamp(targetCell.column, 0, std::max(0, page->columns - targetSpan.columns));
        targetCell.row = std::clamp(targetCell.row, 0, std::max(0, page->rows - targetSpan.rows));

        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (i != widgetIndex && GridAreasOverlap(targetCell, targetSpan, widgets_[i].gridCell, widgets_[i].gridSpan))
            {
                return;
            }
        }

        std::vector<size_t> displacedItemOrder;
        std::unordered_set<size_t> displacedItems;
        std::unordered_set<size_t> displacedWidgets;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (IsTopLevelItem(items_[i]) && GridAreasOverlap(targetCell, targetSpan, items_[i].gridCell, items_[i].gridSpan))
            {
                displacedItems.insert(i);
                displacedItemOrder.push_back(i);
            }
        }

        auto byGridOrder = [this](auto left, auto right) {
            const GridCell& l = items_[left].gridCell;
            const GridCell& r = items_[right].gridCell;
            if (l.pageId != r.pageId) return l.pageId < r.pageId;
            return SlotFromCell(l) < SlotFromCell(r);
        };
        std::sort(displacedItemOrder.begin(), displacedItemOrder.end(), byGridOrder);

        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;
        std::unordered_set<std::wstring> usedSlots;
        std::unordered_set<size_t> noDisplacedWidgets;
        AddOccupiedExcept(usedSlots, widgetIndex, displacedItems, noDisplacedWidgets);
        MarkGridArea(usedSlots, targetCell, targetSpan);

        int searchStart = SlotFromCell(targetCell) + std::max(1, targetSpan.rows);
        for (size_t itemIndex : displacedItemOrder)
        {
            GridCell freeCell;
            if (TryFindFreeCell(items_[itemIndex].gridSpan, usedSlots, freeCell, targetCell.pageId, searchStart))
            {
                items_[itemIndex].gridCell = freeCell;
                items_[itemIndex].slot = SlotFromCell(freeCell);
                MarkGridArea(usedSlots, freeCell, items_[itemIndex].gridSpan);
            }
        }

        LayoutItems();
        SaveLayoutSlots();
    }

    void UpdateWidgetPreviewFromPoint(POINT point)
    {
        if (mouseDownWidgetIndex_ >= widgets_.size())
        {
            return;
        }

        if (resizingWidget_)
        {
            const GridPage* page = FindExactGridPage(widgetDragOriginalCell_.pageId);
            if (page == nullptr)
            {
                return;
            }

            const int stepX = std::max(1, page->cellWidth + page->gapX);
            const int stepY = std::max(1, page->cellHeight + page->gapY);
            const int deltaColumns = static_cast<int>(std::round(static_cast<double>(point.x - mouseDownPoint_.x) / static_cast<double>(stepX)));
            const int deltaRows = static_cast<int>(std::round(static_cast<double>(point.y - mouseDownPoint_.y) / static_cast<double>(stepY)));

            GridCell cell = widgetDragOriginalCell_;
            GridSpan span = widgetDragOriginalSpan_;
            if ((widgetResizeEdges_ & kResizeLeft) != 0)
            {
                cell.column += deltaColumns;
                span.columns -= deltaColumns;
            }
            if ((widgetResizeEdges_ & kResizeRight) != 0)
            {
                span.columns += deltaColumns;
            }
            if ((widgetResizeEdges_ & kResizeTop) != 0)
            {
                cell.row += deltaRows;
                span.rows -= deltaRows;
            }
            if ((widgetResizeEdges_ & kResizeBottom) != 0)
            {
                span.rows += deltaRows;
            }

            const bool needsMinSpan = mouseDownWidgetIndex_ < widgets_.size() &&
                (widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::FileCategories ||
                 widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::FolderMapping);
            const int minCols = needsMinSpan ? 2 : 1;
            const int minRows = needsMinSpan ? 2 : 1;
            const int preCols = span.columns;
            const int preRows = span.rows;
            const int preCellCol = cell.column;
            const int preCellRow = cell.row;
            span.columns = std::clamp(span.columns, minCols, std::max(1, page->columns));
            span.rows = std::clamp(span.rows, minRows, std::max(1, page->rows));
            cell.column = std::clamp(cell.column, 0, std::max(0, page->columns - span.columns));
            cell.row = std::clamp(cell.row, 0, std::max(0, page->rows - span.rows));

            widgetPreviewOccupied_ = false;
            for (size_t i = 0; i < widgets_.size(); ++i)
            {
                if (i != mouseDownWidgetIndex_ &&
                    GridAreasOverlap(cell, span, widgets_[i].gridCell, widgets_[i].gridSpan))
                {
                    widgetPreviewOccupied_ = true;
                    break;
                }
            }
            if (!widgetPreviewOccupied_ &&
                (span.columns != preCols || span.rows != preRows ||
                 cell.column != preCellCol || cell.row != preCellRow))
            {
                widgetPreviewOccupied_ = true;
            }

            widgetPreviewCell_ = cell;
            widgetPreviewSpan_ = span;
            return;
        }

        GridCell target = CellFromPoint(GetDragTargetPoint(point));
        const GridPage* page = FindExactGridPage(target.pageId);
        if (page == nullptr)
        {
            return;
        }
        target.column = std::clamp(target.column, 0, std::max(0, page->columns - widgetDragOriginalSpan_.columns));
        target.row = std::clamp(target.row, 0, std::max(0, page->rows - widgetDragOriginalSpan_.rows));

        widgetPreviewOccupied_ = false;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (i != mouseDownWidgetIndex_ &&
                GridAreasOverlap(target, widgetDragOriginalSpan_, widgets_[i].gridCell, widgets_[i].gridSpan))
            {
                widgetPreviewOccupied_ = true;
                break;
            }
        }

        widgetPreviewCell_ = target;
        widgetPreviewSpan_ = widgetDragOriginalSpan_;
    }

    std::wstring MakeNewWidgetId() const
    {
        return L"collection-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
    }

    void AddLuaWidgetAt(POINT screenPoint, const std::wstring& scriptFilename)
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        GridCell cell = CellFromPoint(clientPoint);
        if (cell.pageId.empty()) return;

        // Extract name from script: read first line
        std::wstring title = scriptFilename;
        if (title.size() > 4 && title.substr(title.size() - 4) == L".lua")
            title = title.substr(0, title.size() - 4);

        DesktopWidget widget;
        widget.id = MakeNewWidgetId();
        widget.type = DesktopWidgetType::LuaScript;
        widget.title = title;
        widget.scriptPath = scriptFilename;
        widget.gridCell = cell;
        widget.gridSpan = { 1, 1 };
        widgets_.push_back(std::move(widget));
        const size_t index = widgets_.size() - 1;
        SelectWidgetOnly(index);
        PlaceWidgetWithDisplacement(index, cell, { 1, 1 });
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void AddCollectionWidgetAt(POINT screenPoint)
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        GridCell cell = CellFromPoint(clientPoint);
        if (cell.pageId.empty())
        {
            return;
        }

        DesktopWidget widget;
        widget.id = MakeNewWidgetId();
        widget.type = DesktopWidgetType::Collection;
        widget.title = L"集合";
        widget.gridCell = cell;
        widget.gridSpan = { 1, 1 };
        widgets_.push_back(std::move(widget));
        const size_t index = widgets_.size() - 1;
        SelectWidgetOnly(index);
        PlaceWidgetWithDisplacement(index, cell, { 1, 1 });
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void AddFileCategoryWidgetAt(POINT screenPoint)
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        GridCell cell = CellFromPoint(clientPoint);
        if (cell.pageId.empty())
        {
            return;
        }

        DesktopWidget widget;
        widget.id = L"file-categories-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
        widget.type = DesktopWidgetType::FileCategories;
        widget.title = L"桌面文件";
        widget.gridCell = cell;
        widget.gridSpan = { 2, 2 };
        widgets_.push_back(std::move(widget));
        const size_t index = widgets_.size() - 1;
        SelectWidgetOnly(index);
        PlaceWidgetWithDisplacement(index, cell, { 2, 2 });
        int collectAnswer = MessageBoxW(
            hwnd_,
            L"是否将当前桌面散文件收集到此分类组件？\n收集后这些项目会从自由桌面顶层隐藏。",
            L"桌面文件分类",
            MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON1);
        if (collectAnswer == IDYES)
        {
            CollectFilesIntoFileCategoryWidget(index, true);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void DeleteWidget(size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size())
        {
            return;
        }

        std::vector<std::wstring> keysToRestore = widgets_[widgetIndex].itemKeys;
        GridCell startCell = widgets_[widgetIndex].gridCell;
        for (auto& entry : widgets_[widgetIndex].folderEntries)
        {
            if (entry.iconBitmap != nullptr)
            {
                DeleteObject(entry.iconBitmap);
            }
        }
        widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(widgetIndex));
        selectedWidgetIndex_ = static_cast<size_t>(-1);
        popupWidgetIndex_ = static_cast<size_t>(-1);

        std::unordered_set<std::wstring> restoringKeys;
        for (const auto& key : keysToRestore)
        {
            restoringKeys.insert(NormalizeLayoutKey(key));
        }

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& item : items_)
        {
            if (!IsTopLevelItem(item) || restoringKeys.contains(NormalizeLayoutKey(item.layoutKey)))
            {
                continue;
            }
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
        }
        for (const auto& widget : widgets_)
        {
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }

        int startSlot = SlotFromCell(startCell);
        for (const auto& key : keysToRestore)
        {
            size_t itemIndex = FindItemIndexByKey(key);
            if (itemIndex == static_cast<size_t>(-1))
            {
                continue;
            }

            items_[itemIndex].gridSpan = { 1, 1 };
            GridCell freeCell;
            if (TryFindFreeCell(items_[itemIndex].gridSpan, usedSlots, freeCell, startCell.pageId, startSlot))
            {
                items_[itemIndex].gridCell = freeCell;
                items_[itemIndex].slot = SlotFromCell(freeCell);
                MarkGridArea(usedSlots, freeCell, items_[itemIndex].gridSpan);
            }
        }

        ClearSelection();
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OpenCollectionPopup(size_t widgetIndex)
    {
        OpenCollectionPopupAt(widgetIndex, { LONG_MIN, LONG_MIN }, L"");
    }

    void OpenCollectionPopupAt(size_t widgetIndex, POINT anchorPoint, const std::wstring& categoryId = L"")
    {
        if (widgetIndex >= widgets_.size())
        {
            return;
        }
        popupWidgetIndex_ = widgetIndex;
        popupScrollOffset_ = 0;
        popupHasAnchor_ = anchorPoint.x != LONG_MIN || anchorPoint.y != LONG_MIN;
        popupAnchorPoint_ = anchorPoint;
        popupCategoryId_ = categoryId;
        popupPageId_ = widgets_[widgetIndex].gridCell.pageId;
        popupRect_ = GetCollectionPopupRect(widgets_[widgetIndex]);
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, GetCollectionPopupMaxScrollOffset(widgets_[widgetIndex], popupRect_));
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void CloseCollectionPopup()
    {
        if (popupWidgetIndex_ == static_cast<size_t>(-1))
        {
            return;
        }
        popupWidgetIndex_ = static_cast<size_t>(-1);
        popupScrollOffset_ = 0;
        popupHasAnchor_ = false;
        popupPageId_.clear();
        popupCategoryId_.clear();
        popupRect_ = {};
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool RemoveItemKeyFromWidget(size_t widgetIndex, const std::wstring& key)
    {
        if (widgetIndex >= widgets_.size())
        {
            return false;
        }

        std::wstring normalized = NormalizeLayoutKey(key);
        auto& keys = widgets_[widgetIndex].itemKeys;
        auto it = std::find(keys.begin(), keys.end(), normalized);
        if (it == keys.end())
        {
            return false;
        }
        keys.erase(it);
        return true;
    }

    void RemoveItemKeyFromAllCollections(const std::wstring& key)
    {
        std::wstring normalized = NormalizeLayoutKey(key);
        for (auto& widget : widgets_)
        {
            auto& keys = widget.itemKeys;
            keys.erase(std::remove(keys.begin(), keys.end(), normalized), keys.end());
        }
    }

    void RemoveSelectedItemsFromCollections(size_t sourceWidget)
    {
        std::vector<std::wstring> selectedKeys = GetSelectedItemKeysInDragOrder(sourceWidget);
        if (selectedKeys.empty())
        {
            return;
        }

        std::unordered_set<std::wstring> selectedSet(selectedKeys.begin(), selectedKeys.end());
        for (auto& widget : widgets_)
        {
            auto& keys = widget.itemKeys;
            keys.erase(
                std::remove_if(keys.begin(), keys.end(), [&](const std::wstring& key) {
                    return selectedSet.contains(NormalizeLayoutKey(key));
                }),
                keys.end());
        }
    }

    void RemoveSelectedItemsFromDesktop()
    {
        for (auto& widget : widgets_)
        {
            auto& keys = widget.itemKeys;
            keys.erase(
                std::remove_if(keys.begin(), keys.end(), [this](const std::wstring& key) {
                    size_t idx = FindItemIndexByKey(key);
                    return idx != static_cast<size_t>(-1) && items_[idx].selected;
                }),
                keys.end());
        }
    }

    size_t GetCollectionInsertIndex(size_t widgetIndex, POINT point, bool popup) const
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        {
            return 0;
        }

        const DesktopWidget& widget = widgets_[widgetIndex];
        const size_t count = widget.itemKeys.size();
        if (count == 0)
        {
            return 0;
        }

        if (popup)
        {
            RECT popupRect = GetCollectionPopupRect(widget);
            RECT content = GetCollectionPopupContentRect(popupRect);
            const int columns = GetCollectionPopupColumnCount(popupRect);
            const int x = std::clamp<int>(
                point.x,
                static_cast<int>(content.left),
                static_cast<int>(std::max<LONG>(content.left, content.right - 1)));
            const int y = std::max<int>(static_cast<int>(content.top), point.y + popupScrollOffset_);
            const int col = std::clamp<int>(
                (x - static_cast<int>(content.left)) / GetDesktopCellWidth(widget.gridCell.pageId),
                0,
                std::max(0, columns - 1));
            const int row = std::max<int>(0, (y - static_cast<int>(content.top)) / GetDesktopCellHeight(widget.gridCell.pageId));
            size_t index = static_cast<size_t>(row * columns + col);
            RECT cell = GetCollectionPopupItemRect(popupRect, std::min(index, count - 1));
            if (index < count && point.x > (cell.left + cell.right) / 2)
            {
                ++index;
            }
            return std::min(index, count);
        }

        const bool compact = widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
        const size_t visible = compact ? std::min<size_t>(4, count) : std::min(GetCollectionInlineCapacity(widget), count);
        if (visible == 0)
        {
            return 0;
        }
        for (size_t i = 0; i < visible; ++i)
        {
            RECT slot = GetCollectionPreviewSlotRect(widget, i);
            if (PtInRect(&slot, point))
            {
                return point.x > (slot.left + slot.right) / 2 ? std::min(i + 1, count) : i;
            }
        }

        size_t allSlot = GetCollectionAllButtonSlot(widget);
        if (allSlot != static_cast<size_t>(-1))
        {
            RECT allRect = GetCollectionPreviewSlotRect(widget, allSlot);
            if (PtInRect(&allRect, point))
            {
                return count;
            }
        }
        return count;
    }

    std::vector<std::wstring> GetSelectedItemKeysInDragOrder(size_t sourceWidget) const
    {
        std::vector<std::wstring> keys;
        std::unordered_set<std::wstring> added;

        auto addIfSelected = [&](const std::wstring& rawKey) {
            std::wstring key = NormalizeLayoutKey(rawKey);
            if (key.empty() || added.contains(key))
            {
                return;
            }
            size_t itemIndex = FindItemIndexByKey(key);
            if (itemIndex != static_cast<size_t>(-1) && items_[itemIndex].selected)
            {
                keys.push_back(key);
                added.insert(key);
            }
        };

        if (sourceWidget < widgets_.size())
        {
            for (const auto& key : widgets_[sourceWidget].itemKeys)
            {
                addIfSelected(key);
            }
        }

        for (const auto& item : items_)
        {
            if (item.selected)
            {
                addIfSelected(item.layoutKey);
            }
        }
        return keys;
    }

    bool MoveSelectedItemsToCollection(size_t widgetIndex, size_t insertIndex, size_t sourceWidget)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        {
            return false;
        }

        std::vector<std::wstring> selectedKeys = GetSelectedItemKeysInDragOrder(sourceWidget);
        if (selectedKeys.empty())
        {
            return false;
        }

        std::unordered_set<std::wstring> selectedSet(selectedKeys.begin(), selectedKeys.end());
        if (sourceWidget == widgetIndex)
        {
            size_t before = 0;
            const auto& oldKeys = widgets_[widgetIndex].itemKeys;
            for (size_t i = 0; i < std::min(insertIndex, oldKeys.size()); ++i)
            {
                if (selectedSet.contains(NormalizeLayoutKey(oldKeys[i])))
                {
                    ++before;
                }
            }
            insertIndex -= std::min(insertIndex, before);
        }

        for (auto& widget : widgets_)
        {
            auto& keys = widget.itemKeys;
            keys.erase(
                std::remove_if(keys.begin(), keys.end(), [&](const std::wstring& key) {
                    return selectedSet.contains(NormalizeLayoutKey(key));
                }),
                keys.end());
        }

        auto& targetKeys = widgets_[widgetIndex].itemKeys;
        insertIndex = std::min(insertIndex, targetKeys.size());
        targetKeys.insert(targetKeys.begin() + static_cast<std::ptrdiff_t>(insertIndex), selectedKeys.begin(), selectedKeys.end());
        LayoutItems();
        SaveLayoutSlots();
        return true;
    }

    bool AddItemKeyToCollection(size_t widgetIndex, const std::wstring& key, size_t insertIndex = static_cast<size_t>(-1))
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        {
            return false;
        }

        std::wstring normalized = NormalizeLayoutKey(key);
        RemoveItemKeyFromAllCollections(normalized);
        auto& keys = widgets_[widgetIndex].itemKeys;
        if (std::find(keys.begin(), keys.end(), normalized) == keys.end())
        {
            insertIndex = insertIndex == static_cast<size_t>(-1) ? keys.size() : std::min(insertIndex, keys.size());
            keys.insert(keys.begin() + static_cast<std::ptrdiff_t>(insertIndex), normalized);
            return true;
        }
        return false;
    }

    bool AddSelectedItemsToCollection(size_t widgetIndex, size_t insertIndex = static_cast<size_t>(-1))
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        {
            return false;
        }

        if (insertIndex != static_cast<size_t>(-1))
        {
            bool moved = MoveSelectedItemsToCollection(widgetIndex, insertIndex, static_cast<size_t>(-1));
            if (moved)
            {
                return true;
            }
        }

        bool changed = false;
        for (auto& item : items_)
        {
            if (!item.selected || item.layoutKey.empty())
            {
                continue;
            }
            changed = AddItemKeyToCollection(widgetIndex, item.layoutKey) || changed;
            item.selected = false;
        }
        selectedCount_ = 0;
        if (changed)
        {
            LayoutItems();
            SaveLayoutSlots();
        }
        return changed;
    }

    int GetRowsPerColumn() const
    {
        return gridPages_.empty() ? 1 : std::max(1, gridPages_.front().rows);
    }

    void MoveKeyboardSelection(int delta)
    {
        if (items_.empty())
        {
            return;
        }

        std::vector<size_t> visibleItems;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!IsTopLevelItem(items_[i]))
            {
                continue;
            }
            if (items_[i].selected)
            {
                visibleItems.push_back(i);
            }
            else if (!IsRectEmptyRect(items_[i].bounds))
            {
                visibleItems.push_back(i);
            }
        }

        if (visibleItems.empty())
        {
            return;
        }

        int current = -1;
        for (size_t i = 0; i < visibleItems.size(); ++i)
        {
            if (items_[visibleItems[i]].selected)
            {
                current = static_cast<int>(i);
                break;
            }
        }
        int next = current < 0 ? 0 : current + delta;
        next = std::clamp(next, 0, static_cast<int>(visibleItems.size()) - 1);
        SelectOnly(visibleItems[static_cast<size_t>(next)]);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    std::vector<PCUITEMID_CHILD> SelectedChildPidls() const
    {
        std::vector<PCUITEMID_CHILD> pidls;
        for (const auto& item : items_)
        {
            if (item.selected)
            {
                pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()));
            }
        }
        return pidls;
    }

    ComPtr<IDataObject> CreateSelectedDataObject()
    {
        auto pidls = SelectedChildPidls();
        if (pidls.empty())
        {
            return nullptr;
        }

        ComPtr<IDataObject> dataObject;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            IID_IDataObject,
            nullptr,
            reinterpret_cast<void**>(dataObject.GetAddressOf()));
        if (FAILED(hr))
        {
            return nullptr;
        }

        return dataObject;
    }

    static ComPtr<IDataObject> CreateFileDropDataObject(const std::vector<std::wstring>& paths)
    {
        if (paths.empty())
        {
            return nullptr;
        }

        std::vector<PIDLIST_ABSOLUTE> pidls;
        for (const auto& p : paths)
        {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)))
            {
                pidls.push_back(pidl);
            }
        }
        if (pidls.empty())
        {
            return nullptr;
        }

        std::vector<PCUITEMID_CHILD> children;
        children.reserve(pidls.size());
        for (auto pidl : pidls)
        {
            children.push_back(ILFindLastID(pidl));
        }

        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD unusedChild = nullptr;
        if (FAILED(SHBindToParent(pidls.front(), IID_IShellFolder,
            reinterpret_cast<void**>(&parentFolder), &unusedChild)))
        {
            for (auto& pidl : pidls) ILFree(pidl);
            return nullptr;
        }

        ComPtr<IDataObject> dataObj;
        parentFolder->GetUIObjectOf(
            nullptr,
            static_cast<UINT>(children.size()),
            children.data(),
            IID_IDataObject,
            nullptr,
            reinterpret_cast<void**>(dataObj.GetAddressOf()));
        parentFolder->Release();

        for (auto& pidl : pidls)
        {
            ILFree(pidl);
        }
        return dataObj;
    }

    ComPtr<IDataObject> CreateFolderEntriesDataObject(size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping)
        {
            return nullptr;
        }

        std::vector<std::wstring> paths;
        for (const auto& entry : widgets_[widgetIndex].folderEntries)
        {
            if (entry.selected)
            {
                paths.push_back(entry.fullPath);
            }
        }
        return CreateFileDropDataObject(paths);
    }

    size_t FindFolderMappingAtPoint(POINT clientPoint) const
    {
        DesktopHit hit = HitTestDesktop(clientPoint);
        if (IsWidgetDropSurface(hit) &&
            widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping)
        {
            return hit.widgetIndex;
        }

        for (int i = static_cast<int>(widgets_.size()) - 1; i >= 0; --i)
        {
            const size_t index = static_cast<size_t>(i);
            if (widgets_[index].type != DesktopWidgetType::FolderMapping)
            {
                continue;
            }
            RECT hitRect = GetWidgetHitRect(widgets_[index]);
            if (PtInRect(&hitRect, clientPoint))
            {
                return index;
            }
        }
        return static_cast<size_t>(-1);
    }

    void CopyFilesToFolder(const std::vector<std::wstring>& paths, const std::wstring& destFolder, bool move)
    {
        if (paths.empty() || destFolder.empty()) return;

        std::wstring fromStr;
        for (const auto& p : paths)
        {
            fromStr += p;
            fromStr += L'\0';
        }
        fromStr += L'\0';

        std::wstring toStr = destFolder;
        if (!toStr.empty() && toStr.back() != L'\\') toStr += L'\\';
        toStr += L'\0';

        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = move ? FO_MOVE : FO_COPY;
        op.pFrom = fromStr.c_str();
        op.pTo = toStr.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI;
        SHFileOperationW(&op);
    }

    void DropExternalFilesToFolderMapping(size_t widgetIndex, IDataObject* dataObject, DWORD, DWORD* effect)
    {
        if (widgetIndex >= widgets_.size() || dataObject == nullptr) return;

        FORMATETC fmt{};
        fmt.cfFormat = CF_HDROP;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        STGMEDIUM med{};
        if (FAILED(dataObject->GetData(&fmt, &med))) return;
        if (med.hGlobal == nullptr)
        {
            ReleaseStgMedium(&med);
            return;
        }

        HDROP hDrop = static_cast<HDROP>(med.hGlobal);
        UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < fileCount; ++i)
        {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
            {
                paths.push_back(path);
            }
        }
        ReleaseStgMedium(&med);
        if (paths.empty()) return;

        bool isCut = false;
        {
            static UINT cfDropEffect = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
            FORMATETC fmtPe{};
            fmtPe.cfFormat = cfDropEffect;
            fmtPe.dwAspect = DVASPECT_CONTENT;
            fmtPe.lindex = -1;
            fmtPe.tymed = TYMED_HGLOBAL;
            STGMEDIUM medPe{};
            if (SUCCEEDED(dataObject->GetData(&fmtPe, &medPe)))
            {
                if (medPe.hGlobal)
                {
                    DWORD* pe = static_cast<DWORD*>(GlobalLock(medPe.hGlobal));
                    if (pe)
                    {
                        isCut = (*pe == DROPEFFECT_MOVE);
                        GlobalUnlock(medPe.hGlobal);
                    }
                }
                ReleaseStgMedium(&medPe);
            }
        }

        bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        std::wstring destFolder = widgets_[widgetIndex].sourceFolderPath;
        if (!destFolder.empty() && destFolder.back() != L'\\') destFolder += L'\\';

        if (altDown)
        {
            for (const auto& filePath : paths)
            {
                std::wstring name = PathFindFileNameW(filePath.c_str());
                std::wstring linkPath = destFolder + name + L".lnk";
                ComPtr<IShellLinkW> shellLink;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                    IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                {
                    shellLink->SetPath(filePath.c_str());
                    ComPtr<IPersistFile> persistFile;
                    if (SUCCEEDED(shellLink.As(&persistFile)))
                        persistFile->Save(linkPath.c_str(), TRUE);
                }
            }
        }
        else
        {
            CopyFilesToFolder(paths, destFolder, isCut);
        }
        if (effect) *effect = isCut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        RefreshFolderMappingWidget(widgetIndex);
        if (isCut) ReloadItems();
    }

    std::unordered_set<std::wstring> SnapshotDesktopItemKeys() const
    {
        std::unordered_set<std::wstring> keys;
        for (const auto& item : items_)
        {
            if (!item.layoutKey.empty())
            {
                keys.insert(NormalizeLayoutKey(item.layoutKey));
            }
        }
        return keys;
    }

    void PlaceNewDesktopItemsAtDropPoint(const std::unordered_set<std::wstring>& existingKeys, POINT dropPoint)
    {
        std::vector<size_t> newItems;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!IsTopLevelItem(items_[i]) || items_[i].layoutKey.empty())
            {
                continue;
            }
            if (!existingKeys.contains(NormalizeLayoutKey(items_[i].layoutKey)))
            {
                newItems.push_back(i);
            }
        }
        if (newItems.empty())
        {
            return;
        }

        GridCell target = CellFromPoint(GetDragTargetPoint(dropPoint));
        const GridPage* page = FindExactGridPage(target.pageId);
        if (page == nullptr)
        {
            return;
        }

        std::unordered_set<size_t> newSet(newItems.begin(), newItems.end());
        std::unordered_set<std::wstring> usedSlots;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            MarkGridArea(usedSlots, widgets_[i].gridCell, widgets_[i].gridSpan);
        }
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!IsTopLevelItem(items_[i]) || newSet.contains(i))
            {
                continue;
            }
            MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
        }

        ClearSelection();
        int searchSlot = SlotFromCell(target);
        bool changed = false;
        for (size_t itemIndex : newItems)
        {
            GridSpan span = items_[itemIndex].gridSpan;
            span.columns = std::max(1, span.columns);
            span.rows = std::max(1, span.rows);

            GridCell cell{};
            bool found = false;
            if (IsGridAreaValid(target, span) && !AreGridSlotsMarked(usedSlots, target, span))
            {
                cell = target;
                found = true;
            }
            else
            {
                found = TryFindFreeCell(span, usedSlots, cell, target.pageId, searchSlot);
            }

            if (!found)
            {
                continue;
            }

            items_[itemIndex].gridCell = cell;
            items_[itemIndex].gridSpan = span;
            items_[itemIndex].slot = SlotFromCell(cell);
            items_[itemIndex].selected = true;
            ++selectedCount_;
            MarkGridArea(usedSlots, cell, span);
            searchSlot = items_[itemIndex].slot + 1;
            changed = true;
        }

        if (changed)
        {
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
    }

    static void ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize)
    {
        if (bitmap == nullptr) return;
        SHSTOCKICONINFO sii{};
        sii.cbSize = sizeof(sii);
        if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || sii.hIcon == nullptr)
            return;
        HDC hdc = CreateCompatibleDC(nullptr);
        HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(hdc, bitmap));
        const int arrowSz = 30;
        DrawIconEx(hdc, 5, bitmapSize.cy - arrowSz,
            sii.hIcon, arrowSz, arrowSz, 0, nullptr, DI_NORMAL);
        SelectObject(hdc, oldBmp);
        DeleteDC(hdc);
        DestroyIcon(sii.hIcon);
    }

    static void DebugLog(const wchar_t* msg)
    {
        OutputDebugStringW(msg);
        OutputDebugStringW(L"\n");

        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* lastSep = wcsrchr(path, L'\\');
        if (lastSep) *(lastSep + 1) = L'\0';
        wcscat_s(path, L"snow_debug.log");

        try
        {
            auto logger = spdlog::get("snow");
            if (!logger)
            {
                char logPath[MAX_PATH * 3];
                WideCharToMultiByte(CP_UTF8, 0, path, -1, logPath, sizeof(logPath), nullptr, nullptr);
                logger = spdlog::basic_logger_mt("snow", logPath, true);
                logger->flush_on(spdlog::level::info);
            }

            char buf[512];
            WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf), nullptr, nullptr);
            logger->info(buf);
            logger->flush();
        }
        catch (...)
        {
            OutputDebugStringW(L"[DebugLog] spdlog exception\n");
        }
    }

    static const wchar_t* DebugBool(bool value)
    {
        return value ? L"true" : L"false";
    }

    static void DebugWindowClass(HWND window, wchar_t* buffer, size_t bufferSize)
    {
        if (buffer == nullptr || bufferSize == 0)
        {
            return;
        }

        if (window == nullptr)
        {
            wcscpy_s(buffer, bufferSize, L"(null)");
            return;
        }

        if (!IsWindow(window))
        {
            wcscpy_s(buffer, bufferSize, L"(invalid)");
            return;
        }

        if (GetClassNameW(window, buffer, static_cast<int>(bufferSize)) == 0)
        {
            wcscpy_s(buffer, bufferSize, L"(class-error)");
        }
    }

    static void DebugLogWindow(const wchar_t* label, HWND window)
    {
        wchar_t className[128]{};
        DebugWindowClass(window, className, std::size(className));

        const bool valid = window != nullptr && IsWindow(window);
        HWND parent = valid ? GetParent(window) : nullptr;
        HWND root = valid ? GetAncestor(window, GA_ROOT) : nullptr;
        LONG_PTR style = valid ? GetWindowLongPtrW(window, GWL_STYLE) : 0;
        LONG_PTR exStyle = valid ? GetWindowLongPtrW(window, GWL_EXSTYLE) : 0;
        RECT rect{};
        if (valid)
        {
            GetWindowRect(window, &rect);
        }

        wchar_t message[1024]{};
        swprintf_s(
            message,
            L"%s hwnd=%p valid=%s class=%s parent=%p root=%p visible=%s style=0x%llX exStyle=0x%llX rect=(%ld,%ld,%ld,%ld)",
            label,
            window,
            DebugBool(valid),
            className,
            parent,
            root,
            DebugBool(valid && IsWindowVisible(window) != FALSE),
            static_cast<unsigned long long>(style),
            static_cast<unsigned long long>(exStyle),
            rect.left,
            rect.top,
            rect.right,
            rect.bottom);
        DebugLog(message);
    }

    static void DebugLogDesktopWindows(const wchar_t* label, const DesktopWindows& windows)
    {
        wchar_t message[512]{};
        swprintf_s(
            message,
            L"%s summary progman=%p host=%p defView=%p listView=%p listViewWasVisible=%s",
            label,
            windows.progman,
            windows.host,
            windows.defView,
            windows.listView,
            DebugBool(windows.listViewWasVisible));
        DebugLog(message);

        DebugLogWindow(L"  progman", windows.progman);
        DebugLogWindow(L"  host", windows.host);
        DebugLogWindow(L"  defView", windows.defView);
        DebugLogWindow(L"  listView", windows.listView);
    }

    std::vector<std::wstring> GetSelectedFolderEntryPaths(size_t widgetIndex) const
    {
        std::vector<std::wstring> paths;
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping)
            return paths;
        for (const auto& entry : widgets_[widgetIndex].folderEntries)
        {
            if (entry.selected)
                paths.push_back(entry.fullPath);
        }
        return paths;
    }

    POINT ScreenPointToClient(POINTL screenPoint) const
    {
        POINT point{ screenPoint.x, screenPoint.y };
        ScreenToClient(hwnd_, &point);
        return point;
    }

    POINTL ClientPointToScreenPoint(POINT clientPoint) const
    {
        POINT point = clientPoint;
        ClientToScreen(hwnd_, &point);
        return POINTL{ point.x, point.y };
    }

    static bool IsSameWindowTree(HWND parent, HWND window)
    {
        return parent != nullptr && window != nullptr && (window == parent || IsChild(parent, window));
    }

    bool IsKnownDesktopSurfaceWindow(HWND window) const
    {
        if (window == nullptr)
        {
            return false;
        }

        HWND root = GetAncestor(window, GA_ROOT);
        if (root == nullptr)
        {
            root = window;
        }

        if (IsSameWindowTree(hwnd_, window) || window == hwnd_ || root == hwnd_)
        {
            return true;
        }
        if (hintHwnd_ != nullptr && (IsSameWindowTree(hintHwnd_, window) || root == hintHwnd_))
        {
            return true;
        }

        auto isDesktopSurface = [&](HWND candidate) {
            return candidate != nullptr &&
                (window == candidate || root == candidate || IsChild(candidate, window));
        };

        if (isDesktopSurface(desktopWindows_.host) ||
            isDesktopSurface(desktopWindows_.progman) ||
            isDesktopSurface(desktopWindows_.defView) ||
            isDesktopSurface(desktopWindows_.listView))
        {
            return true;
        }

        HWND desktop = GetDesktopWindow();
        return window == desktop || root == desktop;
    }

    bool IsExternalDropWindowAt(POINT clientPoint) const
    {
        POINT screenPoint = clientPoint;
        ClientToScreen(hwnd_, &screenPoint);
        HWND hit = WindowFromPoint(screenPoint);
        if (hit == nullptr || IsKnownDesktopSurfaceWindow(hit))
        {
            return false;
        }

        HWND root = GetAncestor(hit, GA_ROOT);
        if (root == nullptr)
        {
            root = hit;
        }
        return IsWindowVisible(root) != FALSE;
    }

    DWORD ChooseDropEffect(DWORD keyState, DWORD allowed) const
    {
        DWORD available = allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        if (available == 0)
        {
            available = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        }

        if ((keyState & MK_CONTROL) != 0 && (available & DROPEFFECT_COPY) != 0)
        {
            return DROPEFFECT_COPY;
        }

        if ((keyState & MK_SHIFT) != 0 && (available & DROPEFFECT_MOVE) != 0)
        {
            return DROPEFFECT_MOVE;
        }

        if ((keyState & MK_ALT) != 0 && (available & DROPEFFECT_LINK) != 0)
        {
            return DROPEFFECT_LINK;
        }

        if ((available & DROPEFFECT_MOVE) != 0)
        {
            return DROPEFFECT_MOVE;
        }

        if ((available & DROPEFFECT_COPY) != 0)
        {
            return DROPEFFECT_COPY;
        }

        return available & DROPEFFECT_LINK;
    }

    ComPtr<IDropTarget> GetShellDropTargetAt(POINT clientPoint, int* targetIndex = nullptr)
    {
        if (targetIndex != nullptr)
        {
            *targetIndex = -1;
        }

        int hit = HitTest(clientPoint);
        if (hit >= 0)
        {
            if (targetIndex != nullptr)
            {
                *targetIndex = hit;
            }

            PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(items_[static_cast<size_t>(hit)].childPidl.get());
            ComPtr<IDropTarget> target;
            HRESULT hr = desktopFolder_->GetUIObjectOf(
                hwnd_,
                1,
                &child,
                IID_IDropTarget,
                nullptr,
                reinterpret_cast<void**>(target.GetAddressOf()));
            if (SUCCEEDED(hr) && target)
            {
                return target;
            }
        }

        ComPtr<IDropTarget> desktopTarget;
        HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IDropTarget, reinterpret_cast<void**>(desktopTarget.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            return desktopTarget;
        }

        return nullptr;
    }

    ComPtr<IDropTarget> GetDesktopItemDropTarget(size_t itemIndex)
    {
        if (itemIndex >= items_.size())
        {
            return nullptr;
        }

        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(items_[itemIndex].childPidl.get());
        ComPtr<IDropTarget> target;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            1,
            &child,
            IID_IDropTarget,
            nullptr,
            reinterpret_cast<void**>(target.GetAddressOf()));
        return SUCCEEDED(hr) ? target : nullptr;
    }

    ComPtr<IDropTarget> GetFolderEntryDropTarget(size_t widgetIndex, size_t memberIndex)
    {
        if (widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
            memberIndex >= widgets_[widgetIndex].folderEntries.size())
        {
            return nullptr;
        }

        PIDLIST_ABSOLUTE pidl = nullptr;
        const std::wstring& fullPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
        if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            return nullptr;
        }

        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD child = nullptr;
        if (FAILED(SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parentFolder), &child)))
        {
            ILFree(pidl);
            return nullptr;
        }

        ComPtr<IDropTarget> target;
        HRESULT hr = parentFolder->GetUIObjectOf(
            hwnd_,
            1,
            &child,
            IID_IDropTarget,
            nullptr,
            reinterpret_cast<void**>(target.GetAddressOf()));
        parentFolder->Release();
        ILFree(pidl);
        return SUCCEEDED(hr) ? target : nullptr;
    }

    HRESULT DropDataObjectToTarget(IDropTarget* target, IDataObject* dataObject, POINT clientPoint, DWORD keyState, DWORD* effect)
    {
        if (target == nullptr || dataObject == nullptr || effect == nullptr)
        {
            return E_POINTER;
        }

        DWORD localEffect = *effect;
        if ((localEffect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) == 0)
        {
            localEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        }

        POINTL screenPoint = ClientPointToScreenPoint(clientPoint);
        HRESULT hr = target->DragEnter(dataObject, keyState, screenPoint, &localEffect);
        if (SUCCEEDED(hr))
        {
            hr = target->DragOver(keyState, screenPoint, &localEffect);
        }
        if (SUCCEEDED(hr))
        {
            hr = target->Drop(dataObject, keyState, screenPoint, &localEffect);
        }
        else
        {
            target->DragLeave();
        }

        *effect = localEffect;
        return hr;
    }

    std::vector<std::wstring> GetDropPaths(IDataObject* dataObject)
    {
        std::vector<std::wstring> paths;
        FORMATETC fmt{};
        fmt.cfFormat = CF_HDROP;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        STGMEDIUM med{};
        if (SUCCEEDED(dataObject->GetData(&fmt, &med)))
        {
            HDROP hDrop = static_cast<HDROP>(med.hGlobal);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i)
            {
                wchar_t path[MAX_PATH]{};
                if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                    paths.push_back(path);
            }
            ReleaseStgMedium(&med);
        }
        return paths;
    }

    HRESULT DropDataObjectToWidgetMember(const DesktopHit& hit, IDataObject* dataObject, POINT clientPoint, DWORD keyState, DWORD* effect)
    {
        if (hit.kind != DesktopHitKind::WidgetMember)
        {
            return E_FAIL;
        }

        ComPtr<IDropTarget> target;
        if (hit.itemIndex < items_.size())
        {
            target = GetDesktopItemDropTarget(hit.itemIndex);
        }
        else if (hit.widgetIndex < widgets_.size() &&
            widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping)
        {
            target = GetFolderEntryDropTarget(hit.widgetIndex, hit.memberIndex);
        }

        if (!target)
        {
            if (effect != nullptr)
            {
                *effect = DROPEFFECT_NONE;
            }
            return E_FAIL;
        }

        return DropDataObjectToTarget(target.Get(), dataObject, clientPoint, keyState, effect);
    }

    HRESULT DropDataObjectAt(IDataObject* dataObject, POINT clientPoint, DWORD keyState, DWORD* effect)
    {
        if (dataObject == nullptr || effect == nullptr)
        {
            return E_POINTER;
        }

        DWORD localEffect = *effect;
        if ((localEffect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) == 0)
        {
            localEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        }

        ComPtr<IDropTarget> target = GetShellDropTargetAt(clientPoint);
        if (!target)
        {
            *effect = DROPEFFECT_NONE;
            return E_FAIL;
        }

        *effect = localEffect;
        return DropDataObjectToTarget(target.Get(), dataObject, clientPoint, keyState, effect);
    }

    void OpenItem(size_t index)
    {
        SHELLEXECUTEINFOW exec{};
        exec.cbSize = sizeof(exec);
        exec.fMask = SEE_MASK_IDLIST;
        exec.hwnd = hwnd_;
        exec.nShow = SW_SHOWNORMAL;
        exec.lpIDList = items_[index].absolutePidl.get();
        ShellExecuteExW(&exec);
    }

    void OpenSelected()
    {
        if (selectedWidgetIndex_ < widgets_.size())
        {
            OpenCollectionPopup(selectedWidgetIndex_);
            return;
        }
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
            {
                OpenItem(i);
                return;
            }
        }
    }

    bool IsProtectedDesktopIcon(const DesktopItem& item) const
    {
        std::wstring clsid = !item.desktopIconClsid.empty()
            ? item.desktopIconClsid
            : ExtractClsidText(item.parsingName);
        return clsid == kDesktopIconClsidThisPC ||
            clsid == kDesktopIconClsidUserFiles ||
            clsid == kDesktopIconClsidNetwork ||
            clsid == kDesktopIconClsidControlPanel ||
            clsid == kDesktopIconClsidRecycleBin;
    }

    bool CanUseSelectedFileCommands() const
    {
        if (selectedCount_ <= 0)
        {
            return false;
        }

        for (const auto& item : items_)
        {
            if (item.selected && IsProtectedDesktopIcon(item))
            {
                return false;
            }
        }
        return true;
    }

    bool IsShellFolderItem(const DesktopItem& item) const
    {
        if (!desktopFolder_ || item.childPidl.get() == nullptr)
        {
            return false;
        }

        SFGAOF attrs = SFGAO_FOLDER;
        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get());
        return SUCCEEDED(desktopFolder_->GetAttributesOf(1, &child, &attrs)) && (attrs & SFGAO_FOLDER) != 0;
    }

    std::wstring GetDesktopItemExtensionUpper(const DesktopItem& item) const
    {
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
        {
            return ToUpperInvariant(PathFindExtensionW(path));
        }
        return ToUpperInvariant(PathFindExtensionW(item.name.c_str()));
    }

    std::vector<std::wstring> GetFileCategoryOrder() const
    {
        return {
            L"all",
            L"folders",
            L"videos",
            L"images",
            L"documents",
            L"archives",
            L"audio",
            L"programs",
            L"others",
        };
    }

    std::wstring GetFileCategoryLabel(const std::wstring& categoryId) const
    {
        if (categoryId == L"all") return L"全部";
        if (categoryId == L"folders") return L"文件夹";
        if (categoryId == L"videos") return L"视频";
        if (categoryId == L"images") return L"图片";
        if (categoryId == L"documents") return L"文档";
        if (categoryId == L"archives") return L"压缩包";
        if (categoryId == L"audio") return L"音频";
        if (categoryId == L"programs") return L"程序";
        return L"其他";
    }

    std::wstring GetFileCategoryId(const DesktopItem& item) const
    {
        const std::wstring ext = GetDesktopItemExtensionUpper(item);
        if (ext == L".ZIP" || ext == L".RAR" || ext == L".7Z" || ext == L".TAR" || ext == L".GZ" || ext == L".BZ2" || ext == L".XZ")
        {
            return L"archives";
        }

        if (IsShellFolderItem(item))
        {
            return L"folders";
        }

        if (ext == L".MP4" || ext == L".MOV" || ext == L".AVI" || ext == L".MKV" || ext == L".WMV" || ext == L".WEBM" || ext == L".M4V")
        {
            return L"videos";
        }
        if (ext == L".PNG" || ext == L".JPG" || ext == L".JPEG" || ext == L".GIF" || ext == L".BMP" || ext == L".WEBP" || ext == L".HEIC" || ext == L".SVG")
        {
            return L"images";
        }
        if (ext == L".TXT" || ext == L".MD" || ext == L".DOC" || ext == L".DOCX" || ext == L".PDF" || ext == L".XLS" || ext == L".XLSX" || ext == L".PPT" || ext == L".PPTX" || ext == L".CSV")
        {
            return L"documents";
        }
        if (ext == L".MP3" || ext == L".WAV" || ext == L".FLAC" || ext == L".AAC" || ext == L".M4A" || ext == L".OGG")
        {
            return L"audio";
        }
        if (ext == L".EXE" || ext == L".MSI" || ext == L".BAT" || ext == L".CMD" || ext == L".LNK")
        {
            return L"programs";
        }
        return L"others";
    }

    bool IsFileCategoryCollectableItem(const DesktopItem& item) const
    {
        return !IsProtectedDesktopIcon(item) && !item.layoutKey.empty();
    }

    bool IsDesktopFileCategoryCandidate(const DesktopItem& item) const
    {
        return IsTopLevelItem(item) && IsFileCategoryCollectableItem(item);
    }

    std::vector<std::wstring> GetFileCategoryKeys(const DesktopWidget& widget, const std::wstring& categoryId = L"") const
    {
        std::vector<std::wstring> keys;
        std::unordered_set<std::wstring> seen;
        auto appendKey = [&](const std::wstring& rawKey) {
            size_t itemIndex = FindItemIndexByKey(rawKey);
            if (itemIndex == static_cast<size_t>(-1) || !IsFileCategoryCollectableItem(items_[itemIndex]))
                return;
            std::wstring nk = NormalizeLayoutKey(items_[itemIndex].layoutKey);
            if (seen.insert(nk).second)
                keys.push_back(nk);
        };

        if (!categoryId.empty())
        {
            if (categoryId == L"all")
            {
                for (const auto& rawKey : widget.itemKeys)
                    appendKey(rawKey);
            }
            else
            {
                for (const auto& rawKey : widget.itemKeys)
                {
                    size_t itemIndex = FindItemIndexByKey(rawKey);
                    if (itemIndex == static_cast<size_t>(-1) ||
                        GetFileCategoryId(items_[itemIndex]) != categoryId)
                        continue;
                    appendKey(rawKey);
                }
            }
        }
        else
        {
            for (const auto& id : GetFileCategoryOrder())
            {
                for (const auto& rawKey : widget.itemKeys)
                {
                    size_t itemIndex = FindItemIndexByKey(rawKey);
                    if (itemIndex == static_cast<size_t>(-1) ||
                        GetFileCategoryId(items_[itemIndex]) != id)
                        continue;
                    appendKey(rawKey);
                }
            }
        }
        return keys;
    }

    int GetFileCategoryCount(const DesktopWidget& widget, const std::wstring& categoryId) const
    {
        return static_cast<int>(GetFileCategoryKeys(widget, categoryId).size());
    }

    std::vector<std::wstring> GetVisibleFileCategoryIds(const DesktopWidget& widget) const
    {
        std::vector<std::wstring> ids;
        for (const auto& id : GetFileCategoryOrder())
        {
            if (GetFileCategoryCount(widget, id) <= 0)
            {
                continue;
            }
            ids.push_back(id);
        }
        return ids;
    }

    std::wstring GetActiveFileCategoryId(const DesktopWidget& widget) const
    {
        std::vector<std::wstring> visible = GetVisibleFileCategoryIds(widget);
        if (!widget.activeCategoryId.empty() &&
            std::find(visible.begin(), visible.end(), widget.activeCategoryId) != visible.end())
        {
            return widget.activeCategoryId;
        }
        return visible.empty() ? L"" : visible.front();
    }

    RECT GetFileCategoryTabsRect(const DesktopWidget& widget) const
    {
        RECT body = GetWidgetBodyRect(widget);
        InflateRect(&body, -10, -8);
        if (IsRectEmptyRect(body))
        {
            return {};
        }
        return MakeRect(body.left, body.top, body.right, std::min<LONG>(body.bottom, body.top + 30));
    }

    RECT GetFileCategoryContentRect(const DesktopWidget& widget) const
    {
        RECT body = GetWidgetBodyRect(widget);
        InflateRect(&body, -4, -8);
        if (IsRectEmptyRect(body))
        {
            return {};
        }
        RECT tabs = GetFileCategoryTabsRect(widget);
        body.top = std::min<LONG>(body.bottom, tabs.bottom + 8);
        return body;
    }

    RECT GetFileCategoryTabRect(const DesktopWidget& widget, size_t index) const
    {
        std::vector<std::wstring> tabs = GetVisibleFileCategoryIds(widget);
        if (index >= tabs.size())
        {
            return {};
        }
        RECT tabsRect = GetFileCategoryTabsRect(widget);
        constexpr int minTabWidth = 64;
        const int tabCount = static_cast<int>(tabs.size());
        const int equalWidth = std::max<int>(1, static_cast<int>(tabsRect.right - tabsRect.left) / tabCount);
        const int tabWidth = std::max(minTabWidth, equalWidth);
        const int totalWidth = tabWidth * tabCount;
        const int maxScroll = std::max(0, totalWidth - static_cast<int>(tabsRect.right - tabsRect.left));
        const int scroll = std::clamp(widget.tabScrollOffset, 0, maxScroll);
        const int startX = static_cast<int>(tabsRect.left) - scroll;
        RECT rect = MakeRect(
            startX + static_cast<LONG>(index * tabWidth),
            tabsRect.top,
            index + 1 == tabs.size() ? startX + totalWidth : startX + static_cast<LONG>((index + 1) * tabWidth),
            tabsRect.bottom);
        InflateRect(&rect, -2, -2);
        return rect;
    }

    int GetFileCategoryTileColumnCount(const DesktopWidget& widget) const
    {
        return std::max<int>(1, widget.gridSpan.columns);
    }

    int GetFileCategoryContentHeight(const DesktopWidget& widget, size_t itemCount) const
    {
        if (widget.listMode)
        {
            constexpr int rowHeight = 38;
            return static_cast<int>(itemCount) * rowHeight;
        }

        RECT content = GetFileCategoryContentRect(widget);
        const int columns = GetFileCategoryTileColumnCount(widget);
        const int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
        return rows * GetDesktopCellHeight(widget.gridCell.pageId);
    }

    int GetFileCategoryMaxScrollOffset(const DesktopWidget& widget) const
    {
        std::wstring categoryId = GetActiveFileCategoryId(widget);
        std::vector<std::wstring> keys = GetFileCategoryKeys(widget, categoryId);
        RECT content = GetFileCategoryContentRect(widget);
        const int contentHeight = std::max<int>(1, static_cast<int>(content.bottom - content.top));
        return std::max(0, GetFileCategoryContentHeight(widget, keys.size()) - contentHeight + kMinCellHeight / 2);
    }

    RECT GetFileCategoryItemRect(const DesktopWidget& widget, size_t linearIndex) const
    {
        RECT content = GetFileCategoryContentRect(widget);
        const int scroll = std::clamp(widget.scrollOffset, 0, GetFileCategoryMaxScrollOffset(widget));
        if (widget.listMode)
        {
            constexpr int rowHeight = 38;
            RECT rect = MakeRect(
                content.left,
                content.top + static_cast<LONG>(linearIndex * rowHeight) - scroll,
                content.right,
                content.top + static_cast<LONG>((linearIndex + 1) * rowHeight) - scroll);
            InflateRect(&rect, -4, -2);
            return rect;
        }

        const int columns = GetFileCategoryTileColumnCount(widget);
        const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
        const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
        const int itemW = std::max<int>(1, static_cast<int>(content.right - content.left) / columns);
        const int cellH = GetDesktopCellHeight(widget.gridCell.pageId);
        RECT rect = MakeRect(
            content.left + col * itemW,
            content.top + row * cellH - scroll,
            col + 1 == columns ? content.right : content.left + (col + 1) * itemW,
            content.top + (row + 1) * cellH - scroll);
        return rect;
    }

    RECT GetFolderMappingContentRect(const DesktopWidget& widget) const
    {
        RECT body = GetWidgetBodyRect(widget);
        InflateRect(&body, -4, -8);
        return body;
    }

    RECT GetFolderMappingToggleRect(const DesktopWidget& widget) const
    {
        RECT handle = GetWidgetMoveHandleRect(widget);
        constexpr int btnSize = 14;
        constexpr int gap = 2;
        constexpr int resizeReserve = 24;
        return MakeRect(handle.right - resizeReserve - gap - btnSize - gap - btnSize, handle.top + 5,
            handle.right - resizeReserve - gap - btnSize - gap, handle.bottom - 3);
    }

    RECT GetFolderMappingOpenRect(const DesktopWidget& widget) const
    {
        RECT handle = GetWidgetMoveHandleRect(widget);
        constexpr int btnSize = 14;
        constexpr int gap = 2;
        constexpr int resizeReserve = 24;
        return MakeRect(handle.right - resizeReserve - gap - btnSize, handle.top + 5,
            handle.right - resizeReserve - gap, handle.bottom - 3);
    }

    RECT GetFileCategoryToggleRect(const DesktopWidget& widget) const
    {
        RECT handle = GetWidgetMoveHandleRect(widget);
        constexpr int btnSize = 14;
        constexpr int gap = 2;
        constexpr int resizeReserve = 24;
        return MakeRect(handle.right - resizeReserve - gap - btnSize, handle.top + 5,
            handle.right - resizeReserve - gap, handle.bottom - 3);
    }

    int GetFolderMappingTileColumnCount(const DesktopWidget& widget) const
    {
        return std::max<int>(1, widget.gridSpan.columns);
    }

    int GetFolderMappingContentHeight(const DesktopWidget& widget, size_t itemCount) const
    {
        if (widget.listMode)
        {
            constexpr int rowHeight = 38;
            return static_cast<int>(itemCount) * rowHeight;
        }
        RECT content = GetFolderMappingContentRect(widget);
        const int columns = GetFolderMappingTileColumnCount(widget);
        const int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
        return rows * GetDesktopCellHeight(widget.gridCell.pageId);
    }

    int GetFolderMappingMaxScrollOffset(const DesktopWidget& widget) const
    {
        RECT content = GetFolderMappingContentRect(widget);
        const int contentHeight = std::max<int>(1, static_cast<int>(content.bottom - content.top));
        return std::max(0, GetFolderMappingContentHeight(widget, widget.folderEntries.size()) - contentHeight + kMinCellHeight / 2);
    }

    RECT GetFolderMappingItemRect(const DesktopWidget& widget, size_t linearIndex) const
    {
        RECT content = GetFolderMappingContentRect(widget);
        const int scroll = std::clamp(widget.scrollOffset, 0, GetFolderMappingMaxScrollOffset(widget));
        if (widget.listMode)
        {
            constexpr int rowHeight = 38;
            RECT rect = MakeRect(
                content.left,
                content.top + static_cast<LONG>(linearIndex * rowHeight) - scroll,
                content.right,
                content.top + static_cast<LONG>((linearIndex + 1) * rowHeight) - scroll);
            InflateRect(&rect, -4, -2);
            return rect;
        }

        const int columns = GetFolderMappingTileColumnCount(widget);
        const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
        const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
        const int itemW = std::max<int>(1, static_cast<int>(content.right - content.left) / columns);
        const int cellH = GetDesktopCellHeight(widget.gridCell.pageId);
        RECT rect = MakeRect(
            content.left + col * itemW,
            content.top + row * cellH - scroll,
            col + 1 == columns ? content.right : content.left + (col + 1) * itemW,
            content.top + (row + 1) * cellH - scroll);
        return rect;
    }

    size_t GetWidgetMemberInsertIndex(size_t widgetIndex, POINT point) const
    {
        if (widgetIndex >= widgets_.size())
        {
            return 0;
        }

        const DesktopWidget& widget = widgets_[widgetIndex];
        if (widget.type == DesktopWidgetType::FileCategories)
        {
            std::wstring active = GetActiveFileCategoryId(widget);
            std::vector<std::wstring> keys = GetFileCategoryKeys(widget, active);
            size_t count = keys.size();
            if (count == 0) return 0;

            RECT content = GetFileCategoryContentRect(widget);
            const int scroll = std::clamp(widget.scrollOffset, 0, GetFileCategoryMaxScrollOffset(widget));
            const int cy = static_cast<int>(content.top);
            const int cx = static_cast<int>(content.left);
            if (widget.listMode)
            {
                constexpr int rowHeight = 38;
                int y = std::max<int>(0, static_cast<int>(point.y) - cy + scroll);
                if (y >= static_cast<int>(count) * rowHeight)
                {
                    return count;
                }
                int row = std::clamp(y / rowHeight, 0, std::max<int>(0, static_cast<int>(count) - 1));
                size_t index = static_cast<size_t>(row);
                if ((y % rowHeight) > rowHeight / 2)
                {
                    ++index;
                }
                return std::min(index, count);
            }

            const int columns = GetFileCategoryTileColumnCount(widget);
            const int itemW = std::max<int>(1, static_cast<int>(content.right - content.left) / columns);
            int col = std::clamp((static_cast<int>(point.x) - cx) / itemW, 0, columns - 1);
            int row = std::max<int>(0, (static_cast<int>(point.y) - cy + scroll) / GetDesktopCellHeight(widget.gridCell.pageId));
            size_t index = static_cast<size_t>(row * columns + col);
            RECT cell = GetFileCategoryItemRect(widget, std::min(index, count - 1));
            if (index < count && point.x > (cell.left + cell.right) / 2) ++index;
            return std::min(index, count);
        }

        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            size_t count = widget.folderEntries.size();
            if (count == 0) return 0;

            RECT content = GetFolderMappingContentRect(widget);
            const int scroll = std::clamp(widget.scrollOffset, 0, GetFolderMappingMaxScrollOffset(widget));
            const int cy = static_cast<int>(content.top);
            const int cx = static_cast<int>(content.left);
            if (widget.listMode)
            {
                constexpr int rowHeight = 38;
                int y = std::max<int>(0, static_cast<int>(point.y) - cy + scroll);
                if (y >= static_cast<int>(count) * rowHeight)
                {
                    return count;
                }
                int row = std::clamp(y / rowHeight, 0, std::max<int>(0, static_cast<int>(count) - 1));
                size_t index = static_cast<size_t>(row);
                if ((y % rowHeight) > rowHeight / 2)
                {
                    ++index;
                }
                return std::min(index, count);
            }

            const int columns = GetFolderMappingTileColumnCount(widget);
            const int itemW = std::max<int>(1, static_cast<int>(content.right - content.left) / columns);
            int col = std::clamp((static_cast<int>(point.x) - cx) / itemW, 0, columns - 1);
            int row = std::max<int>(0, (static_cast<int>(point.y) - cy + scroll) / GetDesktopCellHeight(widget.gridCell.pageId));
            size_t index = static_cast<size_t>(row * columns + col);
            RECT cell = GetFolderMappingItemRect(widget, std::min(index, count - 1));
            if (index < count && point.x > (cell.left + cell.right) / 2) ++index;
            return std::min(index, count);
        }

        return 0;
    }

    void ReorderFileCategoryWidget(size_t widgetIndex, size_t insertIndex)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FileCategories)
            return;

        DesktopWidget& widget = widgets_[widgetIndex];
        std::wstring active = GetActiveFileCategoryId(widget);
        std::vector<std::wstring> activeKeys = GetFileCategoryKeys(widget, active);

        std::vector<std::wstring> selectedKeys;
        for (size_t i = 0; i < activeKeys.size(); ++i)
        {
            size_t itemIndex = FindItemIndexByKey(activeKeys[i]);
            if (itemIndex != static_cast<size_t>(-1) && items_[itemIndex].selected)
                selectedKeys.push_back(activeKeys[i]);
        }
        if (selectedKeys.empty()) return;

        {
            wchar_t buf[256];
            swprintf_s(buf, L"  REORDER enter: insertIdx=%zu activeCnt=%zu selCnt=%zu allCnt=%zu",
                insertIndex, activeKeys.size(), selectedKeys.size(), widget.itemKeys.size());
            DebugLog(buf);
        }

        std::unordered_set<std::wstring> selectedSet(selectedKeys.begin(), selectedKeys.end());
        auto& allKeys = widget.itemKeys;

        if (insertIndex < activeKeys.size())
        {
            size_t anchorIdx = insertIndex;
            while (anchorIdx < activeKeys.size() && selectedSet.contains(NormalizeLayoutKey(activeKeys[anchorIdx])))
                ++anchorIdx;

            if (anchorIdx < activeKeys.size())
            {
                std::wstring anchorKey = NormalizeLayoutKey(activeKeys[anchorIdx]);
                insertIndex = allKeys.size();
                for (size_t i = 0; i < allKeys.size(); ++i)
                {
                    if (NormalizeLayoutKey(allKeys[i]) == anchorKey)
                    {
                        insertIndex = i;
                        break;
                    }
                }
                {
                    wchar_t buf[256];
                    swprintf_s(buf, L"  REORDER anchor: anchorIdx=%zu anchorKey=%ls newInsertIdx=%zu",
                        anchorIdx, anchorKey.c_str(), insertIndex);
                    DebugLog(buf);
                }
            }
            else
            {
                insertIndex = allKeys.size();
                {
                    wchar_t buf[128];
                    swprintf_s(buf, L"  REORDER anchor: fell off end, insertIdx=allKeys.size()=%zu", insertIndex);
                    DebugLog(buf);
                }
            }
        }
        else
        {
            insertIndex = allKeys.size();
            {
                wchar_t buf[128];
                swprintf_s(buf, L"  REORDER insertIdx>=activeCnt, insertIdx=allKeys.size()=%zu", insertIndex);
                DebugLog(buf);
            }
        }

        size_t before = 0;
        for (size_t i = 0; i < std::min(insertIndex, allKeys.size()); ++i)
        {
            if (selectedSet.contains(NormalizeLayoutKey(allKeys[i])))
                ++before;
        }
        insertIndex -= std::min(insertIndex, before);

        {
            wchar_t buf[128];
            swprintf_s(buf, L"  REORDER beforeRemove: before=%zu finalInsertIdx=%zu", before, insertIndex);
            DebugLog(buf);
        }

        allKeys.erase(
            std::remove_if(allKeys.begin(), allKeys.end(),
                [&](const std::wstring& k) { return selectedSet.contains(NormalizeLayoutKey(k)); }),
            allKeys.end());

        insertIndex = std::min(insertIndex, allKeys.size());
        for (size_t i = 0; i < selectedKeys.size(); ++i)
        {
            size_t pos = std::min(insertIndex + i, allKeys.size());
            allKeys.insert(allKeys.begin() + static_cast<std::ptrdiff_t>(pos), selectedKeys[i]);
        }

        {
            wchar_t buf[128];
            swprintf_s(buf, L"  REORDER done: allKeys.size()=%zu", allKeys.size());
            DebugLog(buf);
        }

        LayoutItems();
        SaveLayoutSlots();
    }

    void FinishWidgetMemberReorder(size_t widgetIndex, size_t insertIndex)
    {
        if (widgetIndex >= widgets_.size())
        {
            return;
        }

        DesktopWidget& widget = widgets_[widgetIndex];
        if (widget.type == DesktopWidgetType::FileCategories)
        {
            ReorderFileCategoryWidget(widgetIndex, insertIndex);
            return;
        }

        if (widget.type != DesktopWidgetType::FolderMapping)
        {
            return;
        }

        size_t before = 0;
        for (size_t i = 0; i < std::min(insertIndex, widget.folderEntries.size()); ++i)
        {
            if (widget.folderEntries[i].selected) ++before;
        }
        insertIndex -= std::min(insertIndex, before);

        std::vector<FolderEntry> selectedEntries;
        for (size_t i = widget.folderEntries.size(); i > 0; --i)
        {
            if (widget.folderEntries[i - 1].selected)
            {
                selectedEntries.insert(selectedEntries.begin(), std::move(widget.folderEntries[i - 1]));
                widget.folderEntries.erase(widget.folderEntries.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }
        if (selectedEntries.empty()) return;

        insertIndex = std::min(insertIndex, widget.folderEntries.size());
        for (size_t i = 0; i < selectedEntries.size(); ++i)
        {
            size_t pos = std::min(insertIndex + i, widget.folderEntries.size());
            widget.folderEntries.insert(
                widget.folderEntries.begin() + static_cast<std::ptrdiff_t>(pos),
                std::move(selectedEntries[i]));
        }

        SaveLayoutSlots();
    }

    std::vector<std::wstring> GetPopupItemKeys(const DesktopWidget& widget) const
    {
        if (widget.type == DesktopWidgetType::FileCategories)
        {
            return GetFileCategoryKeys(widget, popupCategoryId_);
        }
        return widget.itemKeys;
    }

    void AddSelectedItemsToFileCategoryWidget(size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FileCategories)
        {
            return;
        }

        DesktopWidget& widget = widgets_[widgetIndex];
        std::unordered_set<std::wstring> existing;
        for (const auto& key : widget.itemKeys)
        {
            existing.insert(NormalizeLayoutKey(key));
        }

        bool changed = false;
        for (const auto& item : items_)
        {
            if (!item.selected || !IsFileCategoryCollectableItem(item))
            {
                continue;
            }
            std::wstring key = NormalizeLayoutKey(item.layoutKey);
            if (key.empty() || existing.contains(key))
            {
                continue;
            }
            widget.itemKeys.push_back(key);
            existing.insert(key);
            changed = true;
        }

        if (changed)
        {
            if (widget.activeCategoryId.empty())
            {
                widget.activeCategoryId = GetActiveFileCategoryId(widget);
            }
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
    }

    void CopyDesktopItemsToFolderMapping(size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping)
            return;

        std::vector<std::wstring> paths;
        for (const auto& item : items_)
        {
            if (!item.selected || IsProtectedDesktopIcon(item)) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
                paths.push_back(path);
        }
        if (paths.empty()) return;

        bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        std::wstring destFolder = widgets_[widgetIndex].sourceFolderPath;
        if (!destFolder.empty() && destFolder.back() != L'\\') destFolder += L'\\';

        if (altDown)
        {
            for (const auto& filePath : paths)
            {
                std::wstring name = PathFindFileNameW(filePath.c_str());
                std::wstring linkPath = destFolder + name + L".lnk";
                ComPtr<IShellLinkW> shellLink;
                if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                    IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                {
                    shellLink->SetPath(filePath.c_str());
                    ComPtr<IPersistFile> persistFile;
                    if (SUCCEEDED(shellLink.As(&persistFile)))
                        persistFile->Save(linkPath.c_str(), TRUE);
                }
            }
        }
        else
        {
            CopyFilesToFolder(paths, destFolder, false);
        }
        ReloadItems();
        RefreshFolderMappingWidget(widgetIndex);
    }

    bool CollectFilesIntoFileCategoryWidget(size_t widgetIndex, bool persist)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FileCategories)
        {
            return false;
        }

        std::unordered_set<std::wstring> existing;
        for (const auto& key : widgets_[widgetIndex].itemKeys)
        {
            existing.insert(NormalizeLayoutKey(key));
        }

        bool changed = false;
        for (const auto& item : items_)
        {
            if (!IsDesktopFileCategoryCandidate(item))
            {
                continue;
            }

            std::wstring key = NormalizeLayoutKey(item.layoutKey);
            if (key.empty() || existing.contains(key))
            {
                continue;
            }
            widgets_[widgetIndex].itemKeys.push_back(key);
            existing.insert(key);
            changed = true;
        }

        if (changed)
        {
            if (widgets_[widgetIndex].activeCategoryId.empty())
            {
                widgets_[widgetIndex].activeCategoryId = GetActiveFileCategoryId(widgets_[widgetIndex]);
            }
            widgets_[widgetIndex].scrollOffset = 0;
            if (persist)
            {
                LayoutItems();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
        return changed;
    }

    void ApplyAutoCollectFileCategoryWidgets()
    {
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (widgets_[i].type == DesktopWidgetType::FileCategories && widgets_[i].autoCollect)
            {
                CollectFilesIntoFileCategoryWidget(i, false);
            }
        }
    }

    void EnumerateFolderMappingEntries(DesktopWidget& widget)
    {
        for (auto& entry : widget.folderEntries)
        {
            if (entry.iconBitmap != nullptr)
            {
                DeleteObject(entry.iconBitmap);
            }
        }
        widget.folderEntries.clear();
        if (widget.sourceFolderPath.empty())
        {
            return;
        }

        std::wstring searchPath = widget.sourceFolderPath;
        if (!searchPath.empty() && searchPath.back() != L'\\')
        {
            searchPath += L'\\';
        }
        searchPath += L'*';

        WIN32_FIND_DATAW findData{};
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
            {
                continue;
            }

            FolderEntry entry;
            entry.name = findData.cFileName;
            entry.fullPath = widget.sourceFolderPath + L"\\" + findData.cFileName;
            entry.isDirectory = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            SHFILEINFOW info{};
            SHGetFileInfoW(entry.fullPath.c_str(), 0, &info, sizeof(info),
                SHGFI_SYSICONINDEX);
            entry.sysIconIndex = info.iIcon;

            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
            {
                entry.iconBitmap = GetHighResolutionShellIconBitmap(pidl, info.iIcon, entry.iconBitmapSize);
                ILFree(pidl);
            }

            if (entry.iconBitmap != nullptr)
            {
                ClampAlphaToColorKey(entry.iconBitmap, kTransparentKey);
                std::wstring upper = entry.name;
                for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
                if (upper.size() > 4 && upper.compare(upper.size() - 4, 4, L".LNK") == 0)
                {
                    wchar_t target[MAX_PATH]{};
                    ComPtr<IShellLinkW> shellLink;
                    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                        IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                    {
                        ComPtr<IPersistFile> persistFile;
                        if (SUCCEEDED(shellLink.As(&persistFile)) &&
                            SUCCEEDED(persistFile->Load(entry.fullPath.c_str(), STGM_READ)) &&
                            SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, 0)))
                        {
                            std::wstring t(target);
                            for (auto& c : t) c = static_cast<wchar_t>(towupper(c));
                            if (t.size() < 4 || t.compare(t.size() - 4, 4, L".EXE") != 0)
                            {
                                ApplyShortcutArrowToBitmap(entry.iconBitmap, entry.iconBitmapSize);
                            }
                        }
                    }
                }
            }

            widget.folderEntries.push_back(std::move(entry));
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);

        std::sort(widget.folderEntries.begin(), widget.folderEntries.end(),
            [](const FolderEntry& a, const FolderEntry& b) {
                if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
                return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
    }

    void AddFolderMappingWidgetAt(POINT screenPoint)
    {
        IFileDialog* pfd = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_IFileDialog, reinterpret_cast<void**>(&pfd))))
        {
            return;
        }

        DWORD options = 0;
        pfd->GetOptions(&options);
        pfd->SetOptions(options | FOS_PICKFOLDERS);

        if (FAILED(pfd->Show(hwnd_)))
        {
            pfd->Release();
            return;
        }

        IShellItem* psi = nullptr;
        if (FAILED(pfd->GetResult(&psi)))
        {
            pfd->Release();
            return;
        }

        wchar_t* folderPath = nullptr;
        psi->GetDisplayName(SIGDN_FILESYSPATH, &folderPath);
        psi->Release();
        pfd->Release();

        if (folderPath == nullptr)
        {
            return;
        }

        std::wstring path(folderPath);
        CoTaskMemFree(folderPath);

        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        GridCell cell = CellFromPoint(clientPoint);
        if (cell.pageId.empty())
        {
            return;
        }

        std::wstring title = path;
        if (!title.empty() && title.back() == L'\\')
        {
            title.pop_back();
        }
        size_t lastSep = title.find_last_of(L"\\/");
        title = (lastSep != std::wstring::npos) ? title.substr(lastSep + 1) : title;

        DesktopWidget widget;
        widget.id = L"folder-map-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
        widget.type = DesktopWidgetType::FolderMapping;
        widget.title = title;
        widget.sourceFolderPath = path;
        widget.gridCell = cell;
        widget.gridSpan = { 2, 2 };
        EnumerateFolderMappingEntries(widget);
        widgets_.push_back(std::move(widget));
        const size_t index = widgets_.size() - 1;
        SelectWidgetOnly(index);
        PlaceWidgetWithDisplacement(index, cell, { 2, 2 });
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void RefreshFolderMappingWidget(size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping)
        {
            return;
        }
        DesktopWidget& widget = widgets_[widgetIndex];
        EnumerateFolderMappingEntries(widget);
        widget.scrollOffset = 0;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool OpenFolderMappingSource(size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
            widgets_[widgetIndex].sourceFolderPath.empty())
        {
            return false;
        }

        ShellExecuteW(hwnd_, L"open", widgets_[widgetIndex].sourceFolderPath.c_str(),
            nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }

    void ShowFolderEntryContextMenu(POINT screenPoint, size_t widgetIndex, size_t memberIndex)
    {
        if (widgetIndex >= widgets_.size() || memberIndex >= widgets_[widgetIndex].folderEntries.size())
        {
            return;
        }

        const std::wstring& fullPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            return;
        }

        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD child = nullptr;
        if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
            reinterpret_cast<void**>(&parentFolder), &child)))
        {
            ILFree(pidl);
            return;
        }

        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = parentFolder->GetUIObjectOf(
            hwnd_, 1, &child, IID_IContextMenu, nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        parentFolder->Release();
        if (FAILED(hr) || !contextMenu)
        {
            ILFree(pidl);
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            ILFree(pidl);
            return;
        }

        constexpr UINT kFirstCmd = 1;
        constexpr UINT kLastCmd = 0x7FFF;
        hr = contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd,
            CMF_NORMAL | CMF_CANRENAME);
        if (FAILED(hr))
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            ILFree(pidl);
            return;
        }

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);

        if (command >= kFirstCmd && command <= kLastCmd)
        {
            UINT commandOffset = command - kFirstCmd;
            wchar_t menuText[128]{};
            bool renameCommand = IsShellRenameCommand(contextMenu.Get(), commandOffset);
            if (!renameCommand &&
                GetMenuStringW(menu, command, menuText, static_cast<int>(std::size(menuText)), MF_BYCOMMAND) > 0)
            {
                renameCommand = StrStrIW(menuText, L"重命名") != nullptr || StrStrIW(menuText, L"Rename") != nullptr;
            }

            if (renameCommand)
            {
                DestroyMenu(menu);
                RestoreDesktopWindowLayer();
                ILFree(pidl);
                BeginRenameFolderEntry(widgetIndex, memberIndex);
                return;
            }

            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
            invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
            invoke.nShow = SW_SHOWNORMAL;
            contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
            RefreshFolderMappingWidget(widgetIndex);
        }
        else
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
        }

        ILFree(pidl);
    }

    bool InvokeSelectedFolderEntryVerb(const char* verb)
    {
        for (size_t wi = 0; wi < widgets_.size(); ++wi)
        {
            if (widgets_[wi].type != DesktopWidgetType::FolderMapping)
                continue;
            for (size_t mi = 0; mi < widgets_[wi].folderEntries.size(); ++mi)
            {
                if (!widgets_[wi].folderEntries[mi].selected)
                    continue;
                const std::wstring& fullPath = widgets_[wi].folderEntries[mi].fullPath;
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                    continue;
                IShellFolder* parentFolder = nullptr;
                PCUITEMID_CHILD child = nullptr;
                if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
                    reinterpret_cast<void**>(&parentFolder), &child)))
                {
                    ILFree(pidl);
                    continue;
                }
                ComPtr<IContextMenu> contextMenu;
                HRESULT hr = parentFolder->GetUIObjectOf(
                    hwnd_, 1, &child, IID_IContextMenu, nullptr,
                    reinterpret_cast<void**>(contextMenu.GetAddressOf()));
                parentFolder->Release();
                if (SUCCEEDED(hr) && contextMenu)
                {
                    CMINVOKECOMMANDINFO cmi{};
                    cmi.cbSize = sizeof(cmi);
                    cmi.lpVerb = verb;
                    cmi.nShow = SW_NORMAL;
                    contextMenu->InvokeCommand(&cmi);
                }
                ILFree(pidl);
                return true;
            }
        }
        return false;
    }

    bool DeleteSelectedFolderEntries()
    {
        std::wstring fromStr;
        for (size_t wi = 0; wi < widgets_.size(); ++wi)
        {
            if (widgets_[wi].type != DesktopWidgetType::FolderMapping)
                continue;
            auto& entries = widgets_[wi].folderEntries;
            for (size_t i = entries.size(); i > 0; --i)
            {
                if (entries[i - 1].selected)
                {
                    fromStr += entries[i - 1].fullPath;
                    fromStr += L'\0';
                    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i - 1));
                }
            }
        }
        if (fromStr.empty()) return false;
        fromStr += L'\0';

        SHFILEOPSTRUCTW op{};
        op.hwnd = hwnd_;
        op.wFunc = FO_DELETE;
        op.pFrom = fromStr.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
        SHFileOperationW(&op);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    void PasteFromClipboardAtCursor()
    {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd_, &cursor);
        DesktopHit hit = HitTestDesktop(cursor);
        if (hit.widgetIndex < widgets_.size() &&
            widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
            (hit.kind == DesktopHitKind::WidgetMember ||
             hit.kind == DesktopHitKind::WidgetContent ||
             hit.kind == DesktopHitKind::Widget))
        {
            ComPtr<IDataObject> dataObject;
            if (SUCCEEDED(OleGetClipboard(&dataObject)) && dataObject)
            {
                DropExternalFilesToFolderMapping(hit.widgetIndex, dataObject.Get(), 0, nullptr);
            }
        }
        else
        {
            InvokeDesktopBackgroundVerb("paste");
        }
    }

    void ShowCustomItemContextMenu(POINT screenPoint)
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const UINT singleItemFlag = selectedCount_ == 1 ? MF_STRING : (MF_STRING | MF_GRAYED);
        const UINT fileCommandFlag = CanUseSelectedFileCommands() ? MF_STRING : (MF_STRING | MF_GRAYED);
        const UINT renameFlag = selectedCount_ == 1 && CanUseSelectedFileCommands() ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(menu, singleItemFlag, kContextOpenCommand, L"打开");
        AppendMenuW(menu, renameFlag, kContextRenameCommand, L"重命名");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, fileCommandFlag, kContextCutCommand, L"剪切");
        AppendMenuW(menu, fileCommandFlag, kContextCopyCommand, L"复制");
        AppendMenuW(menu, fileCommandFlag, kContextDeleteCommand, L"删除");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");

        menuIconPool_.clear();
        SetMenuItemIcon(menu, kContextOpenCommand, L"");
        SetMenuItemIcon(menu, kContextRenameCommand, L"");
        SetMenuItemIcon(menu, kContextCutCommand, L"");
        SetMenuItemIcon(menu, kContextCopyCommand, L"");
        SetMenuItemIcon(menu, kContextDeleteCommand, L"");
        SetMenuItemIcon(menu, kContextMoreCommand, L"");

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);
        DestroyMenu(menu);
        for (HBITMAP bmp : menuIconPool_) { DeleteObject(bmp); }
        menuIconPool_.clear();
        RestoreDesktopWindowLayer();

        switch (command)
        {
        case kContextOpenCommand:
            OpenSelected();
            break;
        case kContextRenameCommand:
            BeginRenameSelected();
            break;
        case kContextCutCommand:
            InvokeSelectedShellVerb("cut");
            break;
        case kContextCopyCommand:
            InvokeSelectedShellVerb("copy");
            break;
        case kContextDeleteCommand:
            InvokeSelectedShellVerb("delete");
            break;
        case kContextMoreCommand:
            ShowShellContextMenu(screenPoint);
            break;
        default:
            break;
        }
    }

    void ShowCustomWidgetContextMenu(POINT screenPoint, size_t widgetIndex)
    {
        if (widgetIndex >= widgets_.size())
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        if (widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping)
        {
            AppendNewSubmenuForFolder(menu, widgets_[widgetIndex].sourceFolderPath);
        }
        if (widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kContextPasteCommand, L"粘贴");
            AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextWidgetRename, L"重命名");
        if (widgets_[widgetIndex].type == DesktopWidgetType::Collection)
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetOpen, L"打开全部");
        }
        else if (widgets_[widgetIndex].type == DesktopWidgetType::FileCategories)
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetManualCollect, L"立即收集");
            AppendMenuW(menu, MF_STRING | (widgets_[widgetIndex].autoCollect ? MF_CHECKED : 0), kContextWidgetToggleAutoCollect, L"自动收集");
            AppendMenuW(menu, MF_STRING | (widgets_[widgetIndex].listMode ? MF_CHECKED : 0), kContextWidgetToggleListMode, L"列表显示");
            HMENU sortMenu = CreatePopupMenu();
            if (sortMenu != nullptr)
            {
                AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByName, L"名称");
                AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByType, L"类型");
                AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByDate, L"修改日期");
                AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
            }
        }
        else if (widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, L"打开文件夹");
            AppendMenuW(menu, MF_STRING | (widgets_[widgetIndex].listMode ? MF_CHECKED : 0), kContextWidgetToggleListMode, L"列表显示");
            HMENU sortMenu = CreatePopupMenu();
            if (sortMenu != nullptr)
            {
                AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByName, L"名称");
                AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByType, L"类型");
                AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByDate, L"修改日期");
                AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextWidgetDelete, L"删除组件");

        menuIconPool_.clear();
        SetMenuItemIcon(menu, kContextPasteCommand, L"");
        SetMenuItemIcon(menu, kContextMoreCommand, L"");
        SetMenuItemIcon(menu, kContextWidgetRename, L"");
        SetMenuItemIcon(menu, kContextWidgetOpen, L"");
        SetMenuItemIcon(menu, kContextWidgetManualCollect, L"");
        SetMenuItemIcon(menu, kContextWidgetToggleAutoCollect, L"");
        SetMenuItemIcon(menu, kContextWidgetToggleListMode, L"");
        SetMenuItemIcon(menu, kContextWidgetOpenFolder, L"");
        SetMenuItemIcon(menu, kContextWidgetDelete, L"");

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);
        DestroyMenu(menu);
        for (HBITMAP bmp : menuIconPool_) { DeleteObject(bmp); }
        menuIconPool_.clear();
        RestoreDesktopWindowLayer();
        newMenuContextMenu_.Reset();

        switch (command)
        {
        case kContextWidgetOpen:
        {
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            OpenCollectionPopupAt(widgetIndex, clientPoint, L"");
            break;
        }
        case kContextWidgetRename:
            SelectWidgetOnly(widgetIndex);
            BeginRenameSelected();
            break;
        case kContextWidgetManualCollect:
            if (!CollectFilesIntoFileCategoryWidget(widgetIndex, true))
            {
                MessageBeep(MB_ICONINFORMATION);
            }
            break;
        case kContextWidgetToggleAutoCollect:
            widgets_[widgetIndex].autoCollect = !widgets_[widgetIndex].autoCollect;
            if (widgets_[widgetIndex].autoCollect)
            {
                CollectFilesIntoFileCategoryWidget(widgetIndex, false);
            }
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
            break;
        case kContextWidgetToggleListMode:
            widgets_[widgetIndex].listMode = !widgets_[widgetIndex].listMode;
            widgets_[widgetIndex].scrollOffset = 0;
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
            break;
        case kContextWidgetOpenFolder:
            OpenFolderMappingSource(widgetIndex);
            break;
        case kContextWidgetDelete:
            DeleteWidget(widgetIndex);
            break;
        case kContextPasteCommand:
            if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping)
            {
                ComPtr<IDataObject> dataObject;
                if (SUCCEEDED(OleGetClipboard(&dataObject)) && dataObject)
                    DropExternalFilesToFolderMapping(widgetIndex, dataObject.Get(), 0, nullptr);
            }
            break;
        case kContextMoreCommand:
            if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping)
            {
                std::wstring folderPath = widgets_[widgetIndex].sourceFolderPath;
                if (!folderPath.empty())
                {
                    POINT pt{};
                    GetCursorPos(&pt);
                    ShowShellContextMenuForPath(folderPath, pt);
                }
            }
            break;
        case kContextNewMenu:
            if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
                !widgets_[widgetIndex].sourceFolderPath.empty())
            {
                ShowNewMenuAndInvoke(screenPoint, widgets_[widgetIndex].sourceFolderPath);
                RefreshFolderMappingWidget(widgetIndex);
            }
            break;
        case kContextWidgetSortByName:
            SortWidgetContents(widgetIndex, 0);
            break;
        case kContextWidgetSortByType:
            SortWidgetContents(widgetIndex, 1);
            break;
        case kContextWidgetSortByDate:
            SortWidgetContents(widgetIndex, 2);
            break;
        default:
            break;
        }
    }

    void SortWidgetContents(size_t widgetIndex, int mode)
    {
        if (widgetIndex >= widgets_.size()) return;
        DesktopWidget& w = widgets_[widgetIndex];

        if (w.type == DesktopWidgetType::FolderMapping)
        {
            std::sort(w.folderEntries.begin(), w.folderEntries.end(),
                [mode](const FolderEntry& a, const FolderEntry& b) {
                    if (a.isDirectory != b.isDirectory) return a.isDirectory;
                    int cmp = 0;
                    if (mode == 0) cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                    else if (mode == 1)
                    {
                        std::wstring extA = PathFindExtensionW(a.name.c_str());
                        std::wstring extB = PathFindExtensionW(b.name.c_str());
                        cmp = _wcsicmp(extA.c_str(), extB.c_str());
                        if (cmp == 0) cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                    }
                    else if (mode == 2)
                    {
                        WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                        if (GetFileAttributesExW(a.fullPath.c_str(), GetFileExInfoStandard, &da) &&
                            GetFileAttributesExW(b.fullPath.c_str(), GetFileExInfoStandard, &db))
                        {
                            int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                            if (timeCmp != 0) return timeCmp < 0;
                        }
                        cmp = _wcsicmp(a.name.c_str(), b.name.c_str());
                    }
                    return cmp < 0;
                });
            RefreshFolderMappingWidget(widgetIndex);
        }
        else if (w.type == DesktopWidgetType::FileCategories)
        {
            std::wstring active = GetActiveFileCategoryId(w);
            std::vector<std::wstring> keys = GetFileCategoryKeys(w, active);
            std::sort(keys.begin(), keys.end(),
                [this, mode](const std::wstring& ka, const std::wstring& kb) {
                    size_t ia = FindItemIndexByKey(ka);
                    size_t ib = FindItemIndexByKey(kb);
                    if (ia == static_cast<size_t>(-1) || ib == static_cast<size_t>(-1)) return false;
                    int cmp = 0;
                    if (mode == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    else if (mode == 1)
                    {
                        cmp = _wcsicmp(items_[ia].typeName.c_str(), items_[ib].typeName.c_str());
                        if (cmp == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    }
                    else if (mode == 2)
                    {
                        WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                        if (GetFileAttributesExW(items_[ia].parsingName.c_str(), GetFileExInfoStandard, &da) &&
                            GetFileAttributesExW(items_[ib].parsingName.c_str(), GetFileExInfoStandard, &db))
                        {
                            int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                            if (timeCmp != 0) return timeCmp < 0;
                        }
                        cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    }
                    return cmp < 0;
                });
            // Rebuild itemKeys: preserve non-active tab items, then sorted active tab
            if (active == L"all")
            {
                w.itemKeys = std::move(keys);
            }
            else
            {
                std::vector<std::wstring> nonActive;
                std::unordered_set<std::wstring> seenNonActive;
                for (const auto& rawKey : w.itemKeys)
                {
                    size_t idx = FindItemIndexByKey(rawKey);
                    if (idx == static_cast<size_t>(-1)) continue;
                    if (GetFileCategoryId(items_[idx]) != active)
                    {
                        std::wstring nk = NormalizeLayoutKey(items_[idx].layoutKey);
                        if (seenNonActive.insert(nk).second)
                            nonActive.push_back(nk);
                    }
                }
                w.itemKeys = std::move(nonActive);
                for (const auto& k : keys)
                    w.itemKeys.push_back(k);
            }
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
    }

    void ShowCustomBackgroundContextMenu(POINT screenPoint)
    {
        lastContextMenuScreenPoint_ = screenPoint;

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, kContextPasteCommand, L"粘贴");
        AppendNewSubmenu(menu);
        AppendMenuW(menu, MF_STRING, kContextRefreshCommand, L"刷新");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU sortMenu = CreatePopupMenu();
        if (sortMenu != nullptr)
        {
            AppendMenuW(sortMenu, MF_STRING, kContextSortByNameCommand, L"名称");
            AppendMenuW(sortMenu, MF_STRING, kContextSortByTypeCommand, L"类型");
            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
        }

        HMENU gridMenu = CreatePopupMenu();
        if (gridMenu != nullptr)
        {
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            const GridPage* page = GridPageFromPoint(clientPoint);
            wchar_t gridLabel[64]{};
            int cols = page != nullptr ? page->columns : 0;
            int rows = page != nullptr ? page->rows : 0;
            swprintf_s(gridLabel, L"行列调整（%d列 × %d行）", cols, rows);
            AppendMenuW(gridMenu, MF_STRING, kContextGridAddRow, L"增加行");
            AppendMenuW(gridMenu, MF_STRING, kContextGridRemoveRow, L"减少行");
            AppendMenuW(gridMenu, MF_STRING, kContextGridAddColumn, L"增加列");
            AppendMenuW(gridMenu, MF_STRING, kContextGridRemoveColumn, L"减少列");
            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(gridMenu), gridLabel);
        }

        HMENU widgetMenu = CreatePopupMenu();
        if (widgetMenu != nullptr)
        {
            AppendMenuW(widgetMenu, MF_STRING, kContextAddCollectionWidget, L"集合");
            AppendMenuW(widgetMenu, MF_STRING, kContextAddFileCategoryWidget, L"桌面文件分类");
            AppendMenuW(widgetMenu, MF_STRING, kContextAddFolderMappingWidget, L"文件夹映射");
            AppendMenuW(widgetMenu, MF_SEPARATOR, 0, nullptr);

            // Add Lua widget scripts
            std::vector<std::wstring> luaWidgets = WidgetEngine::ListAvailable();
            for (size_t li = 0; li < luaWidgets.size(); ++li)
            {
                std::wstring label = luaWidgets[li];
                if (label.size() > 4 && label.substr(label.size() - 4) == L".lua")
                    label = label.substr(0, label.size() - 4);
                AppendMenuW(widgetMenu, MF_STRING,
                    kContextAddLuaWidgetFirst + static_cast<UINT>(li), label.c_str());
            }

            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(widgetMenu), L"添加组件");
        }

        HMENU zoomMenu = CreatePopupMenu();
        if (zoomMenu != nullptr)
        {
            const int presets[] = {50, 70, 80, 90, 100, 110, 120, 130, 150, 200};
            for (int pct : presets)
            {
                wchar_t label[16]{};
                swprintf_s(label, L"%d%%", pct);
                UINT flags = MF_STRING;
                if (static_cast<int>(gapScale_ * 100) == pct)
                {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(zoomMenu, flags, kContextZoomPresetFirst + static_cast<UINT>(pct), label);
            }
            AppendMenuW(zoomMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(zoomMenu, MF_STRING, kContextZoomIncrease, L"放大 (+10%)");
            AppendMenuW(zoomMenu, MF_STRING, kContextZoomDecrease, L"缩小 (-10%)");
            wchar_t zoomLabel[32]{};
            swprintf_s(zoomLabel, L"缩放：%d%%", static_cast<int>(gapScale_ * 100));
            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(zoomMenu), zoomLabel);
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextThisDisplayFirstCommand, L"当前显示器显示首屏");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextSettingsCommand, L"设置");

        menuIconPool_.clear();
        SetMenuItemIcon(menu, kContextNewMenu, L"");
        SetMenuItemIcon(menu, kContextRefreshCommand, L"");
        SetMenuItemIcon(menu, kContextPasteCommand, L"");
        SetMenuItemIcon(menu, kContextMoreCommand, L"");
        if (sortMenu)
        {
            SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu), L"");
            SetMenuItemIcon(sortMenu, kContextSortByNameCommand, L"");
            SetMenuItemIcon(sortMenu, kContextSortByTypeCommand, L"");
        }
        if (gridMenu)
        {
            SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(gridMenu), L"");
            SetMenuItemIcon(gridMenu, kContextGridAddRow, L"");
            SetMenuItemIcon(gridMenu, kContextGridRemoveRow, L"");
            SetMenuItemIcon(gridMenu, kContextGridAddColumn, L"");
            SetMenuItemIcon(gridMenu, kContextGridRemoveColumn, L"");
        }
        if (widgetMenu)
        {
            SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(widgetMenu), L"");
            SetMenuItemIcon(widgetMenu, kContextAddCollectionWidget, L"");
            SetMenuItemIcon(widgetMenu, kContextAddFileCategoryWidget, L"");
            SetMenuItemIcon(widgetMenu, kContextAddFolderMappingWidget, L"");
        }
        if (zoomMenu)
        {
            SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(zoomMenu), L"");
            SetMenuItemIcon(zoomMenu, kContextZoomIncrease, L"");
            SetMenuItemIcon(zoomMenu, kContextZoomDecrease, L"");
        }
        SetMenuItemIcon(menu, kContextThisDisplayFirstCommand, L"");
        SetMenuItemIcon(menu, kContextSettingsCommand, L"");

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);
        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        newMenuContextMenu_.Reset();
        for (HBITMAP bmp : menuIconPool_) { DeleteObject(bmp); }
        menuIconPool_.clear();

        if (command >= kContextZoomPresetFirst && command <= kContextZoomPresetFirst + 150)
        {
            SetZoom(static_cast<float>(command - kContextZoomPresetFirst) / 100.0f);
        }
        else switch (command)
        {
        case kContextRefreshCommand:
            ReloadItems();
            break;
        case kContextSortByNameCommand:
            SortIconsByName();
            break;
        case kContextSortByTypeCommand:
            SortIconsByType();
            break;
        case kContextGridAddRow:
            AdjustGridRows(1);
            break;
        case kContextGridRemoveRow:
            AdjustGridRows(-1);
            break;
        case kContextGridAddColumn:
            AdjustGridColumns(1);
            break;
        case kContextGridRemoveColumn:
            AdjustGridColumns(-1);
            break;
        case kContextZoomIncrease:
            AdjustZoom(+0.1f);
            break;
        case kContextZoomDecrease:
            AdjustZoom(-0.1f);
            break;
        case kContextThisDisplayFirstCommand:
            SetFirstPageMonitorFromPoint(screenPoint);
            break;
        case kContextSettingsCommand:
            if (settingsWindow_)
                settingsWindow_->Show();
            break;
        case kContextAddCollectionWidget:
            AddCollectionWidgetAt(screenPoint);
            break;
        case kContextAddFileCategoryWidget:
            AddFileCategoryWidgetAt(screenPoint);
            break;
        case kContextAddFolderMappingWidget:
            AddFolderMappingWidgetAt(screenPoint);
            break;
        case kContextPasteCommand:
            InvokeDesktopBackgroundVerb("paste");
            break;
        case kContextMoreCommand:
            ShowDesktopBackgroundContextMenu(screenPoint);
            break;
        case kContextNewMenu:
        {
            wchar_t desktopPath[MAX_PATH]{};
            if (SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
            {
                ShowNewMenuAndInvoke(screenPoint, desktopPath);
                ReloadItems();
            }
            break;
        }
        default:
            if (command >= kContextAddLuaWidgetFirst)
            {
                size_t idx = static_cast<size_t>(command - kContextAddLuaWidgetFirst);
                auto scripts = WidgetEngine::ListAvailable();
                if (idx < scripts.size())
                {
                    AddLuaWidgetAt(screenPoint, scripts[idx]);
                }
            }
            break;
        }
    }

    void SetFirstPageMonitorFromPoint(POINT screenPoint)
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* page = GridPageFromPoint(clientPoint);
        if (page == nullptr || page->monitorId.empty())
        {
            return;
        }

        firstPageMonitorId_ = page->monitorId;
        pageOffset_ = 0;
        ApplyPageMapping();
        ApplySavedGridDimensions();
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool IsShellRenameCommand(IContextMenu* contextMenu, UINT commandOffset) const
    {
        if (contextMenu == nullptr)
        {
            return false;
        }

        wchar_t verbW[128]{};
        if (SUCCEEDED(contextMenu->GetCommandString(
                commandOffset,
                GCS_VERBW,
                nullptr,
                reinterpret_cast<LPSTR>(verbW),
                static_cast<UINT>(std::size(verbW)))) &&
            lstrcmpiW(verbW, L"rename") == 0)
        {
            return true;
        }

        char verbA[128]{};
        if (SUCCEEDED(contextMenu->GetCommandString(
                commandOffset,
                GCS_VERBA,
                nullptr,
                verbA,
                static_cast<UINT>(std::size(verbA)))) &&
            lstrcmpiA(verbA, "rename") == 0)
        {
            return true;
        }

        return false;
    }

    void AppendNewSubmenu(HMENU menu)
    {
        AppendMenuW(menu, MF_STRING, kContextNewMenu, L"新建");
    }

    void AppendNewSubmenuForFolder(HMENU menu, const std::wstring& folderPath)
    {
        (void)folderPath;
        AppendMenuW(menu, MF_STRING, kContextNewMenu, L"新建");
    }

    void ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir)
    {
        ComPtr<IContextMenu> ctxMenu;
        if (FAILED(CoCreateInstance(CLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER,
            IID_IContextMenu, reinterpret_cast<void**>(ctxMenu.GetAddressOf()))) || !ctxMenu)
            return;

        ComPtr<IShellExtInit> shellExtInit;
        if (FAILED(ctxMenu.As(&shellExtInit)) || !shellExtInit)
            return;

        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(targetDir.c_str(), nullptr, &pidl, 0, nullptr)) || pidl == nullptr)
            return;

        HRESULT hr = shellExtInit->Initialize(pidl, nullptr, 0);
        ILFree(pidl);
        if (FAILED(hr)) return;

        HMENU tmpMenu = CreatePopupMenu();
        if (tmpMenu == nullptr) return;
        hr = ctxMenu->QueryContextMenu(tmpMenu, 0, 1, 0x7FFF, CMF_NORMAL);
        if (FAILED(hr)) { DestroyMenu(tmpMenu); return; }

        HMENU newSub = GetSubMenu(tmpMenu, 0);
        if (newSub == nullptr) { DestroyMenu(tmpMenu); return; }

        ctxMenu.As(&newMenuContextMenu_);

        SetForegroundWindow(hwnd_);
        UINT cmd = TrackPopupMenuEx(newSub, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_LEFTBUTTON,
            screenPoint.x, screenPoint.y, hwnd_, nullptr);
        newMenuContextMenu_.Reset();

        if (cmd != 0 && cmd >= 1)
        {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(cmd - 1);
            invoke.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
            invoke.nShow = SW_SHOWNORMAL;
            ctxMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        }

        // Clean up other items in tmpMenu
        for (int i = GetMenuItemCount(tmpMenu) - 1; i >= 0; --i)
        {
            if (GetSubMenu(tmpMenu, i) == nullptr)
                RemoveMenu(tmpMenu, i, MF_BYPOSITION);
        }
        DestroyMenu(tmpMenu);
    }

    void ShowShellContextMenu(POINT screenPoint)
    {
        auto pidls = SelectedChildPidls();
        if (pidls.empty())
        {
            return;
        }

        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        constexpr UINT kFirstCommand = 1;
        constexpr UINT kLastCommand = 0x7FFF;
        hr = contextMenu->QueryContextMenu(menu, 0, kFirstCommand, kLastCommand, CMF_NORMAL | CMF_CANRENAME);
        if (FAILED(hr))
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            return;
        }

        contextMenu.As(&activeContextMenu2_);
        contextMenu.As(&activeContextMenu3_);

        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);

        activeContextMenu2_.Reset();
        activeContextMenu3_.Reset();

        if (command != 0)
        {
            UINT commandOffset = command - kFirstCommand;
            wchar_t menuText[128]{};
            bool renameCommand = IsShellRenameCommand(contextMenu.Get(), commandOffset);
            if (!renameCommand &&
                GetMenuStringW(menu, command, menuText, static_cast<int>(std::size(menuText)), MF_BYCOMMAND) > 0)
            {
                renameCommand = StrStrIW(menuText, L"重命名") != nullptr || StrStrIW(menuText, L"Rename") != nullptr;
            }

            if (renameCommand)
            {
                DestroyMenu(menu);
                RestoreDesktopWindowLayer();
                BeginRenameSelected();
                return;
            }

            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
            invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
            ReloadItems();
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
    }

    void ShowShellContextMenuForPath(const std::wstring& folderPath, POINT screenPoint)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &pidl, 0, nullptr)))
            return;

        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD child = nullptr;
        if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
            reinterpret_cast<void**>(&parentFolder), &child)))
        {
            ILFree(pidl);
            return;
        }

        IShellFolder* folder = nullptr;
        HRESULT bindHr = parentFolder->BindToObject(child, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&folder));
        parentFolder->Release();
        if (FAILED(bindHr) || folder == nullptr)
        {
            ILFree(pidl);
            return;
        }

        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = folder->CreateViewObject(hwnd_, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        folder->Release();
        if (FAILED(hr) || !contextMenu)
        {
            ILFree(pidl);
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) { ILFree(pidl); return; }

        constexpr UINT kFirstCmd = 1;
        constexpr UINT kLastCmd = 0x7FFF;
        contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd, CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME);

        UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x, screenPoint.y, hwnd_, nullptr);

        if (command != 0)
        {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCmd);
            invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCmd);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);
    }

    void ShowDesktopBackgroundContextMenu(POINT screenPoint)
    {
        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        constexpr UINT kFirstCommand = 1;
        constexpr UINT kLastCommand = 0x7FFF;
        hr = contextMenu->QueryContextMenu(menu, 0, kFirstCommand, kLastCommand, CMF_NORMAL);
        if (FAILED(hr))
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            return;
        }

        contextMenu.As(&activeContextMenu2_);
        contextMenu.As(&activeContextMenu3_);

        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);

        activeContextMenu2_.Reset();
        activeContextMenu3_.Reset();

        if (command != 0)
        {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCommand);
            invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCommand);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
            ReloadItems();
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
    }

    void InvokeSelectedShellVerb(const char* verb)
    {
        auto pidls = SelectedChildPidls();
        if (pidls.empty())
        {
            return;
        }

        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        CMINVOKECOMMANDINFO invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.hwnd = hwnd_;
        invoke.lpVerb = verb;
        invoke.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(contextMenu->InvokeCommand(&invoke)))
        {
            ReloadItems();
        }
    }

    void InvokeDesktopBackgroundVerb(const char* verb)
    {
        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        CMINVOKECOMMANDINFO invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.hwnd = hwnd_;
        invoke.lpVerb = verb;
        invoke.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(contextMenu->InvokeCommand(&invoke)))
        {
            ReloadItems();
        }
    }

    RECT GetFolderEntryRenameRect(size_t widgetIndex, size_t memberIndex) const
    {
        if (widgetIndex >= widgets_.size() || memberIndex >= widgets_[widgetIndex].folderEntries.size())
        {
            return {};
        }

        const DesktopWidget& widget = widgets_[widgetIndex];
        RECT itemRect = GetFolderMappingItemRect(widget, memberIndex);
        if (widget.listMode)
        {
            const int itemH = std::max<int>(1, static_cast<int>(itemRect.bottom - itemRect.top));
            const int iconSz = std::min(32, itemH - 4);
            return MakeRect(itemRect.left + 4 + iconSz + 6, itemRect.top + 5, itemRect.right - 6, itemRect.bottom - 5);
        }

        return GetItemTextRect(itemRect, true);
    }

    void BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex)
    {
        if (renameEdit_ != nullptr ||
            widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
            memberIndex >= widgets_[widgetIndex].folderEntries.size())
        {
            return;
        }

        SelectFolderEntryOnly(widgetIndex, memberIndex);
        renamingFolderEntry_ = true;
        renameFolderWidgetIndex_ = widgetIndex;
        renameFolderEntryIndex_ = memberIndex;

        RECT rect = GetFolderEntryRenameRect(widgetIndex, memberIndex);
        InflateRect(&rect, 2, 2);
        RECT screenRect = rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

        const bool listMode = widgets_[widgetIndex].listMode;
        DWORD style = WS_POPUP | WS_VISIBLE | ES_AUTOVSCROLL;
        style |= listMode ? ES_LEFT : (ES_MULTILINE | ES_CENTER | ES_WANTRETURN);
        renameEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EDIT",
            widgets_[widgetIndex].folderEntries[memberIndex].name.c_str(),
            style,
            screenRect.left,
            screenRect.top,
            screenRect.right - screenRect.left,
            screenRect.bottom - screenRect.top,
            hwnd_,
            nullptr,
            instance_,
            nullptr);
        if (renameEdit_ == nullptr)
        {
            renamingFolderEntry_ = false;
            renameFolderWidgetIndex_ = static_cast<size_t>(-1);
            renameFolderEntryIndex_ = static_cast<size_t>(-1);
            return;
        }

        if (renameFont_ != nullptr)
        {
            DeleteObject(renameFont_);
        }
        renameFont_ = CreateFontW(
            -15,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(renameFont_ != nullptr ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        SetWindowSubclass(renameEdit_, &SnowDesktopApp::RenameEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        SetFocus(renameEdit_);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void CommitFolderEntryRename(const std::wstring& newName, bool cancel)
    {
        size_t widgetIndex = renameFolderWidgetIndex_;
        size_t memberIndex = renameFolderEntryIndex_;
        renamingFolderEntry_ = false;
        renameFolderWidgetIndex_ = static_cast<size_t>(-1);
        renameFolderEntryIndex_ = static_cast<size_t>(-1);

        if (cancel ||
            widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
            memberIndex >= widgets_[widgetIndex].folderEntries.size() ||
            newName.empty() ||
            newName == widgets_[widgetIndex].folderEntries[memberIndex].name)
        {
            return;
        }

        PIDLIST_ABSOLUTE pidl = nullptr;
        const std::wstring oldPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
        if (FAILED(SHParseDisplayName(oldPath.c_str(), nullptr, &pidl, 0, nullptr)))
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }

        IShellFolder* parentFolder = nullptr;
        PCUITEMID_CHILD child = nullptr;
        HRESULT hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&parentFolder), &child);
        if (SUCCEEDED(hr) && parentFolder != nullptr)
        {
            PITEMID_CHILD newChild = nullptr;
            hr = parentFolder->SetNameOf(hwnd_, child, newName.c_str(), SHGDN_NORMAL, &newChild);
            if (newChild != nullptr)
            {
                ILFree(newChild);
            }
            parentFolder->Release();
        }
        ILFree(pidl);

        if (FAILED(hr))
        {
            MessageBeep(MB_ICONWARNING);
            return;
        }

        RefreshFolderMappingWidget(widgetIndex);
    }

    void BeginRenameSelected()
    {
        if (renameEdit_ != nullptr)
        {
            return;
        }

        if (selectedWidgetIndex_ < widgets_.size())
        {
            renamingWidget_ = true;
            renameIndex_ = selectedWidgetIndex_;
            RECT frame = GetWidgetFrameRect(widgets_[renameIndex_]);
            RECT handle = GetWidgetMoveHandleRect(widgets_[renameIndex_]);
            const int editHeight = std::max(40, static_cast<int>(handle.bottom - handle.top) * 2);
            RECT rect = MakeRect(frame.left + 4, handle.top, frame.right - 4, handle.top + editHeight);
            InflateRect(&rect, 2, 2);
            RECT screenRect = rect;
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);
            renameEdit_ = CreateWindowExW(
                WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                L"EDIT",
                widgets_[renameIndex_].title.c_str(),
                WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
                screenRect.left,
                screenRect.top,
                screenRect.right - screenRect.left,
                screenRect.bottom - screenRect.top,
                hwnd_,
                nullptr,
                instance_,
                nullptr);
            if (renameEdit_ == nullptr)
            {
                renamingWidget_ = false;
                return;
            }
            if (renameFont_ != nullptr)
            {
                DeleteObject(renameFont_);
            }
            renameFont_ = CreateFontW(
                -15,
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI");
            SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(renameFont_ != nullptr ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
            SetWindowSubclass(renameEdit_, &SnowDesktopApp::RenameEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
            SetFocus(renameEdit_);
            return;
        }

        size_t folderWidget = static_cast<size_t>(-1);
        size_t folderMember = static_cast<size_t>(-1);
        if (FindSingleSelectedFolderEntry(folderWidget, folderMember))
        {
            BeginRenameFolderEntry(folderWidget, folderMember);
            return;
        }

        if (selectedCount_ != 1)
        {
            return;
        }

        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i].selected)
            {
                continue;
            }

            renameIndex_ = i;
            RECT itemBounds = items_[i].bounds;
            if (IsRectEmptyRect(itemBounds))
            {
                itemBounds = GetVisibleCollectionItemBounds(i);
            }
            RECT rect = IsRectEmptyRect(itemBounds) ? GetItemTextRect(items_[i], true) : GetItemTextRect(itemBounds, true);
            InflateRect(&rect, 2, 2);
            RECT screenRect = rect;
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);
            renameEdit_ = CreateWindowExW(
                WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                L"EDIT",
                items_[i].name.c_str(),
                WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
                screenRect.left,
                screenRect.top,
                screenRect.right - screenRect.left,
                screenRect.bottom - screenRect.top,
                hwnd_,
                nullptr,
                instance_,
                nullptr);

            if (renameEdit_ == nullptr)
            {
                return;
            }

            if (renameFont_ != nullptr)
            {
                DeleteObject(renameFont_);
            }
            renameFont_ = CreateFontW(
                -15,
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI");
            SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(renameFont_ != nullptr ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
            SetWindowSubclass(renameEdit_, &SnowDesktopApp::RenameEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SetWindowPos(
                renameEdit_,
                HWND_TOPMOST,
                screenRect.left,
                screenRect.top,
                screenRect.right - screenRect.left,
                screenRect.bottom - screenRect.top,
                SWP_SHOWWINDOW);
            SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
            SetFocus(renameEdit_);
            return;
        }
    }

    void CommitRename(bool cancel)
    {
        if (renameEdit_ == nullptr)
        {
            return;
        }

        HWND edit = renameEdit_;
        renameEdit_ = nullptr;
        RemoveWindowSubclass(edit, &SnowDesktopApp::RenameEditSubclassProc, 1);

        std::wstring newName;
        if (!cancel)
        {
            int length = GetWindowTextLengthW(edit);
            if (length > 0)
            {
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
                GetWindowTextW(edit, buffer.data(), length + 1);
                newName.assign(buffer.data());
            }
        }

        DestroyWindow(edit);
        if (renameFont_ != nullptr)
        {
            DeleteObject(renameFont_);
            renameFont_ = nullptr;
        }
        if (renameBackgroundBrush_ != nullptr)
        {
            DeleteObject(renameBackgroundBrush_);
            renameBackgroundBrush_ = nullptr;
        }

        if (renamingFolderEntry_)
        {
            CommitFolderEntryRename(newName, cancel);
            return;
        }

        if (renamingWidget_)
        {
            if (!cancel && renameIndex_ < widgets_.size() && !newName.empty())
            {
                widgets_[renameIndex_].title = newName;
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            renamingWidget_ = false;
            return;
        }

        bool keepUpdatedLayoutSlots = false;
        if (!cancel && renameIndex_ < items_.size() && !newName.empty() && newName != items_[renameIndex_].name)
        {
            int oldSlot = items_[renameIndex_].slot;
            std::wstring oldLayoutKey = items_[renameIndex_].layoutKey;
            PITEMID_CHILD newChild = nullptr;
            HRESULT hr = desktopFolder_->SetNameOf(
                hwnd_,
                reinterpret_cast<PCUITEMID_CHILD>(items_[renameIndex_].childPidl.get()),
                newName.c_str(),
                SHGDN_NORMAL,
                &newChild);
            if (SUCCEEDED(hr))
            {
                if (newChild != nullptr)
                {
                    PIDLIST_ABSOLUTE newAbsolute = ILCombine(desktopPidl_.get(), newChild);
                    std::wstring newParsingName = StrRetToString(desktopFolder_.Get(), newChild, SHGDN_FORPARSING);
                    if (newAbsolute != nullptr)
                    {
                        std::wstring newLayoutKey = GetStableLayoutKey(newAbsolute, newParsingName);
                        LayoutRecord record;
                        record.cell = items_[renameIndex_].gridCell;
                        record.span = items_[renameIndex_].gridSpan;
                        record.hasGrid = true;
                        record.legacySlot = oldSlot;
                        layoutRecords_[newLayoutKey] = record;
                        for (auto& widget : widgets_)
                        {
                            for (auto& key : widget.itemKeys)
                            {
                                if (NormalizeLayoutKey(key) == NormalizeLayoutKey(oldLayoutKey))
                                {
                                    key = newLayoutKey;
                                }
                            }
                        }
                        keepUpdatedLayoutSlots = true;
                        ILFree(newAbsolute);
                    }
                }
            }
            if (newChild != nullptr)
            {
                ILFree(newChild);
            }

            if (FAILED(hr))
            {
                MessageBeep(MB_ICONWARNING);
            }
        }

        ReloadItems(!keepUpdatedLayoutSlots);
    }

    static D2D1_RECT_F ToD2DRect(const RECT& rect)
    {
        return D2D1::RectF(
            static_cast<float>(rect.left),
            static_cast<float>(rect.top),
            static_cast<float>(rect.right),
            static_cast<float>(rect.bottom));
    }

    ComPtr<ID2D1Bitmap1> CreateD2DBitmapFromHBitmap(HBITMAP hbm)
    {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (hbm == nullptr || bitmapContext_ == nullptr)
        {
            return bitmap;
        }

        BITMAP bm{};
        if (GetObjectW(hbm, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight == 0)
        {
            return bitmap;
        }

        const int width = bm.bmWidth;
        const int height = std::abs(bm.bmHeight);
        std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));

        if (bm.bmBits != nullptr && bm.bmBitsPixel == 32)
        {
            const auto* src = static_cast<const std::uint8_t*>(bm.bmBits);
            const size_t srcPitch = static_cast<size_t>(std::abs(bm.bmWidthBytes));
            for (int y = 0; y < height; ++y)
            {
                std::memcpy(
                    pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width)),
                    src + (static_cast<size_t>(y) * srcPitch),
                    static_cast<size_t>(width) * sizeof(std::uint32_t));
            }
        }
        else
        {
            HDC screenDc = GetDC(nullptr);
            if (screenDc == nullptr)
            {
                return bitmap;
            }

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            if (GetDIBits(screenDc, hbm, 0, static_cast<UINT>(height), pixels.data(), &bi, DIB_RGB_COLORS) == 0)
            {
                ReleaseDC(nullptr, screenDc);
                return bitmap;
            }
            ReleaseDC(nullptr, screenDc);
        }

        bool hasAlpha = false;
        bool hasVisiblePixels = false;
        for (std::uint32_t pixel : pixels)
        {
            if (((pixel >> 24) & 0xff) != 0)
            {
                hasAlpha = true;
            }
            if ((pixel & 0x00ffffff) != 0)
            {
                hasVisiblePixels = true;
            }
        }
        if (!hasAlpha && hasVisiblePixels)
        {
            for (std::uint32_t& pixel : pixels)
            {
                if ((pixel & 0x00ffffff) != 0)
                {
                    pixel |= 0xff000000;
                }
            }
        }
        PremultiplyBgraPixels(pixels.data(), width, height);

        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

        bitmapContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
            pixels.data(),
            static_cast<UINT32>(width * sizeof(std::uint32_t)),
            &props,
            &bitmap);
        return bitmap;
    }

    ID2D1Bitmap1* GetOrCreateD2DBitmap(HBITMAP hbm)
    {
        if (hbm == nullptr)
        {
            return nullptr;
        }

        const auto key = reinterpret_cast<std::uintptr_t>(hbm);
        auto found = d2dIconCache_.find(key);
        if (found != d2dIconCache_.end())
        {
            return found->second.Get();
        }

        ComPtr<ID2D1Bitmap1> bitmap = CreateD2DBitmapFromHBitmap(hbm);
        if (!bitmap)
        {
            return nullptr;
        }

        ID2D1Bitmap1* raw = bitmap.Get();
        d2dIconCache_.emplace(key, std::move(bitmap));
        return raw;
    }

    void DrawD2D()
    {
        if (hwnd_ == nullptr || dcompSurface_ == nullptr || dcompDevice_ == nullptr)
        {
            return;
        }

        HRESULT hr = CreateOrResizeCompositionSurface();
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return;
        }

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        hr = dcompSurface_->BeginDraw(
            nullptr,
            __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext),
            &updateOffset);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return;
        }

        ComPtr<ID2D1DeviceContext> context;
        context.Attach(rawContext);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x),
            static_cast<float>(updateOffset.y)));
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        DrawD2DScene(context.Get());

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();

        hr = dcompSurface_->EndDraw();
        if (SUCCEEDED(hr))
        {
            hr = dcompDevice_->Commit();
        }
        lastGraphicsError_ = hr;
    }

    void DrawD2DScene(ID2D1DeviceContext* context)
    {
        if (context == nullptr)
        {
            return;
        }

        for (const auto& item : items_)
        {
            if (IsRectEmptyRect(item.bounds))
            {
                continue;
            }
            if (draggingItems_ && item.selected)
            {
                continue;
            }
            DrawD2DItemAt(context, item, item.bounds, item.selected);
        }

        for (const auto& widget : widgets_)
        {
            DrawD2DWidget(context, widget);
        }

        if (draggingWidget_ || resizingWidget_)
        {
            DrawD2DWidgetPreview(context);
        }

        RECT client{};
        GetClientRect(hwnd_, &client);
        DrawD2DStatus(context, client);

        if (marqueeActive_)
        {
            RECT drawRect = marqueeRect_;
            if (mouseDownWidgetIndex_ < widgets_.size() &&
                widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::FileCategories)
            {
                RECT content = GetFileCategoryContentRect(widgets_[mouseDownWidgetIndex_]);
                RECT clipped{};
                if (IntersectRect(&clipped, &marqueeRect_, &content))
                    drawRect = clipped;
            }
            else if (mouseDownWidgetIndex_ < widgets_.size() &&
                widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::FolderMapping)
            {
                RECT content = GetFolderMappingContentRect(widgets_[mouseDownWidgetIndex_]);
                RECT clipped{};
                if (IntersectRect(&clipped, &marqueeRect_, &content))
                    drawRect = clipped;
            }
            DrawD2DFilledRectangle(
                context,
                drawRect,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f),
                nullptr);
        }

        if (draggingItems_ && !draggingOverNav_)
        {
            const bool overPopup = IsPointInsideOpenPopup(dragCurrentPoint_);
            if (!overPopup)
            {
                DrawD2DCollectionInsertionPreview(context, dragCurrentPoint_);
                DrawD2DWidgetMemberInsertionPreview(context, dragCurrentPoint_);
                DrawD2DFolderEntryDesktopDropPreview(context, dragCurrentPoint_);
            }
            if (!overPopup)
            {
                DesktopHit hit = HitTestDesktop(dragCurrentPoint_);
                bool overWidget = (hit.kind == DesktopHitKind::Widget ||
                    hit.kind == DesktopHitKind::WidgetMember ||
                    hit.kind == DesktopHitKind::WidgetContent ||
                    hit.kind == DesktopHitKind::WidgetAllButton ||
                    hit.kind == DesktopHitKind::PopupMember);
                int handoffTarget = HitTestForDropTarget(dragCurrentPoint_);
                bool overHandoff = (handoffTarget >= 0 && !items_[static_cast<size_t>(handoffTarget)].selected);
                if (!overWidget && !overHandoff)
                {
                    for (RECT targetRect : GetSelectedMovePreviewRectsForCell(dragTargetCell_))
                {
                    targetRect.left += 3;
                    targetRect.top += 3;
                    targetRect.right -= 3;
                    targetRect.bottom -= 3;
                    DrawD2DFilledRectangle(
                        context,
                        targetRect,
                        D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.14f),
                        D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f),
                        dottedStrokeStyle_.Get());
                }
                }
            }
        }

        if (externalDragActive_)
        {
            RECT target{};
            size_t folderMapping = FindFolderMappingAtPoint(externalDragPoint_);
            if (folderMapping < widgets_.size())
            {
                target = GetWidgetFrameRect(widgets_[folderMapping]);
            }
            else
            {
                int hit = HitTest(externalDragPoint_);
                if (hit >= 0)
                {
                    target = GetItemSelectionRect(items_[static_cast<size_t>(hit)], true);
                }
                else
                {
                    target = GetGridRect(CellFromPoint(externalDragPoint_));
                }
            }

            target.left += 3;
            target.top += 3;
            target.right -= 3;
            target.bottom -= 3;
            DrawD2DRectangle(context, target, D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f), dottedStrokeStyle_.Get());
        }

        if (navButtonsVisible_)
        {
            DrawD2DNavigationPanel(context);
        }

        DrawD2DCollectionPopup(context);

        if (draggingItems_ && !draggingOverNav_ && IsPointInsideOpenPopup(dragCurrentPoint_))
        {
            DrawD2DCollectionInsertionPreview(context, dragCurrentPoint_);
                DrawD2DWidgetMemberInsertionPreview(context, dragCurrentPoint_);
        }

        if (draggingItems_)
        {
            DrawD2DDraggedItems(context);
        }
    }

    void DrawD2DFilledRectangle(
        ID2D1DeviceContext* context,
        const RECT& rect,
        const D2D1_COLOR_F& fillColor,
        const D2D1_COLOR_F& strokeColor,
        ID2D1StrokeStyle* strokeStyle)
    {
        ComPtr<ID2D1SolidColorBrush> fillBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(fillColor, &fillBrush)) && fillBrush)
        {
            context->FillRectangle(ToD2DRect(rect), fillBrush.Get());
        }

        DrawD2DRectangle(context, rect, strokeColor, strokeStyle);
    }

    void DrawD2DRectangle(ID2D1DeviceContext* context, const RECT& rect, const D2D1_COLOR_F& color, ID2D1StrokeStyle* strokeStyle)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(context->CreateSolidColorBrush(color, &brush)) || !brush)
        {
            return;
        }

        context->DrawRectangle(ToD2DRect(rect), brush.Get(), 1.0f, strokeStyle);
    }

    void DrawD2DRoundedRectangle(
        ID2D1DeviceContext* context,
        const RECT& rect,
        float radius,
        const D2D1_COLOR_F& fillColor,
        const D2D1_COLOR_F& strokeColor,
        float strokeWidth = 1.0f,
        ID2D1StrokeStyle* strokeStyle = nullptr)
    {
        if (context == nullptr || IsRectEmptyRect(rect))
        {
            return;
        }

        D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(ToD2DRect(rect), radius, radius);
        ComPtr<ID2D1SolidColorBrush> fillBrush;
        if (fillColor.a > 0.0f && SUCCEEDED(context->CreateSolidColorBrush(fillColor, &fillBrush)) && fillBrush)
        {
            context->FillRoundedRectangle(rounded, fillBrush.Get());
        }

        ComPtr<ID2D1SolidColorBrush> strokeBrush;
        if (strokeColor.a > 0.0f && SUCCEEDED(context->CreateSolidColorBrush(strokeColor, &strokeBrush)) && strokeBrush)
        {
            context->DrawRoundedRectangle(rounded, strokeBrush.Get(), strokeWidth, strokeStyle);
        }
    }

    void DrawD2DButton(ID2D1DeviceContext* context, const RECT& rect, const std::wstring& label, bool enabled)
    {
        if (context == nullptr || !enabled)
        {
            return;
        }

        constexpr float kRadius = 4.0f;
        D2D1_RECT_F d2dRect = ToD2DRect(rect);
        D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(d2dRect, kRadius, kRadius);

        ComPtr<ID2D1SolidColorBrush> fillBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &fillBrush);
        if (fillBrush)
        {
            context->FillRoundedRectangle(roundedRect, fillBrush.Get());
        }

        ComPtr<ID2D1SolidColorBrush> strokeBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.56f, 0.60f, 0.66f, 0.95f), &strokeBrush);
        if (strokeBrush)
        {
            context->DrawRoundedRectangle(roundedRect, strokeBrush.Get(), 1.0f);
        }

        IDWriteTextFormat* textFormat = nullptr;
        if (faTextFormat_ && !label.empty() && label[0] >= 0xF000)
        {
            textFormat = faTextFormat_.Get();
        }
        else if (itemTextFormat_)
        {
            textFormat = itemTextFormat_.Get();
        }

        if (!textFormat)
        {
            return;
        }

        ComPtr<ID2D1SolidColorBrush> textBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.13f, 0.18f, 1.0f), &textBrush);
        if (!textBrush)
        {
            return;
        }

        D2D1_RECT_F textRect = ToD2DRect(MakeRect(rect.left + 4, rect.top + 5, rect.right - 4, rect.bottom));
        context->DrawTextW(
            label.c_str(),
            static_cast<UINT32>(label.size()),
            textFormat,
            &textRect,
            textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawD2DText(ID2D1DeviceContext* context, const std::wstring& text, RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color)
    {
        if (context == nullptr || format == nullptr || text.empty() || IsRectEmptyRect(rect))
        {
            return;
        }

        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(context->CreateSolidColorBrush(color, &brush)) || !brush)
        {
            return;
        }

        D2D1_RECT_F d2dRect = ToD2DRect(rect);
        context->DrawTextW(
            text.c_str(),
            static_cast<UINT32>(text.size()),
            format,
            &d2dRect,
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void DrawD2DItemThumbnail(ID2D1DeviceContext* context, const DesktopItem& item, RECT rect, bool showLabel, bool selected = false)
    {
        if (context == nullptr || IsRectEmptyRect(rect))
        {
            return;
        }

        if (selected)
        {
            DrawD2DRoundedRectangle(
                context,
                rect,
                7.0f,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.24f),
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.78f));
        }

        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int labelHeight = showLabel ? (selected ? std::min(34, std::max(18, height / 3)) : 18) : 0;
        const int iconSize = std::max(16, std::min(width - 6, height - labelHeight - 4));
        const int iconX = rect.left + (width - iconSize) / 2;
        const int iconY = showLabel ? rect.top + 2 : rect.top + (height - iconSize) / 2;
        ID2D1Bitmap1* iconBitmap = GetOrCreateD2DBitmap(item.iconBitmap);
        if (iconBitmap != nullptr)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconX),
                static_cast<float>(iconY),
                static_cast<float>(iconX + iconSize),
                static_cast<float>(iconY + iconSize));
            context->DrawBitmap(iconBitmap, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }

        if (showLabel && collectionItemTextFormat_ && !item.name.empty())
        {
            RECT textRect = MakeRect(rect.left, iconY + iconSize + 2, rect.right, rect.bottom);
            IDWriteTextFormat* textFormat = selected && itemTextFormat_ ? itemTextFormat_.Get() : collectionItemTextFormat_.Get();
            if (!selected)
            {
                textRect.bottom = std::min<LONG>(textRect.bottom, textRect.top + 18);
            }
            DrawD2DText(context, item.name, OffsetRectCopy(textRect, 1, 1), textFormat, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.62f));
            DrawD2DText(context, item.name, textRect, textFormat, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        }
    }

    RECT OffsetRectCopy(RECT rect, int dx, int dy) const
    {
        OffsetRect(&rect, dx, dy);
        return rect;
    }

    void DrawD2DCollectionMosaic(ID2D1DeviceContext* context, const DesktopWidget& widget, RECT rect, size_t startIndex)
    {
        if (context == nullptr || IsRectEmptyRect(rect))
        {
            return;
        }

        RECT inner = rect;
        InflateRect(&inner, -8, -8);
        const int columns = 2;
        const int rows = 2;
        const int tileW = std::max<int>(1, static_cast<int>(inner.right - inner.left) / columns);
        const int tileH = std::max<int>(1, static_cast<int>(inner.bottom - inner.top) / rows);
        bool hasRemainingIcon = false;
        for (size_t i = 0; i < 4; ++i)
        {
            const size_t itemKeyIndex = startIndex + i;
            if (itemKeyIndex < widget.itemKeys.size() &&
                FindItemIndexByKey(widget.itemKeys[itemKeyIndex]) != static_cast<size_t>(-1))
            {
                hasRemainingIcon = true;
                break;
            }
        }
        if (!hasRemainingIcon && !PtInRect(&rect, lastMousePoint_))
        {
            return;
        }

        for (size_t i = 0; i < 4; ++i)
        {
            const int col = static_cast<int>(i % columns);
            const int row = static_cast<int>(i / columns);
            RECT tile = MakeRect(
                inner.left + col * tileW,
                inner.top + row * tileH,
                col + 1 == columns ? inner.right : inner.left + (col + 1) * tileW,
                row + 1 == rows ? inner.bottom : inner.top + (row + 1) * tileH);
            InflateRect(&tile, -2, -2);

            const size_t itemKeyIndex = startIndex + i;
            if (itemKeyIndex < widget.itemKeys.size())
            {
                size_t itemIndex = FindItemIndexByKey(widget.itemKeys[itemKeyIndex]);
                if (itemIndex != static_cast<size_t>(-1))
                {
                    InflateRect(&tile, -4, -4);
                    DrawD2DItemThumbnail(context, items_[itemIndex], tile, false, items_[itemIndex].selected);
                    continue;
                }
            }

            if (!hasRemainingIcon)
            {
                constexpr int tilePad = 2;
                InflateRect(&tile, -tilePad, -tilePad);
                DrawD2DRoundedRectangle(
                    context,
                    tile,
                    3.0f,
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f),
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
            }
        }
    }

    void DrawD2DScrollbar(ID2D1DeviceContext* context, RECT content, int scrollOffset, int maxScroll, int contentHeight)
    {
        if (maxScroll <= 0) return;
        const int trackHeight = std::max<int>(1, static_cast<int>(content.bottom - content.top));
        const float ratio = std::min(1.0f, static_cast<float>(trackHeight) / static_cast<float>(std::max(1, contentHeight)));
        const int thumbHeight = std::max<int>(16, static_cast<int>(ratio * trackHeight));
        const int thumbRange = trackHeight - thumbHeight;
        const int thumbTop = content.top + static_cast<int>(
            static_cast<float>(scrollOffset) / static_cast<float>(maxScroll) * thumbRange);

        constexpr int barWidth = 5;
        RECT track = MakeRect(content.right - barWidth - 2, content.top + 2,
            content.right - 2, content.bottom - 2);
        RECT thumb = MakeRect(track.left, thumbTop, track.right, thumbTop + thumbHeight);

        DrawD2DRoundedRectangle(context, track, 2.5f,
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f));
        DrawD2DRoundedRectangle(context, thumb, 2.5f,
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
    }

    void DrawD2DFileCategoryWidget(ID2D1DeviceContext* context, const DesktopWidget& widget)
    {
        if (context == nullptr)
        {
            return;
        }

        std::vector<std::wstring> categoryIds = GetVisibleFileCategoryIds(widget);
        if (categoryIds.empty())
        {
            RECT body = GetWidgetBodyRect(widget);
            InflateRect(&body, -12, -12);
            DrawD2DText(context, L"暂无散文件", body, collectionItemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
            return;
        }

        std::wstring activeCategory = GetActiveFileCategoryId(widget);
        RECT tabsRect = GetFileCategoryTabsRect(widget);
        context->PushAxisAlignedClip(ToD2DRect(tabsRect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < categoryIds.size(); ++i)
        {
            RECT tab = GetFileCategoryTabRect(widget, i);
            if (IsRectEmptyRect(tab))
            {
                continue;
            }

            const bool active = categoryIds[i] == activeCategory;
            const bool hovered = PtInRect(&tab, lastMousePoint_) != FALSE;
            DrawD2DRoundedRectangle(
                context,
                tab,
                7.0f,
                active ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f) : (hovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)),
                active ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.78f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));

            std::wstring label = GetFileCategoryLabel(categoryIds[i]) + L" " + std::to_wstring(GetFileCategoryCount(widget, categoryIds[i]));
            DrawD2DText(context, label, MakeRect(tab.left + 4, tab.top + 7, tab.right - 4, tab.bottom), collectionItemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.98f : 0.78f));
        }
        context->PopAxisAlignedClip();
        if (!IsRectEmptyRect(tabsRect))
        {
            RECT line = MakeRect(tabsRect.left, tabsRect.bottom + 2, tabsRect.right, tabsRect.bottom + 3);
            DrawD2DFilledRectangle(context, line, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f), nullptr);
        }

        RECT fcToggle = GetFileCategoryToggleRect(widget);
        if (!IsRectEmptyRect(fcToggle))
        {
            bool toggleHovered = PtInRect(&fcToggle, lastMousePoint_) != FALSE;
            DrawD2DRoundedRectangle(
                context, fcToggle, 4.0f,
                toggleHovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
            DrawD2DText(context, widget.listMode ? L"" : L"", fcToggle,
                (faTextFormat_ ? faTextFormat_.Get() : collectionItemTextFormat_.Get()), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
        }

        std::vector<std::wstring> keys = GetFileCategoryKeys(widget, activeCategory);
        RECT content = GetFileCategoryContentRect(widget);
        if (IsRectEmptyRect(content))
        {
            return;
        }

        context->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < keys.size(); ++i)
        {
            RECT itemRect = GetFileCategoryItemRect(widget, i);
            if (itemRect.bottom <= content.top || itemRect.top >= content.bottom)
            {
                continue;
            }

            size_t itemIndex = FindItemIndexByKey(keys[i]);
            if (itemIndex == static_cast<size_t>(-1))
            {
                continue;
            }

            if (!widget.listMode)
            {
                DrawD2DItemAt(context, items_[itemIndex], itemRect, items_[itemIndex].selected);
                continue;
            }

            DrawD2DItemGenericList(context, itemRect, items_[itemIndex].selected,
                items_[itemIndex].iconBitmap, items_[itemIndex].name);
        }
        if (PtInRect(&widget.bounds, lastMousePoint_))
        {
            int maxScroll = GetFileCategoryMaxScrollOffset(widget);
            int contentHeight = GetFileCategoryContentHeight(widget, keys.size());
            DrawD2DScrollbar(context, content, widget.scrollOffset, maxScroll, contentHeight);
        }
        context->PopAxisAlignedClip();
    }

    void DrawD2DFolderMappingWidget(ID2D1DeviceContext* context, const DesktopWidget& widget)
    {
        if (context == nullptr)
        {
            return;
        }

        RECT toggleRect = GetFolderMappingToggleRect(widget);
        if (!IsRectEmptyRect(toggleRect))
        {
            bool hovered = PtInRect(&toggleRect, lastMousePoint_) != FALSE;
            DrawD2DRoundedRectangle(
                context, toggleRect, 4.0f,
                hovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
            DrawD2DText(context, widget.listMode ? L"" : L"", toggleRect,
                (faTextFormat_ ? faTextFormat_.Get() : collectionItemTextFormat_.Get()), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
        }

        RECT openRect = GetFolderMappingOpenRect(widget);
        if (!IsRectEmptyRect(openRect))
        {
            bool hovered = PtInRect(&openRect, lastMousePoint_) != FALSE;
            DrawD2DRoundedRectangle(
                context, openRect, 4.0f,
                hovered ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
            DrawD2DText(context, L"", openRect,
                (faTextFormat_ ? faTextFormat_.Get() : collectionItemTextFormat_.Get()), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
        }

        size_t entryCount = widget.folderEntries.size();
        if (entryCount == 0)
        {
            RECT body = GetWidgetBodyRect(widget);
            InflateRect(&body, -12, -12);
            DrawD2DText(context, L"空文件夹", body,
                collectionItemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
            return;
        }

        RECT content = GetFolderMappingContentRect(widget);
        if (IsRectEmptyRect(content))
        {
            return;
        }

        context->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < entryCount; ++i)
        {
            RECT itemRect = GetFolderMappingItemRect(widget, i);
            if (itemRect.bottom <= content.top || itemRect.top >= content.bottom)
            {
                continue;
            }

            const FolderEntry& entry = widget.folderEntries[i];

            if (!widget.listMode)
            {
                DrawD2DItemGeneric(context, itemRect, entry.selected, entry.iconBitmap, entry.name);
            }
            else
            {
                DrawD2DItemGenericList(context, itemRect, entry.selected, entry.iconBitmap, entry.name);
            }
        }
        if (PtInRect(&widget.bounds, lastMousePoint_))
        {
            int maxScroll = GetFolderMappingMaxScrollOffset(widget);
            int contentHeight = GetFolderMappingContentHeight(widget, widget.folderEntries.size());
            DrawD2DScrollbar(context, content, widget.scrollOffset, maxScroll, contentHeight);
        }
        context->PopAxisAlignedClip();
    }

    void DrawD2DWidget(ID2D1DeviceContext* context, const DesktopWidget& widget)
    {
        RECT frame = GetWidgetFrameRect(widget);
        RECT body = GetWidgetBodyRect(widget);
        if (IsRectEmptyRect(frame) || IsRectEmptyRect(body))
        {
            return;
        }

        const bool selected = widget.selected;

        D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
        D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);
        if (settingsWindow_)
        {
            const auto& p = settingsWindow_->GetPersonalization();
            fillColor = D2D1::ColorF(p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
            borderColor = D2D1::ColorF(p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetAlpha);
        }

        DrawD2DRoundedRectangle(
            context,
            frame,
            12.0f,
            fillColor,
            selected ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.90f) : borderColor,
            selected ? 1.6f : 1.0f,
            selected ? dottedStrokeStyle_.Get() : nullptr);

        // Draw gradient first (under content + buttons)
        {
            RECT body = GetWidgetBodyRect(widget);
            RECT gradientRect = MakeRect(frame.left, std::max<LONG>(body.top, frame.bottom - 36), frame.right, frame.bottom);
            bool showGradient = widget.type != DesktopWidgetType::Collection
                || (widget.gridSpan.columns > 1 || widget.gridSpan.rows > 1
                    ? (PtInRect(&frame, lastMousePoint_) != FALSE)
                    : true);
            if (showGradient && !IsRectEmptyRect(gradientRect) && gradientRect.bottom > gradientRect.top)
            {
                ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
                if (SUCCEEDED(d2dFactory_->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(ToD2DRect(frame), 12.0f, 12.0f), &clipGeo)) && clipGeo)
                {
                    context->PushLayer(D2D1::LayerParameters(ToD2DRect(frame), clipGeo.Get()), nullptr);
                }
                ComPtr<ID2D1GradientStopCollection> stops;
                D2D1_GRADIENT_STOP stopDescs[] = {
                    { 0.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, 0.0f) },
                    { 1.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b,
                        settingsWindow_ ? settingsWindow_->GetPersonalization().gradientEndA : 0.65f) },
                };
                if (SUCCEEDED(context->CreateGradientStopCollection(stopDescs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops)) && stops)
                {
                    ComPtr<ID2D1LinearGradientBrush> brush;
                    if (SUCCEEDED(context->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(
                            D2D1::Point2F(0.0f, static_cast<float>(gradientRect.top)),
                            D2D1::Point2F(0.0f, static_cast<float>(gradientRect.bottom))),
                        stops.Get(),
                        &brush)) && brush)
                    {
                        context->FillRectangle(ToD2DRect(gradientRect), brush.Get());
                    }
                }
                if (clipGeo) context->PopLayer();
            }
        }

        // Draw widget content (buttons render ON TOP of gradient)
        if (widget.type == DesktopWidgetType::Collection)
        {
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            d2dFactory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(ToD2DRect(frame), 12.0f, 12.0f), &clipGeo);
            if (clipGeo)
                context->PushLayer(D2D1::LayerParameters(ToD2DRect(frame), clipGeo.Get()), nullptr);

            const size_t inlineCapacity = std::min(GetCollectionInlineCapacity(widget), widget.itemKeys.size());
            for (size_t i = 0; i < inlineCapacity; ++i)
            {
                size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
                if (itemIndex == static_cast<size_t>(-1))
                {
                    continue;
                }
                RECT slotRect = GetCollectionPreviewSlotRect(widget, i);
                if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1)
                {
                    DrawD2DItemThumbnail(context, items_[itemIndex], slotRect, false, items_[itemIndex].selected);
                }
                else
                {
                    DrawD2DItemAt(context, items_[itemIndex], slotRect, items_[itemIndex].selected);
                }
            }

            size_t allSlot = GetCollectionAllButtonSlot(widget);
            if (allSlot != static_cast<size_t>(-1))
            {
                RECT allRect = GetCollectionPreviewSlotRect(widget, allSlot);
                DrawD2DCollectionMosaic(context, widget, allRect, GetCollectionInlineCapacity(widget));
            }

            if (clipGeo) context->PopLayer();
        }
        else if (widget.type == DesktopWidgetType::FileCategories)
        {
            DrawD2DFileCategoryWidget(context, widget);
        }
        else if (widget.type == DesktopWidgetType::FolderMapping)
        {
            DrawD2DFolderMappingWidget(context, widget);
        }
        else if (widget.type == DesktopWidgetType::LuaScript)
        {
            if (widgetEngine_)
                widgetEngine_->RenderWidget(widget.scriptPath, context, widget.bounds);
        }

        const bool isSmallCollection = widget.type == DesktopWidgetType::Collection
            && widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
        const bool hovered = (widget.type == DesktopWidgetType::Collection && !isSmallCollection)
            ? (PtInRect(&frame, lastMousePoint_) != FALSE)
            : true;

        if (hovered)
        {
            RECT titleRect = GetWidgetTitleRect(widget);
        if (!widget.title.empty() && listItemTextFormat_)
        {
            DrawD2DText(context, widget.title, OffsetRectCopy(titleRect, 1, 1), listItemTextFormat_.Get(), D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f));
            DrawD2DText(context, widget.title, titleRect, listItemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
        }

        RECT resizeHandle = GetWidgetResizeHandleRect(widget);
        const int dot = 8;
        const int cx = resizeHandle.left + (resizeHandle.right - resizeHandle.left) / 2;
        const int cy = resizeHandle.top + (resizeHandle.bottom - resizeHandle.top) / 2;
        RECT pill = MakeRect(cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2);
        DrawD2DRoundedRectangle(
            context,
            pill,
            4.0f,
            selected ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.62f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f));
        }
    }

    void DrawD2DWidgetPreview(ID2D1DeviceContext* context)
    {
        if (mouseDownWidgetIndex_ >= widgets_.size())
        {
            return;
        }

        DesktopWidget preview = widgets_[mouseDownWidgetIndex_];
        preview.gridCell = widgetPreviewCell_;
        preview.gridSpan = widgetPreviewSpan_;
        preview.bounds = GetGridRect(preview.gridCell, preview.gridSpan);
        RECT body = GetWidgetFrameRect(preview);

        D2D1_COLOR_F fillColor = widgetPreviewOccupied_
            ? D2D1::ColorF(1.0f, 0.35f, 0.35f, 0.20f)
            : D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.16f);
        D2D1_COLOR_F strokeColor = widgetPreviewOccupied_
            ? D2D1::ColorF(1.0f, 0.30f, 0.30f, 0.85f)
            : D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.85f);

        DrawD2DRoundedRectangle(
            context,
            body,
            12.0f,
            fillColor,
            strokeColor,
            1.4f,
            dottedStrokeStyle_.Get());
    }

    RECT GetCollectionPopupRect(const DesktopWidget& widget) const
    {
        const GridPage* page = FindExactGridPage(widget.gridCell.pageId);
        RECT work = page != nullptr ? page->workArea : layoutWorkArea_;
        const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
        const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
        const int maxWidth = std::max(280, std::min(560, workWidth - 80));
        const int maxColumns = std::max(1, (maxWidth - kCollectionPopupPaddingX * 2) / GetDesktopCellWidth(widget.gridCell.pageId));
        const int itemCount = std::max(1, static_cast<int>(GetPopupItemKeys(widget).size()));
        int columns = std::clamp(std::min(itemCount, 5), 1, maxColumns);
        int rows = (itemCount + columns - 1) / columns;
        const int maxHeight = std::max(220, workHeight - 80);
        int width = kCollectionPopupPaddingX * 2 + columns * GetDesktopCellWidth(widget.gridCell.pageId);
        int height = kCollectionPopupHeaderHeight + rows * GetDesktopCellHeight(widget.gridCell.pageId) + kCollectionPopupBottomPadding;
        if (height > maxHeight && columns < maxColumns)
        {
            columns = maxColumns;
            rows = (itemCount + columns - 1) / columns;
            width = kCollectionPopupPaddingX * 2 + columns * GetDesktopCellWidth(widget.gridCell.pageId);
            height = kCollectionPopupHeaderHeight + rows * GetDesktopCellHeight(widget.gridCell.pageId) + kCollectionPopupBottomPadding;
        }
        height = std::min(height, maxHeight);
        int left = work.left + (workWidth - width) / 2;
        int top = work.top + (workHeight - height) / 2;
        if (popupHasAnchor_)
        {
            left = popupAnchorPoint_.x + 12;
            top = popupAnchorPoint_.y + 12;
            left = std::clamp(left, static_cast<int>(work.left + 12), static_cast<int>(std::max<LONG>(work.left + 12, work.right - width - 12)));
            top = std::clamp(top, static_cast<int>(work.top + 12), static_cast<int>(std::max<LONG>(work.top + 12, work.bottom - height - 12)));
        }
        return MakeRect(left, top, left + width, top + height);
    }

    RECT GetCollectionPopupContentRect(const RECT& popup) const
    {
        return MakeRect(
            popup.left + kCollectionPopupPaddingX,
            popup.top + kCollectionPopupHeaderHeight,
            popup.right - kCollectionPopupPaddingX,
            popup.bottom - kCollectionPopupBottomPadding);
    }

    int GetCollectionPopupColumnCount(const RECT& popup) const
    {
        RECT content = GetCollectionPopupContentRect(popup);
        return std::max<int>(1, static_cast<int>(content.right - content.left) / GetDesktopCellWidth(popupPageId_));
    }

    int GetCollectionPopupRowCount(const DesktopWidget& widget, const RECT& popup) const
    {
        const int columns = GetCollectionPopupColumnCount(popup);
        const int itemCount = std::max(1, static_cast<int>(GetPopupItemKeys(widget).size()));
        return (itemCount + columns - 1) / columns;
    }

    int GetCollectionPopupMaxScrollOffset(const DesktopWidget& widget, const RECT& popup) const
    {
        RECT content = GetCollectionPopupContentRect(popup);
        const int rows = GetCollectionPopupRowCount(widget, popup);
        const int contentHeight = std::max<int>(1, static_cast<int>(content.bottom - content.top));
        return std::max(0, rows * GetDesktopCellHeight(popupPageId_) - contentHeight);
    }

    RECT GetCollectionPopupItemRect(const RECT& popup, size_t linearIndex) const
    {
        RECT content = GetCollectionPopupContentRect(popup);
        const int columns = GetCollectionPopupColumnCount(popup);
        const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
        const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
        RECT rect = MakeRect(
            content.left + col * GetDesktopCellWidth(popupPageId_),
            content.top + row * GetDesktopCellHeight(popupPageId_) - popupScrollOffset_,
            content.left + (col + 1) * GetDesktopCellWidth(popupPageId_),
            content.top + (row + 1) * GetDesktopCellHeight(popupPageId_) - popupScrollOffset_);
        return rect;
    }

    RECT GetVisibleCollectionItemBounds(size_t itemIndex) const
    {
        if (itemIndex >= items_.size())
        {
            return {};
        }

        const std::wstring key = NormalizeLayoutKey(items_[itemIndex].layoutKey);
        if (popupWidgetIndex_ < widgets_.size())
        {
            const DesktopWidget& popupWidget = widgets_[popupWidgetIndex_];
            std::vector<std::wstring> popupKeys = GetPopupItemKeys(popupWidget);
            RECT popup = GetCollectionPopupRect(popupWidget);
            RECT content = GetCollectionPopupContentRect(popup);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                if (NormalizeLayoutKey(popupKeys[i]) != key)
                {
                    continue;
                }
                RECT rect = GetCollectionPopupItemRect(popup, i);
                if (RectsIntersect(rect, content))
                {
                    return rect;
                }
            }
        }

        for (const auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::FileCategories)
            {
                std::vector<std::wstring> keys = GetFileCategoryKeys(widget, GetActiveFileCategoryId(widget));
                RECT content = GetFileCategoryContentRect(widget);
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    if (NormalizeLayoutKey(keys[i]) != key)
                    {
                        continue;
                    }
                    RECT rect = GetFileCategoryItemRect(widget, i);
                    if (RectsIntersect(rect, content))
                    {
                        return rect;
                    }
                }
                continue;
            }
            if (widget.type != DesktopWidgetType::Collection)
            {
                continue;
            }

            const size_t inlineCapacity = std::min(GetCollectionInlineCapacity(widget), widget.itemKeys.size());
            for (size_t i = 0; i < inlineCapacity; ++i)
            {
                if (NormalizeLayoutKey(widget.itemKeys[i]) == key)
                {
                    return GetCollectionPreviewSlotRect(widget, i);
                }
            }
        }
        return {};
    }

    RECT GetCollectionInsertionIndicatorRect(size_t widgetIndex, POINT point, bool popup) const
    {
        if (widgetIndex >= widgets_.size() || widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        {
            return {};
        }

        const DesktopWidget& widget = widgets_[widgetIndex];
        const size_t insertIndex = GetCollectionInsertIndex(widgetIndex, point, popup);
        RECT anchor{};
        if (popup)
        {
            RECT popupRect = GetCollectionPopupRect(widget);
            RECT content = GetCollectionPopupContentRect(popupRect);
            if (widget.itemKeys.empty())
            {
                return MakeRect(content.left, content.top + 8, content.left + 3, content.bottom - 8);
            }
            const size_t anchorIndex = std::min(insertIndex, widget.itemKeys.size() - 1);
            anchor = GetCollectionPopupItemRect(popupRect, anchorIndex);
            LONG x = insertIndex >= widget.itemKeys.size() ? anchor.right + 4 : anchor.left - 4;
            return MakeRect(x, anchor.top + 8, x + 3, anchor.bottom - 8);
        }

        const bool compact = widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
        const size_t visible = compact ? std::min<size_t>(4, widget.itemKeys.size()) : std::min(GetCollectionInlineCapacity(widget), widget.itemKeys.size());
        if (visible == 0)
        {
            RECT body = GetWidgetBodyRect(widget);
            return MakeRect(body.left + 12, body.top + 12, body.left + 15, body.bottom - 12);
        }
        const size_t anchorIndex = std::min(insertIndex, visible - 1);
        anchor = GetCollectionPreviewSlotRect(widget, anchorIndex);
        if (insertIndex >= visible)
        {
            size_t allSlot = GetCollectionAllButtonSlot(widget);
            if (allSlot != static_cast<size_t>(-1))
            {
                anchor = GetCollectionPreviewSlotRect(widget, allSlot);
                return MakeRect(anchor.left - 4, anchor.top + 8, anchor.left - 1, anchor.bottom - 8);
            }
            return MakeRect(anchor.right + 4, anchor.top + 8, anchor.right + 7, anchor.bottom - 8);
        }
        return MakeRect(anchor.left - 4, anchor.top + 8, anchor.left - 1, anchor.bottom - 8);
    }

    RECT GetWidgetInsertionIndicatorRect(size_t widgetIndex, POINT point) const
    {
        if (widgetIndex >= widgets_.size())
        {
            return {};
        }

        const DesktopWidget& widget = widgets_[widgetIndex];
        size_t insertIndex = GetWidgetMemberInsertIndex(widgetIndex, point);
        RECT anchor{};
        size_t itemCount = (widget.type == DesktopWidgetType::FolderMapping)
            ? widget.folderEntries.size()
            : GetFileCategoryKeys(widget, GetActiveFileCategoryId(widget)).size();

        if (itemCount == 0)
        {
            RECT content = (widget.type == DesktopWidgetType::FolderMapping)
                ? GetFolderMappingContentRect(widget)
                : GetFileCategoryContentRect(widget);
            if (widget.listMode)
            {
                return MakeRect(content.left + 8, content.top + 8, content.right - 8, content.top + 11);
            }
            return MakeRect(content.left, content.top + 8, content.left + 3, content.bottom - 8);
        }

        size_t anchorIndex = std::min(insertIndex, itemCount - 1);
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            anchor = GetFolderMappingItemRect(widget, anchorIndex);
        }
        else
        {
            anchor = GetFileCategoryItemRect(widget, anchorIndex);
        }

        if (widget.listMode)
        {
            LONG y = insertIndex >= itemCount ? anchor.bottom + 2 : anchor.top - 4;
            return MakeRect(anchor.left + 8, y, anchor.right - 8, y + 3);
        }

        if (insertIndex >= itemCount)
        {
            return MakeRect(anchor.right + 4, anchor.top + 8, anchor.right + 7, anchor.bottom - 8);
        }
        return MakeRect(anchor.left - 4, anchor.top + 8, anchor.left - 1, anchor.bottom - 8);
    }

    void DrawD2DWidgetMemberInsertionPreview(ID2D1DeviceContext* context, POINT point)
    {
        if (context == nullptr || dragHint_.find(L"交给") != std::wstring::npos)
        {
            return;
        }

        size_t widgetIndex = static_cast<size_t>(-1);
        if (draggingWidgetMember_ && widgetMemberDragWidget_ < widgets_.size())
        {
            DesktopHit hit = HitTestDesktop(point);
            if ((hit.kind == DesktopHitKind::WidgetMember ||
                 hit.kind == DesktopHitKind::WidgetContent) &&
                hit.widgetIndex == widgetMemberDragWidget_)
            {
                widgetIndex = widgetMemberDragWidget_;
            }
            else if (hit.kind == DesktopHitKind::WidgetMember ||
                     hit.kind == DesktopHitKind::WidgetContent ||
                     hit.kind == DesktopHitKind::Widget)
            {
                if (hit.widgetIndex < widgets_.size() &&
                    hit.widgetIndex != widgetMemberDragWidget_ &&
                    widgets_[hit.widgetIndex].type == DesktopWidgetType::FileCategories)
                {
                    widgetIndex = hit.widgetIndex;
                }
            }
        }
        else if (draggingCollectionMember_ &&
            collectionDragSourceWidget_ < widgets_.size() &&
            widgets_[collectionDragSourceWidget_].type == DesktopWidgetType::FileCategories)
        {
            DesktopHit hit = HitTestDesktop(point);
            if ((hit.kind == DesktopHitKind::WidgetMember ||
                 hit.kind == DesktopHitKind::WidgetContent) &&
                hit.widgetIndex == collectionDragSourceWidget_)
            {
                widgetIndex = collectionDragSourceWidget_;
            }
        }

        if (widgetIndex == static_cast<size_t>(-1))
        {
            return;
        }

        RECT indicator = GetWidgetInsertionIndicatorRect(widgetIndex, point);
        if (!IsRectEmptyRect(indicator))
        {
            DrawD2DRoundedRectangle(
                context,
                indicator,
                2.0f,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.92f),
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.92f));
        }
    }

    void DrawD2DFolderEntryDesktopDropPreview(ID2D1DeviceContext* context, POINT point)
    {
        if (context == nullptr ||
            !draggingWidgetMember_ ||
            widgetMemberDragWidget_ >= widgets_.size() ||
            widgets_[widgetMemberDragWidget_].type != DesktopWidgetType::FolderMapping)
        {
            return;
        }

        DesktopHit hit = HitTestDesktop(point);
        const bool insideSource = (hit.kind == DesktopHitKind::WidgetMember ||
            hit.kind == DesktopHitKind::WidgetContent) &&
            hit.widgetIndex == widgetMemberDragWidget_;
        if (insideSource || IsWidgetDropSurface(hit))
        {
            return;
        }

        RECT target = GetGridRect(CellFromPoint(GetDragTargetPoint(point)));
        target.left += 3;
        target.top += 3;
        target.right -= 3;
        target.bottom -= 3;
        DrawD2DFilledRectangle(
            context,
            target,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.14f),
            D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f),
            dottedStrokeStyle_.Get());
    }

    void DrawD2DCollectionInsertionPreview(ID2D1DeviceContext* context, POINT point)
    {
        if (context == nullptr || dragHint_.find(L"交给") != std::wstring::npos)
        {
            return;
        }

        size_t widgetIndex = static_cast<size_t>(-1);
        bool popup = false;
        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popupRect = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popupRect, point) &&
                widgets_[popupWidgetIndex_].type == DesktopWidgetType::Collection)
            {
                widgetIndex = popupWidgetIndex_;
                popup = true;
            }
        }

        if (widgetIndex == static_cast<size_t>(-1))
        {
            DesktopHit hit = HitTestDesktop(point);
            if ((hit.kind == DesktopHitKind::Widget ||
                hit.kind == DesktopHitKind::WidgetMember ||
                hit.kind == DesktopHitKind::WidgetContent ||
                hit.kind == DesktopHitKind::WidgetAllButton) &&
                hit.widgetIndex < widgets_.size() &&
                (widgets_[hit.widgetIndex].type == DesktopWidgetType::Collection ||
                 widgets_[hit.widgetIndex].type == DesktopWidgetType::FileCategories ||
                 widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping))
            {
                widgetIndex = hit.widgetIndex;
            }
        }

        if (widgetIndex == static_cast<size_t>(-1))
        {
            return;
        }

        RECT indicator;
        if (widgets_[widgetIndex].type == DesktopWidgetType::Collection)
            indicator = GetCollectionInsertionIndicatorRect(widgetIndex, point, popup);
        else
            indicator = GetWidgetInsertionIndicatorRect(widgetIndex, point);
        DrawD2DRoundedRectangle(
            context,
            indicator,
            2.0f,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.92f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.92f));
    }

    void DrawD2DCollectionPopup(ID2D1DeviceContext* context)
    {
        if (popupWidgetIndex_ >= widgets_.size())
        {
            return;
        }

        const DesktopWidget& widget = widgets_[popupWidgetIndex_];
        popupPageId_ = widget.gridCell.pageId;
        std::vector<std::wstring> popupKeys = GetPopupItemKeys(widget);
        popupRect_ = GetCollectionPopupRect(widget);
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, GetCollectionPopupMaxScrollOffset(widget, popupRect_));
        DrawD2DRoundedRectangle(
            context,
            popupRect_,
            18.0f,
            D2D1::ColorF(0.08f, 0.10f, 0.13f, 0.92f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.38f));

        RECT titleRect = MakeRect(popupRect_.left + 22, popupRect_.top + 18, popupRect_.right - 22, popupRect_.top + 44);
        std::wstring popupTitle = widget.title.empty() ? L"集合" : widget.title;
        if (widget.type == DesktopWidgetType::FileCategories && !popupCategoryId_.empty())
        {
            popupTitle += L" / " + GetFileCategoryLabel(popupCategoryId_);
        }
        DrawD2DText(context, popupTitle, titleRect, itemTextFormat_.Get(), D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));

        RECT content = GetCollectionPopupContentRect(popupRect_);
        for (size_t i = 0; i < popupKeys.size(); ++i)
        {
            RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
            if (itemRect.bottom <= content.top || itemRect.top >= content.bottom)
            {
                continue;
            }

            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex == static_cast<size_t>(-1))
            {
                continue;
            }
            DrawD2DItemAt(context, items_[itemIndex], itemRect, items_[itemIndex].selected);
        }
    }

    void UpdateNavButtonHover(POINT point)
    {
        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr)
        {
            navButtonsVisible_ = false;
            return;
        }

        constexpr LONG kHoverZoneWidth = 220;
        constexpr LONG kHoverZoneHeight = 80;
        RECT hoverZone = MakeRect(
            page->workArea.right - kHoverZoneWidth,
            page->workArea.bottom - kHoverZoneHeight,
            page->workArea.right,
            page->workArea.bottom);
        navButtonsHoverZone_ = hoverZone;

        bool wasVisible = navButtonsVisible_;
        navButtonsVisible_ = PtInRect(&hoverZone, point) != FALSE;

        if (wasVisible != navButtonsVisible_)
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }

        if (!wasVisible && navButtonsVisible_)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd_, HOVER_DEFAULT };
            TrackMouseEvent(&tme);
        }
    }

    void DrawD2DNavigationPanel(ID2D1DeviceContext* context)
    {
        if (context == nullptr) return;

        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr) return;

        const bool hasPrev = pageOffset_ > 0;
        const bool hasNext = pageOffset_ < MaxPageOffset();
        if (!hasPrev && !hasNext) return;

        constexpr LONG kButtonWidth = 68;
        constexpr LONG kButtonHeight = 28;
        constexpr LONG kGap = 8;
        constexpr LONG kPanelPaddingX = 10;
        constexpr LONG kPanelPaddingY = 8;
        constexpr float kCornerRadius = 8.0f;

        const int visibleCount = (hasPrev ? 1 : 0) + (hasNext ? 1 : 0);
        const LONG kPanelWidth = kButtonWidth * visibleCount + kGap * (visibleCount - 1) + kPanelPaddingX * 2;
        const LONG kPanelHeight = kButtonHeight + kPanelPaddingY * 2;

        const LONG panelRight = std::max<LONG>(page->workArea.left + kPanelWidth,
            page->workArea.right - page->marginX - 10);
        const LONG panelBottom = std::max<LONG>(page->workArea.top + kPanelHeight,
            page->workArea.bottom - page->marginY - 10);
        const LONG panelLeft = panelRight - kPanelWidth;
        const LONG panelTop = panelBottom - kPanelHeight;

        D2D1_RECT_F panelRect = D2D1::RectF(
            static_cast<float>(panelLeft), static_cast<float>(panelTop),
            static_cast<float>(panelRight), static_cast<float>(panelBottom));
        D2D1_ROUNDED_RECT roundedPanel = D2D1::RoundedRect(panelRect, kCornerRadius, kCornerRadius);

        ComPtr<ID2D1SolidColorBrush> panelBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &panelBrush)) && panelBrush)
        {
            context->FillRoundedRectangle(roundedPanel, panelBrush.Get());
        }

        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.70f), &borderBrush)) && borderBrush)
        {
            context->DrawRoundedRectangle(roundedPanel, borderBrush.Get(), 1.0f);
        }

        LONG btnX = panelLeft + kPanelPaddingX;
        if (hasPrev)
        {
            RECT prevRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            DrawD2DButton(context, prevRect, L"上一页", true);
            btnX += kButtonWidth + kGap;
        }
        if (hasNext)
        {
            RECT nextRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            DrawD2DButton(context, nextRect, L"下一页", true);
        }
    }

    bool GetPageNavigationRects(RECT& previousRect, RECT& nextRect) const
    {
        const GridPage* page = FindExactGridPage(lastMonitorPageId_);
        if (page == nullptr)
        {
            return false;
        }

        constexpr LONG buttonWidth = 68;
        constexpr LONG buttonHeight = 28;
        constexpr LONG gap = 8;
        const LONG right = std::max<LONG>(page->workArea.left + buttonWidth, page->workArea.right - page->marginX - 10);
        const LONG bottom = std::max<LONG>(page->workArea.top + buttonHeight, page->workArea.bottom - page->marginY - 10);
        nextRect = MakeRect(right - buttonWidth, bottom - buttonHeight, right, bottom);
        previousRect = MakeRect(nextRect.left - gap - buttonWidth, nextRect.top, nextRect.left - gap, nextRect.bottom);
        return true;
    }

    void DrawD2DPageNavigationControls(ID2D1DeviceContext* context)
    {
        RECT previousRect{};
        RECT nextRect{};
        if (!GetPageNavigationRects(previousRect, nextRect))
        {
            return;
        }

        DrawD2DButton(context, previousRect, L"上一页", pageOffset_ > 0);
        DrawD2DButton(context, nextRect, L"下一页", pageOffset_ < MaxPageOffset());
    }

    int HitTestNavButton(POINT point) const
    {
        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr) return 0;

        const bool hasPrev = pageOffset_ > 0;
        const bool hasNext = pageOffset_ < MaxPageOffset();
        if (!hasPrev && !hasNext) return 0;

        constexpr LONG kButtonWidth = 68;
        constexpr LONG kButtonHeight = 28;
        constexpr LONG kGap = 8;
        constexpr LONG kPanelPaddingX = 10;
        constexpr LONG kPanelPaddingY = 8;
        const int visibleCount = (hasPrev ? 1 : 0) + (hasNext ? 1 : 0);
        const LONG kPanelWidth = kButtonWidth * visibleCount + kGap * (visibleCount - 1) + kPanelPaddingX * 2;
        const LONG kPanelHeight = kButtonHeight + kPanelPaddingY * 2;

        const LONG panelRight = std::max<LONG>(page->workArea.left + kPanelWidth,
            page->workArea.right - page->marginX - 10);
        const LONG panelBottom = std::max<LONG>(page->workArea.top + kPanelHeight,
            page->workArea.bottom - page->marginY - 10);
        const LONG panelLeft = panelRight - kPanelWidth;
        const LONG panelTop = panelBottom - kPanelHeight;

        LONG btnX = panelLeft + kPanelPaddingX;
        if (hasPrev)
        {
            RECT prevRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&prevRect, point)) return -1;
            btnX += kButtonWidth + kGap;
        }
        if (hasNext)
        {
            RECT nextRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&nextRect, point)) return 1;
        }
        return 0;
    }

    RECT GetWidgetFrameRect(const DesktopWidget& widget) const
    {
        RECT rect = widget.bounds;
        const GridPage* page = FindExactGridPage(widget.gridCell.pageId);
        if (page != nullptr)
        {
            const int halfGapX = std::max(2, page->gapX / 2);
            const int halfGapY = std::max(2, page->gapY / 2);
            rect.left -= widget.gridCell.column > 0 ? halfGapX : 0;
            rect.top -= widget.gridCell.row > 0 ? halfGapY : 0;
            rect.right += (widget.gridCell.column + widget.gridSpan.columns) < page->columns ? halfGapX : 0;
            rect.bottom += (widget.gridCell.row + widget.gridSpan.rows) < page->rows ? halfGapY : 0;
            rect.left = std::max<LONG>(page->workArea.left, rect.left);
            rect.top = std::max<LONG>(page->workArea.top, rect.top);
            rect.right = std::min<LONG>(page->workArea.right, rect.right);
            rect.bottom = std::min<LONG>(page->workArea.bottom, rect.bottom);
        }
        if ((rect.right - rect.left) > 16 && (rect.bottom - rect.top) > 16)
        {
            InflateRect(&rect, -4, -4);
        }
        return rect;
    }

    RECT GetWidgetTitleRect(const DesktopWidget& widget) const
    {
        RECT handle = GetWidgetMoveHandleRect(widget);
        LONG left = handle.left + 4;
        const int reserved = widget.type == DesktopWidgetType::FolderMapping ? 76 : 26;
        LONG right = std::max<LONG>(left + 1, handle.right - reserved);
        return MakeRect(left, handle.top + 2, right, handle.bottom - 2);
    }

    RECT GetWidgetMoveHandleRect(const DesktopWidget& widget) const
    {
        RECT frame = GetWidgetFrameRect(widget);
        constexpr int handleHeight = 24;
        return MakeRect(
            frame.left + 4,
            std::max<LONG>(frame.top, frame.bottom - handleHeight - 2),
            frame.right - 4,
            frame.bottom - 2);
    }

    RECT GetWidgetResizeHandleRect(const DesktopWidget& widget) const
    {
        RECT handle = GetWidgetMoveHandleRect(widget);
        constexpr int handleWidth = 24;
        return MakeRect(
            std::max<LONG>(handle.left, handle.right - handleWidth),
            handle.top,
            handle.right,
            handle.bottom);
    }

    RECT GetWidgetBodyRect(const DesktopWidget& widget) const
    {
        RECT frame = GetWidgetFrameRect(widget);
        if (widget.type == DesktopWidgetType::Collection && widget.gridSpan.rows <= 1)
        {
            return frame;
        }
        frame.bottom = std::max<LONG>(frame.top + 24, frame.bottom - 24);
        return frame;
    }

    RECT GetWidgetHitRect(const DesktopWidget& widget) const
    {
        return GetWidgetFrameRect(widget);
    }

    RECT GetWidgetSelectionRect(const DesktopWidget& widget) const
    {
        return InflateCopy(GetWidgetHitRect(widget), 2);
    }

    int HitTestWidgetResizeEdges(const DesktopWidget& widget, POINT point) const
    {
        RECT handle = GetWidgetResizeHandleRect(widget);
        return PtInRect(&handle, point) ? (kResizeRight | kResizeBottom) : kResizeNone;
    }

    RECT GetCollectionPreviewSlotRect(const DesktopWidget& widget, size_t slot) const
    {
        RECT body = GetWidgetBodyRect(widget);
        if (IsRectEmptyRect(body))
        {
            return {};
        }

        if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1)
        {
            InflateRect(&body, -6, -6);
            const int columns = 2;
            const int rows = 2;
            const int bodyW = std::max<int>(1, static_cast<int>(body.right - body.left));
            const int bodyH = std::max<int>(1, static_cast<int>(body.bottom - body.top));
            const int gridSize = std::min(bodyW, bodyH);
            const GridPage* page = FindExactGridPage(widget.gridCell.pageId);
            const int gapY = page != nullptr ? page->gapY : 0;
            const int gridTop = body.top + gapY / 2 - 10;
            const int slotSz = std::max<int>(1, gridSize / 2);
            const int col = static_cast<int>(slot % columns);
            const int row = static_cast<int>(slot / columns);
            RECT rect = MakeRect(
                body.left + col * slotSz,
                gridTop + row * slotSz,
                col + 1 == columns ? body.right : body.left + (col + 1) * slotSz,
                row + 1 == rows ? gridTop + gridSize : gridTop + (row + 1) * slotSz);
            InflateRect(&rect, -1, -1);
            return rect;
        }

        InflateRect(&body, -4, -4);
        const GridPage* page = FindExactGridPage(widget.gridCell.pageId);
        const int cellH = page != nullptr ? page->cellHeight : GetDesktopCellHeight();
        const int gapY = page != nullptr ? page->gapY : 0;
        const int columns = std::max(1, widget.gridSpan.columns);
        const int rows = std::max(1, widget.gridSpan.rows);
        const int col = static_cast<int>(slot % static_cast<size_t>(columns));
        const int row = static_cast<int>(slot / static_cast<size_t>(columns));
        if (row >= rows)
        {
            return {};
        }

        const int width = std::max<int>(1, static_cast<int>(body.right - body.left) / columns);
        const int startY = body.top + gapY / 2 - 8;
        const int rowStep = cellH + gapY;
        RECT rect = MakeRect(
            body.left + col * width,
            startY + row * rowStep,
            col + 1 == columns ? body.right : body.left + (col + 1) * width,
            startY + row * rowStep + cellH);
        return rect;
    }

    size_t GetCollectionInlineCapacity(const DesktopWidget& widget) const
    {
        if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1)
        {
            return 4;
        }
        return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
    }

    size_t GetCollectionAllButtonSlot(const DesktopWidget& widget) const
    {
        if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1)
        {
            return static_cast<size_t>(-1);
        }
        return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
    }

    bool HandlePageNavigationClick(POINT point)
    {
        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr) return false;

        const bool hasPrev = pageOffset_ > 0;
        const bool hasNext = pageOffset_ < MaxPageOffset();
        if (!hasPrev && !hasNext) return false;

        constexpr LONG kButtonWidth = 68;
        constexpr LONG kButtonHeight = 28;
        constexpr LONG kGap = 8;
        constexpr LONG kPanelPaddingX = 10;
        constexpr LONG kPanelPaddingY = 8;

        const int visibleCount = (hasPrev ? 1 : 0) + (hasNext ? 1 : 0);
        const LONG kPanelWidth = kButtonWidth * visibleCount + kGap * (visibleCount - 1) + kPanelPaddingX * 2;
        const LONG kPanelHeight = kButtonHeight + kPanelPaddingY * 2;

        const LONG panelRight = std::max<LONG>(page->workArea.left + kPanelWidth,
            page->workArea.right - page->marginX - 10);
        const LONG panelBottom = std::max<LONG>(page->workArea.top + kPanelHeight,
            page->workArea.bottom - page->marginY - 10);
        const LONG panelLeft = panelRight - kPanelWidth;
        const LONG panelTop = panelBottom - kPanelHeight;

        LONG btnX = panelLeft + kPanelPaddingX;
        int delta = 0;
        if (hasPrev)
        {
            RECT prevRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&prevRect, point)) delta = -1;
            btnX += kButtonWidth + kGap;
        }
        if (hasNext && delta == 0)
        {
            RECT nextRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&nextRect, point)) delta = 1;
        }

        if (delta == 0) return false;

        pageOffset_ = std::clamp(pageOffset_ + delta, 0, MaxPageOffset());
        ApplyPageMapping();
        LayoutItems();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    void DrawD2DItemGeneric(ID2D1DeviceContext* context, RECT bounds, bool selected, HBITMAP hbm, const std::wstring& name)
    {
        if (IsRectEmptyRect(bounds))
        {
            return;
        }

        const bool hovered = PtInRect(&bounds, lastMousePoint_) != FALSE;
        if (hovered && !selected)
        {
            DrawD2DRoundedRectangle(context, bounds, 6.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
        }

        RECT iconRect = GetItemIconRect(bounds);
        const float iconX = static_cast<float>(iconRect.left);
        const float iconY = static_cast<float>(iconRect.top);
        const float drawSize = static_cast<float>(iconRect.right - iconRect.left);

        if (selected)
        {
            DrawD2DFilledRectangle(
                context,
                GetItemSelectionRect(bounds, true),
                D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f),
                D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f),
                nullptr);
        }

        ID2D1Bitmap1* iconBitmap = GetOrCreateD2DBitmap(hbm);
        if (iconBitmap != nullptr)
        {
            D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + drawSize, iconY + drawSize);
            context->DrawBitmap(iconBitmap, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }

        if (dwriteFactory_ && itemTextFormat_ && !name.empty())
        {
            RECT textRect = GetItemTextRect(bounds, selected);
            const float textWidth = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
            const float textHeight = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    name.c_str(),
                    static_cast<UINT32>(name.size()),
                    itemTextFormat_.Get(),
                    textWidth,
                    textHeight,
                    &layout)) &&
                layout)
            {
                ComPtr<ID2D1SolidColorBrush> shadowBrush;
                ComPtr<ID2D1SolidColorBrush> textBrush;
                context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), &shadowBrush);
                context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &textBrush);

                const float tx = static_cast<float>(textRect.left);
                const float ty = static_cast<float>(textRect.top);
                if (shadowBrush)
                {
                    const D2D1_POINT_2F offsets[] = {
                        D2D1::Point2F(tx - 1.0f, ty),
                        D2D1::Point2F(tx + 1.0f, ty),
                        D2D1::Point2F(tx, ty - 1.0f),
                        D2D1::Point2F(tx, ty + 1.0f),
                    };
                    for (const D2D1_POINT_2F& point : offsets)
                    {
                        context->DrawTextLayout(point, layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                if (textBrush)
                {
                    context->DrawTextLayout(D2D1::Point2F(tx, ty), layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    }

    void DrawD2DItemGenericList(ID2D1DeviceContext* context, RECT itemRect, bool selected, HBITMAP hbm, const std::wstring& name)
    {
        if (IsRectEmptyRect(itemRect))
        {
            return;
        }

        const bool hovered = PtInRect(&itemRect, lastMousePoint_) != FALSE;
        if (hovered && !selected)
        {
            DrawD2DRoundedRectangle(context, itemRect, 6.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
        }

        if (selected)
        {
            DrawD2DRoundedRectangle(
                context,
                itemRect,
                6.0f,
                D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.30f),
                D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.48f));
        }

        const int itemH = std::max<int>(1, static_cast<int>(itemRect.bottom - itemRect.top));
        const int iconSz = std::min(32, itemH - 4);
        RECT iconRect = MakeRect(itemRect.left + 4, itemRect.top + (itemH - iconSz) / 2,
            itemRect.left + 4 + iconSz, itemRect.top + (itemH + iconSz) / 2);

        ID2D1Bitmap1* iconBitmap = GetOrCreateD2DBitmap(hbm);
        if (iconBitmap != nullptr)
        {
            D2D1_RECT_F dst = D2D1::RectF(
                static_cast<float>(iconRect.left), static_cast<float>(iconRect.top),
                static_cast<float>(iconRect.right), static_cast<float>(iconRect.bottom));
            context->DrawBitmap(iconBitmap, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }

        if (dwriteFactory_ && listItemTextFormat_ && !name.empty())
        {
            RECT textRect = MakeRect(iconRect.right + 6, itemRect.top,
                itemRect.right - 6, itemRect.bottom);
            const float textWidth = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
            const float textHeight = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    name.c_str(),
                    static_cast<UINT32>(name.size()),
                    listItemTextFormat_.Get(),
                    textWidth,
                    textHeight,
                    &layout)) &&
                layout)
            {
                ComPtr<ID2D1SolidColorBrush> shadowBrush;
                ComPtr<ID2D1SolidColorBrush> textBrush;
                context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), &shadowBrush);
                context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &textBrush);

                const float tx = static_cast<float>(textRect.left);
                const float ty = static_cast<float>(textRect.top);
                if (shadowBrush)
                {
                    const D2D1_POINT_2F offsets[] = {
                        D2D1::Point2F(tx - 1.0f, ty),
                        D2D1::Point2F(tx + 1.0f, ty),
                        D2D1::Point2F(tx, ty - 1.0f),
                        D2D1::Point2F(tx, ty + 1.0f),
                    };
                    for (const D2D1_POINT_2F& point : offsets)
                    {
                        context->DrawTextLayout(point, layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                if (textBrush)
                {
                    context->DrawTextLayout(D2D1::Point2F(tx, ty), layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    }

    void DrawD2DItemAt(ID2D1DeviceContext* context, const DesktopItem& item, RECT bounds, bool selected)
    {
        DrawD2DItemGeneric(context, bounds, selected, item.iconBitmap, item.name);
    }

    void DrawD2DDraggedItems(ID2D1DeviceContext* context)
    {
        int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
        int dy = dragCurrentPoint_.y - mouseDownPoint_.y;
        for (const auto& item : items_)
        {
            if (!item.selected)
            {
                continue;
            }
            RECT sourceBounds = item.bounds;
            if (IsRectEmptyRect(sourceBounds) &&
                draggingCollectionMember_ &&
                mouseDownHit_ >= 0 &&
                &item == &items_[static_cast<size_t>(mouseDownHit_)])
            {
                sourceBounds = collectionDragStartBounds_;
            }
            if (IsRectEmptyRect(sourceBounds))
            {
                continue;
            }

            RECT moved = sourceBounds;
            OffsetRect(&moved, dx, dy);
            DrawD2DItemAt(context, item, moved, true);
        }
    }

    void DrawD2DStatus(ID2D1DeviceContext* context, const RECT& client)
    {
        if (!dwriteFactory_ || !statusTextFormat_ || client.right <= 0 || client.bottom <= 0)
        {
            return;
        }

        const std::wstring status = L"SnowDesktop 桌面验证  |  F5 重新加载  |  Esc 退出并恢复 Explorer 图标";
        const LONG left = std::max<LONG>(16, client.right - 620);
        const LONG right = std::max<LONG>(left + 1, client.right - 16);
        const LONG top = std::max<LONG>(0, client.bottom - 34);
        const LONG bottom = std::max<LONG>(top + 1, client.bottom - 12);
        const float width = static_cast<float>(right - left);
        const float height = static_cast<float>(bottom - top);

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
                status.c_str(),
                static_cast<UINT32>(status.size()),
                statusTextFormat_.Get(),
                width,
                height,
                &layout)) ||
            !layout)
        {
            return;
        }

        ComPtr<ID2D1SolidColorBrush> shadowBrush;
        ComPtr<ID2D1SolidColorBrush> textBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &shadowBrush);
        context->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.96f, 1.0f, 0.92f), &textBrush);
        if (shadowBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(left + 1), static_cast<float>(top + 1)),
                layout.Get(),
                shadowBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (textBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(left), static_cast<float>(top)),
                layout.Get(),
                textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void Draw(HDC hdc, const RECT& paintRect)
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT clippedPaint{};
        if (!IntersectRect(&clippedPaint, &client, &paintRect))
        {
            return;
        }

        int width = clippedPaint.right - clippedPaint.left;
        int height = clippedPaint.bottom - clippedPaint.top;

        HDC memoryDc = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

        HBRUSH transparentBrush = CreateSolidBrush(kTransparentKey);
        RECT localPaint{ 0, 0, width, height };
        FillRect(memoryDc, &localPaint, transparentBrush);
        DeleteObject(transparentBrush);
        SetViewportOrgEx(memoryDc, -clippedPaint.left, -clippedPaint.top, nullptr);

        HFONT font = CreateFontW(
            -15,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(memoryDc, font);
        SetBkMode(memoryDc, TRANSPARENT);

        for (const auto& item : items_)
        {
            if (draggingItems_ && item.selected)
            {
                continue;
            }
            if (!RectsIntersect(item.bounds, clippedPaint))
            {
                continue;
            }
            DrawItem(memoryDc, item);
        }

        if (draggingItems_)
        {
            DrawDraggedItems(memoryDc);
        }

        DrawStatus(memoryDc, client);

        if (marqueeActive_)
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 180, 255));
            HGDIOBJ oldPen = SelectObject(memoryDc, pen);
            HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
            HGDIOBJ oldNullBrush = SelectObject(memoryDc, nullBrush);
            Rectangle(memoryDc, marqueeRect_.left, marqueeRect_.top, marqueeRect_.right, marqueeRect_.bottom);
            SelectObject(memoryDc, oldNullBrush);
            SelectObject(memoryDc, oldPen);
            DeleteObject(pen);
        }

        if (draggingItems_ && !draggingOverNav_)
        {
            int gdiHandoff = HitTestForDropTarget(dragCurrentPoint_);
            bool gdiOverHandoff = (gdiHandoff >= 0 && !items_[static_cast<size_t>(gdiHandoff)].selected);
            if (!gdiOverHandoff)
            {
                RECT targetRect = GetGridRect(dragTargetCell_);
            targetRect.left += 3;
            targetRect.top += 3;
            targetRect.right -= 3;
            targetRect.bottom -= 3;

            HPEN pen = CreatePen(PS_DOT, 1, RGB(120, 180, 255));
            HGDIOBJ oldPen = SelectObject(memoryDc, pen);
            HGDIOBJ oldBrush = SelectObject(memoryDc, GetStockObject(NULL_BRUSH));
            Rectangle(memoryDc, targetRect.left, targetRect.top, targetRect.right, targetRect.bottom);
            SelectObject(memoryDc, oldBrush);
            SelectObject(memoryDc, oldPen);
            DeleteObject(pen);
            }
        }

        if (externalDragActive_)
        {
            DrawExternalDropTarget(memoryDc);
        }

        SetViewportOrgEx(memoryDc, 0, 0, nullptr);
        BitBlt(hdc, clippedPaint.left, clippedPaint.top, width, height, memoryDc, 0, 0, SRCCOPY);

        SelectObject(memoryDc, oldFont);
        DeleteObject(font);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
    }

    void ClampAlphaToColorKey(HBITMAP bitmap, COLORREF key)
    {
        if (bitmap == nullptr) return;
        BITMAP bm{};
        if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmBitsPixel != 32 || bm.bmBits == nullptr) return;

        int w = bm.bmWidth;
        int absH = std::abs(bm.bmHeight);
        auto* pixels = static_cast<std::uint32_t*>(bm.bmBits);
        size_t count = static_cast<size_t>(w) * static_cast<size_t>(absH);

        for (size_t i = 0; i < count; ++i)
        {
            std::uint8_t a = static_cast<std::uint8_t>((pixels[i] >> 24) & 0xff);
            std::uint8_t r = static_cast<std::uint8_t>((pixels[i] >> 16) & 0xff);
            std::uint8_t g = static_cast<std::uint8_t>((pixels[i] >> 8) & 0xff);
            std::uint8_t b = static_cast<std::uint8_t>(pixels[i] & 0xff);
            if (a < 250 && (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) < 150)
            {
                pixels[i] = 0;
            }
        }
        UNREFERENCED_PARAMETER(key);
    }

    void SnapPixelsToColorKey(HDC hdc, const RECT& rect, COLORREF key, int threshold) // unused, kept for reference
    {
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) return;

        BYTE keyR = GetRValue(key);
        BYTE keyG = GetGValue(key);
        BYTE keyB = GetBValue(key);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::vector<std::uint32_t> pixels(count);

        HBITMAP target = static_cast<HBITMAP>(GetCurrentObject(hdc, OBJ_BITMAP));
        if (target == nullptr || GetDIBits(hdc, target, rect.top, static_cast<UINT>(h),
                pixels.data(), &bi, DIB_RGB_COLORS) == 0)
        {
            return;
        }

        for (size_t i = 0; i < count; ++i)
        {
            BYTE r = static_cast<BYTE>((pixels[i] >> 16) & 0xff);
            BYTE g = static_cast<BYTE>((pixels[i] >> 8) & 0xff);
            BYTE b = static_cast<BYTE>(pixels[i] & 0xff);
            int dr = std::abs(static_cast<int>(r) - static_cast<int>(keyR));
            int dg = std::abs(static_cast<int>(g) - static_cast<int>(keyG));
            int db = std::abs(static_cast<int>(b) - static_cast<int>(keyB));
            if (dr + dg + db <= threshold)
            {
                pixels[i] = (pixels[i] & 0xff000000) |
                    keyB | (static_cast<std::uint32_t>(keyG) << 8) | (static_cast<std::uint32_t>(keyR) << 16);
            }
        }

        SetDIBits(hdc, target, rect.top, static_cast<UINT>(h), pixels.data(), &bi, DIB_RGB_COLORS);
    }

    void DrawAlphaRect(HDC hdc, const RECT& rect, COLORREF color, BYTE alpha)
    {
        int width = std::max(0, static_cast<int>(rect.right - rect.left));
        int height = std::max(0, static_cast<int>(rect.bottom - rect.top));
        if (width == 0 || height == 0)
        {
            return;
        }

        HDC alphaDc = CreateCompatibleDC(hdc);
        HBITMAP alphaBitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ oldBitmap = SelectObject(alphaDc, alphaBitmap);

        HBRUSH brush = CreateSolidBrush(color);
        RECT local{ 0, 0, width, height };
        FillRect(alphaDc, &local, brush);
        DeleteObject(brush);

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = alpha;
        AlphaBlend(hdc, rect.left, rect.top, width, height, alphaDc, 0, 0, width, height, blend);

        SelectObject(alphaDc, oldBitmap);
        DeleteObject(alphaBitmap);
        DeleteDC(alphaDc);
    }

    void DrawItem(HDC hdc, const DesktopItem& item)
    {
        DrawItemAt(hdc, item, item.bounds, item.selected);
    }

    void DrawItemAt(HDC hdc, const DesktopItem& item, RECT bounds, bool selected)
    {
        if (IsRectEmptyRect(bounds))
        {
            return;
        }

        const int iconX = bounds.left + (kCellWidth - kIconSize) / 2;
        const int iconY = bounds.top + 2;
        const int contentBottom = bounds.top + kTextTop + kTextHeight + 6;

        if (selected)
        {
            RECT highlight = MakeRect(bounds.left + 4, bounds.top, bounds.right - 4, contentBottom);

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(155, 200, 255));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, highlight.left, highlight.top, highlight.right, highlight.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        if (item.iconBitmap != nullptr)
        {
            BITMAP bm{};
            if (GetObjectW(item.iconBitmap, sizeof(bm), &bm) != 0 && bm.bmBitsPixel == 32)
            {
                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = 255;
                blend.AlphaFormat = AC_SRC_ALPHA;
                HDC iconDc = CreateCompatibleDC(hdc);
                HGDIOBJ oldBmp = SelectObject(iconDc, item.iconBitmap);
                AlphaBlend(hdc, iconX, iconY, kIconSize, kIconSize,
                    iconDc, 0, 0, bm.bmWidth, bm.bmHeight, blend);
                SelectObject(iconDc, oldBmp);
                DeleteDC(iconDc);
            }
        }
        else if (item.sysIconIndex >= 0 && sysImageList_)
        {
            IMAGELISTDRAWPARAMS params{};
            params.cbSize = sizeof(params);
            params.i = item.sysIconIndex;
            params.hdcDst = hdc;
            params.x = iconX;
            params.y = iconY;
            params.cx = kIconSize;
            params.cy = kIconSize;
            params.rgbBk = CLR_NONE;
            params.fStyle = ILD_SCALE | ILD_TRANSPARENT;
            sysImageList_->Draw(&params);
        }

        RECT textRect = MakeRect(bounds.left + 2, bounds.top + kTextTop, bounds.right - 2, bounds.top + kTextTop + kTextHeight);
        constexpr UINT textFlags = DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;
        RECT shadowRect = textRect;
        SetTextColor(hdc, RGB(0, 0, 0));
        OffsetRect(&shadowRect, -1, 0);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        shadowRect = textRect;
        OffsetRect(&shadowRect, 1, 0);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        shadowRect = textRect;
        OffsetRect(&shadowRect, 0, 1);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        shadowRect = textRect;
        OffsetRect(&shadowRect, 0, -1);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, item.name.c_str(), -1, &textRect, textFlags);
    }

    void DrawDraggedItems(HDC hdc)
    {
        int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
        int dy = dragCurrentPoint_.y - mouseDownPoint_.y;
        for (const auto& item : items_)
        {
            if (!item.selected)
            {
                continue;
            }

            RECT moved = item.bounds;
            OffsetRect(&moved, dx, dy);
            DrawItemAt(hdc, item, moved, true);
        }
    }

    void DrawExternalDropTarget(HDC hdc)
    {
        RECT target{};
        size_t folderMapping = FindFolderMappingAtPoint(externalDragPoint_);
        if (folderMapping < widgets_.size())
        {
            target = GetWidgetFrameRect(widgets_[folderMapping]);
        }
        else
        {
            int hit = HitTest(externalDragPoint_);
            if (hit >= 0)
            {
                target = items_[static_cast<size_t>(hit)].bounds;
            }
            else
            {
                target = GetGridRect(CellFromPoint(externalDragPoint_));
            }
        }

        target.left += 3;
        target.top += 3;
        target.right -= 3;
        target.bottom -= 3;

        HPEN pen = CreatePen(PS_DOT, 1, RGB(120, 180, 255));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, target.left, target.top, target.right, target.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    void DrawStatus(HDC hdc, const RECT& client)
    {
        std::wstring status = L"SnowDesktop 桌面验证  |  F5 重新加载  |  Esc 退出并恢复 Explorer 图标";
        RECT textRect = MakeRect(client.right - 620, client.bottom - 34, client.right - 16, client.bottom - 12);
        RECT shadowRect = textRect;
        OffsetRect(&shadowRect, 1, 1);

        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, status.c_str(), -1, &shadowRect, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(hdc, RGB(235, 245, 255));
        DrawTextW(hdc, status.c_str(), -1, &textRect, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (newMenuContextMenu_ && (message == WM_INITMENUPOPUP || message == WM_DRAWITEM || message == WM_MEASUREITEM))
        {
            if (SUCCEEDED(newMenuContextMenu_->HandleMenuMsg(message, wParam, lParam)))
                return 0;
        }
        if (activeContextMenu3_)
        {
            LRESULT result = 0;
            if (SUCCEEDED(activeContextMenu3_->HandleMenuMsg2(message, wParam, lParam, &result)))
            {
                return result;
            }
        }
        else if (activeContextMenu2_)
        {
            if (SUCCEEDED(activeContextMenu2_->HandleMenuMsg(message, wParam, lParam)))
            {
                return 0;
            }
        }

        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            DrawD2D();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            virtualWidth_ = LOWORD(lParam);
            virtualHeight_ = HIWORD(lParam);
            if (dcompVisual_)
            {
                HRESULT hr = CreateOrResizeCompositionSurface();
                if (FAILED(hr))
                {
                    lastGraphicsError_ = hr;
                }
            }
            UpdateLayoutWorkArea();
            ApplySavedGridDimensions();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
            virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
            virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (dcompVisual_)
            {
                HRESULT hr = CreateOrResizeCompositionSurface();
                if (FAILED(hr))
                {
                    lastGraphicsError_ = hr;
                }
            }
            UpdateLayoutWorkArea();
            ApplySavedGridDimensions();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_LBUTTONDOWN:
            OnLeftButtonDown(wParam, lParam);
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(wParam, lParam);
            return 0;
        case WM_MOUSEWHEEL:
            OnMouseWheel(wParam, lParam);
            return 0;
        case WM_MOUSELEAVE:
            if (navButtonsVisible_)
            {
                navButtonsVisible_ = false;
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            return 0;
        case WM_LBUTTONUP:
            OnLeftButtonUp(wParam, lParam);
            return 0;
        case WM_LBUTTONDBLCLK:
            OnDoubleClick(lParam);
            return 0;
        case WM_RBUTTONUP:
            OnRightButtonUp(lParam);
            return 0;
        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;
        case WM_SYSKEYDOWN:
            if ((draggingItems_ || draggingWidgetMember_) && wParam == VK_MENU)
            {
                dragHint_ = MakeInternalDragHint(dragCurrentPoint_);
                ShowDragHintWindow(dragCurrentPoint_, dragHint_);
            }
            if (wParam == VK_MENU) return 0;
            OnKeyDown(wParam);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if ((draggingItems_ || draggingWidgetMember_) && wParam == VK_MENU)
            {
                dragHint_ = MakeInternalDragHint(dragCurrentPoint_);
                ShowDragHintWindow(dragCurrentPoint_, dragHint_);
            }
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lParam) == renameEdit_)
            {
                HDC editDc = reinterpret_cast<HDC>(wParam);
                SetTextColor(editDc, RGB(24, 32, 42));
                SetBkColor(editDc, RGB(255, 255, 255));
                if (renameBackgroundBrush_ == nullptr)
                {
                    renameBackgroundBrush_ = CreateSolidBrush(RGB(255, 255, 255));
                }
                return reinterpret_cast<LRESULT>(renameBackgroundBrush_);
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case kTrayCallbackMessage:
            OnTrayCallback(lParam);
            return 0;
        case kShellChangeMessage:
            SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == 1)
            {
                DebugLog(L"WM_TIMER smoke-test exit");
                RequestExit();
                return 0;
            }
            if (wParam == kShellChangeTimerId)
            {
                KillTimer(hwnd_, kShellChangeTimerId);
                if (!mouseDown_ && !draggingItems_ && !draggingWidget_ && !resizingWidget_ && !reloading_)
                {
                    ReloadItems();
                }
                return 0;
            }
            if (wParam == kRecycleBinPollTimerId)
            {
                SHQUERYRBINFO info{};
                info.cbSize = sizeof(info);
                if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)))
                {
                    if (lastRecycleBinItemCount_ >= 0 && info.i64NumItems != lastRecycleBinItemCount_)
                    {
                        if (!mouseDown_ && !draggingItems_ && !draggingWidget_ && !resizingWidget_ && !reloading_)
                        {
                            ReloadItems();
                        }
                    }
                    lastRecycleBinItemCount_ = info.i64NumItems;
                }
                return 0;
            }
            if (wParam == kDesktopHostWatchTimerId)
            {
                WatchDesktopHost();
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_CLOSE:
            DebugLog(L"WM_CLOSE RequestExit");
            RequestExit();
            return 0;
        case WM_QUERYENDSESSION:
            DebugLog(L"WM_QUERYENDSESSION");
            exitRequested_ = true;
            return TRUE;
        case WM_ENDSESSION:
            DebugLog(L"WM_ENDSESSION");
            if (wParam != 0)
            {
                exitRequested_ = true;
            }
            return 0;
        case WM_DESTROY:
            DebugLogWindow(L"WM_DESTROY hwnd", hwnd_);
            if (renameEdit_ != nullptr)
            {
                CommitRename(true);
            }
            SaveLayoutSlots();
            HideDragHintWindow();
            DestroyDragHintWindow();
            UnregisterOleDropTarget();
            if (shellChangeRegId_ != 0)
            {
                SHChangeNotifyDeregister(shellChangeRegId_);
                shellChangeRegId_ = 0;
            }
            KillTimer(hwnd_, kShellChangeTimerId);
            KillTimer(hwnd_, kRecycleBinPollTimerId);
            if (!exitRequested_)
            {
                DebugLog(L"Desktop window destroyed - keeping process alive for control-window recovery");
                ResetDesktopWindowResources();
                SetTimer(controlHwnd_, kDesktopHostWatchTimerId, 500, nullptr);
                return 0;
            }
            else
            {
                DebugLog(L"WM_DESTROY exitRequested=true no relaunch");
                KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
                RemoveTrayIcon();
                if (faFontHandle_ != nullptr)
                {
                    RemoveFontMemResourceEx(faFontHandle_);
                    faFontHandle_ = nullptr;
                }
                if (faMenuFont_ != nullptr)
                {
                    DeleteObject(faMenuFont_);
                    faMenuFont_ = nullptr;
                }
                RestoreExplorerIcons();
                if (controlHwnd_ != nullptr && IsWindow(controlHwnd_))
                {
                    DestroyWindow(controlHwnd_);
                    controlHwnd_ = nullptr;
                }
                PostQuitMessage(0);
            }
            return 0;
        default:
            if (message == taskbarRestartMsg_ && message != 0)
            {
                DebugLog(L"TaskbarCreated - recovering");
                DebugLogWindow(L"TaskbarCreated hwnd before recovery", hwnd_);
                RecoverDesktopHostAfterExplorerRestart();
                DebugLogWindow(L"TaskbarCreated hwnd after recovery", hwnd_);
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void OnLeftButtonDown(WPARAM wParam, LPARAM lParam)
    {
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        SetFocus(hwnd_);
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        lastMousePoint_ = point;
        if (HandlePageNavigationClick(point))
        {
            return;
        }

        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (!PtInRect(&popup, point))
            {
                CloseCollectionPopup();
                return;
            }
        }

        SetCapture(hwnd_);
        mouseDown_ = true;
        marqueeActive_ = false;
        mouseDownPoint_ = point;
        dragCurrentPoint_ = mouseDownPoint_;
        marqueeRect_ = MakeRect(mouseDownPoint_.x, mouseDownPoint_.y, mouseDownPoint_.x, mouseDownPoint_.y);

        DesktopHit hit = HitTestDesktop(mouseDownPoint_);
        if (hit.kind == DesktopHitKind::WidgetFolderToggle && hit.widgetIndex < widgets_.size())
        {
            widgets_[hit.widgetIndex].listMode = !widgets_[hit.widgetIndex].listMode;
            widgets_[hit.widgetIndex].scrollOffset = 0;
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
            mouseDown_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            return;
        }
        if (hit.kind == DesktopHitKind::WidgetFolderOpen && hit.widgetIndex < widgets_.size())
        {
            OpenFolderMappingSource(hit.widgetIndex);
            mouseDown_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            return;
        }
        mouseDownHit_ = hit.kind == DesktopHitKind::Item ||
            hit.kind == DesktopHitKind::WidgetMember ||
            hit.kind == DesktopHitKind::PopupMember
            ? static_cast<int>(hit.itemIndex)
            : -1;
        mouseDownWidgetIndex_ = hit.widgetIndex;
        draggingItems_ = false;
        draggingWidget_ = false;
        resizingWidget_ = false;
        draggingCollectionMember_ = false;
        draggingOverNav_ = false;
        widgetResizeEdges_ = kResizeNone;
        bool ctrl = (wParam & MK_CONTROL) != 0;

        if (popupWidgetIndex_ < widgets_.size() &&
            hit.kind == DesktopHitKind::Widget &&
            hit.widgetIndex == popupWidgetIndex_ &&
            IsPointInsideOpenPopup(point))
        {
            if (!ctrl)
            {
                ClearSelection();
            }
            if (GetCapture() == hwnd_)
            {
                ReleaseCapture();
            }
            mouseDown_ = false;
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }

        if (hit.kind == DesktopHitKind::WidgetContent)
        {
            if (!ctrl)
            {
                ClearSelection();
                for (auto& w : widgets_)
                {
                    for (auto& e : w.folderEntries)
                    {
                        e.selected = false;
                    }
                }
            }
        }
        else if (hit.kind == DesktopHitKind::WidgetMember)
        {
            bool isFolderMapping = hit.widgetIndex < widgets_.size() &&
                widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping;
            if (isFolderMapping && hit.memberIndex < widgets_[hit.widgetIndex].folderEntries.size())
            {
                bool& sel = widgets_[hit.widgetIndex].folderEntries[hit.memberIndex].selected;
                if (ctrl)
                {
                    sel = !sel;
                }
                else if (!sel)
                {
                    ClearSelection();
                    for (auto& w : widgets_)
                        for (auto& e : w.folderEntries) e.selected = false;
                    sel = true;
                }
                draggingWidgetMember_ = true;
                widgetMemberDragWidget_ = hit.widgetIndex;
                widgetMemberInsertPreview_ = hit.memberIndex;
                dragGroupOriginX_ = hit.bounds.left;
                dragGroupOriginY_ = hit.bounds.top;
            }
            else if (hit.itemIndex != static_cast<size_t>(-1))
            {
                if (ctrl)
                {
                    ToggleSelection(hit.itemIndex);
                }
                else if (!items_[hit.itemIndex].selected)
                {
                    SelectOnly(hit.itemIndex);
                }
                collectionDragSourceWidget_ = hit.widgetIndex;
                collectionDragSourceMember_ = hit.memberIndex;
                collectionDragStartBounds_ = hit.bounds;
                draggingCollectionMember_ = true;
                dragGroupOriginX_ = hit.bounds.left;
                dragGroupOriginY_ = hit.bounds.top;
            }
        }
        else if (hit.kind == DesktopHitKind::PopupMember)
        {
            if (ctrl)
            {
                ToggleSelection(hit.itemIndex);
            }
            else if (!items_[hit.itemIndex].selected)
            {
                SelectOnly(hit.itemIndex);
            }
            collectionDragSourceWidget_ = hit.widgetIndex;
            collectionDragSourceMember_ = hit.memberIndex;
            collectionDragStartBounds_ = hit.bounds;
            draggingCollectionMember_ = true;
            dragGroupOriginX_ = hit.bounds.left;
            dragGroupOriginY_ = hit.bounds.top;
        }
        else if (hit.kind == DesktopHitKind::Item)
        {
            if (ctrl)
            {
                ToggleSelection(hit.itemIndex);
            }
            else if (!items_[hit.itemIndex].selected)
            {
                SelectOnly(hit.itemIndex);
            }

            UpdateDragGroupOrigin(hit.itemIndex);
        }
        else if (hit.kind == DesktopHitKind::Widget || hit.kind == DesktopHitKind::WidgetAllButton)
        {
            if (hit.widgetIndex < widgets_.size())
            {
                SelectWidgetOnly(hit.widgetIndex);
                for (auto& w : widgets_)
                    for (auto& e : w.folderEntries) e.selected = false;
                widgetDragOriginalCell_ = widgets_[hit.widgetIndex].gridCell;
                widgetDragOriginalSpan_ = widgets_[hit.widgetIndex].gridSpan;
                widgetPreviewCell_ = widgetDragOriginalCell_;
                widgetPreviewSpan_ = widgetDragOriginalSpan_;
                RECT bounds = widgets_[hit.widgetIndex].bounds;
                dragGroupOriginX_ = bounds.left;
                dragGroupOriginY_ = bounds.top;
                widgetResizeEdges_ = hit.kind == DesktopHitKind::Widget
                    ? HitTestWidgetResizeEdges(widgets_[hit.widgetIndex], point)
                    : kResizeNone;
            }
        }
        else if (!ctrl)
        {
            ClearSelection();
            for (auto& w : widgets_)
                for (auto& e : w.folderEntries) e.selected = false;
        }

        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OnMouseMove(WPARAM, LPARAM lParam)
    {
        POINT current{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        POINT oldMousePoint = lastMousePoint_;
        lastMousePoint_ = current;
        UpdateNavButtonHover(current);

        if (!mouseDown_)
        {
            if (oldMousePoint.x != current.x || oldMousePoint.y != current.y)
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (std::abs(current.x - mouseDownPoint_.x) > 3 || std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            int hit = mouseDownHit_;
            {
                wchar_t buf[128];
                swprintf_s(buf, L"MOUSEMOVE hit=%d dwm=%d dgc=%d wid=%zu widSel=%d",
                    hit, draggingWidgetMember_, draggingCollectionMember_,
                    mouseDownWidgetIndex_,
                    mouseDownWidgetIndex_ < widgets_.size() ? widgets_[mouseDownWidgetIndex_].selected : -1);
                DebugLog(buf);
            }
            if (mouseDownWidgetIndex_ < widgets_.size() && widgets_[mouseDownWidgetIndex_].selected)
            {
                dragCurrentPoint_ = current;
                if (widgetResizeEdges_ != kResizeNone)
                {
                    resizingWidget_ = true;
                    dragHint_ = L"释放：调整组件大小";
                }
                else
                {
                    draggingWidget_ = true;
                    dragHint_ = L"释放：移动组件并挤占原位置";
                }
                UpdateWidgetPreviewFromPoint(current);
                if (widgetPreviewOccupied_)
                {
                    dragHint_ = resizingWidget_ ? L"释放：已到达最小尺寸或位置被占用" : L"此处已被占用";
                }
                ShowDragHintWindow(dragCurrentPoint_, dragHint_);
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            else if (draggingWidgetMember_ && widgetMemberDragWidget_ < widgets_.size())
            {
                draggingItems_ = true;
                dragCurrentPoint_ = current;
                DesktopHit memberHit = HitTestDesktop(current);
                bool insideSource = (memberHit.kind == DesktopHitKind::WidgetMember ||
                                     memberHit.kind == DesktopHitKind::WidgetContent) &&
                                    memberHit.widgetIndex == widgetMemberDragWidget_;

                POINT screenPt = current;
                ClientToScreen(hwnd_, &screenPt);
                int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                int virtualL = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int virtualT = GetSystemMetrics(SM_YVIRTUALSCREEN);
                bool nearEdge = (screenPt.x <= virtualL + 3) || (screenPt.x >= virtualL + virtualW - 3) ||
                    (screenPt.y <= virtualT + 3) || (screenPt.y >= virtualT + virtualH - 3);
                bool overExternalDropWindow = IsExternalDropWindowAt(current);

                if (nearEdge || overExternalDropWindow)
                {
                    ComPtr<IDataObject> dataObj = CreateFolderEntriesDataObject(widgetMemberDragWidget_);
                    if (dataObj)
                    {
                        size_t sourceWidget = widgetMemberDragWidget_;
                        HideDragHintWindow();
                        if (GetCapture() == hwnd_) ReleaseCapture();
                        mouseDown_ = false;
                        draggingItems_ = false;
                        draggingWidgetMember_ = false;
                        widgetMemberDragWidget_ = static_cast<size_t>(-1);
                        widgetMemberInsertPreview_ = static_cast<size_t>(-1);
                        dragHint_.clear();
                        InvalidateRect(hwnd_, nullptr, TRUE);

                        DWORD oleEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                        DoDragDrop(dataObj.Get(), static_cast<IDropSource*>(this), oleEffect, &oleEffect);
                        RefreshFolderMappingWidget(sourceWidget);
                        return;
                    }
                }

                if (memberHit.kind == DesktopHitKind::WidgetMember)
                {
                    if (memberHit.itemIndex < items_.size() && !items_[memberHit.itemIndex].selected)
                    {
                        dragHint_ = L"释放：交给「" + items_[memberHit.itemIndex].name + L"」处理";
                    }
                    else if (memberHit.widgetIndex < widgets_.size() &&
                        widgets_[memberHit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                        memberHit.memberIndex < widgets_[memberHit.widgetIndex].folderEntries.size() &&
                        !widgets_[memberHit.widgetIndex].folderEntries[memberHit.memberIndex].selected)
                    {
                        dragHint_ = L"释放：交给「" + widgets_[memberHit.widgetIndex].folderEntries[memberHit.memberIndex].name + L"」处理";
                    }
                    else
                    {
                        if (insideSource)
                            dragHint_ = L"释放：重新排序";
                        else
                        {
                            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                            dragHint_ = altDown ? L"释放：创建快捷方式到桌面" : L"释放：复制到桌面";
                        }
                    }
                }
                else
                {
                    if (!insideSource && IsWidgetDropSurface(memberHit))
                    {
                        const DesktopWidget& targetWidget = widgets_[memberHit.widgetIndex];
                        if (targetWidget.type == DesktopWidgetType::Collection)
                            dragHint_ = L"释放：加入集合「" + targetWidget.title + L"」";
                        else if (targetWidget.type == DesktopWidgetType::FileCategories)
                            dragHint_ = L"释放：加入桌面文件「" + targetWidget.title + L"」";
                        else if (targetWidget.type == DesktopWidgetType::FolderMapping)
                        {
                            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                            dragHint_ = altDown
                                ? L"释放：创建快捷方式到「" + targetWidget.title + L"」"
                                : L"释放：复制到映射文件夹「" + targetWidget.title + L"」";
                        }
                        else
                        {
                            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                            dragHint_ = altDown ? L"释放：创建快捷方式到桌面" : L"释放：复制到桌面";
                        }
                    }
                    else
                    {
                        if (insideSource)
                            dragHint_ = L"释放：重新排序";
                        else
                        {
                            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                            dragHint_ = altDown ? L"释放：创建快捷方式到桌面" : L"释放：复制到桌面";
                        }
                    }
                }
                ShowDragHintWindow(dragCurrentPoint_, dragHint_);
                size_t insertIdx = GetWidgetMemberInsertIndex(widgetMemberDragWidget_, current);
                widgetMemberInsertPreview_ = insertIdx;
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            else if (hit >= 0 && items_[static_cast<size_t>(hit)].selected)
            {
                RECT oldDirty = draggingItems_ ? GetInternalDragDirtyRect(dragCurrentPoint_) : GetSelectedDragBoundsAt(mouseDownPoint_);
                draggingItems_ = true;
                dragCurrentPoint_ = current;

                POINT screenPt = current;
                ClientToScreen(hwnd_, &screenPt);
                int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                int virtualL = GetSystemMetrics(SM_XVIRTUALSCREEN);
                int virtualT = GetSystemMetrics(SM_YVIRTUALSCREEN);
                bool nearEdge = (screenPt.x <= virtualL + 3) || (screenPt.x >= virtualL + virtualW - 3) ||
                    (screenPt.y <= virtualT + 3) || (screenPt.y >= virtualT + virtualH - 3);
                bool overExternalDropWindow = IsExternalDropWindowAt(current);

                if (nearEdge || overExternalDropWindow)
                {
                    ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
                    if (dataObj)
                    {
                        HideDragHintWindow();
                        if (GetCapture() == hwnd_) ReleaseCapture();
                        mouseDown_ = false;
                        draggingItems_ = false;
                        dragHint_.clear();
                        InvalidateRect(hwnd_, nullptr, TRUE);

                        DWORD oleEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                        selfDragActive_ = true;
                        selfDragReturned_ = false;
                        selfDragOutKeys_.clear();
                        for (size_t i = 0; i < items_.size(); ++i)
                            if (items_[i].selected) selfDragOutKeys_.push_back(items_[i].layoutKey);
                        DebugLog(L"SELFDRAG DoDragDrop start");
                        HRESULT hr = DoDragDrop(dataObj.Get(), static_cast<IDropSource*>(this), oleEffect, &oleEffect);
                        DebugLog(L"SELFDRAG DoDragDrop end");
                        selfDragActive_ = false;
                        if (hr == DRAGDROP_S_DROP && oleEffect == DROPEFFECT_MOVE && !selfDragReturned_)
                        {
                            RemoveSelectedItemsFromDesktop();
                            SaveLayoutSlots();
                        }
                        if (!selfDragReturned_)
                        {
                            ClearSelection();
                            ReloadItems();
                        }
                        else
                        {
                            SaveLayoutSlots();
                            ClearSelection();
                            InvalidateRect(hwnd_, nullptr, TRUE);
                        }
                        return;
                    }
                }

                int navDelta = HitTestNavButton(current);
                if (navDelta != 0)
                {
                    draggingOverNav_ = true;
                    dragHint_ = navDelta < 0 ? L"释放：移动到上一页" : L"释放：移动到下一页";
                }
                else
                {
                    draggingOverNav_ = false;
                    dragTargetCell_ = FindBestDropCell(CellFromPoint(GetDragTargetPoint(current)));
                    dragHint_ = MakeInternalDragHint(current);
                }
                ShowDragHintWindow(dragCurrentPoint_, dragHint_);
                InvalidateFast(UnionCopy(oldDirty, GetInternalDragDirtyRect(current)));
            }
            else if (hit < 0)
            {
                marqueeActive_ = true;
                marqueeRect_ = NormalizeRect(mouseDownPoint_, current);
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
    }

    void OnMouseWheel(WPARAM wParam, LPARAM lParam)
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd_, &point);
        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popup, point))
            {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                const int maxScroll = GetCollectionPopupMaxScrollOffset(widgets_[popupWidgetIndex_], popup);
                popupScrollOffset_ = std::clamp(popupScrollOffset_ - delta / 2, 0, maxScroll);
                InvalidateRect(hwnd_, nullptr, TRUE);
                return;
            }
        }

        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        for (auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::FileCategories)
            {
                RECT tabsRect = GetFileCategoryTabsRect(widget);
                if (PtInRect(&tabsRect, point))
                {
                    std::vector<std::wstring> tabs = GetVisibleFileCategoryIds(widget);
                    constexpr int minTabWidth = 64;
                    const int tabCount = static_cast<int>(tabs.size());
                    const int equalWidth = std::max<int>(1, static_cast<int>(tabsRect.right - tabsRect.left) / std::max(1, tabCount));
                    const int tabWidth = std::max(minTabWidth, equalWidth);
                    const int totalWidth = tabWidth * tabCount;
                    const int maxScroll = std::max(0, totalWidth - static_cast<int>(tabsRect.right - tabsRect.left));
                    widget.tabScrollOffset = std::clamp(widget.tabScrollOffset - delta, 0, maxScroll);
                    InvalidateRect(hwnd_, nullptr, TRUE);
                    return;
                }

                RECT content = GetFileCategoryContentRect(widget);
                if (!PtInRect(&content, point))
                {
                    continue;
                }
                const int maxScroll = GetFileCategoryMaxScrollOffset(widget);
                widget.scrollOffset = std::clamp(widget.scrollOffset - delta / 2, 0, maxScroll);
                InvalidateRect(hwnd_, nullptr, TRUE);
                return;
            }
            if (widget.type == DesktopWidgetType::FolderMapping)
            {
                RECT content = GetFolderMappingContentRect(widget);
                if (!PtInRect(&content, point))
                {
                    continue;
                }
                const int maxScroll = GetFolderMappingMaxScrollOffset(widget);
                widget.scrollOffset = std::clamp(widget.scrollOffset - delta / 2, 0, maxScroll);
                InvalidateRect(hwnd_, nullptr, TRUE);
                return;
            }
        }
    }

    void OnLeftButtonUp(WPARAM wParam, LPARAM lParam)
    {
        if (GetCapture() == hwnd_)
        {
            ReleaseCapture();
        }

        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        {
            wchar_t buf[128];
            swprintf_s(buf, L"LBUTTONUP dw=%d rs=%d dwm=%d di=%d dgc=%d",
                draggingWidget_, resizingWidget_, draggingWidgetMember_,
                draggingItems_, draggingCollectionMember_);
            DebugLog(buf);
        }

        if (draggingWidget_ || resizingWidget_)
        {
            if (mouseDownWidgetIndex_ < widgets_.size())
            {
                UpdateWidgetPreviewFromPoint(point);
                PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_);
            }
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (draggingWidgetMember_ && widgetMemberDragWidget_ < widgets_.size())
        {
            DesktopHit dropHit = HitTestDesktop(point);
            bool isSameWidget = (dropHit.kind == DesktopHitKind::WidgetMember ||
                                 dropHit.kind == DesktopHitKind::WidgetContent) &&
                                dropHit.widgetIndex == widgetMemberDragWidget_;
            bool handled = false;
            if (dropHit.kind == DesktopHitKind::WidgetMember)
            {
                bool canHandoff = false;
                if (dropHit.itemIndex < items_.size())
                {
                    canHandoff = !items_[dropHit.itemIndex].selected;
                }
                else if (dropHit.widgetIndex < widgets_.size() &&
                    widgets_[dropHit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                    dropHit.memberIndex < widgets_[dropHit.widgetIndex].folderEntries.size())
                {
                    canHandoff = !widgets_[dropHit.widgetIndex].folderEntries[dropHit.memberIndex].selected;
                }

                if (canHandoff)
                {
                    ComPtr<IDataObject> dataObject = CreateFolderEntriesDataObject(widgetMemberDragWidget_);
                    if (dataObject)
                    {
                        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                        if (SUCCEEDED(DropDataObjectToWidgetMember(dropHit, dataObject.Get(), point, MK_LBUTTON, &effect)))
                        {
                            RefreshFolderMappingWidget(widgetMemberDragWidget_);
                            if (dropHit.widgetIndex < widgets_.size() &&
                                dropHit.widgetIndex != widgetMemberDragWidget_ &&
                                widgets_[dropHit.widgetIndex].type == DesktopWidgetType::FolderMapping)
                            {
                                RefreshFolderMappingWidget(dropHit.widgetIndex);
                            }
                            handled = true;
                        }
                    }
                }
            }

            if (!handled && isSameWidget)
            {
                FinishWidgetMemberReorder(widgetMemberDragWidget_, widgetMemberInsertPreview_);
            }
            else if (!handled)
            {
                DesktopHit targetHit = HitTestDesktop(point);
                bool isWidgetTarget = (targetHit.kind == DesktopHitKind::Widget ||
                     targetHit.kind == DesktopHitKind::WidgetMember ||
                     targetHit.kind == DesktopHitKind::WidgetContent ||
                     targetHit.kind == DesktopHitKind::WidgetAllButton) &&
                    targetHit.widgetIndex < widgets_.size() &&
                    targetHit.widgetIndex != widgetMemberDragWidget_;

                if (isWidgetTarget)
                {
                    const DesktopWidget& targetWidget = widgets_[targetHit.widgetIndex];
                    if (targetWidget.type == DesktopWidgetType::FolderMapping)
                    {
                        std::wstring targetFolder = targetWidget.sourceFolderPath;
                        if (!targetFolder.empty() && targetFolder.back() != L'\\')
                            targetFolder += L'\\';
                        std::vector<std::wstring> paths = GetSelectedFolderEntryPaths(widgetMemberDragWidget_);
                        if (!paths.empty())
                        {
                            if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
                            {
                                for (const auto& filePath : paths)
                                {
                                    std::wstring name = PathFindFileNameW(filePath.c_str());
                                    std::wstring linkPath = targetFolder + name + L".lnk";
                                    ComPtr<IShellLinkW> shellLink;
                                    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                                    {
                                        shellLink->SetPath(filePath.c_str());
                                        ComPtr<IPersistFile> persistFile;
                                        if (SUCCEEDED(shellLink.As(&persistFile)))
                                            persistFile->Save(linkPath.c_str(), TRUE);
                                    }
                                }
                            }
                            else
                            {
                                CopyFilesToFolder(paths, targetFolder, false);
                            }
                            RefreshFolderMappingWidget(targetHit.widgetIndex);
                        }
                    }
                    else if (targetWidget.type == DesktopWidgetType::Collection ||
                             targetWidget.type == DesktopWidgetType::FileCategories)
                    {
                        std::vector<std::wstring> paths = GetSelectedFolderEntryPaths(widgetMemberDragWidget_);
                        if (!paths.empty())
                        {
                            wchar_t desktopPath[MAX_PATH]{};
                            if (SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
                            {
                                std::wstring destDir = desktopPath;
                                if (!destDir.empty() && destDir.back() != L'\\') destDir += L'\\';
                                std::unordered_set<std::wstring> existingKeys = SnapshotDesktopItemKeys();
                                bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                                if (altDown)
                                {
                                    for (const auto& filePath : paths)
                                    {
                                        std::wstring name = PathFindFileNameW(filePath.c_str());
                                        std::wstring linkPath = destDir + name + L".lnk";
                                        ComPtr<IShellLinkW> shellLink;
                                        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                                        {
                                            shellLink->SetPath(filePath.c_str());
                                            ComPtr<IPersistFile> persistFile;
                                            if (SUCCEEDED(shellLink.As(&persistFile)))
                                                persistFile->Save(linkPath.c_str(), TRUE);
                                        }
                                    }
                                    SHChangeNotify(SHCNE_CREATE, SHCNF_PATH, desktopPath, nullptr);
                                }
                                else
                                {
                                    CopyFilesToFolder(paths, destDir, false);
                                    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, desktopPath, nullptr);
                                }
                                ReloadItems(false);
                                ClearSelection();
                                selectedCount_ = 0;
                                for (auto& item : items_)
                                {
                                    if (!existingKeys.contains(item.layoutKey) && !item.layoutKey.empty())
                                    {
                                        item.selected = true;
                                        ++selectedCount_;
                                    }
                                }
                                if (selectedCount_ > 0)
                                {
                                    if (targetWidget.type == DesktopWidgetType::Collection)
                                    {
                                        size_t insertIndex = GetCollectionInsertIndex(targetHit.widgetIndex, point, false);
                                        MoveSelectedItemsToCollection(targetHit.widgetIndex, insertIndex, static_cast<size_t>(-1));
                                    }
                                    else
                                    {
                                        AddSelectedItemsToFileCategoryWidget(targetHit.widgetIndex);
                                        size_t insertIndex = GetWidgetMemberInsertIndex(targetHit.widgetIndex, point);
                                        ReorderFileCategoryWidget(targetHit.widgetIndex, insertIndex);
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths(widgetMemberDragWidget_);
                    if (!paths.empty())
                    {
                        wchar_t desktopPath[MAX_PATH]{};
                        if (SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
                        {
                            std::wstring destDir = desktopPath;
                            if (!destDir.empty() && destDir.back() != L'\\') destDir += L'\\';
                            std::unordered_set<std::wstring> existingKeys = SnapshotDesktopItemKeys();
                            bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                            if (altDown)
                            {
                                for (const auto& filePath : paths)
                                {
                                    std::wstring name = PathFindFileNameW(filePath.c_str());
                                    std::wstring linkPath = destDir + name + L".lnk";
                                    ComPtr<IShellLinkW> shellLink;
                                    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                                    {
                                        shellLink->SetPath(filePath.c_str());
                                        ComPtr<IPersistFile> persistFile;
                                        if (SUCCEEDED(shellLink.As(&persistFile)))
                                            persistFile->Save(linkPath.c_str(), TRUE);
                                    }
                                }
                                SHChangeNotify(SHCNE_CREATE, SHCNF_PATH, desktopPath, nullptr);
                            }
                            else
                            {
                                CopyFilesToFolder(paths, destDir, false);
                                SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, desktopPath, nullptr);
                            }
                            ReloadItems(false);
                            PlaceNewDesktopItemsAtDropPoint(existingKeys, point);
                        }
                    }
                }
                RefreshFolderMappingWidget(widgetMemberDragWidget_);
            }
            draggingWidgetMember_ = false;
            widgetMemberDragWidget_ = static_cast<size_t>(-1);
            widgetMemberInsertPreview_ = static_cast<size_t>(-1);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (draggingItems_)
        {
            int navDelta = HitTestNavButton(point);
            if (navDelta != 0)
            {
                pageOffset_ = std::clamp(pageOffset_ + navDelta, 0, MaxPageOffset());
                ApplyPageMapping();
                for (auto& item : items_)
                {
                    if (item.selected)
                    {
                        item.gridCell.pageId = lastMonitorPageId_;
                        item.gridCell.column = 0;
                        item.gridCell.row = 0;
                    }
                }
                SaveLayoutSlots();
                ReloadItems(false);
            }
            else
            {
                bool handledCollectionDrop = false;
                DesktopHit memberDropHit = HitTestDesktop(point);
                {
                    wchar_t buf[256];
                    swprintf_s(buf, L"DROP kind=%d dragColl=%d srcW=%zu selCnt=%d",
                        static_cast<int>(memberDropHit.kind), draggingCollectionMember_,
                        collectionDragSourceWidget_, selectedCount_);
                    DebugLog(buf);
                }
                if (memberDropHit.kind == DesktopHitKind::WidgetMember)
                {
                    bool isFileCategoryReorder = draggingCollectionMember_ &&
                        collectionDragSourceWidget_ < widgets_.size() &&
                        widgets_[collectionDragSourceWidget_].type == DesktopWidgetType::FileCategories &&
                        memberDropHit.widgetIndex == collectionDragSourceWidget_;

                    if (!isFileCategoryReorder)
                    {
                    {
                        wchar_t buf[64];
                        swprintf_s(buf, L"  handoff skip=%d", isFileCategoryReorder);
                        DebugLog(buf);
                    }
                    bool canHandoff = false;
                    if (memberDropHit.itemIndex < items_.size())
                    {
                        canHandoff = !items_[memberDropHit.itemIndex].selected;
                    }
                    else if (memberDropHit.widgetIndex < widgets_.size() &&
                        widgets_[memberDropHit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                        memberDropHit.memberIndex < widgets_[memberDropHit.widgetIndex].folderEntries.size())
                    {
                        canHandoff = !widgets_[memberDropHit.widgetIndex].folderEntries[memberDropHit.memberIndex].selected;
                    }

                    if (canHandoff && IsPointInIconDropTarget(memberDropHit.bounds, point))
                    {
                        ComPtr<IDataObject> dataObject = CreateSelectedDataObject();
                        if (dataObject)
                        {
                            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                            if (SUCCEEDED(DropDataObjectToWidgetMember(memberDropHit, dataObject.Get(), point, MK_LBUTTON, &effect)))
                            {
                                ReloadItems();
                                if (memberDropHit.widgetIndex < widgets_.size() &&
                                    widgets_[memberDropHit.widgetIndex].type == DesktopWidgetType::FolderMapping)
                                {
                                    RefreshFolderMappingWidget(memberDropHit.widgetIndex);
                                }
                                handledCollectionDrop = true;
                                MessageBeep(MB_ICONASTERISK);
                            }
                        }
                    }
                    }
                }

                if (!handledCollectionDrop &&
                    draggingCollectionMember_ &&
                    collectionDragSourceWidget_ < widgets_.size() &&
                    widgets_[collectionDragSourceWidget_].type == DesktopWidgetType::FileCategories)
                {
                    DesktopHit fcHit = HitTestDesktop(point);
                    {
                        wchar_t buf[128];
                        swprintf_s(buf, L"  FCcheck fckind=%d fcW=%zu srcW=%zu handled=%d",
                            static_cast<int>(fcHit.kind), fcHit.widgetIndex,
                            collectionDragSourceWidget_, handledCollectionDrop);
                        DebugLog(buf);
                    }
                    if ((fcHit.kind == DesktopHitKind::WidgetMember ||
                         fcHit.kind == DesktopHitKind::WidgetContent) &&
                        fcHit.widgetIndex == collectionDragSourceWidget_)
                    {
                        size_t insertIndex = GetWidgetMemberInsertIndex(collectionDragSourceWidget_, point);
                        {
                            wchar_t buf[64];
                            swprintf_s(buf, L"  EXEC insertIdx=%zu selCnt=%d", insertIndex, selectedCount_);
                            DebugLog(buf);
                        }
                        ReorderFileCategoryWidget(collectionDragSourceWidget_, insertIndex);
                        handledCollectionDrop = true;
                    }
                }

                if (!handledCollectionDrop && popupWidgetIndex_ < widgets_.size())
                {
                    RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
                    if (PtInRect(&popup, point) &&
                        widgets_[popupWidgetIndex_].type == DesktopWidgetType::Collection)
                    {
                        size_t insertIndex = GetCollectionInsertIndex(popupWidgetIndex_, point, true);
                        MoveSelectedItemsToCollection(popupWidgetIndex_, insertIndex, collectionDragSourceWidget_);
                        handledCollectionDrop = true;
                    }
                }

                if (!handledCollectionDrop)
                {
                    DesktopHit fcDropHit = HitTestDesktop(point);
                    if ((fcDropHit.kind == DesktopHitKind::Widget ||
                         fcDropHit.kind == DesktopHitKind::WidgetMember ||
                         fcDropHit.kind == DesktopHitKind::WidgetContent ||
                         fcDropHit.kind == DesktopHitKind::WidgetAllButton) &&
                        fcDropHit.widgetIndex < widgets_.size() &&
                        widgets_[fcDropHit.widgetIndex].type == DesktopWidgetType::FileCategories)
                    {
                        if (draggingCollectionMember_ && collectionDragSourceWidget_ < widgets_.size())
                            RemoveSelectedItemsFromCollections(collectionDragSourceWidget_);
                        AddSelectedItemsToFileCategoryWidget(fcDropHit.widgetIndex);
                        handledCollectionDrop = true;
                    }
                }

                if (!handledCollectionDrop)
                {
                    DesktopHit fmDropHit = HitTestDesktop(point);
                    if ((fmDropHit.kind == DesktopHitKind::Widget ||
                         fmDropHit.kind == DesktopHitKind::WidgetMember ||
                         fmDropHit.kind == DesktopHitKind::WidgetContent ||
                         fmDropHit.kind == DesktopHitKind::WidgetAllButton) &&
                        fmDropHit.widgetIndex < widgets_.size() &&
                        widgets_[fmDropHit.widgetIndex].type == DesktopWidgetType::FolderMapping)
                    {
                        if (draggingCollectionMember_ && collectionDragSourceWidget_ < widgets_.size())
                            RemoveSelectedItemsFromCollections(collectionDragSourceWidget_);
                        CopyDesktopItemsToFolderMapping(fmDropHit.widgetIndex);
                        handledCollectionDrop = true;
                    }
                }

                if (!handledCollectionDrop)
                {
                    DesktopHit dropHit = HitTestDesktop(point);
                    if ((dropHit.kind == DesktopHitKind::Widget ||
                        dropHit.kind == DesktopHitKind::WidgetMember ||
                        dropHit.kind == DesktopHitKind::WidgetContent ||
                        dropHit.kind == DesktopHitKind::WidgetAllButton) &&
                        dropHit.widgetIndex < widgets_.size() &&
                        widgets_[dropHit.widgetIndex].type == DesktopWidgetType::Collection)
                    {
                        size_t insertIndex = GetCollectionInsertIndex(dropHit.widgetIndex, point, false);
                        MoveSelectedItemsToCollection(dropHit.widgetIndex, insertIndex, collectionDragSourceWidget_);
                    }
                    else if (dropHit.kind == DesktopHitKind::Item && !items_[dropHit.itemIndex].selected &&
                        HitTestForDropTarget(point) == static_cast<int>(dropHit.itemIndex))
                    {
                        ComPtr<IDataObject> dataObject = CreateSelectedDataObject();
                        if (dataObject)
                        {
                            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                            DropDataObjectAt(dataObject.Get(), point, MK_LBUTTON, &effect);
                            ReloadItems();
                        }
                    }
                    else
                    {
                        if (draggingCollectionMember_ && collectionDragSourceWidget_ < widgets_.size() && mouseDownHit_ >= 0)
                        {
                            RemoveSelectedItemsFromCollections(collectionDragSourceWidget_);
                        }
                        MoveSelectedItemsToCell(FindBestDropCell(CellFromPoint(GetDragTargetPoint(point))));
                        LayoutItems();
                    }
                }
            }
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (marqueeActive_)
        {
            bool ctrl = (wParam & MK_CONTROL) != 0;
            if (!ctrl)
            {
                ClearSelection();
                for (auto& w : widgets_)
                    for (auto& e : w.folderEntries) e.selected = false;
            }

            for (auto& item : items_)
            {
                if (IsRectEmptyRect(item.bounds))
                {
                    continue;
                }
                if (RectsIntersect(GetItemSelectionRect(item, false), marqueeRect_) && !item.selected)
                {
                    item.selected = true;
                    ++selectedCount_;
                }
            }

            if (mouseDownWidgetIndex_ < widgets_.size())
            {
                DesktopWidget& w = widgets_[mouseDownWidgetIndex_];
                if (w.type == DesktopWidgetType::FileCategories)
                {
                    std::wstring active = GetActiveFileCategoryId(w);
                    std::vector<std::wstring> keys = GetFileCategoryKeys(w, active);
                    RECT content = GetFileCategoryContentRect(w);
                    for (size_t i = 0; i < keys.size(); ++i)
                    {
                        RECT r = GetFileCategoryItemRect(w, i);
                        if (RectsIntersect(r, content) && RectsIntersect(r, marqueeRect_))
                        {
                            size_t idx = FindItemIndexByKey(keys[i]);
                            if (idx != static_cast<size_t>(-1) && !items_[idx].selected)
                            {
                                items_[idx].selected = true;
                                ++selectedCount_;
                            }
                        }
                    }
                }
                else if (w.type == DesktopWidgetType::FolderMapping)
                {
                    RECT content = GetFolderMappingContentRect(w);
                    for (size_t i = 0; i < w.folderEntries.size(); ++i)
                    {
                        RECT r = GetFolderMappingItemRect(w, i);
                        if (RectsIntersect(r, content) && RectsIntersect(r, marqueeRect_))
                        {
                            w.folderEntries[i].selected = true;
                        }
                    }
                }
            }

            marqueeActive_ = false;
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else
        {
            DesktopHit clickHit = HitTestDesktop(point);
            if (clickHit.kind == DesktopHitKind::WidgetAllButton ||
                (clickHit.kind == DesktopHitKind::Widget &&
                    clickHit.widgetIndex < widgets_.size() &&
                    widgets_[clickHit.widgetIndex].gridSpan.columns <= 1 &&
                    widgets_[clickHit.widgetIndex].gridSpan.rows <= 1))
            {
                OpenCollectionPopupAt(clickHit.widgetIndex, point, L"");
            }
            else if (clickHit.kind == DesktopHitKind::WidgetCategory && clickHit.widgetIndex < widgets_.size())
            {
                std::vector<std::wstring> categories = GetVisibleFileCategoryIds(widgets_[clickHit.widgetIndex]);
                if (clickHit.memberIndex < categories.size())
                {
                    widgets_[clickHit.widgetIndex].activeCategoryId = categories[clickHit.memberIndex];
                    widgets_[clickHit.widgetIndex].scrollOffset = 0;
                    SaveLayoutSlots();
                    InvalidateRect(hwnd_, nullptr, TRUE);
                }
            }
        }

        mouseDown_ = false;
        draggingItems_ = false;
        draggingWidget_ = false;
        resizingWidget_ = false;
        draggingCollectionMember_ = false;
        draggingOverNav_ = false;
        dragTargetCell_ = {};
        dragHint_.clear();
        HideDragHintWindow();
        mouseDownHit_ = -1;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        collectionDragSourceWidget_ = static_cast<size_t>(-1);
        collectionDragSourceMember_ = static_cast<size_t>(-1);
        widgetResizeEdges_ = kResizeNone;
    }

    void OnDoubleClick(LPARAM lParam)
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        DesktopHit hit = HitTestDesktop(point);
        if ((hit.kind == DesktopHitKind::Widget ||
            hit.kind == DesktopHitKind::WidgetFolderOpen ||
            hit.kind == DesktopHitKind::WidgetContent) &&
            OpenFolderMappingSource(hit.widgetIndex))
        {
            return;
        }

        if (hit.kind == DesktopHitKind::WidgetFolderToggle && hit.widgetIndex < widgets_.size())
        {
            widgets_[hit.widgetIndex].listMode = !widgets_[hit.widgetIndex].listMode;
            widgets_[hit.widgetIndex].scrollOffset = 0;
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (hit.kind == DesktopHitKind::Item ||
            hit.kind == DesktopHitKind::WidgetMember ||
            hit.kind == DesktopHitKind::PopupMember)
        {
            if (hit.itemIndex != static_cast<size_t>(-1))
            {
                OpenItem(hit.itemIndex);
            }
            else if (hit.kind == DesktopHitKind::WidgetMember &&
                hit.widgetIndex < widgets_.size() &&
                widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                hit.memberIndex < widgets_[hit.widgetIndex].folderEntries.size())
            {
                const FolderEntry& entry = widgets_[hit.widgetIndex].folderEntries[hit.memberIndex];
                ShellExecuteW(hwnd_, L"open", entry.fullPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        else if (hit.kind == DesktopHitKind::WidgetCategory && hit.widgetIndex < widgets_.size())
        {
            std::vector<std::wstring> categories = GetVisibleFileCategoryIds(widgets_[hit.widgetIndex]);
            if (hit.memberIndex < categories.size())
            {
                widgets_[hit.widgetIndex].activeCategoryId = categories[hit.memberIndex];
                widgets_[hit.widgetIndex].scrollOffset = 0;
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
        else if (hit.kind == DesktopHitKind::Widget || hit.kind == DesktopHitKind::WidgetAllButton)
        {
            if (hit.widgetIndex < widgets_.size() && widgets_[hit.widgetIndex].type == DesktopWidgetType::Collection)
            {
                OpenCollectionPopupAt(hit.widgetIndex, point, L"");
            }
        }
    }

    void OnRightButtonUp(LPARAM lParam)
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        DesktopHit hit = HitTestDesktop(point);
        if ((hit.kind == DesktopHitKind::Item ||
            hit.kind == DesktopHitKind::WidgetMember ||
            hit.kind == DesktopHitKind::PopupMember) &&
            hit.itemIndex < items_.size() &&
            !items_[hit.itemIndex].selected)
        {
            SelectOnly(hit.itemIndex);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (hit.kind == DesktopHitKind::WidgetMember &&
            hit.widgetIndex < widgets_.size() &&
            widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
            hit.memberIndex < widgets_[hit.widgetIndex].folderEntries.size() &&
            !widgets_[hit.widgetIndex].folderEntries[hit.memberIndex].selected)
        {
            SelectFolderEntryOnly(hit.widgetIndex, hit.memberIndex);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if ((hit.kind == DesktopHitKind::Widget ||
            hit.kind == DesktopHitKind::WidgetAllButton ||
            hit.kind == DesktopHitKind::WidgetCategory ||
            hit.kind == DesktopHitKind::WidgetFolderToggle ||
            hit.kind == DesktopHitKind::WidgetFolderOpen) &&
            hit.widgetIndex < widgets_.size())
        {
            SelectWidgetOnly(hit.widgetIndex);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }

        POINT screenPoint = point;
        ClientToScreen(hwnd_, &screenPoint);
        if (hit.kind == DesktopHitKind::Item ||
            hit.kind == DesktopHitKind::WidgetMember ||
            hit.kind == DesktopHitKind::PopupMember)
        {
            if (hit.itemIndex != static_cast<size_t>(-1) && IsProtectedDesktopIcon(items_[hit.itemIndex]))
            {
                ShowShellContextMenu(screenPoint);
            }
            else if (hit.kind == DesktopHitKind::WidgetMember &&
                hit.widgetIndex < widgets_.size() &&
                widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping &&
                hit.memberIndex < widgets_[hit.widgetIndex].folderEntries.size())
            {
                ShowFolderEntryContextMenu(screenPoint, hit.widgetIndex, hit.memberIndex);
            }
            else if (hit.kind == DesktopHitKind::WidgetMember &&
                hit.widgetIndex < widgets_.size() &&
                widgets_[hit.widgetIndex].type == DesktopWidgetType::FolderMapping)
            {
                ShowCustomWidgetContextMenu(screenPoint, hit.widgetIndex);
            }
            else
            {
                ShowCustomItemContextMenu(screenPoint);
            }
        }
        else if ((hit.kind == DesktopHitKind::Widget ||
            hit.kind == DesktopHitKind::WidgetContent ||
            hit.kind == DesktopHitKind::WidgetAllButton ||
            hit.kind == DesktopHitKind::WidgetCategory ||
            hit.kind == DesktopHitKind::WidgetFolderToggle ||
            hit.kind == DesktopHitKind::WidgetFolderOpen) &&
            hit.widgetIndex < widgets_.size())
        {
            ShowCustomWidgetContextMenu(screenPoint, hit.widgetIndex);
        }
        else
        {
            ShowCustomBackgroundContextMenu(screenPoint);
        }
    }

    void OnCommand(WORD command)
    {
        switch (command)
        {
        case kTrayReloadCommand:
            ReloadItems();
            break;
        case kTraySortByNameCommand:
            SortIconsByName();
            break;
        case kTraySortByTypeCommand:
            SortIconsByType();
            break;
        case kTraySwitchNativeCommand:
            SwitchToNativeDesktop();
            break;
        case kTraySwitchCustomCommand:
            SwitchToCustomDesktop();
            break;
        case kTrayExitCommand:
            RequestExit();
            break;
        case kTraySettingsCommand:
            if (settingsWindow_)
                settingsWindow_->Show();
            break;
        case kTrayDesktopIconThisPC:
        case kTrayDesktopIconUserFiles:
        case kTrayDesktopIconNetwork:
        case kTrayDesktopIconControlPanel:
        case kTrayDesktopIconRecycleBin:
            ToggleDesktopIconVisibility(command);
            break;
        default:
            break;
        }
    }

    void OnTrayCallback(LPARAM lParam)
    {
        UINT message = LOWORD(lParam);
        if (message == WM_CONTEXTMENU || message == WM_RBUTTONUP)
        {
            wchar_t callbackMessage[256]{};
            swprintf_s(callbackMessage, L"OnTrayCallback menu message=0x%X trayMenuShowing=%s", message, DebugBool(trayMenuShowing_));
            DebugLog(callbackMessage);
            if (trayMenuShowing_)
            {
                return;
            }
            trayMenuShowing_ = true;
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
            trayMenuShowing_ = false;
        }
        else if (message == WM_LBUTTONDBLCLK)
        {
            DebugLog(L"OnTrayCallback left double click reload");
            ReloadItems();
        }
    }

    void OnKeyDown(WPARAM key)
    {
        if (renameEdit_ != nullptr)
        {
            return;
        }

        switch (key)
        {
        case VK_ESCAPE:
            if (popupWidgetIndex_ < widgets_.size())
            {
                CloseCollectionPopup();
                break;
            }
            RequestExit();
            break;
        case VK_F5:
            ReloadItems();
            break;
        case VK_RETURN:
            OpenSelected();
            break;
        case VK_DELETE:
            if (selectedWidgetIndex_ < widgets_.size())
            {
                DeleteWidget(selectedWidgetIndex_);
            }
            else if (DeleteSelectedFolderEntries())
            {
            }
            else
            {
                InvokeSelectedShellVerb("delete");
            }
            break;
        case VK_F2:
            BeginRenameSelected();
            break;
        case VK_LEFT:
            MoveKeyboardSelection(-1);
            break;
        case VK_RIGHT:
            MoveKeyboardSelection(1);
            break;
        case VK_UP:
            MoveKeyboardSelection(-GetRowsPerColumn());
            break;
        case VK_DOWN:
            MoveKeyboardSelection(GetRowsPerColumn());
            break;
        case 'C':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                if (InvokeSelectedFolderEntryVerb("copy"))
                    break;
                InvokeSelectedShellVerb("copy");
            }
            break;
        case 'X':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                if (InvokeSelectedFolderEntryVerb("cut"))
                    break;
                InvokeSelectedShellVerb("cut");
            }
            break;
        case 'V':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                PasteFromClipboardAtCursor();
            }
            break;
        case 'A':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                ClearSelection();
                for (auto& item : items_)
                {
                    if (!IsTopLevelItem(item))
                    {
                        continue;
                    }
                    item.selected = true;
                    ++selectedCount_;
                }
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            break;
        default:
            break;
        }
    }

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        SnowDesktopApp* app = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<SnowDesktopApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd_ = hwnd;
        }
        else
        {
            app = reinterpret_cast<SnowDesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (app != nullptr)
        {
            return app->HandleMessage(message, wParam, lParam);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT HandleControlMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == taskbarRestartMsg_ && message != 0)
        {
            DebugLog(L"Control TaskbarCreated - recovering");
            DebugLogWindow(L"Control TaskbarCreated desktop hwnd before", hwnd_);
            RecoverDesktopHostAfterExplorerRestart();
            DebugLogWindow(L"Control TaskbarCreated desktop hwnd after", hwnd_);
            return 0;
        }

        switch (message)
        {
        case kTrayCallbackMessage:
            OnTrayCallback(lParam);
            return 0;
        case WM_TIMER:
            if (wParam == kDesktopHostWatchTimerId)
            {
                WatchDesktopHost();
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_CLOSE:
            DebugLog(L"Control WM_CLOSE RequestExit");
            RequestExit();
            return 0;
        case WM_DESTROY:
            DebugLogWindow(L"Control WM_DESTROY", hwnd);
            if (controlHwnd_ == hwnd)
            {
                controlHwnd_ = nullptr;
            }
            if (exitRequested_)
            {
                PostQuitMessage(0);
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    static LRESULT CALLBACK ControlWindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        SnowDesktopApp* app = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<SnowDesktopApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->controlHwnd_ = hwnd;
        }
        else
        {
            app = reinterpret_cast<SnowDesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (app != nullptr)
        {
            return app->HandleControlMessage(hwnd, message, wParam, lParam);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK RenameEditSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR refData)
    {
        UNREFERENCED_PARAMETER(subclassId);
        auto* app = reinterpret_cast<SnowDesktopApp*>(refData);
        if (app == nullptr)
        {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        switch (message)
        {
        case WM_KEYDOWN:
            if (wParam == VK_RETURN)
            {
                app->CommitRename(false);
                return 0;
            }
            if (wParam == VK_ESCAPE)
            {
                app->CommitRename(true);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            app->CommitRename(false);
            return 0;
        default:
            break;
        }

        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND controlHwnd_ = nullptr;
    HWND hintHwnd_ = nullptr;
    HWND renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    HBRUSH renameBackgroundBrush_ = nullptr;
    size_t renameIndex_ = 0;
    bool trayIconAdded_ = false;
    HWND trayIconOwnerHwnd_ = nullptr;
    HICON trayIcon_ = nullptr;
    bool customDesktopVisible_ = true;
    DesktopWindows desktopWindows_{};
    ComPtr<IImageList> sysImageList_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> bitmapContext_;
    ComPtr<IDCompositionDesktopDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual2> dcompVisual_;
    ComPtr<IDCompositionSurface> dcompSurface_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> itemTextFormat_;
    ComPtr<IDWriteTextFormat> collectionItemTextFormat_;
    ComPtr<IDWriteTextFormat> listItemTextFormat_;
    ComPtr<IDWriteTextFormat> statusTextFormat_;
    ComPtr<ID2D1StrokeStyle> dottedStrokeStyle_;
    HANDLE faFontHandle_ = nullptr;
    HFONT faMenuFont_ = nullptr;
    ComPtr<IDWriteTextFormat> faTextFormat_;
    std::unordered_map<std::uintptr_t, ComPtr<ID2D1Bitmap1>> d2dIconCache_;
    ComPtr<IShellFolder> desktopFolder_;
    Pidl desktopPidl_;
    Pidl recycleBinPidl_;
    std::vector<DesktopItem> items_;
    std::vector<GridPage> gridPages_;
    std::vector<std::wstring> savedPageIds_;
    std::unordered_map<std::wstring, int> savedPageColumns_;
    std::unordered_map<std::wstring, int> savedPageRows_;
    std::unordered_map<std::wstring, LayoutRecord> layoutRecords_;
    std::unordered_map<std::wstring, bool> settingsIconVisibility_;
    std::vector<DesktopWidget> widgets_;
    int selectedCount_ = 0;
    LONG refCount_ = 1;
    int virtualLeft_ = 0;
    int virtualTop_ = 0;
    int virtualWidth_ = 0;
    int virtualHeight_ = 0;
    RECT layoutWorkArea_{};
    bool mouseDown_ = false;
    bool marqueeActive_ = false;
    bool draggingItems_ = false;
    bool draggingWidget_ = false;
    bool resizingWidget_ = false;
    bool draggingOverNav_ = false;
    bool externalDragActive_ = false;
    bool selfDragActive_ = false;
    bool selfDragReturned_ = false;
    std::vector<std::wstring> selfDragOutKeys_;
    bool dropTargetRegistered_ = false;
    int mouseDownHit_ = -1;
    size_t mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    size_t selectedWidgetIndex_ = static_cast<size_t>(-1);
    size_t popupWidgetIndex_ = static_cast<size_t>(-1);
    size_t collectionDragSourceWidget_ = static_cast<size_t>(-1);
    size_t collectionDragSourceMember_ = static_cast<size_t>(-1);
    bool draggingCollectionMember_ = false;
    bool draggingWidgetMember_ = false;
    size_t widgetMemberDragWidget_ = static_cast<size_t>(-1);
    size_t widgetMemberInsertPreview_ = static_cast<size_t>(-1);
    bool renamingWidget_ = false;
    bool renamingFolderEntry_ = false;
    size_t renameFolderWidgetIndex_ = static_cast<size_t>(-1);
    size_t renameFolderEntryIndex_ = static_cast<size_t>(-1);
    int widgetResizeEdges_ = kResizeNone;
    GridCell dragTargetCell_;
    GridCell widgetDragOriginalCell_;
    GridCell widgetPreviewCell_;
    GridSpan widgetDragOriginalSpan_;
    GridSpan widgetPreviewSpan_;
    bool widgetPreviewOccupied_ = false;
    int dragGroupOriginX_ = 0;
    int dragGroupOriginY_ = 0;
    POINT mouseDownPoint_{};
    POINT dragCurrentPoint_{};
    POINT externalDragPoint_{};
    POINT lastMousePoint_{};
    RECT collectionDragStartBounds_{};
    RECT popupRect_{};
    int popupScrollOffset_ = 0;
    bool popupHasAnchor_ = false;
    std::wstring popupPageId_;
    POINT popupAnchorPoint_{};
    std::wstring popupCategoryId_;
    std::wstring dragHint_;
    std::wstring externalDragHint_;
    std::wstring primaryMonitorId_;
    std::wstring firstPageMonitorId_;
    std::wstring lastMonitorPageId_;
    int pageOffset_ = 0;
    float gapScale_ = 1.0f;
    ULONG shellChangeRegId_ = 0;
    UINT taskbarRestartMsg_ = 0;
    bool exitRequested_ = false;
    bool restartLaunched_ = false;
    bool reloading_ = false;
    bool trayMenuShowing_ = false;
    LONGLONG lastRecycleBinItemCount_ = -1;
    std::vector<HBITMAP> menuIconPool_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<WidgetEngine> widgetEngine_;
    bool navButtonsVisible_ = false;
    RECT navButtonsHoverZone_{};
    POINT lastContextMenuScreenPoint_{};
    RECT marqueeRect_{};
    ComPtr<IContextMenu2> activeContextMenu2_;
    ComPtr<IContextMenu3> activeContextMenu3_;
    ComPtr<IContextMenu2> newMenuContextMenu_;
    DWORD lastCreateWindowError_ = 0;
    HRESULT lastGraphicsError_ = S_OK;
    UINT compositionWidth_ = 0;
    UINT compositionHeight_ = 0;
};
