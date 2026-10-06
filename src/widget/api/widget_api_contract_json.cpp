#include "widget_api_contract_json.h"

#include "widget_api_registry.h"

#include <locale>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace snowdesktop::widget_api
{
namespace
{
void WriteJsonString(std::ostream& output, std::string_view value)
{
    static constexpr char kHex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20)
            {
                output << "\\u00" << kHex[character >> 4]
                       << kHex[character & 0x0f];
            }
            else
            {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}

void WriteNullableString(std::ostream& output, const char* value)
{
    if (!value) output << "null";
    else WriteJsonString(output, value);
}
}

std::string SerializePublicApiContractJson()
{
    const auto functions = PublicApiFunctionContracts();
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"ok\":true,\"schemaVersion\":1,\"apiVersion\":2,"
              "\"sandboxLibraries\":[";
    bool first = true;
    for (const std::string_view library : SandboxLibraries())
    {
        if (!first) output << ',';
        first = false;
        WriteJsonString(output, library);
    }
    output << "],\"libraries\":[";

    std::unordered_set<std::string_view> emitted;
    bool firstLibrary = true;
    for (const PublicApiFunctionContract& libraryEntry : functions)
    {
        const std::string_view library(libraryEntry.library);
        if (!emitted.insert(library).second) continue;
        if (!firstLibrary) output << ',';
        firstLibrary = false;
        output << "{\"name\":";
        WriteJsonString(output, library);
        output << ",\"functions\":[";
        bool firstFunction = true;
        for (const PublicApiFunctionContract& function : functions)
        {
            if (std::string_view(function.library) != library) continue;
            if (!firstFunction) output << ',';
            firstFunction = false;
            output << "{\"name\":";
            WriteJsonString(output, function.name);
            output << ",\"qualifiedName\":";
            const std::string qualifiedName = std::string(library) + "." +
                function.name;
            WriteJsonString(output, qualifiedName);
            output << ",\"sinceApi\":" << function.sinceApi
                   << ",\"untilApi\":";
            if (function.untilApi == 0) output << "null";
            else output << function.untilApi;
            output << ",\"requiredPermission\":";
            WriteNullableString(output, function.requiredPermission);
            output << '}';
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}
}
