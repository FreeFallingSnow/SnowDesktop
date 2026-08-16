#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class LogicalSlotKind
{
    Binding,
    Collection,
};

struct LogicalSlotDeclaration
{
    LogicalSlotKind kind = LogicalSlotKind::Binding;
    std::vector<std::string> accepts;
    std::string operation = "reference";
    std::string replacePolicy = "allow";
    bool allowClear = true;
    std::size_t capacity = 1;

    bool operator==(const LogicalSlotDeclaration&) const = default;
};

using LogicalSlotDeclarations =
    std::map<std::string, LogicalSlotDeclaration, std::less<>>;

struct LogicalSlotManifestError
{
    std::string code;
    std::string message;
};

std::string SerializeLogicalSlotDeclarations(
    const LogicalSlotDeclarations& declarations);
const char* LogicalSlotKindName(LogicalSlotKind kind) noexcept;

struct LogicalSlotItem
{
    std::string id;
    std::string reference;
    std::string kind;
    std::string title;
    std::string source;
    std::string type;
    std::string target;
    bool available = true;

    bool operator==(const LogicalSlotItem&) const = default;
};

struct LogicalSlotSnapshot
{
    std::string id;
    LogicalSlotKind kind = LogicalSlotKind::Binding;
    std::uint64_t revision = 0;
    std::size_t capacity = 1;
    std::vector<LogicalSlotItem> items;
};

struct LogicalSlotChange
{
    std::string slotId;
    LogicalSlotKind kind = LogicalSlotKind::Binding;
    std::uint64_t revision = 0;
    std::string operation;
    std::vector<std::string> itemIds;
};

class LogicalSlotModel
{
public:
    static constexpr std::size_t MaximumSlots = 16;
    static constexpr std::size_t MaximumCollectionCapacity = 64;

    bool Configure(LogicalSlotDeclarations declarations,
        std::string& error);
    bool Restore(const std::unordered_map<std::string, std::string>& storage,
        std::string_view instancePrefix, std::string& error);
    void Export(std::unordered_map<std::string, std::string>& storage,
        std::string_view instancePrefix) const;

    const LogicalSlotDeclarations& Declarations() const noexcept
    {
        return declarations_;
    }
    const LogicalSlotSnapshot* Find(std::string_view slotId) const noexcept;
    std::vector<LogicalSlotSnapshot> Snapshots() const;

    bool Bind(std::string_view slotId, LogicalSlotItem candidate,
        LogicalSlotChange& change, std::string& error,
        std::optional<std::size_t> targetIndex = std::nullopt);
    bool SetAvailability(std::string_view slotId, std::string_view itemId,
        bool available, LogicalSlotChange& change, std::string& error);
    bool Clear(std::string_view slotId,
        LogicalSlotChange& change, std::string& error);
    bool Remove(std::string_view slotId, std::string_view itemId,
        LogicalSlotChange& change, std::string& error);
    bool Move(std::string_view slotId, std::string_view itemId,
        std::size_t targetIndex, LogicalSlotChange& change,
        std::string& error);

    static std::string StorageKey(std::string_view instancePrefix,
        std::string_view slotId);

private:
    bool Accepts(const LogicalSlotDeclaration& declaration,
        std::string_view kind) const noexcept;
    static std::string MakeOpaqueToken(std::string_view prefix,
        std::string_view seed);
    static bool ValidateItem(const LogicalSlotItem& item,
        std::string& error);
    static std::string SerializeSnapshot(const LogicalSlotSnapshot& snapshot);
    static bool ParseSnapshot(std::string_view text,
        const std::string& slotId,
        const LogicalSlotDeclaration& declaration,
        LogicalSlotSnapshot& snapshot, std::string& error);

    LogicalSlotDeclarations declarations_;
    std::map<std::string, LogicalSlotSnapshot, std::less<>> snapshots_;
};

class LogicalSlotHistory
{
public:
    static constexpr std::size_t MaximumEntries = 32;

    void Record(LogicalSlotModel previous,
        const LogicalSlotChange& change);
    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }
    bool Undo(LogicalSlotModel& model, LogicalSlotChange& change,
        std::string& error);
    bool Redo(LogicalSlotModel& model, LogicalSlotChange& change,
        std::string& error);
    void Clear() noexcept
    {
        undo_.clear();
        redo_.clear();
    }

private:
    struct Entry
    {
        LogicalSlotModel model;
        LogicalSlotChange change;
    };

    bool Restore(bool redo, LogicalSlotModel& model,
        LogicalSlotChange& change, std::string& error);
    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
};
}
