#pragma once
#include "shell/shell_extension_menu.h"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace shell_icon_stage_probe
{
namespace ext = snowdesktop::shell_extensions;
inline void Report(const char* phase, const ext::Reply& reply)
{
    std::cout << "icon_lane phase=" << phase << " entries=" << reply.entries.size() << std::endl;
    for (const auto& entry : reply.entries)
    {
        if (entry.separator || (entry.label != L"7-Zip" && entry.label != L"WinRAR" &&
            std::string_view(phase) != "source-winrar")) continue;
        int left = entry.width, top = entry.height, right = 0, bottom = 0;
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto byte : entry.pixels) { hash ^= byte; hash *= 1099511628211ull; }
        if (entry.pixels.size() == static_cast<size_t>(entry.width * entry.height * 4))
            for (int y = 0; y < entry.height; ++y)
                for (int x = 0; x < entry.width; ++x)
                    if (entry.pixels[(static_cast<size_t>(y) * entry.width + x) * 4 + 3] > 127)
                    {
                        left = std::min(left, x); top = std::min(top, y);
                        right = std::max(right, x + 1); bottom = std::max(bottom, y + 1);
                    }
        std::cout << "icon_stage phase=" << phase << " title=" <<
            (entry.label == L"7-Zip" ? "7-Zip" : entry.label == L"WinRAR" ? "WinRAR" : "other-WinRAR-command")
            << " provider=" << entry.provider << " key=" << entry.key << " registration=" << entry.registration
            << " width=" << entry.width << " height=" << entry.height
            << " alpha_bounds=" << left << ',' << top << ',' << right << ',' << bottom
            << " pixels_hash=" << hash << " children=" << entry.children.size() << std::endl;
    }
}
// Manual installed-provider observation only. Never opens a popup or invokes
// an extension command; all three lanes use the same private selected file.
inline void Run(const std::filesystem::path& selected)
{
    struct Lane { const char* name; const wchar_t* clsid; };
    const Lane lanes[] = {
        {"aggregate", L""},
        {"source-sevenzip", L"{23170f69-40c1-278a-1000-000100020000}"},
        {"source-winrar", L"{B41DB860-64E4-11D2-9906-E49FADC173CA}"},
    };
    for (const auto& lane : lanes)
    {
        ext::Request request; request.paths = {selected.wstring()}; request.context = ext::Context::File;
        request.visibleMenuProviders = std::vector<std::string>{"*"};
        request.sourceClsid = lane.clsid;
        if (*lane.clsid) request.sourceKey = L"*";
        ext::Session session(request, 8000, false);
        std::optional<ext::Reply> complete;
        const auto deadline = GetTickCount64() + 10000;
        while (!complete && GetTickCount64() < deadline)
        {
            MSG event{};
            while (PeekMessageW(&event, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&event); DispatchMessageW(&event); }
            if (auto roots = session.PollRoots()) Report("aggregate-preview", *roots);
            complete = session.Poll();
            if (!complete) MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        if (auto roots = session.PollRoots()) Report("aggregate-preview", *roots);
        if (!complete || !complete->ok) throw std::runtime_error("installed icon stage lane did not complete");
        Report(lane.name, *complete);
    }
}
}
