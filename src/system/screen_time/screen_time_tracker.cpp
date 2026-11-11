#include "screen_time_tracker.h"

#include "common/json_value.h"
#include "data/atomic_file.h"
#include "dock/dock_process_discovery.h"
#include "platform/foreground_fullscreen.h"

#include <windows.h>
#include <appmodel.h>
#include <propkey.h>
#include <propsys.h>
#include <shlobj.h>

#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>

namespace snowdesktop::screen_time
{
namespace
{
using Microsoft::WRL::ComPtr;
using clock_type = std::chrono::system_clock;

struct LocalTime
{
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    std::string date;
};

std::string DateText(const std::tm& value)
{
    char text[16]{};
    sprintf_s(text, "%04d-%02d-%02d",
        value.tm_year + 1900, value.tm_mon + 1, value.tm_mday);
    return text;
}

LocalTime ToLocal(clock_type::time_point time)
{
    const std::time_t seconds = clock_type::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &seconds);
    LocalTime result;
    result.year = local.tm_year + 1900;
    result.month = local.tm_mon + 1;
    result.day = local.tm_mday;
    result.hour = std::clamp(local.tm_hour, 0, 23);
    result.date = DateText(local);
    return result;
}

std::optional<clock_type::time_point> NextLocalMidnight(
    clock_type::time_point time)
{
    const std::time_t seconds = clock_type::to_time_t(time);
    std::tm local{};
    localtime_s(&local, &seconds);
    local.tm_mday += 1;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    const std::time_t midnight = mktime(&local);
    if (midnight == static_cast<std::time_t>(-1)) return std::nullopt;
    return clock_type::from_time_t(midnight);
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), text.data(), size, nullptr, nullptr);
    return text;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring text(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), text.data(), size);
    return text;
}

std::string EscapeJson(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20)
            {
                char hex[8]{};
                sprintf_s(hex, "\\u%04x", ch);
                out += hex;
            }
            else
                out.push_back(static_cast<char>(ch));
            break;
        }
    }
    return out;
}

std::wstring RegisteredAppName(const wchar_t* appId)
{
    if (!appId || !*appId) return {};
    ComPtr<IShellItem> app;
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY,
            appId, IID_PPV_ARGS(&app)))) return {};
    PWSTR displayName = nullptr;
    std::wstring name;
    if (SUCCEEDED(app->GetDisplayName(SIGDN_NORMALDISPLAY, &displayName)) &&
        displayName)
        name = displayName;
    CoTaskMemFree(displayName);
    return name;
}

std::wstring FileDescription(const std::wstring& path)
{
    ComPtr<IShellItem2> executable;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr,
            IID_PPV_ARGS(&executable)))) return {};
    PWSTR description = nullptr;
    std::wstring name;
    if (SUCCEEDED(executable->GetString(PKEY_FileDescription, &description)) &&
        description)
        name = description;
    CoTaskMemFree(description);
    return name;
}

std::wstring FileStem(std::wstring_view path)
{
    const auto separator = path.find_last_of(L"\\/");
    auto name = separator == std::wstring_view::npos
        ? path : path.substr(separator + 1);
    if (name.size() > 4 && _wcsicmp(name.data() + name.size() - 4, L".exe") == 0)
        name.remove_suffix(4);
    return std::wstring(name);
}

struct Identity
{
    std::string key;
    std::string name;
};

std::optional<Identity> ResolveForeground()
{
    const HWND window = GetForegroundWindow();
    if (!window || window == GetShellWindow() ||
        fullscreen::IsShellWindow(window))
        return std::nullopt;
    wchar_t windowClass[128]{};
    GetClassNameW(window, windowClass, static_cast<int>(std::size(windowClass)));
    const std::wstring_view kind(windowClass);
    if (kind == L"Progman" || kind == L"WorkerW") return std::nullopt;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!processId || processId == GetCurrentProcessId()) return std::nullopt;

    std::wstring aumid;
    std::wstring displayName;
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&properties))))
    {
        PROPVARIANT id{};
        if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &id)) &&
            id.vt == VT_LPWSTR && id.pwszVal && *id.pwszVal)
        {
            aumid = id.pwszVal;
            displayName = RegisteredAppName(id.pwszVal);
        }
        PropVariantClear(&id);
    }

    DWORD queryProcess = processId;
    if (kind == L"ApplicationFrameWindow")
    {
        const HWND child = FindWindowExW(
            window, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        DWORD hosted = 0;
        if (!child || !GetWindowThreadProcessId(child, &hosted) || !hosted)
            return std::nullopt;
        queryProcess = hosted;
    }

    const HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, queryProcess);
    if (!process) return std::nullopt;
    if (aumid.empty())
    {
        UINT32 length = 0;
        if (GetApplicationUserModelId(process, &length, nullptr) ==
            ERROR_INSUFFICIENT_BUFFER && length > 1)
        {
            std::wstring id(length, L'\0');
            if (GetApplicationUserModelId(process, &length, id.data()) ==
                ERROR_SUCCESS)
            {
                id.resize(wcslen(id.c_str()));
                if (!id.empty())
                {
                    aumid = id;
                    if (displayName.empty())
                        displayName = RegisteredAppName(id.c_str());
                }
            }
        }
    }
    wchar_t path[32768]{};
    DWORD pathLength = static_cast<DWORD>(std::size(path));
    const bool hasPath = QueryFullProcessImageNameW(
        process, 0, path, &pathLength) != FALSE;
    CloseHandle(process);
    if (!hasPath && aumid.empty()) return std::nullopt;

    std::wstring resolvedPath;
    if (hasPath)
    {
        resolvedPath = dock_process_discovery::ApplicationExecutablePath(
            queryProcess, std::wstring(path, pathLength));
        if (resolvedPath.empty()) resolvedPath.assign(path, pathLength);
    }
    if (displayName.empty() && !resolvedPath.empty())
        displayName = FileDescription(resolvedPath);
    if (displayName.empty() && !resolvedPath.empty())
        displayName = FileStem(resolvedPath);
    if (displayName.empty() && !aumid.empty())
        displayName = aumid;

    Identity identity;
    if (!aumid.empty())
        identity.key = "aumid:" + WideToUtf8(aumid);
    else
    {
        std::wstring folded = resolvedPath;
        CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
        identity.key = "path:" + WideToUtf8(folded);
    }
    identity.name = WideToUtf8(displayName);
    if (identity.key.empty() || identity.name.empty()) return std::nullopt;
    return identity;
}

bool InputIdle()
{
    LASTINPUTINFO input{sizeof(input)};
    if (GetLastInputInfo(&input) == FALSE) return false;
    const DWORD elapsed = GetTickCount() - input.dwTime;
    if (elapsed > MAXLONG) return false;
    return elapsed >= static_cast<DWORD>(kIdlePauseMs);
}

struct DayData
{
    std::array<std::int64_t, 24> hours{};
    std::map<std::string, std::int64_t> seconds;
    std::map<std::string, std::int64_t> pendingMs;
};

std::int64_t Number(const JsonValue& value)
{
    if (!value.IsNumber() || !std::isfinite(value.number) || value.number < 0)
        return 0;
    return static_cast<std::int64_t>(value.number);
}

} // namespace

struct Tracker::State
{
    explicit State(std::filesystem::path storePath)
        : path(std::move(storePath))
    {
        Load();
        last = clock_type::now();
    }

    void SetEnabled(bool value)
    {
        std::scoped_lock guard(mutex);
        if (enabled == value) return;
        Settle(clock_type::now());
        enabled = value;
        current.reset();
        last = clock_type::now();
        Write();
    }

    void Observe()
    {
        std::scoped_lock guard(mutex);
        const auto now = clock_type::now();
        if (now < last)
        {
            last = now;
            current.reset();
            return;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last);
        if (elapsed.count() > kMaximumSliceMs)
            elapsed = std::chrono::milliseconds(kMaximumSliceMs);
        const auto sliceEnd = last + elapsed;
        const bool count = enabled && !locked && displayOn && !suspended &&
            !InputIdle();
        if (count && current && elapsed.count() > 0)
            Add(last, sliceEnd, *current);
        last = now;
        current = count ? ResolveForeground() : std::nullopt;
        const auto sinceWrite = std::chrono::steady_clock::now() - lastWrite;
        if (dirty && sinceWrite >= std::chrono::seconds(60)) Write();
    }

    void SetLocked(bool value)
    {
        std::scoped_lock guard(mutex);
        if (locked == value) return;
        Settle(clock_type::now());
        locked = value;
        if (locked) current.reset();
        last = clock_type::now();
    }

    void SetDisplay(bool on)
    {
        std::scoped_lock guard(mutex);
        if (displayOn == on) return;
        Settle(clock_type::now());
        displayOn = on;
        if (!displayOn) current.reset();
        last = clock_type::now();
    }

    void SetSleeping(bool value)
    {
        std::scoped_lock guard(mutex);
        if (suspended == value) return;
        Settle(clock_type::now());
        suspended = value;
        current.reset();
        last = clock_type::now();
        if (!suspended) Write();
    }

    void Flush()
    {
        std::scoped_lock guard(mutex);
        if (dirty) Write();
    }

    void Clear()
    {
        std::scoped_lock guard(mutex);
        names.clear();
        days.clear();
        current.reset();
        last = clock_type::now();
        dirty = true;
        Write();
    }

    TodayView Snapshot() const
    {
        std::scoped_lock guard(mutex);
        TodayView view;
        view.enabled = enabled;
        const auto today = ToLocal(clock_type::now()).date;
        const auto found = days.find(today);
        if (found == days.end()) return view;
        view.hours = found->second.hours;
        std::vector<AppTotal> apps;
        apps.reserve(found->second.seconds.size());
        for (const auto& [key, seconds] : found->second.seconds)
        {
            if (seconds <= 0) continue;
            view.totalSeconds += seconds;
            const auto name = names.find(key);
            apps.push_back({name == names.end() ? key : name->second, seconds});
        }
        std::sort(apps.begin(), apps.end(), [](const auto& left, const auto& right) {
            return left.seconds > right.seconds;
        });
        if (apps.size() > static_cast<std::size_t>(kWidgetAppLimit))
            apps.resize(kWidgetAppLimit);
        view.apps = std::move(apps);
        return view;
    }

    bool Enabled() const
    {
        std::scoped_lock guard(mutex);
        return enabled;
    }

    void Settle(clock_type::time_point now)
    {
        if (now < last)
        {
            last = now;
            return;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last);
        if (elapsed.count() > kMaximumSliceMs)
            elapsed = std::chrono::milliseconds(kMaximumSliceMs);
        const bool count = enabled && !locked && displayOn && !suspended &&
            !InputIdle();
        if (count && current && elapsed.count() > 0)
            Add(last, last + elapsed, *current);
        last = now;
    }

    void Add(clock_type::time_point start, clock_type::time_point end,
        const Identity& app)
    {
        if (end <= start || app.key.empty()) return;
        const auto midnight = NextLocalMidnight(start);
        if (midnight && *midnight < end)
        {
            Add(start, *midnight, app);
            Add(*midnight, end, app);
            return;
        }
        const auto local = ToLocal(start);
        const auto milliseconds = std::chrono::duration_cast<
            std::chrono::milliseconds>(end - start).count();
        if (milliseconds <= 0 || local.date.empty()) return;
        auto& day = days[local.date];
        auto& pending = day.pendingMs[app.key];
        pending += milliseconds;
        const auto whole = pending / 1000;
        pending %= 1000;
        if (whole <= 0) return;
        day.seconds[app.key] += whole;
        day.hours[static_cast<std::size_t>(local.hour)] += whole;
        if (!app.name.empty()) names[app.key] = app.name;
        dirty = true;
        Prune();
    }

    void Prune()
    {
        const auto today = ToLocal(clock_type::now());
        std::tm cursor{};
        cursor.tm_year = today.year - 1900;
        cursor.tm_mon = today.month - 1;
        cursor.tm_mday = today.day - kRetentionDays;
        cursor.tm_hour = 12;
        if (mktime(&cursor) == static_cast<std::time_t>(-1)) return;
        const std::string oldest = DateText(cursor);
        for (auto day = days.begin(); day != days.end();)
        {
            if (day->first < oldest) day = days.erase(day);
            else ++day;
        }
        std::map<std::string, bool> used;
        for (const auto& [date, day] : days)
        {
            (void)date;
            for (const auto& [key, seconds] : day.seconds)
                if (seconds > 0) used[key] = true;
        }
        for (auto name = names.begin(); name != names.end();)
        {
            if (!used.contains(name->first)) name = names.erase(name);
            else ++name;
        }
    }

    void Load()
    {
        std::string text;
        if (!atomic_file::ReadAll(path, text) || text.empty()) return;
        JsonValue root;
        if (!ParseJson(text, root) || !root.IsObject()) return;
        if (const auto* version = root.Find("version");
            version && version->IsNumber() && version->number > 1.0)
            return;
        if (const auto* apps = root.Find("apps"); apps && apps->IsObject())
        {
            for (const auto& [key, value] : apps->object)
            {
                if (const auto* name = value.Find("name");
                    name && name->IsString() && !name->string.empty())
                    names[key] = name->string;
            }
        }
        if (const auto* storedDays = root.Find("days");
            storedDays && storedDays->IsObject())
        {
            for (const auto& [date, value] : storedDays->object)
            {
                if (date.size() != 10 || !value.IsObject()) continue;
                DayData day;
                if (const auto* hours = value.Find("hours");
                    hours && hours->IsArray())
                {
                    const auto count = std::min<std::size_t>(
                        hours->array.size(), day.hours.size());
                    for (std::size_t index = 0; index < count; ++index)
                        day.hours[index] = Number(hours->array[index]);
                }
                if (const auto* usage = value.Find("apps");
                    usage && usage->IsObject())
                {
                    for (const auto& [key, seconds] : usage->object)
                    {
                        const auto amount = Number(seconds);
                        if (amount > 0) day.seconds[key] = amount;
                    }
                }
                if (!day.seconds.empty()) days.emplace(date, std::move(day));
            }
        }
        Prune();
    }

    void Write()
    {
        Prune();
        std::string text;
        text += "{\n  \"version\": 1,\n  \"apps\": {\n";
        bool firstApp = true;
        for (const auto& [key, name] : names)
        {
            if (!firstApp) text += ",\n";
            firstApp = false;
            text += "    \"" + EscapeJson(key) + "\": {\"name\": \"" +
                EscapeJson(name) + "\"}";
        }
        text += "\n  },\n  \"days\": {\n";
        bool firstDay = true;
        for (const auto& [date, day] : days)
        {
            if (!firstDay) text += ",\n";
            firstDay = false;
            text += "    \"" + EscapeJson(date) + "\": {\"hours\": [";
            for (std::size_t hour = 0; hour < day.hours.size(); ++hour)
            {
                if (hour) text += ", ";
                text += std::to_string(day.hours[hour]);
            }
            text += "], \"apps\": {";
            bool firstUsage = true;
            for (const auto& [key, seconds] : day.seconds)
            {
                if (seconds <= 0) continue;
                if (!firstUsage) text += ", ";
                firstUsage = false;
                text += "\"" + EscapeJson(key) + "\": " + std::to_string(seconds);
            }
            text += "}}";
        }
        text += "\n  }\n}\n";
        dirty = !atomic_file::WriteAll(path, text);
        if (!dirty) lastWrite = std::chrono::steady_clock::now();
    }

    mutable std::mutex mutex;
    std::filesystem::path path;
    bool enabled = true;
    bool locked = false;
    bool displayOn = true;
    bool suspended = false;
    bool dirty = false;
    clock_type::time_point last = clock_type::now();
    std::chrono::steady_clock::time_point lastWrite{};
    std::optional<Identity> current;
    std::map<std::string, std::string> names;
    std::map<std::string, DayData> days;
};

Tracker::Tracker(std::filesystem::path path)
    : state_(new State(std::move(path)))
{
}

Tracker::~Tracker()
{
    delete state_;
}

void Tracker::SetEnabled(bool enabled) { state_->SetEnabled(enabled); }
bool Tracker::Enabled() const { return state_->Enabled(); }
void Tracker::Observe() { state_->Observe(); }
void Tracker::SetSessionLocked(bool locked) { state_->SetLocked(locked); }
void Tracker::SetDisplayOn(bool on) { state_->SetDisplay(on); }
void Tracker::SetSuspended(bool suspended) { state_->SetSleeping(suspended); }
void Tracker::Flush() { state_->Flush(); }
void Tracker::Clear() { state_->Clear(); }
TodayView Tracker::Today() const { return state_->Snapshot(); }

std::vector<DayReport> Tracker::Reports() const
{
    std::scoped_lock guard(state_->mutex);
    std::vector<DayReport> reports;
    reports.reserve(state_->days.size());
    for (const auto& [date, day] : state_->days)
    {
        DayReport report;
        report.date = date;
        report.hours = day.hours;
        for (const auto seconds : day.hours) report.totalSeconds += seconds;
        report.apps.reserve(day.seconds.size());
        for (const auto& [key, seconds] : day.seconds)
        {
            if (seconds <= 0) continue;
            const auto name = state_->names.find(key);
            report.apps.push_back({
                name == state_->names.end() ? key : name->second, seconds});
        }
        std::sort(report.apps.begin(), report.apps.end(),
            [](const auto& left, const auto& right) {
                return left.seconds > right.seconds;
            });
        reports.push_back(std::move(report));
    }
    return reports;
}

std::vector<DayReport> LoadReports(const std::filesystem::path& path)
{
    return Tracker(path).Reports();
}

} // namespace snowdesktop::screen_time
