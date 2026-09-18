#pragma once

#include "../settings_ipc_codec.h"
#include "home_about_page_model.h"

// Shared by the settings process and codec regression tests. This status model
// contains no XAML controls and does not require a running WinUI application.
namespace snowdesktop::settings_ipc
{
template<> struct Fields<winui::HomeAboutStatusPatch>
{
    template<class Value> static auto Tie(Value& v)
    {
        return std::tie(v.generation, v.revision, v.applicationVersion, v.installedWidgetCount,
            v.packaged, v.backupState, v.backupCount, v.backupDetail, v.animationDiagnosticsEnabled,
            v.animationDiagnosticsStatus, v.temporaryInitializationEnabled,
            v.usageGuideExpanded, v.debugProfileEnabled, v.debugDataDirectory, v.debugDesktopDirectory);
    }
};
}
