// Kept inside ExportThemeSurfacePreview: this branch borrows DesktopApp's
// composition state only in the existing private offscreen worker. It is never
// invoked by the live host's IPC handler or against its user data graph.
if (request.component == "demo-scene")
{
    result.stage = "demo-scene.render";
    try
    {
        const auto scene = demo::ParseScene(demo::ReadJson(request.outputDirectory / L"scene.json"));
        if (scene.width != request.canvasWidth || scene.height != request.canvasHeight ||
            scene.locale != request.locale || scene.appearance != request.appearance)
            throw std::runtime_error("demo_worker_configuration_mismatch");
        const auto appearance = personalizationSettings_;
        const bool light = appearance.contentTheme == 1;
        const auto root = request.outputDirectory;
        items_.clear(); widgets_.clear(); containers_.clear(); gridPages_.clear(); dockEntries_.clear();
        generalSettings_ = {}; generalSettings_.demoModeEnabled = false;
        iconBeautifySettings_ = {}; iconBeautifySettings_.enabled = false;
        virtualLeft_ = virtualTop_ = 0; virtualWidth_ = scene.width; virtualHeight_ = scene.height;
        layoutWorkArea_ = {0, 0, scene.width, scene.height};
        GridPage page; page.id = L"isolated-demo";
        page.bounds = page.workArea = page.visualWorkArea = layoutWorkArea_;
        page.dpiX = page.dpiY = 96; page.columns = 32; page.rows = 24;
        page.cellWidth = page.itemPitchWidth = kCellWidth; page.cellHeight = page.itemPitchHeight = kMinCellHeight;
        gridPages_.push_back(page);
        auto fixture = std::make_shared<WidgetPreviewScene>();
        for (const auto& item : scene.items)
        {
            WidgetPreviewItem sample;
            sample.key = L"__demo_item_" + demo::Wide(item.id); sample.title = demo::Wide(item.title);
            sample.glyph = demo::Wide(item.glyph); sample.categoryId = demo::Wide(item.category);
            sample.dateGroup = demo::Wide(item.dateGroup); sample.directory = item.directory; sample.backgroundRgb = item.color;
            fixture->AddItem(std::move(sample));
        }
        fixture->PreparePlaceholderModels(64, light);
        // Dock and popup renderers consume the host model. Give this worker its
        // own copies of the fixed icons; nothing points to a real shell object.
        for (const auto& sample : fixture->Items())
        {
            const auto* source = fixture->FindDesktopItem(sample.key);
            if (!source || !source->iconBitmap) throw std::runtime_error("demo_fixture_icon_unavailable");
            DesktopItem item; item.name = sample.title; item.layoutKey = item.parsingName = sample.key;
            item.iconBitmap = CopyBitmapToAlphaDib(source->iconBitmap, item.iconBitmapSize);
            if (!item.iconBitmap) throw std::runtime_error("demo_fixture_icon_copy_failed");
            item.iconState = IconState::FullQuality; items_.push_back(std::move(item));
        }
        std::unordered_map<std::string, std::size_t> indices;
        widgets_.reserve(scene.surfaces.size());
        for (const auto& surface : scene.surfaces)
        {
            if (surface.kind == "dock" || surface.kind == "status-bar" || surface.kind == "navigation") continue;
            DesktopWidget widget; widget.id = L"__demo_surface_" + demo::Wide(surface.id);
            widget.type = surface.kind == "files" ? DesktopWidgetType::FileCategories :
                surface.kind == "mapping" ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection;
            widget.title = demo::Wide(surface.title); widget.bounds = surface.bounds; widget.gridCell.pageId = page.id;
            widget.cellScale = 1; widget.gridSpan = {std::max(2, static_cast<int>((surface.bounds.right - surface.bounds.left) / kCellWidth)),
                std::max(2, static_cast<int>((surface.bounds.bottom - surface.bounds.top) / kMinCellHeight))};
            widget.showTitle = true; widget.showSearchBox = surface.showSearch; widget.showFileCategories = surface.showCategories;
            widget.listMode = surface.listMode; widget.scrollContainerMode = true; widget.activeCategoryId = L"all";
            for (const auto& id : surface.items)
            {
                const auto key = L"__demo_item_" + demo::Wide(id); widget.itemKeys.push_back(key);
                if (widget.type == DesktopWidgetType::FolderMapping)
                    if (const auto* entry = fixture->FindFolderEntry(key)) widget.folderEntries.push_back(*entry);
            }
            indices.emplace(surface.id, widgets_.size()); widgets_.push_back(std::move(widget));
        }
        std::unordered_map<std::string, WidgetContainer*> views;
        for (const auto& [id, index] : indices)
        {
            auto widget = CreateWidget(&widgets_[index], this);
            auto* container = dynamic_cast<WidgetContainer*>(widget.get());
            if (!container) throw std::runtime_error("demo_native_widget_unavailable");
            views.emplace(id, container); widget.release(); containers_.emplace_back(container);
        }
        struct FixtureCleanup
        {
            DesktopApp* app; WidgetPreviewScene* fixture;
            ~FixtureCleanup()
            {
                app->containers_.clear(); app->brushCache_.clear(); app->brushCacheContext_ = nullptr;
                for (const auto& sample : fixture->Items())
                {
                    if (const auto* item = fixture->FindDesktopItem(sample.key)) app->EraseD2DIconCacheForBitmap(item->iconBitmap);
                    if (const auto* item = fixture->FindFolderEntry(sample.key)) app->EraseD2DIconCacheForBitmap(item->iconBitmap);
                }
            }
        } cleanup{this, fixture.get()};
        RefreshDesktopItemIndexCache(); EnsureNavTabOrder();
        quickNavigationPreview_ = true; quickNavigationAppsIndexed_ = true;
        quickNavDpiScale_ = 1; quickNavAppearance_ = appearance;
        quickNavLightTheme_ = light; quickNavGlassTheme_ = appearance.glassEnabled; quickNavBlurRadius_ = appearance.glassBlurRadius;
        navigationSettings_ = {}; EnsureQuickNavTextFormats();
        dockSettings_ = {}; dockSettings_.followComponentAppearance = false;
        dockSettings_.appearancePreset = kAppearancePresetCustom; dockSettings_.customAppearance = appearance;
        dockSettings_.position = DockPosition::Bottom; dockSettings_.thicknessScale = .75f;
        const auto wallpaper = widget_preview::GenerateWallpaper(scene.width, scene.height, light);
        if (wallpaper.pixels.size() != static_cast<std::size_t>(scene.width) * scene.height) throw std::runtime_error("demo_wallpaper_failed");
        ComPtr<ID2D1DeviceContext> context; require(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
        const auto size = D2D1::SizeU(scene.width, scene.height);
        const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1Bitmap1> target, readback;
        require(context->CreateBitmap(size, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96), &target));
        require(context->CreateBitmap(size, nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96), &readback));
        ComPtr<ID2D1Bitmap> background; require(context->CreateBitmap(size, wallpaper.pixels.data(), scene.width * 4, D2D1::BitmapProperties(format), &background));
        context->SetDpi(96, 96); context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto material = [&](RECT rect, float radius) {
            if (appearance.glassEnabled && !widget_preview::DrawStage(context.Get(), rect,
                {light, true, appearance.glassBlurRadius, radius}, {scene.width, scene.height, rect.left, rect.top}, &wallpaper))
                throw std::runtime_error("demo_material_failed");
        };
        const auto mapRect = [](RECT from, RECT to) {
            const float sx = static_cast<float>(to.right - to.left) / (from.right - from.left);
            const float sy = static_cast<float>(to.bottom - to.top) / (from.bottom - from.top);
            return D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(to.left - from.left * sx, to.top - from.top * sy);
        };
        const auto videoPartial = root / L"video.partial.mp4";
        video_writer::Mp4 writer(videoPartial, scene.width, scene.height, scene.fps);
        const auto frameCount = scene.FrameCount(); const unsigned posterFrame = frameCount * 2 / 3;
        std::set<unsigned> samples{0, frameCount / 5, frameCount * 2 / 5, frameCount * 3 / 5, frameCount * 4 / 5, frameCount - 1};
        auto posters = control::Array();
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(scene.width) * scene.height);
        for (unsigned frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            const auto frame = demo::Evaluate(scene, scene.FrameMilliseconds(frameIndex));
            lastMousePoint_ = {static_cast<LONG>(frame.cursor.x), static_cast<LONG>(frame.cursor.y)};
            context->SetTarget(target.Get()); context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->BeginDraw(); context->DrawBitmap(background.Get());
            brushCache_.clear(); brushCacheContext_ = context.Get();
            for (const auto& presentation : frame.surfaces)
            {
                if (!presentation.visible || presentation.scale <= 0) continue;
                const auto& surface = presentation.surface; const auto rect = surface.bounds;
                const auto transform = D2D1::Matrix3x2F::Scale(presentation.scale, presentation.scale,
                    D2D1::Point2F((rect.left + rect.right) / 2.f, (rect.top + rect.bottom) / 2.f));
                context->SetTransform(transform);
                if (surface.kind == "status-bar")
                {
                    material(rect, 0); const auto fill = StatusBarFillAppearance(appearance);
                    DrawWidgetPanelBackground(context.Get(), rect, 0,
                        D2D1::ColorF(fill.widgetBgR, fill.widgetBgG, fill.widgetBgB, fill.widgetAlpha), D2D1::ColorF(0, 0.f), false, 0, &fill, false, 0, 1);
                    StatusBarSnapshot data; data.clock = demo::Wide(scene.clock); data.inputMethod.label = scene.locale.starts_with("zh") ? L"中" : L"EN";
                    widget_runtime::WidgetNetworkStatusDataSnapshot network; network.available = true; network.connectivity = "internet"; network.transport = "ethernet"; data.network = network;
                    widget_runtime::WidgetAudioOutputVolumeDataSnapshot audio; audio.available = true; audio.volume = .42; data.audio = audio;
                    widget_runtime::WidgetPowerDataSnapshot power; power.available = true; power.batteryPercent = 73; data.power = power;
                    StatusBarSettings settings;
                    auto entries = BuildStatusBarItems(settings, data);
                    context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(rect.left), static_cast<float>(rect.top)) * transform);
                    require(DrawStatusBarContent(context.Get(), dwriteFactory_.Get(), entries, rect.right - rect.left, rect.bottom - rect.top, 1, appearance, {}, {}, false, 0, false));
                }
                else if (surface.kind == "dock")
                {
                    std::vector<DockEntry> entries;
                    for (const auto& id : surface.items) { DockEntry entry; entry.type = DockEntryType::DesktopItem; entry.reference = L"__demo_item_" + demo::Wide(id); entries.push_back(std::move(entry)); }
                    DockContainer dock(this, &entries, rect);
                    const auto bounds = dock.GetBounds(); material(bounds, appearance.cornerRadius);
                    dock.DrawChrome(context.Get(), lastMousePoint_); dock.DrawContents(context.Get());
                }
                else if (surface.kind == "navigation")
                {
                    quickNavigationOpen_ = true; quickNavigationCollapsed_ = false;
                    quickNavigationSearchText_ = quickNavigationEffectiveSearchText_ = demo::Wide(surface.search);
                    quickNavigationOpenPoint_ = {(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
                    quickNavigationRect_ = GetQuickNavigationRect(); quickNavigationHostRect_ = quickNavigationRect_;
                    quickNavigationLastMousePoint_ = lastMousePoint_; UpdateQuickNavTabWidths();
                    material(rect, appearance.cornerRadius);
                    context->SetTransform(mapRect(quickNavigationRect_, rect) * transform); DrawQuickNavigationSurface(context.Get());
                    quickNavigationOpen_ = false;
                }
                else
                {
                    auto& widget = widgets_[indices.at(surface.id)]; widget.bounds = rect;
                    widget.listMode = surface.listMode; widget.activeCategoryId = demo::Wide(surface.category);
                    widget.gridSpan = {std::max(2, static_cast<int>((rect.right - rect.left) / kCellWidth)), std::max(2, static_cast<int>((rect.bottom - rect.top) / kMinCellHeight))};
                    if (surface.kind == "popup")
                    {
                        popupWidgetIndex_ = indices.at(surface.id); popupHasAnchor_ = popupAnchoredToDock_ = false;
                        collectionPopupAppearance_ = appearance; collectionPopupLightTheme_ = light; collectionPopupGlassTheme_ = appearance.glassEnabled;
                        const auto native = GetCollectionPopupRect(widget); material(rect, 18.f);
                        context->SetTransform(mapRect(native, rect) * transform); DrawCollectionPopup(context.Get(), false);
                        popupWidgetIndex_ = static_cast<std::size_t>(-1);
                    }
                    else
                    {
                        WidgetRenderOptions options; options.previewScene = fixture.get(); options.frame = rect; options.pointer = lastMousePoint_;
                        options.dpi = 96; options.interactive = true; options.registerBackdrop = false;
                        auto* view = views.at(surface.id); view->SetRenderOptions(&options);
                        if (auto* scrolling = dynamic_cast<ScrollingItemWidget*>(view)) scrolling->SetSearchText(demo::Wide(surface.search));
                        material(rect, appearance.cornerRadius);
                        view->InvalidateSlots(); view->DrawPreview(context.Get(), rect, options); view->SetRenderOptions(nullptr);
                    }
                }
                context->SetTransform(D2D1::Matrix3x2F::Identity());
            }
            require(context->EndDraw()); context->SetTarget(nullptr); brushCache_.clear(); brushCacheContext_ = nullptr;
            require(readback->CopyFromBitmap(nullptr, target.Get(), nullptr));
            D2D1_MAPPED_RECT mapped{}; require(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            for (int y = 0; y < scene.height; ++y) std::memcpy(pixels.data() + static_cast<std::size_t>(y) * scene.width,
                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(scene.width) * 4);
            readback->Unmap();
            demo_cursor::Draw({reinterpret_cast<std::uint8_t*>(pixels.data()), pixels.size() * 4}, scene.width, scene.height, scene.width * 4, frame.cursor);
            writer.Write(pixels);
            if (samples.contains(frameIndex) || frameIndex == posterFrame)
            {
                const auto name = L"frame-" + std::to_wstring(frameIndex) + L".png";
                std::string error;
                if (!preview_png::Save(root / name, scene.width, scene.height, pixels, error)) throw std::runtime_error("demo_png_failed: " + error);
                posters.array.push_back(control::Object({{"file", control::String(demo::Utf8(name))}, {"frame", control::Number(frameIndex)},
                    {"timeMs", control::Number(static_cast<double>(scene.FrameMilliseconds(frameIndex)))}, {"sha256", control::String(demo::Sha256(root / name))}}));
            }
            if (frameIndex == posterFrame)
            {
                std::string error;
                if (!preview_png::Save(root / L"poster.png", scene.width, scene.height, pixels, error)) throw std::runtime_error("demo_poster_failed: " + error);
            }
            if (frameIndex % 10 == 0 || frameIndex + 1 == frameCount)
                demo::WriteJson(root / L"progress.json", control::Object({{"completedFrames", control::Number(frameIndex + 1)}}));
        }
        writer.Finish(); std::filesystem::rename(videoPartial, root / L"video.mp4");
        wchar_t executable[32768]{}; if (!GetModuleFileNameW(nullptr, executable, 32768)) throw std::runtime_error("demo_worker_identity_unavailable");
        const auto manifest = control::Object({{"sceneSchemaVersion", control::Number(demo::SceneSchemaVersion)}, {"status", control::String("completed")},
            {"renderer", control::String("native-offscreen")}, {"name", control::String(scene.name)}, {"locale", control::String(scene.locale)},
            {"width", control::Number(scene.width)}, {"height", control::Number(scene.height)}, {"fps", control::Number(scene.fps)},
            {"frames", control::Number(frameCount)}, {"duration100ns", control::String(std::to_string(video_writer::FrameTime(frameCount, scene.fps)))},
            {"sourceExeSha256", control::String(demo::Sha256(executable))}, {"sceneSha256", control::String(demo::Sha256(root / L"scene.json"))},
            {"videoSha256", control::String(demo::Sha256(root / L"video.mp4"))}, {"posterSha256", control::String(demo::Sha256(root / L"poster.png"))},
            {"posters", std::move(posters)}});
        demo::WriteJson(root / L"scene-result.json", manifest);
        result.ok = true; result.stage = "complete";
        native_component_preview::Output output; output.component = "demo-scene"; output.path = root / L"poster.png";
        output.componentWidth = scene.width; output.componentHeight = scene.height; result.outputs.push_back(std::move(output));
    }
    catch (const std::exception& failure) { result.error = failure.what(); }
    return result;
}
