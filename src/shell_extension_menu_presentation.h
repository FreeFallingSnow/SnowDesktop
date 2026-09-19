#pragma once
#include "modern_menu.h"
#include "shell_extension_menu.h"
#include <set>

namespace snowdesktop::shell_extensions
{
// Caller owns this bridge for the entire synchronous custom menu.
class Presentation
{
  public:
    static constexpr UINT FirstCommand = 0x71000000;
    static constexpr UINT LoadingCommand = FirstCommand - 1;
    Presentation(const Request &source, Preferences prefs, std::wstring heading, std::wstring loading,
                 std::wstring failed, std::wstring fallback)
        : prefs_(std::move(prefs)), heading_(std::move(heading)), failed_(std::move(failed)),
          fallback_(std::move(fallback))
    {
        if (!prefs_.enabled || source.paths.empty())
            return;
        auto request = source;
        std::set<std::string> providers;
        for (const auto &s : prefs_.selections)
            if (s.placement != Placement::Hidden)
                providers.insert(s.provider);
        if (providers.empty())
            return;
        request.providers.assign(providers.begin(), providers.end());
        try
        {
            session_ = std::make_unique<Session>(request);
        }
        catch (...)
        {
            failedStart_ = true;
        }
        loading_ = std::move(loading);
    }
    ~Presentation()
    {
        for (auto image : images_)
            DeleteObject(image);
    }
    void Attach(std::vector<modern_menu::Item> &items, modern_menu::Options &options)
    {
        if (!session_ && !failedStart_)
            return;
        base_ = items;
        modern_menu::Item loading;
        loading.command = LoadingCommand;
        loading.enabled = false;
        loading.label = failedStart_ ? failed_ : loading_;
        items.push_back(loading);
        options.pollItems = [this](const std::vector<modern_menu::Item> &current)
            -> std::optional<std::vector<modern_menu::Item>> {
            if (!session_)
                return {};
            auto reply = session_->Poll();
            if (!reply)
                return {};
            auto result = current;
            std::erase_if(result, [](const auto &item) { return item.command == LoadingCommand; });
            if (!reply->ok)
            {
                modern_menu::Item failed;
                failed.label = failed_;
                failed.enabled = false;
                result.push_back(std::move(failed));
                return result;
            }
            auto root = Convert(SelectEntries(prefs_, reply->entries, Placement::Root));
            result.insert(result.end(), root.begin(), root.end());
            auto nested = Convert(SelectEntries(prefs_, reply->entries, Placement::Submenu));
            if (!nested.empty())
            {
                modern_menu::Item group;
                group.label = heading_;
                group.children = std::move(nested);
                result.push_back(std::move(group));
            }
            return result;
        };
    }
    bool Invoke(UINT command, POINT point)
    {
        if (command <= FirstCommand || command >= FirstCommand + 65536 || !session_)
            return false;
        try
        {
            session_->Invoke(command - FirstCommand, point);
        }
        catch (...)
        {
            return false;
        }
        return true;
    }

  private:
    std::vector<modern_menu::Item> Convert(const std::vector<Entry> &entries)
    {
        std::vector<modern_menu::Item> result;
        for (const auto &e : entries)
        {
            modern_menu::Item item;
            item.command = e.token ? FirstCommand + e.token : 0;
            item.label = e.label;
            item.enabled = e.enabled;
            item.checked = e.checked;
            item.separator = e.separator;
            item.children = Convert(e.children);
            if (e.native && e.children.empty())
                item.label += L" (" + fallback_ + L")";
            if (e.width > 0 && e.height > 0 && e.width <= 128 && e.height <= 128 &&
                e.pixels.size() == static_cast<size_t>(e.width * e.height * 4))
            {
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = e.width;
                info.bmiHeader.biHeight = -e.height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void *pixels = nullptr;
                HBITMAP image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
                if (image && pixels)
                {
                    memcpy(pixels, e.pixels.data(), e.pixels.size());
                    images_.push_back(image);
                    item.image = image;
                }
                else if (image)
                    DeleteObject(image);
            }
            result.push_back(std::move(item));
        }
        return result;
    }
    Preferences prefs_;
    std::wstring heading_, loading_, failed_, fallback_;
    std::unique_ptr<Session> session_;
    std::vector<modern_menu::Item> base_;
    std::vector<HBITMAP> images_;
    bool failedStart_ = false;
};
} // namespace snowdesktop::shell_extensions
