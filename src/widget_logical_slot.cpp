#include "widget_logical_slot_manifest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::array<std::string_view, 3> kAcceptedKinds{
    "desktop.item", "app.reference", "filesystem.reference"
};

bool ValidIdentifier(std::string_view value)
{
    if (value.empty() || value.size() > 64 ||
        value.front() < 'a' || value.front() > 'z')
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    });
}

std::string EscapeJson(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8);
    static constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0x0f]);
                result.push_back(hex[ch & 0x0f]);
            }
            else result.push_back(static_cast<char>(ch));
            break;
        }
    }
    return result;
}

void AddError(std::vector<LogicalSlotManifestError>& errors,
    std::string code, std::string message)
{
    errors.push_back({ std::move(code), std::move(message) });
}

bool ReadString(const JsonValue& object, std::string_view name,
    std::string& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsString()) return false;
    output = value->string;
    return true;
}

bool ReadBoolean(const JsonValue& object, std::string_view name,
    bool& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsBoolean()) return false;
    output = value->boolean;
    return true;
}

bool ReadUnsigned(const JsonValue& object, std::string_view name,
    std::uint64_t& output)
{
    const JsonValue* value = object.Find(name);
    if (!value || !value->IsNumber() || !std::isfinite(value->number) ||
        value->number < 0.0 || std::floor(value->number) != value->number ||
        value->number > 9007199254740991.0)
        return false;
    output = static_cast<std::uint64_t>(value->number);
    return true;
}
}

const char* LogicalSlotKindName(LogicalSlotKind kind) noexcept
{
    return kind == LogicalSlotKind::Binding ? "binding" : "collection";
}

bool ParseLogicalSlotDeclarations(const JsonValue* value,
    LogicalSlotDeclarations& declarations,
    std::vector<LogicalSlotManifestError>& errors)
{
    declarations.clear();
    errors.clear();
    if (!value) return true;
    if (!value->IsObject())
    {
        AddError(errors, "manifest.slots", "slots must be an object");
        return false;
    }
    if (value->object.size() > LogicalSlotModel::MaximumSlots)
    {
        AddError(errors, "manifest.slotCount",
            "slots cannot contain more than 16 declarations");
    }

    for (const auto& [id, raw] : value->object)
    {
        if (!ValidIdentifier(id))
        {
            AddError(errors, "manifest.slotId",
                "slot ids must start with a lowercase letter and contain only letters, digits, '-' or '_': " + id);
            continue;
        }
        if (!raw.IsObject())
        {
            AddError(errors, "manifest.slot", "slot descriptors must be objects: " + id);
            continue;
        }
        static const std::set<std::string, std::less<>> fields{
            "kind", "accepts", "operation", "replacePolicy",
            "allowClear", "capacity"
        };
        for (const auto& [name, ignored] : raw.object)
        {
            (void)ignored;
            if (!fields.contains(name))
                AddError(errors, "manifest.slotField",
                    "unknown slot descriptor field '" + name + "': " + id);
        }

        LogicalSlotDeclaration declaration;
        std::string kind;
        if (!ReadString(raw, "kind", kind) ||
            (kind != "binding" && kind != "collection"))
        {
            AddError(errors, "manifest.slotKind",
                "slot kind must be binding or collection: " + id);
            continue;
        }
        declaration.kind = kind == "binding"
            ? LogicalSlotKind::Binding : LogicalSlotKind::Collection;

        const JsonValue* accepts = raw.Find("accepts");
        if (!accepts || !accepts->IsArray() || accepts->array.empty())
        {
            AddError(errors, "manifest.slotAccepts",
                "slot accepts must be a non-empty array: " + id);
        }
        else
        {
            std::set<std::string> unique;
            for (const JsonValue& accepted : accepts->array)
            {
                if (!accepted.IsString() ||
                    std::find(kAcceptedKinds.begin(), kAcceptedKinds.end(),
                        accepted.string) == kAcceptedKinds.end())
                {
                    AddError(errors, "manifest.slotAccept",
                        "slot accepts supports desktop.item, app.reference, and filesystem.reference: " + id);
                    continue;
                }
                if (!unique.insert(accepted.string).second)
                {
                    AddError(errors, "manifest.slotAcceptDuplicate",
                        "slot accepts entries must be unique: " + id);
                    continue;
                }
                declaration.accepts.push_back(accepted.string);
            }
        }

        if (const JsonValue* operation = raw.Find("operation"))
        {
            if (!operation->IsString() || operation->string != "reference")
                AddError(errors, "manifest.slotOperation",
                    "slot operation must be reference: " + id);
            else declaration.operation = operation->string;
        }

        if (declaration.kind == LogicalSlotKind::Binding)
        {
            if (const JsonValue* policy = raw.Find("replacePolicy"))
            {
                if (!policy->IsString() ||
                    (policy->string != "allow" && policy->string != "reject"))
                    AddError(errors, "manifest.slotReplacePolicy",
                        "binding replacePolicy must be allow or reject: " + id);
                else declaration.replacePolicy = policy->string;
            }
            if (const JsonValue* clear = raw.Find("allowClear"))
            {
                if (!clear->IsBoolean())
                    AddError(errors, "manifest.slotAllowClear",
                        "binding allowClear must be a boolean: " + id);
                else declaration.allowClear = clear->boolean;
            }
            if (raw.Find("capacity"))
                AddError(errors, "manifest.slotCapacity",
                    "binding slots cannot declare capacity: " + id);
            declaration.capacity = 1;
        }
        else
        {
            if (raw.Find("replacePolicy") || raw.Find("allowClear"))
                AddError(errors, "manifest.slotBindingField",
                    "replacePolicy and allowClear are reserved for binding slots: " + id);
            declaration.replacePolicy.clear();
            declaration.allowClear = false;
            declaration.capacity = LogicalSlotModel::MaximumCollectionCapacity;
            if (const JsonValue* capacity = raw.Find("capacity"))
            {
                if (!capacity->IsNumber() ||
                    !std::isfinite(capacity->number) ||
                    std::floor(capacity->number) != capacity->number ||
                    capacity->number < 1.0 ||
                    capacity->number > static_cast<double>(
                        LogicalSlotModel::MaximumCollectionCapacity))
                {
                    AddError(errors, "manifest.slotCapacity",
                        "collection capacity must be an integer from 1 to 64: " + id);
                }
                else declaration.capacity =
                    static_cast<std::size_t>(capacity->number);
            }
        }
        declarations.emplace(id, std::move(declaration));
    }
    return errors.empty();
}

std::string SerializeLogicalSlotDeclarations(
    const LogicalSlotDeclarations& declarations)
{
    std::ostringstream output;
    output << '{';
    bool firstSlot = true;
    for (const auto& [id, declaration] : declarations)
    {
        if (!firstSlot) output << ',';
        firstSlot = false;
        output << "\n    \"" << EscapeJson(id) << "\": {\"kind\": \""
            << LogicalSlotKindName(declaration.kind)
            << "\", \"accepts\": [";
        for (std::size_t index = 0; index < declaration.accepts.size(); ++index)
        {
            if (index) output << ", ";
            output << '"' << EscapeJson(declaration.accepts[index]) << '"';
        }
        output << "], \"operation\": \"reference\"";
        if (declaration.kind == LogicalSlotKind::Binding)
        {
            output << ", \"replacePolicy\": \""
                << EscapeJson(declaration.replacePolicy)
                << "\", \"allowClear\": "
                << (declaration.allowClear ? "true" : "false");
        }
        else output << ", \"capacity\": " << declaration.capacity;
        output << '}';
    }
    if (!declarations.empty()) output << '\n' << "  ";
    output << '}';
    return output.str();
}

bool LogicalSlotModel::Configure(LogicalSlotDeclarations declarations,
    std::string& error)
{
    error.clear();
    if (declarations.size() > MaximumSlots)
    {
        error = "logical slot declaration limit exceeded";
        return false;
    }
    declarations_ = std::move(declarations);
    snapshots_.clear();
    for (const auto& [id, declaration] : declarations_)
    {
        if (!ValidIdentifier(id) || declaration.accepts.empty() ||
            declaration.operation != "reference" ||
            (declaration.kind == LogicalSlotKind::Binding &&
                declaration.capacity != 1) ||
            (declaration.kind == LogicalSlotKind::Collection &&
                (declaration.capacity == 0 ||
                    declaration.capacity > MaximumCollectionCapacity)))
        {
            error = "invalid logical slot declaration: " + id;
            declarations_.clear();
            snapshots_.clear();
            return false;
        }
        snapshots_.emplace(id, LogicalSlotSnapshot{
            id, declaration.kind, 0, declaration.capacity, {} });
    }
    return true;
}

std::string LogicalSlotModel::StorageKey(std::string_view instancePrefix,
    std::string_view slotId)
{
    if (instancePrefix.empty() || slotId.empty()) return {};
    std::string result(instancePrefix);
    result += ".__host.logicalSlot.";
    result += slotId;
    return result;
}

bool LogicalSlotModel::Restore(
    const std::unordered_map<std::string, std::string>& storage,
    std::string_view instancePrefix, std::string& error)
{
    error.clear();
    for (auto& [id, snapshot] : snapshots_)
    {
        const auto value = storage.find(StorageKey(instancePrefix, id));
        if (value == storage.end()) continue;
        LogicalSlotSnapshot restored;
        std::string parseError;
        if (!ParseSnapshot(value->second, id, declarations_.at(id),
                restored, parseError))
        {
            error = "cannot restore logical slot '" + id + "': " +
                parseError;
            return false;
        }
        snapshot = std::move(restored);
    }
    return true;
}

void LogicalSlotModel::Export(
    std::unordered_map<std::string, std::string>& storage,
    std::string_view instancePrefix) const
{
    const std::string keyPrefix = std::string(instancePrefix) +
        ".__host.logicalSlot.";
    std::erase_if(storage, [&keyPrefix](const auto& entry) {
        return entry.first.starts_with(keyPrefix);
    });
    for (const auto& [id, snapshot] : snapshots_)
        storage[StorageKey(instancePrefix, id)] = SerializeSnapshot(snapshot);
}

const LogicalSlotSnapshot* LogicalSlotModel::Find(
    std::string_view slotId) const noexcept
{
    const auto found = snapshots_.find(slotId);
    return found == snapshots_.end() ? nullptr : &found->second;
}

std::vector<LogicalSlotSnapshot> LogicalSlotModel::Snapshots() const
{
    std::vector<LogicalSlotSnapshot> result;
    result.reserve(snapshots_.size());
    for (const auto& [id, snapshot] : snapshots_)
    {
        (void)id;
        result.push_back(snapshot);
    }
    return result;
}

bool LogicalSlotModel::Accepts(
    const LogicalSlotDeclaration& declaration,
    std::string_view kind) const noexcept
{
    return std::find(declaration.accepts.begin(), declaration.accepts.end(),
        kind) != declaration.accepts.end();
}

std::string LogicalSlotModel::MakeOpaqueToken(
    std::string_view prefix, std::string_view seed)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : seed)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream output;
    output << prefix << std::hex << std::setw(16) << std::setfill('0')
        << hash;
    return output.str();
}

bool LogicalSlotModel::ValidateItem(const LogicalSlotItem& item,
    std::string& error)
{
    if (item.kind.empty() || item.kind.size() > 64 ||
        item.title.empty() || item.title.size() > 4096 ||
        item.source.size() > 128 || item.type.size() > 128 ||
        item.target.empty() || item.target.size() > 32768)
    {
        error = "logical slot item metadata is missing or exceeds its limit";
        return false;
    }
    return true;
}

bool LogicalSlotModel::Bind(std::string_view slotId,
    LogicalSlotItem candidate, LogicalSlotChange& change,
    std::string& error, std::optional<std::size_t> targetIndex)
{
    error.clear();
    change = {};
    const auto declaration = declarations_.find(slotId);
    auto snapshot = snapshots_.find(slotId);
    if (declaration == declarations_.end() || snapshot == snapshots_.end())
    {
        error = "logical slot is not declared";
        return false;
    }
    if (!ValidateItem(candidate, error)) return false;
    if (!Accepts(declaration->second, candidate.kind))
    {
        error = "logical slot does not accept this reference kind";
        return false;
    }
    const auto duplicate = std::find_if(snapshot->second.items.begin(),
        snapshot->second.items.end(), [&candidate](const auto& item) {
            return item.kind == candidate.kind &&
                item.target == candidate.target;
        });
    if (duplicate != snapshot->second.items.end())
    {
        change = { snapshot->second.id, snapshot->second.kind,
            snapshot->second.revision, "unchanged", { duplicate->id } };
        return true;
    }
    if (declaration->second.kind == LogicalSlotKind::Collection &&
        snapshot->second.items.size() >= declaration->second.capacity)
    {
        error = "logical slot capacity exceeded";
        return false;
    }
    if (declaration->second.kind == LogicalSlotKind::Binding &&
        !snapshot->second.items.empty() &&
        declaration->second.replacePolicy == "reject")
    {
        error = "logical slot replacement is disabled";
        return false;
    }

    const std::string seed = snapshot->second.id + "\n" + candidate.kind +
        "\n" + candidate.target;
    candidate.id = MakeOpaqueToken("lsi-", seed);
    candidate.reference = MakeOpaqueToken("lsr-", seed);
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> references;
    for (const auto& item : snapshot->second.items)
    {
        ids.insert(item.id);
        references.insert(item.reference);
    }
    for (std::size_t suffix = 1;
        ids.contains(candidate.id) || references.contains(candidate.reference);
        ++suffix)
    {
        const std::string uniqueSeed = seed + "\n" + std::to_string(suffix);
        candidate.id = MakeOpaqueToken("lsi-", uniqueSeed);
        candidate.reference = MakeOpaqueToken("lsr-", uniqueSeed);
    }

    std::string operation = "added";
    if (declaration->second.kind == LogicalSlotKind::Binding)
    {
        operation = snapshot->second.items.empty() ? "bound" : "replaced";
        snapshot->second.items.assign(1, candidate);
    }
    else
    {
        const std::size_t insertAt = std::min(
            targetIndex.value_or(snapshot->second.items.size()),
            snapshot->second.items.size());
        snapshot->second.items.insert(
            snapshot->second.items.begin() +
                static_cast<std::ptrdiff_t>(insertAt),
            candidate);
    }
    ++snapshot->second.revision;
    if (snapshot->second.revision == 0) ++snapshot->second.revision;
    change = { snapshot->second.id, snapshot->second.kind,
        snapshot->second.revision, operation, { candidate.id } };
    return true;
}

bool LogicalSlotModel::SetAvailability(std::string_view slotId,
    std::string_view itemId, bool available, LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    change = {};
    auto snapshot = snapshots_.find(slotId);
    if (snapshot == snapshots_.end())
    {
        error = "logical slot is not declared";
        return false;
    }
    const auto item = std::find_if(snapshot->second.items.begin(),
        snapshot->second.items.end(), [itemId](const auto& candidate) {
            return candidate.id == itemId;
        });
    if (item == snapshot->second.items.end())
    {
        error = "logical slot item does not exist";
        return false;
    }
    if (item->available == available)
    {
        change = { snapshot->second.id, snapshot->second.kind,
            snapshot->second.revision, "unchanged", { item->id } };
        return true;
    }
    item->available = available;
    ++snapshot->second.revision;
    if (snapshot->second.revision == 0) ++snapshot->second.revision;
    change = { snapshot->second.id, snapshot->second.kind,
        snapshot->second.revision, "availability", { item->id } };
    return true;
}

bool LogicalSlotModel::Clear(std::string_view slotId,
    LogicalSlotChange& change, std::string& error)
{
    error.clear();
    change = {};
    const auto declaration = declarations_.find(slotId);
    auto snapshot = snapshots_.find(slotId);
    if (declaration == declarations_.end() || snapshot == snapshots_.end())
    {
        error = "logical slot is not declared";
        return false;
    }
    if (declaration->second.kind != LogicalSlotKind::Binding)
    {
        error = "clear is reserved for binding slots";
        return false;
    }
    if (!declaration->second.allowClear)
    {
        error = "logical slot clearing is disabled";
        return false;
    }
    if (snapshot->second.items.empty())
    {
        change = { snapshot->second.id, snapshot->second.kind,
            snapshot->second.revision, "unchanged", {} };
        return true;
    }
    const std::string removed = snapshot->second.items.front().id;
    snapshot->second.items.clear();
    ++snapshot->second.revision;
    if (snapshot->second.revision == 0) ++snapshot->second.revision;
    change = { snapshot->second.id, snapshot->second.kind,
        snapshot->second.revision, "cleared", { removed } };
    return true;
}

bool LogicalSlotModel::Remove(std::string_view slotId,
    std::string_view itemId, LogicalSlotChange& change,
    std::string& error)
{
    error.clear();
    change = {};
    const auto declaration = declarations_.find(slotId);
    auto snapshot = snapshots_.find(slotId);
    if (declaration == declarations_.end() || snapshot == snapshots_.end())
    {
        error = "logical slot is not declared";
        return false;
    }
    if (declaration->second.kind != LogicalSlotKind::Collection)
    {
        error = "remove is reserved for collection slots";
        return false;
    }
    const auto item = std::find_if(snapshot->second.items.begin(),
        snapshot->second.items.end(), [itemId](const auto& candidate) {
            return candidate.id == itemId;
        });
    if (item == snapshot->second.items.end())
    {
        error = "logical slot item does not exist";
        return false;
    }
    const std::string removed = item->id;
    snapshot->second.items.erase(item);
    ++snapshot->second.revision;
    if (snapshot->second.revision == 0) ++snapshot->second.revision;
    change = { snapshot->second.id, snapshot->second.kind,
        snapshot->second.revision, "removed", { removed } };
    return true;
}

bool LogicalSlotModel::Move(std::string_view slotId,
    std::string_view itemId, std::size_t targetIndex,
    LogicalSlotChange& change, std::string& error)
{
    error.clear();
    change = {};
    const auto declaration = declarations_.find(slotId);
    auto snapshot = snapshots_.find(slotId);
    if (declaration == declarations_.end() || snapshot == snapshots_.end())
    {
        error = "logical slot is not declared";
        return false;
    }
    if (declaration->second.kind != LogicalSlotKind::Collection)
    {
        error = "move is reserved for collection slots";
        return false;
    }
    if (targetIndex >= snapshot->second.items.size())
    {
        error = "logical slot target index is out of range";
        return false;
    }
    const auto item = std::find_if(snapshot->second.items.begin(),
        snapshot->second.items.end(), [itemId](const auto& candidate) {
            return candidate.id == itemId;
        });
    if (item == snapshot->second.items.end())
    {
        error = "logical slot item does not exist";
        return false;
    }
    const std::size_t sourceIndex = static_cast<std::size_t>(
        std::distance(snapshot->second.items.begin(), item));
    if (sourceIndex == targetIndex)
    {
        change = { snapshot->second.id, snapshot->second.kind,
            snapshot->second.revision, "unchanged", { std::string(itemId) } };
        return true;
    }
    LogicalSlotItem moving = std::move(*item);
    snapshot->second.items.erase(snapshot->second.items.begin() +
        static_cast<std::ptrdiff_t>(sourceIndex));
    snapshot->second.items.insert(snapshot->second.items.begin() +
        static_cast<std::ptrdiff_t>(targetIndex), std::move(moving));
    ++snapshot->second.revision;
    if (snapshot->second.revision == 0) ++snapshot->second.revision;
    change = { snapshot->second.id, snapshot->second.kind,
        snapshot->second.revision, "moved", { std::string(itemId) } };
    return true;
}

std::string LogicalSlotModel::SerializeSnapshot(
    const LogicalSlotSnapshot& snapshot)
{
    std::ostringstream output;
    output << "{\"version\":1,\"revision\":" << snapshot.revision
        << ",\"items\":[";
    for (std::size_t index = 0; index < snapshot.items.size(); ++index)
    {
        if (index) output << ',';
        const auto& item = snapshot.items[index];
        output << "{\"id\":\"" << EscapeJson(item.id)
            << "\",\"reference\":\"" << EscapeJson(item.reference)
            << "\",\"kind\":\"" << EscapeJson(item.kind)
            << "\",\"title\":\"" << EscapeJson(item.title)
            << "\",\"source\":\"" << EscapeJson(item.source)
            << "\",\"type\":\"" << EscapeJson(item.type)
            << "\",\"target\":\"" << EscapeJson(item.target)
            << "\",\"available\":"
            << (item.available ? "true" : "false") << '}';
    }
    output << "]}";
    return output.str();
}

bool LogicalSlotModel::ParseSnapshot(std::string_view text,
    const std::string& slotId,
    const LogicalSlotDeclaration& declaration,
    LogicalSlotSnapshot& snapshot, std::string& error)
{
    error.clear();
    JsonValue root;
    if (text.size() > 1024 * 1024 || !ParseJson(text, root, &error) ||
        !root.IsObject())
    {
        if (error.empty()) error = "persisted slot state is not an object";
        return false;
    }
    std::uint64_t version = 0;
    std::uint64_t revision = 0;
    if (!ReadUnsigned(root, "version", version) || version != 1 ||
        !ReadUnsigned(root, "revision", revision))
    {
        error = "persisted slot version or revision is invalid";
        return false;
    }
    const JsonValue* items = root.Find("items");
    if (!items || !items->IsArray() ||
        items->array.size() > declaration.capacity ||
        (declaration.kind == LogicalSlotKind::Binding &&
            items->array.size() > 1))
    {
        error = "persisted slot item count exceeds its declaration";
        return false;
    }
    snapshot = { slotId, declaration.kind, revision,
        declaration.capacity, {} };
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> references;
    for (const JsonValue& value : items->array)
    {
        if (!value.IsObject())
        {
            error = "persisted slot item must be an object";
            return false;
        }
        LogicalSlotItem item;
        if (!ReadString(value, "id", item.id) ||
            !ReadString(value, "reference", item.reference) ||
            !ReadString(value, "kind", item.kind) ||
            !ReadString(value, "title", item.title) ||
            !ReadString(value, "source", item.source) ||
            !ReadString(value, "type", item.type) ||
            !ReadString(value, "target", item.target) ||
            !ReadBoolean(value, "available", item.available) ||
            !ValidateItem(item, error) ||
            item.id.empty() || item.id.size() > 128 ||
            item.reference.empty() || item.reference.size() > 128 ||
            !ids.insert(item.id).second ||
            !references.insert(item.reference).second ||
            std::find(declaration.accepts.begin(), declaration.accepts.end(),
                item.kind) == declaration.accepts.end())
        {
            if (error.empty())
                error = "persisted slot item metadata is invalid";
            return false;
        }
        snapshot.items.push_back(std::move(item));
    }
    return true;
}

void LogicalSlotHistory::Record(LogicalSlotModel previous,
    const LogicalSlotChange& change)
{
    if (undo_.size() >= MaximumEntries)
        undo_.erase(undo_.begin());
    undo_.push_back({ std::move(previous), change });
    redo_.clear();
}

bool LogicalSlotHistory::Restore(bool redo, LogicalSlotModel& model,
    LogicalSlotChange& change, std::string& error)
{
    auto& source = redo ? redo_ : undo_;
    auto& destination = redo ? undo_ : redo_;
    if (source.empty())
    {
        error = redo ? "nothingToRedo" : "nothingToUndo";
        return false;
    }
    Entry entry = std::move(source.back());
    source.pop_back();
    if (destination.size() >= MaximumEntries)
        destination.erase(destination.begin());
    destination.push_back({ std::move(model), entry.change });
    model = std::move(entry.model);
    change = std::move(entry.change);
    change.operation = redo ? "redone" : "undone";
    if (const auto* snapshot = model.Find(change.slotId))
        change.revision = snapshot->revision;
    error.clear();
    return true;
}

bool LogicalSlotHistory::Undo(LogicalSlotModel& model,
    LogicalSlotChange& change, std::string& error)
{
    return Restore(false, model, change, error);
}

bool LogicalSlotHistory::Redo(LogicalSlotModel& model,
    LogicalSlotChange& change, std::string& error)
{
    return Restore(true, model, change, error);
}
}
