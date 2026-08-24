#pragma once

namespace snowdesktop::winui
{

struct SettingsTitleBarPolicyInput
{
    bool windows11OrGreater = false;
    bool highContrastQuerySucceeded = false;
    bool highContrastEnabled = false;
    bool customizationSupported = false;
};

/**
 * Full title-bar extension is deliberately conservative. Windows 11 is the
 * first platform on which AppWindowTitleBar customization is fully supported;
 * high contrast and failed capability probes retain the complete native
 * non-client caption instead.
 */
[[nodiscard]] constexpr bool ShouldUseIntegratedSettingsTitleBar(
    SettingsTitleBarPolicyInput input) noexcept
{
    return input.windows11OrGreater &&
        input.highContrastQuerySucceeded && !input.highContrastEnabled &&
        input.customizationSupported;
}

} // namespace snowdesktop::winui
