#include "widget.h"
#include "types.h"
#include "constants.h"

// ── Widget base (Item only) ─────────────────────────────────

Widget::Widget(DesktopWidget* data, DesktopApp* app)
    : data_(data), app_(app) {}

std::wstring Widget::GetTitle() const { return data_ ? data_->title : L""; }
std::wstring Widget::GetPath() const { return L""; }
HBITMAP Widget::GetIconBitmap() const { return nullptr; }
RECT Widget::GetBounds() const { return data_ ? data_->bounds : RECT{}; }
void Widget::SetBounds(RECT bounds) { if (data_) data_->bounds = bounds; }
bool Widget::IsSelected() const { return data_ && data_->selected; }
void Widget::SetSelected(bool selected) { if (data_) data_->selected = selected; }
Container* Widget::GetContainer() const { return nullptr; }

void Widget::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    (void)context;
    (void)rect;
    (void)state;
}

ComPtr<IDataObject> Widget::CreateDataObject()
{
    return nullptr;
}

// ── Factory ─────────────────────────────────────────────────

std::unique_ptr<Widget> CreateWidget(DesktopWidget* data, DesktopApp* app)
{
    if (!data) return nullptr;
    switch (data->type)
    {
    case DesktopWidgetType::Collection:
        return std::make_unique<Collection>(data, app);
    case DesktopWidgetType::FileCategories:
        return std::make_unique<FileCategories>(data, app);
    case DesktopWidgetType::FolderMapping:
        return std::make_unique<FolderMapping>(data, app);
    case DesktopWidgetType::LuaScript:
        return std::make_unique<LuaScript>(data, app);
    }
    return nullptr;
}
