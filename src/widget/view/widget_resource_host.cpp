#include "widget_resource_host.h"

#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "common/l10n.h"
#include "diagnostics/performance_trace.h"
#include "widget/view/widget_draw_lua.h"
#include "widget/view/widget_draw_context.h"
#include "widget/packages/widget_package_host.h"
#include "widget/packages/widget_package_manager.h"
#include "widget/packages/widget_package_paths.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/permissions/widget_permission_broker.h"
#include "common/background_work.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>

using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_runtime::BoundWidgetRuntimeToken;
using snowdesktop::widget_runtime::RequirePermission;
using snowdesktop::widget_runtime::GetWidgetPackageManager;
using snowdesktop::widget_runtime::LuaResourceType;
using snowdesktop::widget_runtime::TestResourceHandle;
using snowdesktop::widget_runtime::ResolveResourceHandleImageContentKey;
using snowdesktop::widget_runtime::ResolveResourceHandlePath;

using snowdesktop::widget::ResolvePackageAssetWithinRoot;

static std::string RegisterRuntimeImageSource(D2DState* state,
    const std::wstring& widgetId, std::string_view token,
    std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels> pixels,
    std::string source, bool preserveExisting = false)
{
    return state ? state->runtimeImageCache.Register(widgetId, token,
        std::move(pixels), std::move(source), preserveExisting) : std::string{};
}

void ClearRuntimeImagesForWidget(D2DState* state, const std::wstring& widgetId)
{
    if (!state || widgetId.empty()) return;
    state->backgroundCache.Erase(widgetId);
    state->textLayoutCache.Erase(widgetId);
    state->runtimeImageCache.RemoveWidget(widgetId);
}

static std::string WidgetPrewarmContentDigest(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.size() > (std::numeric_limits<ULONG>::max)()) return {};
    std::array<unsigned char, 32> digest{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
            digest.data(), static_cast<ULONG>(digest.size())) < 0)
        return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string key;
    key.reserve(64);
    for (const auto byte : digest)
    {
        key.push_back(hex[byte >> 4]);
        key.push_back(hex[byte & 0x0f]);
    }
    return key;
}

void PrewarmWidgetPackageResources(D2DState* state)
{
    // Preparation is optional: the normal loader remains authoritative for
    // current files, permissions, errors, Lua execution and device resources.
    try
    {
        std::vector<std::wstring> files;
        std::vector<std::wstring> images;
        auto enqueue = [&](const std::filesystem::path& root,
            const std::wstring& relative, bool image = false) {
            if (files.size() + images.size() >= 128) return;
            if (const auto path = ResolvePackageAssetWithinRoot(root, relative))
                (image ? images : files).push_back(*path);
        };
        for (const auto& package : GetWidgetPackageManager().ListPackages())
        {
            if (!package.active || !package.enabled ||
                !snowdesktop::widget::IsExecutablePackageContract(package.manifest))
                continue;
            const auto grant = snowdesktop::widget::WidgetPermissionBroker::Evaluate(
                package.permissionState, package.manifest.permissions,
                package.manifest.optionalPermissions, package.manifest.networkDomains,
                package.grantedPermissions, package.grantedNetworkDomains);
            if (grant.runtimeBlock != snowdesktop::widget::PermissionRuntimeBlock::None)
                continue;
            enqueue(package.root, L"widget.json");
            enqueue(package.root, Utf8ToWide(package.manifest.entry));
            const auto defersImages = [](const auto& features) {
                return std::any_of(features.begin(), features.end(), [](const auto& feature) {
                    return feature == "resource.image.lazy" ||
                        feature == "resource.image.onDemand";
                });
            };
            const bool deferredImages = defersImages(package.manifest.requiredFeatures) ||
                defersImages(package.manifest.optionalFeatures);
            for (const auto& [name, resource] : package.manifest.resources)
            {
                (void)name;
                if (resource.type == "image" && !deferredImages)
                    enqueue(package.root, Utf8ToWide(resource.path), true);
            }
            if (files.size() + images.size() >= 128) break;
        }
        state->packageImageCache.Prewarm(std::move(files), std::move(images),
            WidgetPrewarmContentDigest);
    }
    catch (...)
    {
        // Speculation must never replace the synchronous load/error path.
    }
}

using snowdesktop::widget_runtime::IsFilesystemHandleTask;
using snowdesktop::widget_runtime::IsFilesystemPickerTask;
using snowdesktop::widget_runtime::IsPreviewFilesystemHandle;
using snowdesktop::widget_runtime::kFilesystemReadPermission;
using snowdesktop::widget_runtime::kFilesystemWritePermission;

static void ReleasePackageImageResourcesUnprotected(
    D2DState* state, lua_State* luaState)
{
    if (!state || !luaState) return;
    state->packageImageCache.ForgetOwner(
        WideToUtf8(BoundWidgetId(luaState)) + "|");
    lua_getfield(luaState, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    if (lua_istable(luaState, -1))
    {
        lua_pushnil(luaState);
        while (lua_next(luaState, -2) != 0)
        {
            size_t length = 0;
            const char* value = lua_tolstring(luaState, -1, &length);
            if (value && length > 0)
            {
                const std::string contentKey(value, length);
                if (state->packageImageCache.Release(contentKey))
                {
                    if (const auto found = state->imageCache.find(contentKey);
                        found != state->imageCache.end())
                    {
                        state->imageSamplingCache.Remove(found->second.Get());
                        state->imageCache.erase(found);
                        state->imageCacheRevisions.erase(contentKey);
                    }
                }
            }
            lua_pop(luaState, 1);
        }
    }
    lua_pop(luaState, 1);
    lua_pushnil(luaState);
    lua_setfield(luaState, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
}

void ReleasePackageImageResources(D2DState* state, lua_State* luaState)
{
    if (!state || !luaState) return;
    std::string error;
    (void)snowdesktop::lua_runtime::ProtectedHostCall(luaState,
        +[](lua_State* inner) {
            ReleasePackageImageResourcesUnprotected(
                static_cast<D2DState*>(lua_touserdata(inner, 1)), inner);
            return 0;
        }, state, 0, error);
}

static snowdesktop::widget_runtime::LuaDrawImageResult ResolveDrawImageBitmap(
    lua_State* state, int index,
    float width, float height,
    snowdesktop::widget_runtime::DrawImageFit fit,
    D2D1_SIZE_F* originalSize, bool originalPixels)
{
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine) return {};

    const auto* handle = TestResourceHandle(state, index);
    if (!handle)
        return {nullptr, "API v2 requires an image handle"};
    if (handle->type != LuaResourceType::Image)
        return {nullptr, "invalid image resource handle"};

    ID2D1Bitmap1* bitmap = nullptr;
    if (snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(handle->name))
        bitmap = LoadRuntimeImageBitmap(
            d2d, BoundWidgetId(state), handle->name);
    else if (const auto contentKey = ResolveResourceHandleImageContentKey(state, index))
    {
        snowdesktop::widget_runtime::PackageImageSize original;
        bitmap = LoadImageBitmap(d2d, *contentKey, width, height, fit,
            originalPixels, &original);
        if (originalSize)
            *originalSize = D2D1::SizeF(static_cast<float>(original.width),
                static_cast<float>(original.height));
        if (!bitmap && d2d->packageImageCache.Find(*contentKey))
            return {};
    }
    if (!bitmap)
        return {nullptr, "invalid image resource handle"};
    if (originalSize && originalSize->width <= 0)
        *originalSize = bitmap->GetSize();
    return {bitmap, nullptr};
}

static const snowdesktop::widget_runtime::LuaDrawHostServices kLuaDrawHostServices{
    [](lua_State* state, int index) {
        return ResolveResourceHandlePath(state, index, LuaResourceType::Font);
    },
    ResolveDrawImageBitmap,
    [](lua_State* state, snowdesktop::widget_runtime::NativeMarqueeText&& marquee) {
        const auto* d2d = GetD2D(state);
        std::string error;
        return d2d && d2d->engine && d2d->engine->RuntimeSubmitNativeMarquee(
            BoundWidgetId(state), std::move(marquee), error);
    },
};

static const snowdesktop::widget_runtime::LuaResourceHost kLuaResourceHost{
    [](lua_State* state) -> snowdesktop::widget_runtime::WidgetPackageImageCache* {
        auto* d2d = GetD2D(state);
        return d2d ? &d2d->packageImageCache : nullptr;
    },
    [](lua_State* state, const std::string& key) {
        auto* d2d = GetD2D(state);
        return d2d && (!d2d->ctx || LoadImageBitmap(d2d, key));
    },
    snowdesktop::widget_runtime::HashWidgetFile,
    WidgetPrewarmContentDigest,
    [](lua_State* state) {
        const auto* d2d = GetD2D(state);
        return d2d && d2d->dwrite;
    },
    [](lua_State* state, const std::wstring& path) {
        return GetD2D(state)->PreloadPrivateFont(path);
    },
    [](lua_State* state, const std::wstring& path) {
        return GetD2D(state)->HasPrivateFont(path);
    },
    [](lua_State* state, const char* token) {
        const auto* d2d = GetD2D(state);
        return d2d && d2d->runtimeImageCache.GetPixels(BoundWidgetId(state), token) != nullptr;
    },
};

int lua_ResourceImage(lua_State* state)
{
    return snowdesktop::widget_runtime::LoadLuaPackageImage(state, kLuaResourceHost);
}

int lua_ResourceFont(lua_State* state)
{
    return snowdesktop::widget_runtime::LoadLuaPackageFont(state, kLuaResourceHost);
}

int lua_ResourceStatus(lua_State* state)
{
    return snowdesktop::widget_runtime::PushLuaResourceStatus(state, kLuaResourceHost);
}

static std::wstring ReadLuaPreviewIconTitle(
    lua_State* state, int index, const std::wstring& fallback)
{
    if (lua_istable(state, index))
    {
        const int table = lua_absindex(state, index);
        lua_getfield(state, table, "title");
        const char* title = lua_isstring(state, -1)
            ? lua_tostring(state, -1) : nullptr;
        const std::wstring result = Utf8ToWide(title ? title : "");
        lua_pop(state, 1);
        if (!result.empty()) return result;
    }
    std::wstring result = fallback;
    const size_t separator = result.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
        result.erase(0, separator + 1);
    const size_t dot = result.find(L'.');
    if (dot != std::wstring::npos) result.erase(dot);
    return result;
}

int lua_DrawIcon(lua_State* L)
{
    if (!RequirePermission(L, "desktop.read")) return 0;
    auto* s = GetD2D(L);
    std::wstring path;
    std::size_t length = 0;
    const char* raw = luaL_checklstring(L, 1, &length);
    const std::string reference(raw ? raw : "", length);
    if (reference.empty() || reference.size() > 128)
        return luaL_error(L,
            "draw.icon: reference must contain 1 to 128 bytes");
    if (s && s->engine)
    {
        const auto resolved = s->engine->RuntimeResolveItemReference(
            BoundWidgetId(L), BoundWidgetRuntimeToken(L), reference);
        if (resolved) path = *resolved;
    }
    float x = static_cast<float>(luaL_checknumber(L, 2));
    float y = static_cast<float>(luaL_checknumber(L, 3));
    float size = static_cast<float>(luaL_optnumber(L, 4, 32));
    float alpha = static_cast<float>(luaL_optnumber(L, 5, 1.0));
    if (!s || !s->ctx || path.empty()) return 0;

    if (s->engine && s->engine->IsPreviewOnly())
    {
        DrawSimulatedPreviewIcon(s, path,
            ReadLuaPreviewIconTitle(L, 1, path),
            x, y, size, alpha);
        return 0;
    }

    EnsureBitmapCachesForCurrentDevice(s);
    if (s->shellIconFailures.contains(path))
        return 0;
    auto cached = s->shellIconCache.find(path);
    if (cached == s->shellIconCache.end())
    {
        if (s->shellIconLoader)
            s->shellIconLoader->Request(
                path, BoundWidgetId(L));
        return 0;
    }
    D2D1_RECT_F dst = D2D1::RectF(x + s->widgetRect.left, y + s->widgetRect.top,
        x + s->widgetRect.left + size, y + s->widgetRect.top + size);
    s->ctx->DrawBitmap(
        cached->second.Get(), dst, alpha,
        D2D1_INTERPOLATION_MODE_LINEAR);
    return 0;
}

void BindWidgetDrawHost(lua_State* state, D2DState* context)
{
    snowdesktop::widget_runtime::BindLuaDrawContext(state, context, kLuaDrawHostServices);
}

void WidgetEngine::DrainPackageImageChanges()
{
    if (d2dState_)
    {
        for (const auto& widget : widgets_)
        {
            const auto prefix = WideToUtf8(widget.widgetId) + "|";
            if (!widget.valid)
                d2dState_->packageImageCache.ForgetOwner(prefix);
            else if (!widget.preview && !widget.desktopVisible)
                d2dState_->packageImageCache.ForgetOwner(prefix + "desktop");
        }
        d2dState_->packageImageCache.Pump();
        const auto changed = d2dState_->packageImageCache.TakeChanged();
        std::vector<std::wstring> invalidated;
        for (const auto& key : changed)
        {
            if (const auto bitmap = d2dState_->imageCache.find(key);
                bitmap != d2dState_->imageCache.end())
            {
                d2dState_->imageSamplingCache.Remove(bitmap->second.Get());
                d2dState_->imageCache.erase(bitmap);
                d2dState_->imageCacheRevisions.erase(key);
            }
            for (const auto& widget : widgets_)
            {
                if (!std::any_of(widget.packageImageContentKeys.begin(),
                        widget.packageImageContentKeys.end(),
                        [&](const auto& resource) { return resource.second == key; }))
                    continue;
                d2dState_->backgroundCache.Erase(widget.widgetId);
                if (d2dState_->packageImageCache.DecodeFailed(key))
                    RuntimeAddLog(widget.widgetId, "error", "Lazy package image decoding failed");
                invalidated.push_back(widget.widgetId);
            }
        }
        std::sort(invalidated.begin(), invalidated.end());
        invalidated.erase(std::unique(invalidated.begin(), invalidated.end()), invalidated.end());
        for (const auto& id : invalidated) RuntimeInvalidateHost(id);
    }
}

std::optional<std::wstring> WidgetEngine::RuntimeResolvePackageAsset(
    const std::wstring& widgetId, const std::wstring& relativePath) const
{
    const int index = FindWidget(widgetId);
    if (index < 0) return std::nullopt;
    return ResolvePackageAssetWithinRoot(
        widgets_[index].packageRoot, relativePath);
}

std::optional<std::wstring> WidgetEngine::RuntimeResolvePackageResource(
    const std::wstring& widgetId, std::string_view name,
    std::string_view expectedType) const
{
    const int index = FindWidget(widgetId);
    if (index < 0 || name.empty() || expectedType.empty())
        return std::nullopt;
    const auto resource = widgets_[index].manifest.resources.find(
        std::string(name));
    if (resource == widgets_[index].manifest.resources.end() ||
        resource->second.type != expectedType)
        return std::nullopt;
    return ResolvePackageAssetWithinRoot(widgets_[index].packageRoot,
        Utf8ToWide(resource->second.path));
}

std::optional<std::string>
WidgetEngine::RuntimeResolvePackageImageContentKey(
    const std::wstring& widgetId, std::string_view name) const
{
    const int index = FindWidget(widgetId);
    if (index < 0 || name.empty()) return std::nullopt;
    const auto key = widgets_[index].packageImageContentKeys.find(
        std::string(name));
    if (key == widgets_[index].packageImageContentKeys.end())
        return std::nullopt;
    return key->second;
}

const snowdesktop::widget_runtime::PackageImageSource*
WidgetEngine::RuntimeFindPackageImageSource(
    const std::wstring& widgetId, std::string_view name) const noexcept
{
    if (!d2dState_) return nullptr;
    const auto contentKey = RuntimeResolvePackageImageContentKey(
        widgetId, name);
    return contentKey
        ? d2dState_->packageImageCache.Find(*contentKey) : nullptr;
}

std::string WidgetEngine::RegisterTaskImage(const std::wstring& widgetId,
    std::string_view token,
    std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels> pixels,
    std::string source)
{
    return RegisterRuntimeImageSource(d2dState_, widgetId, token, std::move(pixels), std::move(source));
}

std::shared_ptr<const snowdesktop::widget_runtime::WidgetRuntimeImagePixels>
WidgetEngine::ResolveTaskImage(const std::wstring& widgetId, const std::string& token) const
{
    return d2dState_ ? d2dState_->runtimeImageCache.GetPixels(widgetId, token) : nullptr;
}

void CapturePackageImageResources(LuaWidget& w)
{
    lua_getfield(w.state, LUA_REGISTRYINDEX,
        "__widget_resource_content_keys");
    if (lua_istable(w.state, -1))
    {
        lua_pushnil(w.state);
        while (lua_next(w.state, -2) != 0)
        {
            size_t nameLength = 0;
            size_t keyLength = 0;
            const char* resourceName = lua_tolstring(
                w.state, -2, &nameLength);
            const char* contentKey = lua_tolstring(
                w.state, -1, &keyLength);
            if (resourceName && contentKey && nameLength > 0 &&
                keyLength > 0)
            {
                w.packageImageContentKeys.insert_or_assign(
                    std::string(resourceName, nameLength),
                    std::string(contentKey, keyLength));
            }
            lua_pop(w.state, 1);
        }
    }
    lua_pop(w.state, 1);
}
