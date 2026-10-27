#pragma once
#include <filesystem>
#include <atomic>
#include <string>
namespace snowdesktop::performance
{
struct AnalysisResult { std::filesystem::path report; std::string error, quality = "unknown"; };
// Worker-only: fixed shipped script; no caller-supplied executable/SQL/path.
AnalysisResult AnalyzeCapture(const std::filesystem::path& manifest, const std::atomic<bool>* cancel = nullptr, bool offline = false);
}
