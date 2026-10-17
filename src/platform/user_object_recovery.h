#pragma once

#include <windows.h>

#include <new>

namespace snowdesktop::user_object_recovery
{
inline constexpr DWORD kDefaultQuota = 10000;
inline constexpr ULONGLONG kSampleIntervalMs = 1000;
inline constexpr ULONGLONG kPressureDurationMs = 3000;
inline constexpr ULONGLONG kRetryIntervalMs = 30000;
inline constexpr ULONGLONG kRestartIntervalMs = 15 * 60 * 1000;
inline constexpr wchar_t kHistoryKey[] = L"Software\\SnowDesktop\\ResourceRecovery";

inline DWORD ProcessQuota()
{
    DWORD quota = 0;
    DWORD bytes = sizeof(quota);
    const auto error = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
        L"USERProcessHandleQuota", RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
        nullptr, &quota, &bytes);
    return error == ERROR_SUCCESS && quota >= 200 && quota <= 18000
        ? quota : kDefaultQuota;
}

// Separate from crash history: an orderly recovery exits successfully and
// clears the crash counter. Persist before handing off so the new host cannot
// immediately repeat an automatic restart. Registry failure denies recovery.
inline LSTATUS ReserveRestart(ULONGLONG now,
    HKEY root = HKEY_CURRENT_USER, const wchar_t* path = kHistoryKey)
{
    HKEY key = nullptr;
    auto error = RegCreateKeyExW(root, path, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error != ERROR_SUCCESS) return error;
    ULONGLONG previous = 0;
    DWORD bytes = sizeof(previous);
    error = RegGetValueW(key, nullptr, L"LastRestartUptime",
        RRF_RT_REG_QWORD, nullptr, &previous, &bytes);
    if (error == ERROR_FILE_NOT_FOUND) error = ERROR_SUCCESS;
    // A lower uptime belongs to a new boot. Within one boot, allow at most
    // one automatic attempt in the interval, including failed handoffs.
    if (error == ERROR_SUCCESS && previous && now >= previous &&
        now - previous < kRestartIntervalMs)
        error = ERROR_RETRY;
    if (error == ERROR_SUCCESS)
    {
        const ULONGLONG stamp = now ? now : 1;
        error = RegSetValueExW(key, L"LastRestartUptime", 0, REG_QWORD,
            reinterpret_cast<const BYTE*>(&stamp), sizeof(stamp));
    }
    RegCloseKey(key);
    return error;
}

enum class Event
{
    Pressure,
    CheckpointFailed,
    RestartLimited,
    Restarting,
    RestartFailed,
    CallbackFailed,
};

// Runs on the host's existing maintenance timer. It creates no USER objects,
// and does not depend on opening a menu, window or another timer at exhaustion.
class Monitor
{
public:
    template<class Query, class Checkpoint, class Reserve, class Restart, class Report>
    bool Poll(ULONGLONG now, DWORD quota, bool safeToRestart,
        Query&& query, Checkpoint&& checkpoint, Reserve&& reserve,
        Restart&& restart, Report&& report)
    {
        if (recovering_ || (sampled_ && now - lastSample_ < kSampleIntervalMs))
            return false;
        sampled_ = true;
        lastSample_ = now;
        const DWORD count = query();
        if (quota < 200 || quota > 18000) quota = kDefaultQuota;
        const auto emit = [&](Event event, LSTATUS error) noexcept {
            try { report(event, count, quota, error); }
            catch (...) { /* Diagnostic allocation cannot abort recovery. */ }
        };
        const bool pressure = count >= quota * 80 / 100;
        if (pressure && !pressureReported_)
            emit(Event::Pressure, ERROR_SUCCESS);
        pressureReported_ = pressure;
        if (count < quota * 95 / 100)
        {
            critical_ = false;
            return false;
        }
        if (!critical_)
        {
            critical_ = true;
            criticalSince_ = now;
        }
        if (!safeToRestart || now - criticalSince_ < kPressureDurationMs ||
            (attempted_ && now - lastAttempt_ < retryInterval_))
            return false;

        recovering_ = true;
        struct Reset { bool& active; ~Reset() { active = false; } } reset{recovering_};
        attempted_ = true;
        lastAttempt_ = now;
        retryInterval_ = kRetryIntervalMs;
        try
        {
            if (!checkpoint())
            {
                emit(Event::CheckpointFailed, ERROR_CANCELLED);
                return false;
            }
            const auto error = reserve(now);
            if (error != ERROR_SUCCESS)
            {
                retryInterval_ = kRestartIntervalMs;
                emit(Event::RestartLimited, error);
                return false;
            }
            retryInterval_ = kRestartIntervalMs;
            emit(Event::Restarting, ERROR_SUCCESS);
            if (restart()) return true;
            emit(Event::RestartFailed, ERROR_GEN_FAILURE);
        }
        catch (const std::bad_alloc&)
        {
            emit(Event::CallbackFailed, ERROR_NOT_ENOUGH_MEMORY);
        }
        catch (...)
        {
            emit(Event::CallbackFailed, ERROR_UNHANDLED_EXCEPTION);
        }
        return false;
    }

private:
    ULONGLONG lastSample_ = 0, criticalSince_ = 0, lastAttempt_ = 0;
    ULONGLONG retryInterval_ = kRetryIntervalMs;
    bool sampled_ = false, critical_ = false, pressureReported_ = false;
    bool attempted_ = false, recovering_ = false;
};
}
