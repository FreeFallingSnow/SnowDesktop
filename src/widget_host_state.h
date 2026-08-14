#pragma once

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
