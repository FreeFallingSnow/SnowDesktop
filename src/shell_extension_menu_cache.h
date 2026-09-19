#pragma once
#include "shell_extension_menu.h"

namespace snowdesktop::shell_extensions
{
// A display snapshot, never a persisted Shell command/session. Exact selections
// are shared by the settings child and host; the four sample catalogues have
// separate keys. The directory argument lets tests use private temporary data.
class MenuSnapshotCache
{
  public:
    struct Ticket
    {
        settings_ipc::Bytes identity;
        std::wstring epoch;
        Request request;
        unsigned slot = 0;
        explicit operator bool() const { return !identity.empty(); }
    };
    static constexpr std::uint64_t LifetimeMs = 24 * 60 * 60 * 1000;
    static constexpr unsigned Slots = 36;
    explicit MenuSnapshotCache(std::filesystem::path directory);
    Ticket Capture(const Request &request) const;
    std::optional<Reply> Find(const Ticket &ticket, std::uint64_t now = Now());
    bool Store(const Ticket &ticket, const Reply &reply, std::uint64_t now = Now());
    void Invalidate();
    std::wstring Epoch() const;
    static std::uint64_t Now();
    const std::filesystem::path &Directory() const { return directory_; }

  private:
    struct MemoryRow
    {
        settings_ipc::Bytes identity;
        std::wstring epoch;
        std::uint64_t written = 0, fileStamp = 0;
        Reply reply;
    };
    std::filesystem::path directory_;
    std::map<unsigned, MemoryRow> rows_;
};
MenuSnapshotCache &SharedMenuCache();

// Labels and canonical verbs identify a path through the fresh menu. Tokens
// and positional indices deliberately do not participate in this identity.
using CommandReference = std::vector<std::tuple<std::string, std::string, std::wstring, bool>>;
CommandReference AppendReference(CommandReference path, const Entry &entry);
UINT ResolveCommand(const Reply &reply, const CommandReference &reference);
// Keeps a pending query on its original STA until it finishes. Called only
// after an explicit click; a cache hit alone can never invoke a command.
bool InvokeWhenReady(std::unique_ptr<Session> session, CommandReference reference, POINT position,
                     std::uint64_t generation);
void FinishQueryInBackground(std::unique_ptr<Session> session, MenuSnapshotCache::Ticket ticket,
                             std::uint64_t generation);
} // namespace snowdesktop::shell_extensions
