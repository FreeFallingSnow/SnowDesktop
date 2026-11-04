#pragma once
#include "status_bar_settings_types.h"

struct JsonValue;

namespace snowdesktop
{
template<class Visitor> void VisitStatusBarAppearanceRules(Visitor visit)
{
    visit("noWindow", &StatusBarSettings::noWindow);
    visit("maximizedWindow", &StatusBarSettings::maximizedWindow);
}

template<class Visitor> void VisitStatusBarStoredAppearanceRules(Visitor visit)
{
    VisitStatusBarAppearanceRules(visit);
    visit("shellUi", &StatusBarSettings::legacyShellUi);
    visit("visibleWindow", &StatusBarSettings::legacyVisibleWindow);
}

template<class Visitor> void VisitStatusBarFlags(Visitor visit)
{
    visit("enabled", &StatusBarSettings::enabled);
    visit("showOnlyWhenSummoned", &StatusBarSettings::showOnlyWhenSummoned);
    visit("menu", &StatusBarSettings::menu);
    visit("quickSearch", &StatusBarSettings::quickSearch);
    visit("taskView", &StatusBarSettings::taskView);
    visit("showDesktop", &StatusBarSettings::showDesktop);
    visit("inputMethod", &StatusBarSettings::inputMethod);
    visit("inputMethodSystemPanel", &StatusBarSettings::inputMethodSystemPanel);
    visit("clockSystemPanel", &StatusBarSettings::clockSystemPanel);
    visit("clockShowSeconds", &StatusBarSettings::clockShowSeconds);
    visit("timeNotificationRightAligned", &StatusBarSettings::timeNotificationRightAligned);
    visit("controlCenterSystemPanel", &StatusBarSettings::controlCenterSystemPanel);
    visit("clock", &StatusBarSettings::clock);
    visit("tray", &StatusBarSettings::tray);
    visit("network", &StatusBarSettings::network);
    visit("volume", &StatusBarSettings::volume);
    visit("battery", &StatusBarSettings::battery);
    visit("controlCenter", &StatusBarSettings::controlCenter);
    visit("controlCenterSingleIcon", &StatusBarSettings::controlCenterSingleIcon);
    visit("notifications", &StatusBarSettings::notifications);
    visit("batteryPercentage", &StatusBarSettings::batteryPercentage);
    visit("cpu", &StatusBarSettings::cpu);
    visit("memory", &StatusBarSettings::memory);
    visit("gpu", &StatusBarSettings::gpu);
    visit("traffic", &StatusBarSettings::traffic);
    visit("audioControls", &StatusBarSettings::audioControls);
    visit("brightnessControls", &StatusBarSettings::brightnessControls);
    visit("wifiControls", &StatusBarSettings::wifiControls);
    visit("bluetoothControls", &StatusBarSettings::bluetoothControls);
    visit("mediaControls", &StatusBarSettings::mediaControls);
    visit("powerControls", &StatusBarSettings::powerControls);
    visit("vpnControls", &StatusBarSettings::vpnControls);
}

void NormalizeStatusBarSettings(StatusBarSettings& value);

bool DecodeStatusBarSettings(const JsonValue& input, StatusBarSettings& output);

std::string EncodeStatusBarSettings(StatusBarSettings value);
}
