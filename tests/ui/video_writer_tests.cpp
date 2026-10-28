#include <windows.h>
#include <ole2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include "ui/preview/video_writer.h"
#include "test_temporary_directory.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
using Microsoft::WRL::ComPtr;
using namespace snowdesktop;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(HRESULT result) { Require(SUCCEEDED(result), "Media Foundation round-trip operation"); }
struct Apartment { Apartment() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); } ~Apartment() { CoUninitialize(); } };
struct Media { Media() { Check(MFStartup(MF_VERSION)); } ~Media() { MFShutdown(); } };
}
int main()
{
    try
    {
        using namespace video_writer;
        Require(FrameTime(30000, 30) == 10000000000LL && FrameTime(180000, 60) == 30000000000LL,
            "integer frame boundaries do not accumulate rounded frame durations");
        std::vector<std::uint32_t> gray(16, 0xff000000);
        std::vector<std::uint8_t> yuv(24);
        ToNv12(gray, 4, 4, yuv);
        Require(yuv[0] == 16 && yuv[16] == 128 && yuv[17] == 128, "black maps to legal neutral NV12");
        std::fill(gray.begin(), gray.end(), 0xffffffff); ToNv12(gray, 4, 4, yuv);
        Require(yuv[0] == 235 && yuv[16] == 128 && yuv[17] == 128, "white retains limited-range level and neutral chroma");
        bool rejected = false;
        try { ToNv12(gray, 3, 4, yuv); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "odd or mismatched input cannot write beyond a plane");
        Apartment apartment; Media media; test::TemporaryDirectory temporary;
        const auto path = temporary.path / L"round-trip.mp4";
        constexpr unsigned width = 320, height = 180, fps = 30, frames = 6;
        const std::uint32_t colors[]{0xffff0000, 0xff00ff00, 0xff0000ff};
        {
            Mp4 writer(path, width, height, fps, 1000000);
            std::vector<std::uint32_t> pixels(width * height);
            for (unsigned index = 0; index < frames; ++index)
            { std::fill(pixels.begin(), pixels.end(), colors[index % 3]); writer.Write(pixels); }
            Require(writer.FrameCount() == frames, "writer counts only accepted samples"); writer.Finish(); writer.Finish();
        }
        Require(std::filesystem::file_size(path) > 1000, "finalized MP4 has encoded data");
        rejected = false;
        try { Mp4 overwrite(path, width, height, fps); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "existing output cannot be overwritten");
        ComPtr<IMFAttributes> attributes; Check(MFCreateAttributes(&attributes, 1));
        Check(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
        ComPtr<IMFSourceReader> reader; Check(MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader));
        Check(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE));
        Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE));
        ComPtr<IMFMediaType> output; Check(MFCreateMediaType(&output));
        Check(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)); Check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
        Check(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output.Get()));
        unsigned count = 0;
        for (unsigned attempt = 0; attempt < 50; ++attempt)
        {
            DWORD stream = 0, flags = 0; LONGLONG time = 0; ComPtr<IMFSample> sample;
            Check(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &time, &sample));
            if (sample)
            {
                Require(count < frames && std::abs(time - FrameTime(count, fps)) < 2000, "decoded samples retain ordered presentation times");
                ComPtr<IMFMediaBuffer> buffer; Check(sample->ConvertToContiguousBuffer(&buffer));
                BYTE* pixels = nullptr; DWORD length = 0; Check(buffer->Lock(&pixels, nullptr, &length));
                Require(length >= width * height * 4, "decoded image contains the entire frame");
                const auto offset = (width * (height / 2) + width / 2) * 4;
                const int b = pixels[offset], g = pixels[offset + 1], r = pixels[offset + 2];
                Check(buffer->Unlock());
                const bool correct = count % 3 == 0 ? r > 150 && g < 80 && b < 80 :
                    count % 3 == 1 ? g > 150 && r < 80 && b < 80 : b > 150 && r < 80 && g < 80;
                if (!correct) std::cerr << "decoded color frame=" << count << " rgb=" << r << ',' << g << ',' << b << '\n';
                Require(correct, "actual H264 decode preserves per-frame red/green/blue identity"); ++count;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        Require(count == frames, "all encoded frames decode before EOS");
        std::cout << "native MP4 encode/decode round-trip passed\n"; return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
