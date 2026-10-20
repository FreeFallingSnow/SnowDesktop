    static void PrepareRunningDock(DesktopApp& app, const std::vector<std::filesystem::path>& files)
    {
        PopulateModel(app, files, 0); SeedDock(app);
        app.dockUnpinnedRunningApps_.clear();
        // Resolve actual local shortcut/EXE identities synchronously, then
        // publish through the same cache as the background Shell provider.
        // Application-window discovery is supplied as the external snapshot.
        for (const auto& item : app.items_)
        {
            const auto key = DockItemWindowKey(item);
            const auto cacheKey = snowdesktop::dock_refresh_cache::SourceKey(key, item.parsingName);
            const auto cached = app.dockAppIdentityCache_.Read(cacheKey, snowdesktop::shell_icon_request::Stamp(item));
            app.dockAppIdentityCache_.Publish(cacheKey, cached.ticket, app.ReadDockAppIdentity(item.parsingName));
        }
    }
    static std::weak_ptr<void> QueueNativeRunningDockPin(DesktopApp& app,
        const std::filesystem::path& executable, bool matched, bool mapping, bool retired, bool explorer)
    {
        DockRunningAppInfo running;
        running.identityKey = L"fixture-running-application";
        running.title = L"Actual pin boundary";
        running.executablePath = app.ReadDockAppIdentity(executable.wstring()).executablePath;
        if (explorer)
        {
            std::array<wchar_t, 32768> directory{};
            const auto length = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (!length || length >= directory.size()) throw std::runtime_error("resolve Windows Explorer executable identity without launching it");
            running.executablePath = (std::filesystem::path(directory.data()) / L"explorer.exe").wstring();
        }
        running.presence.SetVisible(true, 0, false);
        if (!retired) app.dockUnpinnedRunningApps_.push_back(running);
        auto state = std::make_shared<DesktopApp::DockRunningDragState>();
        state->running = running; state->resolved = true; state->existingDesktopItem = matched; state->explorer = explorer;
        state->serial = ++app.dockRunningDragSerial_;
        state->application = std::make_shared<DesktopApp::DockRunningPinTarget>();
        auto nativePath = executable; nativePath.make_preferred();
        state->application->entry.absolutePidl.reset(ILCreateFromPathW(nativePath.c_str()));
        if (!state->application->entry.absolutePidl.get()) throw std::runtime_error("real running pin application target PIDL required");
        state->application->entry.parsingName = executable.wstring();
        state->application->entry.name = L"Resolved running app";
        state->application->identity = app.ReadDockAppIdentity(executable.wstring());
        if (explorer)
        {
            state->folder = state->application;
            state->folder->folderPath = executable.wstring();
            state->application.reset();
        }
        std::weak_ptr<void> finished = state;
        // CommitDockRunningDrag has already ended its input gesture before
        // this production queue boundary. Retain real lookup-result dispatch,
        // live membership checks, pin commit, file creation and storage.
        app.QueueDockRunningPin(std::move(state), {explorer ? snowdesktop::dock_running_drag::Area::Files
            : snowdesktop::dock_running_drag::Area::Fixed, 0}, mapping ? MK_CONTROL : 0, 0);
        return finished;
    }
