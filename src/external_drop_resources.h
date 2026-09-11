#pragma once

#include "external_drop_content.h"
#include "shell_file_operation_worker.h"
#include <algorithm>

namespace snowdesktop::external_drop_content
{
// The same cleanup runs after success, partial failure and cancelled reads.
// A batch-level failure never invalidates a successfully created reference.
inline void Cleanup(const Content& content, const ShellFileOperationResult& result)
{
    if (!content.owned) return;
    for (const auto& path : content.paths)
    {
        const bool referenced = std::any_of(result.outputs.begin(), result.outputs.end(),
            [&](const auto& output) {
                return output.referencesSource &&
                    _wcsicmp(output.source.c_str(), path.c_str()) == 0;
            });
        if (!referenced) DeleteFileW(path.c_str());
    }
}
}
