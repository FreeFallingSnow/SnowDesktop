#pragma once
#include "item.h"
#include "slot.h"
#include "container.h"
#include "desktop.h"
#include "widget.h"
#include "utils.h"
#include "types.h"
#include "constants.h"
#include "resource.h"

#include <windowsx.h>
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
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

class SnowDesktopAppOO
{
public:
    SnowDesktopAppOO() = default;
    ~SnowDesktopAppOO();

    int Run(HINSTANCE instance, int showCommand);

private:
    // ── Window ──────────────────────────────────────────────
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // ── Graphics ────────────────────────────────────────────
    bool InitGraphics();
    HRESULT CreateOrResizeCompositionSurface();
    void OnPaint();
    void RenderFrame(ID2D1DeviceContext* ctx);

    // ── Data ────────────────────────────────────────────────
    void LoadDesktopItems();
    void ReloadItems(bool reloadLayoutFromDisk = true);
    void UpdateLayoutWorkArea();
    void ConfigureGridPage(GridPage& page) const;
    void ApplySavedGridDimensions();
    void ApplyGapScaleToPage(GridPage& page);
    void LayoutItems();
    void RebuildContainersAndItems();

    // ── Layout persistence ──────────────────────────────────
    std::wstring GetLayoutPath() const;
    void LoadLayoutSlots();
    void SaveLayoutSlots();
    void LoadSavedPagesFromJson(const std::string& text);
    void RememberSavedPageId(const std::wstring& pageId);

    // ── Interaction ─────────────────────────────────────────
    int HitTestItem(POINT pt) const;
    void OnMouseMove(WPARAM wp, LPARAM lp);
    void OnLeftButtonDown(WPARAM wp, LPARAM lp);
    void OnLeftButtonUp(WPARAM wp, LPARAM lp);
    void OnRightButtonUp(LPARAM lp);
    void OnKeyDown(WPARAM key);
    void OnTimer(WPARAM timerId);
    void ClearSelection();
    void SelectOnly(int index);
    void ToggleSelection(int index);
    void SortIconsByName();
    void SortIconsByType();
    void UpdateCutState();

    // ── Tray ────────────────────────────────────────────────
    void AddTrayIcon(bool force = false);
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);
    void OnTrayCallback(LPARAM lParam);

    // ── Context menus ───────────────────────────────────────
    void ShowBackgroundContextMenu(POINT screenPoint);
    void ShowItemContextMenu(POINT screenPoint, int itemIndex);
    void ShowShellContextMenu(POINT screenPoint, int itemIndex = -1);
    void ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir);
    void ShowDesktopBackgroundContextMenu(POINT screenPoint);
    void RestoreDesktopWindowLayer();
    bool IsProtectedDesktopIcon(const DesktopItem& item) const;

    // ── Grid helpers ────────────────────────────────────────
    const GridPage* GridPageFromPoint(POINT point) const;
    void AdjustGridRows(int delta);
    void AdjustGridColumns(int delta);
    void SetFirstPageMonitorFromPoint(POINT screenPoint);
    void SetZoom(float value);
    void AdjustZoom(float delta);
    void ApplyPageMapping();
    int MaxPageOffset() const;
    size_t FirstMonitorOrderIndex() const;
    std::vector<size_t> BuildMonitorRenderOrder() const;
    bool TryFindFreeCell(GridSpan span, std::unordered_set<std::wstring>& usedSlots, GridCell& result,
        const std::wstring& preferredPageId = L"", int preferredStartSlot = 0) const;
    static void MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span);
    static bool AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span);
    static bool IsGridAreaValid(const GridCell& cell, GridSpan span);
    void RelayoutDisplacedItems();

    // ── Drag helpers ───────────────────────────────────────
    GridCell CellFromPoint(POINT point) const;
    static int GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal);
    GridCell FindBestDropCell(GridCell targetCell) const;
    struct PendingGridMove { size_t index; GridCell cell; };
    std::vector<PendingGridMove> BuildSelectedMove(GridCell targetCell) const;
    bool IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const;
    void MoveSelectedItemsToCell(GridCell targetCell);
    void UpdateDragGroupOrigin();
    POINT GetDragTargetPoint(POINT current) const;
    ComPtr<IDataObject> CreateSelectedDataObject() const;
    void DropSelectedItemsOnTarget(int targetIndex);

    // ── Rendering helpers ───────────────────────────────────
    RECT GetItemIconRect(RECT bounds) const;
    RECT GetItemTextRect(RECT bounds, bool expanded) const;
    RECT GetItemSelectionRect(RECT bounds, bool expanded) const;
    void DrawD2DRoundedRectangle(ID2D1DeviceContext* ctx, RECT rect, float radius,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth = 1.0f);
    void DrawD2DFilledRectangle(ID2D1DeviceContext* ctx, RECT rect,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke);
    static D2D1_RECT_F ToD2DRect(const RECT& r);

    // ── Filtering ───────────────────────────────────────────
    std::wstring GetStableLayoutKey(PCIDLIST_ABSOLUTE pidl,
        const std::wstring& parsingName, const std::wstring& desktopIconClsid = {});
    static void ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize);
    void RegisterShellChangeNotifications();

    // ── JSON helpers ────────────────────────────────────────
    bool ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const;
    bool ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const;

    // ── Member variables ────────────────────────────────────
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    int virtualLeft_ = 0, virtualTop_ = 0, virtualWidth_ = 0, virtualHeight_ = 0;

    // D3D / D2D / DComp
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDCompositionDesktopDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual2> dcompVisual_;
    ComPtr<IDCompositionSurface> dcompSurface_;
    UINT compositionWidth_ = 0, compositionHeight_ = 0;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> itemTextFormat_;
    ComPtr<IDWriteTextFormat> listItemTextFormat_;

    // Shell
    ComPtr<IShellFolder> desktopFolder_;
    Pidl desktopPidl_;
    Pidl recycleBinPidl_;
    DesktopWindows desktopWindows_{};
    ULONG shellChangeRegId_ = 0;
    bool reloading_ = false;

    // Data
    std::vector<DesktopItem> items_;
    std::vector<GridPage> gridPages_;
    std::vector<DesktopWidget> widgets_;
    std::unordered_map<std::wstring, LayoutRecord> layoutRecords_;
    std::unordered_map<std::wstring, bool> settingsIconVisibility_;
    std::unordered_map<std::wstring, int> savedPageColumns_;
    std::unordered_map<std::wstring, int> savedPageRows_;
    std::vector<std::wstring> savedPageIds_;
    RECT layoutWorkArea_{};
    float gapScale_ = 1.0f;
    std::wstring primaryMonitorId_;
    std::wstring firstPageMonitorId_;
    std::wstring lastMonitorPageId_;
    int pageOffset_ = 0;
    POINT lastContextMenuScreenPoint_{};

    // Control window (for tray icon ownership + desktop host watch)
    HWND controlHwnd_ = nullptr;
    static LRESULT CALLBACK ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    UINT taskbarRestartMsg_ = 0;

    // Tray
    HICON trayIcon_ = nullptr;
    HWND trayIconOwnerHwnd_ = nullptr;
    bool trayIconAdded_ = false;

    // Mouse / interaction state
    POINT lastMousePoint_{};
    bool mouseDown_ = false;
    POINT mouseDownPoint_{};
    int mouseDownHit_ = -1;
    bool marqueeActive_ = false;
    RECT marqueeRect_{};

    // Drag state
    bool draggingItems_ = false;
    POINT dragCurrentPoint_{};
    int dragGroupOriginX_ = 0;
    int dragGroupOriginY_ = 0;

    // Recycle bin polling
    int64_t lastRecycleBinItemCount_ = -1;

    // Clipboard cut tracking
    std::unordered_set<std::wstring> cutPaths_;

    // Rename
    HWND renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    size_t renameIndex_ = static_cast<size_t>(-1);
    void BeginRenameSelected();
    void CommitRename(bool cancel);
    static LRESULT CALLBACK RenameEditSubclassProc(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);

    // Drag hint
    HWND hintHwnd_ = nullptr;
    bool EnsureDragHintWindow();
    void ShowDragHintWindow(POINT clientPoint, const std::wstring& text);
    void HideDragHintWindow();
    void DestroyDragHintWindow();
    std::wstring MakeDragHint(POINT point) const;

    // OO system
    std::vector<std::unique_ptr<Container>> containers_;
    std::vector<std::unique_ptr<Item>> items_oo_;

    // D2D bitmap cache
    std::unordered_map<std::uintptr_t, ComPtr<ID2D1Bitmap1>> d2dIconCache_;
    ID2D1Bitmap1* GetOrCreateD2DBitmap(HBITMAP hbm);

    // New menu COM context
    ComPtr<IContextMenu2> newMenuContextMenu_;
    ComPtr<IContextMenu2> activeContextMenu2_;
    ComPtr<IContextMenu3> activeContextMenu3_;
};

// ── Inline implementations (split into sub-headers) ─────────
#include "app_oo_run.h"
#include "app_oo_gfx.h"
#include "app_oo_interact.h"
#include "app_oo_menu.h"
#include "app_oo_grid.h"
