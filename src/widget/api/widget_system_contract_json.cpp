#include "widget_system_contract_json.h"

#include "widget_contract_json_writer.h"
#include "widget_api_registry.h"

#include <locale>
#include <ostream>
#include <sstream>
#include <string_view>

namespace snowdesktop::widget_api
{
namespace
{
using widget_contract_json::WriteJsonString;

void WriteNullableString(std::ostream& output, const char* value)
{
    if (value && value[0] != '\0') WriteJsonString(output, value);
    else output << "null";
}

template <typename Range, typename Writer>
void WriteJsonArray(std::ostream& output, const Range& values,
    Writer&& writer)
{
    output << '[';
    bool first = true;
    for (const auto& value : values)
    {
        if (!first) output << ',';
        first = false;
        writer(output, value);
    }
    output << ']';
}

std::string_view PreviewName(SystemCapabilityPreview preview) noexcept
{
    return preview == SystemCapabilityPreview::Deterministic
        ? "deterministic" : "noSideEffects";
}

void WriteCommon(std::ostream& output, const char* name,
    const char* feature, const char* permission,
    SystemCapabilityPreview preview)
{
    output << "\"name\":";
    WriteJsonString(output, name);
    output << ",\"feature\":";
    WriteJsonString(output, feature);
    output << ",\"permission\":";
    WriteNullableString(output, permission);
    output << ",\"preview\":";
    WriteJsonString(output, PreviewName(preview));
}

void WriteFunction(std::ostream& output,
    const SystemFunctionContract& contract)
{
    output << '{';
    WriteCommon(output, contract.name, contract.feature, nullptr,
        SystemCapabilityPreview::Deterministic);
    output << ",\"parameters\":";
    WriteJsonArray(output, contract.parameters,
        [](std::ostream& stream,
            const SystemFunctionParameterContract& parameter) {
            stream << "{\"name\":";
            WriteJsonString(stream, parameter.name);
            stream << ",\"type\":";
            WriteJsonString(stream, parameter.type);
            stream << ",\"optional\":"
                   << (parameter.optional ? "true" : "false") << '}';
        });
    output << ",\"resultType\":";
    WriteJsonString(output, contract.resultType);
    output << '}';
}

void WriteDataTopic(std::ostream& output,
    const SystemDataTopicContract& contract)
{
    output << '{';
    WriteCommon(output, contract.name, contract.feature,
        contract.requiredPermission, contract.preview);
    output << ",\"minimumIntervalMs\":" << contract.minimumIntervalMs
           << ",\"hiddenIntervalMs\":" << contract.hiddenIntervalMs
           << ",\"idleGraceMs\":" << contract.idleGraceMs
           << ",\"highRisk\":" << (contract.highRisk ? "true" : "false")
           << ",\"supportsHiddenContinue\":"
           << (contract.supportsHiddenContinue ? "true" : "false")
           << ",\"optionsType\":";
    WriteJsonString(output, contract.optionsType);
    output << ",\"valueType\":";
    WriteJsonString(output, contract.valueType);
    output << '}';
}

void WriteTask(std::ostream& output, const SystemTaskContract& contract)
{
    output << '{';
    WriteCommon(output, contract.name, contract.feature,
        contract.requiredPermission, contract.preview);
    output << ",\"requiresTrustedGesture\":"
           << (contract.requiresTrustedGesture ? "true" : "false")
           << ",\"maximumPerInstance\":"
           << contract.maximumPerInstance << ",\"argumentsType\":";
    WriteNullableString(output, contract.argumentsType);
    output << ",\"resultType\":";
    WriteNullableString(output, contract.resultType);
    output << '}';
}
}

std::string SerializeSystemCapabilityContractJson()
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"ok\":true,\"schemaVersion\":1,\"apiVersion\":2,"
              "\"functions\":";
    WriteJsonArray(output, SystemFunctionContracts(), WriteFunction);
    output << ",\"dataTopics\":";
    WriteJsonArray(output, SystemDataTopicContracts(), WriteDataTopic);
    output << ",\"tasks\":";
    WriteJsonArray(output, SystemTaskContracts(), WriteTask);
    output << '}';
    return output.str();
}
}
