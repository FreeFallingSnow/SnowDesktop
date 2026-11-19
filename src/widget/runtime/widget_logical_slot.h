#pragma once

#include "widget_logical_slot_values.h"
#include "widget/api/widget_logical_slot_declarations.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
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
    bool Transfer(std::string_view sourceSlotId,
        std::string_view itemId, std::string_view targetSlotId,
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
