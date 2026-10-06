#pragma once
#include "widget_gpu_sampler.h"
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace snowdesktop::gpu_diagnostics
{
struct Options
{
    std::filesystem::path output;
    unsigned samples = 11, intervalMs = 1000;
};
bool ParseOptions(std::span<const std::wstring_view> arguments, Options& options, std::string& error);
std::string SerializeSample(unsigned index, const widget_runtime::WidgetGpuDataSnapshot& sample,
    const widget_runtime::WidgetGpuDiagnosticSample& diagnostic);
int Run(int argc, wchar_t** argv, std::string_view version);
}
