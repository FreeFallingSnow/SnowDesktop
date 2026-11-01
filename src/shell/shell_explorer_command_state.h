#pragma once
#include <shobjidl.h>
#include <cstddef>
#include <vector>

namespace snowdesktop::shell_extensions
{
inline HRESULT ReadExplorerCommandState(IExplorerCommand* command, IShellItemArray* selection,
    bool allowSlow, EXPCMDSTATE& state)
{
    state = ECS_HIDDEN;
    const HRESULT result = command->GetState(selection, FALSE, &state);
    return result == E_PENDING && allowSlow ? command->GetState(selection, TRUE, &state) : result;
}

// Keep every command on its owning STA. Finish the fast pass before allowing
// a pending provider to block that apartment; publish only confirmed roots.
// A false read owns no partial command mappings and is retried exactly once.
template<class Candidate, class Read, class Publish>
void ReadExplorerCommandBatch(std::vector<Candidate>& candidates, Read&& read, Publish&& publish)
{
    std::vector<std::size_t> pending;
    for (std::size_t index = 0; index < candidates.size(); ++index)
        if (!read(candidates[index], false)) pending.push_back(index);
    if (pending.empty()) return;
    publish();
    for (const auto index : pending)
    {
        read(candidates[index], true);
        publish();
    }
}
}
