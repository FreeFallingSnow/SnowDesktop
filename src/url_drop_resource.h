#pragma once

#include <string>
#include <string_view>

namespace snowdesktop::url_drop_resource
{

enum class Action
{
    Download,
    Shortcut,
};

struct Decision
{
    Action action = Action::Shortcut;
    std::wstring suggestedFileName;
    std::wstring normalizedContentType;
};

/**
 * Decide whether a URL-only desktop drop represents a downloadable resource.
 *
 * The response MIME type wins over the URL suffix so an HTML error page is
 * never saved as a misleading image or document. The returned file name is a
 * single Windows-safe name and never contains URL query or fragment data.
 */
Decision Decide(std::wstring_view effectiveUrl,
    std::wstring_view rawContentType,
    std::wstring_view rawContentDisposition = {});

} // namespace snowdesktop::url_drop_resource
