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
        const auto recovered = host.Deploy(source);
        check(!recovered.empty() && recovered != updated && InjectableRuntimeCache::Digest(recovered) ==
                InjectableRuntimeCache::Digest(source),
            "a corrupted leased slot is not returned or overwritten; an available slot receives correct bytes");
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

    std::filesystem::path stable;
    for (unsigned version = 0; version < 32; ++version)
    {
        Write(source, ("version-" + std::to_string(version)).c_str());
        InjectableRuntimeCache host(root / L"rebuilds");
        const auto deployed = host.Deploy(source);
        if (version == 0) stable = deployed;
        check(!deployed.empty() && deployed == stable && InjectableRuntimeCache::Digest(deployed) ==
                InjectableRuntimeCache::Digest(source),
            "successive rebuilds reuse the inactive path instead of minting content-addressed paths");
    }
    {
        std::vector<std::unique_ptr<InjectableRuntimeCache>> hosts;
        std::vector<std::filesystem::path> paths;
        for (unsigned version = 0; version < InjectableRuntimeCache::kSlotCount; ++version)
        {
            Write(source, ("retained-" + std::to_string(version)).c_str());
            hosts.push_back(std::make_unique<InjectableRuntimeCache>(root / L"retained"));
            paths.push_back(hosts.back()->Deploy(source));
            check(!paths.back().empty(), "each retained version obtains an available bounded slot");
        }
        Write(source, "one too many");
        InjectableRuntimeCache next(root / L"retained");
        check(next.Deploy(source).empty() && next.LastError() == ERROR_BUSY,
            "all retained slots refuse another version instead of falling back to an unbounded path");
        check(InjectableRuntimeCache::Digest(paths.front()) != InjectableRuntimeCache::Digest(source),
            "pool exhaustion preserves the previously leased bytes");
        hosts.clear();
        check(next.Deploy(source) == paths.front(), "closing owners makes the original slot reusable");
    }
    {
        InjectableRuntimeCache host(root / L"isolated");
        const auto selected = host.Deploy(source);
        const auto isolated = host.Deploy(source, selected);
        check(!isolated.empty() && isolated != selected && InjectableRuntimeCache::Digest(isolated) ==
                InjectableRuntimeCache::Digest(selected),
            "a retired same-content instance uses another bounded path without a per-host directory");
        check(host.Deploy(source, selected) == isolated,
            "repeated isolated requests reuse the same alternative slot");
    }
    {
        const auto imageSource = root / L"source" / L"MappedFixture.dll";
        wchar_t system[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        check(length && length < MAX_PATH && CopyFileW((std::filesystem::path(system) / L"version.dll").c_str(),
                imageSource.c_str(), TRUE), "copy a real DLL into the isolated mapped-image fixture");
        std::filesystem::path mapped;
        {
            InjectableRuntimeCache host(root / L"mapped");
            mapped = host.Deploy(imageSource);
        }
        const auto original = InjectableRuntimeCache::Digest(mapped);
        const HMODULE module = mapped.empty() ? nullptr : LoadLibraryExW(mapped.c_str(), nullptr,
            DONT_RESOLVE_DLL_REFERENCES);
        check(module != nullptr, "map the isolated DLL after its originating host closes");
        Write(imageSource, "replacement after host exit");
        {
            InjectableRuntimeCache host(root / L"mapped");
            const auto next = host.Deploy(imageSource);
            check(!next.empty() && next != mapped && InjectableRuntimeCache::Digest(mapped) == original,
                "mapped images remain immutable without a host lease; deployment uses another slot");
        }
        if (module) FreeLibrary(module);
        InjectableRuntimeCache released(root / L"mapped");
        Write(imageSource, "replacement after image unload");
        check(released.Deploy(imageSource) == mapped,
            "the same registered path becomes reusable only after the image unloads");
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "isolated runtime cache fixture cleans up after all leases close");
    if (!failures) std::cout << "Injectable runtime cache checks passed\n";
    return failures ? 1 : 0;
}
