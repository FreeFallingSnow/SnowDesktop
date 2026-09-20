#pragma once
#include "shell_extension_catalogue.h"
#include "shell_extension_menu_cache.h"

namespace snowdesktop::shell_extensions
{
enum class QueryPriority : int { Execute, Menu, Prewarm, Inspect };
struct MenuView
{
    std::optional<Reply> snapshot;
    bool pending = false;
    std::uint64_t revision = 0;
    unsigned contexts = 0;
    std::string error;
};
struct CatalogueView
{
    Catalogue catalogue;
    Request selection;
    MenuView menu;
    bool scanning = false;
};
// An injected query replaces only the supervised process boundary in scheduler
// tests. Scheduling, de-duplication, persistence and publication remain real.
struct QueryWork
{
    std::function<std::optional<Reply>()> poll;
    std::function<void(UINT, POINT)> invoke;
};
class MenuService
{
  public:
    using QueryFactory = std::function<QueryWork(const Request &)>;
    using CatalogueReader = std::function<Catalogue()>;
    explicit MenuService(std::filesystem::path directory = {}, QueryFactory factory = {}, CatalogueReader reader = {});
    ~MenuService();
    MenuService(const MenuService &) = delete;
    // All public methods are memory-only. The worker owns all disk I/O, target
    // checks, Shell sessions and the immutable snapshot publication boundary.
    MenuView View(const Request &request);
    void Query(const Request &request, QueryPriority priority = QueryPriority::Menu, bool force = false);
    void Prewarm(const Request &request);
    void Configure(Preferences preferences);
    void Manage(const Request &request);
    CatalogueView Inspect(const Request &request = {}, bool refresh = false);
    void Execute(const Request &request, CommandReference reference, POINT point, std::function<void(bool)> completed = {});
    void Invalidate(const Request &request);
    void Shutdown();
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
MenuService &SharedMenuService();
std::vector<Entry> VisibleSnapshot(const Preferences &, const Reply &, unsigned contexts);
}
namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::MenuView>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.snapshot, v.pending, v.revision, v.contexts, v.error); }
};
template <> struct Fields<shell_extensions::CatalogueView>
{
    template<class T> static auto Tie(T &v) { return std::tie(v.catalogue, v.selection, v.menu, v.scanning); }
};
}
