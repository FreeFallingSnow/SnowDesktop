#pragma once

#include <cstddef>
#include <string>

inline int RenameInitialSelectionEnd(
    const std::wstring& name,
    bool isDirectory)
{
    if (isDirectory || name.empty())
        return -1;
    const std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 ||
        dot + 1 >= name.size())
        return -1;
    return static_cast<int>(dot);
}

enum class RenameTargetKind
{
    None,
    DesktopItem,
    Widget,
    FolderEntry,
    DockFolderEntry,
};

/** Keeps rename targets mutually exclusive across every application surface. */
class RenameController
{
public:
    static constexpr std::size_t InvalidIndex =
        static_cast<std::size_t>(-1);

    void BeginDesktopItem(std::size_t itemIndex)
    {
        Begin(RenameTargetKind::DesktopItem,
            itemIndex, InvalidIndex);
    }

    void BeginWidget(std::size_t widgetIndex)
    {
        Begin(RenameTargetKind::Widget,
            widgetIndex, InvalidIndex);
    }

    void BeginFolderEntry(
        std::size_t widgetIndex,
        std::size_t entryIndex)
    {
        Begin(RenameTargetKind::FolderEntry,
            entryIndex, widgetIndex);
    }

    void BeginDockFolderEntry(std::size_t entryIndex)
    {
        Begin(RenameTargetKind::DockFolderEntry,
            entryIndex, InvalidIndex);
    }

    void SetQuickNavigationPresentation(bool value)
    {
        quickNavigationPresentation_ =
            value && kind_ != RenameTargetKind::None;
    }

    void Reset()
    {
        kind_ = RenameTargetKind::None;
        index_ = InvalidIndex;
        ownerIndex_ = InvalidIndex;
        quickNavigationPresentation_ = false;
    }

    RenameTargetKind Kind() const { return kind_; }
    std::size_t Index() const { return index_; }
    std::size_t OwnerIndex() const { return ownerIndex_; }
    std::size_t SessionId() const { return sessionId_; }
    bool MatchesSession(std::size_t sessionId) const
    {
        return IsActive() && sessionId != 0 &&
            sessionId == sessionId_;
    }
    bool IsActive() const
    {
        return kind_ != RenameTargetKind::None;
    }
    bool IsDesktopItem() const
    {
        return kind_ == RenameTargetKind::DesktopItem;
    }
    bool IsWidget() const
    {
        return kind_ == RenameTargetKind::Widget;
    }
    bool IsFolderEntry() const
    {
        return kind_ == RenameTargetKind::FolderEntry ||
            kind_ == RenameTargetKind::DockFolderEntry;
    }
    bool IsDockFolderEntry() const
    {
        return kind_ == RenameTargetKind::DockFolderEntry;
    }
    bool IsQuickNavigationPresentation() const
    {
        return quickNavigationPresentation_;
    }
    bool BlocksScrolling() const
    {
        return IsActive();
    }

private:
    void Begin(
        RenameTargetKind kind,
        std::size_t index,
        std::size_t ownerIndex)
    {
        ++sessionId_;
        if (sessionId_ == 0)
            ++sessionId_;
        kind_ = kind;
        index_ = index;
        ownerIndex_ = ownerIndex;
        quickNavigationPresentation_ = false;
    }

    RenameTargetKind kind_ = RenameTargetKind::None;
    // Reset retires the target without reusing its queued focus-loss messages.
    std::size_t sessionId_ = 0;
    std::size_t index_ = InvalidIndex;
    std::size_t ownerIndex_ = InvalidIndex;
    bool quickNavigationPresentation_ = false;
};
