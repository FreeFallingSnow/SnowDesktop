#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>

namespace snowdesktop::test
{
// Own only a directory created exclusively by this fixture. Never remove a
// pre-existing path to make a test start successfully.
class TemporaryDirectory
{
public:
    TemporaryDirectory() : path(Create()) {}
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    const std::filesystem::path path;

private:
    static std::filesystem::path Create()
    {
        const auto root = std::filesystem::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            auto candidate = root / ("SnowDesktopTests-" +
                std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error))
                return candidate;
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error(
                    "create test directory", candidate, error);
        }
        throw std::runtime_error("could not exclusively create a test directory");
    }
};
}
