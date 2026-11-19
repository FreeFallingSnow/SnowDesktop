#pragma once

#include "home_about_status.h"
#include "settings_ipc_codec.h"

// Shared by the settings process and codec regression tests. This status model
// contains no XAML controls and does not require a running WinUI application.
namespace snowdesktop::settings_ipc
{
template<> struct Fields<settings::DebugEnvironmentSummary>
{
    template<class Value> static auto Tie(Value& v) { return std::tie(v.id, v.name); }
};
template<> struct Fields<settings::HomeAboutStatusPatch>
{
    template<class Value> static auto Tie(Value& v)
    {
        return std::tie(v.generation, v.revision, v.applicationVersion, v.installedWidgetCount,
            v.packaged, v.backupState, v.backupCount, v.backupDetail, v.animationDiagnosticsEnabled,
            v.animationDiagnosticsStatus, v.temporaryInitializationEnabled,
            v.usageGuideExpanded, v.debugProfileEnabled, v.debugDataDirectory, v.debugDesktopDirectory,
            v.debugEnvironments, v.debugEnvironmentId, v.debugEnvironmentName);
    }
};
}
