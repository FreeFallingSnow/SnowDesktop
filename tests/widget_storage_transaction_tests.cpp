#include "widget_storage_transaction.h"
#include "widget_storage_value.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestReadWriteAndRemoval()
{
    using snowdesktop::widget_runtime::StorageMap;
    using snowdesktop::widget_runtime::WidgetStorageTransaction;
    StorageMap source = {
        { "instance.keep", "old" },
        { "other.keep", "untouched" },
    };
    WidgetStorageTransaction transaction(source, "instance");
    std::string error;
    bool changed = false;
    Check(transaction.Set("keep", "new", changed, error) && changed,
        "set stages a changed value");
    Check(transaction.Get("keep", error) == "new",
        "transaction reads its own staged value");
    Check(transaction.Remove("keep", changed, error) && changed &&
            !transaction.Get("keep", error).has_value(),
        "remove hides a staged value");
    Check(transaction.Set("便笺", "可用", changed, error) &&
            transaction.ValidateCommit(error),
        "valid UTF-8 keys and values are accepted");
    const auto candidate = transaction.TakeCandidate();
    Check(candidate.contains("instance.便笺") &&
            candidate.contains("other.keep"),
        "commit candidate preserves other widget instances");
}

void TestFailureLeavesSourceUntouched()
{
    using snowdesktop::widget_runtime::StorageMap;
    using snowdesktop::widget_runtime::WidgetStorageTransaction;
    StorageMap source = { { "instance.value", "original" } };
    WidgetStorageTransaction transaction(source, "instance");
    std::string error;
    bool changed = false;
    const std::string invalidUtf8(1, static_cast<char>(0xff));
    Check(!transaction.Set("value", invalidUtf8, changed, error),
        "invalid UTF-8 rejects the staged operation");
    Check(source.at("instance.value") == "original",
        "failed staging never mutates the source map");
}

void TestFinalQuotaIsAtomic()
{
    using snowdesktop::widget_runtime::StorageMap;
    using snowdesktop::widget_runtime::WidgetStorageTransaction;
    StorageMap source;
    for (std::size_t index = 0;
         index < WidgetStorageTransaction::kMaximumKeys; ++index)
        source["instance.key" + std::to_string(index)] = "value";

    WidgetStorageTransaction replacement(source, "instance");
    std::string error;
    bool changed = false;
    Check(replacement.Set("new", "value", changed, error) &&
            replacement.Remove("key0", changed, error) &&
            replacement.ValidateCommit(error),
        "quota is evaluated on the final transaction snapshot");

    WidgetStorageTransaction overflow(source, "instance");
    Check(overflow.Set("new", "value", changed, error) &&
            !overflow.ValidateCommit(error) &&
            error == "widget storage quota exceeded",
        "an over-quota final snapshot rejects the whole transaction");
}

void TestHostReservedStateDoesNotConsumeWidgetQuota()
{
    using snowdesktop::widget_runtime::StorageMap;
    using snowdesktop::widget_runtime::WidgetStorageTransaction;
    StorageMap source;
    for (std::size_t index = 0;
         index < WidgetStorageTransaction::kMaximumKeys; ++index)
        source["instance.key" + std::to_string(index)] = "value";
    for (std::size_t index = 0; index < 32; ++index)
        source["instance.__host.logicalSlot.slot" +
            std::to_string(index)] = std::string(65536, 'x');

    WidgetStorageTransaction transaction(source, "instance");
    std::string error;
    Check(transaction.ValidateCommit(error),
        "host-reserved logical slot state must not consume Lua storage quota");
}

void TestHostMetadataIsAtomicAndHiddenFromQuota()
{
    using snowdesktop::widget_runtime::StorageMap;
    using snowdesktop::widget_runtime::WidgetStorageTransaction;
    StorageMap source = { { "instance.value", "legacy" } };
    WidgetStorageTransaction transaction(source, "instance");
    std::string error;
    bool changed = false;
    const std::string metadataKey =
        "__host.typedStorage.value.with.a.long.user.key";
    Check(transaction.SetHostMetadata(metadataKey,
            "snowdesktop.typed.v1", changed, error) && changed &&
            transaction.GetHostMetadata(metadataKey, error) ==
                "snowdesktop.typed.v1" &&
            transaction.ValidateCommit(error),
        "typed-storage host metadata participates in the atomic candidate without consuming quota");
    Check(transaction.RemoveHostMetadata(
            metadataKey, changed, error) && changed &&
            !transaction.GetHostMetadata(metadataKey, error).has_value(),
        "typed-storage host metadata can be removed atomically");
}

void TestHostMetadataDoesNotReducePublicOperationLimit()
{
    using snowdesktop::widget_runtime::StorageMap;
    using snowdesktop::widget_runtime::WidgetStorageTransaction;
    StorageMap source;
    WidgetStorageTransaction transaction(source, "instance");
    std::string error;
    bool changed = false;
    for (std::size_t index = 0;
         index < WidgetStorageTransaction::kMaximumOperations; ++index)
    {
        const std::string key = "value" + std::to_string(index % 2);
        Check(transaction.Set(key, std::to_string(index), changed, error) &&
                transaction.SetHostMetadata(
                    "__host.typedStorage." + key,
                    "snowdesktop.typed.v1", changed, error),
            "one public typed write consumes one transaction operation");
    }
    Check(!transaction.Set("overflow", "value", changed, error),
        "the public transaction operation limit remains 1024 writes");
}

void TestTypedStorageValueRoundTrip()
{
    using Value = snowdesktop::widget_runtime::InteractionValue;
    Value root;
    root.type = Value::Type::Object;
    Value enabled;
    enabled.type = Value::Type::Boolean;
    enabled.boolean = true;
    root.object.emplace("enabled", enabled);
    Value maximum;
    maximum.type = Value::Type::Integer;
    maximum.integer = std::numeric_limits<long long>::max();
    root.object.emplace("maximum", maximum);
    Value ratio;
    ratio.type = Value::Type::Number;
    ratio.number = 3.25;
    root.object.emplace("ratio", ratio);
    Value items;
    items.type = Value::Type::Array;
    Value text;
    text.type = Value::Type::String;
    text.string = "line\n\"quoted\" 雪";
    items.array.push_back(text);
    items.array.push_back(Value{});
    root.object.emplace("items", items);

    std::string encoded;
    std::string error;
    Value decoded;
    Check(snowdesktop::widget_runtime::EncodeTypedStorageValue(
            root, encoded, error) && !encoded.empty() &&
            snowdesktop::widget_runtime::DecodeTypedStorageValue(
                encoded, decoded, error) && decoded == root,
        "typed storage values preserve JSON-like types, exact integers, and escaped UTF-8 strings");
    Check(snowdesktop::widget_runtime::TypedStorageMetadataKey("state") ==
            "__host.typedStorage.state",
        "typed storage metadata keys stay in the host-reserved instance namespace");
}

void TestTypedStorageValueRejectsMalformedPayloads()
{
    snowdesktop::widget_runtime::InteractionValue decoded;
    std::string error;
    Check(!snowdesktop::widget_runtime::DecodeTypedStorageValue(
            "[\"unknown\",1]", decoded, error) && !error.empty(),
        "unknown typed storage tags are rejected");
    std::string nested;
    for (int index = 0; index < 40; ++index) nested.push_back('[');
    nested += "0";
    for (int index = 0; index < 40; ++index) nested.push_back(']');
    Check(!snowdesktop::widget_runtime::DecodeTypedStorageValue(
            nested, decoded, error),
        "deeply nested persisted payloads are rejected before JSON recursion");
}
}

int main()
{
    TestReadWriteAndRemoval();
    TestFailureLeavesSourceUntouched();
    TestFinalQuotaIsAtomic();
    TestHostReservedStateDoesNotConsumeWidgetQuota();
    TestHostMetadataIsAtomicAndHiddenFromQuota();
    TestHostMetadataDoesNotReducePublicOperationLimit();
    TestTypedStorageValueRoundTrip();
    TestTypedStorageValueRejectsMalformedPayloads();
    if (failures != 0)
    {
        std::cerr << failures << " widget storage transaction checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget storage transaction checks passed\n";
    return EXIT_SUCCESS;
}
