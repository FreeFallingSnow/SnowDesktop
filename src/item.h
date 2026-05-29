#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <string>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

class Container;

class Item
{
public:
    virtual ~Item() = default;
    virtual std::wstring GetTitle() const = 0;
    virtual std::wstring GetPath() const = 0;
    virtual HBITMAP GetIconBitmap() const = 0;
    virtual RECT GetBounds() const = 0;
    virtual void SetBounds(RECT bounds) = 0;
    virtual bool IsSelected() const = 0;
    virtual void SetSelected(bool selected) = 0;
    virtual Container* GetContainer() const = 0;
    virtual void Draw(ID2D1DeviceContext* context, RECT rect, int state) = 0;
    // state: 0=normal, 1=hovered, 2=selected, 3=dragged
    virtual ComPtr<IDataObject> CreateDataObject() = 0;
};

// DesktopIcon wraps an existing DesktopItem (non-owning pointer).
// Forward-declared here; full definition requires types.h.
struct DesktopItem;
struct FolderEntry;

class DesktopIcon : public Item
{
public:
    explicit DesktopIcon(DesktopItem* item, Container* container = nullptr);
    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT bounds) override;
    bool IsSelected() const override;
    void SetSelected(bool selected) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
    ComPtr<IDataObject> CreateDataObject() override;
    DesktopItem* GetDesktopItem() const { return item_; }

private:
    DesktopItem* item_;
    Container* container_;
};

class FolderEntryIcon : public Item
{
public:
    explicit FolderEntryIcon(FolderEntry* entry, Container* container = nullptr);
    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT bounds) override;
    bool IsSelected() const override;
    void SetSelected(bool selected) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
    ComPtr<IDataObject> CreateDataObject() override;
    FolderEntry* GetFolderEntry() const { return entry_; }

private:
    FolderEntry* entry_;
    Container* container_;
    RECT bounds_{};
};
