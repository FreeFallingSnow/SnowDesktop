#include "app.h"
#include "../menu_fluent_glyphs.h"

void DesktopApp::AppendWebsiteIconMenu(HMENU menu, const std::wstring& path)
{
    if (snowdesktop::website_icon::ReadUrl(path).empty()) return;
    AppendMenuW(menu, MF_STRING | (websiteIconPendingPath_.empty() ? 0 : MF_GRAYED),
        kContextFetchWebsiteIconCommand, websiteIconPendingPath_ == path
            ? _LW("app.website_icon.fetching") : _LW("app.website_icon.fetch"));
    SetMenuItemIcon(menu, kContextFetchWebsiteIconCommand,
        snowdesktop::menu_fluent_glyphs::kWorkshop, MenuIconFont::FluentRegular);
}

void DesktopApp::FetchWebsiteIcon(const std::wstring& path)
{
    if (exitRequested_ || !websiteIconPendingPath_.empty()) return;
    const auto shortcut = snowdesktop::website_icon::Capture(path);
    const HWND completionWindow = controlHwnd_ ? controlHwnd_ : hwnd_;
    if (!shortcut || !completionWindow || !IsWindow(completionWindow))
    {
        ShowBalloonNotification(_LW("app.website_icon.fetch"), _LW("app.website_icon.save_failed"));
        return;
    }
    const std::filesystem::path directory(GetDataSubdirectoryPath(L"website-icons"));
    websiteIconPendingPath_ = path;
    ShowBalloonNotification(_LW("app.website_icon.fetch"),
        shortcut->path.filename().wstring() + L"\n" + _LW("app.website_icon.fetching"));
    try
    {
        websiteIconWorker_ = std::jthread([this, shortcut = *shortcut, directory, completionWindow](std::stop_token stop) {
            WebsiteIconCompletion completion{shortcut, {}};
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try
            {
                if (SUCCEEDED(initialized)) completion.icon = snowdesktop::website_icon::Fetch(shortcut.url, directory, stop);
            }
            catch (...) { /* Deliver a failure without changing the shortcut. */ }
            if (SUCCEEDED(initialized)) CoUninitialize();
            {
                std::lock_guard lock(websiteIconMutex_);
                websiteIconCompletion_ = std::move(completion);
            }
            PostMessageW(completionWindow, kWebsiteIconReadyMessage, 0, 0);
        });
    }
    catch (...)
    {
        websiteIconPendingPath_.clear();
        ShowBalloonNotification(_LW("app.website_icon.fetch"), _LW("app.website_icon.fetch_failed"));
    }
}

void DesktopApp::OnWebsiteIconReady()
{
    std::optional<WebsiteIconCompletion> completion;
    {
        std::lock_guard lock(websiteIconMutex_);
        completion.swap(websiteIconCompletion_);
    }
    if (!completion) return;
    websiteIconPendingPath_.clear();
    const bool fetched = !completion->icon.empty();
    const bool applied = fetched && !exitRequested_ &&
        snowdesktop::website_icon::Apply(completion->shortcut, completion->icon);
    if (fetched && !applied) DeleteFileW(completion->icon.c_str());
    if (exitRequested_) return;
    if (applied)
    {
        ReloadItems(false);
        for (size_t i = 0; i < items_.size(); ++i)
            if (items_[i].largeIcon && _wcsicmp(items_[i].parsingName.c_str(), completion->shortcut.path.c_str()) == 0)
                RequestLargeIconAsset(i, true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    ShowBalloonNotification(_LW("app.website_icon.fetch"),
        completion->shortcut.path.filename().wstring() + L"\n" +
        (applied ? _LW("app.website_icon.updated") : fetched
            ? _LW("app.website_icon.save_failed") : _LW("app.website_icon.fetch_failed")));
}

void DesktopApp::StopWebsiteIconWorker()
{
    websiteIconWorker_.request_stop();
    if (websiteIconWorker_.joinable()) websiteIconWorker_.join();
    std::lock_guard lock(websiteIconMutex_);
    if (websiteIconCompletion_ && !websiteIconCompletion_->icon.empty())
        DeleteFileW(websiteIconCompletion_->icon.c_str());
    websiteIconCompletion_.reset();
    websiteIconPendingPath_.clear();
}
