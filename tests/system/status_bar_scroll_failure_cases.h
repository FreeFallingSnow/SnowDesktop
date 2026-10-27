// Fault only DirectWrite's external CreateTextLayout boundary. Font selection,
// complete/retained production layout, rendering, and surface updates stay real.
class FailingStatusBarLayoutFactory final : public IDWriteFactory
{
public:
    FailingStatusBarLayoutFactory(IDWriteFactory* real, std::wstring_view failing) : real_(real), failing_(failing) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFactory))
        { *value = static_cast<IDWriteFactory*>(this); AddRef(); return S_OK; }
        return real_->QueryInterface(iid, value);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE GetSystemFontCollection(IDWriteFontCollection** a, BOOL b) override { return real_->GetSystemFontCollection(a, b); }
    HRESULT STDMETHODCALLTYPE CreateCustomFontCollection(IDWriteFontCollectionLoader* a, const void* b, UINT32 c, IDWriteFontCollection** d) override { return real_->CreateCustomFontCollection(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE RegisterFontCollectionLoader(IDWriteFontCollectionLoader* a) override { return real_->RegisterFontCollectionLoader(a); }
    HRESULT STDMETHODCALLTYPE UnregisterFontCollectionLoader(IDWriteFontCollectionLoader* a) override { return real_->UnregisterFontCollectionLoader(a); }
    HRESULT STDMETHODCALLTYPE CreateFontFileReference(const WCHAR* a, const FILETIME* b, IDWriteFontFile** c) override { return real_->CreateFontFileReference(a, b, c); }
    HRESULT STDMETHODCALLTYPE CreateCustomFontFileReference(const void* a, UINT32 b, IDWriteFontFileLoader* c, IDWriteFontFile** d) override { return real_->CreateCustomFontFileReference(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE CreateFontFace(DWRITE_FONT_FACE_TYPE a, UINT32 b, IDWriteFontFile* const* c, UINT32 d, DWRITE_FONT_SIMULATIONS e, IDWriteFontFace** f) override { return real_->CreateFontFace(a, b, c, d, e, f); }
    HRESULT STDMETHODCALLTYPE CreateRenderingParams(IDWriteRenderingParams** a) override { return real_->CreateRenderingParams(a); }
    HRESULT STDMETHODCALLTYPE CreateMonitorRenderingParams(HMONITOR a, IDWriteRenderingParams** b) override { return real_->CreateMonitorRenderingParams(a, b); }
    HRESULT STDMETHODCALLTYPE CreateCustomRenderingParams(FLOAT a, FLOAT b, FLOAT c, DWRITE_PIXEL_GEOMETRY d, DWRITE_RENDERING_MODE e, IDWriteRenderingParams** f) override { return real_->CreateCustomRenderingParams(a, b, c, d, e, f); }
    HRESULT STDMETHODCALLTYPE RegisterFontFileLoader(IDWriteFontFileLoader* a) override { return real_->RegisterFontFileLoader(a); }
    HRESULT STDMETHODCALLTYPE UnregisterFontFileLoader(IDWriteFontFileLoader* a) override { return real_->UnregisterFontFileLoader(a); }
    HRESULT STDMETHODCALLTYPE CreateTextFormat(const WCHAR* a, IDWriteFontCollection* b, DWRITE_FONT_WEIGHT c, DWRITE_FONT_STYLE d, DWRITE_FONT_STRETCH e, FLOAT f, const WCHAR* g, IDWriteTextFormat** h) override { return real_->CreateTextFormat(a, b, c, d, e, f, g, h); }
    HRESULT STDMETHODCALLTYPE CreateTypography(IDWriteTypography** a) override { return real_->CreateTypography(a); }
    HRESULT STDMETHODCALLTYPE GetGdiInterop(IDWriteGdiInterop** a) override { return real_->GetGdiInterop(a); }
    HRESULT STDMETHODCALLTYPE CreateTextLayout(const WCHAR* value, UINT32 length, IDWriteTextFormat* format,
        FLOAT width, FLOAT height, IDWriteTextLayout** layout) override
    {
        if (!failures && value && std::wstring_view(value, length) == failing_)
        { ++failures; if (layout) *layout = nullptr; return E_FAIL; }
        return real_->CreateTextLayout(value, length, format, width, height, layout);
    }
    HRESULT STDMETHODCALLTYPE CreateGdiCompatibleTextLayout(const WCHAR* a, UINT32 b, IDWriteTextFormat* c, FLOAT d, FLOAT e, FLOAT f, const DWRITE_MATRIX* g, BOOL h, IDWriteTextLayout** i) override { return real_->CreateGdiCompatibleTextLayout(a, b, c, d, e, f, g, h, i); }
    HRESULT STDMETHODCALLTYPE CreateEllipsisTrimmingSign(IDWriteTextFormat* a, IDWriteInlineObject** b) override { return real_->CreateEllipsisTrimmingSign(a, b); }
    HRESULT STDMETHODCALLTYPE CreateTextAnalyzer(IDWriteTextAnalyzer** a) override { return real_->CreateTextAnalyzer(a); }
    HRESULT STDMETHODCALLTYPE CreateNumberSubstitution(DWRITE_NUMBER_SUBSTITUTION_METHOD a, const WCHAR* b, BOOL c, IDWriteNumberSubstitution** d) override { return real_->CreateNumberSubstitution(a, b, c, d); }
    HRESULT STDMETHODCALLTYPE CreateGlyphRunAnalysis(const DWRITE_GLYPH_RUN* a, FLOAT b, const DWRITE_MATRIX* c, DWRITE_RENDERING_MODE d, DWRITE_MEASURING_MODE e, FLOAT f, FLOAT g, IDWriteGlyphRunAnalysis** h) override { return real_->CreateGlyphRunAnalysis(a, b, c, d, e, f, g, h); }
    unsigned failures = 0;
private:
    ULONG references_ = 1;
    ComPtr<IDWriteFactory> real_;
    std::wstring failing_;
};

void CheckTrayScrollFailureRecovery(ID2D1DeviceContext* context, IDWriteFactory* text,
    const StatusBarSnapshot& original, const std::vector<std::string>& pinned)
{
    auto data = original; data.clock = L"2026/10/23   16:25";
    StatusBarSettings settings; settings.pinnedTrayItems = pinned;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> candidate, reference, stale, readable;
    Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 32), nullptr, 0, properties, &candidate)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 32), nullptr, 0, properties, &reference)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 32), nullptr, 0, properties, &stale)) &&
        SUCCEEDED(context->CreateBitmap(D2D1::SizeU(640, 32), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                properties.pixelFormat), &readable)), "fallback recovery surfaces initialize");
    const auto pixels = [&](ID2D1Bitmap1* target) {
        Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "fallback pixels copy");
        D2D1_MAPPED_RECT mapped{}; Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "fallback pixels map");
        std::vector<std::uint32_t> result(640 * 32);
        for (unsigned y = 0; y < 32; ++y) std::memcpy(result.data() + y * 640, mapped.bits + y * mapped.pitch, 640 * 4);
        readable->Unmap(); return result;
    };
    const auto full = [&](ID2D1Bitmap1* target, IDWriteFactory* factory, auto& content, auto& scroll, bool* reusable) {
        context->SetTarget(target); context->SetDpi(96, 96); context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
        const auto result = DrawStatusBarContent(context, factory, content, 640, 32, 1.f,
            PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, false, false, &scroll,
            true, nullptr, nullptr, false, reusable);
        const auto ended = context->EndDraw(); context->SetTarget(nullptr);
        Check(SUCCEEDED(result) && SUCCEEDED(ended), "recoverable text failure retains original successful fallback rendering");
    };
    // The reserved clock is measured independently of its current digits. The
    // second case faults its actual drawing, after successful measurement.
    for (const auto& failing : {std::wstring(L"8888/88/88   88:88"), data.clock})
    {
        ComPtr<FailingStatusBarLayoutFactory> failure, originalFailure;
        failure.Attach(new FailingStatusBarLayoutFactory(text, failing));
        originalFailure.Attach(new FailingStatusBarLayoutFactory(text, failing));
        auto items = BuildStatusBarItems(settings, data), expected = items;
        StatusBarTrayScroll scroll, expectedScroll;
        bool reusable = true;
        full(candidate.Get(), failure.Get(), items, scroll, &reusable);
        full(reference.Get(), originalFailure.Get(), expected, expectedScroll, nullptr);
        Check(failure->failures == 1 && originalFailure->failures == 1 && !reusable,
            "real measurement/draw failure marks the retained frame unsafe without suppressing fallback");
        Check(pixels(candidate.Get()) == pixels(reference.Get()), "validity tracking preserves exact original fallback pixels");
        const auto* fallbackClock = Find(items, "clock"); Check(fallbackClock != nullptr, "fallback clock exists");
        if (failing != data.clock) Check(fallbackClock->bounds.right - fallbackClock->bounds.left == 28, "original minimum-width measure fallback remains visible");
        const auto before = items;
        const LONG oldOffset = scroll.offset;
        Check(scroll.Move(4), "fallback fixture has an overflowing tray");
        Check(!UpdateStatusBarTrayScrollLayout(items, oldOffset, scroll, reusable), "incomplete retained frame requires a complete retry");
        for (std::size_t i = 0; i < items.size(); ++i)
            Check(EqualRect(&items[i].bounds, &before[i].bounds) && EqualRect(&items[i].contentBounds, &before[i].contentBounds),
                "declining incomplete retained layout does not mutate any target");
        Check(SUCCEEDED(stale->CopyFromBitmap(nullptr, candidate.Get(), nullptr)), "retain the failure frame for an independent negative control");
        auto staleItems = before; auto staleScroll = scroll;
        Check(UpdateStatusBarTrayScrollLayout(staleItems, oldOffset, staleScroll, true), "negative control actually ignores failure validity");
        ComPtr<AtlasSurface> staleSurface; staleSurface.Attach(new AtlasSurface(context, stale.Get()));
        const auto damage = StatusBarTrayScrollDamage(staleItems, {}, staleScroll.viewport, {}, {}, 640, 32);
        Check(SUCCEEDED(DrawStatusBarContentUpdate(staleSurface.Get(), text, staleItems, damage, 640, 32, 1.f,
            PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, false, staleScroll, {}, 96)), "negative stale-retention draw executes");
        full(candidate.Get(), failure.Get(), items, scroll, &reusable);
        expected = BuildStatusBarItems(settings, data); expectedScroll = scroll;
        full(reference.Get(), text, expected, expectedScroll, nullptr);
        Check(reusable && pixels(candidate.Get()) == pixels(reference.Get()), "complete retry restores exact current pixels");
        for (std::size_t i = 0; i < items.size(); ++i)
            Check(EqualRect(&items[i].bounds, &expected[i].bounds) && EqualRect(&items[i].contentBounds, &expected[i].contentBounds),
                "complete retry restores independent geometry and tray viewport");
        Check(pixels(stale.Get()) != pixels(reference.Get()), "ignoring validity leaves an observably stale clock or width");
        const auto* recoveredClock = Find(items, "clock");
        Check(recoveredClock && recoveredClock->bounds.right - recoveredClock->bounds.left > 28, "clock measurement recovers without a content change");
    }
    std::cout << "Clock measurement/draw fallback pixels, full retry, and stale-retention negative controls passed\n";
}
