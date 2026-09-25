#pragma once
#include "system_controls.h"
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <thread>
#include <winrt/Windows.Foundation.h>

namespace snowdesktop::system_control::windows
{
template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
inline std::string Utf8(std::wstring_view value) { return winrt::to_string(value); }
inline std::wstring Wide(std::string_view value) { return std::wstring(winrt::to_hstring(value)); }
inline std::string Argument(const Request& request, const char* name)
{ const auto found = request.arguments.find(name); return found == request.arguments.end() ? std::string{} : found->second; }
inline double Numeric(const Request& request, const char* name)
{ const auto value = Argument(request, name); double number = 0; std::from_chars(value.data(), value.data() + value.size(), number); return number; }
inline std::string Guid(const GUID& guid)
{ wchar_t buffer[40]{}; StringFromGUID2(guid, buffer, 40); return Utf8(buffer); }
inline bool ParseGuid(std::string_view text, GUID& guid)
{ return text.size() <= 40 && SUCCEEDED(CLSIDFromString(Wide(text).c_str(), &guid)); }
inline Result Error(DWORD status, std::string error = "unavailable")
{ return {false, status == ERROR_ACCESS_DENIED || status == static_cast<DWORD>(E_ACCESSDENIED) ? "accessDenied" : std::move(error), status}; }
inline Result Status(HRESULT status)
{ return SUCCEEDED(status) ? Result{true, {}, 0} : Error(static_cast<DWORD>(status)); }
inline Snapshot Value(JsonValue value) { return {true, std::move(value), {}, 0, 0}; }
inline Snapshot Missing(std::string error = "notPresent") { Snapshot result; result.error = std::move(error); return result; }
template<class T> auto Await(T operation, const Cancellation& cancel)
{
    using winrt::Windows::Foundation::AsyncStatus;
    while (operation.Status() == AsyncStatus::Started)
    {
        if (cancel.Stop()) { operation.Cancel(); throw winrt::hresult_canceled(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return operation.GetResults();
}
std::shared_ptr<Backend> CreateAudioBackend();
std::shared_ptr<Backend> CreatePowerBackend();
std::shared_ptr<Backend> CreateBrightnessBackend();
std::shared_ptr<Backend> CreateWifiBackend();
std::shared_ptr<Backend> CreateBluetoothBackend();
std::shared_ptr<Backend> CreateMediaBackend();
}
