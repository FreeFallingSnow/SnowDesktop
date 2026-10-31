#pragma once
#include "shell/shell_extension_service.h"
#include <fstream>
#include <stdexcept>

namespace shell_service_fixture
{
// This adapter reads an in-memory test registry truth. It cannot call a full
// production catalogue reader: a missing requested ID stays missing.
inline void SeedMetadata(const std::filesystem::path &cache,
    const snowdesktop::shell_extensions::Catalogue &catalogue)
{
    std::filesystem::create_directories(cache);
    std::vector<std::pair<std::string, std::uint64_t>> menuProof;
    for (const auto &row : catalogue.rows) menuProof.emplace_back(row.id, row.menuRevision);
    const auto bytes = snowdesktop::settings_ipc::Pack(std::uint32_t(6), catalogue, menuProof);
    std::ofstream output(cache / L"catalogue.bin", std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("write isolated registration metadata");
}
inline snowdesktop::shell_extensions::Catalogue Sources(
    const snowdesktop::shell_extensions::Catalogue &truth,
    const std::vector<snowdesktop::shell_extensions::Registration> &seeds)
{
    snowdesktop::shell_extensions::Catalogue result;
    result.revision = truth.revision;
    for (const auto &seed : seeds)
        for (const auto &row : truth.rows)
            if (row.id == seed.id) { result.rows.push_back(row); break; }
    return result;
}
}
