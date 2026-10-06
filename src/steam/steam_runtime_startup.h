#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <string>
#include <string_view>

namespace snowdesktop::steam_runtime::startup
{
inline constexpr wchar_t kChannelEnvironment[] = L"SNOWDESKTOP_STARTUP_CHANNEL";
inline constexpr DWORD kMagic = 0x53445331;
enum class Phase : LONG { AwaitingHost, Attached, DataAccess, Ready };
struct SharedState
{
    DWORD magic;
    DWORD version;
    DWORD childProcessId;
    volatile LONG phase;
};

class Channel
{
public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel()
    {
        if (state_) UnmapViewOfFile(state_);
        if (mapping_) CloseHandle(mapping_);
        if (ready_) CloseHandle(ready_);
    }
    bool Create()
    {
        std::array<UCHAR, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            return false;
        name_ = L"Local\\SnowDesktop.Startup.";
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (const auto value : random)
        {
            name_.push_back(hex[value >> 4]);
            name_.push_back(hex[value & 15]);
        }
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(SharedState), name_.c_str());
        if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        state_ = static_cast<SharedState*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS,
            0, 0, sizeof(SharedState)));
        if (!state_) return false;
        state_->magic = kMagic;
        state_->version = 1;
        state_->childProcessId = 0;
        state_->phase = static_cast<LONG>(Phase::AwaitingHost);
        ready_ = CreateEventW(nullptr, TRUE, FALSE, (name_ + L".ready").c_str());
        return ready_ && GetLastError() != ERROR_ALREADY_EXISTS;
    }
    void AttachFromEnvironment()
    {
        wchar_t name[128]{};
        const DWORD length = GetEnvironmentVariableW(kChannelEnvironment, name, 128);
        // Children and watchdogs must never inherit the parent's identity.
        SetEnvironmentVariableW(kChannelEnvironment, nullptr);
        if (length == 0 || length >= 128 ||
            !std::wstring_view(name).starts_with(L"Local\\SnowDesktop.Startup."))
            return;
        mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!mapping_) return;
        state_ = static_cast<SharedState*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS,
            0, 0, sizeof(SharedState)));
        if (!state_) return;
        if (state_->magic != kMagic || state_->version != 1 ||
            state_->childProcessId != GetCurrentProcessId())
        {
            UnmapViewOfFile(state_);
            state_ = nullptr;
            return;
        }
        ready_ = OpenEventW(EVENT_MODIFY_STATE, FALSE, (std::wstring(name) + L".ready").c_str());
        if (!ready_)
        {
            UnmapViewOfFile(state_);
            state_ = nullptr;
            return;
        }
        SetPhase(Phase::Attached);
    }
    void SetChild(DWORD processId) { state_->childProcessId = processId; }
    const std::wstring& Name() const { return name_; }
    HANDLE ReadyEvent() const { return ready_; }
    Phase CurrentPhase() const
    {
        return state_ ? static_cast<Phase>(InterlockedCompareExchange(&state_->phase, 0, 0))
                      : Phase::AwaitingHost;
    }
    void SetPhase(Phase phase)
    {
        if (!state_) return;
        InterlockedExchange(&state_->phase, static_cast<LONG>(phase));
        if (phase == Phase::Ready && ready_) SetEvent(ready_);
    }
private:
    std::wstring name_;
    HANDLE mapping_ = nullptr;
    HANDLE ready_ = nullptr;
    SharedState* state_ = nullptr;
};

inline Channel& HostChannel() { static Channel channel; return channel; }
inline void Begin() { HostChannel().AttachFromEnvironment(); }
inline void BeginDataAccess() { HostChannel().SetPhase(Phase::DataAccess); }
inline void Ready() { HostChannel().SetPhase(Phase::Ready); }
}
