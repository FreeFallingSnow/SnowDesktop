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
        constexpr unsigned versions = 24;
        for (unsigned version = 0; version < versions; ++version)
        {
            Write(source, ("retained-" + std::to_string(version)).c_str());
            hosts.push_back(std::make_unique<InjectableRuntimeCache>(root / L"retained"));
            paths.push_back(hosts.back()->Deploy(source));
            check(!paths.back().empty(), "24 simultaneously retained versions obtain distinct reusable slots");
            if (version >= InjectableRuntimeCache::kInitialSlotCount)
                check(hosts.back()->Expanded(), "an occupied initial pool expands instead of rejecting another version");
        }
        Write(source, "one too many");
        InjectableRuntimeCache next(root / L"retained");
        const auto extra = next.Deploy(source);
        check(!extra.empty() && extra.parent_path().filename() == L"slot-24" && next.Expanded(),
            "the pool expands beyond 24 busy versions without a hard slot-count failure");
        check(next.Deploy(source) == extra && !next.Expanded(),
            "repeated requests for the new bytes reuse the overflow slot without further growth");
        check(InjectableRuntimeCache::Digest(paths.front()) != InjectableRuntimeCache::Digest(source),
            "pool exhaustion preserves the previously leased bytes");
        hosts.clear();
        Write(source, "new bytes after old hosts close");
        check(next.Deploy(source) == paths.front(), "closing owners makes the original slot reusable");
    }
    {
        const auto pool = root / L"concurrent-expansion";
        std::vector<std::unique_ptr<InjectableRuntimeCache>> owners;
        for (unsigned i = 0; i < InjectableRuntimeCache::kInitialSlotCount; ++i)
        {
            Write(source, ("occupied-" + std::to_string(i)).c_str());
            owners.push_back(std::make_unique<InjectableRuntimeCache>(pool));
            check(!owners.back()->Deploy(source).empty(), "retain the initial pool before competing expansion");
        }
        Write(source, "concurrent overflow");
        const auto directory = pool / L"slots-v1" / L"Hook.dll";
        const auto malformed = directory / L"slot-04";
        std::filesystem::create_directories(malformed);
        Write(malformed / L"Hook.dll", "concurrent overflow");
        std::filesystem::create_directories(directory / L"slot-42949672960");
        std::filesystem::path a, b;
        std::thread publisherA([&] { InjectableRuntimeCache host(pool); a = host.Deploy(source); });
        std::thread publisherB([&] { InjectableRuntimeCache host(pool); b = host.Deploy(source); });
        publisherA.join(); publisherB.join();
        check(!a.empty() && a == b && a.parent_path().filename() == L"slot-4",
            "competing publishers allocate one canonical fifth slot and ignore malformed or overflowing numbers");
        InjectableRuntimeCache later(pool);
        check(later.Deploy(source) == a && !later.Expanded(),
            "a new host discovers and reuses the expanded pool");
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
    {
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
        const auto imageSource = source.parent_path() / L"Resident.dll";
        const auto pool = root / L"resident-pool";
        std::vector<HMODULE> images;
        std::vector<std::filesystem::path> paths;
        std::vector<std::wstring> originalHashes;
        constexpr unsigned mappedVersions = 12;
        for (unsigned version = 0; version < mappedVersions; ++version)
        {
            check(CopyFileW((std::filesystem::path(system) / L"version.dll").c_str(),
                imageSource.c_str(), FALSE), "stage real images for full mapped pool regression");
            { std::ofstream overlay(imageSource, std::ios::binary | std::ios::app); overlay.put(static_cast<char>(version)); }
            InjectableRuntimeCache host(pool);
            paths.push_back(host.Deploy(imageSource));
            originalHashes.push_back(InjectableRuntimeCache::Digest(paths.back()));
            const HMODULE image = paths.back().empty() ? nullptr :
                LoadLibraryExW(paths.back().c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
            check(image != nullptr, "12 real DLL images remain mapped after their originating hosts exit");
            images.push_back(image);
        }
        { std::ofstream overlay(imageSource, std::ios::binary | std::ios::app); overlay.put('x'); }
        InjectableRuntimeCache next(pool);
        const auto expanded = next.Deploy(imageSource);
        check(!expanded.empty() && expanded.parent_path().filename() == L"slot-12" && next.Expanded(),
            "a thirteenth image deploys past a pool of 12 mapped DLLs without touching any mapped bytes");
        for (std::size_t i = 0; i < paths.size(); ++i)
            check(InjectableRuntimeCache::Digest(paths[i]) == originalHashes[i],
                "expansion preserves every previously mapped DLL byte for byte");
        const HMODULE newest = expanded.empty() ? nullptr : LoadLibraryExW(expanded.c_str(), nullptr,
            DONT_RESOLVE_DLL_REFERENCES);
        check(newest != nullptr, "the expanded slot contains a valid loadable new DLL");
        const auto resident = InjectableRuntimeCache::ResidentModulePath(pool, L"Resident.dll",
            GetCurrentProcessId(), reinterpret_cast<std::uintptr_t>(newest));
        check(!resident.empty() && resident == expanded &&
            InjectableRuntimeCache::Digest(resident) == InjectableRuntimeCache::Digest(imageSource),
            "resident lookup recognizes the thirteenth slot rather than scanning only four");
        check(InjectableRuntimeCache::ResidentModulePath(root / L"foreign-pool", L"Resident.dll",
            GetCurrentProcessId(), reinterpret_cast<std::uintptr_t>(images.back())).empty(),
            "resident lookup cannot adopt modules outside this deployment's bounded pool");
        check(InjectableRuntimeCache::ResidentModulePath(pool, L"Other.dll",
            GetCurrentProcessId(), reinterpret_cast<std::uintptr_t>(images.back())).empty(),
            "resident lookup rejects a different DLL identity");
        for (HMODULE image : images) if (image) FreeLibrary(image);
        if (newest) FreeLibrary(newest);
        { std::ofstream overlay(imageSource, std::ios::binary | std::ios::app); overlay.put('y'); }
        check(next.Deploy(imageSource) == paths.front(),
            "release of mapped images allows a later build to reuse the original path");
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
    check(!error, "isolated runtime cache fixture cleans up after all leases close");
    if (!failures) std::cout << "Injectable runtime cache checks passed\n";
    return failures ? 1 : 0;
}
