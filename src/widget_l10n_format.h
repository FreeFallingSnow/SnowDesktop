#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_l10n
{
struct NumberOptions
{
    int minimumFractionDigits = 0;
    int maximumFractionDigits = 2;
    bool grouping = true;
};

std::string FormatNumber(double value, std::string_view locale,
    NumberOptions options = {});

std::string FormatBytes(double bytes, std::string_view locale,
    int base = 1024, int maximumFractionDigits = 1);

std::string FormatDuration(std::int64_t milliseconds,
    std::string_view locale, std::string_view style = "short");

std::string FormatList(const std::vector<std::string>& values,
    std::string_view locale);
}
