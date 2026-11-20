#pragma once

#include "control_settings.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{
struct IconBeautifySettings;
struct LargeIconConfig;

namespace control_settings
{
struct NativeWidgetSettingsState;

struct SettingsSearchTarget
{
    Target target;
    bool requiresId = false;
    std::string action;
    std::string name;
};

class SettingsDescriptionSearch
{
public:
    explicit SettingsDescriptionSearch(const Value& params);
    void AddHostFields();
    void AddField(const SettingsSearchTarget& target, std::string_view field, std::string_view description);
    Value Results(std::size_t unavailableWidgets);

private:
    struct Match
    {
        SettingsSearchTarget target;
        std::string field;
        std::string description;
        int rank;
    };

    void AddFields(const SettingsSearchTarget& target, const Value& fields, std::string_view parent = {});
    void AddSchema(const SettingsSearchTarget& target, const Value& schema, const std::string& field);

    std::string query_;
    std::string foldedQuery_;
    std::vector<std::string> words_;
    std::size_t offset_ = 0;
    std::size_t limit_ = 12;
    std::vector<Match> matches_;
};

// Each adapter owns the same field metadata used by the direct catalog.
Value DescribeGlobalSettingsFields();
Value DescribePageSettingsFields();
Value DescribeDebugEnvironmentFields(bool editable);
Value DescribeNativeWidgetFields(const NativeWidgetSettingsState& state);
Value DescribeIconAppearanceFields(const IconBeautifySettings& defaults);
Value DescribeLargeIconFields(const LargeIconConfig& defaults);
Value DescribeCalendarEventFields();
Value DescribeCalendarSeriesFields();
Value DescribeFontSettingsFields();
Value DescribeRuntimeSettingsFields(bool animationDiagnosticsAvailable, bool usageGuideAvailable);
Value DescribePackageSettingsFields(bool canEnable, bool canUseDevelopmentOverride);
Value DescribeAgentSkillSettingsFields();
Value DescribeWidgetAppearanceFields();
}
}
