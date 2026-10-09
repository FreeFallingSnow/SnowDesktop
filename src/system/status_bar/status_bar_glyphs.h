#pragma once

namespace snowdesktop::status_bar_glyphs
{
// Official Fluent System Icons Regular glyphs, pinned with the embedded font:
// microsoft/fluentui-system-icons@21d5d02f724be2aaf586564775fff73a18a76eb6.
inline constexpr wchar_t kMenu[] = L"\uF132"; // apps_16_regular
inline constexpr wchar_t kSearch[] = L"\uEA7C"; // search_16_regular
inline constexpr wchar_t kTaskView[] = L"\uF125"; // app_recent_24_regular
// Fluent's numeric suffix counts removed arcs: wifi_1 is the strongest.
inline constexpr wchar_t kWifi[] = L"\uF8AC"; // wifi_1_20_regular
inline constexpr wchar_t kWifiLow[] = L"\uF8B2"; // wifi_4_20_regular
inline constexpr wchar_t kWifiMedium[] = L"\uF8B0"; // wifi_3_20_regular
inline constexpr wchar_t kWifiGood[] = L"\uF8AE"; // wifi_2_20_regular
inline constexpr wchar_t kWifiOff[] = L"\uEE59"; // wifi_off_20_regular
inline constexpr wchar_t kEthernet[] = L"\uE999"; // plug_connected_20_regular
inline constexpr wchar_t kNetwork[] = L"\uF45A"; // globe_20_regular
inline constexpr wchar_t kOffline[] = L"\uE6B9"; // globe_prohibited_20_regular
inline constexpr wchar_t kUnknown[] = L"\uF63D"; // question_circle_20_regular
inline constexpr wchar_t kSpeaker[] = L"\uEB41"; // speaker_2_16_regular
inline constexpr wchar_t kSpeakerLow[] = L"\uEB3C"; // speaker_1_16_regular
inline constexpr wchar_t kSpeakerZero[] = L"\uEB37"; // speaker_0_16_regular
inline constexpr wchar_t kMuted[] = L"\uEB49"; // speaker_mute_16_regular
inline constexpr wchar_t kBattery[] = L"\uF1CD"; // battery_9_20_regular
inline constexpr wchar_t kCharging[] = L"\uF1CF"; // battery_charge_20_regular
inline constexpr wchar_t kTray[] = L"\uF2A2"; // chevron_down_16_regular
inline constexpr wchar_t kControls[] = L"\uF586"; // options_16_regular
inline constexpr wchar_t kBatteryFull[] = L"\uE143"; // battery_10_20_regular
inline constexpr wchar_t kBatteryPlug[] = L"\uE145"; // battery_checkmark_20_regular
inline constexpr wchar_t kNotifications[] = L"\uF114"; // alert_20_regular
inline constexpr wchar_t kNotificationsQuiet[] = L"\uF11C"; // alert_snooze_20_regular
inline constexpr wchar_t kNotificationsPending[] = L"\uE019"; // alert_badge_24_regular
inline constexpr wchar_t kNotificationsPresent[] = L"\uE019"; // alert_badge_24_regular
}
