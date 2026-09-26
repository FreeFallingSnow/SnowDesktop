#include "gpu_diagnostics.h"
#include "widget_package.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>
#include <thread>
#include <vector>

namespace snowdesktop::gpu_diagnostics
{
namespace
{
constexpr std::uint64_t MaximumOutputBytes = 64ull * 1024 * 1024;
std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}
std::string Quote(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value)
    {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else if (c < 32) { result += "\\u00"; result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
        else result.push_back(static_cast<char>(c));
    }
    return result + '"';
}
template<class T> void DecimalString(std::ostream& out, T value) { out << '"' << value << '"'; }
void Number(std::ostream& out, double value)
{
    if (std::isfinite(value)) out << std::setprecision(17) << value;
    else out << "null";
}
template<class Range, class Write>
void Array(std::ostream& out, const Range& values, Write&& write)
{
    out << '[';
    bool first = true;
    for (const auto& value : values) { if (!first) out << ','; first = false; write(value); }
    out << ']';
}
bool Integer(std::wstring_view value, unsigned minimum, unsigned maximum, unsigned& result)
{
    if (value.empty()) return false;
    unsigned number = 0;
    for (const auto c : value)
    {
        if (c < L'0' || c > L'9') return false;
        const auto digit = static_cast<unsigned>(c - L'0');
        if (number > (maximum - digit) / 10) return false;
        number = number * 10 + digit;
    }
    if (number < minimum || number > maximum) return false;
    result = number; return true;
}
struct File
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::uint64_t written = 0;
    explicit File(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool Write(const std::string& value)
    {
        std::size_t offset = 0;
        while (offset < value.size())
        {
            DWORD bytes = 0;
            const auto length = static_cast<DWORD>(std::min<std::size_t>(value.size() - offset, 1024 * 1024));
            if (!WriteFile(handle, value.data() + offset, length, &bytes, nullptr) || !bytes) return false;
            offset += bytes; written += bytes;
        }
        return true;
    }
};
int Error(std::string_view message, int code, DWORD status = 0)
{
    std::cerr << "{\"ok\":false,\"error\":" << Quote(message) << ",\"win32Status\":" << status << "}\n";
    return code;
}
std::filesystem::path Executable()
{
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    buffer.resize(length); return buffer;
}
std::string Metadata(const Options& options, std::string_view version, std::string_view hash)
{
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"kind\":\"metadata\",\"schemaVersion\":1,\"version\":" << Quote(version)
        << ",\"executableSha256\":" << Quote(hash) << ",\"architecture\":\"x64\",\"samples\":" << options.samples
        << ",\"intervalMs\":" << options.intervalMs << ",\"maximumOutputBytes\":" << MaximumOutputBytes
        << ",\"source\":\"standalone-production-sampler\",\"os\":";
    using GetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto getVersion = module ? reinterpret_cast<GetVersion>(GetProcAddress(module, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW os{}; os.dwOSVersionInfoSize = sizeof(os);
    if (getVersion && getVersion(&os) == 0)
        out << "{\"major\":" << os.dwMajorVersion << ",\"minor\":" << os.dwMinorVersion << ",\"build\":" << os.dwBuildNumber << '}';
    else out << "null";
    return out.str() + "}\n";
}
}
bool ParseOptions(std::span<const std::wstring_view> arguments, Options& options, std::string& error)
{
    options = {}; error.clear();
    if (arguments.empty() || arguments.front().empty())
    { error = "gpu-diagnostics requires a new output file"; return false; }
    options.output = arguments.front();
    bool samplesSet = false, intervalSet = false;
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const auto option = arguments[index];
        if (option != L"--samples" && option != L"--interval-ms")
        { error = "unknown gpu-diagnostics option"; return false; }
        if (++index == arguments.size())
        { error = "gpu-diagnostics option is missing its value"; return false; }
        if (option == L"--samples")
        {
            if (samplesSet || !Integer(arguments[index], 2, 120, options.samples))
            { error = "samples must be specified once as an integer from 2 to 120"; return false; }
            samplesSet = true;
        }
        else
        {
            if (intervalSet || !Integer(arguments[index], 250, 5000, options.intervalMs))
            { error = "interval-ms must be specified once as an integer from 250 to 5000"; return false; }
            intervalSet = true;
        }
    }
    if ((options.samples - 1) * options.intervalMs > 120000)
    { error = "requested sampling waits must not exceed 120 seconds"; return false; }
    return true;
}
std::string SerializeSample(unsigned index, const widget_runtime::WidgetGpuDataSnapshot& sample,
    const widget_runtime::WidgetGpuDiagnosticSample& diagnostic)
{
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::boolalpha;
    out << "{\"kind\":\"sample\",\"index\":" << index << ",\"timestampMs\":" << sample.timestampMs
        << ",\"available\":" << sample.available << ",\"warmingUp\":" << sample.warmingUp
        << ",\"error\":" << Quote(sample.error) << ",\"collectStatus\":" << diagnostic.collectStatus
        << ",\"topologyStatus\":" << diagnostic.topologyStatus << ",\"fileTime\":";
    DecimalString(out, diagnostic.fileTime);
    out << ",\"elapsedUs\":" << diagnostic.elapsedUs << ",\"intervalUs\":" << diagnostic.intervalUs
        << ",\"resumed\":" << diagnostic.resumed << ",\"statistics\":{\"samples\":" << diagnostic.statistics.samples
        << ",\"queryCreations\":" << diagnostic.statistics.queryCreations << ",\"topologyRefreshes\":" << diagnostic.statistics.topologyRefreshes
        << ",\"bufferGrowths\":" << diagnostic.statistics.bufferGrowths << "},\"adapters\":";
    Array(out, sample.adapters, [&](const auto& a) {
        out << "{\"id\":" << Quote(a.id) << ",\"name\":" << Quote(a.name) << ",\"luid\":"; DecimalString(out, a.luid);
        out << ",\"vendor\":" << a.vendor << ",\"device\":" << a.device
            << ",\"usageAvailable\":" << a.usageAvailable << ",\"usagePercent\":";
        if (a.usageAvailable && !sample.warmingUp) Number(out, a.usagePercent); else out << "null";
        out << ",\"dedicatedUsageAvailable\":" << a.dedicatedUsageAvailable << ",\"dedicatedUsedBytes\":";
        if (a.dedicatedUsageAvailable) DecimalString(out, a.dedicatedUsedBytes); else out << "null";
        out << ",\"dedicatedMemoryBytes\":"; DecimalString(out, a.dedicatedMemoryBytes);
        out << ",\"sharedUsageAvailable\":" << a.sharedUsageAvailable << ",\"sharedUsedBytes\":";
        if (a.sharedUsageAvailable) DecimalString(out, a.sharedUsedBytes); else out << "null";
        out << ",\"sharedMemoryBytes\":"; DecimalString(out, a.sharedMemoryBytes);
        out << ",\"engines\":";
        Array(out, a.engines, [&](const auto& e) {
            out << "{\"physical\":" << e.physical << ",\"engine\":" << e.engine << ",\"type\":" << Quote(e.type)
                << ",\"samples\":" << e.samples << ",\"rawTotal\":"; Number(out, e.rawTotal);
            out << ",\"usagePercent\":"; Number(out, e.usagePercent); out << '}';
        });
        out << '}';
    });
    out << ",\"counters\":";
    Array(out, diagnostic.counters, [&](const auto& c) {
        out << "{\"path\":" << Quote(Utf8(c.path)) << ",\"addStatus\":" << c.addStatus << ",\"formattedStatus\":" << c.formattedStatus
            << ",\"rawStatus\":" << c.rawStatus << ",\"formatted\":";
        Array(out, c.formatted, [&](const auto& f) {
            out << "{\"instance\":" << Quote(Utf8(f.instance)) << ",\"status\":" << f.status << ",\"percent\":";
            Number(out, f.percent); out << ",\"bytes\":"; DecimalString(out, f.bytes); out << '}';
        });
        out << ",\"raw\":";
        Array(out, c.raw, [&](const auto& r) {
            out << "{\"instance\":" << Quote(Utf8(r.instance)) << ",\"status\":" << r.status << ",\"multiCount\":" << r.multiCount
                << ",\"first\":"; DecimalString(out, r.first); out << ",\"second\":"; DecimalString(out, r.second);
            out << ",\"fileTime\":"; DecimalString(out, r.fileTime); out << '}';
        });
        out << '}';
    });
    return out.str() + "}\n";
}
int Run(int argc, wchar_t** argv, std::string_view version)
{
    try
    {
        std::vector<std::wstring_view> arguments;
        for (int index = 2; index < argc; ++index) arguments.emplace_back(argv[index]);
        Options options; std::string error;
        if (!ParseOptions(arguments, options, error)) return Error(error, 2);
        File file(options.output);
        if (file.handle == INVALID_HANDLE_VALUE) return Error("cannot create new diagnostic output; existing files are never overwritten", 1, GetLastError());
        if (GetFileType(file.handle) != FILE_TYPE_DISK) return Error("diagnostic output must be a regular disk file", 1);
        const auto executable = Executable();
        const auto hash = executable.empty() ? std::string{} : widget::WidgetPackageManager::Sha256File(executable);
        if (hash.empty()) return Error("cannot fingerprint diagnostic executable", 1);
        if (!file.Write(Metadata(options, version, hash))) return Error("cannot write diagnostic metadata", 1, GetLastError());
        widget_runtime::WidgetGpuSampler sampler;
        unsigned captured = 0, usable = 0;
        std::string reason;
        for (unsigned index = 0; index < options.samples; ++index)
        {
            if (index) std::this_thread::sleep_for(std::chrono::milliseconds(options.intervalMs));
            const auto sample = sampler.Sample(false, true);
            const auto& diagnostic = sampler.Diagnostic();
            if (!diagnostic) return Error("sampler did not provide diagnostic details", 1);
            const auto record = SerializeSample(index, sample, *diagnostic);
            if (record.size() + file.written > MaximumOutputBytes - 512) { reason = "outputLimit"; break; }
            if (!file.Write(record)) return Error("cannot write diagnostic sample", 1, GetLastError());
            ++captured;
            if (std::any_of(sample.adapters.begin(), sample.adapters.end(), [&](const auto& a) { return a.usageAvailable && !sample.warmingUp; })) ++usable;
        }
        const bool complete = captured == options.samples;
        const auto summary = std::string("{\"kind\":\"summary\",\"complete\":") + (complete ? "true" : "false") +
            ",\"capturedSamples\":" + std::to_string(captured) + ",\"usableUsageSamples\":" + std::to_string(usable) +
            ",\"reason\":" + Quote(reason) + "}\n";
        if (!file.Write(summary) || !FlushFileBuffers(file.handle)) return Error("cannot finish diagnostic output", 1, GetLastError());
        std::cout << "{\"ok\":" << (complete ? "true" : "false") << ",\"path\":" << Quote(Utf8(options.output.wstring()))
            << ",\"capturedSamples\":" << captured << ",\"usableUsageSamples\":" << usable << ",\"reason\":" << Quote(reason) << "}\n";
        return complete ? 0 : 1;
    }
    catch (const std::exception& e) { return Error(e.what(), 1); }
}
}

