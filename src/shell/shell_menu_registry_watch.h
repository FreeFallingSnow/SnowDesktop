#pragma once

#include "shell_extension_diagnostics.h"
#include <windows.h>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::shell_extensions::detail
{
// An absent optional registration needs only a watch for its next path
// component. Unrelated values below its ancestor are not menu changes.
class MenuRegistryWatch
{
public:
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool armed = false;

    MenuRegistryWatch(HKEY hive, std::wstring subkey, unsigned id, bool filterClasses = false)
        : root_(hive), path_(std::move(subkey)), traceId_(id), filterClasses_(filterClasses) { Arm(); }
    MenuRegistryWatch(const MenuRegistryWatch&) = delete;
    MenuRegistryWatch& operator=(const MenuRegistryWatch&) = delete;
    ~MenuRegistryWatch()
    {
        CloseKeys();
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
    bool filterClasses_ = false;
    std::vector<HKEY> branches_;

    void CloseKeys()
    {
        // Closing outstanding subscriptions cancels them before the shared
        // event is reused. Never register a second wait on an uncompleted key.
        for (auto key : branches_) RegCloseKey(key);
        branches_.clear();
        if (key_ && key_ != root_) RegCloseKey(key_);
        key_ = nullptr;
    }
    bool Branch(std::wstring path, bool recursive, bool followMissing = false)
    {
        HKEY key = nullptr;
        bool target = true;
        while (RegOpenKeyExW(root_, path.c_str(), 0, KEY_NOTIFY, &key) != ERROR_SUCCESS)
        {
            if (!followMissing) return false;
            const auto slash = path.rfind(L'\\');
            if (slash == std::wstring::npos) return false;
            path.resize(slash); target = false;
        }
        branches_.push_back(key);
        const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC |
            (target ? REG_NOTIFY_CHANGE_LAST_SET : 0);
        return RegNotifyChangeKeyValue(key, target && recursive, filter, event, TRUE) == ERROR_SUCCESS;
    }
    bool FilteredClasses()
    {
        // A hive without Local Settings can retain a single recursive watch.
        // Its later creation signals that watch and installs the split below.
        HKEY local = nullptr;
        if (RegOpenKeyExW(root_, (path_ + L"\\Local Settings").c_str(), 0, KEY_NOTIFY, &local) != ERROR_SUCCESS)
            return false;
        RegCloseKey(local);
        // All ordinary class roots share ONE event, preserving simultaneous
        // real changes while excluding nested Shell browse-state writes.
        // A direct root watch catches newly created/removed types and ProgIDs.
        if (RegNotifyChangeKeyValue(key_, FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET |
                REG_NOTIFY_THREAD_AGNOSTIC, event, TRUE) != ERROR_SUCCESS) return false;
        HKEY names = nullptr;
        if (RegOpenKeyExW(root_, path_.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &names) != ERROR_SUCCESS) return false;
        bool ok = true;
        for (DWORD i = 0;; ++i)
        {
            wchar_t name[1024]{}; DWORD length = 1024;
            const auto status = RegEnumKeyExW(names, i, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS) { ok = false; break; }
            if (lstrcmpiW(name, L"Local Settings") != 0 && !Branch(path_ + L"\\" + name, true))
            { ok = false; break; }
        }
        RegCloseKey(names);
        // Local Settings is also a potential ordinary class; keep its own
        // values / shell / shellex plus the package repository read by Scanner.
        // Missing branches follow direct ancestor NAME changes only.
        ok = ok && Branch(path_ + L"\\Local Settings", false) &&
            Branch(path_ + L"\\Local Settings\\shell", true, true) &&
            Branch(path_ + L"\\Local Settings\\shellex", true, true) &&
            Branch(path_ + L"\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages", true, true);
        return ok;
    }

    void Arm()
    {
        armed = false;
        target_ = false;
        CloseKeys();
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
        if (target_ && filterClasses_)
        {
            if (FilteredClasses()) { armed = true; MenuTrace("registry.watch", "classes.filtered", 0, traceId_); return; }
            // Enumeration / access races must preserve conservative coverage.
            // Close all partial subscriptions before the broad fallback.
            CloseKeys(); ResetEvent(event);
            if (RegOpenKeyExW(root_, path_.c_str(), 0, KEY_NOTIFY, &key_) != ERROR_SUCCESS) return;
        }
        const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_THREAD_AGNOSTIC |
            (target_ ? REG_NOTIFY_CHANGE_LAST_SET : 0);
        armed = RegNotifyChangeKeyValue(key_, target_, filter, event, TRUE) == ERROR_SUCCESS;
        MenuTrace("registry.watch", !armed ? "failed" : target_ ? "target" : "ancestor", 0, traceId_);
    }
};
}
