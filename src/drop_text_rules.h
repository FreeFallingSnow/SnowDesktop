#pragma once

#include <array>
#include <string>
#include <string_view>

namespace snowdesktop::drop_text_rules
{

enum class Source
{
    UnicodeText,
    AdvertisedUri,
};

enum class Kind
{
    Empty,
    PlainText,
    HttpUrl,
    HttpsUrl,
    FtpUrl,
    FileUrl,
    DataUrl,
    OpaqueUri,
};

struct Classification
{
    Kind kind = Kind::Empty;
    std::wstring value;
};

using ResourceCandidates = std::array<Classification, 3>;

/**
 * Classify one textual OLE payload without attempting to resolve it.
 *
 * Plain clipboard text is deliberately conservative: only URI forms that
 * cannot reasonably be prose (scheme://..., file:..., data:...) are treated
 * as resource references. A format explicitly advertised as a URI can use
 * any RFC-style scheme. The returned value is trimmed but otherwise intact.
 */
Classification Classify(std::wstring_view input, Source source);

// Preserve every standard textual field so a syntactically recognized but
// unusable data: or file: candidate cannot hide a later valid fallback.
ResourceCandidates ClassifyResourceCandidates(
    std::wstring_view advertisedWide,
    std::wstring_view advertisedAnsi,
    std::wstring_view unicodeText);

/**
 * Select the usable reference exposed by the standard OLE URI/text fields.
 * A private advertised URI is retained as an opaque fallback, but it does not
 * hide a standard HTTP, HTTPS, FTP, local-file, or inline-data URI exposed in
 * another field by the same source.
 */
Classification SelectResourceReference(
    std::wstring_view advertisedWide,
    std::wstring_view advertisedAnsi,
    std::wstring_view unicodeText);
Classification SelectResourceReference(
    const ResourceCandidates& candidates);

// True only for an app-private hierarchical resource marker such as
// native-resource://sdk/image. Known web, file, inline-data, renderer-local,
// and message schemes are deliberately excluded even when malformed.
bool IsPrivateHierarchicalResource(const Classification& classification);

// Lexical gate used before touching a file: URI target. UNC, device,
// relative, and alternate-data-stream paths are deliberately excluded.
bool IsAbsoluteLocalDrivePath(std::wstring_view path);

// Extract a host-only label for a hierarchical URI. User information, port,
// path, query, fragment, and IPv6 brackets are excluded.
std::wstring HierarchicalUriHost(std::wstring_view uri);

} // namespace snowdesktop::drop_text_rules
