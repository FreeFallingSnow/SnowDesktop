#include "usage_guide.h"
#include "atomic_file.h"
#include "json_value.h"

namespace snowdesktop::usage_guide
{
bool LoadExpanded(const std::filesystem::path& path, bool& expanded, std::string* error)
{
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    if (!exists) { expanded = true; return true; }
    std::string text;
    if (!atomic_file::ReadAll(path, text, error)) return false;
    JsonValue root;
    if (ParseJson(text, root) && root.IsObject())
    {
        const auto* version = root.Find("version");
        const auto* value = root.Find("expanded");
        if (version && version->IsNumber() && version->number == 1 && value && value->IsBoolean())
        { expanded = value->boolean; return true; }
    }
    if (error) *error = "Invalid usage guide preference";
    return false;
}
bool SaveExpanded(const std::filesystem::path& path, bool expanded, std::string* error)
{
    return atomic_file::WriteAll(path, expanded ?
        "{\"version\":1,\"expanded\":true}\n" : "{\"version\":1,\"expanded\":false}\n", {}, error);
}
}
