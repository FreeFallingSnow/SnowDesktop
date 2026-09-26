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
    const bool bootstrap = event.operation == kBootstrapIcon;
    auto found = std::find_if(icons.begin(), icons.end(), [&](const auto& icon) { return SameIdentity(icon.identity, event.identity); });
    if (bootstrap && std::any_of(icons.begin(), icons.end(), [&](const auto& icon) {
        return icon.identity.window == event.identity.window && icon.identity.process == event.identity.process &&
            icon.identity.id == event.identity.id;
    })) return false; // Never overwrite authoritative GUID/version/state data.
    if (event.operation == NIM_DELETE)
    {
        if (found == icons.end() || (event.identity.window &&
            (event.identity.window != found->identity.window || event.identity.process != found->identity.process))) return false;
        icons.erase(found); return true;
    }
    if (!bootstrap && event.operation != NIM_ADD && event.operation != NIM_MODIFY && event.operation != NIM_SETVERSION) return false;
    if (event.operation == NIM_SETVERSION && event.version > NOTIFYICON_VERSION_4) return false;
    const bool image = (event.flags & NIF_ICON) && event.width && event.height &&
        event.width <= kIconSize && event.height <= kIconSize;
    const bool owner = event.identity.window && event.identity.process;
    // Classic toolbar supplementation does not expose GUIDs. Upgrade its
    // provisional HWND identity when the authoritative registration arrives.
    if (found == icons.end() && owner && HasGuid(event.identity.guid) &&
        (event.operation == NIM_ADD || event.operation == NIM_MODIFY))
        found = std::find_if(icons.begin(), icons.end(), [&](const auto& icon) {
            return !HasGuid(icon.identity.guid) && icon.identity.window == event.identity.window &&
                icon.identity.id == event.identity.id && icon.identity.process == event.identity.process;
        });
    if (found == icons.end())
    {
        // An application already running at attachment may answer the
        // re-registration request with a complete MODIFY. Partial changes and
        // version-only packets cannot manufacture a new actionable icon.
        if (!owner || icons.size() >= kGeometries || (!bootstrap && event.operation != NIM_ADD &&
            !(event.operation == NIM_MODIFY && image && (event.flags & NIF_MESSAGE) && event.callback))) return false;
        icons.emplace_back(); found = icons.end() - 1;
        found->identity = event.identity;
    }
    else if (owner && (found->identity.window != event.identity.window || found->identity.process != event.identity.process))
    {
        // Only registration may rebind a GUID to another owner. Do not inherit
        // a previous process's image, callback, version or resolved path.
        if (event.operation != NIM_ADD) return false;
        *found = Icon{};
        found->identity = event.identity;
    }
    // GUID-only MODIFY/SETVERSION deliberately omit HWND and uID. Preserve
    // the live owner's callback destination instead of erasing its identity.
    if (owner) found->identity = event.identity;
    found->key = Key(found->identity);
    if (HasGuid(found->identity.guid)) found->persistentKey = found->key;
    if (event.operation == NIM_SETVERSION)
    {
        found->version = event.version; return true;
    }
    // A duplicate ADD is observed even when Explorer rejects it because the
    // icon already exists. It must not silently downgrade a v4 registration.
    if (event.flags & NIF_MESSAGE) found->callback = event.callback;
    if (event.flags & NIF_TIP) found->tip.assign(event.tip, wcsnlen_s(event.tip, std::size(event.tip)));
    if (event.flags & NIF_STATE) found->state = (found->state & ~event.stateMask) | (event.state & event.stateMask);
    if (image)
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
