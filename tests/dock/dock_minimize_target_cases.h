#pragma once

// Included inside the resource fixture namespace after Check/WaitFor helpers.
struct ProcessFixture
{
    DWORD waitResult = WAIT_TIMEOUT;
    bool wow64 = true;
    bool querySucceeds = true;
};
std::array<ProcessFixture, 4> processFixtures;
unsigned currentProcessFixture = 0, processOpens = 0, processQueries = 0, processCloses = 0;
bool denyProcessOpen = false, denySynchronize = false;
HANDLE WINAPI OpenProcessFixture(DWORD access, BOOL, DWORD)
{
    ++processOpens;
    if (denyProcessOpen || (denySynchronize && (access & SYNCHRONIZE))) return nullptr;
    return reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(currentProcessFixture + 1));
}
BOOL WINAPI QueryProcessFixture(HANDLE handle, PBOOL wow64)
{
    ++processQueries;
    const auto& state = processFixtures[reinterpret_cast<UINT_PTR>(handle) - 1];
    *wow64 = state.wow64;
    return state.querySucceeds;
}
DWORD WINAPI WaitProcessFixture(HANDLE handle, DWORD)
{
    return processFixtures[reinterpret_cast<UINT_PTR>(handle) - 1].waitResult;
}
BOOL WINAPI CloseProcessFixture(HANDLE) { ++processCloses; return TRUE; }
void RunProcessBitnessCacheTests()
{
    processFixtures = {};
    currentProcessFixture = processOpens = processQueries = processCloses = 0;
    denyProcessOpen = denySynchronize = false;
    {
        ProcessBitnessCache cache({OpenProcessFixture, QueryProcessFixture, WaitProcessFixture, CloseProcessFixture});
        bool sameResults = true;
        for (unsigned refresh = 0; refresh < 10000; ++refresh)
        {
            cache.BeginUpdate();
            sameResults &= cache.Is32Bit(1234) && cache.Is32Bit(1234);
            cache.EndUpdate();
        }
        Check(sameResults && processOpens == 1 && processQueries == 1 && processCloses == 0,
            "same live process and multiple windows reuse one bitness query over 10000 updates");
        processFixtures[0].waitResult = WAIT_OBJECT_0;
        currentProcessFixture = 1;
        processFixtures[1].wow64 = false;
        cache.BeginUpdate();
        Check(!cache.Is32Bit(1234) && processOpens == 2 && processQueries == 2 && processCloses == 1,
            "recycled PID observes the replacement process bitness and retires the old identity");
        cache.EndUpdate();
        processFixtures[1].waitResult = WAIT_FAILED;
        currentProcessFixture = 2;
        processFixtures[2].querySucceeds = false;
        Check(!cache.Is32Bit(1234) && !cache.Is32Bit(1234) && processQueries == 4 && processCloses == 4,
            "failed lifetime validation and failed queries cannot leave a cached native result");
        processFixtures[2].querySucceeds = true;
        Check(cache.Is32Bit(1234) && processQueries == 5,
            "bitness query recovers immediately after a transient failure");
        cache.BeginUpdate(); cache.EndUpdate();
        Check(processCloses == 5, "removing the last target releases its retained process identity");
        denyProcessOpen = true;
        Check(!cache.Is32Bit(1234) && processQueries == 5, "access denied keeps the original false fallback");
        denyProcessOpen = false;
        denySynchronize = true;
        const unsigned beforeQueries = processQueries;
        Check(cache.Is32Bit(1234) && cache.Is32Bit(1234) && processQueries == beforeQueries + 2 && processCloses == 7,
            "query-only permission still works and stays uncached without lifetime validation");
        denySynchronize = false;
        Check(cache.Is32Bit(1234), "full access recovers after query-only fallback");
        cache.Clear();
        Check(processCloses == 8, "stop clears the retained process handle");
        Check(cache.Is32Bit(1234), "cleared cache can start a new lifetime");
    }
    Check(processCloses == 9, "cache destruction closes its remaining handle");
}
void RunTargetMaintenanceTests(DockExternalMinimize& monitor, HWND receiver, HWND target)
{
    const std::array<HWND, 4> windows{target, receiver, target, nullptr};
    monitor.UpdateTargets(windows);
    const auto attempts = monitor.RegistrationStatistics().attempts;
    const auto revision = GetPropW(receiver, kRevisionProperty);
    Check(monitor.OwnsTarget(target) && !monitor.OwnsTarget(receiver) && monitor.HookCount() == 1 &&
        GetPropW(target, kReadyProperty) == receiver, "real maintenance accepts duplicate unsorted input and rejects own windows");
    monitor.UpdateTargets(windows);
    Check(monitor.RegistrationStatistics().attempts == attempts && GetPropW(receiver, kRevisionProperty) == revision,
        "unchanged targets preserve hooks and the helper revision");
    RemovePropW(target, kReadyProperty); RemovePropW(target, kCloakDispatcherProperty);
    monitor.UpdateTargets(windows);
    Check(GetPropW(target, kReadyProperty) == receiver && GetPropW(target, kCloakDispatcherProperty),
        "unchanged targets repair lost readiness and dispatcher properties");
    RemovePropW(target, kTargetProperty);
    monitor.UpdateTargets(windows);
    Check(monitor.OwnsTarget(target) && GetPropW(target, kReadyProperty) == receiver &&
        GetPropW(receiver, kRevisionProperty) != revision, "lost target ownership is reclaimed and notifies the helper");
    const HWND other = CreateWindowFixture();
    Check(other != nullptr, "foreign owner fixture initializes");
    if (other)
    {
        SetPropW(other, kOwnerProperty, ULongToHandle(GetCurrentProcessId()));
        SetPropW(other, kRevisionProperty, ULongToHandle(1));
        SetPropW(target, kTargetProperty, other); SetPropW(target, kReadyProperty, other);
        monitor.UpdateTargets(windows);
        Check(!monitor.OwnsTarget(target) && monitor.HookCount() == 0 && GetPropW(target, kTargetProperty) == other,
            "a live foreign host retains ownership and removes our thread hook");
        RemovePropW(other, kOwnerProperty); RemovePropW(other, kRevisionProperty);
        monitor.UpdateTargets(windows);
        Check(monitor.OwnsTarget(target) && monitor.HookCount() == 1 && GetPropW(target, kReadyProperty) == receiver,
            "a crashed host marker is reclaimed with ready state restored");
        DestroyWindow(other);
    }
    bool acquired = false;
    Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(target, receiver, acquired)) && acquired,
        "real maintenance target can acquire a source cloak");
    monitor.UpdateTargets({});
    Check(!monitor.OwnsTarget(target) && monitor.HookCount() == 0 && !GetPropW(target, kTargetProperty) &&
        !GetPropW(target, kReadyProperty) && !GetPropW(target, kCloakDispatcherProperty),
        "empty target snapshot removes hooks and owned protocol properties");
    Check(WaitFor([&] { DWORD flags = 0; return SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
        !(flags & DWM_CLOAKED_APP); }), "target removal releases the actual source cloak");
    monitor.UpdateTargets(windows);
    Check(monitor.OwnsTarget(target) && monitor.HookCount() == 1, "removed target can be registered again");
}
