#pragma once

// Included after the shared native preview runner helpers. The published
// fenced recipe itself is the input; no copied Lua implementation is tested.
void CheckPublishedVirtualListRecipe(const std::filesystem::path& snowwidget,
    const std::filesystem::path& host, const std::filesystem::path& root,
    const std::filesystem::path& repository,
    const std::filesystem::path& recipeOverride = {})
{
    const auto document = recipeOverride.empty()
        ? repository / L"widgets/snowdesktop-lua-widget/references/api-v2.md"
        : recipeOverride;
    std::ifstream input(document, std::ios::binary);
    Check(static_cast<bool>(input), "published author recipe is readable");
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto anchor = text.find("local itemExtent = 44");
    const auto start = anchor == std::string::npos ? anchor : text.rfind("```lua", anchor);
    const auto end = start == std::string::npos ? start : text.find("```", start + 6);
    Check(start != std::string::npos && end != std::string::npos,
        "published fixed-extent virtual list recipe is complete");
    const std::string recipe = text.substr(start + 6, end - start - 6);
    std::vector<std::string> failures;
    for (const int count : {0, 1, 500})
    {
        try
        {
            const auto source = root / (L"doc-virtual-list-" + std::to_wstring(count));
            std::filesystem::create_directories(source);
            CopyManifestAndPreview(repository / L"widgets/system-monitor", source);
            const std::string script = "local articles = {}\nfor i = 1, " + std::to_string(count) +
                " do articles[i] = { id = tostring(i), title = 'Article ' .. i } end\n" +
                "local recipe = function(context, model)\n" + recipe + "\nend\n" +
                "return widget.define({ setup = function() return {initialArticleIndex = " +
                std::to_string(count > 0 ? 475 : 1) + "} end,\n" +
                "view = function(context, model)\nlocal tree = recipe(context, model)\n" +
                "assert(tree.itemCount == #articles, 'published recipe retains full item count')\n" +
                "assert((#articles == 0 and tree.firstIndex == 0 and #tree.children == 0) or " +
                "(#articles > 0 and tree.firstIndex >= 1 and #tree.children > 0), " +
                "'published recipe materializes a real nonempty window or an empty collection')\n" +
                "if #articles == 500 then assert(tree.firstIndex > 1 and tree.firstIndex <= 475 and " +
                "tree.firstIndex + #tree.children > 475, 'scrolled recipe includes its requested article') end\n" +
                "return tree\nend })\n";
            Write(source / L"main.lua", script);
            const auto output = source / L"result.png";
            const auto [exit, json] = Run(snowwidget,
                {L"preview", source.wstring(), output.wstring(), L"--host", host.wstring()});
            if (exit != 0) std::cerr << "doc virtual list count=" << count << ": " << json << '\n';
            Check(exit == 0 && json.find("\"ok\":true") != std::string::npos &&
                std::filesystem::is_regular_file(output),
                "published virtual list recipe executes through actual sandbox, layout and PNG rendering");
            CheckPng(output);
        }
        catch (const ProcessTimeout&) { throw; }
        catch (const std::exception& error)
        { failures.emplace_back(std::to_string(count) + ": " + error.what()); }
    }
    if (!failures.empty())
    {
        for (const auto& failure : failures) std::cerr << failure << '\n';
        Check(false, "published virtual list recipe must work for empty, single and scrolled large collections");
    }
}
