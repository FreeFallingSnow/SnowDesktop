#pragma once
#include "data/layout_storage.h"

namespace native_async_layout_save_cases
{
inline void Run()
{
    namespace cases = native_drag_commit_cases;
    using Access = NativeDragIntegrationAccess;
    cases::context = "asynchronous native layout persistence";
    cases::Profile profile;
    const auto file = profile.root / L"file.txt";
    { std::ofstream stream(file); stream << "unchanged source"; }
    { std::ofstream stream(profile.root / L"other.txt"); stream << "other source"; }
    auto& app = profile.Model(false, profile.root / L"layout");
    Access::PopulateModel(app, {file, profile.root / L"other.txt"}, 0);
    Access::UseSaveWindow(app);
    cases::Require(Access::Save(app), "save initial native layout");
    const auto before = cases::Read(Access::LayoutPath(app));
    Access::Items(app)[0].gridCell.column = 1;
    Access::RequestSave(app);
    Access::Items(app)[0].gridCell.column = 3;
    Access::RequestSave(app);
    cases::Require(Access::PendingSave(app) && Access::ScrollSavePending(app) &&
        cases::Read(Access::LayoutPath(app)) == before,
        "interactive requests retain unsaved state and leave the disk outside the release handler");
    Access::DueSaveTimer(app);
    Access::DispatchSaveTimer(app);
    cases::Require(Access::PendingSave(app), "queue acceptance alone does not claim successful persistence");
    Access::FlushSave(app);
    snowdesktop::layout_storage::Document saved;
    const auto savedColumn = [&] {
        const auto key = WideToUtf8(Access::Items(app)[0].layoutKey);
        for (const auto& item : saved.items) if (item.key == key) return item.column.value_or(-1);
        return -1;
    };
    cases::Require(snowdesktop::layout_storage::ParseDocument(cases::Read(Access::LayoutPath(app)), saved) &&
        savedColumn() == 3 && !Access::PendingSave(app),
        "production timer, writer and completion persist the final coalesced native model");
    Access::Items(app)[0].gridCell.column = 2;
    Access::RequestSave(app);
    Access::DueSaveTimer(app);
    Access::DispatchSaveTimer(app);
    Access::Items(app)[0].gridCell.column = 1;
    cases::Require(Access::Save(app), "explicit native save acts as a background persistence barrier");
    Access::DrainSave(app);
    cases::Require(snowdesktop::layout_storage::ParseDocument(cases::Read(Access::LayoutPath(app)), saved) &&
        savedColumn() == 1 && !Access::PendingSave(app),
        "old background snapshots and completion messages cannot replace newer synchronous native state");
    const auto beforeFailure = cases::Read(Access::LayoutPath(app));
    struct LockedLayout
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~LockedLayout() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } locked{CreateFileW(Access::LayoutPath(app).c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    cases::Require(locked.handle != INVALID_HANDLE_VALUE, "hold a real sharing violation against layout replacement");
    Access::Items(app)[0].gridCell.column = 0;
    Access::RequestSave(app);
    Access::DueSaveTimer(app);
    Access::DispatchSaveTimer(app);
    Access::FlushSave(app);
    cases::Require(Access::PendingSave(app) && cases::Read(Access::LayoutPath(app)) == beforeFailure &&
        Access::Items(app)[0].gridCell.column == 0,
        "failed real background replacement preserves both saved data and the pending in-memory edit");
    CloseHandle(locked.handle);
    locked.handle = INVALID_HANDLE_VALUE;
    Access::RequestSave(app);
    Access::DueSaveTimer(app);
    Access::DispatchSaveTimer(app);
    Access::FlushSave(app);
    cases::Require(!Access::PendingSave(app) &&
        snowdesktop::layout_storage::ParseDocument(cases::Read(Access::LayoutPath(app)), saved) && savedColumn() == 0,
        "a later request retries the retained model after the real filesystem obstruction is removed");
    cases::Require(cases::Read(file) == "unchanged source", "layout persistence never edits source file contents");
    std::cout << "native deferred layout timer, ordered writer and synchronous boundary passed\n";
}
}
