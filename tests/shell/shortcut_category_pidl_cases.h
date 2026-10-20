#pragma once

#include "desktop/desktop_category_item_rules.h"
#include "drag_drop/drop_pipeline.h"
#include "shell/shortcut_category_target.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace shortcut_category_pidl_cases
{
using Bytes = std::vector<std::uint8_t>;

inline void U32(Bytes& bytes, size_t offset, std::uint32_t value)
{
    for (unsigned index = 0; index < 4; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
}

inline void U16(Bytes& bytes, size_t offset, size_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

inline void StringProperty(Bytes& stores, const GUID& format, DWORD id,
    std::wstring_view text)
{
    // MS-PROPSTORE serialized string property, including its reserved byte,
    // typed value and four-byte-padded UTF-16 payload. No installed app needed.
    const size_t payloadSize = ((text.size() + 1) * sizeof(wchar_t) + 3) & ~size_t{3};
    const size_t valueSize = 17 + payloadSize;
    const size_t start = stores.size();
    stores.resize(start + 24 + valueSize + 4);
    U32(stores, start, static_cast<std::uint32_t>(stores.size() - start));
    U32(stores, start + 4, 0x53505331); // "1SPS"
    std::memcpy(stores.data() + start + 8, &format, sizeof(format));
    U32(stores, start + 24, static_cast<std::uint32_t>(valueSize));
    U32(stores, start + 28, id);
    U16(stores, start + 33, VT_LPWSTR);
    U32(stores, start + 37, static_cast<std::uint32_t>(text.size() + 1));
    std::memcpy(stores.data() + start + 41, text.data(), text.size() * sizeof(wchar_t));
}

inline bool WriteAppsFolderLink(const std::filesystem::path& path)
{
    // Equivalent to Issue #27's saved AppsFolder target, with synthetic app
    // metadata only. The .lnk itself has no path, arguments, AppID or icon.
    // Its target exposes a Win32 AppID without '!' and parsing names omit the
    // AppsFolder prefix. This fixture never reads the user's AppsFolder.
    Bytes stores;
    StringProperty(stores, PKEY_AppUserModel_ID.fmtid,
        PKEY_AppUserModel_ID.pid, L"SnowDesktopApp27");
    StringProperty(stores, PKEY_ItemNameDisplay.fmtid,
        PKEY_ItemNameDisplay.pid, L"Issue 27 test application");
    stores.resize(stores.size() + 4);
    Bytes child{0, 0, 0, 0, 0, 0, 'A', 'P', 'P', 'S', 0, 0, 8, 0,
        3, 0, 0, 0, 0, 0, 0, 0};
    child.insert(child.end(), stores.begin(), stores.end());
    child.resize(child.size() + 2);
    U16(child, 0, child.size());
    U16(child, 4, stores.size() + 18);
    U16(child, 10, stores.size());
    Bytes pidl{0x14, 0, 0x1f, 0x80, 0x9b, 0xd4, 0x34, 0x42,
        0x45, 0x02, 0xf3, 0x4d, 0xb7, 0x80, 0x38, 0x93, 0x94, 0x34, 0x56, 0xe1};
    pidl.insert(pidl.end(), child.begin(), child.end());
    pidl.resize(pidl.size() + 2);
    Bytes bytes(78);
    U32(bytes, 0, 76);
    const GUID shellLink = CLSID_ShellLink;
    std::memcpy(bytes.data() + 4, &shellLink, sizeof(shellLink));
    U32(bytes, 20, 0x81); // HasLinkTargetIDList | IsUnicode
    U16(bytes, 76, pidl.size());
    bytes.insert(bytes.end(), pidl.begin(), pidl.end());
    bytes.resize(bytes.size() + 4);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

inline void Run(const std::filesystem::path& directory,
    void (*check)(bool, const char*))
{
    namespace category = snowdesktop::category_collection_rules;
    const auto path = directory / L"apps-folder.lnk";
    check(WriteAppsFolderLink(path), "the synthetic AppsFolder shortcut fixture is writable");
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    const bool loaded = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Load(path.c_str(), STGM_READ));
    check(loaded, "the AppsFolder fixture loads through the real Windows Shell link reader");
    if (!loaded) return;
    wchar_t target[MAX_PATH]{};
    check(link->GetPath(target, static_cast<int>(std::size(target)), nullptr, 0) == S_FALSE && !target[0],
        "the regression input has no ordinary executable path");
    Microsoft::WRL::ComPtr<IPropertyStore> ownProperties;
    PROPVARIANT ownAppId{};
    const bool noOwnAppId = SUCCEEDED(link.As(&ownProperties)) &&
        SUCCEEDED(ownProperties->GetValue(PKEY_AppUserModel_ID, &ownAppId)) && ownAppId.vt == VT_EMPTY;
    check(noOwnAppId, "the regression input has no AppID on the shortcut itself");
    PropVariantClear(&ownAppId);
    PIDLIST_ABSOLUTE targetPidl = nullptr;
    PWSTR parsingName = nullptr;
    const bool opaqueName = SUCCEEDED(link->GetIDList(&targetPidl)) && targetPidl &&
        SUCCEEDED(SHGetNameFromIDList(targetPidl, SIGDN_DESKTOPABSOLUTEPARSING, &parsingName)) &&
        parsingName && std::wstring_view(parsingName) == L"SnowDesktopApp27";
    check(opaqueName, "the real target parsing name contains neither AppsFolder nor a packaged-app separator");
    CoTaskMemFree(parsingName);
    CoTaskMemFree(targetPidl);

    const std::vector<std::wstring> programRules{L".EXE", L".MSI", L".BAT", L".CMD", L".LNK:APP", L".URL:STEAM"};
    for (const auto& classified : {category::ReadShellLinkTarget(link.Get()),
            category::ReadShortcutTarget(path.wstring())})
    {
        check(classified.classified && classified.application && !classified.directory,
            "Issue 27: both shared entry points classify an opaque AppsFolder shortcut as a program");
        check(category::MatchesExtensionRule(L".LNK:APP", L".LNK", classified),
            "Issue 27: the opaque AppsFolder shortcut matches the default Programs category");
        DesktopItem item;
        item.layoutKey = L"ISSUE-27";
        item.parsingName = path.wstring();
        item.shortcutTarget = classified;
        check(!category::IsCollectableDesktopItem(item, false, false, L".LNK", programRules) &&
            category::IsCollectableDesktopItem(item, true, false, L".LNK", programRules),
            "Issue 27: AppsFolder shortcuts respect the program collection opt-in");
    }
    DragSourceList sources;
    sources.entries.emplace_back().filePath = path.wstring();
    const auto normalize = [](std::wstring value) {
        for (auto& ch : value) ch = static_cast<wchar_t>(std::towupper(ch));
        return value;
    };
    DesktopItem item;
    check(snowdesktop::drop_pipeline::ContainsPrograms(sources, programRules,
        [&](const auto&) { return &item; }, normalize) && item.shortcutTarget.application &&
        item.isApplicationShortcut,
        "Issue 27: the production drop classifier detects and caches the program before dispatch");
    check(snowdesktop::drop_pipeline::ContainsPrograms(sources, programRules,
        [&](const auto&) { return &item; }, normalize),
        "Issue 27: a cached AppsFolder identity retains the program gate");
}
}
