#include "video_writer.h"
#include <windows.h>
#include <ole2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace snowdesktop::video_writer
{
namespace
{
using Microsoft::WRL::ComPtr;
void Check(HRESULT hr)
{
    if (SUCCEEDED(hr)) return;
    std::ostringstream text; text << "video_media_failure: 0x" << std::hex << static_cast<unsigned long>(hr);
    throw std::runtime_error(text.str());
}
struct Library
{
    HMODULE module;
    explicit Library(const wchar_t* name) : module(LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    { if (!module) throw std::runtime_error("video_media_components_unavailable"); }
    ~Library() { FreeLibrary(module); }
    Library(const Library&) = delete;
    template<class T> T Function(const char* name)
    {
        const auto function = GetProcAddress(module, name);
        if (!function) throw std::runtime_error("video_media_function_unavailable");
        return reinterpret_cast<T>(function);
    }
};
struct Media
{
    Library platform{L"mfplat.dll"}, readwrite{L"mfreadwrite.dll"};
    decltype(&MFStartup) startup = platform.Function<decltype(&MFStartup)>("MFStartup");
    decltype(&MFShutdown) shutdown = platform.Function<decltype(&MFShutdown)>("MFShutdown");
    decltype(&MFCreateMediaType) type = platform.Function<decltype(&MFCreateMediaType)>("MFCreateMediaType");
    decltype(&MFCreateAttributes) attributes = platform.Function<decltype(&MFCreateAttributes)>("MFCreateAttributes");
    decltype(&MFCreateMemoryBuffer) buffer = platform.Function<decltype(&MFCreateMemoryBuffer)>("MFCreateMemoryBuffer");
    decltype(&MFCreateSample) sample = platform.Function<decltype(&MFCreateSample)>("MFCreateSample");
    decltype(&MFCreateSinkWriterFromURL) sink = readwrite.Function<decltype(&MFCreateSinkWriterFromURL)>("MFCreateSinkWriterFromURL");
    Media() { Check(startup(MF_VERSION, MFSTARTUP_FULL)); }
    ~Media() { shutdown(); }
};
void Geometry(IMFMediaType* type, unsigned width, unsigned height, unsigned fps)
{
    Check(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    Check(type->SetUINT64(MF_MT_FRAME_SIZE, (static_cast<UINT64>(width) << 32) | height));
    Check(type->SetUINT64(MF_MT_FRAME_RATE, (static_cast<UINT64>(fps) << 32) | 1));
    Check(type->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (static_cast<UINT64>(1) << 32) | 1));
    Check(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    Check(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
    Check(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    Check(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
    Check(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
}
std::uint8_t Byte(int value) { return static_cast<std::uint8_t>(std::clamp(value, 0, 255)); }
}

std::int64_t FrameTime(std::uint64_t index, unsigned fps)
{
    if (!fps || fps > 240 || index > 240ULL * 60 * 60 * 24)
        throw std::invalid_argument("invalid_video_frame_time");
    return static_cast<std::int64_t>(index * 10000000ULL / fps);
}
void ToNv12(std::span<const std::uint32_t> bgra, unsigned width, unsigned height, std::span<std::uint8_t> nv12)
{
    const auto count = static_cast<std::size_t>(width) * height;
    if (width < 2 || height < 2 || width > 3840 || height > 2160 || (width & 1) || (height & 1) ||
        bgra.size() != count || nv12.size() != count + count / 2)
        throw std::invalid_argument("invalid_video_frame_shape");
    // BT.709, limited range, top-down NV12. Chroma averages each 2x2 block;
    // coefficients sum to zero on the U/V planes, preserving neutral colors.
    for (unsigned y = 0; y < height; y += 2)
        for (unsigned x = 0; x < width; x += 2)
        {
            int red = 0, green = 0, blue = 0;
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx)
                {
                    const auto position = static_cast<std::size_t>(y + dy) * width + x + dx;
                    const auto pixel = bgra[position];
                    const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
                    nv12[position] = Byte(16 + ((47 * r + 157 * g + 16 * b + 128) >> 8));
                    red += r; green += g; blue += b;
                }
            const auto uv = count + static_cast<std::size_t>(y / 2) * width + x;
            nv12[uv] = Byte(128 + ((-26 * red - 86 * green + 112 * blue + 512) >> 10));
            nv12[uv + 1] = Byte(128 + ((112 * red - 102 * green - 10 * blue + 512) >> 10));
        }
}
struct Mp4::Impl
{
    Media media;
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    unsigned width = 0, height = 0, fps = 0;
    std::uint64_t count = 0;
    bool finished = false;
    std::vector<std::uint8_t> nv12;
};
Mp4::Mp4(const std::filesystem::path& path, unsigned width, unsigned height, unsigned fps, unsigned bitrate)
{
    if (!path.is_absolute() || std::filesystem::exists(path) || width < 2 || height < 2 ||
        width > 3840 || height > 2160 || (width & 1) || (height & 1) || fps < 1 || fps > 60 ||
        bitrate < 100000 || bitrate > 100000000) throw std::invalid_argument("invalid_video_output");
    impl_ = std::make_unique<Impl>();
    auto& state = *impl_; state.width = width; state.height = height; state.fps = fps;
    state.nv12.resize(static_cast<std::size_t>(width) * height * 3 / 2);
    ComPtr<IMFAttributes> options; Check(state.media.attributes(&options, 3));
    Check(options->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    Check(options->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    Check(options->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    Check(state.media.sink(path.c_str(), nullptr, options.Get(), &state.writer));
    ComPtr<IMFMediaType> output; Check(state.media.type(&output)); Geometry(output.Get(), width, height, fps);
    Check(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)); Check(output->SetUINT32(MF_MT_AVG_BITRATE, bitrate));
    Check(state.writer->AddStream(output.Get(), &state.stream));
    ComPtr<IMFMediaType> input; Check(state.media.type(&input)); Geometry(input.Get(), width, height, fps);
    Check(input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)); Check(input->SetUINT32(MF_MT_DEFAULT_STRIDE, width));
    Check(state.writer->SetInputMediaType(state.stream, input.Get(), nullptr));
    Check(state.writer->BeginWriting());
}
Mp4::~Mp4() = default;
void Mp4::Write(std::span<const std::uint32_t> bgra)
{
    auto& state = *impl_;
    if (state.finished) throw std::logic_error("video_already_finished");
    ToNv12(bgra, state.width, state.height, state.nv12);
    ComPtr<IMFMediaBuffer> buffer; Check(state.media.buffer(static_cast<DWORD>(state.nv12.size()), &buffer));
    BYTE* destination = nullptr; Check(buffer->Lock(&destination, nullptr, nullptr));
    std::memcpy(destination, state.nv12.data(), state.nv12.size());
    Check(buffer->Unlock()); Check(buffer->SetCurrentLength(static_cast<DWORD>(state.nv12.size())));
    ComPtr<IMFSample> sample; Check(state.media.sample(&sample)); Check(sample->AddBuffer(buffer.Get()));
    const auto start = FrameTime(state.count, state.fps), end = FrameTime(state.count + 1, state.fps);
    Check(sample->SetSampleTime(start)); Check(sample->SetSampleDuration(end - start));
    Check(state.writer->WriteSample(state.stream, sample.Get())); ++state.count;
}
void Mp4::Finish()
{
    auto& state = *impl_;
    if (state.finished) return;
    if (!state.count) throw std::logic_error("video_has_no_frames");
    Check(state.writer->Finalize()); state.finished = true; state.writer.Reset();
}
std::uint64_t Mp4::FrameCount() const noexcept { return impl_->count; }
}
