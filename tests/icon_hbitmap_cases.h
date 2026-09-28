// Real GDI handles exercise the production reader used before D2D upload.
// Distinct rows/corners expose vertical inversion, unlike uniform-color icons.
void TestIconBitmapRowOrder()
{
    using namespace snowdesktop::icon_bitmap_pixels;
    const std::vector<std::uint32_t> expected{
        0xffff0000, 0xff00ff00, 0xff0000ff,
        0x80402010, 0x00000000, 0xff778899};
    Buffer output;
    for (const LONG signedHeight : {-2L, 2L})
    {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 3;
        info.bmiHeader.biHeight = signedHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP original = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        Check(original && bits, "asymmetric DIB fixture is available");
        if (!original || !bits) { if (original) DeleteObject(original); continue; }
        auto* colors = static_cast<std::uint32_t*>(bits);
        const std::vector<std::uint32_t> bottomUp{
            0x80402010, 0x00000000, 0xff778899,
            0xffff0000, 0xff00ff00, 0xff0000ff};
        const auto& stored = signedHeight < 0 ? expected : bottomUp;
        std::copy(stored.begin(), stored.end(), colors);
        Check(ReadHBitmap(original, output) && output.width == 3 && output.height == 2 &&
            output.pixels == expected, "both DIB orientations upload the same upright icon pixels");
        HBITMAP current = original;
        // ShortcutCache::Put/Get each uses CopyImage. Test the same two copies.
        for (int copy = 0; copy < 2; ++copy)
        {
            HBITMAP next = static_cast<HBITMAP>(CopyImage(current, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
            Check(next && ReadHBitmap(next, output) && output.pixels == expected,
                "copied shortcut pixels retain direction through cache insertion and retrieval");
            if (current != original) DeleteObject(current);
            current = next;
            if (!current) break;
        }
        if (current && current != original) DeleteObject(current);
        DeleteObject(original);
    }

    // Non-32-bit DIBs take the GDI conversion path, including padded rows.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 1;
    info.bmiHeader.biHeight = 2;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HBITMAP rgb = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Check(rgb && bits, "padded RGB fixture is available");
    if (rgb && bits)
    {
        const unsigned char stored[]{255, 0, 0, 0, 0, 0, 255, 0};
        std::memcpy(bits, stored, sizeof(stored));
        Check(ReadHBitmap(rgb, output) && output.width == 1 && output.height == 2 &&
            output.pixels == std::vector<std::uint32_t>({0xffff0000, 0xff0000ff}),
            "GDI fallback preserves top/bottom color order and opaque alpha");
    }
    if (rgb) DeleteObject(rgb);
    Check(!ReadHBitmap(nullptr, output) && output.pixels.empty() && !output.width && !output.height,
        "failed bitmap reads cannot expose an earlier icon's pixels");
}
