#include "app/app.h"

#include "common/l10n.h"
#include "common/utils.h"
#include "platform/operation_feedback.h"
#include "steam/steam_entitlement.h"
#include "widget/runtime/widget_engine.h"

#include <algorithm>

void DesktopApp::ConfigureSteamWidgetMultiplayer(const std::filesystem::path& bridge)
{
    widgetEngine_->ConfigureSteamMultiplayer(bridge,
        [this] {
            if (!steamEntitlementService_) return 0;
            const auto state = steamEntitlementService_->Current();
            if (!state.widgetMultiplayerAvailable) return 0;
            if (!state.registered) return 1;
            return generalSettings_.steamShowRunningStatus ? 3 : 2;
        },
        [this] {
            ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
                snowdesktop::SettingsPage::General, "general.advancedFeatures.unlockRequired"));
        },
        [this](const std::vector<std::string>& friends) -> std::optional<std::size_t> {
            if (friends.empty()) return std::nullopt;
            std::vector<std::wstring> labels;
            labels.reserve(friends.size());
            for (const auto& name : friends)
            {
                auto& label = labels.emplace_back(Utf8ToWide(name));
                std::replace_if(label.begin(), label.end(), [](wchar_t ch) { return ch < L' '; }, L' ');
            }
            std::vector<TASKDIALOG_BUTTON> buttons;
            for (std::size_t index = 0; index < labels.size(); ++index)
                buttons.push_back({100 + static_cast<int>(index), labels[index].c_str()});
            TASKDIALOGCONFIG dialog{};
            dialog.cbSize = sizeof(dialog);
            dialog.hwndParent = controlHwnd_;
            dialog.pszWindowTitle = L"Steam";
            dialog.pszMainInstruction = _LW("app.steam.widget_invite_instruction");
            dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_NO_DEFAULT_RADIO_BUTTON;
            dialog.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
            dialog.cRadioButtons = static_cast<UINT>(buttons.size());
            dialog.pRadioButtons = buttons.data();
            dialog.nDefaultButton = IDCANCEL;
            int button = 0, selected = 0;
            if (FAILED(snowdesktop::operation_feedback::ShowTaskDialog(
                    &dialog, &button, &selected, nullptr)) || button != IDOK || selected < 100)
                return std::nullopt;
            const auto index = static_cast<std::size_t>(selected - 100);
            return index < labels.size() ? std::optional<std::size_t>(index) : std::nullopt;
        });
}
