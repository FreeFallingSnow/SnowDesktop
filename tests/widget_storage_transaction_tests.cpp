#include "widget_storage_transaction.h"

#include <cstdlib>
#include <iostream>
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
}

int main()
{
    TestReadWriteAndRemoval();
    TestFailureLeavesSourceUntouched();
    TestFinalQuotaIsAtomic();
    TestHostReservedStateDoesNotConsumeWidgetQuota();
    if (failures != 0)
    {
        std::cerr << failures << " widget storage transaction checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget storage transaction checks passed\n";
    return EXIT_SUCCESS;
}
