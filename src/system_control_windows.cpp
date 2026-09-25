#include "system_control_windows.h"

namespace snowdesktop::system_control
{
namespace
{
class WindowsBackend final : public Backend
{
    std::map<std::string, std::shared_ptr<Backend>, std::less<>> sources_{
        {"audio", windows::CreateAudioBackend()}, {"power", windows::CreatePowerBackend()},
        {"brightness", windows::CreateBrightnessBackend()}, {"wifi", windows::CreateWifiBackend()},
        {"bluetooth", windows::CreateBluetoothBackend()}, {"media", windows::CreateMediaBackend()}};
public:
    std::map<std::string, Snapshot> Sample(std::string_view source, const Cancellation& cancel) override
    { const auto found = sources_.find(source); return found == sources_.end() ? std::map<std::string, Snapshot>{} : found->second->Sample(source, cancel); }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (!ValidateRequest(request)) return {false, "invalidArguments", 0};
        const auto found = sources_.find(Source(request.name));
        return found == sources_.end() ? Result{false, "actionUnsupported", 0} : found->second->Execute(request, cancel);
    }
    void Release(std::string_view source) override
    { const auto found = sources_.find(source); if (found != sources_.end()) found->second->Release(source); }
};
}
std::shared_ptr<Backend> CreateWindowsBackend() { return std::make_shared<WindowsBackend>(); }
}
