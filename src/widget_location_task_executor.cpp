#include "widget_location_task_executor.h"
#include <windows.h>
#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Foundation.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
namespace snowdesktop::widget_runtime
{
bool ParseLocationOptions(const std::unordered_map<std::string, std::string>& arguments, LocationOptions& options) noexcept
{
    options = {};
    for (const auto& [key, text] : arguments)
    {
        int value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
        if (key == "timeoutMs" && value >= 1000 && value <= 30000) options.timeoutMs = value;
        else if (key == "maximumAgeMs" && value >= 0 && value <= 3600000) options.maximumAgeMs = value;
        else return false;
    }
    return true;
}
bool ValidLocationResult(const LocationResult& r) noexcept
{
    return std::isfinite(r.latitude) && r.latitude >= -90 && r.latitude <= 90 &&
        std::isfinite(r.longitude) && r.longitude >= -180 && r.longitude <= 180 &&
        std::isfinite(r.accuracyMeters) && r.accuracyMeters >= 0 && r.timestampMs > 0;
}
struct WidgetLocationTaskExecutor::State
{
    std::mutex mutex;
    bool stopped = false;
    std::unordered_map<std::uint64_t, winrt::Windows::Foundation::IAsyncInfo> operations;
    std::vector<LocationResult> completions;
    std::function<void()> wake;
    bool Track(std::uint64_t id, winrt::Windows::Foundation::IAsyncInfo operation)
    {
        {
            std::lock_guard lock(mutex);
            const auto found = operations.find(id);
            if (!stopped && found != operations.end())
            { found->second = std::move(operation); return true; }
        }
        operation.Cancel(); return false;
    }
    void Complete(LocationResult result)
    {
        std::lock_guard lock(mutex);
        if (stopped || !operations.erase(result.id)) return;
        completions.push_back(std::move(result));
        // Host wake only posts a message. Serialize it with callback teardown;
        // never call back into DrainCompletions while holding this lock.
        if (wake) wake();
    }
    static winrt::fire_and_forget Run(std::shared_ptr<State> state, std::uint64_t id, LocationOptions options)
    {
        using namespace winrt::Windows::Devices::Geolocation;
        LocationResult result; result.id = id;
        try
        {
            auto access = Geolocator::RequestAccessAsync();
            if (!state->Track(id, access.as<winrt::Windows::Foundation::IAsyncInfo>())) co_return;
            if (co_await access != GeolocationAccessStatus::Allowed) result.error = "locationDenied";
            else
            {
                Geolocator locator; locator.DesiredAccuracy(PositionAccuracy::Default);
                auto position = locator.GetGeopositionAsync(std::chrono::milliseconds(options.maximumAgeMs),
                    std::chrono::milliseconds(options.timeoutMs));
                if (!state->Track(id, position.as<winrt::Windows::Foundation::IAsyncInfo>())) co_return;
                const auto value = co_await position;
                const auto coordinate = value.Coordinate();
                const auto point = coordinate.Point().Position();
                result.latitude = point.Latitude; result.longitude = point.Longitude;
                result.accuracyMeters = coordinate.Accuracy();
                result.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    winrt::clock::to_sys(coordinate.Timestamp()).time_since_epoch()).count();
                switch (coordinate.PositionSource())
                {
                case PositionSource::Satellite: result.source = "satellite"; break;
                case PositionSource::WiFi: result.source = "wifi"; break;
                case PositionSource::Cellular: result.source = "cellular"; break;
                case PositionSource::IPAddress: result.source = "ip"; break;
                case PositionSource::Default: result.source = "default"; break;
                case PositionSource::Obfuscated: result.source = "obfuscated"; break;
                default: result.source = "unknown"; break;
                }
                result.ok = ValidLocationResult(result);
                if (!result.ok) result.error = "locationUnavailable";
            }
        }
        catch (const winrt::hresult_error& error)
        {
            const auto code = error.code();
            result.error = code == E_ACCESSDENIED ? "locationDenied" :
                code == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? "locationTimeout" :
                code == HRESULT_FROM_WIN32(ERROR_CANCELLED) ? "canceled" : "locationUnavailable";
        }
        catch (...) { result.error = "locationUnavailable"; }
        state->Complete(std::move(result));
    }
};
WidgetLocationTaskExecutor::WidgetLocationTaskExecutor() : state_(std::make_shared<State>()) {}
WidgetLocationTaskExecutor::~WidgetLocationTaskExecutor()
{
    std::unordered_map<std::uint64_t, winrt::Windows::Foundation::IAsyncInfo> operations;
    { std::lock_guard lock(state_->mutex); state_->stopped = true; state_->wake = {}; operations.swap(state_->operations); }
    for (const auto& [id, operation] : operations)
    { (void)id; try { if (operation) operation.Cancel(); } catch (...) {} }
}
bool WidgetLocationTaskExecutor::Start(std::uint64_t id, LocationOptions options, bool preview)
{
    if (!id || options.timeoutMs < 1000 || options.timeoutMs > 30000 || options.maximumAgeMs < 0 || options.maximumAgeMs > 3600000) return false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopped || state_->operations.size() >= 16 || state_->operations.contains(id)) return false;
        state_->operations.emplace(id, nullptr);
    }
    DWORD process = 0;
    if (!preview) GetWindowThreadProcessId(GetForegroundWindow(), &process);
    if (preview || process != GetCurrentProcessId())
    {
        LocationResult r; r.id = id; r.error = preview ? "previewUnavailable" : "foregroundRequired";
        state_->Complete(std::move(r));
    }
    else State::Run(state_, id, options);
    return true;
}
bool WidgetLocationTaskExecutor::Cancel(std::uint64_t id)
{
    winrt::Windows::Foundation::IAsyncInfo operation{ nullptr };
    {
        std::lock_guard lock(state_->mutex); const auto found = state_->operations.find(id);
        if (found == state_->operations.end()) return false;
        operation = found->second; state_->operations.erase(found);
    }
    try { if (operation) operation.Cancel(); } catch (...) {}
    return true;
}
void WidgetLocationTaskExecutor::SetCompletionCallback(std::function<void()> callback)
{ std::lock_guard lock(state_->mutex); state_->wake = std::move(callback); }
std::vector<LocationResult> WidgetLocationTaskExecutor::DrainCompletions()
{ std::lock_guard lock(state_->mutex); return std::exchange(state_->completions, {}); }
}
