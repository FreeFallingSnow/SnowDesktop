#pragma once

#include "shell_extension_diagnostics.h"
#include <windows.h>
#include <string>
#include <utility>

namespace snowdesktop::shell_extensions::detail
{
// An absent optional registration needs only a watch for its next path
// component. Unrelated values below its ancestor are not menu changes.
class MenuRegistryWatch
{
public:
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool armed = false;

    MenuRegistryWatch(HKEY hive, std::wstring subkey, unsigned id)
        : root_(hive), path_(std::move(subkey)), traceId_(id) { Arm(); }
    MenuRegistryWatch(const MenuRegistryWatch&) = delete;
    MenuRegistryWatch& operator=(const MenuRegistryWatch&) = delete;
    ~MenuRegistryWatch()
    {
        if (key_ && key_ != root_) RegCloseKey(key_);
        if (event) CloseHandle(event);
    }

    bool Changed()
    {
        if (!armed) { Arm(); return false; }
        const auto wait = WaitForSingleObject(event, 0);
        if (wait == WAIT_TIMEOUT) return false;
        const bool hadTarget = target_;
        MenuTrace("registry.change", wait == WAIT_OBJECT_0 ? "signal" : "wait_failure", 0, traceId_);
        Arm();
        // Rearm deeper when an intermediate component appears, but invalidate
        // the inventory only when the actual target appeared/disappeared or
        // changed. A failed wait/rearm remains conservative.
        return wait != WAIT_OBJECT_0 || !armed || hadTarget || target_;
    }

private:
    HKEY root_ = nullptr, key_ = nullptr;
    std::wstring path_;
    unsigned traceId_ = 0;
    bool target_ = false;

    void Arm()
    {
        armed = false;
        target_ = false;
        if (key_) { if (key_ != root_) RegCloseKey(key_); key_ = nullptr; }
        if (!event) return;
        auto existing = path_;
        while (RegOpenKeyExW(root_, existing.c_str(), 0, KEY_NOTIFY, &key_) != ERROR_SUCCESS)
        {
            if (existing.empty()) return;
            const auto slash = existing.rfind(L'\\');
            existing.resize(slash == std::wstring::npos ? 0 : slash);
        }
        target_ = existing == path_;
        ResetEvent(event);
        const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC |
            (target_ ? REG_NOTIFY_CHANGE_LAST_SET : 0);
        armed = RegNotifyChangeKeyValue(key_, target_, filter, event, TRUE) == ERROR_SUCCESS;
        MenuTrace("registry.watch", !armed ? "failed" : target_ ? "target" : "ancestor", 0, traceId_);
    }
};
}
