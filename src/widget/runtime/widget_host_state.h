#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace snowdesktop::widget_runtime
{
enum class WidgetHostStateKind
{
    Ready,
    PackageMissing,
    PermissionPending,
    PermissionDenied,
    QuotaExceeded,
    RuntimeSuspended,
    LoadFailed,
};

enum class WidgetHostAction
{
    None,
    OpenPackageSource,
    RequestPermission,
    Reload,
};

enum class WidgetConsentSessionAction
{
    Start,
    WaitForWindow,
    ActivateWindow,
    ReplaceStale,
};

struct WidgetConsentDialogPosition
{
    int x = 0;
    int y = 0;
};

constexpr WidgetConsentDialogPosition CenterConsentDialogInWorkArea(
    int workLeft, int workTop, int workRight, int workBottom,
    int dialogWidth, int dialogHeight) noexcept
{
    const int workWidth = workRight > workLeft
        ? workRight - workLeft : 0;
    const int workHeight = workBottom > workTop
        ? workBottom - workTop : 0;
    const int width = dialogWidth > 0 ? dialogWidth : 0;
    const int height = dialogHeight > 0 ? dialogHeight : 0;
    const int centeredX = workLeft + (workWidth - width) / 2;
    const int centeredY = workTop + (workHeight - height) / 2;
    const int maxX = workRight - width;
    const int maxY = workBottom - height;
    return {
        width >= workWidth ? workLeft
            : (centeredX < workLeft ? workLeft
                : (centeredX > maxX ? maxX : centeredX)),
        height >= workHeight ? workTop
            : (centeredY < workTop ? workTop
                : (centeredY > maxY ? maxY : centeredY)),
    };
}

constexpr WidgetConsentSessionAction ConsentSessionActionFor(
    bool hasPendingSession, bool windowPublished, bool windowAlive,
    std::uint64_t elapsedMilliseconds,
    std::uint64_t openingTimeoutMilliseconds) noexcept
{
    if (!hasPendingSession) return WidgetConsentSessionAction::Start;
    if (windowAlive) return WidgetConsentSessionAction::ActivateWindow;
    if (!windowPublished &&
        elapsedMilliseconds < openingTimeoutMilliseconds)
        return WidgetConsentSessionAction::WaitForWindow;
    return WidgetConsentSessionAction::ReplaceStale;
}

struct WidgetHostState
{
    WidgetHostStateKind kind = WidgetHostStateKind::Ready;
    std::string detail;
};

constexpr bool ShowsHostPlaceholder(WidgetHostStateKind kind) noexcept
{
    return kind != WidgetHostStateKind::Ready;
}

constexpr WidgetHostAction HostActionFor(
    WidgetHostStateKind kind) noexcept
{
    switch (kind)
    {
    case WidgetHostStateKind::PackageMissing:
        return WidgetHostAction::OpenPackageSource;
    case WidgetHostStateKind::PermissionPending:
    case WidgetHostStateKind::PermissionDenied:
        return WidgetHostAction::RequestPermission;
    case WidgetHostStateKind::QuotaExceeded:
    case WidgetHostStateKind::RuntimeSuspended:
    case WidgetHostStateKind::LoadFailed:
        return WidgetHostAction::Reload;
    case WidgetHostStateKind::Ready:
        return WidgetHostAction::None;
    }
    return WidgetHostAction::None;
}

inline bool ContainsQuotaFailure(std::string_view message) noexcept
{
    return message.find("quota exceeded") != std::string_view::npos ||
        message.find("limit exceeded") != std::string_view::npos;
}

inline WidgetHostStateKind ClassifyWidgetRuntimeFailure(
    bool quotaExceeded, bool circuitOpen,
    std::string_view message) noexcept
{
    if (quotaExceeded || ContainsQuotaFailure(message))
        return WidgetHostStateKind::QuotaExceeded;
    if (circuitOpen)
        return WidgetHostStateKind::RuntimeSuspended;
    return WidgetHostStateKind::LoadFailed;
}
}
