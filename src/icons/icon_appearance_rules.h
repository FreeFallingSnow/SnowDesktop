#pragma once
#include "icon_appearance_codec.h"
#include "icon_render_rules.h"
#include <optional>
#include <utility>

namespace snowdesktop::icon_appearance
{
inline IconBeautifySettings Resolve(const IconBeautifySettings& global,
    const IconAppearanceRecord* independent, bool media, bool rawSource,
    bool beautifyFileThumbnails = false)
{
    auto style = independent && !independent->followGlobalTheme ? independent->settings : global;
    if (independent) style.mode = independent->settings.mode;
    style.enabled = !rawSource && icon_render_rules::ShouldBeautify(
        style.enabled, media && !independent, beautifyFileThumbnails);
    return style;
}
inline bool CurrentEdit(const IconAppearanceRequest& request, std::wstring_view target,
    std::uint64_t session, std::uint64_t revision)
{
    return request.session != 0 && request.target == target && request.session == session && request.revision == revision;
}
// A live editor overlay never mutates the records serialized by the layout.
// A null value explicitly previews global inheritance over an existing record.
class EditPreview
{
public:
    bool Update(const IconAppearanceRequest& request, std::wstring_view target,
        std::uint64_t session, std::uint64_t revision, std::optional<IconAppearanceRecord> value)
    {
        if (!CurrentEdit(request, target, session, revision)) return false;
        target_ = request.target; value_ = std::move(value); active_ = true;
        return true;
    }
    const IconAppearanceRecord* Effective(std::wstring_view target, const IconAppearanceRecord* saved) const
    {
        return active_ && target == target_ ? (value_ ? &*value_ : nullptr) : saved;
    }
    bool Clear()
    {
        const bool changed = active_; active_ = false; target_.clear(); value_.reset(); return changed;
    }
private:
    bool active_ = false;
    std::wstring target_;
    std::optional<IconAppearanceRecord> value_;
};
// Intern complete normalized styles. Do not merge styles by an unchecked hash
// or by a source handle alone; the same original may have different owners.
class StyleIdentities
{
public:
    size_t Get(const IconBeautifySettings& value)
    {
        for (size_t i = 0; i < styles_.size(); ++i)
            if (styles_[i].mode == value.mode && styles_[i].preset == value.preset && themes::SameIconTheme(styles_[i], value)) return i;
        styles_.push_back(value); return styles_.size() - 1;
    }
    void Clear() { styles_.clear(); }
private:
    std::vector<IconBeautifySettings> styles_;
};
}
