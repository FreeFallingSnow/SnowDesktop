#include "widget_background_cache.h"

#include <d2d1effects.h>
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
using Microsoft::WRL::ComPtr;

// Exact byte comparison, not a probabilistic hash. Unknown operations fail
// closed: no image is reused unless every pixel-affecting input is described.
class CommandSignature final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, ID2D1CommandSink>
{
public:
    explicit CommandSignature(const WidgetBackgroundCache::ImmutableBitmap& predicate)
        : immutable_(predicate) { bytes.reserve(512); }
    std::vector<std::byte> bytes;
    std::vector<ComPtr<ID2D1Bitmap>> images;
    std::size_t imageBytes = 0;
    bool failed = false;

    template<typename... T> HRESULT Add(std::uint8_t command, const T&... values) noexcept
    {
        try
        {
            if (failed || bytes.size() + 1 + (sizeof(T) + ... + 0) > 65536)
                return Reject();
            const auto append = [&](const auto& value) {
                const auto* first = reinterpret_cast<const std::byte*>(&value);
                bytes.insert(bytes.end(), first, first + sizeof(value));
            };
            append(command);
            (append(values), ...);
            return S_OK;
        }
        catch (...) { failed = true; return E_OUTOFMEMORY; }
    }
    HRESULT Reject() noexcept { failed = true; return E_NOTIMPL; }
    template<typename T> HRESULT Optional(std::uint8_t command, const T* value) noexcept
    {
        return value ? Add(command, true, *value) : Add(command, false);
    }
    HRESULT Brush(ID2D1Brush* brush) noexcept
    {
        if (!brush) return Reject();
        D2D1_MATRIX_3X2_F transform{};
        brush->GetTransform(&transform);
        if (FAILED(Add(30, brush->GetOpacity(), transform))) return E_FAIL;
        ComPtr<ID2D1SolidColorBrush> solid;
        if (SUCCEEDED(brush->QueryInterface(IID_PPV_ARGS(&solid))))
            return Add(31, solid->GetColor());
        ComPtr<ID2D1LinearGradientBrush> linear;
        if (FAILED(brush->QueryInterface(IID_PPV_ARGS(&linear)))) return Reject();
        ComPtr<ID2D1GradientStopCollection> collection;
        linear->GetGradientStopCollection(&collection);
        if (!collection) return Reject();
        const auto count = collection->GetGradientStopCount();
        std::array<D2D1_GRADIENT_STOP, 64> stops{};
        if (!count || count > stops.size()) return Reject();
        if (FAILED(Add(32, linear->GetStartPoint(), linear->GetEndPoint(),
            collection->GetColorInterpolationGamma(), collection->GetExtendMode(), count)))
            return E_FAIL;
        ComPtr<ID2D1GradientStopCollection1> advanced;
        if (SUCCEEDED(collection.As(&advanced)))
        {
            if (FAILED(Add(33, advanced->GetPreInterpolationSpace(),
                advanced->GetPostInterpolationSpace(), advanced->GetBufferPrecision(),
                advanced->GetColorInterpolationMode()))) return E_FAIL;
            advanced->GetGradientStops1(stops.data(), count);
        }
        else collection->GetGradientStops(stops.data(), count);
        for (UINT32 i = 0; i < count; ++i)
            if (FAILED(Add(34, stops[i].position, stops[i].color))) return E_FAIL;
        return S_OK;
    }

    IFACEMETHODIMP BeginDraw() override { return Add(1); }
    IFACEMETHODIMP EndDraw() override { return Add(2); }
    IFACEMETHODIMP SetAntialiasMode(D2D1_ANTIALIAS_MODE mode) override { return Add(3, mode); }
    // Tags affect diagnostics, and text settings cannot affect supported draws.
    IFACEMETHODIMP SetTags(D2D1_TAG, D2D1_TAG) override { return S_OK; }
    IFACEMETHODIMP SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE) override { return S_OK; }
    IFACEMETHODIMP SetTextRenderingParams(IDWriteRenderingParams*) override { return S_OK; }
    IFACEMETHODIMP SetTransform(const D2D1_MATRIX_3X2_F* matrix) override
        { return matrix ? Add(4, *matrix) : Reject(); }
    IFACEMETHODIMP SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND blend) override
        { return blend == D2D1_PRIMITIVE_BLEND_SOURCE_OVER ? Add(5, blend) : Reject(); }
    IFACEMETHODIMP SetUnitMode(D2D1_UNIT_MODE mode) override { return Add(6, mode); }
    IFACEMETHODIMP Clear(const D2D1_COLOR_F* color) override { return Optional(7, color); }
    IFACEMETHODIMP DrawBitmap(ID2D1Bitmap* bitmap, const D2D1_RECT_F* destination,
        FLOAT opacity, D2D1_INTERPOLATION_MODE interpolation,
        const D2D1_RECT_F* source, const D2D1_MATRIX_4X4_F* perspective) override
    {
        try
        {
            if (!bitmap || perspective || !immutable_ || !immutable_(bitmap)) return Reject();
            const bool known = std::any_of(images.begin(), images.end(),
                [&](const auto& value) { return value.Get() == bitmap; });
            if (!known)
            {
                const auto size = bitmap->GetPixelSize();
                const std::uint64_t charge = std::uint64_t(size.width) * size.height * 4;
                if (images.size() >= 16 || charge > WidgetBackgroundCache::MaximumEntryBytes ||
                    imageBytes + charge > WidgetBackgroundCache::MaximumEntryBytes) return Reject();
                images.emplace_back(bitmap);
                imageBytes += static_cast<std::size_t>(charge);
            }
            if (FAILED(Add(8, reinterpret_cast<std::uintptr_t>(bitmap), opacity, interpolation)) ||
                FAILED(Optional(9, destination))) return E_FAIL;
            return Optional(10, source);
        }
        catch (...) { failed = true; return E_OUTOFMEMORY; }
    }
    IFACEMETHODIMP FillRectangle(const D2D1_RECT_F* rect, ID2D1Brush* brush) override
    {
        if (!rect || FAILED(Add(11, *rect))) return Reject();
        return Brush(brush);
    }
    IFACEMETHODIMP PushAxisAlignedClip(const D2D1_RECT_F* rect, D2D1_ANTIALIAS_MODE mode) override
        { return rect ? Add(12, *rect, mode) : Reject(); }
    IFACEMETHODIMP PopAxisAlignedClip() override { return Add(13); }
    IFACEMETHODIMP DrawGlyphRun(D2D1_POINT_2F, const DWRITE_GLYPH_RUN*,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, ID2D1Brush*, DWRITE_MEASURING_MODE) override { return Reject(); }
    IFACEMETHODIMP DrawLine(D2D1_POINT_2F, D2D1_POINT_2F, ID2D1Brush*, FLOAT, ID2D1StrokeStyle*) override { return Reject(); }
    IFACEMETHODIMP DrawGeometry(ID2D1Geometry*, ID2D1Brush*, FLOAT, ID2D1StrokeStyle*) override { return Reject(); }
    IFACEMETHODIMP DrawRectangle(const D2D1_RECT_F*, ID2D1Brush*, FLOAT, ID2D1StrokeStyle*) override { return Reject(); }
    IFACEMETHODIMP DrawImage(ID2D1Image*, const D2D1_POINT_2F*, const D2D1_RECT_F*,
        D2D1_INTERPOLATION_MODE, D2D1_COMPOSITE_MODE) override { return Reject(); }
    IFACEMETHODIMP DrawGdiMetafile(ID2D1GdiMetafile*, const D2D1_POINT_2F*) override { return Reject(); }
    IFACEMETHODIMP FillMesh(ID2D1Mesh*, ID2D1Brush*) override { return Reject(); }
    IFACEMETHODIMP FillOpacityMask(ID2D1Bitmap*, ID2D1Brush*, const D2D1_RECT_F*, const D2D1_RECT_F*) override { return Reject(); }
    IFACEMETHODIMP FillGeometry(ID2D1Geometry*, ID2D1Brush*, ID2D1Brush*) override { return Reject(); }
    IFACEMETHODIMP PushLayer(const D2D1_LAYER_PARAMETERS1*, ID2D1Layer*) override { return Reject(); }
    IFACEMETHODIMP PopLayer() override { return Reject(); }
private:
    const WidgetBackgroundCache::ImmutableBitmap& immutable_;
};

bool IntegralPixel(float value) noexcept
{
    return std::isfinite(value) && std::abs(value - std::round(value)) < 0.0001f;
}
}

void WidgetBackgroundCache::Erase(std::wstring_view owner) noexcept
{
    const auto found = entries_.find(owner);
    if (found == entries_.end()) return;
    retainedBytes_ -= found->second.bytes;
    entries_.erase(found);
}
void WidgetBackgroundCache::Clear() noexcept
{
    entries_.clear();
    retainedBytes_ = 0;
    rasterContext_.Reset();
    device_.Reset();
}

WidgetBackgroundCache::Result WidgetBackgroundCache::Resolve(std::wstring_view owner,
    ID2D1DeviceContext* context, ID2D1CommandList* commands, D2D1_RECT_F bounds,
    float blurRadius, const ImmutableBitmap& immutableBitmap) noexcept
{
    try
    {
        const auto bypass = [&]() { Erase(owner); return Result{}; };
        if (owner.empty() || !context || !commands || !std::isfinite(blurRadius) ||
            blurRadius <= 0.0f || blurRadius > 48.0f) return bypass();
        float dpiX = 96.0f, dpiY = 96.0f;
        context->GetDpi(&dpiX, &dpiY);
        const auto unit = context->GetUnitMode();
        const float scaleX = unit == D2D1_UNIT_MODE_PIXELS ? 1.0f : dpiX / 96.0f;
        const float scaleY = unit == D2D1_UNIT_MODE_PIXELS ? 1.0f : dpiY / 96.0f;
        D2D1_MATRIX_3X2_F transform{};
        context->GetTransform(&transform);
        // Resampling a cached bitmap under rotation, scale or fractional pixel
        // translation can differ from direct vector/effect rendering.
        if (!(scaleX > 0) || !(scaleY > 0) || transform._11 != 1.0f ||
            transform._22 != 1.0f || transform._12 != 0 || transform._21 != 0 ||
            !IntegralPixel(transform._31 * scaleX) || !IntegralPixel(transform._32 * scaleY) ||
            !IntegralPixel(bounds.left * scaleX) || !IntegralPixel(bounds.top * scaleY))
            return bypass();
        const float width = (bounds.right - bounds.left) * scaleX;
        const float height = (bounds.bottom - bounds.top) * scaleY;
        if (!(width > 0) || !(height > 0) || width > 2048 || height > 2048 ||
            !IntegralPixel(width) || !IntegralPixel(height)) return bypass();
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::round(width)),
            static_cast<UINT32>(std::round(height)));
        const std::size_t pixelBytes = std::size_t(size.width) * size.height * 4;
        if (pixelBytes > MaximumEntryBytes) return bypass();

        ComPtr<ID2D1Device> device;
        context->GetDevice(&device);
        if (!device) return bypass();
        if (device.Get() != device_.Get()) { Clear(); device_ = device; }
        auto signature = Microsoft::WRL::Make<CommandSignature>(immutableBitmap);
        if (!signature || FAILED(signature->Add(0, bounds, blurRadius, dpiX, dpiY, unit)) ||
            FAILED(commands->Stream(signature.Get())) || signature->failed)
            return bypass();
        const auto bytes = pixelBytes + signature->imageBytes + signature->bytes.size();
        if (bytes > MaximumEntryBytes) return bypass();
        auto found = entries_.find(owner);
        if (found != entries_.end() && found->second.signature == signature->bytes)
        {
            found->second.used = ++sequence_;
            return { found->second.bitmap, Outcome::Hit };
        }
        Erase(owner);
        if (!rasterContext_ && FAILED(device_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &rasterContext_))) return {};
        auto* raster = rasterContext_.Get();
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
        if (FAILED(raster->CreateBitmap(size, nullptr, 0, properties, &bitmap))) return {};
        ComPtr<ID2D1Effect> blur;
        if (FAILED(raster->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return {};
        blur->SetInput(0, commands);
        if (FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blurRadius)) ||
            FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED)) ||
            FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD))) return {};
        raster->SetDpi(dpiX, dpiY);
        raster->SetUnitMode(unit);
        raster->SetTransform(D2D1::Matrix3x2F::Translation(-bounds.left, -bounds.top));
        raster->SetTarget(bitmap.Get());
        raster->BeginDraw();
        raster->Clear(D2D1::ColorF(0, 0, 0, 0));
        raster->DrawImage(blur.Get());
        const HRESULT hr = raster->EndDraw();
        raster->SetTarget(nullptr);
        if (FAILED(hr)) return {};
        while (!entries_.empty() && (entries_.size() >= MaximumEntries ||
            retainedBytes_ + bytes > MaximumBytes))
        {
            const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            retainedBytes_ -= oldest->second.bytes;
            entries_.erase(oldest);
        }
        Entry entry{ std::move(signature->bytes), std::move(signature->images), bitmap, bytes, ++sequence_ };
        entries_.emplace(std::wstring(owner), std::move(entry));
        retainedBytes_ += bytes;
        return { std::move(bitmap), Outcome::Miss };
    }
    catch (...) { Erase(owner); return {}; }
}
}
