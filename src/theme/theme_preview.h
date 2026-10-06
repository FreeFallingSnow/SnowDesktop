#pragma once
#include "theme_library.h"
#include "theme_preview_parts.h"
#include "widget/preview/widget_preview_stage.h"
#include <atomic>

namespace snowdesktop::themes::preview
{
inline constexpr int kCoverSize = 1024;
inline constexpr std::size_t kCoverMaximumBytes = 1024 * 1024;
struct Image { std::string component; std::filesystem::path path; };
bool RenderGallery(const std::filesystem::path& host, const Package&, std::string_view root,
    unsigned scope, const std::filesystem::path& directory, std::vector<Image>& images,
    std::filesystem::path& cover, std::string& error, const std::atomic_bool* cancel = nullptr,
    const std::filesystem::path& background = {});
// No retained bitmap cache. Each request owns and releases its stage and child.
bool Render(const std::filesystem::path& host, const Package&, std::string_view root,
    unsigned scope, const std::filesystem::path& directory, std::filesystem::path& cover,
    std::string& error, const std::atomic_bool* cancel = nullptr);
bool SaveCover(const std::filesystem::path&, widget_preview::Wallpaper, std::string& error);
bool NormalizeCover(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error);
std::wstring QuoteArgument(std::wstring_view);
bool Run(const std::filesystem::path&, const std::vector<std::wstring>&, std::string& output,
    unsigned timeoutMs, std::string& error, const std::atomic_bool* cancel = nullptr);
}
