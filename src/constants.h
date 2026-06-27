#pragma once
/**
 * @file constants.h
 * @brief 全局常量定义
 * @details 包含窗口类名、透明色键值、图标尺寸、网格布局参数、系统托盘消息、
 *          桌面图标CLSID、右键菜单命令、外壳变更通知、定时器ID、快捷导航、
 *          集合弹出面板布局等所有全局常量
 */
#include <windows.h>

// ── 窗口类名 ──────────────────────────────────
constexpr wchar_t kWindowClassName[] = L"SnowDesktopNativeProofWindow";
constexpr wchar_t kControlWindowClassName[] = L"SnowDesktopControlWindow";
constexpr wchar_t kInputWindowClassName[] = L"SnowDesktopInputWindow";
constexpr wchar_t kHintWindowClassName[] = L"SnowDesktopDragHintWindow";
constexpr wchar_t kQuickNavigationWindowClassName[] = L"SnowDesktopQuickNavigationWindow";
constexpr wchar_t kHiddenBySnowDesktopProp[] = L"SnowDesktop.HiddenExplorerIconLayer";

// ── 透明色键值 ────────────────────────────────
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);

// ── 图标与网格布局 ────────────────────────────
constexpr int kIconSize = 64;
constexpr int kIconBitmapSize = 64;
constexpr int kCellWidth = 92;
constexpr int kMinCellHeight = 116;
constexpr int kGridMarginX = 6;
constexpr int kGridMarginY = 6;
constexpr int kMarginX = kGridMarginX;
constexpr int kMarginY = 6;
constexpr int kTextTop = 70;
constexpr float kItemFontSize = 14.0f;
constexpr float kItemLineHeight = kItemFontSize * 7.0f / 6.0f;
constexpr float kItemBaseline = kItemFontSize * 5.0f / 6.0f;
constexpr int kTextCollapsedHeight = 28;
constexpr int kTextExpandedHeight = 48;
constexpr int kTextHeight = kTextCollapsedHeight;
constexpr float kGapPercentX = 0.16f;
constexpr float kGapPercentY = 0.14f;

// ── 重命名编辑框控件ID ────────────────────────
constexpr int kRenameEditId = 1001;

// ── 系统托盘通知 ──────────────────────────────
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;

// ── 托盘右键菜单命令 ──────────────────────────
constexpr UINT kTrayReloadCommand = 40001;
constexpr UINT kTraySortByNameCommand = 40002;
constexpr UINT kTraySwitchNativeCommand = 40003;
constexpr UINT kTraySwitchCustomCommand = 40004;
constexpr UINT kTrayExitCommand = 40005;
constexpr UINT kTraySortByTypeCommand = 40006;
constexpr UINT kTraySettingsCommand = 40012;
constexpr UINT kTrayDesktopIconThisPC = 40007;
constexpr UINT kTrayDesktopIconUserFiles = 40008;
constexpr UINT kTrayDesktopIconNetwork = 40009;
constexpr UINT kTrayDesktopIconControlPanel = 40010;
constexpr UINT kTrayDesktopIconRecycleBin = 40011;

// ── 桌面特殊图标CLSID ─────────────────────────
constexpr wchar_t kDesktopIconClsidThisPC[] = L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
constexpr wchar_t kDesktopIconClsidUserFiles[] = L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}";
constexpr wchar_t kDesktopIconClsidNetwork[] = L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}";
constexpr wchar_t kDesktopIconClsidControlPanel[] = L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}";
constexpr wchar_t kDesktopIconClsidRecycleBin[] = L"{645FF040-5081-101B-9F08-00AA002F954E}";

// ── 右键菜单命令ID ────────────────────────────
constexpr UINT kContextOpenCommand = 41001;
constexpr UINT kContextRenameCommand = 41002;
constexpr UINT kContextCutCommand = 41003;
constexpr UINT kContextCopyCommand = 41004;
constexpr UINT kContextPasteCommand = 41005;
constexpr UINT kContextDeleteCommand = 41006;
constexpr UINT kContextRefreshCommand = 41007;
constexpr UINT kContextSortByNameCommand = 41008;
constexpr UINT kContextSortByTypeCommand = 41009;
constexpr UINT kContextMoreCommand = 41010;
constexpr UINT kContextGridAddRow = 41012;
constexpr UINT kContextGridRemoveRow = 41013;
constexpr UINT kContextGridAddColumn = 41014;
constexpr UINT kContextGridRemoveColumn = 41015;
constexpr UINT kContextSpacingIncrease = 41016;
constexpr UINT kContextSpacingDecrease = 41017;
constexpr UINT kContextAddCollectionWidget = 41018;
constexpr UINT kContextWidgetOpen = 41019;
constexpr UINT kContextWidgetRename = 41020;
constexpr UINT kContextWidgetDelete = 41021;
constexpr UINT kContextWidgetEdit = 41032;
constexpr UINT kContextAddFileCategoryWidget = 41022;
constexpr UINT kContextWidgetManualCollect = 41023;
constexpr UINT kContextWidgetToggleAutoCollect = 41024;
constexpr UINT kContextWidgetToggleListMode = 41025;
constexpr UINT kContextAddFolderMappingWidget = 41026;
constexpr UINT kContextAddLuaWidgetFirst = 41450;
constexpr UINT kContextLuaWidgetMenuFirst = 41600;
constexpr UINT kContextLuaWidgetMenuLast = 41799;
constexpr UINT kContextWidgetOpenFolder = 41027;
constexpr UINT kContextWidgetToggleFolderView = 41028;
constexpr UINT kContextWidgetSortByName = 41029;
constexpr UINT kContextWidgetSortByType = 41030;
constexpr UINT kContextWidgetSortByDate = 41031;
constexpr UINT kContextSortByNameDescCommand = 41033;
constexpr UINT kContextSortByTypeDescCommand = 41034;
constexpr UINT kContextWidgetSortByNameDesc = 41035;
constexpr UINT kContextWidgetSortByTypeDesc = 41036;
constexpr UINT kContextWidgetSortByDateDesc = 41037;
constexpr UINT kContextWidgetShowOnHover = 41038;
constexpr UINT kContextWidgetShowOnHoverOn = 41038;
constexpr UINT kContextWidgetPrivacyMode = 41043;
constexpr UINT kContextWidgetPrivacyModeOn = 41043;
constexpr UINT kContextWidgetPrivacyModeOff = 41044;
constexpr UINT kContextWidgetToggleDateGroup = 41042;
constexpr UINT kContextWidgetShowOnHoverOff = 41041;
constexpr UINT kContextWidgetCollModeLargeFolder = 41039;
constexpr UINT kContextWidgetCollModeScrollContainer = 41040;
constexpr UINT kContextSpacingPresetFirst = 41150;
constexpr UINT kContextNewMenu = 41400;
constexpr UINT kContextSettingsCommand = 41401;
constexpr UINT kContextGridAdjustmentMenu = 41402;
constexpr UINT kContextGridAdjustmentDone = 41403;
constexpr UINT kContextFontSizeSmall = 41404;
constexpr UINT kContextFontSizeMedium = 41405;
constexpr UINT kContextFontSizeLarge = 41406;
constexpr UINT kContextFontWeightBold = 41427;
constexpr UINT kContextFontWeightMedium = 41428;
constexpr UINT kContextFontWeightFine = 41429;
constexpr UINT kContextPagePrev = 41407;
constexpr UINT kContextPageNext = 41408;
constexpr UINT kContextPageAdd = 41409;
constexpr UINT kContextPinFirstPage = 41410;
constexpr UINT kContextPinLastPage = 41411;
constexpr UINT kContextGridRecommended169First = 41414;
constexpr UINT kContextGridRecommended169Last = 41418;
constexpr UINT kContextGridRecommended1610First = 41419;
constexpr UINT kContextGridRecommended1610Last = 41423;
constexpr UINT kContextPageJumpFirst = 41500;
constexpr UINT kContextPageJumpLast  = 41550;

// ── 外壳变更通知 ──────────────────────────────
constexpr UINT kShellChangeMessage = WM_APP + 2;
constexpr UINT kIconLoadedMessage = WM_APP + 3;
constexpr UINT_PTR kShellChangeTimerId = 2;
constexpr UINT kShellChangeDebounceMs = 500;

// ── 定时器ID与间隔 ────────────────────────────
constexpr UINT_PTR kRecycleBinPollTimerId = 3;
constexpr UINT kRecycleBinPollIntervalMs = 2000;
constexpr UINT_PTR kDesktopHostWatchTimerId = 4;
constexpr UINT kDesktopHostWatchIntervalMs = 2000;
constexpr UINT_PTR kWidgetRefreshTimerId = 5;
constexpr UINT kWidgetRefreshIntervalMs = 1000;
constexpr UINT_PTR kCollectionPopupDwellTimerId = 6;
constexpr UINT kCollectionPopupDwellIntervalMs = 50;
constexpr DWORD kCollectionPopupDwellDelayMs = 600;
constexpr UINT_PTR kPageNotifyTimerId = 7;
constexpr UINT kPageNotifyTimerIntervalMs = 30;
constexpr DWORD kPageNotifyVisibleMs = 1800;
constexpr DWORD kPageNotifyFadeMs = 500;
constexpr UINT_PTR kDisplayTopologyRefreshTimerId = 8;
constexpr UINT kDisplayTopologyRefreshDebounceMs = 750;
constexpr UINT_PTR kHiddenHintTimerId = 9;
constexpr UINT kHiddenHintVisibleMs = 2000;
constexpr UINT_PTR kWidgetAddedHintTimerId = 10;
constexpr UINT kWidgetAddedHintVisibleMs = 2000;

// ── 组件独立刷新定时器 ────────────────────────
// 由 manifest 的 refreshIntervalMs 声明驱动的 per-widget Win32 定时器 ID 基址，
// 宿主从此值起递增分配，与上面固定定时器 ID 区分。
constexpr UINT_PTR kWidgetTimerIdBase = 1000;
constexpr UINT kWidgetRefreshMinIntervalMs = 16;      // 单组件声明刷新间隔下限
constexpr UINT kWidgetRefreshMaxIntervalMs = 86400000; // 上限（24h）

// ── 快捷导航 ──────────────────────────────────
constexpr int kQuickNavigationHotkeyId = 101;

// ── 集合弹出面板布局 ──────────────────────────
constexpr int kCollectionPopupPaddingX = 18;
constexpr int kCollectionPopupHeaderHeight = 54;
constexpr int kCollectionPopupBottomPadding = 18;

// ── 快捷导航单元格尺寸 ────────────────────────
constexpr int kQuickNavigationCellWidth = 72;
constexpr int kQuickNavigationCellHeight = 88;
constexpr int kQuickNavigationTextHeight = 36;
