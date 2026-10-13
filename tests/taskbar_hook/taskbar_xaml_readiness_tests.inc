// Exercise the production discovery loop with a controlled clock and an OS
// boundary that publishes endpoint 1 after its first probe. No live Shell use.
void CheckDelayedTaskbarXamlEndpoint()
{
    using namespace snowdesktop::taskbar_hook;
    ULONGLONG now = 0;
    unsigned firstEndpointCalls = 0;
    const auto result = WaitForTaskbarXaml([&](unsigned endpoint) {
        if (endpoint == 1) ++firstEndpointCalls;
        return endpoint == 1 && now >= 500 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }, [&] { return now; }, [&](DWORD duration) { now += duration; }, [] { return false; });
    Check(result.error == S_OK && result.endpoint == 1,
        "a diagnostics endpoint published after its first probe is found in the same startup attempt");
    Check(firstEndpointCalls >= 2 && now < 30000,
        "startup revisits the early endpoint instead of exhausting the failure budget");
}

void CheckTaskbarXamlReadiness()
{
    using namespace snowdesktop::taskbar_hook;
    CheckDelayedTaskbarXamlEndpoint();
    {
        ULONGLONG now = 0;
        bool waited = false;
        const auto result = WaitForTaskbarXaml([](unsigned endpoint) {
            return endpoint == 60 ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }, [&] { return now; }, [&](DWORD duration) { waited = true; now += duration; }, [] { return false; });
        Check(result.error == S_OK && result.endpoint == 60 && !waited,
            "a ready higher-numbered endpoint is discovered without sleeps between names");
    }
    {
        ULONGLONG now = 0;
        const auto result = WaitForTaskbarXaml([](unsigned) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }, [&] { return now; }, [&](DWORD duration) { now += duration; }, [] { return false; });
        Check(result.error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && now == 30000 &&
            result.attempts > 0 && result.attempts <= 3600,
            "a permanently missing endpoint exits at the readiness deadline with bounded attempts");
    }
    {
        ULONGLONG now = 0;
        unsigned calls = 0;
        const auto result = WaitForTaskbarXaml([&](unsigned) {
            ++calls;
            return E_ACCESSDENIED;
        }, [&] { return now; }, [&](DWORD duration) { now += duration; }, [] { return false; });
        Check(result.error == E_ACCESSDENIED && calls == 1 && now == 0,
            "a permanent initialization error is preserved and reported without readiness retries");
    }
    {
        ULONGLONG now = 0;
        unsigned calls = 0;
        unsigned callsAtCancellation = 0;
        bool cancelled = false;
        const auto result = WaitForTaskbarXaml([&](unsigned) {
            ++calls;
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }, [&] { return now; }, [&](DWORD duration) {
            now += duration; callsAtCancellation = calls; cancelled = true;
        }, [&] { return cancelled; });
        Check(result.error == HRESULT_FROM_WIN32(ERROR_CANCELLED) && callsAtCancellation > 0 &&
            calls == callsAtCancellation && now == 500,
            "cancellation between readiness passes prevents another OS initialization call");
    }
    {
        ULONGLONG now = 0;
        unsigned calls = 0;
        const auto result = WaitForTaskbarXaml([&](unsigned) {
            ++calls;
            now = 30000;
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }, [&] { return now; }, [&](DWORD duration) { now += duration; }, [] { return false; });
        Check(result.error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && calls == 1,
            "an OS call that consumes the readiness budget prevents remaining endpoint probes");
    }
}
