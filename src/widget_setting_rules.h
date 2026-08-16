#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
bool IsValidUrlSettingValue(std::string_view value) noexcept;
bool IsValidDateSettingValue(std::string_view value) noexcept;
bool IsValidTimeSettingValue(std::string_view value) noexcept;

bool ParseFiniteSettingNumber(std::string_view value,
    double& output) noexcept;
double SnapRangeSettingValue(double value, double minimum,
    double maximum, double step) noexcept;

bool IsValidMultiSelectSettingValue(
    const std::vector<std::string>& options,
    const std::vector<std::string>& values) noexcept;

bool IsValidFilesystemSettingAccess(std::string_view value) noexcept;
bool NormalizeFilesystemSettingExtensions(
    const std::vector<std::string>& input,
    std::vector<std::string>& output) noexcept;
bool IsValidSettingGroupId(std::string_view value) noexcept;
bool IsValidSettingCondition(std::string_view operation,
    std::size_t valueCount) noexcept;
bool EvaluateSettingCondition(std::string_view operation,
    const std::vector<std::string>& current,
    const std::vector<std::string>& expected) noexcept;
bool ValidateSettingTextValue(std::string_view value, bool required,
    int minimumLength, int maximumLength) noexcept;
}
