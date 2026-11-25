#include "control_settings_search.h"

#include "control_native_widget_settings.h"
#include "control_runtime_settings.h"
#include "icons/icon_beautify.h"
#include "icons/large_icon_config.h"
#include <algorithm>
#include <tuple>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
constexpr std::string_view Whitespace = " \t\r\n";

std::string Fold(std::string_view text)
{
    std::string result(text);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return result;
}

bool Contains(std::string_view text, std::string_view word)
{
    return text.find(word) != std::string_view::npos;
}
}

SettingsDescriptionSearch::SettingsDescriptionSearch(const Value& params)
{
    Fields(params, {"query", "offset", "limit"});
    const auto text = Text(params, "query");
    const auto first = text.find_first_not_of(Whitespace);
    if (first == std::string::npos || text.size() > 256)
        throw Error("invalid_params", "query must contain 1 to 256 bytes of non-blank text", -32602);
    query_ = text.substr(first, text.find_last_not_of(Whitespace) - first + 1);
    foldedQuery_ = Fold(query_);
    for (std::size_t start = 0; start < foldedQuery_.size();)
    {
        const auto end = foldedQuery_.find_first_of(Whitespace, start);
        words_.push_back(foldedQuery_.substr(start, end - start));
        if (end == std::string::npos) break;
        start = foldedQuery_.find_first_not_of(Whitespace, end);
    }
    if (params.Find("offset")) offset_ = static_cast<std::size_t>(Integer(params, "offset", 0, 1000000));
    if (params.Find("limit")) limit_ = static_cast<std::size_t>(Integer(params, "limit", 1, 50));
}

void SettingsDescriptionSearch::AddField(const SettingsSearchTarget& target,
    std::string_view field, std::string_view description)
{
    if (description.empty()) return;
    const auto fieldKey = Fold(field);
    const auto qualifiedKey = Fold(target.target.kind) + "." + fieldKey;
    const auto help = Fold(description);
    const auto name = Fold(target.name);
    if (!std::all_of(words_.begin(), words_.end(), [&](const auto& word) {
            return Contains(qualifiedKey, word) || Contains(help, word) || Contains(name, word);
        })) return;

    const auto separator = fieldKey.find_last_of('.');
    const auto leaf = std::string_view(fieldKey).substr(separator == std::string::npos ? 0 : separator + 1);
    int rank = 3;
    if (foldedQuery_ == fieldKey || foldedQuery_ == qualifiedKey || foldedQuery_ == leaf) rank = 0;
    else if (Contains(qualifiedKey, foldedQuery_)) rank = 1;
    else if (Contains(help, foldedQuery_)) rank = 2;
    matches_.push_back({target, std::string(field), std::string(description), rank});
}

void SettingsDescriptionSearch::AddFields(const SettingsSearchTarget& target,
    const Value& fields, std::string_view parent)
{
    for (const auto& [key, schema] : fields.object)
        AddSchema(target, schema, parent.empty() ? key : std::string(parent) + "." + key);
}

void SettingsDescriptionSearch::AddSchema(const SettingsSearchTarget& target,
    const Value& schema, const std::string& field)
{
    if (const auto* help = schema.Find("description"); help && help->IsString())
        AddField(target, field, help->string);
    if (const auto* children = schema.Find("properties"))
        AddFields(target, *children, field);
    if (const auto* items = schema.Find("items"))
        AddSchema(target, *items, field + "[]");
    if (const auto* entries = schema.Find("additionalProperties"); entries && entries->IsObject())
        AddSchema(target, *entries, field + ".*");
}

void SettingsDescriptionSearch::AddHostFields()
{
    const auto global = DescribeGlobalSettingsFields();
    for (const auto& [kind, schema] : global.Find("properties")->object)
    {
        if (const auto* help = schema.Find("description"); help && help->IsString())
            AddField({{"global", {}}}, kind, help->string);
        if (const auto* fields = schema.Find("properties"))
            AddFields({{kind, {}}}, *fields);
    }
    AddFields({{"page", {}}, true}, DescribePageSettingsFields());
    AddFields({{"debug-environment", {}}, true}, DescribeDebugEnvironmentFields(false));
    AddFields({{"native-widget", {}}, true}, DescribeNativeWidgetFields({}));
    AddFields({{"icon-appearance", {}}, true}, *DescribeIconAppearanceFields({}).Find("properties"));
    AddFields({{"large-icon", {}}, true}, DescribeLargeIconFields({}));
    AddFields({{"calendar-event", {}}, true}, DescribeCalendarEventFields());
    const auto series = DescribeCalendarSeriesFields();
    AddFields({{"calendar-series", {}}, true}, *series.Find("event"), "event");
    AddFields({{"calendar-series", {}}, true}, *series.Find("rule"), "rule");
    AddFields({{"fonts", {}}}, DescribeFontSettingsFields());
    AddFields({{"runtime-services", {}}}, DescribeRuntimeSettingsFields(false, false));
    AddField({{"runtime-services", {}}, false, DesktopStyleAction}, DesktopStyleAction, DesktopStyleActionDescription);
    AddField({{"runtime-services", {}}, false, DesktopStyleAnimationAction}, DesktopStyleAnimationAction, DesktopStyleAnimationActionDescription);
    AddFields({{"package", {}}, true}, DescribePackageSettingsFields(false, false));
    AddFields({{"agent-skills", {}}}, DescribeAgentSkillSettingsFields());
    AddFields({{"widget", {}}, true, "appearance"}, *DescribeWidgetAppearanceFields().Find("properties"));
}

Value SettingsDescriptionSearch::Results(std::size_t unavailableWidgets)
{
    const auto sortKey = [](const Match& match) {
        return std::tie(match.rank, match.target.target.kind, match.target.target.id,
            match.target.action, match.field, match.description);
    };
    std::sort(matches_.begin(), matches_.end(), [&](const auto& left, const auto& right) {
        return sortKey(left) < sortKey(right);
    });
    auto results = Array();
    for (std::size_t index = offset_; index < matches_.size() && index < offset_ + limit_; ++index)
    {
        const auto& match = matches_[index];
        auto item = Object({{"target", TargetJson(match.target.target)},
            {"field", String(match.field)}, {"description", String(match.description)}});
        if (match.target.requiresId) item.object.emplace("requiresTargetId", Boolean(true));
        if (!match.target.action.empty()) item.object.emplace("action", String(match.target.action));
        if (!match.target.name.empty()) item.object.emplace("name", String(match.target.name));
        results.array.push_back(std::move(item));
    }
    return Object({{"query", String(query_)}, {"results", std::move(results)},
        {"total", Number(static_cast<double>(matches_.size()))}, {"offset", Number(static_cast<double>(offset_))},
        {"limit", Number(static_cast<double>(limit_))},
        {"unavailableWidgetCount", Number(static_cast<double>(unavailableWidgets))}});
}
}
