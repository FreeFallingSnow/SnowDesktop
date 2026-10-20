#include "icons/icon_library_bitmap_cache.h"
#include <iostream>

// Exercises the real source-cache boundary used by GetDockCustomIcon. The
// host's IPC route and on-screen frame timing still require desktop acceptance.
int RunIconLibraryBitmapCacheTests()
{
    using namespace snowdesktop;
    int failures = 0;
    const auto check = [&](bool value, const char* text) {
        if (!value) { ++failures; std::cerr << "FAILED: " << text << '\n'; }
    };
    const auto pixels = [] {
        auto result = std::make_shared<BackgroundBitmap>();
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 1; info.bmiHeader.biHeight = -1;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        result->bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bits) *static_cast<std::uint32_t*>(bits) = 0xff2164b2;
        return result;
    };
    auto first = pixels(), second = pixels();
    check(first->bitmap && second->bitmap, "allocate independent source pixels");
    if (!first->bitmap || !second->bitmap) return failures;
    const std::string a = std::string(64, 'a') + ".png", b = std::string(64, 'b') + ".webp";
    const auto asset = [](std::string id) { IconLibraryEntry entry; entry.id = std::move(id); return entry; };
    std::vector<std::string> released;
    const auto release = [&](const auto& key, const auto&) { released.push_back(key); };
    IconLibraryBitmapCache cache(2);
    const auto one = cache.Begin("a", a, L"item:file", {a}, release);
    check(cache.Publish("a", one, first) && cache.Read("a", L"item:file") == first, "publish the chosen replacement");
    const auto two = cache.Begin("b", b, L"widget:folder", {a,b}, release);
    // Opening, importing, renaming and moving groups all return snapshots.
    // Metadata changes must leave ready pixels and in-flight decodes intact.
    auto renamed = asset(a); renamed.name = L"Renamed"; renamed.groupIds = {"new-group"};
    cache.Reconcile({renamed, asset(b)}, release);
    check(cache.Read("a", L"item:file") == first && released.empty(), "library refresh and metadata edits never expose the original icon");
    check(cache.Publish("b", two, second), "refresh preserves an in-flight decode and its delivery generation");
    cache.Begin("pending", b, L"item:file", {a,b}, release);
    check(cache.Read("pending", L"item:file") == first, "keep the displayed replacement until its new source is ready");
    cache.Publish("pending", 0, second); // stale completion cannot replace the pending source.
    check(cache.Read("pending", L"item:file") == first, "a stale completion cannot end the retained display");
    cache.Reconcile({asset(a)}, release);
    check(cache.Read("a", L"item:file") == first && !cache.Contains("b") && !cache.Contains("pending"),
        "deleting or losing one asset only invalidates that asset");
    const auto retry = cache.Begin("b", b, L"item:file", {a,b}, release);
    check(!cache.Publish("b", two, second) && cache.Publish("b", retry, second), "deleted requests cannot overwrite a later re-import");
    check(cache.Read("b", L"item:file") == second, "the completed source replaces the retained pixels");
    const auto broken = cache.Begin("broken", a, L"item:file", {a,b}, release);
    cache.Publish("broken", broken, {});
    check(!cache.Read("broken", L"item:file"), "confirmed decode failure falls back instead of retaining a misleading source");
    cache.Reconcile({asset(a),asset(b)}, release);
    check(!cache.Contains("broken"), "explicit refresh permits failed sources to be retried");
    check(cache.Read("b", L"item:file") == second, "display a real replacement before restoring default");
    cache.Forget(L"item:file");
    cache.Begin("new", a, L"item:file", {a,b}, release);
    check(!cache.Read("new", L"item:file"), "restoring default discards the previous replacement transition");
    IconLibraryBitmapCache bounded(2);
    for (int i = 0; i < 6; ++i)
    {
        const auto key = std::to_string(i);
        const auto ticket = bounded.Begin(key, i ? b : a, {}, {a}, release);
        bounded.Publish(key, ticket, i ? second : first);
    }
    check(bounded.Read("0", L"control:start") == first && bounded.Contains("5") && !bounded.Contains("1"),
        "capacity pressure evicts unused sources while preserving active desktop and Dock icons");
    IconLibraryBitmapCache active(1);
    active.Publish("a", active.Begin("a", a, {}, {a,b}, release), first);
    active.Publish("b", active.Begin("b", b, {}, {a,b}, release), second);
    check(active.Read("a", L"control:search") == first && active.Read("b", L"recycle-full:bin") == second,
        "an active working set larger than the soft limit does not cycle through default icons");
    return failures;
}

#ifdef SNOWDESKTOP_ICON_CACHE_STANDALONE
int main() { return RunIconLibraryBitmapCacheTests() ? 1 : 0; }
#endif
