// The OS discovery/state boundary is replaced; this is the same cache decision
// executed by the Windows backend, not a separate implementation of its rules.
void HotspotManagerCacheLifecycle()
{
    using State = HotspotManagerState;
    struct Manager { State state = State::Off; };
    using Handle = std::shared_ptr<Manager>;
    using Cache = HotspotManagerCache<Handle>;
    const auto epoch = Cache::Clock::time_point{};
    const auto read = [](const Handle& manager) { return manager->state; };
    Cache cache;
    unsigned discoveries = 0;
    Handle next = std::make_shared<Manager>();
    const auto discover = [&] { ++discoveries; return next; };
    const auto get = [&](auto time, std::uint64_t generation = 0, bool notify = true, bool enable = false) {
        return cache.Get(epoch + time, generation, notify, enable, read, discover);
    };
    const auto first = get(0s);
    for (int sample = 0; sample < 1000; ++sample)
        Require(get(1s) == first, "unchanged closed hotspot retains its OS manager");
    Require(discoveries == 1, "stable hotspot reads must not enumerate historical profiles again");

    next = std::make_shared<Manager>();
    Require(get(2s, 1) == next && discoveries == 2, "network change replaces a closed manager");
    const auto active = next;
    active->state = State::On;
    next = std::make_shared<Manager>();
    Require(get(100s, 2, true, true) == active && discoveries == 2,
        "an active hotspot remains controllable after upstream loss and a new enable gesture");
    active->state = State::InTransition;
    Require(get(101s, 3) == active && discoveries == 2, "an in-flight hotspot keeps the same manager");
    active->state = State::Off;
    Require(get(102s, 3) == next && discoveries == 3, "stopped hotspot reconciles the changed upstream");
    const auto previous = next;
    next = std::make_shared<Manager>();
    Require(get(103s, 3, true, true) == next && next != previous && discoveries == 4,
        "explicit enable rediscovers the upstream even inside the audit interval");
    next->state = State::Unknown;
    next = std::make_shared<Manager>();
    Require(get(104s, 3) == next && discoveries == 5, "an invalid OS manager is replaced immediately");
    cache.Reset();
    Require(get(105s, 3) == next && discoveries == 6, "closing and reopening does not retain a stale cache");

    Cache audit;
    unsigned audits = 0;
    const auto auditDiscover = [&] { ++audits; return next; };
    audit.Get(epoch, 0, true, false, read, auditDiscover);
    audit.Get(epoch + 29s, 0, true, false, read, auditDiscover);
    Require(audits == 1, "normal polls reuse discovery before its bounded audit");
    audit.Get(epoch + 30s, 0, true, false, read, auditDiscover);
    Require(audits == 2, "lost network notifications cannot pin an upstream indefinitely");
    audit.Get(epoch + 29s, 0, true, false, read, auditDiscover);
    Require(audits == 3, "a backwards clock cannot extend a stale cache");

    Cache fallback;
    unsigned retries = 0;
    const auto retry = [&] { ++retries; return next; };
    fallback.Get(epoch, 0, false, false, read, retry);
    fallback.Get(epoch + 4s, 0, false, false, read, retry);
    Require(retries == 1, "notification failure still avoids per-poll discovery");
    fallback.Get(epoch + 5s, 0, false, false, read, retry);
    Require(retries == 2, "notification failure uses a bounded retry interval");

    Cache absent;
    unsigned missing = 0;
    const auto unavailable = [&]() -> Handle { ++missing; return nullptr; };
    absent.Get(epoch, 0, true, false, read, unavailable);
    absent.Get(epoch + 1s, 0, true, false, read, unavailable);
    Require(missing == 1, "unsupported tethering must not enumerate on every panel poll");
    absent.Get(epoch + 5s, 0, true, false, read, unavailable);
    absent.Get(epoch + 6s, 1, true, false, read, unavailable);
    absent.Get(epoch + 6s, 1, true, true, read, unavailable);
    Require(missing == 4, "unsupported tethering retries on expiry, network change and explicit enable");
}

// Included by the shared system-control regression driver. The backend below
// replaces only the slow OS call; scheduling, publication and demand are real.
struct HotspotSlowSampleBackend final : Backend
{
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::chrono::steady_clock::time_point> starts;
    std::chrono::steady_clock::time_point firstEnd{};
    static constexpr auto Interval = std::chrono::milliseconds(40);

    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
    {
        std::unique_lock guard(mutex);
        starts.push_back(std::chrono::steady_clock::now());
        changed.notify_all();
        if (starts.size() == 1)
        {
            // A bounded slow OS boundary, deliberately longer than demand.
            // The observed end/start timestamps establish the failure signal.
            changed.wait_until(guard, starts.front() + Interval * 2, [] { return false; });
            firstEnd = std::chrono::steady_clock::now();
        }
        return {{"host.hotspot", {true, json::Object(), {}, 0, 0}}};
    }
    Result Execute(const Request&, const Cancellation&) override { return {}; }
    void Release(std::string_view) override {}
};

void SlowHotspotSamplingDoesNotCatchUp()
{
    auto backend = std::make_shared<HotspotSlowSampleBackend>();
    Service service(backend);
    Require(service.Subscribe("panel", "host.hotspot", backend->Interval), "hotspot polling subscribes");
    std::unique_lock guard(backend->mutex);
    Require(backend->changed.wait_for(guard, 3s, [&] { return backend->starts.size() >= 2; }),
        "hotspot sampling resumes after its slow first read");
    const auto idle = backend->starts[1] - backend->firstEnd;
    guard.unlock();
    service.RemoveConsumer("panel");
    service.Shutdown();
    std::cout << "hotspot slow-sample idle-us="
        << std::chrono::duration_cast<std::chrono::microseconds>(idle).count() << '\n';
    Require(idle >= backend->Interval,
        "a slow hotspot read must leave one requested interval before another periodic read");
}

struct HotspotInvalidationBackend final : Backend
{
    std::mutex mutex;
    std::condition_variable changed;
    unsigned samples = 0, releases = 0;
    bool releaseFirst = false;
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
    {
        std::unique_lock guard(mutex);
        ++samples;
        changed.notify_all();
        if (samples == 1)
            Require(changed.wait_for(guard, 3s, [&] { return releaseFirst; }), "release first hotspot sample gate");
        return {{"host.hotspot", {true, json::Object(), {}, 0, 0}}};
    }
    Result Execute(const Request&, const Cancellation&) override { return {}; }
    void Release(std::string_view source) override
    { if (source == "hotspot") { std::lock_guard guard(mutex); ++releases; changed.notify_all(); } }
};

void HotspotInvalidationAndShutdown()
{
    auto backend = std::make_shared<HotspotInvalidationBackend>();
    Service service(backend);
    Require(service.Subscribe("panel", "host.hotspot", 60s), "hotspot invalidation fixture subscribes");
    std::unique_lock guard(backend->mutex);
    Require(backend->changed.wait_for(guard, 3s, [&] { return backend->samples == 1; }), "first hotspot sample enters");
    guard.unlock();
    // The snapshot read and invalidation must not wait for the blocked OS read.
    Require(!service.Current("host.hotspot"), "in-flight initial sample leaves the snapshot readable");
    service.Invalidate("hotspot");
    guard.lock();
    backend->releaseFirst = true;
    backend->changed.notify_all();
    Require(backend->changed.wait_for(guard, 3s, [&] { return backend->samples >= 2; }),
        "invalidation during sampling requests fresh state without the periodic cooldown");
    guard.unlock();
    service.Shutdown();
    guard.lock();
    Require(backend->releases == 1, "shutdown releases the hotspot backend and its event subscription");
}
