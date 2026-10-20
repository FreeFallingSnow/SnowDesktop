#include "widget/api/widget_logical_slot_manifest.h"
#include "layout/logical_slot_pointer_rules.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace
{
using namespace snowdesktop::widget_runtime;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

LogicalSlotDeclarations ParseDeclarations()
{
    JsonValue root;
    std::string error;
    Check(ParseJson(R"json({
        "primaryApp": {
            "kind": "binding",
            "accepts": ["app.reference"],
            "operation": "reference",
            "replacePolicy": "allow",
            "allowClear": true
        },
        "favorites": {
            "kind": "collection",
            "accepts": ["desktop.item", "filesystem.reference"],
            "operation": "reference",
            "capacity": 2
        }
    })json", root, &error), "declaration fixture JSON must parse");
    LogicalSlotDeclarations declarations;
    std::vector<LogicalSlotManifestError> errors;
    Check(ParseLogicalSlotDeclarations(&root, declarations, errors) &&
            errors.empty() && declarations.size() == 2 &&
            declarations.at("primaryApp").kind ==
                LogicalSlotKind::Binding &&
            declarations.at("favorites").capacity == 2,
        "binding and collection declarations must parse as distinct types");
    return declarations;
}

LogicalSlotItem Item(std::string kind, std::string title,
    std::string target)
{
    return { {}, {}, std::move(kind), std::move(title), "test",
        "application", std::move(target), true };
}

LogicalSlotDeclarations TransferDeclarations()
{
    return {
        { "source", { LogicalSlotKind::Collection,
            { "app.reference", "desktop.item" }, "reference", {},
            false, 3 } },
        { "target", { LogicalSlotKind::Collection,
            { "app.reference", "desktop.item" }, "reference", {},
            false, 2 } },
        { "files", { LogicalSlotKind::Collection,
            { "filesystem.reference" }, "reference", {}, false, 2 } },
    };
}

void TestManifestValidation()
{
    const auto declarations = ParseDeclarations();
    const std::string serialized =
        SerializeLogicalSlotDeclarations(declarations);
    JsonValue roundTrip;
    std::string parseError;
    Check(ParseJson(serialized, roundTrip, &parseError),
        "serialized slot declarations must remain valid JSON");
    LogicalSlotDeclarations parsed;
    std::vector<LogicalSlotManifestError> errors;
    Check(ParseLogicalSlotDeclarations(&roundTrip, parsed, errors) &&
            parsed == declarations,
        "serialized slot declarations must round-trip exactly");

    JsonValue invalid;
    Check(ParseJson(R"json({
        "bad": {"kind":"binding","accepts":["widget"]},
        "list": {"kind":"collection","accepts":["app.reference"],"capacity":65},
        "one": {"kind":"binding","accepts":["app.reference"],"capacity":1}
    })json", invalid, &parseError), "invalid declaration JSON must parse");
    Check(!ParseLogicalSlotDeclarations(&invalid, parsed, errors) &&
            errors.size() >= 3,
        "unknown reference kinds and cross-kind fields must be rejected");
}

void TestBindingAndCollectionTransactions()
{
    LogicalSlotModel model;
    std::string error;
    Check(model.Configure(ParseDeclarations(), error),
        "valid declarations must configure the host model");
    LogicalSlotChange change;
    Check(model.Bind("primaryApp",
            Item("app.reference", "Calendar", "app:calendar"),
            change, error) && change.operation == "bound" &&
            change.revision == 1,
        "an empty binding must atomically accept one app reference");
    const auto first = *model.Find("primaryApp");
    Check(first.items.size() == 1 &&
            first.items[0].reference.starts_with("lsr-") &&
            first.items[0].reference.find("calendar") == std::string::npos,
        "Lua-facing binding references must be stable and opaque");
    Check(model.SetAvailability("primaryApp", first.items[0].id, false,
            change, error) && change.operation == "availability" &&
            !model.Find("primaryApp")->items[0].available &&
        model.SetAvailability("primaryApp", first.items[0].id, false,
            change, error) && change.operation == "unchanged" &&
        model.SetAvailability("primaryApp", first.items[0].id, true,
            change, error) &&
            model.Find("primaryApp")->items[0].available,
        "host catalog reconciliation must update availability without replacing the opaque reference");
    Check(model.Bind("primaryApp",
            Item("app.reference", "Mail", "app:mail"),
            change, error) && change.operation == "replaced" &&
            model.Find("primaryApp")->items[0].title == "Mail",
        "an allow binding must replace its prior item atomically");
    Check(model.Clear("primaryApp", change, error) &&
            change.operation == "cleared" &&
            model.Find("primaryApp")->items.empty(),
        "a clearable binding must preserve an explicit cleared revision");

    Check(model.Bind("favorites",
            Item("desktop.item", "Notes", "desktop:notes"),
            change, error) && change.operation == "added",
        "a collection must accept its declared desktop reference kind");
    Check(model.Bind("favorites",
            Item("filesystem.reference", "Plan", "file:plan"),
            change, error),
        "a collection must accept a second declared reference kind");
    Check(model.Remove("favorites", model.Find("favorites")->items[1].id,
            change, error) &&
        model.Bind("favorites",
            Item("filesystem.reference", "Plan", "file:plan"),
            change, error, 0) &&
        model.Find("favorites")->items.front().title == "Plan",
        "a host drop must insert a new collection reference at its exact boundary");
    Check(!model.Bind("favorites",
            Item("filesystem.reference", "Overflow", "file:overflow"),
            change, error) && error.find("capacity") != std::string::npos,
        "collection capacity must be enforced before mutation");
    const std::string secondId = model.Find("favorites")->items[1].id;
    Check(model.Move("favorites", secondId, 0, change, error) &&
            change.operation == "moved" &&
            model.Find("favorites")->items[0].id == secondId,
        "collection reorder must use stable item ids");
    Check(model.Remove("favorites", secondId, change, error) &&
            change.operation == "removed" &&
            model.Find("favorites")->items.size() == 1,
        "collection removal must remove only the requested reference");
}

void TestPersistenceRoundTrip()
{
    LogicalSlotModel source;
    std::string error;
    Check(source.Configure(ParseDeclarations(), error),
        "source model must configure");
    LogicalSlotChange change;
    Check(source.Bind("primaryApp",
            Item("app.reference", "Calendar", "app:calendar"),
            change, error) &&
        source.Bind("favorites",
            Item("filesystem.reference", "Plan", "file:plan"),
            change, error) &&
        source.SetAvailability("primaryApp",
            source.Find("primaryApp")->items[0].id, false,
            change, error), "fixture mutations must succeed");
    std::unordered_map<std::string, std::string> storage;
    source.Export(storage, "instance-1");
    Check(storage.size() == 2 &&
            storage.contains("instance-1.__host.logicalSlot.primaryApp"),
        "each declared slot must persist in a host-reserved instance key");

    LogicalSlotModel restored;
    Check(restored.Configure(ParseDeclarations(), error) &&
            restored.Restore(storage, "instance-1", error) &&
            restored.Find("primaryApp")->items ==
                source.Find("primaryApp")->items &&
            !restored.Find("primaryApp")->items[0].available &&
            restored.Find("favorites")->revision ==
                source.Find("favorites")->revision,
        "logical slot targets and opaque references must survive restart");
    storage["instance-1.__host.logicalSlot.primaryApp"] =
        R"json({"version":1,"revision":2,"items":[{"id":"x"}]})json";
    Check(!restored.Restore(storage, "instance-1", error) &&
            error.find("primaryApp") != std::string::npos,
        "corrupt host slot state must fail closed with its slot id");
}

void TestCrossCollectionTransfer()
{
    LogicalSlotModel model;
    std::string error;
    Check(model.Configure(TransferDeclarations(), error),
        "cross-collection fixture must configure");
    LogicalSlotChange change;
    Check(model.Bind("source",
            Item("app.reference", "Calendar", "app:calendar"),
            change, error) &&
        model.Bind("source",
            Item("desktop.item", "Notes", "desktop:notes"),
            change, error) &&
        model.Bind("target",
            Item("app.reference", "Mail", "app:mail"),
            change, error),
        "cross-collection fixture items must bind");

    const LogicalSlotItem moving = model.Find("source")->items.front();
    const auto previous = model;
    Check(model.Transfer("source", moving.id, "target", 1,
            change, error) && change.operation == "transferred" &&
            change.slotId == "target" &&
            change.relatedSlotId == "source" &&
            change.itemIds.size() == 1 &&
            change.itemIds.front() == moving.id &&
            model.Find("source")->items.size() == 1 &&
            model.Find("target")->items.size() == 2 &&
            model.Find("target")->items[1].id == moving.id &&
            model.Find("target")->items[1].reference == moving.reference,
        "cross-collection transfer must atomically preserve opaque identity");

    const auto afterTransfer = model;
    LogicalSlotHistory history;
    history.Record(previous, change);
    Check(history.Undo(model, change, error) &&
            change.relatedSlotId == "source" &&
            model.Find("source")->items.size() == 2 &&
            model.Find("target")->items.size() == 1,
        "cross-collection transfer must undo as one model transaction");
    Check(history.Redo(model, change, error) &&
            model.Find("source")->items ==
                afterTransfer.Find("source")->items &&
            model.Find("target")->items ==
                afterTransfer.Find("target")->items,
        "cross-collection transfer must redo both affected slots together");

    const auto beforeRejected = model;
    const std::string remainingId =
        model.Find("source")->items.front().id;
    Check(!model.Transfer("source", remainingId, "files", 0,
            change, error) && error.find("accept") != std::string::npos &&
            model.Find("source")->items ==
                beforeRejected.Find("source")->items &&
            model.Find("files")->items.empty(),
        "incompatible cross-collection transfer must fail without mutation");
}

void TestTransferRebindAndLegacyIdentities()
{
    LogicalSlotModel model;
    std::string error;
    LogicalSlotChange change;
    Check(model.Configure(TransferDeclarations(), error) &&
        model.Bind("source", Item("app.reference", "Calendar", "app:calendar"), change, error),
        "identity fixture binds the original item");
    const auto original = model.Find("source")->items.front();
    Check(model.Transfer("source", original.id, "target", 0, change, error) &&
        model.Bind("source", Item("app.reference", "Calendar", "app:calendar"), change, error),
        "moving an item and adding the same target back to the original slot are valid operations");
    const auto rebound = model.Find("source")->items.front();
    const auto moved = model.Find("target")->items.front();
    const bool distinct = rebound.id != moved.id && rebound.reference != moved.reference &&
        moved.id == original.id && moved.reference == original.reference;

    std::unordered_map<std::string, std::string> storage;
    model.Export(storage, "instance");
    auto& legacySource = storage.at(LogicalSlotModel::StorageKey("instance", "source"));
    for (const auto& tokens : {std::pair{rebound.id, moved.id},
            std::pair{rebound.reference, moved.reference}})
    {
        const auto position = legacySource.find(tokens.first);
        Check(position != std::string::npos, "legacy identity fixture uses actual exported storage");
        legacySource.replace(position, tokens.first.size(), tokens.second);
    }
    LogicalSlotModel repaired;
    Check(repaired.Configure(TransferDeclarations(), error) && repaired.Restore(storage, "instance", error),
        "old cross-slot duplicate identities remain recoverable without dropping items");
    const auto repairedSource = repaired.Find("source")->items.front();
    const auto repairedTarget = repaired.Find("target")->items.front();
    const bool migrated = repairedSource.id != repairedTarget.id &&
        repairedSource.reference != repairedTarget.reference &&
        repairedSource.target == "app:calendar" && repairedTarget.target == "app:calendar";
    repaired.Export(storage, "instance");
    LogicalSlotModel reopened;
    Check(reopened.Configure(TransferDeclarations(), error) && reopened.Restore(storage, "instance", error) &&
        reopened.Find("source")->items == repaired.Find("source")->items &&
        reopened.Find("target")->items == repaired.Find("target")->items,
        "repaired identities persist and remain stable on a second load");
    Check(distinct && migrated,
        "IDs and references are unique across the instance after transfer/rebind and legacy recovery");

    auto corrupt = storage;
    auto& changedSource = corrupt.at(LogicalSlotModel::StorageKey("instance", "source"));
    const auto title = changedSource.find("Calendar");
    Check(title != std::string::npos, "restoration rollback fixture has a known title");
    changedSource.replace(title, 8, "Changed before failure");
    corrupt[LogicalSlotModel::StorageKey("instance", "target")] = "{";
    Check(!reopened.Restore(corrupt, "instance", error) &&
        reopened.Find("source")->items == repaired.Find("source")->items &&
        reopened.Find("target")->items == repaired.Find("target")->items,
        "a later corrupt slot cannot partially publish an earlier restored slot");
}

void TestBoundedUndoRedoHistory()
{
    LogicalSlotModel model;
    std::string error;
    Check(model.Configure(ParseDeclarations(), error),
        "history fixture must configure");
    LogicalSlotHistory history;
    LogicalSlotChange change;
    auto previous = model;
    Check(model.Bind("primaryApp",
            Item("app.reference", "Calendar", "app:calendar"),
            change, error),
        "history fixture mutation must succeed");
    history.Record(std::move(previous), change);
    Check(history.CanUndo() && !history.CanRedo(),
        "a committed slot mutation must open one undo branch");

    Check(history.Undo(model, change, error) &&
            change.operation == "undone" &&
            model.Find("primaryApp")->items.empty() &&
            history.CanRedo(),
        "undo must atomically restore the exact prior slot model");
    Check(history.Redo(model, change, error) &&
            change.operation == "redone" &&
            model.Find("primaryApp")->items.size() == 1,
        "redo must restore the forward slot model and metadata");

    Check(history.Undo(model, change, error) && history.CanRedo(),
        "a new branch fixture must start with a pending redo transaction");
    previous = model;
    Check(model.Bind("primaryApp",
            Item("app.reference", "Notes", "app:notes"), change, error),
        "a new branch mutation must succeed");
    history.Record(std::move(previous), change);
    Check(!history.CanRedo() && !history.Redo(model, change, error) &&
            model.Find("primaryApp")->items.front().target == "app:notes",
        "recording a new slot transaction must discard the redo branch");

    LogicalSlotHistory bounded;
    for (std::size_t index = 0; index < LogicalSlotHistory::MaximumEntries + 2; ++index)
    {
        previous = model;
        Check(model.Bind("primaryApp", Item("app.reference", "History",
                "app:history-" + std::to_string(index)), change, error),
            "each bounded-history fixture must replace the binding");
        bounded.Record(std::move(previous), change);
    }
    for (std::size_t remaining = LogicalSlotHistory::MaximumEntries;
         remaining > 0; --remaining)
    {
        Check(bounded.Undo(model, change, error) &&
                model.Find("primaryApp")->items.front().target ==
                    "app:history-" + std::to_string(remaining),
            "bounded undo must restore each retained model in reverse order");
    }
    Check(!bounded.CanUndo() && !bounded.Undo(model, change, error) &&
            model.Find("primaryApp")->items.front().target == "app:history-1",
        "the oldest overflowed history entries must be unavailable");
}

void TestClippedPointerModelTransactions()
{
    auto declarations=TransferDeclarations();
    declarations.at("source").capacity=8;
    declarations.at("target").capacity=8;
    LogicalSlotModel original;
    LogicalSlotChange change;
    std::string error;
    Check(original.Configure(declarations,error),"clipped model configures");
    for(int i=0;i<6;++i)
        Check(original.Bind("source",Item("app.reference",std::to_string(i),
            "app:item"+std::to_string(i)),change,error),"populate complete source model");
    for(int i=0;i<5;++i)
        Check(original.Bind("target",Item("app.reference","target"+std::to_string(i),
            "app:target"+std::to_string(i)),change,error),"populate complete target model");
    const RECT surface{0,0,120,120};
    const std::vector<RECT> bounds{{10,10,100,40},{10,50,100,80}};
    for (const auto sourceIndex : {0u,3u,5u})
    {
        auto model=original;
        const auto snapshot=*model.Find("source");
        const std::vector<std::string_view> visible{snapshot.items[2].id,snapshot.items[3].id};
        const auto target=ResolveLogicalSlotModelPointerTarget(bounds,visible,snapshot,
            snapshot.items[sourceIndex].id,POINT{50,75},surface);
        Check(target && model.Move("source",snapshot.items[sourceIndex].id,
            target->targetIndex,change,error),"clipped move applies to complete model");
        auto expected=snapshot.items;
        auto moved=expected[sourceIndex]; expected.erase(expected.begin()+sourceIndex);
        expected.insert(expected.begin()+(sourceIndex<4?3:4),moved);
        Check(model.Find("source")->items==expected,
            "visible insertion preserves hidden prefix/suffix and complete source identity");
        std::unordered_map<std::string,std::string> storage;
        model.Export(storage,"clipped");
        LogicalSlotModel reopened;
        Check(reopened.Configure(declarations,error) && reopened.Restore(storage,"clipped",error) &&
            reopened.Find("source")->items==expected,"clipped move persists complete order");
    }
    auto model=original;
    const auto source=*model.Find("source");
    const auto targetSnapshot=*model.Find("target");
    const std::vector<RECT> single{{10,30,100,60}};
    const std::vector<std::string_view> visible{targetSnapshot.items[2].id};
    const auto target=ResolveLogicalSlotModelPointerTarget(single,visible,targetSnapshot,
        {},POINT{50,55},surface);
    Check(target && model.Transfer("source",source.items[4].id,"target",
        target->targetIndex,change,error),"single visible target transfers at complete model insertion");
    Check(model.Find("target")->items.size()==6 &&
        model.Find("target")->items[3].target==source.items[4].target &&
        model.Find("source")->items.size()==5,"transfer respects hidden target prefix and source removal");
    const std::vector<std::string_view> stale{"removed-id"};
    Check(!ResolveLogicalSlotModelPointerTarget(single,stale,targetSnapshot,{},POINT{50,55},surface),
        "stale visible identity cannot mutate a different full-model item");
}
}

int main()
{
    TestManifestValidation();
    TestBindingAndCollectionTransactions();
    TestPersistenceRoundTrip();
    TestCrossCollectionTransfer();
    TestTransferRebindAndLegacyIdentities();
    TestBoundedUndoRedoHistory();
    TestClippedPointerModelTransactions();
    std::cout << "widget logical slot tests passed\n";
    return 0;
}
