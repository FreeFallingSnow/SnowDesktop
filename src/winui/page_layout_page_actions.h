#pragma once

#include "layout/page_layout_settings.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace snowdesktop::winui
{

struct PageLayoutPageActions
{
    using ConfirmationCompletion = std::function<void(bool confirmed)>;

    std::function<PageLayoutSnapshot()> capture;
    std::function<PageRemovalImpact(const std::wstring& pageId)> analyzeRemoval;
    std::function<PageLayoutOperationResult(std::uint64_t expectedRevision,
        const std::wstring& pageId, const std::wstring& name)> renamePage;
    std::function<PageLayoutOperationResult(std::uint64_t expectedRevision,
        const std::wstring& pageId)> removePage;
    std::function<PageGridChangeImpact(
        const std::wstring& pageId, int columns, int rows)> analyzeGrid;
    std::function<PageLayoutOperationResult(
        std::uint64_t expectedRevision,
        const std::vector<std::wstring>& pageIds)> applyOrder;
    std::function<PageLayoutOperationResult(
        std::uint64_t expectedRevision,
        const std::wstring& pageId, int columns, int rows)> applyGrid;
    std::function<PageLayoutOperationResult(
        std::uint64_t expectedRevision)> addPage;
    std::function<void(
        std::wstring title,
        std::wstring message,
        std::wstring primaryButtonText,
        ConfirmationCompletion completed)> confirm;
};

} // namespace snowdesktop::winui
