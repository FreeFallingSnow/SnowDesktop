#pragma once
#include "diagnostics/debug_profile.h"
#include "../test_temporary_directory.h"
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace behavior
{
namespace fs = std::filesystem;
inline std::string scene;
inline void Require(bool ok, const std::string& message)
{ if (!ok) throw std::runtime_error(scene + ": " + message); }
inline void Write(const fs::path& file, std::string_view bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary); stream << bytes;
    Require(static_cast<bool>(stream), "write isolated input");
}
inline std::string Read(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    Require(static_cast<bool>(stream), "read physical output");
    return {std::istreambuf_iterator<char>(stream), {}};
}
// A private data profile is sufficient for storage/controller contracts.
// Host composition and its asynchronous work are opt-in below.
struct Profile
{
    snowdesktop::test::TemporaryDirectory temporary;
    snowdesktop::debug_profile::Session previousSession = snowdesktop::debug_profile::Current();
    // Shell enumeration publishes long/canonical paths as item identities.
    // TMP can contain an 8.3 alias even when .NET displays its long spelling.
    fs::path root = fs::weakly_canonical(temporary.path), desktop;
    Profile()
    {
        namespace debug = snowdesktop::debug_profile;
        const auto paths = debug::ResolvePaths(root / L"normal-data");
        debug::Configuration config; config.enabled = true; config.desktop = paths.defaultDesktop;
        std::string error;
        if (!debug::Write(paths, config, error) || !debug::Initialize(paths.normalData, {}, error))
        {
            debug::runtimeSession = std::move(previousSession);
            throw std::runtime_error(scene + ": isolate profile: " + error);
        }
        desktop = debug::Current().configuration.desktop;
    }
    ~Profile() { snowdesktop::debug_profile::runtimeSession = std::move(previousSession); }
};
struct Apartment
{
    Apartment() { Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize STA"); }
    ~Apartment() { CoUninitialize(); }
};
struct WindowOwner
{
    HWND handle = nullptr;
    ~WindowOwner() { if (handle) DestroyWindow(handle); }
};
}
