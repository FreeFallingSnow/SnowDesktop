#include "ui/preview/screen_capture.h"
#include "ui/preview/video_writer.h"
#include "test_temporary_directory.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class Action> void Invalid(Action action)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "invalid recording options were accepted");
}
}
int main()
{
    try
    {
        using namespace snowdesktop;
        using namespace screen_capture;
        Require(RecordingFrameCount(8000, 30) == 240, "8 seconds at 30fps must retain 240 output slots");
        Require(RecordingFrameCount(120000, 60) == 7200, "maximum recording remains bounded");
        Require(RecordingFrameCount(100, 1) == 1 && RecordingFrameCount(101, 30) == 4,
            "short recordings round up to a complete CFR frame");
        for (const unsigned rate : {1U, 7U, 29U, 30U, 59U, 60U})
            for (const unsigned milliseconds : {100U, 101U, 999U, 1000U, 12345U, 120000U})
            {
                const auto frames = RecordingFrameCount(milliseconds, rate);
                const auto requested = static_cast<std::int64_t>(milliseconds) * 10000;
                Require(video_writer::FrameTime(frames, rate) >= requested,
                    "encoded CFR duration cannot truncate the requested real-time interval");
                Require(video_writer::FrameTime(frames - 1, rate) < requested,
                    "CFR rounding may add at most one frame");
            }
        Invalid([] { RecordingFrameCount(0, 30); });
        Invalid([] { RecordingFrameCount(99, 30); });
        Invalid([] { RecordingFrameCount(120001, 30); });
        Invalid([] { RecordingFrameCount(1000, 0); });
        Invalid([] { RecordingFrameCount(1000, 61); });
        Invalid([] { RecordingFrameCount(std::numeric_limits<unsigned>::max(), 60); });

        // These rejection paths run before monitor enumeration or WGC startup.
        // The ordinary regression does not capture the user's personal desktop.
        Service service;
        std::string error;
        test::TemporaryDirectory temporary;
        Require(!service.Record(L"unavailable", temporary.path, false, 99, 30, error) &&
            error == "invalid_recording_options", "invalid duration cannot start a capture worker");
        Require(!service.Record(L"unavailable", temporary.path, false, 1000, 61, error) &&
            error == "invalid_recording_options", "invalid rate cannot start a capture worker");
        Require(!service.Record(L"unavailable", L"relative-output", false, 1000, 30, error) &&
            error == "invalid_capture_root", "relative output cannot select a monitor or write files");
        Require(!service.Screenshot(L"unavailable", L"relative-output", false, error) &&
            error == "invalid_capture_root", "screenshot shares the protected output contract");
        const auto state = service.Current();
        Require(state.status == "idle" && state.id.empty() && state.source == "wgc-monitor" &&
            state.frames == 0 && state.capturedFrames == 0 && state.path.empty(),
            "rejected requests cannot fabricate frames or transition service state");
        Require(std::filesystem::is_empty(temporary.path), "rejected requests cannot leave capture artifacts");
        service.Cancel(); service.Stop(); service.Stop();
        Require(service.Current().status == "idle", "idle cancellation cannot invent a completed task");
        std::cout << "real-time capture option and CFR coverage checks passed; no desktop was captured\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
