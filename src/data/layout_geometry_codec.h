#pragma once

#include "common/types.h"
#include "layout_storage.h"
#include <ostream>

namespace snowdesktop::layout_geometry_codec
{
template<class Decode, class Slot>
LayoutRecord RestoreItem(const layout_storage::ItemRecord& saved, Decode decode, Slot slot)
{
    LayoutRecord record;
    record.largeIcon = saved.largeIcon;
    if (saved.page && saved.column && saved.row)
    {
        record.cell = {decode(*saved.page), *saved.column, *saved.row};
        record.span = {std::max(1, saved.width), std::max(1, saved.height)};
        record.hasGrid = true;
        record.legacySlot = slot(record.cell);
    }
    return record;
}

template<class Escape>
void WriteItem(std::ostream& file, const DesktopItem& item, Escape escape)
{
    file << "    { \"key\": \"" << escape(item.layoutKey)
        << "\", \"page\": \"" << escape(item.gridCell.pageId)
        << "\", \"x\": " << item.gridCell.column << ", \"y\": " << item.gridCell.row
        << ", \"w\": " << std::max(1, item.gridSpan.columns) << ", \"h\": " << std::max(1, item.gridSpan.rows)
        << ", \"slot\": " << item.slot;
    if (item.largeIcon) file << ", \"largeIcon\": " << EncodeLargeIconConfig(*item.largeIcon);
    file << " }";
}

template<class Escape>
void WritePage(std::ostream& file, const std::wstring& id, const std::wstring& monitor,
    int columns, int rows, const std::wstring* name, Escape escape)
{
    file << "    { \"id\": \"" << escape(id) << "\", \"monitor\": \"" << escape(monitor) << "\"";
    if (name) file << ", \"name\": \"" << escape(*name) << "\"";
    file << ", \"columns\": " << std::max(1, columns) << ", \"rows\": " << std::max(1, rows) << " }";
}
}
