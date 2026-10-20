#pragma once

#include "common/types.h"
#include "dock/dock_folder_rules.h"
#include "dock/dock_refresh_cache.h"

#include <array>
#include <string>
#include <vector>

namespace snowdesktop::tests::dock_folder_order
{
using Cache = dock_refresh_cache::Cache<bool>;

inline std::vector<DockEntry> Entries(const std::wstring& names)
{
    std::vector<DockEntry> entries;
    for (const wchar_t name : names)
    {
        DockEntry entry;
        entry.reference = std::wstring(1, name);
        if (name == L'M') entry.type = DockEntryType::FolderMapping;
        entries.push_back(std::move(entry));
    }
    return entries;
}

inline std::wstring Order(const std::vector<DockEntry>& entries)
{
    std::wstring result;
    for (const auto& entry : entries) result += entry.reference;
    return result;
}

inline std::wstring SavedOrder(const std::vector<DockEntry>& entries)
{
    std::wstring result;
    for (const auto* entry : dock_folder_rules::EntriesInUserOrder(entries))
        result += entry->reference;
    return result;
}

inline void Normalize(std::vector<DockEntry>& entries, Cache& cache,
    bool negativeControl = false)
{
    const auto classify = [&](const DockEntry& entry) {
        if (entry.reference == L"R") return dock_folder_rules::EntryGroup::Recycle;
        const bool folder = entry.type == DockEntryType::FolderMapping ||
            cache.Read(entry.reference).value.value_or(false);
        return folder ? dock_folder_rules::EntryGroup::Folder : dock_folder_rules::EntryGroup::Main;
    };
    if (negativeControl)
        dock_folder_rules::StableNormalize(entries, classify);
    else
        dock_folder_rules::NormalizeKeepingUserOrder(entries, classify);
}

inline bool Complete(Cache& cache, const wchar_t name, bool folder = true)
{
    const std::wstring key(1, name);
    const auto pending = cache.Read(key);
    return cache.Publish(key, pending.ticket, folder);
}

template <typename Check>
void Run(Check check, bool negativeControl = false)
{
    std::array<wchar_t, 3> completionOrder{L'A', L'B', L'C'};
    do
    {
        auto entries = Entries(L"ABC");
        Cache cold;
        Normalize(entries, cold, negativeControl);
        for (const auto name : completionOrder)
        {
            check(Complete(cold, name), "each initial folder result must publish");
            Normalize(entries, cold, negativeControl);
            check(SavedOrder(entries) == L"ABC",
                "saving during partial cold-start classification must preserve the user's array");
        }
        check(Order(entries) == L"ABC",
            "every cold-start completion permutation must preserve folder order");
        cold.Invalidate();
        Normalize(entries, cold, negativeControl);
        check(Order(entries) == L"ABC", "metadata invalidation must retain folder order");

        Cache restarted;
        auto restored = Entries(SavedOrder(entries));
        Normalize(restored, restarted, negativeControl);
        for (auto it = completionOrder.rbegin(); it != completionOrder.rend(); ++it)
        {
            check(Complete(restarted, *it), "a new process must accept new folder results");
            Normalize(restored, restarted, negativeControl);
        }
        check(Order(restored) == L"ABC", "a second cold restart must preserve the original order");
    } while (std::next_permutation(completionOrder.begin(), completionOrder.end()));

    for (const auto sequence : {L"AB", L"BA"})
    {
        auto mixed = Entries(L"PAMBR");
        Cache cache;
        Normalize(mixed, cache, negativeControl);
        for (const wchar_t* name = sequence; *name; ++name)
        {
            check(Complete(cache, *name), "mixed folder completion must publish");
            Normalize(mixed, cache, negativeControl);
            check(SavedOrder(mixed) == L"PAMBR",
                "pending ordinary folders must not reorder a mapped folder in the saved array");
        }
        check(Order(mixed) == L"PAMBR",
            "ordinary and mapped folders retain their relative order and section boundaries");
    }

    // Explicit edits establish a new order. Later background callbacks must
    // follow it, including edits while some folders are still unclassified.
    auto edited = Entries(L"ABC");
    Cache cache;
    Normalize(edited, cache);
    for (const wchar_t name : std::wstring(L"ABC")) check(Complete(cache, name), "initial edit fixture must resolve");
    Normalize(edited, cache);
    std::rotate(edited.begin(), edited.begin() + 2, edited.end());
    dock_folder_rules::CaptureUserOrder(edited);
    cache.Invalidate();
    Normalize(edited, cache);
    check(Order(edited) == L"CAB" && SavedOrder(edited) == L"CAB",
        "a deliberate Dock reorder must replace the saved user order");
    std::erase_if(edited, [](const DockEntry& entry) { return entry.reference == L"A"; });
    auto addition = Entries(L"D");
    edited.insert(edited.begin() + 1, addition.front());
    Normalize(edited, cache);
    check(Complete(cache, L'D'), "a new folder must resolve");
    Normalize(edited, cache);
    check(Order(edited) == L"CDB" && SavedOrder(edited) == L"CDB",
        "removal and insertion must preserve existing order and the requested boundary");

    auto pending = Entries(L"AMB");
    Cache partial;
    Normalize(pending, partial);
    auto inserted = Entries(L"D");
    pending.push_back(inserted.front());
    Normalize(pending, partial);
    check(SavedOrder(pending) == L"AMBD",
        "inserting before classification completes must not recapture temporary grouping");
    for (const wchar_t name : std::wstring(L"ABD")) check(Complete(partial, name), "partial edit fixture must resolve");
    Normalize(pending, partial);
    check(Order(pending) == L"AMBD", "new and old folders must converge on the edited user order");

    // Rename keeps the rank on the entry, rather than using its path as an
    // ordering key. Stale results cannot alter classification or the rank.
    edited[1].reference = L"E";
    const auto stale = cache.Read(L"E");
    cache.Invalidate();
    check(!cache.Publish(L"E", stale.ticket, true), "an invalidated folder result must be rejected");
    Normalize(edited, cache);
    check(SavedOrder(edited) == L"CEB", "rename while classification is pending must retain position");
    check(Complete(cache, L'E'), "a renamed folder must accept its current result");
    Normalize(edited, cache);
    check(Order(edited) == L"CEB", "the renamed folder must return to its original relative position");
    check(Complete(cache, L'E', false), "a retargeted shortcut must accept a non-folder result");
    Normalize(edited, cache);
    check(Order(edited) == L"ECB" && SavedOrder(edited) == L"CEB",
        "retargeting changes sections without destroying the user's persistent order");
    check(Complete(cache, L'E'), "a retargeted shortcut may become a folder again");
    Normalize(edited, cache);
    check(Order(edited) == L"CEB", "a folder returning to its section must recover its relative position");
}
} // namespace snowdesktop::tests::dock_folder_order
