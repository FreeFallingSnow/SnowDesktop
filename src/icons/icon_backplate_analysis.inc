// Included by icon_beautify.cpp, inside snowdesktop::icon_beautify.
namespace
{
struct PlateSample
{
    EdgeColor color;
    double x = 0, y = 0;
    int angle = 0, band = 0;
};

EdgeColor PlateColor(std::uint32_t pixel)
{
    const int a = static_cast<int>(pixel >> 24);
    if (!a) return {};
    const auto straight = [a](int value) { return std::clamp((value * 255 + a / 2) / a, 0, 255); };
    return {straight((pixel >> 16) & 255), straight((pixel >> 8) & 255), straight(pixel & 255)};
}

double PlateDistance(EdgeColor a, EdgeColor b)
{
    const double r = a.r - b.r, g = a.g - b.g, blue = a.b - b.b;
    return r * r + g * g + blue * blue;
}

EdgeColor DominantPlateColor(const std::vector<PlateSample>& samples, int band = -1)
{
    struct Bucket { int count = 0, r = 0, g = 0, b = 0; };
    std::array<Bucket, 1331> buckets{};
    for (const auto& sample : samples)
    {
        if (band >= 0 && sample.band != band) continue;
        const auto c = sample.color;
        auto& b = buckets[(c.r / 24 * 11 + c.g / 24) * 11 + c.b / 24];
        ++b.count; b.r += c.r; b.g += c.g; b.b += c.b;
    }
    const auto best = std::max_element(buckets.begin(), buckets.end(),
        [](const auto& a, const auto& b) { return a.count < b.count; });
    if (!best->count) return {};
    return {best->r / best->count, best->g / best->count, best->b / best->count};
}

// Solve RGB affine planes together. Angular coverage is checked separately so a
// good least-squares fit to three sides cannot authorize a conflicting fourth.
bool FitPlatePlane(const std::vector<PlateSample>& samples, const std::vector<bool>& keep,
    std::array<std::array<double, 3>, 3>& coefficients)
{
    double matrix[3][6]{};
    for (size_t i = 0; i < samples.size(); ++i)
    {
        if (!keep[i]) continue;
        const auto& s = samples[i];
        const double point[]{1, s.x, s.y}, color[]{double(s.color.r), double(s.color.g), double(s.color.b)};
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col) matrix[row][col] += point[row] * point[col];
            for (int col = 0; col < 3; ++col) matrix[row][col + 3] += point[row] * color[col];
        }
    }
    for (int col = 0; col < 3; ++col)
    {
        int pivot = col;
        for (int row = col + 1; row < 3; ++row)
            if (std::abs(matrix[row][col]) > std::abs(matrix[pivot][col])) pivot = row;
        if (std::abs(matrix[pivot][col]) < 1e-8) return false;
        for (int j = 0; j < 6; ++j) std::swap(matrix[pivot][j], matrix[col][j]);
        const double divisor = matrix[col][col];
        for (int j = col; j < 6; ++j) matrix[col][j] /= divisor;
        for (int row = 0; row < 3; ++row)
        {
            if (row == col) continue;
            const double multiplier = matrix[row][col];
            for (int j = col; j < 6; ++j) matrix[row][j] -= multiplier * matrix[col][j];
        }
    }
    for (int row = 0; row < 3; ++row)
        for (int c = 0; c < 3; ++c) coefficients[row][c] = matrix[row][c + 3];
    return true;
}
}

BackplateAnalysis AnalyzeBackplate(const std::vector<std::uint32_t>& pixels, int width, int height)
{
    BackplateAnalysis result;
    if (width < 8 || height < 8 || pixels.size() != static_cast<size_t>(width) * height) return result;
    const auto opaque = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < width && y < height && PixelA(pixels[static_cast<size_t>(y) * width + x]) >= 160;
    };
    int left = width, top = height, right = -1, bottom = -1;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        if (opaque(x, y)) { left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y); }
    if (right < left || bottom < top) return result;
    const int w = right - left + 1, h = bottom - top + 1, span = std::max(w, h);
    if (double(std::min(w, h)) / span < .96 || double(w) / width < .70 || double(h) / height < .70) return result;

    // Normalize geometry only. Color sampling always uses the untouched source.
    // Row prefix sums bound shape fitting to 49 * 64 rows, independent of icon size.
    constexpr int grid = 64;
    std::array<std::array<int, grid + 1>, grid> rows{};
    std::array<std::array<bool, grid>, grid> mask{};
    int count = 0, hull = 0, symmetric = 0;
    for (int y = 0; y < grid; ++y)
    {
        int first = grid, last = -1;
        for (int x = 0; x < grid; ++x)
        {
            const bool visible = opaque(left + (2 * x + 1) * w / (2 * grid), top + (2 * y + 1) * h / (2 * grid));
            mask[y][x] = visible; count += visible;
            rows[y][x + 1] = rows[y][x] + visible;
            if (visible) { first = std::min(first, x); last = x; }
        }
        if (last >= first) hull += last - first + 1;
    }
    if (!count || double(count) / std::max(1, hull) < .985) return result;
    for (int y = 0; y < grid; ++y) for (int x = 0; x < grid; ++x)
        if (mask[y][x]) symmetric += mask[y][grid - 1 - x] + mask[grid - 1 - y][x];
    if (double(symmetric) / (2 * count) < .965) return result;
    double best = 0;
    bool circle = false;
    const auto fitShape = [&](double parameter, bool superellipse) {
        int area = 0, intersection = 0;
        for (int y = 0; y < grid; ++y)
        {
            const double ny = std::abs((y + .5) / grid - .5);
            const double dy = std::max(0., ny - (.5 - parameter));
            const double half = superellipse ? .5 * std::pow(1 - std::pow(ny * 2, parameter), 1 / parameter)
                : parameter == 0 ? .5 : .5 - parameter + std::sqrt(std::max(0., parameter * parameter - dy * dy));
            const int x0 = std::clamp(static_cast<int>(std::ceil((.5 - half) * grid - .5)), 0, grid - 1);
            const int x1 = std::clamp(static_cast<int>(std::floor((.5 + half) * grid - .5)), 0, grid - 1);
            area += x1 - x0 + 1; intersection += rows[y][x1 + 1] - rows[y][x0];
        }
        const double iou = double(intersection) / std::max(1, area + count - intersection);
        if (iou > best) { best = iou; circle = superellipse ? parameter <= 2.5 : parameter >= .44; }
    };
    for (int radius = 0; radius <= 40; ++radius) fitShape(radius / 80., false);
    for (double power : {2., 2.5, 3., 3.5, 4., 5., 6., 8.}) fitShape(power, true);
    if (best < .965) return result;
    // Discs are recognizable foreground contours, including their own shading.
    // Extending them into another outline destroys that contour (e.g. Arctime).
    if (circle) { result.kind = BackplateKind::Contour; return result; }
    result.kind = BackplateKind::Artwork;

    constexpr int directions = 128, bands = 4, sectors = 16;
    constexpr double pi = 3.14159265358979323846;
    const double cx = (left + right) * .5, cy = (top + bottom) * .5;
    std::vector<PlateSample> samples;
    samples.reserve(directions * bands);
    constexpr std::array<double, bands> depths{0, .012, .03, .06};
    for (int angle = 0; angle < directions; ++angle)
    {
        const double dx = std::cos(angle * 2 * pi / directions), dy = std::sin(angle * 2 * pi / directions);
        for (double distance = span * .75; distance >= 0; distance -= .5)
        {
            if (!opaque(static_cast<int>(std::lround(cx + dx * distance)), static_cast<int>(std::lround(cy + dy * distance)))) continue;
            for (int band = 0; band < bands; ++band)
            {
                const double inner = std::max(0., distance - depths[band] * span);
                const int x = static_cast<int>(std::lround(cx + dx * inner)), y = static_cast<int>(std::lround(cy + dy * inner));
                if (!opaque(x, y)) continue;
                samples.push_back({PlateColor(pixels[static_cast<size_t>(y) * width + x]),
                    (x + .5) / width - .5, (y + .5) / height - .5, angle, band});
            }
            break;
        }
    }
    if (samples.size() < directions * bands * .95) return result;
    const auto base = DominantPlateColor(samples);
    std::array<EdgeColor, bands> bandColors{};
    std::array<int, bands> bandCount{}, bandMatches{};
    for (int band = 0; band < bands; ++band) bandColors[band] = DominantPlateColor(samples, band);
    for (const auto& s : samples)
    {
        ++bandCount[s.band];
        if (PlateDistance(s.color, bandColors[s.band]) <= 30 * 30) ++bandMatches[s.band];
    }
    bool uniformBands = true;
    double bandDelta = 0;
    for (int band = 0; band < bands; ++band)
    {
        uniformBands &= bandMatches[band] >= bandCount[band] * .86;
        for (int other = 0; other < band; ++other)
            bandDelta = std::max(bandDelta, PlateDistance(bandColors[band], bandColors[other]));
    }
    if (uniformBands && bandDelta > 40 * 40) { result.kind = BackplateKind::Framed; return result; }

    std::array<std::array<double, 3>, 3> coefficients{};
    std::vector<bool> keep(samples.size(), true);
    std::vector<double> errors(samples.size());
    for (int iteration = 0; iteration < 5; ++iteration)
    {
        if (!FitPlatePlane(samples, keep, coefficients)) return result;
        for (size_t i = 0; i < samples.size(); ++i)
        {
            const auto& s = samples[i];
            const double color[]{double(s.color.r), double(s.color.g), double(s.color.b)};
            double error = 0;
            for (int c = 0; c < 3; ++c)
            {
                const double delta = color[c] - (coefficients[0][c] + coefficients[1][c] * s.x + coefficients[2][c] * s.y);
                error += delta * delta;
            }
            errors[i] = error;
        }
        auto sorted = errors;
        const size_t trim = sorted.size() * 9 / 10;
        std::nth_element(sorted.begin(), sorted.begin() + trim, sorted.end());
        for (size_t i = 0; i < samples.size(); ++i) keep[i] = errors[i] <= sorted[trim];
    }
    int solidMatches = 0, gradientMatches = 0;
    std::array<int, sectors> totals{}, solids{}, gradients{};
    std::array<int, directions> angleMatches{};
    double solidError = 0, gradientError = 0;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const auto& s = samples[i]; const int sector = s.angle * sectors / directions;
        ++totals[sector];
        const double error = PlateDistance(s.color, base);
        if (error <= 30 * 30) { ++solidMatches; ++solids[sector]; }
        if (errors[i] <= 24 * 24) { ++gradientMatches; ++gradients[sector]; ++angleMatches[s.angle]; }
        // Compare robust errors so small marks do not overwhelm a mild gradient.
        solidError += std::min(error, 30. * 30); gradientError += std::min(errors[i], 30. * 30);
    }
    int solidSectors = 0, gradientSectors = 0, run = 0, maxGap = 0;
    for (int s = 0; s < sectors; ++s)
    {
        solidSectors += totals[s] > 0 && solids[s] >= totals[s] * .8;
        gradientSectors += totals[s] > 0 && gradients[s] >= totals[s] * .7;
    }
    for (int angle = 0; angle < directions * 2; ++angle)
    {
        run = angleMatches[angle % directions] < 3 ? run + 1 : 0;
        maxGap = std::max(maxGap, run);
    }
    const bool continuous = gradientMatches >= samples.size() * .90 && gradientSectors >= 14 && maxGap <= 8;
    double slope = 0;
    for (int c = 0; c < 3; ++c)
        slope += coefficients[1][c] * coefficients[1][c] + coefficients[2][c] * coefficients[2][c];
    const bool gradient = continuous && slope > 8 * 8 && gradientError < solidError * .45;
    if (gradient)
    {
        PlateFill fill(EdgeColor{static_cast<int>(std::lround(coefficients[0][0])),
            static_cast<int>(std::lround(coefficients[0][1])), static_cast<int>(std::lround(coefficients[0][2]))});
        fill.gradient = true;
        for (int c = 0; c < 3; ++c) { fill.dx[c] = static_cast<float>(coefficients[1][c]); fill.dy[c] = static_cast<float>(coefficients[2][c]); }
        result.kind = BackplateKind::Gradient; result.fill = fill;
    }
    else if (solidMatches >= samples.size() * .90 && solidSectors >= 14 && bandDelta <= 35 * 35)
    {
        result.kind = BackplateKind::Solid; result.fill = PlateFill(base);
    }
    return result;
}

std::optional<PlateFill> DetectPlateFill(const std::vector<std::uint32_t>& pixels, int width, int height)
{
    return AnalyzeBackplate(pixels, width, height).fill;
}

std::optional<PlateFill> DetectEdgeFill(const std::vector<std::uint32_t>& pixels, int width, int height)
{
    return DetectPlateFill(pixels, width, height);
}
