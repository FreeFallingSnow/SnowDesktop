#include "control_font_settings.h"

#include <algorithm>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
Value Selection(const app_fonts::Selection& value)
{ return Object({{"package", String(value.package)}, {"family", String(value.family)}}); }
std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw Error("settings_unavailable", "Invalid host font name");
    std::string output(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), length, nullptr, nullptr);
    return output;
}
std::filesystem::path Path(const std::string& value)
{
    if (value.empty() || value.size() > 32768 || value.find('\0') != std::string::npos)
        throw Error("invalid_params", "Invalid font source path", -32602);
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw Error("invalid_params", "Font source path must be UTF-8", -32602);
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), length);
    std::filesystem::path path(output);
    if (!path.is_absolute()) throw Error("invalid_params", "Font source path must be absolute", -32602);
    return path;
}
Value Choice(const app_fonts::Choice& choice)
{ return Object({{"selection", Selection(choice.selection)}, {"name", String(Utf8(choice.name))}}); }
app_fonts::Selection ParseSelection(const Value& value)
{
    Fields(value, {"package", "family"});
    const auto* family = value.Find("family");
    if (!family || !family->IsString()) throw Error("invalid_params", "Font family must be a string", -32602);
    auto selection = app_fonts::Selection{Text(value, "package"), family->string};
    if (selection.package.empty() || selection.package.size() > 128 || selection.family.size() > 1024 ||
        selection.package.find('\0') != std::string::npos || selection.family.find('\0') != std::string::npos)
        throw Error("invalid_params", "Invalid font selection", -32602);
    return selection;
}
Value Names(std::initializer_list<std::string_view> names)
{
    auto output = Array();
    for (const auto name : names) output.array.push_back(String(std::string(name)));
    return output;
}
}

bool IsFontSettingsTarget(const Target& target) noexcept { return target.kind == "fonts"; }

Value HandleFontSettings(std::string_view method, const Value& params,
    const Target& target, const FontSettingsContext& context)
{
    if (target.kind != "fonts" || !target.id.empty()) throw Error("settings_target_not_found", "Unknown font settings target");
    const auto snapshot = context.controller.Snapshot();
    if (!snapshot || !context.list) throw Error("settings_unavailable", "Font service is unavailable");
    const auto choices = context.list();
    const auto applied = context.applied ? context.applied() : app_fonts::Selection{};
    auto allChoices = Array();
    for (const auto& choice : choices) allChoices.array.push_back(Choice(choice));
    auto state = Object({{"selection", Selection(snapshot->values.general.font)}, {"applied", Selection(applied)},
        {"choices", allChoices}, {"generation", String(std::to_string(snapshot->generation))},
        {"generalRevision", String(std::to_string(snapshot->domainRevisions.general))}});
    const auto revision = Revision(state);
    const auto read = [&] {
        const int offset = params.Find("offset") ? Integer(params, "offset", 0, 100000) : 0;
        const int limit = params.Find("limit") ? Integer(params, "limit", 1, 200) : 200;
        auto rows = Array();
        for (std::size_t index = static_cast<std::size_t>(offset); index < allChoices.array.size() && index < static_cast<std::size_t>(offset + limit); ++index)
            rows.array.push_back(allChoices.array[index]);
        return Object({{"target", TargetJson(target)}, {"revision", String(revision)},
            {"values", Object({{"selection", Selection(snapshot->values.general.font)}, {"applied", Selection(applied)},
                {"restartRequired", Boolean(snapshot->values.general.font != applied)}, {"choices", std::move(rows)},
                {"choiceCount", Number(static_cast<double>(choices.size()))}, {"offset", Number(offset)}, {"limit", Number(limit)}})}});
    };
    if (method == "settings.get" || method == "settings.catalog")
    {
        Fields(params, {"target", "offset", "limit"});
        auto result = read();
        if (method == "settings.catalog")
        {
            result.object.emplace("fields", Object({{"selection", Object({{"type", String("object")},
                {"properties", Object({{"package", Object({{"type", String("string")}})}, {"family", Object({{"type", String("string")}})}})},
                {"default", Selection({})}, {"optionsSource", String("values.choices; paginate with offset/limit")}})}}));
            result.object.emplace("readOnly", Names({"applied", "restartRequired", "choices", "choiceCount"}));
            result.object.emplace("actions", Names({"import", "restart"}));
        }
        return result;
    }
    if (snapshot->externalReplacementPending) throw Error("settings_unavailable", "Data replacement is pending");
    RequireRevision(params, revision);
    if (method == "settings.update" || method == "settings.reset")
    {
        app_fonts::Selection selection;
        if (method == "settings.update")
        {
            Fields(params, {"target", "ifRevision", "changes"});
            const auto* changes = params.Find("changes");
            if (!changes) throw Error("invalid_params", "changes is required", -32602);
            Fields(*changes, {"selection"});
            const auto* value = changes->Find("selection");
            if (!value) throw Error("invalid_params", "selection is required", -32602);
            selection = ParseSelection(*value);
        }
        else
        {
            Fields(params, {"target", "ifRevision", "keys"});
            const auto* keys = params.Find("keys");
            if (!keys || !keys->IsArray() || keys->array.size() != 1 || !keys->array[0].IsString() || keys->array[0].string != "selection")
                throw Error("invalid_params", "Only selection has a font default", -32602);
        }
        if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.selection == selection; }))
            throw Error("invalid_params", "Font selection is not in the current font catalogue", -32602);
        auto general = snapshot->values.general;
        general.font = std::move(selection);
        context.controller.UpdateGeneral(std::move(general), SettingsUpdateMode::PreviewAndCommit);
        const auto flushed = context.controller.FlushPending();
        if (!flushed.Succeeded()) throw Error(flushed.status == SettingsActionStatus::Busy ? "busy" : "settings_failed", "Font selection could not be committed");
        return Object({{"target", TargetJson(target)}, {"status", String("applied")},
            {"restartRequired", Boolean(context.controller.Snapshot()->values.general.font != applied)}});
    }
    if (method != "settings.action") throw Error("method_not_found", "Unknown font settings method", -32601);
    Fields(params, {"target", "ifRevision", "action", "arguments"});
    const auto action = Text(params, "action");
    if (action == "import")
    {
        if (!context.import) throw Error("settings_unavailable", "Font import is unavailable");
        const auto* arguments = params.Find("arguments");
        if (!arguments) throw Error("invalid_params", "Font import arguments are required", -32602);
        Fields(*arguments, {"path"});
        const auto path = Path(Text(*arguments, "path"));
        std::vector<app_fonts::Choice> imported;
        std::string error;
        if (!context.import(path, imported, error)) throw Error("settings_failed", error.empty() ? "Font import failed" : error);
        auto rows = Array();
        for (const auto& choice : imported) rows.array.push_back(Choice(choice));
        return Object({{"target", TargetJson(target)}, {"status", String("applied")}, {"imported", std::move(rows)},
            {"selectionChanged", Boolean(false)}});
    }
    if (action == "restart")
    {
        if (const auto* arguments = params.Find("arguments")) Fields(*arguments, {});
        SettingsHostActions::Request request;
        request.action = SettingsHostActions::Action::RestartApplication;
        const auto result = context.controller.InvokeHostAction(request);
        if (!result.Succeeded()) throw Error(result.status == SettingsActionStatus::Busy ? "busy" : "settings_failed", "Application restart was rejected");
        return Object({{"target", TargetJson(target)}, {"status", String("accepted")}, {"completion", String("Restart completion is not yet acknowledged")}});
    }
    throw Error("unsupported_action", "Unknown font settings action");
}
}
