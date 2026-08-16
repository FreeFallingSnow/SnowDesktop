#pragma once

#include <filesystem>
#include <string>

namespace snowdesktop::widget_authoring
{

struct MigrationDraftReport
{
    bool ok = false;
    std::string stage;
    std::string error;
    std::filesystem::path source;
    std::filesystem::path output;
    std::string originalEntry;
    std::string draftEntry;

    std::string ToJson() const;
};

/** Create a new transactional v2 draft; source and existing output are untouched. */
MigrationDraftReport CreateV2MigrationDraft(
    const std::filesystem::path& source,
    const std::filesystem::path& output);

} // namespace snowdesktop::widget_authoring
