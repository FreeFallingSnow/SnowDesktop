#include "widget/storage/widget_storage_transaction.h"
#include "widget/storage/widget_storage_value.h"
#include "widget/storage/widget_settings_preview.h"
#include "widget/storage/widget_storage_persistence.h"

#include <cstdlib>
#include <iostream>
#include <iterator>
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

void TestSnapshotAndNetChanges()
{
    using namespace snowdesktop::widget_runtime;
    StorageMap source = {
        { "instance.value", "original" },
        { "instance.empty", "" },
        { "instance.__host.typedStorage.value", "marker" },
        { "other.value", std::string(65536, 'x') },
    };
    const StorageMap initial = source;
    WidgetStorageTransaction transaction(source, "instance");
    source.clear();
    std::string error;
    bool changed = true;
    Check(!transaction.Changed() &&
            transaction.Get("value", error) == "original" &&
            transaction.Set("value", "original", changed, error) && !changed &&
            transaction.Remove("missing", changed, error) && !changed &&
            !transaction.Changed(),
        "transaction owns its snapshot and same-value or absent writes remain unchanged");
    Check(transaction.Set("value", "replacement", changed, error) && changed &&
            transaction.Changed() &&
            transaction.Set("value", "original", changed, error) && changed &&
            !transaction.Changed(),
        "restoring an existing value cancels the net transaction change");
    Check(transaction.Remove("empty", changed, error) && changed &&
            transaction.Changed() &&
            transaction.Set("empty", "", changed, error) && changed &&
            !transaction.Changed() &&
            transaction.Set("new", "", changed, error) && changed &&
            transaction.Changed() &&
            transaction.Remove("new", changed, error) && changed &&
            !transaction.Changed(),
        "empty values remain distinct from missing values during change cancellation");
    Check(transaction.RemoveHostMetadata("__host.typedStorage.value", changed, error) && changed &&
            transaction.Changed() &&
            transaction.SetHostMetadata("__host.typedStorage.value", "marker", changed, error) && changed &&
            !transaction.Changed() &&
            transaction.SetHostMetadata("__host.new", "", changed, error) && changed &&
            transaction.Changed() &&
            transaction.RemoveHostMetadata("__host.new", changed, error) && changed &&
            !transaction.Changed(),
        "host metadata restores and removals participate in net-change detection");
    Check(transaction.ValidateCommit(error) && transaction.TakeCandidate() == initial,
        "a reverted transaction preserves all original entries, including unrelated widgets");
}

void TestMixedTransactionEquivalence()
{
    using namespace snowdesktop::widget_runtime;
    StorageMap source{{"instance.key0", "original"}, {"other.key0", "unrelated"}};
    WidgetStorageTransaction transaction(source, "instance");
    StorageMap expected = source;
    std::string error;
    for (int index = 0; index < 512; ++index)
    {
        const std::string key = "key" + std::to_string((index / 4) % 7);
        const bool metadata = (index / 28) % 2 != 0;
        const std::string relative = metadata ? "__host." + key : key;
        const std::string full = "instance." + relative;
        const bool remove = index % 4 == 3;
        const std::string value = index % 4 == 2 ? "" : "value";
        const auto before = expected.find(full);
        const bool expectedChanged = remove ? before != expected.end() :
            before == expected.end() || before->second != value;
        bool changed = false;
        const bool ok = remove
            ? (metadata ? transaction.RemoveHostMetadata(relative, changed, error)
                        : transaction.Remove(relative, changed, error))
            : (metadata ? transaction.SetHostMetadata(relative, value, changed, error)
                        : transaction.Set(relative, value, changed, error));
        if (remove) expected.erase(full);
        else expected[full] = value;
        const auto read = metadata ? transaction.GetHostMetadata(relative, error)
                                   : transaction.Get(relative, error);
        const auto found = expected.find(full);
        Check(ok && changed == expectedChanged && error.empty() &&
                read == (found == expected.end() ? std::nullopt :
                    std::optional<std::string>(found->second)) &&
                transaction.Changed() == (expected != source) &&
                transaction.ValidateCommit(error),
            "mixed repeated writes preserve immediate reads, operation results, and full-snapshot net changes");
    }
    Check(transaction.TakeCandidate() == expected && source.at("other.key0") == "unrelated",
        "the final mixed candidate equals the independently updated snapshot");
}

void TestPersistenceBytesAndFailureRecovery()
{
    using namespace snowdesktop::widget_runtime;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"SnowDesktop-storage-equivalence-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(directory);
    const auto path = directory / L"storage.json";
    const auto temporary = std::filesystem::path(path.wstring() + L".tmp");
    struct Cleanup
    {
        std::filesystem::path directory, path, temporary;
        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(temporary, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{directory, path, temporary};
    const auto readText = [&] {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    Check(storage_persistence::SaveStorageMap(path, {}) && readText() == "{}\n",
        "empty storage keeps its exact persisted JSON format");
    std::string controls;
    for (int index = 0; index < 32; ++index) controls.push_back(static_cast<char>(index));
    StorageMap committed{{"z", ""}, {"line\n\"\\", "雪/emoji 😀"}, {"a", controls}};
    const std::string expected = "{\n  \"a\": \""
        "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
        "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f"
        "\",\n  \"line\\n\\\"\\\\\": \"雪/emoji 😀\",\n  \"z\": \"\"\n}\n";
    Check(storage_persistence::SaveStorageMap(path, committed) && readText() == expected,
        "sorted JSON bytes preserve all control escapes, quoted keys, UTF-8, and empty values");
    StorageMap reordered;
    for (const auto& [key, value] : committed) reordered.emplace(key, value);
    reordered.rehash(97);
    Check(storage_persistence::SaveStorageMap(path, reordered) && readText() == expected,
        "serialization is deterministic across map insertion and bucket order");
    StorageMap loaded;
    Check(storage_persistence::ReadStorageMap(path, loaded, [](const auto&) { return false; }) &&
            loaded == committed,
        "the actual storage reader round-trips every escaped value");
    const StorageMap before = committed;
    const auto commit = [&] {
        WidgetStorageTransaction transaction(committed, "instance");
        bool changed = false;
        std::string error;
        if (!transaction.Set("value", "new", changed, error) ||
            !transaction.ValidateCommit(error)) return false;
        auto previous = transaction.TakeCandidate();
        committed.swap(previous);
        if (storage_persistence::SaveStorageMap(path, committed)) return true;
        committed.swap(previous);
        return false;
    };
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "replacement failure fixture locks the actual target file");
    if (lock != INVALID_HANDLE_VALUE)
    {
        Check(!commit() && committed == before && readText() == expected,
            "failed atomic replacement reports failure and preserves both memory and disk");
        CloseHandle(lock);
    }
    lock = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "temporary-open failure fixture locks the actual temporary file");
    if (lock != INVALID_HANDLE_VALUE)
    {
        Check(!commit() && committed == before && readText() == expected,
            "failed temporary-file open reports failure and preserves both memory and disk");
        CloseHandle(lock);
    }
    loaded.clear();
    Check(commit() &&
            storage_persistence::ReadStorageMap(path, loaded, [](const auto&) { return false; }) &&
            loaded == committed && loaded.at("instance.value") == "new",
        "an unlocked retry synchronously persists the complete transaction before success");
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

void TestSettingsPreviewCommitIsolation()
{
    using namespace snowdesktop::widget_runtime;
    StorageMap committed{{"a.color","old-a"},{"b.color","old-b"},{"a.keep","keep"}};
    WidgetSettingsPreviewLayers previews;
    int ownerA=0,ownerB=0;
    std::string error;
    bool changed=false;
    const auto stage=[&](const void* owner,const std::string& instance,const std::string& value) {
        const auto previous=previews.Read(committed);
        WidgetStorageTransaction transaction(previous,instance);
        Check(transaction.Set("color",value,changed,error) && transaction.ValidateCommit(error),
            "preview uses complete real storage transaction validation");
        return previews.Update(owner,instance+".",previous,transaction.TakeCandidate());
    };
    Check(stage(&ownerA,"a","preview-a") && stage(&ownerB,"b","preview-b"),
        "independent owners stage simultaneous previews");
    Check(committed.at("a.color")=="old-a" && committed.at("b.color")=="old-b" &&
        previews.Read(committed).at("a.color")=="preview-a", "previews affect reads without entering committed storage");
    WidgetStorageTransaction ordinary(committed,"a");
    Check(ordinary.Set("color","ordinary-a",changed,error) &&
        ordinary.Set("callback","ordinary-side-effect",changed,error) && ordinary.ValidateCommit(error),
        "ordinary callback transaction remains usable during preview");
    committed=ordinary.TakeCandidate(); previews.InvalidateReads();
    const auto directory=std::filesystem::temp_directory_path() /
        (L"SnowDesktop-settings-preview-"+std::to_wstring(GetCurrentProcessId())+
            L"-"+std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(directory);
    const auto path=directory/L"storage.json";
    struct Cleanup {
        std::filesystem::path directory,path;
        ~Cleanup(){std::error_code ignored; std::filesystem::remove(path,ignored);
            std::filesystem::remove(path.wstring()+L".tmp",ignored);
            std::filesystem::remove(directory,ignored);}
    } cleanup{directory,path};
    StorageMap saved;
    const auto reload=[&] {
        saved.clear();
        return storage_persistence::ReadStorageMap(path,saved,[](const auto&){return false;});
    };
    const auto save=[&] {
        return storage_persistence::SaveStorageMap(path,committed) && reload();
    };
    Check(save(),"ordinary persistence uses actual production atomic writer and reader");
    const auto validate=[&](const StorageMap& candidate,const std::string& prefix) {
        return WidgetStorageTransaction(candidate,prefix.substr(0,prefix.size()-1)).ValidateCommit(error);
    };
    Check(saved.at("a.color")=="ordinary-a" && saved.at("b.color")=="old-b" &&
        saved.at("a.callback")=="ordinary-side-effect", "ordinary persistence snapshot excludes every open preview");
    previews.Remove(&ownerA);
    Check(reload() && saved.at("a.color")=="ordinary-a" &&
        saved.at("b.color")=="old-b" && previews.Read(committed).at("a.color")=="ordinary-a" &&
        committed.at("a.callback")=="ordinary-side-effect", "cancel never reverts ordinary writes made during preview");
    Check(stage(&ownerA,"a","final-a") && previews.Commit(&ownerA,committed,save,validate),
        "preview commit applies its patch to latest committed storage");
    Check(saved.at("a.color")=="final-a" && saved.at("a.callback")=="ordinary-side-effect" &&
        saved.at("b.color")=="old-b" && previews.Read(committed).at("b.color")=="preview-b",
        "committing one owner never persists another owner's active preview");
    const auto before=committed;
    Check(!previews.Commit(&ownerB,committed,[]{return false;},validate) && committed==before &&
        previews.Read(committed).at("b.color")=="preview-b", "failed persistence rolls back committed state while retaining pending preview");
    Check(previews.Commit(&ownerB,committed,save,validate) && saved.at("b.color")=="preview-b",
        "failed commit can be retried without losing another widget's writes");
    Check(stage(&ownerA,"a","rejected") && !stage(&ownerB,"a","conflict"),
        "competing preview owners cannot overlap the same instance");
    previews.Clear();
    Check(previews.Read(committed)==saved,"closing previews leaves committed persistence intact");
}
}

int main()
{
    TestReadWriteAndRemoval();
    TestFailureLeavesSourceUntouched();
    TestSnapshotAndNetChanges();
    TestMixedTransactionEquivalence();
    TestPersistenceBytesAndFailureRecovery();
    TestFinalQuotaIsAtomic();
    TestHostReservedStateDoesNotConsumeWidgetQuota();
    TestHostMetadataIsAtomicAndHiddenFromQuota();
    TestHostMetadataDoesNotReducePublicOperationLimit();
    TestTypedStorageValueRoundTrip();
    TestTypedStorageValueRejectsMalformedPayloads();
    TestSettingsPreviewCommitIsolation();
    if (failures != 0)
    {
        std::cerr << failures << " widget storage transaction checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget storage transaction checks passed\n";
    return EXIT_SUCCESS;
}
