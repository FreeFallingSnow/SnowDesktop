#include "widget_author_migrate.h"

#include "json_value.h"
#include "widget_package.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_authoring
{
namespace
{
std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), length,
        nullptr, nullptr);
    return result;
}

std::string JsonEscape(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[character >> 4]);
                result.push_back(hex[character & 0x0f]);
            }
            else result.push_back(static_cast<char>(character));
            break;
        }
    }
    return result;
}

std::string JsonString(std::string_view value)
{
    return "\"" + JsonEscape(value) + "\"";
}

void SerializeJsonValue(std::ostringstream& output,
    const JsonValue& value, int indent)
{
    switch (value.type)
    {
    case JsonValue::Type::Null:
        output << "null";
        return;
    case JsonValue::Type::Boolean:
        output << (value.boolean ? "true" : "false");
        return;
    case JsonValue::Type::Number:
        if (std::isfinite(value.number) &&
            std::floor(value.number) == value.number)
            output << static_cast<std::int64_t>(value.number);
        else
            output << std::setprecision(17) << value.number;
        return;
    case JsonValue::Type::String:
        output << JsonString(value.string);
        return;
    case JsonValue::Type::Array:
        if (value.array.empty())
        {
            output << "[]";
            return;
        }
        output << "[\n";
        for (std::size_t index = 0; index < value.array.size(); ++index)
        {
            output << std::string(indent + 2, ' ');
            SerializeJsonValue(output, value.array[index], indent + 2);
            output << (index + 1 == value.array.size() ? "\n" : ",\n");
        }
        output << std::string(indent, ' ') << ']';
        return;
    case JsonValue::Type::Object:
        if (value.object.empty())
        {
            output << "{}";
            return;
        }
        break;
    }
    std::vector<std::string> keys;
    keys.reserve(value.object.size());
    for (const auto& [key, child] : value.object)
    {
        (void)child;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    output << "{\n";
    for (std::size_t index = 0; index < keys.size(); ++index)
    {
        output << std::string(indent + 2, ' ')
            << JsonString(keys[index]) << ": ";
        SerializeJsonValue(output, value.object.at(keys[index]), indent + 2);
        output << (index + 1 == keys.size() ? "\n" : ",\n");
    }
    output << std::string(indent, ' ') << '}';
}

JsonValue Number(double number)
{
    JsonValue result;
    result.type = JsonValue::Type::Number;
    result.number = number;
    return result;
}

JsonValue String(std::string value)
{
    JsonValue result;
    result.type = JsonValue::Type::String;
    result.string = std::move(value);
    return result;
}

bool ReadText(const std::filesystem::path& path, std::string& output)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0 || size > 1024 * 1024) return false;
    input.seekg(0, std::ios::beg);
    output.assign(std::istreambuf_iterator<char>(input), {});
    return static_cast<bool>(input) || input.eof();
}

bool WriteText(const std::filesystem::path& path,
    std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return static_cast<bool>(output);
}

std::string FirstValidationError(
    const snowdesktop::widget::ValidationReport& report)
{
    for (const auto& issue : report.issues)
    {
        if (issue.severity ==
            snowdesktop::widget::ValidationSeverity::Error)
            return issue.code + ": " + issue.message;
    }
    return "component package validation failed";
}

bool CopyValidatedTree(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error)
{
    std::error_code filesystemError;
    std::filesystem::create_directory(destination, filesystemError);
    if (filesystemError)
    {
        error = "cannot create the migration staging directory: " +
            filesystemError.message();
        return false;
    }
    for (std::filesystem::recursive_directory_iterator iterator(source,
            std::filesystem::directory_options::none, filesystemError), end;
        !filesystemError && iterator != end; iterator.increment(filesystemError))
    {
        const auto relative = std::filesystem::relative(
            iterator->path(), source, filesystemError);
        if (filesystemError) break;
        const auto target = destination / relative;
        if (iterator->is_directory(filesystemError))
            std::filesystem::create_directory(target, filesystemError);
        else if (iterator->is_regular_file(filesystemError))
            std::filesystem::copy_file(iterator->path(), target,
                std::filesystem::copy_options::none, filesystemError);
        else
        {
            error = "validated package contains an unsupported entry";
            return false;
        }
        if (filesystemError) break;
    }
    if (filesystemError)
    {
        error = "cannot copy the migration source: " +
            filesystemError.message();
        return false;
    }
    return true;
}

class StagingCleanup
{
public:
    explicit StagingCleanup(std::filesystem::path path)
        : path_(std::move(path))
    {
    }
    ~StagingCleanup()
    {
        if (committed_) return;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    void Commit() { committed_ = true; }

private:
    std::filesystem::path path_;
    bool committed_ = false;
};
} // namespace

std::string MigrationDraftReport::ToJson() const
{
    std::ostringstream result;
    result << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"stage\":" << JsonString(stage)
        << ",\"error\":" << JsonString(error)
        << ",\"source\":" << JsonString(WideToUtf8(source.wstring()))
        << ",\"output\":" << JsonString(WideToUtf8(output.wstring()))
        << ",\"originalEntry\":" << JsonString(originalEntry)
        << ",\"draftEntry\":" << JsonString(draftEntry) << '}';
    return result.str();
}

MigrationDraftReport CreateV2MigrationDraft(
    const std::filesystem::path& source,
    const std::filesystem::path& output)
{
    MigrationDraftReport result;
    result.source = source;
    result.output = output;
    snowdesktop::widget::WidgetPackageValidator validator;
    snowdesktop::widget::PackageManifest manifest;
    const auto sourceValidation = validator.ValidateDirectory(
        source, &manifest);
    if (!sourceValidation.Ok())
    {
        result.stage = "source.validate";
        result.error = FirstValidationError(sourceValidation);
        return result;
    }
    if (manifest.schemaVersion != 1 || manifest.apiVersion != 1)
    {
        result.stage = "source.contract";
        result.error = "migrate-v2 requires a schema/API v1 package";
        return result;
    }
    result.originalEntry = manifest.entry;

    std::error_code filesystemError;
    if (std::filesystem::exists(output, filesystemError) || filesystemError)
    {
        result.stage = "output.exists";
        result.error = "migration output already exists and will not be overwritten";
        return result;
    }
    const auto outputParent = output.parent_path().empty()
        ? std::filesystem::current_path() : output.parent_path();
    if (!std::filesystem::is_directory(outputParent, filesystemError) ||
        filesystemError)
    {
        result.stage = "output.parent";
        result.error = "migration output parent is not a directory";
        return result;
    }
    const auto canonicalSource = std::filesystem::weakly_canonical(
        source, filesystemError);
    if (filesystemError)
    {
        result.stage = "source.canonical";
        result.error = "cannot canonicalize the migration source";
        return result;
    }
    const auto canonicalParent = std::filesystem::weakly_canonical(
        outputParent, filesystemError);
    if (filesystemError)
    {
        result.stage = "output.canonical";
        result.error = "cannot canonicalize the migration output parent";
        return result;
    }
    const auto canonicalOutput = canonicalParent / output.filename();
    const auto relativeToSource = std::filesystem::relative(
        canonicalOutput, canonicalSource, filesystemError);
    if (!filesystemError && !relativeToSource.empty() &&
        *relativeToSource.begin() != L"..")
    {
        result.stage = "output.insideSource";
        result.error = "migration output cannot be inside the source package";
        return result;
    }
    filesystemError.clear();

    const auto staging = canonicalParent /
        (L".snowwidget-migrate-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    if (std::filesystem::exists(staging, filesystemError))
    {
        result.stage = "output.staging";
        result.error = "migration staging path collision";
        return result;
    }
    StagingCleanup cleanup(staging);
    if (!CopyValidatedTree(canonicalSource, staging, result.error))
    {
        result.stage = "output.copy";
        return result;
    }

    std::string manifestText;
    JsonValue root;
    std::string parseError;
    if (!ReadText(staging / L"widget.json", manifestText) ||
        !ParseJson(manifestText, root, &parseError) || !root.IsObject())
    {
        result.stage = "manifest.read";
        result.error = parseError.empty()
            ? "cannot read the source manifest" : parseError;
        return result;
    }
    std::filesystem::path draftEntry = L"main-v2.lua";
    if (std::filesystem::exists(staging / draftEntry, filesystemError))
        draftEntry = L"main-v2-draft.lua";
    result.draftEntry = WideToUtf8(draftEntry.generic_wstring());
    root.object["schemaVersion"] = Number(2);
    root.object["apiVersion"] = Number(2);
    root.object["entry"] = String(result.draftEntry);
    std::ostringstream serialized;
    SerializeJsonValue(serialized, root, 0);
    serialized << '\n';
    if (!WriteText(staging / L"widget.json", serialized.str()))
    {
        result.stage = "manifest.write";
        result.error = "cannot write the v2 draft manifest";
        return result;
    }

    const std::string scaffold =
        "-- Generated by snowwidget migrate-v2.\n"
        "-- Port behavior from the preserved API v1 entry before publishing.\n"
        "local function render(context, model)\n"
        "    -- TODO: replace API v1 globals with API v2 host libraries.\n"
        "end\n\n"
        "return widget.define({\n"
        "    render = render,\n"
        "})\n";
    if (!WriteText(staging / draftEntry, scaffold))
    {
        result.stage = "entry.write";
        result.error = "cannot write the v2 entry scaffold";
        return result;
    }
    std::filesystem::path guide = L"MIGRATION-V2.md";
    if (std::filesystem::exists(staging / guide, filesystemError))
        guide = L"MIGRATION-V2-DRAFT.md";
    const std::string guideText =
        "# API v2 migration draft\n\n"
        "The original API v1 entry is preserved at `" +
        result.originalEntry + "`. The manifest now points to `" +
        result.draftEntry + "`.\n\n"
        "Port lifecycle, storage, permissions, data/task calls, drawing or view "
        "nodes, resources, localization, and interactions. Then run "
        "`snowwidget lint`, `test`, `preview`, and `validate` on this directory.\n";
    if (!WriteText(staging / guide, guideText))
    {
        result.stage = "guide.write";
        result.error = "cannot write the migration guide";
        return result;
    }

    const auto draftValidation = validator.ValidateDirectory(staging);
    if (!draftValidation.Ok())
    {
        result.stage = "draft.validate";
        result.error = FirstValidationError(draftValidation);
        return result;
    }
    std::filesystem::rename(staging, canonicalOutput, filesystemError);
    if (filesystemError)
    {
        result.stage = "output.commit";
        result.error = "cannot commit the migration draft: " +
            filesystemError.message();
        return result;
    }
    cleanup.Commit();
    result.output = canonicalOutput;
    result.ok = true;
    result.stage = "complete";
    return result;
}

} // namespace snowdesktop::widget_authoring
