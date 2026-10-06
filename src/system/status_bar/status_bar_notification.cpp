#include "status_bar_notification.h"
#include <windows.h>
#include <mutex>

// WNF state identifiers and the count payload reference YASB, MIT:
// d6d1e6d553b0aac34fd5fb34928d3ca82b8d055f,
// src/core/widgets/services/dnd/dnd_api.py and
// src/core/widgets/services/notifications/windows_notification.py.
// See third_party/yasb/README.md and LICENSE for source and compatibility notes.
namespace snowdesktop::status_bar_notification
{
namespace
{
constexpr std::uint64_t kActiveQuietProfile = 0x0d83063ea3bf1c75;
constexpr std::uint64_t kUnreadNotifications = 0x0d83063ea3bc1035;
constexpr std::uint64_t kTotalNotifications = 0x0d83063ea3b8d035;

using QueryState = LONG(NTAPI*)(const std::uint64_t*, const GUID*, const void*, ULONG*, void*, ULONG*);
using QueryVersion = LONG(NTAPI*)(OSVERSIONINFOW*);

struct Reader
{
    // ntdll is process-resident. Borrow it once, with no LoadLibrary reference
    // to release and no static/dynamic native-library link dependency.
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    QueryState query = module ? reinterpret_cast<QueryState>(GetProcAddress(module, "NtQueryWnfStateData")) : nullptr;
    std::uint32_t major = 0, build = 0;

    Reader()
    {
        const auto version = module ? reinterpret_cast<QueryVersion>(GetProcAddress(module, "RtlGetVersion")) : nullptr;
        OSVERSIONINFOW info{}; info.dwOSVersionInfoSize = sizeof(info);
        if (version && version(&info) == 0)
        { major = info.dwMajorVersion; build = info.dwBuildNumber; }
    }
    detail::WordReply Word(std::uint64_t name) const
    {
        detail::WordReply result;
        if (!query) return result;
        ULONG stamp = 0, bytes = sizeof(result.value);
        result.status = query(&name, nullptr, nullptr, &stamp, &result.value, &bytes);
        result.bytes = bytes;
        return result;
    }
    Snapshot Read() const
    {
        if (!query || major != 10 || build < 10240) return {};
        // Do not fall back from total to unread across Windows families. A
        // removed/changed private state must remain unknown on that system.
        return detail::Decode(major, build, Word(kActiveQuietProfile),
            Word(build >= 22000 ? kTotalNotifications : kUnreadNotifications));
    }
};
struct SharedSampler
{
    std::mutex mutex;
    detail::Cache cache;
    Reader reader;
};
}

Snapshot Current()
{
    static SharedSampler sampler;
    std::lock_guard lock(sampler.mutex);
    return sampler.cache.Get(GetTickCount64(), [&] { return sampler.reader.Read(); });
}
}
