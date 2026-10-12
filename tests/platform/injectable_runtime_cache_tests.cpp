#include "platform/injectable_runtime_cache.h"
#include <fstream>
#include <iostream>
#include <thread>

namespace
{
void Write(const std::filesystem::path& file, const char* bytes)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream << bytes;
}
bool Leased(const std::filesystem::path& directory)
{
    const auto handle = CreateFileW((directory / L".owner.lock").c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
    CloseHandle(handle);
    return false;
}
}

int main()
{
    using snowdesktop::deployment::InjectableRuntimeCache;
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto root = std::filesystem::temp_directory_path() / (L"SnowDesktop.RuntimeCache-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto source = root / L"source" / L"Hook.dll";
    const auto cacheRoot = root / L"shared";
    std::filesystem::create_directories(source.parent_path());
    Write(source, "abc");
    check(InjectableRuntimeCache::Digest(source) ==
        L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "digest matches an independent SHA-256 known-answer vector");
    std::filesystem::path first;
    {
        InjectableRuntimeCache host(cacheRoot);
        first = host.Deploy(source);
        check(!first.empty() && first.filename() == L"Hook.dll" && first != source &&
            InjectableRuntimeCache::Digest(first) == InjectableRuntimeCache::Digest(source),
            "the copied DLL retains its name and exact bytes outside the build output");
        check(Leased(first.parent_path()), "an active host protects its directory from cleanup");
        {
            InjectableRuntimeCache second(cacheRoot);
            check(second.Deploy(source) == first && Leased(first.parent_path()),
                "overlapping hosts reuse the immutable copy while retaining independent leases");
        }
        check(Leased(first.parent_path()), "closing one host does not release another host's lease");
        Write(source, "changed runtime");
        const auto updated = host.Deploy(source);
        check(!updated.empty() && updated != first && std::filesystem::is_regular_file(first) &&
            InjectableRuntimeCache::Digest(updated) == InjectableRuntimeCache::Digest(source),
            "new DLL bytes select a new path without overwriting a retained old copy");
        Write(updated, "tampered runtime");
        check(host.Deploy(source).empty(), "a corrupted existing cache file is rejected without overwriting it");
        check(host.Deploy(root / L"missing.dll").empty(), "missing source does not create an injectable copy");
    }
    check(!Leased(first.parent_path()), "the final host releases the cleanup lease");
    Write(source, "abc");
    {
        InjectableRuntimeCache restarted(cacheRoot);
        check(restarted.Deploy(source) == first, "a restarted host retains the previously registered DLL path");
    }
    std::filesystem::path left, right;
    std::thread firstHost([&] { InjectableRuntimeCache host(root / L"concurrent"); left = host.Deploy(source); });
    std::thread secondHost([&] { InjectableRuntimeCache host(root / L"concurrent"); right = host.Deploy(source); });
    firstHost.join(); secondHost.join();
    check(!left.empty() && left == right && InjectableRuntimeCache::Digest(left) == InjectableRuntimeCache::Digest(source),
        "concurrent publishers converge on one complete immutable copy");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "isolated runtime cache fixture cleans up after all leases close");
    if (!failures) std::cout << "Injectable runtime cache checks passed\n";
    return failures ? 1 : 0;
}
