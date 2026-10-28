#include <windows.h>
#include <ole2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include "demo/export_service.h"
#include "demo/artifact_io.h"
#include "demo/builtin_scenes.h"
#include "test_temporary_directory.h"
#include <chrono>
#include <atomic>
#include <iostream>
#include <set>
#include <thread>

namespace
{
using namespace snowdesktop;
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(HRESULT result) { Require(SUCCEEDED(result), "native export media decode"); }
struct Apartment { Apartment() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); } ~Apartment() { CoUninitialize(); } };
demo::Scene Scene()
{
    control::Value value;
    Require(ParseJson(R"({"schemaVersion":1,"name":"Isolated native desktop","locale":"en-US","appearance":"glass-light","width":960,"height":540,"fps":6,"durationMs":1000,
      "items":[{"id":"notes","title":"Project notes","glyph":"\uf379","category":"documents"},{"id":"photos","title":"Travel photos","glyph":"\uf489","category":"images"},{"id":"budget","title":"Budget","glyph":"\ue54c","category":"documents"}],
      "surfaces":[
        {"id":"bar","kind":"status-bar","x":0,"y":0,"width":960,"height":32},
        {"id":"files","kind":"files","title":"Workspace","x":40,"y":80,"width":360,"height":300},
        {"id":"mapping","kind":"mapping","title":"Library","x":460,"y":80,"width":350,"height":290},
        {"id":"favorites","kind":"collection","title":"Favorites","x":820,"y":80,"width":120,"height":260},
        {"id":"dock","kind":"dock","x":250,"y":440,"width":500,"height":80},
        {"id":"popup","kind":"popup","title":"All files","x":260,"y":90,"width":400,"height":340,"visible":false},
        {"id":"navigation","kind":"navigation","x":180,"y":80,"width":600,"height":400,"visible":false}],
      "timeline":[{"atMs":0,"action":"cursor.move","durationMs":100,"x":300,"y":180},
        {"atMs":150,"action":"cursor.down"},{"atMs":180,"action":"cursor.up"},
        {"atMs":180,"action":"surface.view","target":"files","value":"list"},
        {"atMs":250,"action":"surface.search","target":"mapping","value":"Travel"},
        {"atMs":350,"action":"surface.category","target":"files","value":"documents"},
        {"atMs":500,"action":"surface.search","target":"mapping","value":""},
        {"atMs":500,"action":"surface.category","target":"files","value":"all"},
        {"atMs":550,"action":"surface.show","target":"popup"},
        {"atMs":700,"action":"surface.hide","target":"popup"},
        {"atMs":740,"action":"surface.show","target":"navigation"}]})", value), "decode fixed host-render fixture");
    return demo::ParseScene(value);
}
demo::ExportState Wait(demo::ExportService& service)
{
    const auto deadline = GetTickCount64() + 45000;
    auto state = service.Current();
    while (state.status == "running" && GetTickCount64() < deadline)
    { std::this_thread::sleep_for(std::chrono::milliseconds(40)); state = service.Current(); }
    if (state.status != "completed" && state.status != "cancelled") std::cerr << "export status=" << state.status << " error=" << state.error << '\n';
    Require(state.status != "running", "native export completed within its bounded fixture deadline"); return state;
}
void Png(const std::filesystem::path& file)
{
    ComPtr<IWICImagingFactory> factory; Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmapDecoder> decoder; Check(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
    ComPtr<IWICBitmapFrameDecode> frame; Check(decoder->GetFrame(0, &frame));
    UINT width = 0, height = 0; Check(frame->GetSize(&width, &height)); Require(width == 960 && height == 540, "poster matches scene canvas");
    ComPtr<IWICFormatConverter> converter; Check(factory->CreateFormatConverter(&converter));
    Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    std::vector<std::uint32_t> pixels(width * height); Check(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
    std::set<std::uint32_t> colors;
    for (std::size_t index = 0; index < pixels.size(); index += 97) { Require((pixels[index] >> 24) == 255, "desktop poster is opaque"); colors.insert(pixels[index]); }
    Require(colors.size() > 64, "native desktop output contains rendered visual detail");
}
void Video(const std::filesystem::path& file)
{
    Check(MFStartup(MF_VERSION));
    struct Media { ~Media() { MFShutdown(); } } media;
    ComPtr<IMFSourceReader> reader; Check(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader));
    constexpr auto videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFMediaType> type; Check(reader->GetNativeMediaType(videoStream, 0, &type));
    UINT64 size = 0; Check(type->GetUINT64(MF_MT_FRAME_SIZE, &size)); Require((size >> 32) == 960 && (size & 0xffffffff) == 540, "actual MP4 native size matches PNG");
    unsigned count = 0;
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        DWORD stream = 0, flags = 0; LONGLONG time = 0; ComPtr<IMFSample> sample;
        Check(reader->ReadSample(videoStream, 0, &stream, &flags, &time, &sample));
        if (sample) ++count;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    Require(count == 6, "every native desktop frame decodes to EOS");
}
void ProgressReplacement(const std::filesystem::path& file)
{
    const auto payload = control::String(std::string(256 * 1024, 'p'));
    demo::WriteJson(file, control::Object({{"generation", control::Number(0)}, {"payload", payload}}));
    struct Lease { HANDLE value; ~Lease() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } held{
        CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr)};
    Require(held.value != INVALID_HANDLE_VALUE, "hold an old complete progress snapshot");
    demo::WriteJson(file, control::Object({{"generation", control::Number(1)}, {"payload", payload}}));
    Require(demo::ReadJson(file).Find("generation")->number == 1, "new opens see the replacement while the old snapshot is held");
    CloseHandle(held.value); held.value = INVALID_HANDLE_VALUE;
    std::atomic<bool> failed{false};
    std::jthread reader([&](std::stop_token stopped) {
        while (!stopped.stop_requested())
        {
            try
            {
                const auto snapshot = demo::ReadJson(file);
                if (snapshot.Find("payload")->string.size() != 256 * 1024) failed.store(true);
            }
            catch (...) { failed.store(true); }
        }
    });
    for (int generation = 1; generation <= 40; ++generation)
        demo::WriteJson(file, control::Object({{"generation", control::Number(generation)}, {"payload", payload}}));
    reader.request_stop(); reader.join();
    Require(!failed.load(), "progress reads allow atomic replacement and never observe a partial JSON file");
}
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        Require(argc == 2, "trusted host executable argument"); Apartment apartment; test::TemporaryDirectory temporary;
        const std::filesystem::path executable(argv[1]); const auto root = temporary.path / L"演示 exports";
        ProgressReplacement(temporary.path / L"progress-replacement.json");
        const auto scene = Scene(); (void)demo::ParseScene(*demo::Describe().Find("example"));
        demo::ExportService service; std::string error;
        Require(!service.Start(scene, L"relative.exe", root, error), "relative/untrusted host path rejected before launch");
        Require(service.Start(scene, executable, root, error), "start native worker");
        Require(!service.Start(scene, executable, root, error) && error == "demo_busy", "concurrent export cannot replace a running task");
        const auto first = Wait(service); Require(first.status == "completed" && first.completedFrames == 6, "first desktop export completed");
        Require(first.sourceExeSha256 == demo::Sha256(executable), "worker output binds the trusted source binary");
        Png(first.directory / L"poster.png"); Video(first.directory / L"video.mp4");
        Require(service.Start(scene, executable, root, error), "repeat exact scene in a new worker");
        const auto second = Wait(service); Require(second.status == "completed" && first.directory != second.directory, "repeated export owns a fresh directory");
        Require(demo::Sha256(first.directory / L"poster.png") == demo::Sha256(second.directory / L"poster.png"), "native poster pixels repeat across separate worker processes");
        const auto* samples = first.manifest.Find("posters"); const auto* repeated = second.manifest.Find("posters");
        Require(samples && repeated && samples->array.size() == repeated->array.size(), "sampled frame manifest is complete");
        for (std::size_t index = 0; index < samples->array.size(); ++index)
            Require(samples->array[index].Find("sha256")->string == repeated->array[index].Find("sha256")->string, "all sampled native frames repeat exactly");
        auto longInput = scene.source; longInput.object["durationMs"] = control::Number(30000); longInput.object["fps"] = control::Number(60);
        const auto longScene = demo::ParseScene(longInput);
        Require(service.Start(longScene, executable, root, error), "start cancellable owned render worker"); service.Cancel();
        const auto cancelled = Wait(service); Require(cancelled.status == "cancelled", "cancellation stops only the owned worker");
        Require(!std::filesystem::exists(cancelled.directory / L"scene-result.json"), "cancelled partial output is not published as completed");
        Require(demo::Sha256(first.directory / L"poster.png") == demo::Sha256(second.directory / L"poster.png"), "cancelling a later task preserves previous artifacts");
        std::cout << "native scene exports, media decode, repeatability and cancellation passed\n"; return 0;
    }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
