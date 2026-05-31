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

class DesktopApp : public IDropTarget, public IDropSource
{
public:
    DesktopApp() = default;
    ~DesktopApp();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override;

    // IDropSource
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override;
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

    int Run(HINSTANCE instance, int showCommand);

    // Friends for OO rendering dispatch
    friend class DesktopIcon;
    friend class FolderEntryIcon;
    friend class DesktopGrid;
    friend class Widget;
    friend class WidgetContainer;
    friend class Collection;
    friend class FileCategories;
    friend class FolderMapping;
    friend class ScrollingItemWidget;
    friend class LuaScript;

    // OO system accessors
    std::vector<std::unique_ptr<Container>>& GetContainers() { return containers_; }
    const std::vector<std::unique_ptr<Item>>& GetItemsOO() const { return items_oo_; }
    std::vector<std::unique_ptr<Item>>& GetItemsOO() { return items_oo_; }
    const std::vector<DesktopItem>& GetDesktopItems() const { return items_; }
    std::vector<DesktopItem>& GetDesktopItems() { return items_; }
    DesktopGrid* GetDesktopGrid() { return static_cast<DesktopGrid*>(containers_.empty() ? nullptr : containers_[0].get()); }
    std::vector<DesktopWidget>& GetWidgets() { return widgets_; }
    const std::vector<DesktopWidget>& GetWidgets() const { return widgets_; }
    void InvalidateDesktop() { ::InvalidateRect(hwnd_, nullptr, TRUE); }

private:
    // ── Window ──────────────────────────────────────────────
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // ── Graphics ────────────────────────────────────────────
    bool InitGraphics();
    HRESULT CreateOrResizeCompositionSurface();
    void OnPaint();
    void RenderFrame(ID2D1DeviceContext* ctx);
    void DrawPageNavButtons(ID2D1DeviceContext* ctx);
    void GetNavButtonRects(RECT& outPrev, RECT& outNext) const;

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
    DesktopIcon* HitTestIcon(POINT pt) const;
    void OnMouseMove(WPARAM wp, LPARAM lp);
    void OnLeftButtonDown(WPARAM wp, LPARAM lp);
    void OnLeftButtonUp(WPARAM wp, LPARAM lp);
    void OnRightButtonUp(LPARAM lp);
    void OnKeyDown(WPARAM key);
    void RefreshDragHintFromKeyboard();
    void InvokeSelectedShellVerb(const char* verb);
    void MoveKeyboardSelection(WPARAM arrowKey);
    void OnTimer(WPARAM timerId);
    void ClearSelection();
    bool IsItemInAnyWidget(const DesktopItem& item) const;
    void ClearSelectionOutsideWidget(size_t widgetIndex);
    void ClearSelectionOutsideDesktop();
    void SelectOnly(int index);
    void SelectWidgetOnly(size_t index);
    void ToggleSelection(int index);
    bool HandlePageNavClick(POINT point);
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
    void ShowWidgetContextMenu(POINT screenPoint, size_t widgetIndex);
    void ShowFolderEntryContextMenu(POINT screenPoint, size_t widgetIndex, size_t memberIndex);
    void ShowItemContextMenu(POINT screenPoint, int itemIndex);
    void ShowShellContextMenu(POINT screenPoint, int itemIndex = -1);
    void ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir);
    void ShowDesktopBackgroundContextMenu(POINT screenPoint);
    HBITMAP CreateMenuIconBitmap(const wchar_t* text);
    void SetMenuItemIcon(HMENU menu, UINT_PTR command, const wchar_t* text);
    void ClearMenuIcons();
    void RestoreDesktopWindowLayer();
    bool IsProtectedDesktopIcon(const DesktopItem& item) const;
    bool IsShellRenameCommand(IContextMenu* contextMenu, UINT commandOffset) const;

    // ── Grid helpers ────────────────────────────────────────
    const GridPage* GridPageFromPoint(POINT point) const;
    void AdjustGridRows(int delta);
    void AdjustGridColumns(int delta);
    void SetFirstPageMonitorFromPoint(POINT screenPoint);
    void SetZoom(float value);
    void AdjustZoom(float delta);
    void ApplyPageMapping();
    int MaxPageOffset() const;
    bool PageHasContent(const std::wstring& pageId) const;
    int NextNonEmptyOffset(int fromOffset, int direction) const;
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
    void MigrateSelectedItemsToLastMonitorPage();
    POINT GetDragTargetPoint(POINT current) const;
    ComPtr<IDataObject> CreateSelectedDataObject() const;
    void DropSelectedItemsOnTarget(int targetIndex);
    size_t FindItemIndexByKey(const std::wstring& key) const;
    struct DesktopDropPayload
    {
        std::vector<std::wstring> filePaths;
        std::vector<std::wstring> desktopKeys;
        bool hasDesktopIcons = false;
        bool hasPathItems = false;
        bool hasWidgets = false;
    };
    DesktopDropPayload CollectDesktopDropPayload(const std::vector<Item*>& sourceItems) const;
    void RemoveDesktopKeysFromWidgets(const std::vector<std::wstring>& keys);
    std::unordered_set<std::wstring> SnapshotDesktopKeys() const;
    std::vector<std::wstring> NewDesktopKeysSince(const std::unordered_set<std::wstring>& existingKeys) const;
    bool DropFilePathsToDesktopCell(const std::vector<std::wstring>& paths, GridCell targetCell,
        int mods, bool duplicateDesktopCopyNames);
    void CopySelectedItemsToCell(GridCell targetCell);
    void CreateShortcutSelectedItemsToCell(GridCell targetCell);
    void PlaceNewItemsAtDropPoint(const std::unordered_set<std::wstring>& existingKeys, GridCell targetCell);

    // ── Rendering helpers ───────────────────────────────────
    RECT GetItemIconRect(RECT bounds) const;
    RECT GetItemTextRect(RECT bounds, bool expanded) const;
    RECT GetItemSelectionRect(RECT bounds, bool expanded) const;
    void DrawD2DRoundedRectangle(ID2D1DeviceContext* ctx, RECT rect, float radius,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke, float strokeWidth = 1.0f);
    void DrawD2DFilledRectangle(ID2D1DeviceContext* ctx, RECT rect,
        D2D1_COLOR_F fill, D2D1_COLOR_F stroke);
    void DrawItemText(ID2D1DeviceContext* ctx, RECT bounds,
        const std::wstring& text, bool selected, float opacity = 1.0f);
    void DrawD2DText(ID2D1DeviceContext* ctx, const std::wstring& text,
        RECT rect, IDWriteTextFormat* format, const D2D1_COLOR_F& color);
    void DrawCollectionPopup(ID2D1DeviceContext* ctx);
    static D2D1_RECT_F ToD2DRect(const RECT& r);

    // D2D bitmap cache — public for Item::Draw
    ID2D1Bitmap1* GetOrCreateD2DBitmap(HBITMAP hbm);

    // ── Filtering ───────────────────────────────────────────
    std::wstring GetStableLayoutKey(PCIDLIST_ABSOLUTE pidl,
        const std::wstring& parsingName, const std::wstring& desktopIconClsid = {});
    static void ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize);
    void RegisterShellChangeNotifications();

    // ── JSON helpers ────────────────────────────────────────
    bool ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const;
    bool ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const;
    bool ReadJsonBoolField(const std::string& objectText, const char* fieldName, bool& value) const;
    bool ReadJsonStringArrayField(const std::string& objectText, const char* fieldName, std::vector<std::wstring>& values) const;
    size_t FindJsonObjectEnd(const std::string& text, size_t start) const;
    size_t FindJsonArrayEnd(const std::string& text, size_t start) const;
    size_t FindJsonContainerEnd(const std::string& text, size_t start, char open, char close) const;
    DesktopWidgetType WidgetTypeFromJson(const std::wstring& type) const;
    std::wstring WidgetTypeToJson(DesktopWidgetType type) const;

    // ── Widget helpers ──────────────────────────────────────
    std::wstring MakeNewWidgetId() const;
    void AddWidgetToGrid(DesktopWidget&& widget, GridSpan span);
    void AddCollectionWidgetAt(POINT screenPoint);
    void AddFileCategoryWidgetAt(POINT screenPoint);
    void AddFolderMappingWidgetAt(POINT screenPoint);
    void PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan);
    void EnumerateFolderMappingEntries(DesktopWidget& widget);
    void RefreshFolderMappingWidget(size_t widgetIndex);
    bool CollectFileCategoryWidget(size_t widgetIndex, bool persist);
    void ApplyAutoCollectFileCategoryWidgets();
    void EnforceSingleAutoCollectFileCategory(size_t activeWidgetIndex);
    void OpenCollectionPopupAt(size_t widgetIndex, POINT anchorPoint, const std::wstring& categoryId = L"");
    void CloseCollectionPopup();
    bool IsPointInsideOpenPopup(POINT point) const;
    std::vector<std::wstring> GetPopupItemKeys(const DesktopWidget& widget) const;
    RECT GetCollectionPopupRect(const DesktopWidget& widget) const;
    RECT GetCollectionPopupContentRect(const RECT& popup) const;
    int GetCollectionPopupColumnCount(const RECT& popup) const;
    int GetCollectionPopupRowCount(const DesktopWidget& widget, const RECT& popup) const;
    int GetCollectionPopupMaxScrollOffset(const DesktopWidget& widget, const RECT& popup) const;
    RECT GetCollectionPopupItemRect(const RECT& popup, size_t linearIndex) const;
    void OnMouseWheel(WPARAM wp, LPARAM lp);
    RECT GetVisibleCollectionItemBounds(size_t itemIndex) const;
    bool FindSingleSelectedFolderEntry(size_t& widgetIndex, size_t& memberIndex) const;
    RECT GetFolderEntryRenameRect(size_t widgetIndex, size_t memberIndex) const;
    void BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex);
    void CommitFolderEntryRename(const std::wstring& newName, bool cancel);

    // ── Member variables ────────────────────────────────────
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    int virtualLeft_ = 0, virtualTop_ = 0, virtualWidth_ = 0, virtualHeight_ = 0;

    // D3D / D2D / DComp
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ID2D1Factory1* GetD2DFactory() const { return d2dFactory_.Get(); }
    IDWriteFactory* GetDWriteFactory() const { return dwriteFactory_.Get(); }
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
    ComPtr<IDWriteTextFormat> faTextFormat_;
    HANDLE faFontHandle_ = nullptr;
    HFONT faMenuFont_ = nullptr;
    std::vector<HBITMAP> menuIconPool_;

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
    int navHoverSide_ = 0; // -1 left, +1 right, 0 none
    DWORD navAutoFlipTick_ = 0;
    int navAutoFlipDir_ = 0;
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
    Item* mouseDownHit_ = nullptr;
    bool marqueeActive_ = false;
    RECT marqueeRect_{};
    size_t marqueeWidgetIndex_ = static_cast<size_t>(-1);

    // Drag state
    bool draggingItems_ = false;
    POINT dragCurrentPoint_{};
    int dragGroupOriginX_ = 0;
    int dragGroupOriginY_ = 0;
    bool dragCopyMode_ = false;
    bool dragLinkMode_ = false;

    // OO drag state (Slot-based)
    Container* dragSource_ = nullptr;
    std::vector<Item*> dragSourceItems_;
    Container* dragTargetContainer_ = nullptr;
    Slot* dragTargetSlot_ = nullptr;
    HitRegion dragTargetRegion_ = HitRegion::None;

    // Widget drag / resize state
    size_t mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    bool draggingWidget_ = false;
    bool resizingWidget_ = false;
    enum class WidgetAction { None, Move, Resize };
    WidgetAction widgetAction_ = WidgetAction::None;
    GridCell widgetDragOriginalCell_{};
    GridSpan widgetDragOriginalSpan_{};
    GridCell widgetPreviewCell_{};
    GridSpan widgetPreviewSpan_{};
    bool widgetPreviewOccupied_ = false;

    // OLE drag state
    LONG refCount_ = 1;
    bool selfDragActive_ = false;
    bool selfDragReturned_ = false;
    std::vector<std::wstring> selfDragOutKeys_;
    bool externalDragActive_ = false;
    POINT externalDragPoint_{};
    int externalDropFileCount_ = 0;
    bool dropTargetRegistered_ = false;

    // Pending placement (survives across ReloadItems for async file ops)
    std::vector<std::wstring> pendingPlaceNames_;
    GridCell pendingPlaceCell_;
    bool hasPendingPlace_ = false;
    DWORD pendingPlaceTick_ = 0;

    POINT ScreenPointToClient(POINTL screen) const;
    bool IsExternalDropWindowAt(POINT clientPoint) const;
    bool IsKnownDesktopSurfaceWindow(HWND window) const;
    static bool IsSameWindowTree(HWND parent, HWND window);
    DWORD ChooseDropEffect(DWORD keyState, DWORD allowed) const;

    // Recycle bin polling
    int64_t lastRecycleBinItemCount_ = -1;

    // Clipboard cut tracking
    std::unordered_set<std::wstring> cutPaths_;

    // Rename
    HWND renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    size_t renameIndex_ = static_cast<size_t>(-1);
    bool renamingWidget_ = false;
    bool renamingFolderEntry_ = false;
    size_t renameFolderWidgetIndex_ = static_cast<size_t>(-1);
    size_t renameFolderEntryIndex_ = static_cast<size_t>(-1);
    void BeginRenameSelected();
    void CommitRename(bool cancel);
    static LRESULT CALLBACK RenameEditSubclassProc(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);

    // Drag hint
    std::wstring dragHint_;
    HWND hintHwnd_ = nullptr;
    bool EnsureDragHintWindow();
    void ShowDragHintWindow(POINT clientPoint, const std::wstring& text);
    void ShowDragHintWindowScreen(POINT screenPoint, const std::wstring& text);
    void HideDragHintWindow();
    void DestroyDragHintWindow();
    static std::vector<std::wstring> GetDropPaths(IDataObject* dataObject);
    static std::wstring FileNameFromPath(const std::wstring& path);
    static bool MatchPendingName(const std::wstring& itemName, const std::wstring& srcFileName);
    void ApplyPendingPlacement();

    // Collection popup
    size_t popupWidgetIndex_ = static_cast<size_t>(-1);
    RECT popupRect_{};
    int popupScrollOffset_ = 0;
    bool popupHasAnchor_ = false;
    POINT popupAnchorPoint_{};
    std::wstring popupPageId_;
    std::wstring popupCategoryId_;
    std::unique_ptr<DesktopIcon> popupMouseDownItem_;
    std::unique_ptr<Slot> popupDragTargetSlot_;

    // OO system
    std::vector<std::unique_ptr<Container>> containers_;
    std::vector<std::unique_ptr<Item>> items_oo_;

    // D2D bitmap cache
    std::unordered_map<std::uintptr_t, ComPtr<ID2D1Bitmap1>> d2dIconCache_;

    // New menu COM context
    ComPtr<IContextMenu2> newMenuContextMenu_;
    ComPtr<IContextMenu2> activeContextMenu2_;
    ComPtr<IContextMenu3> activeContextMenu3_;
};

// ── Inline implementations (split into sub-headers) ─────────
#include "app_run.h"
#include "app_gfx.h"
#include "app_interact.h"
#include "app_menu.h"
#include "app_grid.h"
