#include "widget_logical_slot_manifest.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

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
            restored.Find("favorites")->revision ==
                source.Find("favorites")->revision,
        "logical slot targets and opaque references must survive restart");
    storage["instance-1.__host.logicalSlot.primaryApp"] =
        R"json({"version":1,"revision":2,"items":[{"id":"x"}]})json";
    Check(!restored.Restore(storage, "instance-1", error) &&
            error.find("primaryApp") != std::string::npos,
        "corrupt host slot state must fail closed with its slot id");
}
}

int main()
{
    TestManifestValidation();
    TestBindingAndCollectionTransactions();
    TestPersistenceRoundTrip();
    std::cout << "widget logical slot tests passed\n";
    return 0;
}
