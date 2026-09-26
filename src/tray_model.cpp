#include "tray_service.h"
#include <sstream>
#include <iomanip>

namespace snowdesktop::tray
{
std::string Key(const Identity& identity)
{
    std::ostringstream key;
    key << std::hex;
    if (HasGuid(identity.guid))
    {
        key << "guid:" << std::setfill('0') << std::setw(8) << identity.guid.Data1 << '-'
            << std::setw(4) << identity.guid.Data2 << '-' << std::setw(4) << identity.guid.Data3;
        for (const auto part : identity.guid.Data4) key << std::setw(2) << static_cast<unsigned>(part);
    }
    else key << "window:" << identity.window << ':' << identity.id << ':' << identity.process;
    return key.str();
}
bool Apply(std::vector<Icon>& icons, const Event& event)
{
    auto found = std::find_if(icons.begin(), icons.end(), [&](const auto& icon) { return SameIdentity(icon.identity, event.identity); });
    if (event.operation == NIM_DELETE)
    {
        if (found == icons.end()) return false;
        icons.erase(found); return true;
    }
    if (event.operation != NIM_ADD && event.operation != NIM_MODIFY && event.operation != NIM_SETVERSION) return false;
    if (found == icons.end())
    {
        // A version-only update cannot resurrect an icon removed in this epoch.
        if (event.operation != NIM_ADD || icons.size() >= kGeometries) return false;
        icons.emplace_back(); found = icons.end() - 1;
        found->key = Key(event.identity);
        if (HasGuid(event.identity.guid)) found->persistentKey = found->key;
    }
    found->identity = event.identity;
    if (event.operation == NIM_SETVERSION) { found->version = event.version; return true; }
    if (event.operation == NIM_ADD) found->version = 0;
    if (event.flags & NIF_MESSAGE) found->callback = event.callback;
    if (event.flags & NIF_TIP) found->tip.assign(event.tip, wcsnlen_s(event.tip, std::size(event.tip)));
    if (event.flags & NIF_STATE) found->state = (found->state & ~event.stateMask) | (event.state & event.stateMask);
    if ((event.flags & NIF_ICON) && event.width && event.height && event.width <= kIconSize && event.height <= kIconSize)
    {
        found->width = event.width; found->height = event.height;
        found->pixels.assign(event.pixels.begin(), event.pixels.begin() + event.width * event.height);
    }
    return true;
}
std::vector<Callback> Callbacks(const Icon& icon, Activation activation, POINT anchor)
{
    std::vector<Callback> result;
    const auto add = [&](UINT message) { result.push_back(MakeCallback(icon.version, icon.identity.id, message, anchor)); };
    switch (activation)
    {
    case Activation::LeftDown: add(WM_LBUTTONDOWN); break;
    case Activation::LeftUp:
        add(WM_LBUTTONUP);
        if (icon.version >= NOTIFYICON_VERSION) add(NIN_SELECT);
        break;
    case Activation::DoubleClick: add(WM_LBUTTONDBLCLK); break;
    // Match YASB's mouse compatibility sequence: several applications opt in
    // to v4 packing but still handle the raw right-button notification.
    // Keyboard invocation remains a single semantic notification.
    case Activation::RightDown: add(WM_RBUTTONDOWN); break;
    case Activation::RightUp:
        add(WM_RBUTTONUP);
        if (icon.version >= NOTIFYICON_VERSION) add(WM_CONTEXTMENU);
        break;
    case Activation::ContextKeyboard:
        if (icon.version >= NOTIFYICON_VERSION) add(WM_CONTEXTMENU);
        else { add(WM_RBUTTONDOWN); add(WM_RBUTTONUP); }
        break;
    case Activation::Keyboard:
        if (icon.version >= NOTIFYICON_VERSION) add(NIN_KEYSELECT);
        // Pre-version-3 keyboard activation uses the legacy context gesture,
        // as documented by Shell_NotifyIcon; it is not a synthetic left click.
        else { add(WM_RBUTTONDOWN); add(WM_RBUTTONUP); }
        break;
    case Activation::Hover:
        add(WM_MOUSEMOVE);
        if (icon.version >= NOTIFYICON_VERSION_4) add(NIN_POPUPOPEN);
        break;
    case Activation::Leave:
        if (icon.version >= NOTIFYICON_VERSION_4) add(NIN_POPUPCLOSE);
        break;
    }
    return result;
}
}
