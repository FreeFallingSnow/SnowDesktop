#include "widgets/lua_logical_slot.h"
#include "core/item.h"
#include "widget/runtime/widget_logical_slot.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace
{
void Require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

struct TemporaryDirectory
{
    std::filesystem::path parent, path;
    TemporaryDirectory()
    {
        wchar_t temp[MAX_PATH]{}, name[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"snl", 0, name),
            "allocate isolated Lua ingress files");
        parent = std::filesystem::canonical(temp);
        path = name;
        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        path = std::filesystem::canonical(path);
        Require(path.parent_path() == parent, "Lua file fixture remains in its allocated temporary directory");
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        if (!error && resolved.parent_path() == parent && resolved.filename().wstring().starts_with(L"snl"))
            std::filesystem::remove_all(resolved, error);
    }
};

class FileItem final : public Item
{
public:
    explicit FileItem(std::filesystem::path path) : path_(std::move(path)) {}
    std::wstring GetTitle() const override { return path_.filename().wstring(); }
    std::wstring GetPath() const override { return path_.wstring(); }
    HBITMAP GetIconBitmap() const override { return nullptr; }
    RECT GetBounds() const override { return {}; }
    void SetBounds(RECT) override {}
    bool IsSelected() const override { return false; }
    void SetSelected(bool) override {}
    Container* GetContainer() const override { return nullptr; }
    void Draw(ID2D1DeviceContext*, RECT, int) override {}
    Microsoft::WRL::ComPtr<IDataObject> CreateDataObject() override { return {}; }
private:
    std::filesystem::path path_;
};

std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
}

// A06: the actual native Lua container enforces single-item ingress even when
// the real logical model has capacity for both files. Only the filesystem item
// wrapper and engine surface provider are substituted; successful controls use
// the production model's Bind/Export and real unchanged source files.
void RunLuaFileIngressMatrix()
{
    using namespace snowdesktop::widget_runtime;
    TemporaryDirectory temporary;
    const auto a = temporary.path / L"a.txt", b = temporary.path / L"b.txt";
    std::ofstream(a) << "alpha"; std::ofstream(b) << "beta";
    FileItem first(a), second(b);
    for (LogicalSlotKind kind : {LogicalSlotKind::Binding, LogicalSlotKind::Collection})
    {
        LogicalSlotModel model;
        LogicalSlotDeclaration declaration;
        declaration.kind = kind; declaration.accepts = {"filesystem.reference"};
        declaration.capacity = kind == LogicalSlotKind::Binding ? 1 : 8;
        std::string error;
        Require(model.Configure({{"files", declaration}}, error), "configure the real logical-slot model");
        auto surface = [&]() -> std::optional<LogicalSlotHostSurface> {
            const auto* state = model.Find("files");
            if (!state) return std::nullopt;
            LogicalSlotHostSurface value;
            value.widgetId = L"lua-file-target"; value.slotId = "files";
            value.kind = kind; value.revision = state->revision;
            value.capacity = state->capacity; value.itemCount = state->items.size();
            value.accepts = declaration.accepts; value.bounds = {0, 0, 100, 100};
            return value;
        };
        int commits = 0;
        LuaLogicalSlotContainer container(L"lua-file-target", "files", surface,
            [&](const std::vector<Item*>& sources, size_t index) {
                ++commits;
                bool succeeded = true;
                for (const auto* item : sources)
                {
                    const auto utf8 = std::filesystem::path(item->GetPath()).u8string();
                    LogicalSlotItem candidate;
                    candidate.kind = "filesystem.reference"; candidate.title = "file";
                    candidate.source = "native-drop-fixture"; candidate.type = "file";
                    candidate.target.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
                    LogicalSlotChange change;
                    succeeded &= model.Bind("files", std::move(candidate), change, error, index++);
                }
                return succeeded;
            });
        std::unordered_map<std::string, std::string> before, after;
        model.Export(before, "lua-file-target");
        const auto revision = model.Find("files")->revision;
        Require(!container.AcceptsDragPayload(snowdesktop::slot_contract::DragPayloadKind::ExternalFile, 2),
            "native Lua admission rejects two external files even when the collection has capacity");
        Require(!container.CommitItems({&first, &second}, nullptr, HitRegion::Empty),
            "A06: the native final commit also rejects ambiguous multiple-file ingress");
        model.Export(after, "lua-file-target");
        Require(commits == 0 && model.Find("files")->revision == revision && before == after &&
            model.Find("files")->items.empty() && Bytes(a) == "alpha" && Bytes(b) == "beta",
            "native multi-file rejection leaves real model revision, storage, members and original files unchanged");
        Require(container.AcceptsDragPayload(snowdesktop::slot_contract::DragPayloadKind::ExternalFile, 1) &&
            container.CommitItems({&first}, nullptr, HitRegion::Empty) && commits == 1 &&
            model.Find("files")->revision == revision + 1 && model.Find("files")->items.size() == 1 &&
            Bytes(a) == "alpha" && Bytes(b) == "beta",
            "single-file control binds a real logical reference once and preserves both physical source files");
    }
    std::cout << "Lua native file-ingress model and filesystem cases passed\n";
}
