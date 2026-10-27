#pragma once

#include "async_slot_transfer_cases.h"
#include "drag_drop/delayed_desktop_file_drop.h"
#include "drag_drop/right_drag_actions.h"

namespace right_drag_file_cases
{
using namespace async_slot_transfer_cases;

inline void Run(const std::filesystem::path& root)
{
    struct Decision { UINT command; DWORD effect; const char* label; };
    struct Pair { UINT command; DWORD modifiers; const char* label; widget_pair_drop::Action action; };
    // Menu labels are bound product promises. These literal expectations do
    // not call production modifier conversion or action-selection helpers.
    constexpr Decision decisions[]{
        {1, 2, "widget.base.move"}, {2, 1, "widget.base.copy_label"},
        {3, 4, "widget.base.create_shortcut"}, {4, 0, "app.settings.cancel"},
        {0, 0, "dismiss/Esc"},
    };
    size_t serial = 0;
    for (DWORD allowed : {7u, 0u, 1u, 2u, 3u, 4u, 5u, 6u})
    for (size_t count : {size_t{1}, size_t{2}})
    for (const auto& decision : decisions)
    {
        const auto path = root / (L"right-drag-" + std::to_wstring(serial++));
        const auto selected = Seed(path, count);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        right_drag_actions::Context context;
        context.allowedEffects = allowed; context.externalSource = true; context.itemTarget = true;
        // A delayed source advertises files, but the hover phase does not know
        // the eventual multi-selection count. Building the menu must not read.
        context.externalFileCount = 1;
        context.preview = [](DWORD) -> DropPreviewList {
            throw std::runtime_error("ordinary external-file menu cannot require an internal preview");
        };
        const auto choices = right_drag_actions::Build(context, std::vector<Pair>{});
        for (const auto& named : decisions)
        {
            if (named.effect == 0) continue;
            const auto found = std::find_if(choices.begin(), choices.end(),
                [&](const auto& choice) { return choice.command == named.command; });
            Require((found != choices.end()) == ((allowed & named.effect) != 0),
                "right-drag menu offers exactly the named operations supported by the source");
            if (found != choices.end()) Require(std::string(found->label) == named.label,
                "right-drag operation retains the promised bound label");
        }
        Require(batch.source->reads == 0 && batch.commits == 0,
            "opening and inspecting right-drag choices never renders or commits source files");
        const DWORD expected = decision.effect & allowed;
        const auto* choice = right_drag_actions::Select(choices, decision.command);
        if (choice)
        {
            const auto keys = right_drag_actions::SelectedKeys(*choice);
            Require((keys & MK_RBUTTON) == 0 && (keys & MK_LBUTTON) != 0,
                "selected operation cannot reopen a second native right-drag menu");
            Harness harness;
            const auto dispatched = delayed_desktop_file_drop::Dispatch(batch.source.Get(), true, true,
                keys, allowed, [&](DWORD effect) {
                    batch.effect = effect;
                    return harness.Submit(batch);
                });
            Require(dispatched.has_value() && *dispatched == expected,
                "right-drag selection reaches production dispatch with the selected offered operation");
            Require(batch.source->metadataReads > 0,
                "dispatch probes Shell selection metadata before starting the content transfer");
            PumpUntil([&] { return batch.source->ends == 1; });
            // Assert physical results against the menu promise, independently
            // of any effect produced by the selected key conversion.
            batch.effect = expected;
            AssertSuccess(batch, selected);
        }
        else
            Require(expected == 0 && batch.source->reads == 0 && batch.commits == 0 &&
                batch.source->starts == 0 && batch.source->ends == 0,
                "cancel, dismissal and unavailable actions never begin a transfer");
        for (size_t index = 0; index < count; ++index)
            Require(std::filesystem::exists(selected[index]) == (expected != DROPEFFECT_MOVE),
                "right-menu move removes originals; copy, link, cancellation and rejection preserve them");
        if (!expected) Require(std::filesystem::is_empty(batch.destination),
            "cancelled or unavailable right-drag action leaves the destination empty");
    }
    std::cout << "right-drag menu, selected operation, async files and cancellation cases passed\n";
}
}
