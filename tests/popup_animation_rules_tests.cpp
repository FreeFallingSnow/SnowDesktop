#include "popup_animation_rules.h"
#include "app/popup_dwell_controller.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

bool NearlyEqual(float left, float right, float tolerance = 0.0001f)
{
    return std::fabs(left - right) <= tolerance;
}

float EvaluateSegmentCurve(
    float normalizedStartSlope, float progress)
{
    const float remaining = 1.0f - progress;
    return 3.0f * remaining * remaining * progress *
            (normalizedStartSlope / 3.0f) +
        3.0f * remaining * progress * progress +
        progress * progress * progress;
}
}

int main()
{
    using namespace snowdesktop::popup_animation_rules;

    // Exercise the production hover-switch dispatcher and timing controller.
    // Only the window/GPU boundary is replaced: close advances the real State,
    // and publishing replacement content must wait for its hidden endpoint.
    for (const bool fadeMode : {false, true})
    {
        State outgoing;
        outgoing.Configure(fadeMode, 2.0);
        outgoing.ShowImmediately();
        PopupHoverController hover;
        hover.Track(L"dock:folder-b", 100);
        int closes = 0, opens = 0;
        const auto close = [&] { ++closes; outgoing.Close(700); };
        const auto open = [&] {
            Check(outgoing.IsHidden(), "new popup cannot replace visible outgoing content");
            Check(hover.Consume(900, 600), "waiting for close retains the completed hover delay");
            ++opens;
        };
        Check(hover.IsReady(700, 600) && !OpenAfterClose(outgoing, true, close, open) &&
                outgoing.IsClosing() && closes == 1 && opens == 0 && hover.Pending(),
            "mature hover starts the old popup's close before publishing another source");
        outgoing.Advance(790);
        Check(!OpenAfterClose(outgoing, true, close, open) && closes == 1 &&
                opens == 0 && outgoing.GetVisual().visible,
            "repeated hover polls preserve the in-progress outgoing animation");
        outgoing.Advance(880);
        Check(OpenAfterClose(outgoing, false, close, open) && closes == 1 && opens == 1,
            "new content opens only after close completes for scale and fade");
    }
    {
        State immediate;
        immediate.ShowImmediately();
        int closes = 0, opens = 0;
        Check(OpenAfterClose(immediate, true,
                [&] { ++closes; immediate.ResetHidden(); }, [&] { ++opens; }) &&
                closes == 1 && opens == 1,
            "disabled effects close and open in one dispatch without an extra dwell");
        immediate.ResetHidden();
        Check(OpenAfterClose(immediate, false, [&] { ++closes; }, [&] { ++opens; }) &&
                closes == 1 && opens == 2,
            "opening from an empty surface does not request an outgoing animation");
    }
    {
        PopupHoverController hover;
        hover.Track(L"collection:b", 100);
        Check(hover.IsReady(700, 600), "a switch can start after the dwell delay");
        hover.Reset();
        Check(!hover.IsReady(900, 600) && !hover.Consume(900, 600),
            "leaving or disabling hover during close cancels replacement opening");
        hover.Track(L"collection:b", 1000);
        Check(hover.IsReady(1600, 600), "reentry must complete another dwell");
        hover.SuppressUntilLeave();
        Check(!hover.IsReady(1800, 600), "clicking during close cancels the pending hover switch");
        hover.Track(L"collection:c", 1900);
        Check(!hover.IsReady(2400, 600) && hover.IsReady(2500, 600),
            "moving to a third opener during close requires its own full dwell");
    }

    // A configured dwell must govern both readiness and opening, including
    // values on either side of the former fixed 600 ms threshold.
    for (const DWORD delay : {100u, 1200u, 3000u})
    {
        PopupHoverController hover;
        hover.Track(L"dock:custom-delay", 100);
        Check(!hover.IsReady(100 + delay - 1, delay) &&
                !hover.Consume(100 + delay - 1, delay),
            "a configured hover delay must not open early");
        Check(hover.IsReady(100 + delay, delay) &&
                hover.Consume(100 + delay, delay) &&
                !hover.Consume(100 + delay + 1, delay),
            "a configured hover delay opens exactly once at its threshold");
    }

    // Drag dwell uses the same production close dispatcher, but must retain
    // its candidate/timer until publishing. Model/GPU callbacks are replaced;
    // the real timing and animation states remain under test. A queued B must
    // never open after the pointer has moved to C or the drag has ended.
    for (const bool fadeMode : {false, true})
    {
        State visual;
        visual.Configure(fadeMode, 2.0);
        visual.ShowImmediately();
        PopupDwellController dwell;
        std::size_t source = 0;
        int closes = 0, opens = 0;
        const auto poll = [&](DWORD now, bool dragging) {
            if (!dragging || !dwell.IsReady(now, 600)) return false;
            const auto candidate = dwell.Candidate();
            return OpenAfterClose(visual, source != PopupDwellController::NoCandidate,
                [&] { ++closes; visual.Close(now); },
                [&] {
                    Check(visual.IsHidden(), "drag replacement preserves outgoing content until hidden");
                    source = candidate;
                    dwell.Reset();
                    visual.Open(now);
                    ++opens;
                });
        };
        const auto finishClose = [&](DWORD now) {
            visual.Advance(now);
            Check(visual.IsHidden(), "outgoing drag popup reaches its real close endpoint");
            source = PopupDwellController::NoCandidate;
        };

        dwell.Track(1, 100);
        Check(!poll(700, true) && source == 0 && closes == 1 && opens == 0 &&
                dwell.Candidate() == 1 && visual.IsClosing(),
            "drag A to B starts close without replacing A or discarding B's dwell");
        visual.Advance(760);
        Check(!poll(760, true) && source == 0 && closes == 1 && visual.GetVisual().visible,
            "drag polling cannot retire the outgoing scale/fade frame early");
        dwell.Track(2, 760);
        finishClose(880);
        Check(!poll(880, true) && !poll(1359, true) && opens == 0,
            "A to B to C does not open stale B or bypass C's dwell");
        Check(poll(1360, true) && source == 2 && opens == 1,
            "A to B to C publishes only C after both close and dwell finish");
        visual.Advance(1540);
        dwell.Track(0, 1540);
        Check(!poll(2140, true) && source == 2 && closes == 2,
            "returning to A first closes the current C popup");
        finishClose(2320);
        Check(poll(2320, true) && source == 0 && opens == 2,
            "returning to A cannot be closed by C's completed timeline");
        visual.Advance(2500);
        dwell.Track(1, 2500);
        Check(!poll(3100, true) && closes == 3, "next drag replacement starts close");
        dwell.Reset(); // Escape, release, lost capture or destroyed candidate.
        finishClose(3280);
        Check(!poll(3280, false) && !poll(4000, true) && opens == 2,
            "ending/cancelling the drag during close leaves no deferred open");
    }
    {
        State visual;
        visual.ShowImmediately();
        PopupDwellController dwell;
        dwell.Track(1, 100);
        int opens = 0;
        Check(dwell.IsReady(700, 600) && OpenAfterClose(visual, true,
                [&] { visual.ResetHidden(); }, [&] { dwell.Reset(); ++opens; }) &&
                opens == 1 && dwell.IsIdle(),
            "drag replacement with effects disabled publishes synchronously and retires dwell");
    }
    {
        PopupHoverController hover;
        hover.Track(L"collection:updated-delay", 100);
        Check(!hover.IsReady(800, 1200) && hover.IsReady(800, 300),
            "changing the setting updates the pending hover's threshold");
        hover.Track(L"collection:other", 800);
        Check(!hover.IsReady(1099, 300) && hover.IsReady(1100, 300),
            "a different opener still needs its own configured delay");
    }

    State state;
    // Directory/icon completions used to retire the native snapshot and cancel
    // its completion token mid-open. Exercise production refresh dispatch with
    // GPU operations as callbacks; this does not measure displayed DWM frames.
    State native;
    native.Open(100);
    int queued = 0, prepared = 0, retired = 0;
    const auto queueNative = [&] { ++queued; };
    const auto prepareNative = [&] { ++prepared; };
    const auto retireNative = [&] { ++retired; };
    for (const std::uint64_t arrival : { 115u, 145u, 220u })
    {
        const auto nativeAction = RefreshContent(native, arrival, true,
            queueNative, prepareNative, retireNative);
        Check(nativeAction == ContentRefreshAction::ContinueCompositor &&
            prepared == 0 && retired == 0 && native.IsAnimating() && native.IsInteractive() &&
            NearlyEqual(native.GetVisual().progress, 0.0f),
            "content arrivals, including a late completion, must keep the native track alive");
    }
    Check(queued == 3, "every content arrival requests fresh snapshot pixels");
    native.Advance(230);
    Check(!native.IsAnimating() && NearlyEqual(native.GetVisual().progress, 1.0f),
        "the original native completion still finishes open on time");
    native.Close(300);
    const auto nativeCloseAction = RefreshContent(native, 400, true,
        queueNative, prepareNative, retireNative);
    Check(nativeCloseAction == ContentRefreshAction::ContinueCompositor &&
        native.IsClosing() && retired == 0 && prepared == 0,
        "late content during native close cannot reopen or prematurely retire the popup");
    native.Advance(400);
    Check(native.IsHidden(), "the original native completion still finishes close on time");

    // Without an independent native track, the UI fallback still replaces its
    // cached pixels before retirement and resumes the original elapsed clock.
    State refreshed;
    bool oldSnapshot = true, oldCompletion = true;
    bool liveReady = false;
    const auto prepareLive = [&] {
        Check(oldSnapshot, "replacement pixels are prepared while the native snapshot still covers the host");
        liveReady = true;
    };
    const auto retire = [&] {
        Check(liveReady || refreshed.IsHidden(), "a visible snapshot cannot retire before replacement pixels are ready");
        oldSnapshot = false; oldCompletion = false;
    };
    int unexpectedNativeUpdates = 0;
    const auto noNativeQueue = [&] { ++unexpectedNativeUpdates; };
    refreshed.Open(100);
    auto action = RefreshContent(refreshed, 145, false, noNativeQueue, prepareLive, retire);
    Check(!oldSnapshot && !oldCompletion && action == ContentRefreshAction::ContinueAnimation &&
        NearlyEqual(refreshed.GetVisual().progress, 0.5f) && refreshed.IsInteractive(),
        "UI fallback content completion retires its snapshot and resumes the elapsed opening timeline");
    refreshed.Close(145);
    oldSnapshot = oldCompletion = true;
    liveReady = false;
    action = RefreshContent(refreshed, 160, false, noNativeQueue, prepareLive, retire);
    Check(!oldSnapshot && !oldCompletion && action == ContentRefreshAction::ContinueAnimation &&
        refreshed.IsClosing() && NearlyEqual(refreshed.GetVisual().progress, 1.0f / 3.0f),
        "new folder contents during close preserve direction and elapsed progress");
    liveReady = false;
    action = RefreshContent(refreshed, 200, false, noNativeQueue, prepareLive, retire);
    Check(!liveReady, "an elapsed close does not publish another visible frame");
    Check(action == ContentRefreshAction::FinalizeClose && refreshed.IsHidden(),
        "late content completion finalizes an elapsed close instead of resurrecting its snapshot");
    refreshed.Open(300);
    oldSnapshot = oldCompletion = true;
    action = RefreshContent(refreshed, 400, false, noNativeQueue, prepareLive, retire);
    Check(action == ContentRefreshAction::Stable && refreshed.IsInteractive() && !refreshed.IsAnimating(),
        "late content completion after opening paints current content without replaying animation");
    Check(unexpectedNativeUpdates == 0, "UI fallback cannot queue a native surface update");
    Check(state.IsHidden(), "new state starts hidden");
    Check(!state.IsInteractive(), "hidden popup does not accept input");

    state.Open(1000);
    Check(state.IsAnimating(), "opening starts an animation");
    Check(state.IsInteractive(), "opening popup accepts input immediately");
    Check(OccludesSurface(state),
        "opening popup still occludes covered elements");
    Check(ShouldConsumePointerInsidePopup(true, true),
        "a visible popup consumes input inside its rect");
    Check(!ShouldConsumePointerInsidePopup(true, false),
        "input outside a visible popup keeps its normal routing");
    Check(!ShouldConsumePointerInsidePopup(false, true),
        "a hidden popup never consumes input");
    state.Advance(1000 + kOpenDurationMs / 2);
    const Visual halfOpen = state.GetVisual();
    Check(halfOpen.visible, "half-open popup is visible");
    Check(NearlyEqual(halfOpen.progress, 0.5f),
        "opening progress advances linearly");
    Check(NearlyEqual(
            EaseInOutSmooth(0.5f), 0.5f),
        "scale easing is centered");
    Check(EaseInOutSmooth(0.25f) < 0.25f &&
          EaseInOutSmooth(0.75f) > 0.75f,
        "scale easing slows at both endpoints");
    Check(NearlyEqual(
            ScaleSegmentNormalizedStartSlope(0.0f, true),
            0.0f) &&
          NearlyEqual(
            ScaleSegmentNormalizedStartSlope(1.0f, false),
            0.0f),
        "full popup transitions start with zero endpoint velocity");
    const float segmentStarts[] = { 0.2f, 0.5f, 0.8f };
    const float segmentSamples[] = { 0.25f, 0.5f, 0.75f };
    for (const bool opening : { false, true })
    {
        const float targetProgress = opening ? 1.0f : 0.0f;
        for (const float segmentStart : segmentStarts)
        {
            const float fromScale =
                ScaleForProgress(segmentStart);
            const float toScale =
                ScaleForProgress(targetProgress);
            const float slope =
                ScaleSegmentNormalizedStartSlope(
                    segmentStart, opening);
            for (const float sample : segmentSamples)
            {
                const float segmentScale = fromScale +
                    (toScale - fromScale) *
                        EvaluateSegmentCurve(slope, sample);
                const float globalScale = ScaleForProgress(
                    segmentStart +
                    (targetProgress - segmentStart) * sample);
                Check(NearlyEqual(
                        segmentScale, globalScale, 0.0002f),
                    "a reversed compositor segment stays on the global scale curve");
            }
        }
    }
    Check(kOpenDurationMs <= 90 &&
          kCloseDurationMs <= 90,
        "popup scale animation stays responsive");
    Check(ShouldUsePopupItemBounds(true, true),
        "interactive popup owns member coordinates");
    Check(!ShouldUsePopupItemBounds(true, false),
        "closing popup cannot replace exposed-item drag coordinates");
    Check(!ShouldUsePopupItemBounds(false, true),
        "missing popup source cannot provide member coordinates");
    Check(halfOpen.scale >
            kMinimumScale + 0.35f,
        "opening visibly grows instead of popping in");

    state.Advance(1000 + kOpenDurationMs);
    const Visual open = state.GetVisual();
    Check(!state.IsAnimating(), "opening finishes at its duration");
    Check(NearlyEqual(open.scale, 1.0f), "open popup reaches full scale");

    state.Close(2000);
    Check(state.IsClosing(), "closing state is reported");
    Check(!state.IsInteractive(), "closing popup stops accepting input");
    Check(OccludesSurface(state),
        "closing popup still occludes covered elements while visible");
    Check(ShouldConsumePointerInsidePopup(
            OccludesSurface(state), true),
        "input inside a closing popup is consumed instead of falling through");
    state.Advance(2000 + kCloseDurationMs / 2);
    const Visual halfClosed = state.GetVisual();
    Check(NearlyEqual(halfClosed.progress, 0.5f),
        "closing progress advances toward hidden");
    Check(halfClosed.scale >
            kMinimumScale + 0.35f,
        "closing visibly shrinks instead of disappearing");
    state.Advance(2000 + kCloseDurationMs);
    Check(state.IsHidden(), "closing reaches hidden");
    Check(!state.IsAnimating(), "closing animation stops when hidden");
    Check(!OccludesSurface(state),
        "a fully hidden popup no longer occludes covered elements");

    state.Open(3000);
    state.Advance(3060);
    const Visual beforeReverse = state.GetVisual();
    state.Close(3060);
    const Visual afterReverse = state.GetVisual();
    Check(NearlyEqual(beforeReverse.scale, afterReverse.scale),
        "reversing to close keeps scale continuous");
    state.Advance(3090);
    const Visual closing = state.GetVisual();
    Check(closing.progress < beforeReverse.progress,
        "reversed close moves toward hidden");
    state.Open(3090);
    const Visual reopened = state.GetVisual();
    Check(NearlyEqual(closing.scale, reopened.scale),
        "reversing back to open keeps scale continuous");
    Check(state.IsInteractive(), "reopened popup accepts input");

    state.ShowImmediately();
    Check(!state.IsAnimating(), "show-immediately is stable");
    Check(NearlyEqual(state.GetVisual().progress, 1.0f),
        "show-immediately reaches open state");
    state.ResetHidden();
    Check(state.IsHidden(), "reset-hidden clears state");

    Check(ResolveExistingSourceAction(false, true) ==
            ExistingSourceAction::OpenAtRequestedAnchor,
        "a different collection may open at its requested anchor");
    Check(ResolveExistingSourceAction(false, false, true) ==
            ExistingSourceAction::OpenAfterExistingCloses,
        "a desktop collection switch waits for the old close animation");
    Check(ResolveExistingSourceAction(
              false, false, false, true) ==
            ExistingSourceAction::OpenAfterExistingCloses,
        "a later Dock switch replaces the pending target instead of interrupting an existing close");
    Check(ResolveExistingSourceAction(true, true) ==
            ExistingSourceAction::CloseExisting,
        "clicking an open collection from another anchor closes it");
    Check(ResolveExistingSourceAction(true, false) ==
            ExistingSourceAction::ReopenExisting,
        "clicking the same collection while closing reverses it immediately");
    Check(ResolveExistingSourceAction(true, false, true) ==
            ExistingSourceAction::KeepClosing,
        "the pointer press that started closing does not reopen the popup");
    Check(ShouldDispatchCollectionDoubleClickPress(true, false),
        "a collection toggle double click replays the second press");
    Check(!ShouldDispatchCollectionDoubleClickPress(true, true),
        "an interactive popup keeps ownership of double clicks inside it");
    Check(!ShouldDispatchCollectionDoubleClickPress(false, false),
        "unrelated desktop double clicks retain their existing behavior");

    State fade;
    fade.Configure(true, 2.0);
    fade.Open(10000);
    fade.Advance(10090);
    Check(NearlyEqual(fade.GetVisual().scale, 1.0f) &&
        NearlyEqual(fade.GetVisual().opacity, 0.5f),
        "fade uses fixed geometry and its configured real-time duration");
    fade.Close(10090);
    Check(NearlyEqual(fade.GetVisual().opacity, 0.5f),
        "reversing a fade preserves its displayed opacity");
    fade.Advance(10180);
    Check(fade.IsHidden() && !OccludesSurface(fade),
        "closed fade releases pointer occlusion");
    fade.Open(10200);
    fade.ShowImmediately();
    Check(!fade.IsAnimating() && fade.GetVisual().opacity == 1.0f,
        "disabling an in-flight fade can settle immediately");

    std::cout << "popup animation rules tests passed\n";
    return 0;
}
